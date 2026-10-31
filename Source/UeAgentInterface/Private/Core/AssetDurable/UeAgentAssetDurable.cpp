#include "Core/AssetDurable/UeAgentAssetDurable.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Core/AssetDurable/UeAgentPackageHeader.h"
#include "Core/AssetDurable/UeAgentPackageSave.h"
#include "Utilities/UeAgentFileOperationDiagnostic.h"
#include "Utilities/UeAgentNativeOperationFeedback.h"

#include "Core/Contract/UeAgentContract.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/LinkerLoad.h"
#include "UObject/ObjectSaveContext.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectGlobals.h"

#include <openssl/sha.h>

namespace UE::UAI::AssetDurable
{
	using FileDiagnostics::MakeFileOperationFailure;
	using UE::UAI::ContextResource::EUeAgentResourceDimension;
	using UE::UAI::ContextResource::FUeAgentResourceChargeError;
	using UE::UAI::Contract::FUeAgentDescriptorIdentity;
	using UE::UAI::PlanningPreflight::FChangeSet;
	using UE::UAI::PlanningPreflight::FImpactSet;
	using UE::UAI::PlanningPreflight::FPlannedChange;
	using UE::UAI::PlanningPreflight::FPlanningPreflightPlan;
	using UE::UAI::StateSemantic::FUeAgentStateRevision;

	namespace
	{
		using NativeFeedback::FNativeOperationFeedback;

		const TCHAR* JournalSchemaV1 =
			TEXT("uai.c8d.asset_durable.journal.v1");
		const TCHAR* JournalSchemaV2 =
			TEXT("uai.c8d.asset_durable.journal.v2");
		const TCHAR* JournalSchemaV3 =
			TEXT("uai.c8d.asset_durable.journal.v3");
		const TCHAR* FenceStateSchemaV1 =
			TEXT("uai.c8d.asset_durable.fence_state.v1");

		FString NormalizedText(FString Value)
		{
			Value.TrimStartAndEndInline();
			return Value;
		}

		FString FullPath(const FString& Value)
		{
			FString Result = FPaths::ConvertRelativePathToFull(Value);
			FPaths::NormalizeFilename(Result);
			FPaths::CollapseRelativeDirectories(Result);
			return Result;
		}

		bool IsWithin(const FString& CandidateValue, const FString& RootValue)
		{
			const FString Candidate = FullPath(CandidateValue);
			FString Root = FullPath(RootValue);
			if (Candidate.Equals(Root, ESearchCase::IgnoreCase))
			{
				return false;
			}
			Root.RemoveFromEnd(TEXT("/"));
			return Candidate.StartsWith(
				Root + TEXT("/"),
				ESearchCase::IgnoreCase);
		}

		FString Sha256(const FString& Text)
		{
			const FTCHARToUTF8 Utf8(*Text);
			uint8 Digest[SHA256_DIGEST_LENGTH];
			SHA256(
				reinterpret_cast<const uint8*>(Utf8.Get()),
				Utf8.Length(),
				Digest);
			FString Hex;
			Hex.Reserve(SHA256_DIGEST_LENGTH * 2);
			for (const uint8 Byte : Digest)
			{
				Hex += FString::Printf(TEXT("%02x"), Byte);
			}
			return TEXT("sha256:") + Hex;
		}

		FString Sha256Bytes(const TArray<uint8>& Bytes)
		{
			uint8 Digest[SHA256_DIGEST_LENGTH];
			SHA256(Bytes.GetData(), Bytes.Num(), Digest);
			FString Hex;
			Hex.Reserve(SHA256_DIGEST_LENGTH * 2);
			for (const uint8 Byte : Digest)
			{
				Hex += FString::Printf(TEXT("%02x"), Byte);
			}
			return TEXT("sha256:") + Hex;
		}

		FString ArtifactToken(const FString& ArtifactId)
		{
			const FString Digest = Sha256(ArtifactId);
			return Digest.Mid(7);
		}

		FString DurableArtifactId(
			const FAssetDurableTarget& Target,
			const FString& Role)
		{
			return Target.GetIdentity().GetCanonicalKey()
				+ TEXT(":")
				+ ArtifactToken(Target.GetDurableTargetKey())
				+ TEXT(":")
				+ Role;
		}

		FString BackupDataPath(
			const FAssetDurableSessionRef& Session,
			const FDurableArtifact& Artifact)
		{
			return Session->GetTransactionRoot()
				/ TEXT("backups")
				/ (ArtifactToken(Artifact.GetArtifactId())
					+ TEXT(".data"));
		}

		FString BackupMarkerPath(
			const FAssetDurableSessionRef& Session,
			const FDurableArtifact& Artifact)
		{
			return Session->GetTransactionRoot()
				/ TEXT("backups")
				/ (ArtifactToken(Artifact.GetArtifactId())
					+ TEXT(".marker"));
		}

		FString StagedArtifactPath(
			const FAssetDurableSessionRef& Session,
			const FDurableArtifact& Artifact)
		{
			return Session->GetStagingRoot()
				/ (ArtifactToken(Artifact.GetArtifactId())
					+ FPaths::GetExtension(
						Artifact.GetFinalPath(),
						true));
		}

		const FDurableArtifact* FindArtifact(
			const FAssetDurableSessionRef& Session,
			const FAssetDurableTarget& Target,
			const FString& Role)
		{
			return Session->GetInventory().GetArtifacts()
				.FindByPredicate(
					[&Target, &Role](
						const FDurableArtifact& Artifact)
					{
						return Artifact.GetTargetIdentity()
								.GetCanonicalKey()
								== Target.GetIdentity()
									.GetCanonicalKey()
							&& Artifact.GetArtifactKind()
								== Target.GetArtifactKind()
							&& Artifact.GetDurableLocation()
								== Target.GetDurableLocation()
							&& Artifact.GetRole() == Role;
					});
		}

		FAssetDurableError BackendFailure(const TCHAR* Code, const FString& Reason, const FString& Target)
		{
			FAssetDurableError Error = FAssetDurableError::Failure(Code, Reason, TEXT("asset_durable"));
			Error.SourceKind = TEXT("native_result");
			Error.TargetPath = Target;
			return Error;
		}

		FAssetDurableError CompareArtifacts(const FDurableArtifact& Expected, const FDurableArtifact& Actual)
		{
			FAssetDurableError Error;
			const auto EqualField = [&](const TCHAR* Field, const FString& ExpectedValue, const FString& ActualValue, bool bInternal)
			{
				if (ExpectedValue == ActualValue) return true;
				Error = BackendFailure(TEXT("durable_enlist_inventory_conflict"), FString::Printf(TEXT("Adding durability targets changed the existing artifact field %s."), Field), Expected.GetDurableLocation());
				Error.PropertyPath = Field;
				if (bInternal)
				{
					Error.ActualEvidence = MakeShared<FJsonValueString>(ActualValue);
					Error.ExpectedEvidence = MakeShared<FJsonValueString>(ExpectedValue);
				}
				else
				{
					Error.ActualValue = MakeShared<FJsonValueString>(ActualValue);
					Error.ExpectedValue = MakeShared<FJsonValueString>(ExpectedValue);
				}
				return false;
			};
			if (!EqualField(TEXT("artifact_id"), Expected.GetArtifactId(), Actual.GetArtifactId(), true)
				|| !EqualField(TEXT("target_identity"), Expected.GetTargetIdentity().GetCanonicalKey(), Actual.GetTargetIdentity().GetCanonicalKey(), true)
				|| !EqualField(TEXT("artifact_kind"), LexToString(Expected.GetArtifactKind()), LexToString(Actual.GetArtifactKind()), false)
				|| !EqualField(TEXT("durable_location"), Expected.GetDurableLocation(), Actual.GetDurableLocation(), false)
				|| !EqualField(TEXT("final_path"), Expected.GetFinalPath(), Actual.GetFinalPath(), true)
				|| !EqualField(TEXT("intent"), LexToString(Expected.GetIntent()), LexToString(Actual.GetIntent()), false)
				|| !EqualField(TEXT("role"), Expected.GetRole(), Actual.GetRole(), false)) return Error;
			return {};
		}

		bool TryVerifyRestoredFile(
			const FString& FinalPath,
			const FString& BackupPath,
			const FString& PublicTarget,
			FAssetDurableError& OutError)
		{
			TArray<uint8> Left;
			TArray<uint8> Right;
			if (!FFileHelper::LoadFileToArray(Left, *FinalPath))
			{
				OutError = MakeFileOperationFailure(TEXT("durable_restore_failed"), TEXT("The restored file could not be read for verification."), TEXT("asset_durable"), FinalPath);
				OutError.TargetPath = PublicTarget;
				return false;
			}
			if (!FFileHelper::LoadFileToArray(Right, *BackupPath))
			{
				OutError = MakeFileOperationFailure(TEXT("durable_restore_failed"), TEXT("The backup file could not be read to verify restoration."), TEXT("asset_durable"), BackupPath);
				OutError.TargetPath = PublicTarget;
				return false;
			}
			if (Left != Right)
			{
				OutError = BackendFailure(TEXT("durable_restore_failed"), TEXT("The restored file content differs from its backup."), PublicTarget);
				OutError.InternalContext = BackupPath;
				if (Left.Num() != Right.Num())
				{
					OutError.PropertyPath = TEXT("file_size_bytes");
					OutError.ActualValue = MakeShared<FJsonValueNumber>(Left.Num());
					OutError.ExpectedValue = MakeShared<FJsonValueNumber>(Right.Num());
				}
				return false;
			}
			return true;
		}

		bool TrySerializeObject(
			const TSharedRef<FJsonObject>& Object,
			FString& OutText)
		{
			OutText.Reset();
			const TSharedRef<TJsonWriter<>> Writer =
				TJsonWriterFactory<>::Create(&OutText);
			return FJsonSerializer::Serialize(Object, Writer);
		}

		FAssetDurableError ResourceError(const FUeAgentResourceChargeError& Error)
		{
			if (!Error.IsEmpty()) return Error;
			FAssetDurableError Missing = FAssetDurableError::Failure(TEXT("resource_accounting_failed"),
				TEXT("Durability resource accounting failed without reporting a diagnostic."), TEXT("asset_durable"));
			Missing.SourceKind = TEXT("native_result");
			return Missing;
		}

		void AddTargetIndex(FAssetDurableError& Error, const TCHAR* Collection, const int32 Index)
		{
			const FString Prefix = FString::Printf(TEXT("/%s/%d"), Collection, Index);
			Error.PropertyPath = Prefix + (Error.PropertyPath.IsEmpty() || Error.PropertyPath.StartsWith(TEXT("/"))
				? Error.PropertyPath : TEXT("/") + Error.PropertyPath);
		}

		bool TryCharge(
			FUeAgentResourceTracker& Resources,
			const FString& Path,
			const int64 Files,
			const int64 Items,
			const int64 InputBytes,
			const int64 OutputBytes,
			const int64 WorkUnits,
			FAssetDurableError& OutError)
		{
			FUeAgentResourceChargeError Error;
			if (!Resources.TryChargeMany(
				{
					{EUeAgentResourceDimension::Files, Files},
					{EUeAgentResourceDimension::Items, Items},
					{EUeAgentResourceDimension::InputBytes, InputBytes},
					{EUeAgentResourceDimension::OutputBytes, OutputBytes},
					{EUeAgentResourceDimension::WorkUnits, WorkUnits},
				},
				Path,
				Error))
			{
				OutError = ResourceError(Error);
				return false;
			}
			return true;
		}

		bool TryComputeDurableInventoryHashImpl(
			const TArray<FDurableArtifact>& Artifacts,
			bool bBindArtifactKind,
			FUeAgentResourceTracker& Resources,
			FString& OutInventoryHash,
			FAssetDurableError& OutError);

		bool IsKnownPhase(const FString& Phase)
		{
			return Phase == TEXT("inventory_ready")
				|| Phase == TEXT("backups_ready")
				|| Phase == TEXT("prepared")
				|| Phase == TEXT("inventory_extended")
				|| Phase == TEXT("staged")
				|| Phase == TEXT("native_mutating")
				|| Phase == TEXT("published")
				|| Phase == TEXT("persisted_unverified")
				|| Phase == TEXT("verified")
				|| Phase == TEXT("committed")
				|| Phase == TEXT("reconciled_original")
				|| Phase == TEXT("rolled_back")
				|| Phase == TEXT("recovery_required")
				|| Phase == TEXT("state_unknown");
		}

		bool IsPrePublishPhase(const FString& Phase)
		{
			return Phase == TEXT("inventory_ready")
				|| Phase == TEXT("backups_ready")
				|| Phase == TEXT("prepared")
				|| Phase == TEXT("inventory_extended")
				|| Phase == TEXT("staged");
		}

		bool IsTerminalPhase(const FString& Phase)
		{
			return Phase == TEXT("verified")
				|| Phase == TEXT("committed")
				|| Phase == TEXT("reconciled_original")
				|| Phase == TEXT("rolled_back");
		}

		struct FBoundTerminalEvidence
		{
			bool bPresent = false;
			FString Phase;
			TArray<FString> PhaseHistory;
		};

		bool TryReadBoundCompletedTerminal(
			const FAssetDurableSessionRef& Session,
			FUeAgentResourceTracker& Resources,
			FBoundTerminalEvidence& OutEvidence,
			FAssetDurableError& OutError);

		FAssetDurableSessionRef Advance(
			const FAssetDurableSessionRef& Session,
			const FString& Phase)
		{
			TArray<FString> History = Session->GetPhaseHistory();
			if (History.IsEmpty() || History.Last() != Phase)
			{
				History.Add(Phase);
			}
			return MakeShared<const FAssetDurableSession>(
				Session->GetRequest(),
				Session->GetInventory(),
				Session->GetJournalId(),
				Session->GetJournalPath(),
				Session->GetJournalRoot(),
				Session->GetTransactionRoot(),
				Session->GetStagingRoot(),
				MoveTemp(History),
				Session->GetPreMutationStates(),
				Session->GetExpectedPersistedStates());
		}

		const FDurableArtifactState* FindState(
			const TArray<FDurableArtifactState>& States,
			const FString& ArtifactId)
		{
			return States.FindByPredicate(
				[&ArtifactId](const FDurableArtifactState& State)
				{
					return State.GetArtifactId() == ArtifactId;
				});
		}

		bool TryCaptureArtifactStates(
			const FDurableInventory& Inventory,
			const FString& ResourcePath,
			FUeAgentResourceTracker& Resources,
			TArray<FDurableArtifactState>& OutStates,
			FAssetDurableError& OutError)
		{
			OutStates.Reset();
			for (const FDurableArtifact& Artifact : Inventory.GetArtifacts())
			{
				const int64 FileSize = IFileManager::Get().FileSize(
					*Artifact.GetFinalPath());
				if (!TryCharge(
						Resources,
						ResourcePath,
						1,
						1,
						FMath::Max<int64>(0, FileSize),
						0,
						1,
						OutError))
				{
					return false;
				}
				if (FileSize < 0)
				{
					OutStates.Emplace(
						Artifact.GetArtifactId(),
						EDurableStatePresence::Absent);
					continue;
				}
				TArray<uint8> Bytes;
				if (!FFileHelper::LoadFileToArray(
						Bytes,
						*Artifact.GetFinalPath()))
				{
					OutError = MakeFileOperationFailure(TEXT("durable_artifact_readback_failed"), TEXT("Reading the durable artifact file to capture its current state failed."), TEXT("asset_durable"), Artifact.GetFinalPath());
					OutError.TargetPath = Artifact.GetDurableLocation();
					return false;
				}
				OutStates.Emplace(
					Artifact.GetArtifactId(),
					EDurableStatePresence::Present,
					Sha256Bytes(Bytes));
			}
			return true;
		}

		TSharedRef<FJsonObject> StateObject(
			const FDurableArtifactState* State,
			const FString& ArtifactId)
		{
			const TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("artifact_id"), ArtifactId);
			const EDurableStatePresence Presence = State
				? State->GetPresence()
				: EDurableStatePresence::Unknown;
			Result->SetStringField(TEXT("presence"), LexToString(Presence));
			if (State && !State->GetContentHash().IsEmpty())
			{
				Result->SetStringField(
					TEXT("content_hash"), State->GetContentHash());
			}
			return Result;
		}

		bool TryResolvePreMutationStates(
			const FAssetDurableSessionRef& Session,
			FUeAgentResourceTracker& Resources,
			TArray<FDurableArtifactState>& OutStates,
			FAssetDurableError& OutError)
		{
			OutStates.Reset();
			for (const FDurableArtifact& Artifact :
				Session->GetInventory().GetArtifacts())
			{
				const FDurableArtifactState* Bound = FindState(
					Session->GetPreMutationStates(),
					Artifact.GetArtifactId());
				if (Bound && Bound->IsKnown())
				{
					OutStates.Add(*Bound);
					continue;
				}

				const FString MarkerPath =
					BackupMarkerPath(Session, Artifact);
				const int64 MarkerSize =
					IFileManager::Get().FileSize(*MarkerPath);
				if (!TryCharge(
						Resources,
						TEXT("/c8d/reconcile/legacy_pre_state"),
						0,
						MarkerSize >= 0 ? 1 : 0,
						FMath::Max<int64>(0, MarkerSize),
						0,
						1,
						OutError))
				{
					return false;
				}
				FString Marker;
				if (MarkerSize < 0
					|| !FFileHelper::LoadFileToString(
						Marker,
						*MarkerPath))
				{
					OutStates.Emplace(
						Artifact.GetArtifactId(),
						EDurableStatePresence::Unknown);
					continue;
				}
				Marker.TrimStartAndEndInline();
				if (Marker == TEXT("absent"))
				{
					OutStates.Emplace(
						Artifact.GetArtifactId(),
						EDurableStatePresence::Absent);
					continue;
				}
				if (Marker != TEXT("present"))
				{
					OutStates.Emplace(
						Artifact.GetArtifactId(),
						EDurableStatePresence::Unknown);
					continue;
				}

				const FString DataPath =
					BackupDataPath(Session, Artifact);
				const int64 DataSize =
					IFileManager::Get().FileSize(*DataPath);
				TArray<uint8> Bytes;
				if (!TryCharge(
						Resources,
						TEXT("/c8d/reconcile/legacy_pre_state_data"),
						0,
						DataSize >= 0 ? 1 : 0,
						FMath::Max<int64>(0, DataSize),
						0,
						1,
						OutError))
				{
					return false;
				}
				if (DataSize < 0
					|| !FFileHelper::LoadFileToArray(Bytes, *DataPath))
				{
					OutStates.Emplace(
						Artifact.GetArtifactId(),
						EDurableStatePresence::Unknown);
					continue;
				}
				OutStates.Emplace(
					Artifact.GetArtifactId(),
					EDurableStatePresence::Present,
					Sha256Bytes(Bytes));
			}
			return true;
		}

		FAssetDurableError CompareArtifactStates(
			const FDurableInventory& Inventory,
			const TArray<FDurableArtifactState>& Expected,
			const TArray<FDurableArtifactState>& Observed,
			const FString& ExpectedStateName)
		{
			int32 Index = 0;
			for (const FDurableArtifact& Artifact :
				Inventory.GetArtifacts())
			{
				const FDurableArtifactState* ExpectedState = FindState(
					Expected,
					Artifact.GetArtifactId());
				const FDurableArtifactState* ObservedState = FindState(
					Observed,
					Artifact.GetArtifactId());
				if (!ExpectedState
					|| !ObservedState
					|| !ExpectedState->Matches(*ObservedState))
				{
					const bool bMissing = !ExpectedState || !ObservedState;
					const bool bPresenceDiffers = !bMissing && (!ExpectedState->IsKnown() || !ObservedState->IsKnown()
						|| ExpectedState->GetPresence() != ObservedState->GetPresence());
					FAssetDurableError Error = BackendFailure(TEXT("durable_reconcile_evidence_insufficient"), bMissing
						? FString::Printf(TEXT("The artifact is missing its %s."), !ExpectedState ? *ExpectedStateName : TEXT("observed state"))
						: bPresenceDiffers ? FString::Printf(TEXT("The observed artifact presence does not establish the bound %s."), *ExpectedStateName)
						: FString::Printf(TEXT("The observed artifact content differs from the bound %s."), *ExpectedStateName), Artifact.GetDurableLocation());
					Error.PropertyPath = FString::Printf(TEXT("%s.artifacts[%d]%s"), *ExpectedStateName, Index,
						bMissing ? TEXT("") : bPresenceDiffers ? TEXT(".presence") : TEXT(".content_hash"));
					if (bMissing || bPresenceDiffers)
					{
						Error.ExpectedValuePresent = ExpectedState != nullptr;
						Error.ActualValuePresent = ObservedState != nullptr;
						if (ExpectedState) Error.ExpectedValue = MakeShared<FJsonValueString>(LexToString(ExpectedState->GetPresence()));
						if (ObservedState) Error.ActualValue = MakeShared<FJsonValueString>(LexToString(ObservedState->GetPresence()));
					}
					else
					{
						Error.ExpectedEvidence = MakeShared<FJsonValueString>(ExpectedState->GetContentHash());
						Error.ActualEvidence = MakeShared<FJsonValueString>(ObservedState->GetContentHash());
					}
					return Error;
				}
				++Index;
			}
			return {};
		}

		bool IsResourceChargeError(const FAssetDurableError& Error)
		{
			return Error.Code == TEXT("resource_accounting_failed");
		}

		FDurableReceipt IsolationReceipt(
			const FString& JournalPath,
			const FAssetDurableError& Error)
		{
			return FDurableReceipt(FPaths::GetCleanFilename(FPaths::GetPath(JournalPath)),
				EDurableStatus::StateUnknown,
				FString(),
				FString(),
				{},
				ERecoveryAction::ManualIntervention,
				false,
				true,
				Error);
		}

		TSharedRef<FJsonObject> IdentityObject(
			const FUeAgentCanonicalIdentity& Identity)
		{
			const TSharedRef<FJsonObject> Result =
				MakeShared<FJsonObject>();
			Result->SetStringField(
				TEXT("semantic_id"),
				Identity.GetSemanticId());
			Result->SetStringField(
				TEXT("target_kind"),
				Identity.GetTargetKind());
			Result->SetStringField(
				TEXT("owner_id"),
				Identity.GetOwnerId());
			Result->SetStringField(
				TEXT("selector"),
				Identity.GetSelector());
			Result->SetStringField(
				TEXT("object_role"),
				Identity.GetObjectRole());
			Result->SetStringField(
				TEXT("generation"),
				Identity.GetGeneration());
			return Result;
		}

		FAssetDurableError JournalFailure(const FString& Reason, const FString& Path)
		{
			FAssetDurableError Error = BackendFailure(TEXT("durable_journal_integrity_invalid"), Reason, FString());
			Error.PropertyPath = TEXT("journal.") + Path;
			return Error;
		}

		void PrependDiagnosticPath(FAssetDurableError& Error, const FString& Prefix)
		{
			Error.PropertyPath = Prefix + (Error.PropertyPath.IsEmpty() ? FString() : TEXT(".") + Error.PropertyPath);
		}

		struct FJournalFields
		{
			const TSharedPtr<FJsonObject>& Json;
			FString Prefix;
			FAssetDurableError& Error;
			const TCHAR* FailureCode = TEXT("durable_journal_integrity_invalid");

			bool Invalid(const TCHAR* Name, const TCHAR* Expected)
			{
				const FString Path = Prefix.IsEmpty() ? FString(Name) : Prefix + TEXT(".") + Name;
				Error = JournalFailure(FString::Printf(TEXT("The durable journal field %s must be %s."), *Path, Expected), Path);
				Error.Code = FailureCode;
				Error.ExpectedValue = MakeShared<FJsonValueString>(Expected);
				Error.ActualValuePresent = Json.IsValid() && Json->HasField(Name);
				if (Json.IsValid()) Error.ActualEvidence = Json->TryGetField(Name);
				return false;
			}

			bool String(const TCHAR* Name, FString& Value)
			{
				return (Json.IsValid() && Json->TryGetStringField(Name, Value)) || Invalid(Name, TEXT("a string"));
			}
			bool Number(const TCHAR* Name, double& Value)
			{
				return (Json.IsValid() && Json->TryGetNumberField(Name, Value)) || Invalid(Name, TEXT("a number"));
			}
			bool Array(const TCHAR* Name, const TArray<TSharedPtr<FJsonValue>>*& Value)
			{
				return (Json.IsValid() && Json->TryGetArrayField(Name, Value)) || Invalid(Name, TEXT("an array"));
			}
			bool Object(const TCHAR* Name, const TSharedPtr<FJsonObject>*& Value)
			{
				return (Json.IsValid() && Json->TryGetObjectField(Name, Value)) || Invalid(Name, TEXT("an object"));
			}
		};

		bool TryParseIdentity(
			const TSharedPtr<FJsonObject>& Object,
			const FString& Path,
			FUeAgentCanonicalIdentity& OutIdentity,
			FAssetDurableError& OutError)
		{
			FString SemanticId;
			FString TargetKind;
			FString OwnerId;
			FString Selector;
			FString ObjectRole;
			FString Generation;
			FJournalFields Fields{Object, Path, OutError};
			if (!Fields.String(TEXT("semantic_id"), SemanticId)
				|| !Fields.String(TEXT("target_kind"), TargetKind)
				|| !Fields.String(TEXT("owner_id"), OwnerId)
				|| !Fields.String(TEXT("selector"), Selector)
				|| !Fields.String(TEXT("object_role"), ObjectRole)
				|| !Fields.String(TEXT("generation"), Generation))
			{
				return false;
			}
			OutIdentity = FUeAgentCanonicalIdentity(
				MoveTemp(SemanticId),
				MoveTemp(TargetKind),
				MoveTemp(OwnerId),
				MoveTemp(Selector),
				MoveTemp(ObjectRole),
				MoveTemp(Generation));
			if (!OutIdentity.IsValid())
			{
				OutError = OutIdentity.GetValidationDiagnostic();
				PrependDiagnosticPath(OutError, TEXT("journal.") + Path);
				OutError.SourceKind = TEXT("native_result");
				return false;
			}
			return true;
		}

		bool TryIntentFromString(
			const FString& Value,
			EAssetDurableIntent& OutIntent)
		{
			if (Value == TEXT("save"))
			{
				OutIntent = EAssetDurableIntent::Save;
				return true;
			}
			if (Value == TEXT("delete"))
			{
				OutIntent = EAssetDurableIntent::Delete;
				return true;
			}
			if (Value == TEXT("native"))
			{
				OutIntent = EAssetDurableIntent::Native;
				return true;
			}
			return false;
		}

		bool TryArtifactKindFromString(
			const FString& Value,
			EDurableArtifactKind& OutKind)
		{
			if (Value == TEXT("package"))
			{
				OutKind = EDurableArtifactKind::Package;
				return true;
			}
			if (Value == TEXT("project_config"))
			{
				OutKind = EDurableArtifactKind::ProjectConfig;
				return true;
			}
			return false;
		}

		bool TryStatePresenceFromString(
			const FString& Value,
			EDurableStatePresence& OutPresence)
		{
			if (Value == TEXT("unknown"))
			{
				OutPresence = EDurableStatePresence::Unknown;
				return true;
			}
			if (Value == TEXT("absent"))
			{
				OutPresence = EDurableStatePresence::Absent;
				return true;
			}
			if (Value == TEXT("present"))
			{
				OutPresence = EDurableStatePresence::Present;
				return true;
			}
			return false;
		}

		bool TryParseArtifactState(
			const TSharedPtr<FJsonObject>& Object,
			const FString& ExpectedArtifactId,
			const FString& Path,
			FDurableArtifactState& OutState,
			FAssetDurableError& OutError)
		{
			FString ArtifactId;
			FString PresenceText;
			FString ContentHash;
			EDurableStatePresence Presence =
				EDurableStatePresence::Unknown;
			FJournalFields Fields{Object, Path, OutError};
			if (!Fields.String(TEXT("artifact_id"), ArtifactId)) return false;
			if (ArtifactId != ExpectedArtifactId)
			{
				OutError = JournalFailure(TEXT("The durable artifact state is bound to a different artifact identifier."), Path + TEXT(".artifact_id"));
				OutError.ActualEvidence = MakeShared<FJsonValueString>(ArtifactId);
				OutError.ExpectedEvidence = MakeShared<FJsonValueString>(ExpectedArtifactId);
				return false;
			}
			if (!Fields.String(TEXT("presence"), PresenceText)) return false;
			if (!TryStatePresenceFromString(PresenceText, Presence))
			{
				OutError = JournalFailure(TEXT("The durable artifact presence must be unknown, absent, or present."), Path + TEXT(".presence"));
				OutError.ActualValue = MakeShared<FJsonValueString>(PresenceText);
				return false;
			}
			Object->TryGetStringField(TEXT("content_hash"), ContentHash);
			OutState = FDurableArtifactState(
				MoveTemp(ArtifactId), Presence, MoveTemp(ContentHash));
			if (const FAssetDurableError* StateError = OutState.GetValidationDiagnostic())
			{
				OutError = *StateError;
				PrependDiagnosticPath(OutError, TEXT("journal.") + Path);
				return false;
			}
			return true;
		}

		TSharedRef<FJsonObject> BuildPayload(
			const FAssetDurableSessionRef& Session)
		{
			const TSharedRef<FJsonObject> Payload =
				MakeShared<FJsonObject>();
			Payload->SetStringField(
				TEXT("schema"),
				JournalSchemaV3);
			Payload->SetStringField(
				TEXT("journal_id"),
				Session->GetJournalId());
			Payload->SetStringField(
				TEXT("journal_owner_id"),
				Session->GetRequest().GetJournalOwnerId());
			Payload->SetNumberField(
				TEXT("fencing_token"),
				static_cast<double>(
					Session->GetRequest().GetFencingToken()));
			Payload->SetStringField(
				TEXT("phase"),
				Session->GetCurrentPhase());
			TArray<TSharedPtr<FJsonValue>> History;
			for (const FString& Phase : Session->GetPhaseHistory())
			{
				History.Add(MakeShared<FJsonValueString>(Phase));
			}
			Payload->SetArrayField(
				TEXT("phase_history"),
				MoveTemp(History));
			Payload->SetStringField(
				TEXT("plan_fingerprint"),
				Session->GetRequest().GetPlan()->GetFingerprint());
			Payload->SetStringField(
				TEXT("inventory_hash"),
				Session->GetInventory().GetInventoryHash());
			Payload->SetStringField(
				TEXT("transaction_root"),
				Session->GetTransactionRoot());
			Payload->SetStringField(
				TEXT("staging_root"),
				Session->GetStagingRoot());
			TArray<TSharedPtr<FJsonValue>> Artifacts;
			for (const FDurableArtifact& Artifact :
				Session->GetInventory().GetArtifacts())
			{
				const TSharedRef<FJsonObject> Value =
					MakeShared<FJsonObject>();
				Value->SetStringField(
					TEXT("artifact_id"),
					Artifact.GetArtifactId());
				Value->SetObjectField(
					TEXT("target_identity"),
					IdentityObject(Artifact.GetTargetIdentity()));
				Value->SetStringField(
					TEXT("artifact_kind"),
					LexToString(Artifact.GetArtifactKind()));
				Value->SetStringField(
					TEXT("durable_location"),
					Artifact.GetDurableLocation());
				Value->SetStringField(
					TEXT("package_name"),
					Artifact.GetPackageName());
				Value->SetStringField(
					TEXT("final_path"),
					Artifact.GetFinalPath());
				Value->SetStringField(
					TEXT("intent"),
					LexToString(Artifact.GetIntent()));
				Value->SetStringField(
					TEXT("role"),
					Artifact.GetRole());
				Value->SetObjectField(
					TEXT("pre_state"),
					StateObject(
						FindState(
							Session->GetPreMutationStates(),
							Artifact.GetArtifactId()),
						Artifact.GetArtifactId()));
				Value->SetObjectField(
					TEXT("expected_persisted_state"),
					StateObject(
						FindState(
							Session->GetExpectedPersistedStates(),
							Artifact.GetArtifactId()),
						Artifact.GetArtifactId()));
				Artifacts.Add(MakeShared<FJsonValueObject>(Value));
			}
			Payload->SetArrayField(
				TEXT("artifacts"),
				MoveTemp(Artifacts));
			return Payload;
		}

		bool TryWriteJournal(
			const FAssetDurableSessionRef& Session,
			FUeAgentResourceTracker& Resources,
			FAssetDurableError& OutError)
		{
			if (!IsWithin(
				Session->GetJournalPath(),
				Session->GetJournalRoot() / TEXT("active")))
			{
				OutError = BackendFailure(TEXT("durable_journal_root_invalid"), TEXT("The active durability journal path is outside its journal root."), FString());
				OutError.InternalContext = Session->GetJournalPath();
				return false;
			}
			FBoundTerminalEvidence TerminalEvidence;
			if (!TryReadBoundCompletedTerminal(
					Session,
					Resources,
					TerminalEvidence,
					OutError))
			{
				return false;
			}
			if (TerminalEvidence.bPresent
				&& Session->GetCurrentPhase() != TerminalEvidence.Phase)
			{
				OutError = BackendFailure(TEXT("durable_journal_already_terminal"), TEXT("The durability journal already has a completed phase that differs from the phase being written."), FString());
				OutError.PropertyPath = TEXT("journal.phase");
				OutError.ActualValue = MakeShared<FJsonValueString>(Session->GetCurrentPhase());
				OutError.ExpectedValue = MakeShared<FJsonValueString>(TerminalEvidence.Phase);
				return false;
			}
			FString PayloadText;
			if (!TrySerializeObject(BuildPayload(Session), PayloadText))
			{
				OutError = BackendFailure(TEXT("durable_journal_serialize_failed"), TEXT("The durability journal payload could not be encoded as JSON."), FString());
				return false;
			}
			const TSharedRef<FJsonObject> Outer =
				MakeShared<FJsonObject>();
			Outer->SetStringField(TEXT("payload"), PayloadText);
			Outer->SetStringField(
				TEXT("integrity_sha256"),
				Sha256(PayloadText));
			FString Encoded;
			if (!TrySerializeObject(Outer, Encoded))
			{
				OutError = BackendFailure(TEXT("durable_journal_serialize_failed"), TEXT("The durability journal envelope could not be encoded as JSON."), FString());
				return false;
			}
			const FTCHARToUTF8 Utf8(*Encoded);
			if (!TryCharge(
				Resources,
				TEXT("/c8d/journal/write"),
				1,
				0,
				0,
				Utf8.Length(),
				1,
				OutError))
			{
				return false;
			}
			IFileManager::Get().MakeDirectory(
				*FPaths::GetPath(Session->GetJournalPath()),
				true);
			const FString Temporary =
				Session->GetJournalPath() + TEXT(".tmp");
			if (!FFileHelper::SaveStringToFile(
				Encoded,
				*Temporary,
				FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
			{
				OutError = MakeFileOperationFailure(TEXT("durable_journal_write_failed"), TEXT("Writing the temporary durability journal file failed."), TEXT("asset_durable"), Temporary);
				IFileManager::Get().Delete(*Temporary);
				return false;
			}
			if (!IFileManager::Get().Move(
					*Session->GetJournalPath(),
					*Temporary,
					true,
					true,
					false,
					true))
			{
				OutError = MakeFileOperationFailure(TEXT("durable_journal_write_failed"), TEXT("Replacing the active durability journal with the written file failed."), TEXT("asset_durable"), Session->GetJournalPath());
				IFileManager::Get().Delete(*Temporary);
				return false;
			}
			return true;
		}

		bool TryValidateTargetAgainstPlan(
			const FPlanningPreflightPlanRef& Plan,
			const FAssetDurableTarget& Target,
			FAssetDurableError& OutError)
		{
			const FString& IdentityKey =
				Target.GetIdentity().GetCanonicalKey();
			const FPlannedChange* PlannedChange =
				Plan->GetChangeSet().GetChanges().FindByPredicate(
					[&IdentityKey](const FPlannedChange& Change)
					{
						return Change.GetIdentity().GetCanonicalKey()
							== IdentityKey;
					});
			if (!PlannedChange)
			{
				OutError = FAssetDurableError::Failure(TEXT("durable_target_not_in_plan"), TEXT("The durable target has no corresponding change in the execution plan."), TEXT("asset_durable"), IdentityKey);
				OutError.SourceKind = TEXT("native_result");
				OutError.TargetPath = Target.GetDurableLocation();
				return false;
			}
			const bool bTargetDeletes =
				Target.GetIntent() == EAssetDurableIntent::Delete;
			const bool bPlanDeletes =
				PlannedChange->GetIntent()
				== UE::UAI::PlanningPreflight::EChangeIntent::Delete;
			if (bTargetDeletes != bPlanDeletes)
			{
				OutError = FAssetDurableError::Failure(TEXT("durable_intent_plan_mismatch"), TEXT("The durable target and execution plan disagree about whether the target is being deleted."), TEXT("asset_durable"), IdentityKey);
				OutError.SourceKind = TEXT("native_result");
				OutError.TargetPath = Target.GetDurableLocation();
				OutError.PropertyPath = TEXT("intent");
				OutError.ActualValue = MakeShared<FJsonValueString>(LexToString(Target.GetIntent()));
				OutError.ExpectedValue = MakeShared<FJsonValueString>(bPlanDeletes ? TEXT("delete") : TEXT("non-delete intent"));
				return false;
			}
			return true;
		}

		bool TryValidateRequest(
			const FAssetDurableRequest& Request,
			FAssetDurableError& OutError)
		{
			if (!Request.IsValid())
			{
				OutError = Request.GetValidationDiagnostic();
				return false;
			}
			TSet<FString> Seen;
			TSet<FString> Owners;
			for (int32 Index = 0; Index < Request.GetTargets().Num(); ++Index)
			{
				const FAssetDurableTarget& Target = Request.GetTargets()[Index];
				const FString Key = Target.GetDurableTargetKey();
				if (!TryValidateTargetAgainstPlan(
						Request.GetPlan(),
						Target,
						OutError))
				{
					AddTargetIndex(OutError, TEXT("targets"), Index);
					return false;
				}
				if (Seen.Contains(Key))
				{
				OutError = FAssetDurableError::Failure(TEXT("durable_target_duplicate"), TEXT("The request contains more than one durable target for the same storage location."), TEXT("asset_durable"), Key);
				OutError.TargetPath = Target.GetDurableLocation();
					OutError.PropertyPath = TEXT("durable_location");
				OutError.SourceKind = TEXT("native_result");
					AddTargetIndex(OutError, TEXT("targets"), Index);
					return false;
				}
				Seen.Add(Key);
				Owners.Add(
					LexToString(Target.GetArtifactKind())
						+ TEXT(":")
						+ Target.GetDurableLocation());
				if (Target.GetRollbackGrade() == ERollbackGrade::None)
				{
				OutError = FAssetDurableError::Failure(TEXT("durable_rollback_grade_unsupported"), TEXT("The durable target does not support rollback."), TEXT("asset_durable"), Key);
				OutError.TargetPath = Target.GetDurableLocation();
				OutError.PropertyPath = TEXT("rollback_grade");
				OutError.SourceKind = TEXT("native_result");
				OutError.ActualValue = MakeShared<FJsonValueString>(TEXT("none"));
					AddTargetIndex(OutError, TEXT("targets"), Index);
					return false;
				}
			}
			if (Request.GetAtomicityMode()
					== EAtomicityMode::SingleOwnerAtomic
				&& Owners.Num() != 1)
			{
				OutError = FAssetDurableError::Failure(TEXT("durable_atomicity_mismatch"), TEXT("Single-owner atomic execution requires exactly one durable storage owner."), TEXT("asset_durable"));
				OutError.PropertyPath = TEXT("targets");
				OutError.ActualValue = MakeShared<FJsonValueNumber>(Owners.Num());
				OutError.ExpectedValue = MakeShared<FJsonValueNumber>(1);
				OutError.SourceKind = TEXT("native_result");
				return false;
			}
			if (Request.GetAtomicityMode()
				== EAtomicityMode::NonAtomicRejected)
			{
				OutError = FAssetDurableError::Failure(TEXT("durable_atomicity_mismatch"), TEXT("The durability request selects a rejected non-atomic execution mode."), TEXT("asset_durable"));
				OutError.PropertyPath = TEXT("atomicity_mode");
				OutError.ActualValue = MakeShared<FJsonValueString>(LexToString(Request.GetAtomicityMode()));
				OutError.SourceKind = TEXT("native_result");
				return false;
			}
			return true;
		}

		bool TryValidateInventory(
			const FAssetDurableRequest& Request,
			const FDurableInventory& Inventory,
			const TArray<FString>& RawAllowedRoots,
			FUeAgentResourceTracker& Resources,
			FAssetDurableError& OutError)
		{
			if (const FAssetDurableError* InventoryError = Inventory.GetValidationDiagnostic())
			{
				OutError = *InventoryError;
				return false;
			}
			if (RawAllowedRoots.IsEmpty())
			{
				OutError = BackendFailure(TEXT("durable_inventory_invalid"), TEXT("No allowed artifact directories were supplied for inventory validation."), FString());
				return false;
			}
			TArray<FString> AllowedRoots;
			for (const FString& Root : RawAllowedRoots)
			{
				const FString Normalized = FullPath(Root);
				if (!Normalized.IsEmpty())
				{
					AllowedRoots.Add(Normalized);
				}
			}
			if (AllowedRoots.IsEmpty())
			{
				OutError = BackendFailure(TEXT("durable_artifact_root_unavailable"), TEXT("None of the supplied artifact directories resolves to a non-empty path."), FString());
				return false;
			}
			TMap<FString, const FAssetDurableTarget*> Targets;
			for (const FAssetDurableTarget& Target :
				Request.GetTargets())
			{
				Targets.Add(
					Target.GetDurableTargetKey(),
					&Target);
			}
			FString PreviousId;
			TSet<FString> Seen;
			int32 ArtifactIndex = 0;
			for (const FDurableArtifact& Artifact :
				Inventory.GetArtifacts())
			{
				const FString Key =
					LexToString(Artifact.GetArtifactKind())
					+ TEXT(":")
					+ Artifact.GetDurableLocation().ToLower();
				const FAssetDurableTarget* const* TargetPtr =
					Targets.Find(Key);
				if (!TargetPtr)
				{
					OutError = BackendFailure(TEXT("durable_inventory_invalid"), TEXT("The durable inventory artifact has no matching request target."), Artifact.GetDurableLocation());
					OutError.PropertyPath = FString::Printf(TEXT("artifacts[%d]"), ArtifactIndex);
					return false;
				}
				if ((*TargetPtr)->GetIdentity().GetCanonicalKey()
						!= Artifact.GetTargetIdentity()
							.GetCanonicalKey()
					|| (*TargetPtr)->GetArtifactKind()
						!= Artifact.GetArtifactKind()
					|| (*TargetPtr)->GetDurableLocation()
						!= Artifact.GetDurableLocation()
					|| (*TargetPtr)->GetIntent()
						!= Artifact.GetIntent())
				{
					const FAssetDurableTarget& Target = **TargetPtr;
					const bool bIdentityDiffers = Target.GetIdentity().GetCanonicalKey() != Artifact.GetTargetIdentity().GetCanonicalKey();
					const bool bKindDiffers = Target.GetArtifactKind() != Artifact.GetArtifactKind();
					const bool bLocationDiffers = Target.GetDurableLocation() != Artifact.GetDurableLocation();
					const FString Field = bIdentityDiffers ? TEXT("target_identity") : bKindDiffers ? TEXT("artifact_kind") : bLocationDiffers ? TEXT("durable_location") : TEXT("intent");
					OutError = BackendFailure(TEXT("durable_inventory_invalid"), FString::Printf(TEXT("The durable artifact %s differs from its request target."), *Field), Artifact.GetDurableLocation());
					OutError.PropertyPath = FString::Printf(TEXT("artifacts[%d].%s"), ArtifactIndex, *Field);
					if (bIdentityDiffers)
					{
						OutError.ActualEvidence = MakeShared<FJsonValueString>(Artifact.GetTargetIdentity().GetCanonicalKey());
						OutError.ExpectedEvidence = MakeShared<FJsonValueString>(Target.GetIdentity().GetCanonicalKey());
					}
					else
					{
						OutError.ActualValue = MakeShared<FJsonValueString>(bKindDiffers ? FString(LexToString(Artifact.GetArtifactKind())) : bLocationDiffers ? Artifact.GetDurableLocation() : FString(LexToString(Artifact.GetIntent())));
						OutError.ExpectedValue = MakeShared<FJsonValueString>(bKindDiffers ? FString(LexToString(Target.GetArtifactKind())) : bLocationDiffers ? Target.GetDurableLocation() : FString(LexToString(Target.GetIntent())));
					}
					return false;
				}
				if (Seen.Contains(Artifact.GetArtifactId()) || (!PreviousId.IsEmpty() && Artifact.GetArtifactId() < PreviousId))
				{
					OutError = BackendFailure(TEXT("durable_inventory_invalid"), Seen.Contains(Artifact.GetArtifactId())
						? TEXT("The durable inventory contains a duplicate artifact identifier.")
						: TEXT("The durable inventory artifact identifiers are not in ascending order."), Artifact.GetDurableLocation());
					OutError.PropertyPath = FString::Printf(TEXT("artifacts[%d].artifact_id"), ArtifactIndex);
					OutError.ActualEvidence = MakeShared<FJsonValueString>(Artifact.GetArtifactId());
					OutError.ExpectedEvidence = MakeShared<FJsonValueString>(PreviousId);
					return false;
				}
				bool bAllowed = false;
				for (const FString& Root : AllowedRoots)
				{
					if (IsWithin(Artifact.GetFinalPath(), Root))
					{
						bAllowed = true;
						break;
					}
				}
				if (!bAllowed)
				{
					OutError = BackendFailure(TEXT("durable_artifact_path_invalid"), TEXT("The durable artifact file is outside the allowed artifact directories."), Artifact.GetDurableLocation());
					OutError.PropertyPath = FString::Printf(TEXT("artifacts[%d].final_path"), ArtifactIndex);
					OutError.InternalContext = Artifact.GetFinalPath();
					return false;
				}
				Seen.Add(Artifact.GetArtifactId());
				PreviousId = Artifact.GetArtifactId();
				++ArtifactIndex;
			}
			FString ExpectedHash;
			if (!TryComputeDurableInventoryHash(
					Inventory.GetArtifacts(),
					Resources,
					ExpectedHash,
					OutError))
			{
				return false;
			}
			if (Inventory.GetInventoryHash() != ExpectedHash)
			{
				OutError = BackendFailure(TEXT("durable_inventory_invalid"), TEXT("The durable inventory hash does not match its artifacts."), FString());
				OutError.PropertyPath = TEXT("inventory_hash");
				OutError.ActualEvidence = MakeShared<FJsonValueString>(Inventory.GetInventoryHash());
				OutError.ExpectedEvidence = MakeShared<FJsonValueString>(ExpectedHash);
				return false;
			}
			return true;
		}

		bool TryCheckFence(
			const FAssetDurableSessionRef& Session,
			const int64 CurrentFencingToken,
			FAssetDurableError& OutError)
		{
			if (CurrentFencingToken < 0
				|| CurrentFencingToken
					!= Session->GetRequest().GetFencingToken())
			{
				OutError = BackendFailure(TEXT("durable_fence_stale"), CurrentFencingToken < 0
					? TEXT("The current durability fencing token must not be negative.")
					: TEXT("The current durability fencing token differs from the token bound to the session."), FString());
				OutError.PropertyPath = TEXT("fencing_token");
				OutError.ActualEvidence = MakeShared<FJsonValueString>(FString::Printf(TEXT("%lld"), CurrentFencingToken));
				OutError.ExpectedEvidence = MakeShared<FJsonValueString>(FString::Printf(TEXT("%lld"), Session->GetRequest().GetFencingToken()));
				return false;
			}
			return true;
		}

		FDurableReceipt Receipt(
			const FAssetDurableSessionRef& Session,
			const EDurableStatus Status,
			const ERecoveryAction Action,
			const bool bRequiresIndependent,
			const bool bReplayed,
			FAssetDurableError Failure = {})
		{
			return FDurableReceipt(Session->GetJournalId(),
				Status,
				Session->GetRequest().GetPlan()->GetFingerprint(),
				Session->GetInventory().GetInventoryHash(),
				Session->GetPhaseHistory(),
				Action,
				bRequiresIndependent,
				bReplayed,
				MoveTemp(Failure),
				FString(),
				Session->GetPreMutationStates(),
				Session->GetExpectedPersistedStates());
		}

		bool TryArchiveTerminal(
			const FAssetDurableSessionRef& Session,
			FUeAgentResourceTracker& Resources,
			FAssetDurableError& OutError)
		{
			const FString CompletedRoot =
				Session->GetJournalRoot() / TEXT("completed");
			const FString CompletedPath =
				CompletedRoot
				/ (Session->GetJournalId() + TEXT(".json"));
			if (!IsWithin(CompletedPath, CompletedRoot))
			{
				OutError = BackendFailure(TEXT("durable_journal_root_invalid"), TEXT("The completed durability journal path is outside the completed journal root."), FString());
				OutError.InternalContext = CompletedPath;
				return false;
			}
			if (!IsWithin(Session->GetTransactionRoot(), Session->GetJournalRoot() / TEXT("active")))
			{
				OutError = BackendFailure(TEXT("durable_journal_root_invalid"), TEXT("The durability transaction directory is outside the active journal root."), FString());
				OutError.InternalContext = Session->GetTransactionRoot();
				return false;
			}
			if (!TryCharge(
				Resources,
				TEXT("/c8d/journal/archive"),
				1,
				0,
				0,
				0,
				1,
				OutError))
			{
				return false;
			}
			IFileManager::Get().MakeDirectory(*CompletedRoot, true);
			FString ArchivePath = CompletedPath;
			bool bNeedsCopy = true;
			if (IFileManager::Get().FileExists(*CompletedPath))
			{
				TArray<uint8> CurrentBytes;
				TArray<uint8> CompletedBytes;
				if (!FFileHelper::LoadFileToArray(
						CurrentBytes,
						*Session->GetJournalPath()))
				{
					OutError = MakeFileOperationFailure(TEXT("durable_journal_archive_failed"), TEXT("The active journal could not be read for archive comparison."), TEXT("asset_durable"), Session->GetJournalPath());
					return false;
				}
				if (!FFileHelper::LoadFileToArray(
						CompletedBytes,
						*CompletedPath))
				{
					OutError = MakeFileOperationFailure(TEXT("durable_journal_archive_failed"), TEXT("The completed journal could not be read for archive comparison."), TEXT("asset_durable"), CompletedPath);
					return false;
				}
				if (!TryCharge(
						Resources,
						TEXT("/c8d/journal/archive_collision"),
						0,
						2,
						CurrentBytes.Num() + CompletedBytes.Num(),
						0,
						1,
						OutError))
				{
					return false;
				}
				if (CurrentBytes == CompletedBytes)
				{
					bNeedsCopy = false;
				}
				else
				{
					FString Hash = Sha256Bytes(CurrentBytes);
					Hash.RemoveFromStart(TEXT("sha256:"));
					ArchivePath = CompletedRoot / FString::Printf(
						TEXT("%s.%s.%s.json"),
						*Session->GetJournalId(),
						*Session->GetCurrentPhase(),
						*Hash.Left(12));
					if (!IsWithin(ArchivePath, CompletedRoot))
					{
						OutError = BackendFailure(TEXT("durable_journal_root_invalid"), TEXT("The conflict archive path is outside the completed journal root."), FString());
						OutError.InternalContext = ArchivePath;
						return false;
					}
					if (IFileManager::Get().FileExists(*ArchivePath))
					{
						TArray<uint8> ExistingBytes;
						if (!FFileHelper::LoadFileToArray(
								ExistingBytes,
								*ArchivePath))
						{
							OutError = MakeFileOperationFailure(TEXT("durable_journal_archive_collision"), TEXT("The existing conflict archive could not be read for comparison."), TEXT("asset_durable"), ArchivePath);
							return false;
						}
						if (ExistingBytes != CurrentBytes)
						{
							OutError = BackendFailure(TEXT("durable_journal_archive_collision"), TEXT("The conflict archive already contains different journal data."), FString());
							OutError.InternalContext = ArchivePath;
							return false;
						}
						bNeedsCopy = false;
					}
				}
			}
			if (bNeedsCopy
					&& IFileManager::Get().Copy(
						*ArchivePath,
						*Session->GetJournalPath(),
						false,
						true)
						!= COPY_OK)
			{
				OutError = MakeFileOperationFailure(TEXT("durable_journal_archive_failed"), TEXT("Copying the terminal durability journal into the archive failed."), TEXT("asset_durable"), ArchivePath);
				return false;
			}
			if (!IFileManager::Get().DeleteDirectory(
					*Session->GetTransactionRoot(),
					false,
					true))
			{
				OutError = MakeFileOperationFailure(TEXT("durable_journal_archive_failed"), TEXT("Removing the archived durability transaction directory failed."), TEXT("asset_durable"), Session->GetTransactionRoot());
				return false;
			}
			return true;
		}

		bool TryRecordUnknown(
			const FAssetDurableSessionRef& Session,
			FUeAgentResourceTracker& Resources,
			const bool bReplayed,
			FDurableReceipt& OutReceipt,
			FAssetDurableError& OutError,
			FAssetDurableError Failure)
		{
			const FAssetDurableSessionRef Unknown =
				Advance(Session, TEXT("state_unknown"));
			FAssetDurableError JournalError;
			if (!TryWriteJournal(Unknown, Resources, JournalError))
			{
				if (!JournalError.IsEmpty()) Failure.Diagnostics.Add(MoveTemp(JournalError));
				OutError = MoveTemp(Failure);
				return false;
			}
			OutReceipt = Receipt(
				Unknown,
				EDurableStatus::StateUnknown,
				ERecoveryAction::ManualIntervention,
				false,
				bReplayed,
				MoveTemp(Failure));
			OutError = FAssetDurableError();
			return true;
		}

		bool TryRestoreAfterFailure(
			const FAssetDurableSessionRef& Session,
			IAssetDurableBackend& Backend,
			FUeAgentResourceTracker& Resources,
			const bool bReplayed,
			FDurableReceipt& OutReceipt,
			FAssetDurableError& OutError)
		{
			bool bConfirmed = false;
			FAssetDurableError RestoreError;
			const bool bRestored = Backend.Restore(
				Session,
				Resources,
				bConfirmed,
				RestoreError);
			if (!bRestored || !bConfirmed)
			{
				if (RestoreError.IsEmpty())
				{
					RestoreError = FAssetDurableError::Failure(TEXT("durable_restore_unconfirmed"), bRestored
						? TEXT("The durability backend did not confirm restoration of the original state.")
						: TEXT("The durability backend failed to restore the original state without reporting a diagnostic."), TEXT("asset_durable"));
					RestoreError.SourceKind = TEXT("native_result");
				}
				RestoreError.RollbackAttempted = true;
				RestoreError.RollbackSucceeded = false;
				RestoreError.bRetrySafe = false;
				return TryRecordUnknown(
					Session,
					Resources,
					bReplayed,
					OutReceipt,
					OutError,
					MoveTemp(RestoreError));
			}
			const FAssetDurableSessionRef RolledBack =
				Advance(Session, TEXT("rolled_back"));
			if (!TryWriteJournal(RolledBack, Resources, OutError)
				|| !TryArchiveTerminal(
					RolledBack,
					Resources,
					OutError))
			{
				return false;
			}
			OutReceipt = Receipt(
				RolledBack,
				EDurableStatus::RolledBack,
				ERecoveryAction::RestoredOriginal,
				false,
				bReplayed);
			OutError = FAssetDurableError();
			return true;
		}

		bool TryRestorePersistFailure(
			const FAssetDurableSessionRef& Session,
			IAssetDurableBackend& Backend,
			FUeAgentResourceTracker& Resources,
			FAssetDurableError Failure,
			FDurableReceipt& OutReceipt,
			FAssetDurableError& OutError)
		{
			if (Failure.IsEmpty())
				Failure = FAssetDurableError::Failure(TEXT("durable_persist_failed"), TEXT("Durable persistence failed without reporting a diagnostic."), TEXT("asset_durable"));
			if (!TryRestoreAfterFailure(Session, Backend, Resources, false, OutReceipt, OutError))
			{
				Failure.Diagnostics.Add(OutError);
				Failure.RollbackAttempted = true;
				Failure.RollbackSucceeded = false;
				Failure.bRetrySafe = false;
				OutError = MoveTemp(Failure);
				return false;
			}
			// Recovery success must not erase the reason persistence did not succeed.
			const FAssetDurableError& RecoveryFailure = OutReceipt.GetFailureDiagnostic();
			if (!RecoveryFailure.IsEmpty()) Failure.Diagnostics.Add(RecoveryFailure);
			Failure.RollbackAttempted = true;
			Failure.RollbackSucceeded = OutReceipt.GetStatus() == EDurableStatus::RolledBack;
			if (!Failure.RollbackSucceeded.GetValue()) Failure.bRetrySafe = false;
			OutReceipt = FDurableReceipt(OutReceipt.GetJournalId(),
				OutReceipt.GetStatus(),
				OutReceipt.GetPlanFingerprint(),
				OutReceipt.GetInventoryHash(),
				OutReceipt.GetPhaseHistory(),
				OutReceipt.GetRecoveryAction(),
				OutReceipt.RequiresIndependentVerification(),
				OutReceipt.IsReplayed(),
				Failure,
				OutReceipt.GetVerificationId(),
				OutReceipt.GetPreMutationStates(),
				OutReceipt.GetExpectedPersistedStates());
			return true;
		}

		bool TryReadOuterJournal(
			const FString& JournalPath,
			FUeAgentResourceTracker& Resources,
			FString& OutPayloadText,
			FAssetDurableError& OutError)
		{
			FString Encoded;
			if (!FFileHelper::LoadFileToString(Encoded, *JournalPath))
			{
				OutError = MakeFileOperationFailure(TEXT("durable_journal_integrity_invalid"), TEXT("The durability journal file could not be read."), TEXT("asset_durable"), JournalPath);
				return false;
			}
			const FTCHARToUTF8 Utf8(*Encoded);
			if (!TryCharge(
				Resources,
				TEXT("/c8d/journal/read"),
				0,
				0,
				Utf8.Length(),
				0,
				1,
				OutError))
			{
				return false;
			}
			TSharedPtr<FJsonObject> Outer;
			const TSharedRef<TJsonReader<>> Reader =
				TJsonReaderFactory<>::Create(Encoded);
			FString Integrity;
			if (!FJsonSerializer::Deserialize(Reader, Outer) || !Outer.IsValid())
			{
				OutError = JournalFailure(TEXT("The durability journal envelope is not a valid JSON object."), TEXT("envelope"));
				OutError.InternalContext = JournalPath;
				OutError.ActualEvidence = MakeShared<FJsonValueString>(Reader->GetErrorMessage());
				return false;
			}
			FJournalFields Fields{Outer, TEXT("envelope"), OutError};
			if (!Fields.String(TEXT("payload"), OutPayloadText)
				|| !Fields.String(TEXT("integrity_sha256"), Integrity)) return false;
			const FString ExpectedIntegrity = Sha256(OutPayloadText);
			if (Integrity != ExpectedIntegrity)
			{
				OutError = JournalFailure(TEXT("The durability journal integrity hash does not match its payload."), TEXT("envelope.integrity_sha256"));
				OutError.InternalContext = JournalPath;
				OutError.ActualEvidence = MakeShared<FJsonValueString>(Integrity);
				OutError.ExpectedEvidence = MakeShared<FJsonValueString>(ExpectedIntegrity);
				return false;
			}
			return true;
		}

		bool TryReadBoundCompletedTerminal(
			const FAssetDurableSessionRef& Session,
			FUeAgentResourceTracker& Resources,
			FBoundTerminalEvidence& OutEvidence,
			FAssetDurableError& OutError)
		{
			OutEvidence = FBoundTerminalEvidence();
			OutError = FAssetDurableError();
			const FString CompletedRoot =
				Session->GetJournalRoot() / TEXT("completed");
			const FString CompletedPath = CompletedRoot
				/ (Session->GetJournalId() + TEXT(".json"));
			if (!IsWithin(CompletedPath, CompletedRoot))
			{
				OutError = BackendFailure(TEXT("durable_journal_root_invalid"), TEXT("The completed durability evidence path is outside its journal root."), FString());
				OutError.InternalContext = CompletedPath;
				return false;
			}
			if (!IFileManager::Get().FileExists(*CompletedPath))
			{
				return true;
			}

			FString PayloadText;
			FAssetDurableError ReadError;
			if (!TryReadOuterJournal(
					CompletedPath,
					Resources,
					PayloadText,
					ReadError))
			{
				if (IsResourceChargeError(ReadError))
				{
					OutError = ReadError;
					return false;
				}
				return true;
			}

			TSharedPtr<FJsonObject> Payload;
			const TSharedRef<TJsonReader<>> Reader =
				TJsonReaderFactory<>::Create(PayloadText);
			FString Schema;
			FString JournalId;
			FString OwnerId;
			FString Phase;
			FString Fingerprint;
			FString InventoryHash;
			double FencingTokenNumber = -1.0;
			const TArray<TSharedPtr<FJsonValue>>* HistoryValues = nullptr;
			if (!FJsonSerializer::Deserialize(Reader, Payload)
				|| !Payload.IsValid()
				|| !Payload->TryGetStringField(TEXT("schema"), Schema)
				|| (Schema != JournalSchemaV1
					&& Schema != JournalSchemaV2
					&& Schema != JournalSchemaV3)
				|| !Payload->TryGetStringField(
					TEXT("journal_id"),
					JournalId)
				|| !Payload->TryGetStringField(
					TEXT("journal_owner_id"),
					OwnerId)
				|| !Payload->TryGetNumberField(
					TEXT("fencing_token"),
					FencingTokenNumber)
				|| !Payload->TryGetStringField(TEXT("phase"), Phase)
				|| !Payload->TryGetStringField(
					TEXT("plan_fingerprint"),
					Fingerprint)
				|| !Payload->TryGetStringField(
					TEXT("inventory_hash"),
					InventoryHash)
				|| !Payload->TryGetArrayField(
					TEXT("phase_history"),
					HistoryValues)
				|| !FMath::IsFinite(FencingTokenNumber)
				|| FencingTokenNumber < 0.0
				|| FMath::FloorToDouble(FencingTokenNumber)
					!= FencingTokenNumber
				|| !IsTerminalPhase(Phase))
			{
				return true;
			}

			TArray<FString> History;
			for (const TSharedPtr<FJsonValue>& Value : *HistoryValues)
			{
				FString Item;
				if (!Value.IsValid()
					|| !Value->TryGetString(Item)
					|| !IsKnownPhase(Item))
				{
					return true;
				}
				History.Add(MoveTemp(Item));
			}
			if (History.IsEmpty()
				|| History.Last() != Phase
				|| JournalId != Session->GetJournalId()
				|| OwnerId
					!= Session->GetRequest().GetJournalOwnerId()
				|| FencingTokenNumber
					!= static_cast<double>(
						Session->GetRequest().GetFencingToken())
				|| Fingerprint
					!= Session->GetRequest().GetPlan()->GetFingerprint()
				|| InventoryHash
					!= Session->GetInventory().GetInventoryHash())
			{
				return true;
			}

			OutEvidence.bPresent = true;
			OutEvidence.Phase = MoveTemp(Phase);
			OutEvidence.PhaseHistory = MoveTemp(History);
			return true;
		}

		FPlanningPreflightPlanRef RecoveryPlan(
			const FString& JournalId,
			const FString& Fingerprint)
		{
			return MakeShared<const FPlanningPreflightPlan>(
				FUeAgentDescriptorIdentity(
					TEXT("uai.asset_durable.recovery"),
					1,
					TEXT(
						"sha256:1b885ad00666e86438e1f27e96f3918ebf550ee54428c2fef9b3db5e6a56a815")),
				TEXT("recovery:") + JournalId,
				TEXT("recovery:") + JournalId,
				TEXT("asset-durable-recovery"),
				TArray<FString>{},
				TEXT("journal:") + JournalId,
				FUeAgentStateRevision(
					TEXT("asset_durable_journal"),
					TEXT("recovery"),
					0),
				TEXT("journal:") + JournalId,
				FChangeSet(),
				FImpactSet(),
				TArray<UE::UAI::PlanningPreflight::FPrecondition>{},
				Fingerprint);
		}

		bool TryParseSession(
			const FString& PayloadText,
			const FString& JournalPath,
			const FString& JournalRoot,
			const TArray<FString>& InAllowedArtifactRoots,
			FUeAgentResourceTracker& Resources,
			FAssetDurableSessionPtr& OutSession,
			FAssetDurableError& OutError)
		{
			TSharedPtr<FJsonObject> Payload;
			const TSharedRef<TJsonReader<>> Reader =
				TJsonReaderFactory<>::Create(PayloadText);
			if (!FJsonSerializer::Deserialize(Reader, Payload)
				|| !Payload.IsValid())
			{
				OutError = JournalFailure(TEXT("The durability journal payload is not a valid JSON object."), TEXT("payload"));
				OutError.ActualEvidence = MakeShared<FJsonValueString>(Reader->GetErrorMessage());
				return false;
			}
			FString Schema;
			FString JournalId;
			FString OwnerId;
			FString Phase;
			FString Fingerprint;
			FString InventoryHash;
			FString TransactionRoot;
			FString StagingRoot;
			double FencingTokenNumber = -1.0;
			const TArray<TSharedPtr<FJsonValue>>* HistoryValues = nullptr;
			const TArray<TSharedPtr<FJsonValue>>* ArtifactValues = nullptr;
			FJournalFields Fields{Payload, FString(), OutError};
			if (!Fields.String(TEXT("schema"), Schema)) return false;
			if (Schema != JournalSchemaV1 && Schema != JournalSchemaV2 && Schema != JournalSchemaV3)
			{
				OutError = JournalFailure(TEXT("The durability journal schema version is unsupported."), TEXT("schema"));
				OutError.ActualValue = MakeShared<FJsonValueString>(Schema);
				OutError.ExpectedValue = MakeShared<FJsonValueString>(FString::Printf(TEXT("%s, %s, or %s"), JournalSchemaV1, JournalSchemaV2, JournalSchemaV3));
				return false;
			}
			if (!Fields.String(TEXT("journal_id"), JournalId)
				|| !Fields.String(TEXT("journal_owner_id"), OwnerId)
				|| !Fields.Number(TEXT("fencing_token"), FencingTokenNumber)
				|| !Fields.String(TEXT("phase"), Phase)
				|| !Fields.String(TEXT("plan_fingerprint"), Fingerprint)
				|| !Fields.String(TEXT("inventory_hash"), InventoryHash)
				|| !Fields.String(TEXT("transaction_root"), TransactionRoot)
				|| !Fields.String(TEXT("staging_root"), StagingRoot)
				|| !Fields.Array(TEXT("phase_history"), HistoryValues)
				|| !Fields.Array(TEXT("artifacts"), ArtifactValues)) return false;
			if (!FMath::IsFinite(FencingTokenNumber)
				|| FencingTokenNumber < 0.0
				|| FMath::FloorToDouble(FencingTokenNumber)
					!= FencingTokenNumber)
			{
				OutError = JournalFailure(TEXT("The durability journal fencing token must be a finite non-negative integer."), TEXT("fencing_token"));
				OutError.ActualEvidence = MakeShared<FJsonValueString>(FString::Printf(TEXT("%.17g"), FencingTokenNumber));
				return false;
			}
			const bool bTypedJournal =
				Schema == JournalSchemaV2
				|| Schema == JournalSchemaV3;
			const bool bStateBoundJournal =
				Schema == JournalSchemaV3;
			TArray<FString> History;
			for (const TSharedPtr<FJsonValue>& Value : *HistoryValues)
			{
				FString Item;
				if (!Value.IsValid()
					|| !Value->TryGetString(Item)
					|| !IsKnownPhase(Item))
				{
					OutError = JournalFailure(TEXT("Each durability journal phase history item must name a known phase."), FString::Printf(TEXT("phase_history[%d]"), History.Num()));
					OutError.ActualValue = Value;
					return false;
				}
				History.Add(MoveTemp(Item));
			}
			if (History.IsEmpty())
			{
				OutError = JournalFailure(TEXT("The durability journal phase history must not be empty."), TEXT("phase_history"));
				return false;
			}
			if (History.Last() != Phase || !IsKnownPhase(Phase))
			{
				OutError = JournalFailure(TEXT("The durability journal current phase must match the last known phase in its history."), TEXT("phase"));
				OutError.ActualValue = MakeShared<FJsonValueString>(Phase);
				OutError.ExpectedValue = MakeShared<FJsonValueString>(History.Last());
				return false;
			}
			const FString ExpectedTransaction =
				FullPath(FPaths::GetPath(JournalPath));
			if (!FullPath(TransactionRoot).Equals(
					ExpectedTransaction,
					ESearchCase::IgnoreCase))
			{
				OutError = JournalFailure(TEXT("The transaction directory recorded in the journal differs from the directory containing the journal."), TEXT("transaction_root"));
				OutError.ActualEvidence = MakeShared<FJsonValueString>(TransactionRoot);
				OutError.ExpectedEvidence = MakeShared<FJsonValueString>(ExpectedTransaction);
				return false;
			}
			if (!IsWithin(TransactionRoot, JournalRoot / TEXT("active")))
			{
				OutError = JournalFailure(TEXT("The journal transaction directory is outside the active journal root."), TEXT("transaction_root"));
				OutError.ActualEvidence = MakeShared<FJsonValueString>(TransactionRoot);
				return false;
			}
			if (!IsWithin(StagingRoot, TransactionRoot))
			{
				OutError = JournalFailure(TEXT("The journal staging directory is outside its transaction directory."), TEXT("staging_root"));
				OutError.ActualEvidence = MakeShared<FJsonValueString>(StagingRoot);
				return false;
			}
			TArray<FString> AllowedRoots = InAllowedArtifactRoots;
			if (AllowedRoots.IsEmpty())
			{
				OutError = JournalFailure(TEXT("No allowed artifact directories are available for durability recovery."), TEXT("artifacts"));
				return false;
			}
			TArray<FDurableArtifact> Artifacts;
			TArray<FDurableArtifactState> PreMutationStates;
			TArray<FDurableArtifactState> ExpectedPersistedStates;
			TMap<FString, FAssetDurableTarget> TargetsByKey;
			TSet<FString> ArtifactIds;
			FString PreviousArtifactId;
			for (const TSharedPtr<FJsonValue>& Value : *ArtifactValues)
			{
				const FString ArtifactPath = FString::Printf(TEXT("artifacts[%d]"), Artifacts.Num());
				const TSharedPtr<FJsonObject> ArtifactObject =
					Value.IsValid() && Value->Type == EJson::Object
						? Value->AsObject()
						: nullptr;
				const TSharedPtr<FJsonObject>* IdentityPtr = nullptr;
				FString ArtifactId;
				FString ArtifactKindText;
				FString DurableLocation;
				FString PackageName;
				FString FinalPathValue;
				FString IntentText;
				FString Role;
				const TSharedPtr<FJsonObject>* PreStatePtr = nullptr;
				const TSharedPtr<FJsonObject>* ExpectedStatePtr = nullptr;
				if (!ArtifactObject.IsValid())
				{
					OutError = JournalFailure(TEXT("Each durability journal artifact must be an object."), ArtifactPath);
					OutError.ActualEvidence = Value;
					return false;
				}
				FJournalFields ArtifactFields{ArtifactObject, ArtifactPath, OutError};
				if (!ArtifactFields.String(TEXT("artifact_id"), ArtifactId)
					|| !ArtifactFields.Object(TEXT("target_identity"), IdentityPtr)
					|| !ArtifactFields.String(TEXT("package_name"), PackageName)
					|| !ArtifactFields.String(TEXT("final_path"), FinalPathValue)
					|| !ArtifactFields.String(TEXT("intent"), IntentText)
					|| !ArtifactFields.String(TEXT("role"), Role)) return false;
				if (bStateBoundJournal && (!ArtifactFields.Object(TEXT("pre_state"), PreStatePtr)
						|| !ArtifactFields.Object(TEXT("expected_persisted_state"), ExpectedStatePtr))) return false;
				if (bTypedJournal && (!ArtifactFields.String(TEXT("artifact_kind"), ArtifactKindText)
					|| !ArtifactFields.String(TEXT("durable_location"), DurableLocation))) return false;
				bool bAllowed = false;
				for (const FString& Root : AllowedRoots)
				{
					if (IsWithin(FinalPathValue, Root))
					{
						bAllowed = true;
						break;
					}
				}
				if (!bAllowed)
				{
					OutError = JournalFailure(TEXT("The journal artifact file path is outside the allowed artifact directories."), ArtifactPath + TEXT(".final_path"));
					OutError.ActualEvidence = MakeShared<FJsonValueString>(FinalPathValue);
					return false;
				}
				FUeAgentCanonicalIdentity Identity(
					TEXT("invalid"),
					TEXT("invalid"),
					TEXT("invalid"),
					TEXT("invalid"),
					TEXT("invalid"),
					TEXT("invalid"));
				EAssetDurableIntent Intent =
					EAssetDurableIntent::Save;
				EDurableArtifactKind ArtifactKind =
					EDurableArtifactKind::Package;
				if (!bTypedJournal)
				{
					DurableLocation = PackageName;
				}
				if (!TryParseIdentity(
						IdentityPtr ? *IdentityPtr : nullptr,
						ArtifactPath + TEXT(".target_identity"),
						Identity,
						OutError)) return false;
				if (!TryIntentFromString(IntentText, Intent))
				{
					OutError = JournalFailure(TEXT("The journal artifact intent must be save, delete, or native."), ArtifactPath + TEXT(".intent"));
					OutError.ActualValue = MakeShared<FJsonValueString>(IntentText);
					return false;
				}
				if (bTypedJournal && !TryArtifactKindFromString(ArtifactKindText, ArtifactKind))
				{
					OutError = JournalFailure(TEXT("The journal artifact kind must be package or project_config."), ArtifactPath + TEXT(".artifact_kind"));
					OutError.ActualValue = MakeShared<FJsonValueString>(ArtifactKindText);
					return false;
				}
				if (ArtifactKind == EDurableArtifactKind::Package
					&& PackageName != DurableLocation)
				{
					OutError = JournalFailure(TEXT("The journal package artifact durable location must match its package name."), ArtifactPath + TEXT(".durable_location"));
					OutError.ActualValue = MakeShared<FJsonValueString>(DurableLocation);
					OutError.ExpectedValue = MakeShared<FJsonValueString>(PackageName);
					return false;
				}
				if (ArtifactIds.Contains(ArtifactId)
					|| (!PreviousArtifactId.IsEmpty()
						&& ArtifactId < PreviousArtifactId))
				{
					OutError = JournalFailure(ArtifactIds.Contains(ArtifactId)
						? TEXT("The journal artifact identifier is duplicated.")
						: TEXT("The journal artifact identifiers are not in ascending order."), ArtifactPath + TEXT(".artifact_id"));
					OutError.ActualEvidence = MakeShared<FJsonValueString>(ArtifactId);
					OutError.ExpectedEvidence = MakeShared<FJsonValueString>(PreviousArtifactId);
					return false;
				}
				const FAssetDurableTarget ParsedTarget(
					Identity,
					ArtifactKind,
					DurableLocation,
					Intent,
					ERollbackGrade::DurableBackup);
				const FString TargetKey =
					ParsedTarget.GetDurableTargetKey();
				const FAssetDurableTarget* ExistingTarget =
					TargetsByKey.Find(TargetKey);
				if (ExistingTarget
					&& (ExistingTarget->GetIdentity()
							.GetCanonicalKey()
							!= Identity.GetCanonicalKey()
						|| ExistingTarget->GetArtifactKind()
							!= ArtifactKind
						|| ExistingTarget->GetDurableLocation()
							!= DurableLocation
						|| ExistingTarget->GetIntent() != Intent))
				{
					const bool bIdentityDiffers = ExistingTarget->GetIdentity().GetCanonicalKey() != Identity.GetCanonicalKey();
					const bool bKindDiffers = ExistingTarget->GetArtifactKind() != ArtifactKind;
					const bool bLocationDiffers = ExistingTarget->GetDurableLocation() != DurableLocation;
					const FString Field = bIdentityDiffers ? TEXT("target_identity") : bKindDiffers ? TEXT("artifact_kind") : bLocationDiffers ? TEXT("durable_location") : TEXT("intent");
					OutError = JournalFailure(FString::Printf(TEXT("Journal artifacts for the same durable target disagree on %s."), *Field), ArtifactPath + TEXT(".") + Field);
					if (bIdentityDiffers)
					{
						OutError.ActualEvidence = MakeShared<FJsonValueString>(Identity.GetCanonicalKey());
						OutError.ExpectedEvidence = MakeShared<FJsonValueString>(ExistingTarget->GetIdentity().GetCanonicalKey());
					}
					else
					{
						OutError.ActualValue = MakeShared<FJsonValueString>(bKindDiffers ? FString(LexToString(ArtifactKind)) : bLocationDiffers ? DurableLocation : FString(LexToString(Intent)));
						OutError.ExpectedValue = MakeShared<FJsonValueString>(bKindDiffers ? FString(LexToString(ExistingTarget->GetArtifactKind())) : bLocationDiffers ? ExistingTarget->GetDurableLocation() : FString(LexToString(ExistingTarget->GetIntent())));
					}
					return false;
				}
				Artifacts.Emplace(
					ArtifactId,
					Identity,
					ArtifactKind,
					DurableLocation,
					FinalPathValue,
					Intent,
					Role);
				if (bStateBoundJournal)
				{
					FDurableArtifactState PreState;
					FDurableArtifactState ExpectedState;
					if (!TryParseArtifactState(
							PreStatePtr ? *PreStatePtr : nullptr,
							ArtifactId,
							ArtifactPath + TEXT(".pre_state"),
							PreState,
							OutError)
						|| !TryParseArtifactState(
							ExpectedStatePtr ? *ExpectedStatePtr : nullptr,
							ArtifactId,
							ArtifactPath + TEXT(".expected_persisted_state"),
							ExpectedState,
							OutError))
					{
						return false;
					}
					PreMutationStates.Add(MoveTemp(PreState));
					ExpectedPersistedStates.Add(
						MoveTemp(ExpectedState));
				}
				ArtifactIds.Add(ArtifactId);
				PreviousArtifactId = ArtifactId;
				TArray<FString> AdditionalExtensions = ExistingTarget
					? ExistingTarget->GetAdditionalPackageExtensions() : TArray<FString>();
				if (Role.StartsWith(TEXT("package_sidecar:")))
					AdditionalExtensions.AddUnique(Role.Mid(16));
				const FAssetDurableTarget RestoredTarget(Identity, ArtifactKind, DurableLocation,
					Intent, ERollbackGrade::DurableBackup, MoveTemp(AdditionalExtensions));
				if (!RestoredTarget.IsValid())
				{
					OutError = JournalFailure(TEXT("The journal declares an unsupported native package sidecar."), ArtifactPath + TEXT(".role"));
					OutError.PublicCause = MakeShared<FAssetDurableError>(RestoredTarget.GetValidationDiagnostic());
					return false;
				}
				TargetsByKey.Add(
					TargetKey,
					RestoredTarget);
			}
			if (Artifacts.IsEmpty())
			{
				OutError = JournalFailure(TEXT("The durability journal must contain at least one artifact."), TEXT("artifacts"));
				return false;
			}
			FString ExpectedInventoryHash;
			if (!TryComputeDurableInventoryHashImpl(
					Artifacts,
					bTypedJournal,
					Resources,
					ExpectedInventoryHash,
					OutError))
			{
				if (!IsResourceChargeError(OutError)) PrependDiagnosticPath(OutError, TEXT("journal"));
				return false;
			}
			if (ExpectedInventoryHash != InventoryHash)
			{
				OutError = JournalFailure(TEXT("The durability journal inventory hash does not match its artifacts."), TEXT("inventory_hash"));
				OutError.ActualEvidence = MakeShared<FJsonValueString>(InventoryHash);
				OutError.ExpectedEvidence = MakeShared<FJsonValueString>(ExpectedInventoryHash);
				return false;
			}
			TArray<FAssetDurableTarget> Targets;
			TargetsByKey.GenerateValueArray(Targets);
			Targets.Sort(
				[](const FAssetDurableTarget& Left,
					const FAssetDurableTarget& Right)
				{
					return Left.GetDurableTargetKey()
						< Right.GetDurableTargetKey();
				});
			const FAssetDurableRequest Request(
				RecoveryPlan(JournalId, Fingerprint),
				MoveTemp(Targets),
				static_cast<int64>(FencingTokenNumber),
				OwnerId,
				EAtomicityMode::MultiOwnerAtomicSupported);
			OutSession = MakeShared<const FAssetDurableSession>(
				Request,
				FDurableInventory(
					MoveTemp(Artifacts),
					InventoryHash),
				JournalId,
				FullPath(JournalPath),
				FullPath(JournalRoot),
				FullPath(TransactionRoot),
				FullPath(StagingRoot),
				MoveTemp(History),
				MoveTemp(PreMutationStates),
				MoveTemp(ExpectedPersistedStates));
			return true;
		}

		bool TryLoadSession(
			const FString& JournalPath,
			const FString& JournalRoot,
			const TArray<FString>& AllowedArtifactRoots,
			FUeAgentResourceTracker& Resources,
			FAssetDurableSessionPtr& OutSession,
			FAssetDurableError& OutError)
		{
			if (!IsWithin(
				JournalPath,
				JournalRoot / TEXT("active")))
			{
				OutError = JournalFailure(TEXT("The durability journal file is outside the active journal root."), TEXT("path"));
				OutError.InternalContext = JournalPath;
				return false;
			}
			FString PayloadText;
			const bool bLoaded = TryReadOuterJournal(
					JournalPath,
					Resources,
					PayloadText,
					OutError)
				&& TryParseSession(
					PayloadText,
					JournalPath,
					JournalRoot,
					AllowedArtifactRoots,
					Resources,
					OutSession,
					OutError);
			if (!bLoaded && OutError.InternalContext.IsEmpty()) OutError.InternalContext = JournalPath;
			return bLoaded;
		}

		bool TryEnumerateJournals(
			const FString& ActiveRoot,
			FUeAgentResourceTracker& Resources,
			TArray<FString>& OutJournalPaths,
			FAssetDurableError& OutError)
		{
			OutJournalPaths.Reset();
			if (!IFileManager::Get().DirectoryExists(*ActiveRoot))
			{
				return true;
			}
			TArray<FString> Stack;
			Stack.Add(FullPath(ActiveRoot));
			while (!Stack.IsEmpty())
			{
				const FString Current = Stack.Pop(EAllowShrinking::No);
				if (!TryCharge(
					Resources,
					TEXT("/c8d/recovery/enumerate/directory"),
					0,
					0,
					0,
					0,
					1,
					OutError))
				{
					return false;
				}
				const FString Candidate =
					Current / TEXT("journal.json");
				if (IFileManager::Get().FileExists(*Candidate))
				{
					if (!TryCharge(
						Resources,
						TEXT("/c8d/recovery/enumerate"),
						1,
						0,
						0,
						0,
						1,
						OutError))
					{
						return false;
					}
					OutJournalPaths.Add(FullPath(Candidate));
				}
				TArray<FString> Directories;
				IFileManager::Get().FindFiles(
					Directories,
					*(Current / TEXT("*")),
					false,
					true);
				Directories.Sort();
				for (int32 Index = Directories.Num() - 1;
					Index >= 0;
					--Index)
				{
					Stack.Add(Current / Directories[Index]);
				}
			}
			OutJournalPaths.Sort();
			return true;
		}

		bool TryComputeDurableInventoryHashImpl(
			const TArray<FDurableArtifact>& Artifacts,
			const bool bBindArtifactKind,
			FUeAgentResourceTracker& Resources,
			FString& OutInventoryHash,
			FAssetDurableError& OutError)
		{
			OutInventoryHash.Reset();
			OutError = FAssetDurableError();
			if (Artifacts.IsEmpty())
			{
				OutError = BackendFailure(TEXT("durable_inventory_invalid"), TEXT("A durable inventory hash requires at least one artifact."), FString());
				OutError.PropertyPath = TEXT("artifacts");
				return false;
			}
			FString Material;
			for (int32 Index = 0;
				Index < Artifacts.Num();
				++Index)
			{
				const FDurableArtifact& Artifact =
					Artifacts[Index];
				if (const FAssetDurableError* ArtifactError = Artifact.GetValidationDiagnostic())
				{
					OutError = *ArtifactError;
					PrependDiagnosticPath(OutError, FString::Printf(TEXT("artifacts[%d]"), Index));
					return false;
				}
				if (Index > 0)
				{
					Material += TEXT("\n");
				}
				Material += Artifact.GetArtifactId();
				Material += TEXT("|");
				if (bBindArtifactKind)
				{
					Material += LexToString(
						Artifact.GetArtifactKind());
					Material += TEXT("|");
				}
				Material += Artifact.GetDurableLocation();
				Material += TEXT("|");
				Material += Artifact.GetFinalPath();
				Material += TEXT("|");
				Material += LexToString(Artifact.GetIntent());
				Material += TEXT("|");
				Material += Artifact.GetRole();
			}
			const FTCHARToUTF8 Utf8(*Material);
			if (!TryCharge(
					Resources,
					TEXT("/c8d/inventory/hash"),
					0,
					Artifacts.Num(),
					Utf8.Length(),
					0,
					Artifacts.Num(),
					OutError))
			{
				return false;
			}
			OutInventoryHash = Sha256(Material);
			return true;
		}
	}









	FString LexToString(const EAssetDurableIntent Value)
	{
		switch (Value)
		{
		case EAssetDurableIntent::Save: return TEXT("save");
		case EAssetDurableIntent::Delete: return TEXT("delete");
		case EAssetDurableIntent::Native: return TEXT("native");
		default: return TEXT("unknown");
		}
	}

	FString LexToString(const EDurableArtifactKind Value)
	{
		switch (Value)
		{
		case EDurableArtifactKind::Package:
			return TEXT("package");
		case EDurableArtifactKind::ProjectConfig:
			return TEXT("project_config");
		default:
			return TEXT("unknown");
		}
	}

	FString LexToString(const ERollbackGrade Value)
	{
		switch (Value)
		{
		case ERollbackGrade::Full: return TEXT("full");
		case ERollbackGrade::DurableBackup:
			return TEXT("durable_backup");
		case ERollbackGrade::Compensatable:
			return TEXT("compensatable");
		case ERollbackGrade::None: return TEXT("none");
		default: return TEXT("unknown");
		}
	}

	FString LexToString(const EAtomicityMode Value)
	{
		switch (Value)
		{
		case EAtomicityMode::SingleOwnerAtomic:
			return TEXT("single_owner_atomic");
		case EAtomicityMode::MultiOwnerAtomicSupported:
			return TEXT("multi_owner_atomic_supported");
		case EAtomicityMode::CompensatedSaga:
			return TEXT("compensated_saga");
		case EAtomicityMode::NonAtomicRejected:
			return TEXT("non_atomic_rejected");
		default: return TEXT("unknown");
		}
	}

	FString LexToString(const EDurableStatus Value)
	{
		switch (Value)
		{
		case EDurableStatus::Prepared: return TEXT("prepared");
		case EDurableStatus::PersistedUnverified:
			return TEXT("persisted_unverified");
		case EDurableStatus::Verified: return TEXT("verified");
		case EDurableStatus::Committed: return TEXT("committed");
		case EDurableStatus::RolledBack: return TEXT("rolled_back");
		case EDurableStatus::RecoveryRequired:
			return TEXT("recovery_required");
		case EDurableStatus::StateUnknown:
			return TEXT("state_unknown");
		default: return TEXT("unknown");
		}
	}

	FString LexToString(const EDurableStatePresence Value)
	{
		switch (Value)
		{
		case EDurableStatePresence::Unknown: return TEXT("unknown");
		case EDurableStatePresence::Absent: return TEXT("absent");
		case EDurableStatePresence::Present: return TEXT("present");
		default: return TEXT("unknown");
		}
	}

	FString LexToString(const ERecoveryAction Value)
	{
		switch (Value)
		{
		case ERecoveryAction::None: return TEXT("none");
		case ERecoveryAction::AbortedBeforeMutation:
			return TEXT("aborted_before_mutation");
		case ERecoveryAction::RestoredOriginal:
			return TEXT("restored_original");
		case ERecoveryAction::VerifyPersisted:
			return TEXT("verify_persisted");
		case ERecoveryAction::ManualIntervention:
			return TEXT("manual_intervention");
		default: return TEXT("unknown");
		}
	}

	FAssetDurableTarget::FAssetDurableTarget(
		FUeAgentCanonicalIdentity InIdentity,
		FString InPackageName,
		const EPackageHeaderKind InHeaderKind,
		const EAssetDurableIntent InIntent,
		const ERollbackGrade InRollbackGrade,
		TArray<FString> InAdditionalPackageExtensions)
		: FAssetDurableTarget(
			MoveTemp(InIdentity),
			EDurableArtifactKind::Package,
			MoveTemp(InPackageName),
			InIntent,
			InRollbackGrade, MoveTemp(InAdditionalPackageExtensions))
	{
		PackageHeaderKind = InHeaderKind;
	}

	FAssetDurableTarget::FAssetDurableTarget(
		FUeAgentCanonicalIdentity InIdentity,
		const EDurableArtifactKind InArtifactKind,
		FString InDurableLocation,
		const EAssetDurableIntent InIntent,
		const ERollbackGrade InRollbackGrade,
		TArray<FString> InAdditionalPackageExtensions)
		: Identity(MoveTemp(InIdentity))
		, ArtifactKind(InArtifactKind)
		, DurableLocation(
			NormalizedText(MoveTemp(InDurableLocation)))
		, PackageName(
			ArtifactKind == EDurableArtifactKind::Package
				? DurableLocation
				: FString())
		, Intent(InIntent)
		, RollbackGrade(InRollbackGrade)
	{
		if (!Identity.IsValid())
		{
			ValidationDiagnostic = MakeShared<FAssetDurableError>(Identity.GetValidationDiagnostic());
		}
		else if (DurableLocation.IsEmpty())
		{
			FAssetDurableError Error = FAssetDurableError::Failure(TEXT("durable_target_location_missing"),
				TEXT("The durable target requires a non-empty storage location."), TEXT("durable"));
			Error.SourceKind = TEXT("native_result");
			Error.PropertyPath = TEXT("durable_location");
			ValidationDiagnostic = MakeShared<FAssetDurableError>(MoveTemp(Error));
		}
		for (FString Extension : InAdditionalPackageExtensions)
		{
			Extension.TrimStartAndEndInline();
			Extension.ToLowerInline();
			if (ArtifactKind != EDurableArtifactKind::Package || Intent != EAssetDurableIntent::Native
				|| (Extension != TEXT(".uexp") && Extension != TEXT(".ubulk")
					&& Extension != TEXT(".uptnl") && Extension != TEXT(".m.ubulk")))
			{
				FAssetDurableError Error = FAssetDurableError::Failure(TEXT("durable_native_sidecar_invalid"),
					TEXT("Explicit package sidecars require native package intent and a supported UE package extension (.uexp, .ubulk, .uptnl or .m.ubulk)."), TEXT("durable"));
				Error.PropertyPath = TEXT("additional_package_extensions");
				Error.ActualValue = MakeShared<FJsonValueString>(Extension);
				ValidationDiagnostic = MakeShared<FAssetDurableError>(MoveTemp(Error));
				break;
			}
			AdditionalPackageExtensions.AddUnique(MoveTemp(Extension));
		}
		AdditionalPackageExtensions.Sort();
	}

	const FUeAgentCanonicalIdentity&
	FAssetDurableTarget::GetIdentity() const
	{
		return Identity;
	}

	EDurableArtifactKind
	FAssetDurableTarget::GetArtifactKind() const
	{
		return ArtifactKind;
	}

	const FString&
	FAssetDurableTarget::GetDurableLocation() const
	{
		return DurableLocation;
	}

	const FString& FAssetDurableTarget::GetPackageName() const
	{
		return PackageName;
	}

	EPackageHeaderKind FAssetDurableTarget::GetPackageHeaderKind() const
	{
		return PackageHeaderKind;
	}

	const TArray<FString>& FAssetDurableTarget::GetAdditionalPackageExtensions() const
	{
		return AdditionalPackageExtensions;
	}

	FString FAssetDurableTarget::GetDurableTargetKey() const
	{
		return LexToString(ArtifactKind)
			+ TEXT(":")
			+ DurableLocation.ToLower();
	}

	EAssetDurableIntent FAssetDurableTarget::GetIntent() const
	{
		return Intent;
	}

	ERollbackGrade FAssetDurableTarget::GetRollbackGrade() const
	{
		return RollbackGrade;
	}

	bool FAssetDurableTarget::IsValid() const
	{
		return !ValidationDiagnostic.IsValid();
	}

	const FAssetDurableError& FAssetDurableTarget::GetValidationDiagnostic() const
	{
		static const FAssetDurableError Empty;
		return ValidationDiagnostic.IsValid() ? *ValidationDiagnostic : Empty;
	}

	FAssetDurableRequest::FAssetDurableRequest(
		FPlanningPreflightPlanRef InPlan,
		TArray<FAssetDurableTarget> InTargets,
		const int64 InFencingToken,
		FString InJournalOwnerId,
		const EAtomicityMode InAtomicityMode)
		: Plan(MoveTemp(InPlan))
		, Targets(MoveTemp(InTargets))
		, FencingToken(InFencingToken)
		, JournalOwnerId(
			NormalizedText(MoveTemp(InJournalOwnerId)))
		, AtomicityMode(InAtomicityMode)
	{
		FAssetDurableError Error;
		if (Targets.IsEmpty())
		{
			Error = FAssetDurableError::Failure(TEXT("durable_request_invalid"), TEXT("The durability request has no targets."), TEXT("asset_durable"));
			Error.PropertyPath = TEXT("targets");
		}
		else if (FencingToken < 0)
		{
			Error = FAssetDurableError::Failure(TEXT("durable_request_invalid"), TEXT("The durability request fencing token must be non-negative."), TEXT("asset_durable"));
			Error.PropertyPath = TEXT("fencing_token");
			Error.ActualEvidence = MakeShared<FJsonValueNumber>(FencingToken);
		}
		else if (JournalOwnerId.IsEmpty())
		{
			Error = FAssetDurableError::Failure(TEXT("durable_request_invalid"), TEXT("The durability request has no journal owner."), TEXT("asset_durable"));
			Error.PropertyPath = TEXT("journal_owner_id");
		}
		else
		{
			for (int32 Index = 0; Index < Targets.Num(); ++Index)
			{
				if (!Targets[Index].IsValid())
				{
					Error = Targets[Index].GetValidationDiagnostic();
					AddTargetIndex(Error, TEXT("targets"), Index);
					break;
				}
			}
		}
		if (!Error.IsEmpty())
		{
			if (Error.SourceKind.IsEmpty()) Error.SourceKind = TEXT("native_result");
			ValidationDiagnostic = MakeShared<FAssetDurableError>(MoveTemp(Error));
		}
	}

	const FPlanningPreflightPlanRef&
	FAssetDurableRequest::GetPlan() const
	{
		return Plan;
	}

	const TArray<FAssetDurableTarget>&
	FAssetDurableRequest::GetTargets() const
	{
		return Targets;
	}

	int64 FAssetDurableRequest::GetFencingToken() const
	{
		return FencingToken;
	}

	const FString& FAssetDurableRequest::GetJournalOwnerId() const
	{
		return JournalOwnerId;
	}

	EAtomicityMode FAssetDurableRequest::GetAtomicityMode() const
	{
		return AtomicityMode;
	}

	bool FAssetDurableRequest::IsValid() const
	{
		return !ValidationDiagnostic.IsValid();
	}

	const FAssetDurableError& FAssetDurableRequest::GetValidationDiagnostic() const
	{
		static const FAssetDurableError Empty;
		return ValidationDiagnostic.IsValid() ? *ValidationDiagnostic : Empty;
	}

	FDurableArtifact::FDurableArtifact(
		FString InArtifactId,
		FUeAgentCanonicalIdentity InTargetIdentity,
		FString InPackageName,
		FString InFinalPath,
		const EAssetDurableIntent InIntent,
		FString InRole)
		: FDurableArtifact(
			MoveTemp(InArtifactId),
			MoveTemp(InTargetIdentity),
			EDurableArtifactKind::Package,
			MoveTemp(InPackageName),
			MoveTemp(InFinalPath),
			InIntent,
			MoveTemp(InRole))
	{
	}

	FDurableArtifact::FDurableArtifact(
		FString InArtifactId,
		FUeAgentCanonicalIdentity InTargetIdentity,
		const EDurableArtifactKind InArtifactKind,
		FString InDurableLocation,
		FString InFinalPath,
		const EAssetDurableIntent InIntent,
		FString InRole)
		: ArtifactId(NormalizedText(MoveTemp(InArtifactId)))
		, TargetIdentity(MoveTemp(InTargetIdentity))
		, ArtifactKind(InArtifactKind)
		, DurableLocation(
			NormalizedText(MoveTemp(InDurableLocation)))
		, PackageName(
			ArtifactKind == EDurableArtifactKind::Package
				? DurableLocation
				: FString())
		, FinalPath(FullPath(InFinalPath))
		, Intent(InIntent)
		, Role(NormalizedText(MoveTemp(InRole)))
	{
		FAssetDurableError Error;
		if (ArtifactId.IsEmpty())
		{
			Error = BackendFailure(TEXT("durable_inventory_invalid"), TEXT("A durable artifact requires a non-empty artifact identifier."), DurableLocation);
			Error.PropertyPath = TEXT("artifact_id");
		}
		else if (!TargetIdentity.IsValid())
		{
			Error = TargetIdentity.GetValidationDiagnostic();
		}
		else if (DurableLocation.IsEmpty())
		{
			Error = BackendFailure(TEXT("durable_inventory_invalid"), TEXT("A durable artifact requires a durable location."), DurableLocation);
			Error.PropertyPath = TEXT("durable_location");
		}
		else if (FinalPath.IsEmpty())
		{
			Error = BackendFailure(TEXT("durable_inventory_invalid"), TEXT("A durable artifact requires a final file path."), DurableLocation);
			Error.PropertyPath = TEXT("final_path");
		}
		else if (Role.IsEmpty())
		{
			Error = BackendFailure(TEXT("durable_inventory_invalid"), TEXT("A durable artifact requires a non-empty role."), DurableLocation);
			Error.PropertyPath = TEXT("role");
		}
		if (!Error.IsEmpty())
		{
			ValidationDiagnostic = MakeShared<const FAssetDurableError>(MoveTemp(Error));
		}
	}

	const FString& FDurableArtifact::GetArtifactId() const
	{
		return ArtifactId;
	}

	const FUeAgentCanonicalIdentity&
	FDurableArtifact::GetTargetIdentity() const
	{
		return TargetIdentity;
	}

	EDurableArtifactKind
	FDurableArtifact::GetArtifactKind() const
	{
		return ArtifactKind;
	}

	const FString& FDurableArtifact::GetDurableLocation() const
	{
		return DurableLocation;
	}

	const FString& FDurableArtifact::GetPackageName() const
	{
		return PackageName;
	}

	const FString& FDurableArtifact::GetFinalPath() const
	{
		return FinalPath;
	}

	EAssetDurableIntent FDurableArtifact::GetIntent() const
	{
		return Intent;
	}

	const FString& FDurableArtifact::GetRole() const
	{
		return Role;
	}

	bool FDurableArtifact::IsValid() const
	{
		return !ValidationDiagnostic.IsValid();
	}

	const FAssetDurableError* FDurableArtifact::GetValidationDiagnostic() const
	{
		return ValidationDiagnostic.Get();
	}

	FDurableArtifactState::FDurableArtifactState()
		: FDurableArtifactState(FString(), EDurableStatePresence::Unknown, FString())
	{
	}

	FDurableArtifactState::FDurableArtifactState(
		FString InArtifactId,
		const EDurableStatePresence InPresence,
		FString InContentHash)
		: ArtifactId(NormalizedText(MoveTemp(InArtifactId)))
		, Presence(InPresence)
		, ContentHash(NormalizedText(MoveTemp(InContentHash)))
	{
		FAssetDurableError Error;
		if (ArtifactId.IsEmpty())
		{
			Error = BackendFailure(TEXT("durable_journal_integrity_invalid"), TEXT("A durable artifact state requires an artifact identifier."), FString());
			Error.PropertyPath = TEXT("artifact_id");
		}
		else if (Presence == EDurableStatePresence::Present && !ContentHash.StartsWith(TEXT("sha256:")))
		{
			Error = BackendFailure(TEXT("durable_journal_integrity_invalid"), TEXT("A present durable artifact state requires a content hash beginning with sha256:."), FString());
			Error.PropertyPath = TEXT("content_hash");
			Error.ActualEvidence = MakeShared<FJsonValueString>(ContentHash);
		}
		else if (Presence != EDurableStatePresence::Present && !ContentHash.IsEmpty())
		{
			Error = BackendFailure(TEXT("durable_journal_integrity_invalid"), TEXT("A durable artifact state that is not present must have an empty content hash."), FString());
			Error.PropertyPath = TEXT("content_hash");
			Error.ActualEvidence = MakeShared<FJsonValueString>(ContentHash);
		}
		if (!Error.IsEmpty())
		{
			ValidationDiagnostic = MakeShared<const FAssetDurableError>(MoveTemp(Error));
		}
	}

	const FString& FDurableArtifactState::GetArtifactId() const
	{
		return ArtifactId;
	}

	EDurableStatePresence FDurableArtifactState::GetPresence() const
	{
		return Presence;
	}

	const FString& FDurableArtifactState::GetContentHash() const
	{
		return ContentHash;
	}

	bool FDurableArtifactState::IsKnown() const
	{
		return Presence != EDurableStatePresence::Unknown;
	}

	bool FDurableArtifactState::IsValid() const
	{
		return !ValidationDiagnostic.IsValid();
	}

	const FAssetDurableError* FDurableArtifactState::GetValidationDiagnostic() const
	{
		return ValidationDiagnostic.Get();
	}

	bool FDurableArtifactState::Matches(
		const FDurableArtifactState& Other) const
	{
		return IsKnown()
			&& Other.IsKnown()
			&& ArtifactId == Other.ArtifactId
			&& Presence == Other.Presence
			&& (Presence != EDurableStatePresence::Present
				|| ContentHash == Other.ContentHash);
	}

	FDurableInventory::FDurableInventory(
		TArray<FDurableArtifact> InArtifacts,
		FString InInventoryHash)
		: Artifacts(MoveTemp(InArtifacts))
		, InventoryHash(
			NormalizedText(MoveTemp(InInventoryHash)))
	{
		FAssetDurableError Error;
		if (Artifacts.IsEmpty())
		{
			Error = BackendFailure(TEXT("durable_inventory_invalid"), TEXT("The durable inventory must contain at least one artifact."), FString());
			Error.PropertyPath = TEXT("artifacts");
			Error.ActualValue = MakeShared<FJsonValueNumber>(0);
		}
		else if (!InventoryHash.StartsWith(TEXT("sha256:")))
		{
			Error = BackendFailure(TEXT("durable_inventory_invalid"), TEXT("The durable inventory hash must begin with sha256:."), FString());
			Error.PropertyPath = TEXT("inventory_hash");
			Error.ActualEvidence = MakeShared<FJsonValueString>(InventoryHash);
		}
		else
		{
			for (int32 Index = 0; Index < Artifacts.Num(); ++Index)
			{
				if (const FAssetDurableError* ArtifactError = Artifacts[Index].GetValidationDiagnostic())
				{
					Error = *ArtifactError;
					Error.PropertyPath = FString::Printf(TEXT("artifacts[%d]"), Index)
						+ (Error.PropertyPath.IsEmpty() ? FString() : TEXT(".") + Error.PropertyPath);
					break;
				}
			}
		}
		if (!Error.IsEmpty())
		{
			ValidationDiagnostic = MakeShared<const FAssetDurableError>(MoveTemp(Error));
		}
	}

	const TArray<FDurableArtifact>&
	FDurableInventory::GetArtifacts() const
	{
		return Artifacts;
	}

	const FString& FDurableInventory::GetInventoryHash() const
	{
		return InventoryHash;
	}

	bool FDurableInventory::IsValid() const
	{
		return !ValidationDiagnostic.IsValid();
	}

	const FAssetDurableError* FDurableInventory::GetValidationDiagnostic() const
	{
		return ValidationDiagnostic.Get();
	}

	FAssetDurableSession::FAssetDurableSession(
		FAssetDurableRequest InRequest,
		FDurableInventory InInventory,
		FString InJournalId,
		FString InJournalPath,
		FString InJournalRoot,
		FString InTransactionRoot,
		FString InStagingRoot,
		TArray<FString> InPhaseHistory,
		TArray<FDurableArtifactState> InPreMutationStates,
		TArray<FDurableArtifactState> InExpectedPersistedStates)
		: Request(MoveTemp(InRequest))
		, Inventory(MoveTemp(InInventory))
		, JournalId(NormalizedText(MoveTemp(InJournalId)))
		, JournalPath(FullPath(InJournalPath))
		, JournalRoot(FullPath(InJournalRoot))
		, TransactionRoot(FullPath(InTransactionRoot))
		, StagingRoot(FullPath(InStagingRoot))
		, PhaseHistory(MoveTemp(InPhaseHistory))
		, PreMutationStates(MoveTemp(InPreMutationStates))
		, ExpectedPersistedStates(MoveTemp(InExpectedPersistedStates))
	{
	}

	const FAssetDurableRequest&
	FAssetDurableSession::GetRequest() const
	{
		return Request;
	}

	const FDurableInventory&
	FAssetDurableSession::GetInventory() const
	{
		return Inventory;
	}

	const FString& FAssetDurableSession::GetJournalId() const
	{
		return JournalId;
	}

	const FString& FAssetDurableSession::GetJournalPath() const
	{
		return JournalPath;
	}

	const FString& FAssetDurableSession::GetJournalRoot() const
	{
		return JournalRoot;
	}

	const FString& FAssetDurableSession::GetTransactionRoot() const
	{
		return TransactionRoot;
	}

	const FString& FAssetDurableSession::GetStagingRoot() const
	{
		return StagingRoot;
	}

	const TArray<FString>&
	FAssetDurableSession::GetPhaseHistory() const
	{
		return PhaseHistory;
	}

	const FString& FAssetDurableSession::GetCurrentPhase() const
	{
		check(!PhaseHistory.IsEmpty());
		return PhaseHistory.Last();
	}

	const TArray<FDurableArtifactState>&
	FAssetDurableSession::GetPreMutationStates() const
	{
		return PreMutationStates;
	}

	const TArray<FDurableArtifactState>&
	FAssetDurableSession::GetExpectedPersistedStates() const
	{
		return ExpectedPersistedStates;
	}

	FDurableReceipt::FDurableReceipt(
		FString InJournalId,
		const EDurableStatus InStatus,
		FString InPlanFingerprint,
		FString InInventoryHash,
		TArray<FString> InPhaseHistory,
		const ERecoveryAction InRecoveryAction,
		const bool bInRequiresIndependentVerification,
		const bool bInReplayed,
		FAssetDurableError InFailure,
		FString InVerificationId,
		TArray<FDurableArtifactState> InPreMutationStates,
		TArray<FDurableArtifactState> InExpectedPersistedStates)
		: JournalId(NormalizedText(MoveTemp(InJournalId)))
		, Status(InStatus)
		, PlanFingerprint(
			NormalizedText(MoveTemp(InPlanFingerprint)))
		, InventoryHash(
			NormalizedText(MoveTemp(InInventoryHash)))
		, PhaseHistory(MoveTemp(InPhaseHistory))
		, RecoveryAction(InRecoveryAction)
		, bRequiresIndependentVerification(
			bInRequiresIndependentVerification)
		, bReplayed(bInReplayed)
		, Failure(MoveTemp(InFailure))
		, VerificationId(NormalizedText(MoveTemp(InVerificationId)))
		, PreMutationStates(MoveTemp(InPreMutationStates))
		, ExpectedPersistedStates(MoveTemp(InExpectedPersistedStates))
	{
		if (!Failure.IsEmpty())
		{
			Failure.TransactionStatus = LexToString(Status);
			Failure.RecoveryAction = LexToString(RecoveryAction);
		}
	}

	const FString& FDurableReceipt::GetJournalId() const
	{
		return JournalId;
	}

	EDurableStatus FDurableReceipt::GetStatus() const
	{
		return Status;
	}

	const FString& FDurableReceipt::GetPlanFingerprint() const
	{
		return PlanFingerprint;
	}

	const FString& FDurableReceipt::GetInventoryHash() const
	{
		return InventoryHash;
	}

	const TArray<FString>& FDurableReceipt::GetPhaseHistory() const
	{
		return PhaseHistory;
	}

	ERecoveryAction FDurableReceipt::GetRecoveryAction() const
	{
		return RecoveryAction;
	}

	bool FDurableReceipt::RequiresIndependentVerification() const
	{
		return bRequiresIndependentVerification;
	}

	bool FDurableReceipt::IsReplayed() const
	{
		return bReplayed;
	}

	const FString& FDurableReceipt::GetFailureCode() const
	{
		return Failure.Code;
	}

	const FAssetDurableError& FDurableReceipt::GetFailureDiagnostic() const
	{
		return Failure;
	}

	const FString& FDurableReceipt::GetVerificationId() const
	{
		return VerificationId;
	}

	const TArray<FDurableArtifactState>&
	FDurableReceipt::GetPreMutationStates() const
	{
		return PreMutationStates;
	}

	const TArray<FDurableArtifactState>&
	FDurableReceipt::GetExpectedPersistedStates() const
	{
		return ExpectedPersistedStates;
	}

	FDurableVerificationAcknowledgement::FDurableVerificationAcknowledgement()
		: FDurableVerificationAcknowledgement(FString(), FString(), FString(), FString(), FString(), -1)
	{
	}

	FDurableVerificationAcknowledgement::
	FDurableVerificationAcknowledgement(
		FString InJournalId,
		FString InPlanFingerprint,
		FString InInventoryHash,
		FString InVerificationId,
		FString InJournalOwnerId,
		const int64 InFencingToken,
		bool bInSemanticVerified)
		: JournalId(NormalizedText(MoveTemp(InJournalId)))
		, PlanFingerprint(
			NormalizedText(MoveTemp(InPlanFingerprint)))
		, InventoryHash(
			NormalizedText(MoveTemp(InInventoryHash)))
		, VerificationId(
			NormalizedText(MoveTemp(InVerificationId)))
		, JournalOwnerId(
			NormalizedText(MoveTemp(InJournalOwnerId)))
		, FencingToken(InFencingToken)
		, bSemanticVerified(bInSemanticVerified)
	{
		FAssetDurableError Error;
		const auto Invalid = [&](const TCHAR* Field, const TCHAR* Reason, const FString& Evidence)
		{
			Error = BackendFailure(TEXT("durable_acknowledgement_invalid"), Reason, FString());
			Error.PropertyPath = Field;
			Error.ActualEvidence = MakeShared<FJsonValueString>(Evidence);
		};
		if (JournalId.IsEmpty()) Invalid(TEXT("journal_id"), TEXT("A durability acknowledgement requires a journal identifier."), JournalId);
		else if (!PlanFingerprint.StartsWith(TEXT("sha256:"))) Invalid(TEXT("plan_fingerprint"), TEXT("The acknowledged plan fingerprint must begin with sha256:."), PlanFingerprint);
		else if (!InventoryHash.StartsWith(TEXT("sha256:"))) Invalid(TEXT("inventory_hash"), TEXT("The acknowledged inventory hash must begin with sha256:."), InventoryHash);
		else if (VerificationId.IsEmpty()) Invalid(TEXT("verification_id"), TEXT("A durability acknowledgement requires a verification identifier."), VerificationId);
		else if (JournalOwnerId.IsEmpty()) Invalid(TEXT("journal_owner_id"), TEXT("A durability acknowledgement requires a journal owner."), JournalOwnerId);
		else if (FencingToken < 0) Invalid(TEXT("fencing_token"), TEXT("The acknowledged fencing token must not be negative."), FString::Printf(TEXT("%lld"), FencingToken));
		if (!Error.IsEmpty()) ValidationDiagnostic = MakeShared<const FAssetDurableError>(MoveTemp(Error));
	}

	const FString&
	FDurableVerificationAcknowledgement::GetJournalId() const
	{
		return JournalId;
	}

	const FString&
	FDurableVerificationAcknowledgement::GetPlanFingerprint() const
	{
		return PlanFingerprint;
	}

	const FString&
	FDurableVerificationAcknowledgement::GetInventoryHash() const
	{
		return InventoryHash;
	}

	const FString&
	FDurableVerificationAcknowledgement::GetVerificationId() const
	{
		return VerificationId;
	}

	const FString&
	FDurableVerificationAcknowledgement::GetJournalOwnerId() const
	{
		return JournalOwnerId;
	}

	int64
	FDurableVerificationAcknowledgement::GetFencingToken() const
	{
		return FencingToken;
	}

	bool FDurableVerificationAcknowledgement::IsValid() const
	{
		return !ValidationDiagnostic.IsValid();
	}

	const FAssetDurableError* FDurableVerificationAcknowledgement::GetValidationDiagnostic() const
	{
		return ValidationDiagnostic.Get();
	}

	FPackageAssetDurableBackend::FPackageAssetDurableBackend(
		FString InAllowedContentRoot)
		: FPackageAssetDurableBackend(MoveTemp(InAllowedContentRoot), {})
	{
	}

	FPackageAssetDurableBackend::FPackageAssetDurableBackend(
		FString InAllowedContentRoot, TArray<FString> InAdditionalContentRoots)
		: AllowedContentRoot(
			FullPath(
				InAllowedContentRoot.TrimStartAndEnd().IsEmpty()
					? FPaths::ProjectContentDir()
					: InAllowedContentRoot))
	{
		for (const FString& Root : InAdditionalContentRoots)
		{
			if (!Root.TrimStartAndEnd().IsEmpty()) AdditionalContentRoots.AddUnique(FullPath(Root));
		}
	}

	TArray<FString>
	FPackageAssetDurableBackend::GetAllowedArtifactRoots() const
	{
		TArray<FString> Roots = {AllowedContentRoot};
		Roots.Append(AdditionalContentRoots);
		return Roots;
	}

	bool FPackageAssetDurableBackend::SupportsAtomicity(
		const EAtomicityMode Mode) const
	{
		return Mode == EAtomicityMode::CompensatedSaga;
	}

	bool FPackageAssetDurableBackend::BuildInventory(
		const FAssetDurableRequest& Request,
		FUeAgentResourceTracker& Resources,
		FDurableInventory& OutInventory,
		FAssetDurableError& OutError)
	{
		OutInventory = FDurableInventory();
		OutError = FAssetDurableError();
		TArray<FDurableArtifact> Artifacts;
		TSet<FString> PackagePaths;
		for (const FAssetDurableTarget& Target :
			Request.GetTargets())
		{
			if (Target.GetArtifactKind()
				!= EDurableArtifactKind::Package)
			{
				OutError = BackendFailure(TEXT("durable_artifact_kind_unsupported"), TEXT("The package durability backend requires a package target."), Target.GetDurableLocation());
				OutError.PropertyPath = TEXT("artifact_kind");
				OutError.ActualValue = MakeShared<FJsonValueString>(LexToString(Target.GetArtifactKind()));
				OutError.ExpectedValue = MakeShared<FJsonValueString>(LexToString(EDurableArtifactKind::Package));
				return false;
			}
			const FString PackageName =
				Target.GetPackageName().TrimStartAndEnd();
			FText PackageNameReason;
			const bool bValidPackageName = FPackageName::IsValidLongPackageName(PackageName, false, &PackageNameReason);
			if (!bValidPackageName
				|| FPackageName::GetPackageMountPoint(PackageName)
					.IsNone())
			{
				OutError = BackendFailure(TEXT("durable_package_name_invalid"), bValidPackageName
					? TEXT("The package name has no registered mount point.")
					: FString::Printf(TEXT("The package name is invalid: %s"), *PackageNameReason.ToString()), PackageName);
				OutError.PropertyPath = TEXT("package_name");
				return false;
			}
			FString HeaderPath;
			UE::UAI::Contract::FDiagnostic HeaderError;
			if (!TryResolvePackageHeaderFilename(
					PackageName,
					Target.GetPackageHeaderKind(),
					HeaderPath,
					HeaderError))
			{
				OutError = HeaderError.IsEmpty() ? BackendFailure(TEXT("durable_package_path_unavailable"), TEXT("Package header path resolution failed without reporting a diagnostic."), PackageName) : HeaderError;
				return false;
			}
			HeaderPath = FullPath(HeaderPath);
			const FString FoldedPath = HeaderPath.ToLower();
			if (PackagePaths.Contains(FoldedPath))
			{
				OutError = BackendFailure(TEXT("durable_package_duplicate"), TEXT("More than one package target resolves to the same header file."), PackageName);
				OutError.ActualEvidence = MakeShared<FJsonValueString>(HeaderPath);
				return false;
			}
			PackagePaths.Add(FoldedPath);
			const FString PayloadPath =
				FPaths::ChangeExtension(
					HeaderPath,
					TEXT("upayload"));
			Artifacts.Emplace(
				DurableArtifactId(Target, TEXT("package_header")),
				Target.GetIdentity(),
				PackageName,
				HeaderPath,
				Target.GetIntent(),
				TEXT("package_header"));
			Artifacts.Emplace(
				DurableArtifactId(Target, TEXT("package_payload")),
				Target.GetIdentity(),
				PackageName,
				PayloadPath,
				Target.GetIntent(),
				TEXT("package_payload"));
			for (const FString& Extension : Target.GetAdditionalPackageExtensions())
			{
				const FString Role = TEXT("package_sidecar:") + Extension;
				Artifacts.Emplace(DurableArtifactId(Target, Role), Target.GetIdentity(), PackageName,
					FPaths::ChangeExtension(HeaderPath, Extension.Mid(1)), Target.GetIntent(), Role);
			}
		}
		Artifacts.Sort(
			[](const FDurableArtifact& Left,
				const FDurableArtifact& Right)
			{
				return Left.GetArtifactId()
					< Right.GetArtifactId();
			});
		FString InventoryHash;
		if (!TryComputeDurableInventoryHash(
			Artifacts,
			Resources,
			InventoryHash,
			OutError))
		{
			return false;
		}
		OutInventory = FDurableInventory(
			MoveTemp(Artifacts),
			MoveTemp(InventoryHash));
		return true;
	}

	bool FPackageAssetDurableBackend::CaptureBackups(
		const FAssetDurableSessionRef& Session,
		FUeAgentResourceTracker& Resources,
		FAssetDurableError& OutError)
	{
		OutError = FAssetDurableError();
		const FString BackupRoot =
			Session->GetTransactionRoot() / TEXT("backups");
		if (!IFileManager::Get().MakeDirectory(
			*BackupRoot,
			true))
		{
			OutError = MakeFileOperationFailure(TEXT("durable_backup_root_unavailable"), TEXT("Creating the package backup directory failed."), TEXT("asset_durable"), BackupRoot);
			return false;
		}
		for (const FDurableArtifact& Artifact :
			Session->GetInventory().GetArtifacts())
		{
			if (!TryCharge(
				Resources,
				TEXT("/c8d/package/backup"),
				1,
				1,
				0,
				0,
				1,
				OutError))
			{
				return false;
			}
			const FString DataPath =
				BackupDataPath(Session, Artifact);
			const FString MarkerPath =
				BackupMarkerPath(Session, Artifact);
			const bool bExists = IFileManager::Get().FileExists(
				*Artifact.GetFinalPath());
			if (bExists
					&& IFileManager::Get().Copy(
						*DataPath,
						*Artifact.GetFinalPath(),
						true,
						true)
						!= COPY_OK)
			{
				OutError = MakeFileOperationFailure(TEXT("durable_backup_failed"), TEXT("Copying the original package file into its backup failed."), TEXT("asset_durable"), DataPath);
				OutError.TargetPath = Artifact.GetDurableLocation();
				return false;
			}
			if (!FFileHelper::SaveStringToFile(
					bExists ? TEXT("present") : TEXT("absent"),
					*MarkerPath,
					FFileHelper::EEncodingOptions::
						ForceUTF8WithoutBOM))
			{
				OutError = MakeFileOperationFailure(TEXT("durable_backup_failed"), TEXT("Writing the package backup presence marker failed."), TEXT("asset_durable"), MarkerPath);
				OutError.TargetPath = Artifact.GetDurableLocation();
				return false;
			}
		}
		return true;
	}

	bool FPackageAssetDurableBackend::Stage(
		const FAssetDurableSessionRef& Session,
		FUeAgentResourceTracker& Resources,
		FAssetDurableError& OutError)
	{
		OutError = FAssetDurableError();
		if (!IFileManager::Get().MakeDirectory(
			*Session->GetStagingRoot(),
			true))
		{
			OutError = MakeFileOperationFailure(TEXT("durable_stage_root_unavailable"), TEXT("Creating the package staging directory failed."), TEXT("asset_durable"), Session->GetStagingRoot());
			return false;
		}
		for (const FAssetDurableTarget& Target :
			Session->GetRequest().GetTargets())
		{
			if (Target.GetIntent() == EAssetDurableIntent::Native)
			{
				OutError = BackendFailure(TEXT("durable_native_operation_required"), TEXT("A target with native persistence intent must use the native operation path instead of package staging."), Target.GetPackageName());
				OutError.PropertyPath = TEXT("intent");
				OutError.ActualValue = MakeShared<FJsonValueString>(TEXT("native"));
				return false;
			}
			UPackage* const Package =
				FindPackage(nullptr, *Target.GetPackageName());
			if (Target.GetIntent()
				== EAssetDurableIntent::Delete)
			{
				continue;
			}
			if (!Package || Package == GetTransientPackage())
			{
				OutError = BackendFailure(TEXT("durable_package_not_loaded"), !Package
					? TEXT("The package must be loaded before it can be staged.")
					: TEXT("The transient package cannot be staged for persistence."), Target.GetPackageName());
				return false;
			}
			UObject* TopLevelAsset =
				Package->FindAssetInPackage();
			const FDurableArtifact* Header =
				FindArtifact(
					Session,
					Target,
					TEXT("package_header"));
			const FDurableArtifact* Payload =
				FindArtifact(
					Session,
					Target,
					TEXT("package_payload"));
			if (!Header || !Payload)
			{
				OutError = BackendFailure(TEXT("durable_inventory_invalid"), !Header && !Payload
					? TEXT("The durability inventory contains neither the package header nor its payload entry.")
					: !Header ? TEXT("The durability inventory has no package header entry.")
					: TEXT("The durability inventory has no package payload entry."), Target.GetPackageName());
				return false;
			}
			FString PreparedHeaderPath;
			UE::UAI::Contract::FDiagnostic HeaderError;
			if (!TryPrepareLoadedPackageHeaderForSave(
					*Package,
					TopLevelAsset,
					PreparedHeaderPath,
					HeaderError))
			{
				OutError = HeaderError.IsEmpty() ? BackendFailure(TEXT("durable_package_header_prepare_failed"), TEXT("Preparing the package header for save failed without reporting a diagnostic."), Target.GetPackageName()) : HeaderError;
				return false;
			}
			if (!FullPath(PreparedHeaderPath).Equals(
					FullPath(Header->GetFinalPath()),
					ESearchCase::IgnoreCase))
			{
				OutError = BackendFailure(TEXT("durable_package_inventory_header_mismatch"), TEXT("The prepared package header path differs from the durability inventory."), Target.GetPackageName());
				OutError.PropertyPath = TEXT("package_header");
				OutError.ActualEvidence = MakeShared<FJsonValueString>(FullPath(PreparedHeaderPath));
				OutError.ExpectedEvidence = MakeShared<FJsonValueString>(FullPath(Header->GetFinalPath()));
				return false;
			}
			const FString ScratchHeader =
				Session->GetStagingRoot()
				/ (Sha256(Target.GetPackageName()).Mid(7)
					+ FPaths::GetExtension(
						Header->GetFinalPath(),
						true));
			const FString ScratchPayload =
				FPaths::ChangeExtension(
					ScratchHeader,
					TEXT("upayload"));
			const FString HeaderStage =
				StagedArtifactPath(Session, *Header);
			const FString PayloadStage =
				StagedArtifactPath(Session, *Payload);
			IFileManager::Get().Delete(*ScratchHeader);
			IFileManager::Get().Delete(*ScratchPayload);
			IFileManager::Get().Delete(*HeaderStage);
			IFileManager::Get().Delete(*PayloadStage);
			if (!TryCharge(
				Resources,
				TEXT("/c8d/package/stage"),
				2,
				2,
				0,
				0,
				2,
				OutError))
			{
				return false;
			}
			FSavePackageArgs SaveArgs;
			SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
			SaveArgs.SaveFlags =
				SAVE_KeepDirty | SAVE_FromAutosave;
			UE::UAI::Contract::FDiagnostic SaveErrorDetail;
			if (!TrySaveLoadedPackage(
				*Package,
				TopLevelAsset,
				ScratchHeader,
				SaveArgs, SaveErrorDetail))
			{
				IFileManager::Get().Delete(*ScratchHeader);
				IFileManager::Get().Delete(*ScratchPayload);
				OutError = SaveErrorDetail;
				return false;
			}
			if (!IFileManager::Get().Move(
					*HeaderStage,
					*ScratchHeader,
					true,
					true,
					false,
					true))
			{
				OutError = MakeFileOperationFailure(TEXT("durable_package_stage_move_failed"), TEXT("Moving the saved package header into staging failed."), TEXT("asset_durable"), HeaderStage);
				OutError.TargetPath = Target.GetPackageName();
				IFileManager::Get().Delete(*ScratchHeader);
				IFileManager::Get().Delete(*ScratchPayload);
				return false;
			}
			if (IFileManager::Get().FileExists(*ScratchPayload)
				&& !IFileManager::Get().Move(
					*PayloadStage,
					*ScratchPayload,
					true,
					true,
					false,
					true))
			{
				OutError = MakeFileOperationFailure(TEXT("durable_package_stage_move_failed"), TEXT("Moving the saved package payload into staging failed."), TEXT("asset_durable"), PayloadStage);
				OutError.TargetPath = Target.GetPackageName();
				IFileManager::Get().Delete(*HeaderStage);
				IFileManager::Get().Delete(*ScratchPayload);
				return false;
			}
		}
		return true;
	}

	bool FPackageAssetDurableBackend::Publish(
		const FAssetDurableSessionRef& Session,
		FUeAgentResourceTracker& Resources,
		FAssetDurableError& OutError)
	{
		OutError = FAssetDurableError();
		TArray<FString> PublishedFiles;
		ON_SCOPE_EXIT { RefreshPublishedPackageFiles(PublishedFiles); };
		for (const FDurableArtifact& Artifact :
			Session->GetInventory().GetArtifacts())
		{
			if (!TryCharge(
				Resources,
				TEXT("/c8d/package/publish"),
				1,
				1,
				0,
				0,
				1,
				OutError))
			{
				return false;
			}
			const FString FinalPath = Artifact.GetFinalPath();
			PublishedFiles.Add(FinalPath);
			ReleaseLoadedPackageFileForReplacement(FinalPath);
			if (Artifact.GetIntent()
				== EAssetDurableIntent::Delete)
			{
				if (IFileManager::Get().FileExists(*FinalPath)
					&& !IFileManager::Get().Delete(
						*FinalPath,
						false,
						true,
						true))
				{
					OutError = MakeFileOperationFailure(TEXT("durable_package_delete_failed"), TEXT("Deleting the package file failed."), TEXT("asset_durable"), FinalPath);
					OutError.TargetPath = Artifact.GetDurableLocation();
					return false;
				}
				continue;
			}
			const FString StagePath =
				StagedArtifactPath(Session, Artifact);
			if (!IFileManager::Get().FileExists(*StagePath))
			{
				if (Artifact.GetRole() == TEXT("package_header"))
				{
					OutError = BackendFailure(TEXT("durable_package_stage_missing"), TEXT("The staged package header file is missing."), Artifact.GetDurableLocation());
					OutError.InternalContext = StagePath;
					return false;
				}
				if (IFileManager::Get().FileExists(*FinalPath)
					&& !IFileManager::Get().Delete(
						*FinalPath,
						false,
						true,
						true))
				{
					OutError = MakeFileOperationFailure(TEXT("durable_package_publish_failed"), TEXT("Deleting the stale package payload failed."), TEXT("asset_durable"), FinalPath);
					OutError.TargetPath = Artifact.GetDurableLocation();
					return false;
				}
				continue;
			}
			IFileManager::Get().MakeDirectory(
				*FPaths::GetPath(FinalPath),
				true);
			if (!IFileManager::Get().Move(
				*FinalPath,
				*StagePath,
				true,
				true,
				false,
				true))
			{
				OutError = MakeFileOperationFailure(TEXT("durable_package_publish_failed"), TEXT("Moving the staged package file to its final location failed."), TEXT("asset_durable"), FinalPath);
				OutError.TargetPath = Artifact.GetDurableLocation();
				return false;
			}
		}
		return true;
	}

	bool FPackageAssetDurableBackend::Restore(
		const FAssetDurableSessionRef& Session,
		FUeAgentResourceTracker& Resources,
		bool& bOutConfirmed,
		FAssetDurableError& OutError)
	{
		bOutConfirmed = false;
		OutError = FAssetDurableError();
		TArray<FString> PublishedFiles;
		ON_SCOPE_EXIT { RefreshPublishedPackageFiles(PublishedFiles); };
		for (const FDurableArtifact& Artifact :
			Session->GetInventory().GetArtifacts())
		{
			if (!TryCharge(
				Resources,
				TEXT("/c8d/package/restore"),
				1,
				1,
				0,
				0,
				1,
				OutError))
			{
				return false;
			}
			FString Marker;
			const FString MarkerPath =
				BackupMarkerPath(Session, Artifact);
			const FString DataPath =
				BackupDataPath(Session, Artifact);
			if (!FFileHelper::LoadFileToString(
				Marker,
				*MarkerPath))
			{
				OutError = MakeFileOperationFailure(TEXT("durable_backup_marker_missing"), TEXT("The package backup presence marker could not be read."), TEXT("asset_durable"), MarkerPath);
				OutError.TargetPath = Artifact.GetDurableLocation();
				return false;
			}
			Marker.TrimStartAndEndInline();
			ReleaseLoadedPackageFileForReplacement(Artifact.GetFinalPath());
			PublishedFiles.Add(Artifact.GetFinalPath());
			if (Marker == TEXT("present"))
			{
				IFileManager::Get().MakeDirectory(
					*FPaths::GetPath(Artifact.GetFinalPath()),
					true);
				if (IFileManager::Get().Copy(
					*Artifact.GetFinalPath(),
					*DataPath,
					true,
					true)
						!= COPY_OK)
				{
					OutError = MakeFileOperationFailure(TEXT("durable_restore_failed"), TEXT("Copying the package backup to the original file failed."), TEXT("asset_durable"), Artifact.GetFinalPath());
					OutError.TargetPath = Artifact.GetDurableLocation();
					return false;
				}
				if (!TryVerifyRestoredFile(Artifact.GetFinalPath(), DataPath, Artifact.GetDurableLocation(), OutError))
				{
					return false;
				}
			}
			else if (Marker == TEXT("absent"))
			{
				if (IFileManager::Get().FileExists(
						*Artifact.GetFinalPath())
					&& !IFileManager::Get().Delete(
						*Artifact.GetFinalPath(),
						false,
						true,
						true))
				{
					OutError = MakeFileOperationFailure(TEXT("durable_restore_failed"), TEXT("Removing the package file to restore its original absence failed."), TEXT("asset_durable"), Artifact.GetFinalPath());
					OutError.TargetPath = Artifact.GetDurableLocation();
					return false;
				}
			}
			else
			{
				OutError = BackendFailure(TEXT("durable_backup_marker_invalid"), TEXT("The package backup presence marker must contain present or absent."), Artifact.GetDurableLocation());
				OutError.InternalContext = MarkerPath;
				OutError.ActualEvidence = MakeShared<FJsonValueString>(Marker);
				return false;
			}
		}
		bOutConfirmed = true;
		return true;
	}

	bool FPackageAssetDurableBackend::Inspect(
		const FAssetDurableSessionRef& Session,
		FUeAgentResourceTracker& Resources,
		FString& OutObservedState,
		FAssetDurableError& OutError)
	{
		OutObservedState = TEXT("unknown");
		OutError = FAssetDurableError();
		if (Session->GetRequest().GetTargets().ContainsByPredicate([](const FAssetDurableTarget& Target) {
			return Target.GetIntent() == EAssetDurableIntent::Native;
		}))
		{
			TArray<FDurableArtifactState> Observed;
			if (!TryCaptureArtifactStates(Session->GetInventory(), TEXT("/c8d/native/inspect"), Resources, Observed, OutError)) return false;
			if (CompareArtifactStates(Session->GetInventory(), Session->GetExpectedPersistedStates(), Observed, TEXT("persisted_state")).IsEmpty())
				OutObservedState = TEXT("persisted");
			return true;
		}
		for (const FDurableArtifact& Artifact :
			Session->GetInventory().GetArtifacts())
		{
			if (!TryCharge(
				Resources,
				TEXT("/c8d/package/inspect"),
				1,
				1,
				0,
				0,
				1,
				OutError))
			{
				return false;
			}
			const bool bExists = IFileManager::Get().FileExists(
				*Artifact.GetFinalPath());
			if ((Artifact.GetIntent()
					== EAssetDurableIntent::Delete
					&& bExists)
				|| (Artifact.GetIntent()
						== EAssetDurableIntent::Save
					&& Artifact.GetRole()
						== TEXT("package_header")
					&& !bExists))
			{
				return true;
			}
		}
		OutObservedState = TEXT("persisted");
		return true;
	}

	FProjectConfigAssetDurableBackend::
		FProjectConfigAssetDurableBackend(
			TArray<FString> InAllowedConfigRoots,
			TSharedRef<
				IProjectConfigDurableStageSource,
				ESPMode::ThreadSafe> InStageSource)
		: StageSource(MoveTemp(InStageSource))
	{
		TSet<FString> Seen;
		for (const FString& RawRoot : InAllowedConfigRoots)
		{
			const FString Root = FullPath(RawRoot);
			const FString Folded = Root.ToLower();
			if (!Root.IsEmpty() && !Seen.Contains(Folded))
			{
				Seen.Add(Folded);
				AllowedConfigRoots.Add(Root);
			}
		}
		AllowedConfigRoots.Sort();
	}

	TArray<FString>
	FProjectConfigAssetDurableBackend::
		GetAllowedArtifactRoots() const
	{
		return AllowedConfigRoots;
	}

	bool FProjectConfigAssetDurableBackend::SupportsAtomicity(
		const EAtomicityMode Mode) const
	{
		return Mode == EAtomicityMode::CompensatedSaga;
	}

	bool FProjectConfigAssetDurableBackend::BuildInventory(
		const FAssetDurableRequest& Request,
		FUeAgentResourceTracker& Resources,
		FDurableInventory& OutInventory,
		FAssetDurableError& OutError)
	{
		OutInventory = FDurableInventory();
		OutError = FAssetDurableError();
		TArray<FDurableArtifact> Artifacts;
		TSet<FString> FinalPaths;
		for (const FAssetDurableTarget& Target :
			Request.GetTargets())
		{
			if (Target.GetArtifactKind()
				!= EDurableArtifactKind::ProjectConfig)
			{
				OutError = BackendFailure(TEXT("durable_artifact_kind_unsupported"), TEXT("The project configuration backend requires a project configuration target."), Target.GetDurableLocation());
				OutError.PropertyPath = TEXT("artifact_kind");
				OutError.ActualValue = MakeShared<FJsonValueString>(LexToString(Target.GetArtifactKind()));
				OutError.ExpectedValue = MakeShared<FJsonValueString>(LexToString(EDurableArtifactKind::ProjectConfig));
				return false;
			}
			const FString FinalPath =
				FullPath(Target.GetDurableLocation());
			bool bAllowed = false;
			for (const FString& Root : AllowedConfigRoots)
			{
				if (IsWithin(FinalPath, Root))
				{
					bAllowed = true;
					break;
				}
			}
			const FString FoldedPath = FinalPath.ToLower();
			if (!bAllowed)
			{
				OutError = BackendFailure(TEXT("durable_project_config_path_invalid"), TEXT("The configuration file is outside the allowed project configuration directories."), FinalPath);
				OutError.PropertyPath = TEXT("durable_location");
				return false;
			}
			if (FinalPaths.Contains(FoldedPath))
			{
				OutError = BackendFailure(TEXT("durable_project_config_duplicate"), TEXT("Multiple configuration targets resolve to the same file."), FinalPath);
				return false;
			}
			FinalPaths.Add(FoldedPath);
			Artifacts.Emplace(
				DurableArtifactId(
					Target,
					TEXT("project_config_document")),
				Target.GetIdentity(),
				EDurableArtifactKind::ProjectConfig,
				Target.GetDurableLocation(),
				FinalPath,
				Target.GetIntent(),
				TEXT("project_config_document"));
		}
		Artifacts.Sort(
			[](const FDurableArtifact& Left,
				const FDurableArtifact& Right)
			{
				return Left.GetArtifactId()
					< Right.GetArtifactId();
			});
		FString InventoryHash;
		if (!TryComputeDurableInventoryHash(
				Artifacts,
				Resources,
				InventoryHash,
				OutError))
		{
			return false;
		}
		OutInventory = FDurableInventory(
			MoveTemp(Artifacts),
			MoveTemp(InventoryHash));
		return true;
	}

	bool FProjectConfigAssetDurableBackend::CaptureBackups(
		const FAssetDurableSessionRef& Session,
		FUeAgentResourceTracker& Resources,
		FAssetDurableError& OutError)
	{
		OutError = FAssetDurableError();
		const FString BackupRoot =
			Session->GetTransactionRoot() / TEXT("backups");
		if (!IFileManager::Get().MakeDirectory(
				*BackupRoot,
				true))
		{
			OutError = MakeFileOperationFailure(TEXT("durable_backup_root_unavailable"), TEXT("Creating the project configuration backup directory failed."), TEXT("asset_durable"), BackupRoot);
			return false;
		}
		for (const FDurableArtifact& Artifact :
			Session->GetInventory().GetArtifacts())
		{
			if (!TryCharge(
					Resources,
					TEXT("/c8d/project_config/backup"),
					1,
					1,
					0,
					0,
					1,
					OutError))
			{
				return false;
			}
			const FString DataPath =
				BackupDataPath(Session, Artifact);
			const FString MarkerPath =
				BackupMarkerPath(Session, Artifact);
			const bool bExists =
				IFileManager::Get().FileExists(
					*Artifact.GetFinalPath());
			if (bExists
					&& IFileManager::Get().Copy(
						*DataPath,
						*Artifact.GetFinalPath(),
						true,
						true)
						!= COPY_OK)
			{
				OutError = MakeFileOperationFailure(TEXT("durable_backup_failed"), TEXT("Copying the original configuration file into its backup failed."), TEXT("asset_durable"), DataPath);
				OutError.TargetPath = Artifact.GetDurableLocation();
				return false;
			}
			if (!FFileHelper::SaveStringToFile(
					bExists ? TEXT("present") : TEXT("absent"),
					*MarkerPath,
					FFileHelper::EEncodingOptions::
						ForceUTF8WithoutBOM))
			{
				OutError = MakeFileOperationFailure(TEXT("durable_backup_failed"), TEXT("Writing the configuration backup presence marker failed."), TEXT("asset_durable"), MarkerPath);
				OutError.TargetPath = Artifact.GetDurableLocation();
				return false;
			}
		}
		return true;
	}

	bool FProjectConfigAssetDurableBackend::Stage(
		const FAssetDurableSessionRef& Session,
		FUeAgentResourceTracker& Resources,
		FAssetDurableError& OutError)
	{
		OutError = FAssetDurableError();
		if (!IFileManager::Get().MakeDirectory(
				*Session->GetStagingRoot(),
				true))
		{
			OutError = MakeFileOperationFailure(TEXT("durable_stage_root_unavailable"), TEXT("Creating the project configuration staging directory failed."), TEXT("asset_durable"), Session->GetStagingRoot());
			return false;
		}
		for (const FAssetDurableTarget& Target :
			Session->GetRequest().GetTargets())
		{
			if (Target.GetArtifactKind()
				!= EDurableArtifactKind::ProjectConfig)
			{
				OutError = BackendFailure(TEXT("durable_artifact_kind_unsupported"), TEXT("The project configuration backend requires a project configuration target."), Target.GetDurableLocation());
				OutError.PropertyPath = TEXT("artifact_kind");
				OutError.ActualValue = MakeShared<FJsonValueString>(LexToString(Target.GetArtifactKind()));
				OutError.ExpectedValue = MakeShared<FJsonValueString>(LexToString(EDurableArtifactKind::ProjectConfig));
				return false;
			}
			if (Target.GetIntent()
				== EAssetDurableIntent::Delete)
			{
				continue;
			}
			const FDurableArtifact* Artifact =
				FindArtifact(
					Session,
					Target,
					TEXT("project_config_document"));
			if (!Artifact)
			{
				OutError = BackendFailure(TEXT("durable_inventory_invalid"), TEXT("The durable inventory has no configuration document for this target."), Target.GetDurableLocation());
				OutError.InternalContext = Target.GetIdentity().GetCanonicalKey();
				return false;
			}
			const FString StagePath =
				StagedArtifactPath(Session, *Artifact);
			IFileManager::Get().Delete(
				*StagePath,
				false,
				true,
				true);
			if (!TryCharge(
					Resources,
					TEXT("/c8d/project_config/stage"),
					1,
					1,
					0,
					0,
					1,
					OutError))
			{
				return false;
			}
			if (!StageSource->TryWriteStagedArtifact(
					Target,
					StagePath,
					Resources,
					OutError))
			{
				if (OutError.IsEmpty())
				{
					OutError = BackendFailure(TEXT("durable_project_config_stage_failed"), TEXT("The configuration staging provider failed without returning a diagnostic."), Target.GetDurableLocation());
					OutError.InternalContext = StagePath;
				}
				return false;
			}
			if (!IFileManager::Get().FileExists(*StagePath))
			{
				OutError = BackendFailure(TEXT("durable_project_config_stage_failed"), TEXT("The configuration staging provider reported success but did not produce the staged file."), Target.GetDurableLocation());
				OutError.InternalContext = StagePath;
				return false;
			}
		}
		return true;
	}

	bool FProjectConfigAssetDurableBackend::Publish(
		const FAssetDurableSessionRef& Session,
		FUeAgentResourceTracker& Resources,
		FAssetDurableError& OutError)
	{
		OutError = FAssetDurableError();
		for (const FDurableArtifact& Artifact :
			Session->GetInventory().GetArtifacts())
		{
			if (!TryCharge(
					Resources,
					TEXT("/c8d/project_config/publish"),
					1,
					1,
					0,
					0,
					1,
					OutError))
			{
				return false;
			}
			const FString& FinalPath = Artifact.GetFinalPath();
			if (Artifact.GetIntent()
				== EAssetDurableIntent::Delete)
			{
				if (IFileManager::Get().FileExists(*FinalPath)
					&& !IFileManager::Get().Delete(
						*FinalPath,
						false,
						true,
						true))
				{
					OutError = MakeFileOperationFailure(TEXT("durable_project_config_delete_failed"), TEXT("Deleting the project configuration file failed."), TEXT("asset_durable"), FinalPath);
					OutError.TargetPath = Artifact.GetDurableLocation();
					return false;
				}
				continue;
			}
			const FString StagePath =
				StagedArtifactPath(Session, Artifact);
			if (!IFileManager::Get().FileExists(*StagePath))
			{
				OutError = BackendFailure(TEXT("durable_project_config_stage_missing"), TEXT("The staged configuration file is missing before publication."), Artifact.GetDurableLocation());
				OutError.InternalContext = StagePath;
				return false;
			}
			IFileManager::Get().MakeDirectory(
				*FPaths::GetPath(FinalPath),
				true);
			if (!IFileManager::Get().Move(
					*FinalPath,
					*StagePath,
					true,
					true,
					false,
					true))
			{
				OutError = MakeFileOperationFailure(TEXT("durable_project_config_publish_failed"), TEXT("Moving the staged configuration file to its final location failed."), TEXT("asset_durable"), FinalPath);
				OutError.TargetPath = Artifact.GetDurableLocation();
				return false;
			}
		}
		return true;
	}

	bool FProjectConfigAssetDurableBackend::Restore(
		const FAssetDurableSessionRef& Session,
		FUeAgentResourceTracker& Resources,
		bool& bOutConfirmed,
		FAssetDurableError& OutError)
	{
		bOutConfirmed = false;
		OutError = FAssetDurableError();
		for (const FDurableArtifact& Artifact :
			Session->GetInventory().GetArtifacts())
		{
			if (!TryCharge(
					Resources,
					TEXT("/c8d/project_config/restore"),
					1,
					1,
					0,
					0,
					1,
					OutError))
			{
				return false;
			}
			FString Marker;
			const FString MarkerPath =
				BackupMarkerPath(Session, Artifact);
			const FString DataPath =
				BackupDataPath(Session, Artifact);
			if (!FFileHelper::LoadFileToString(
					Marker,
					*MarkerPath))
			{
				OutError = MakeFileOperationFailure(TEXT("durable_backup_marker_missing"), TEXT("The configuration backup presence marker could not be read."), TEXT("asset_durable"), MarkerPath);
				OutError.TargetPath = Artifact.GetDurableLocation();
				return false;
			}
			Marker.TrimStartAndEndInline();
			if (Marker == TEXT("present"))
			{
				IFileManager::Get().MakeDirectory(
					*FPaths::GetPath(
						Artifact.GetFinalPath()),
					true);
				if (IFileManager::Get().Copy(
						*Artifact.GetFinalPath(),
						*DataPath,
						true,
						true)
						!= COPY_OK)
				{
					OutError = MakeFileOperationFailure(TEXT("durable_restore_failed"), TEXT("Copying the configuration backup to the original file failed."), TEXT("asset_durable"), Artifact.GetFinalPath());
					OutError.TargetPath = Artifact.GetDurableLocation();
					return false;
				}
				if (!TryVerifyRestoredFile(Artifact.GetFinalPath(), DataPath, Artifact.GetDurableLocation(), OutError))
				{
					return false;
				}
			}
			else if (Marker == TEXT("absent"))
			{
				if (IFileManager::Get().FileExists(
						*Artifact.GetFinalPath())
					&& !IFileManager::Get().Delete(
						*Artifact.GetFinalPath(),
						false,
						true,
						true))
				{
					OutError = MakeFileOperationFailure(TEXT("durable_restore_failed"), TEXT("Removing the configuration file to restore its original absence failed."), TEXT("asset_durable"), Artifact.GetFinalPath());
					OutError.TargetPath = Artifact.GetDurableLocation();
					return false;
				}
			}
			else
			{
				OutError = BackendFailure(TEXT("durable_backup_marker_invalid"), TEXT("The configuration backup presence marker must contain present or absent."), Artifact.GetDurableLocation());
				OutError.InternalContext = MarkerPath;
				OutError.ActualEvidence = MakeShared<FJsonValueString>(Marker);
				return false;
			}
		}
		bOutConfirmed = true;
		return true;
	}

	bool FProjectConfigAssetDurableBackend::Inspect(
		const FAssetDurableSessionRef& Session,
		FUeAgentResourceTracker& Resources,
		FString& OutObservedState,
		FAssetDurableError& OutError)
	{
		OutObservedState = TEXT("unknown");
		OutError = FAssetDurableError();
		for (const FDurableArtifact& Artifact :
			Session->GetInventory().GetArtifacts())
		{
			if (!TryCharge(
					Resources,
					TEXT("/c8d/project_config/inspect"),
					1,
					1,
					0,
					0,
					1,
					OutError))
			{
				return false;
			}
			const bool bExists =
				IFileManager::Get().FileExists(
					*Artifact.GetFinalPath());
			if ((Artifact.GetIntent()
					== EAssetDurableIntent::Delete
					&& bExists)
				|| (Artifact.GetIntent()
						== EAssetDurableIntent::Save
					&& !bExists))
			{
				return true;
			}
		}
		OutObservedState = TEXT("persisted");
		return true;
	}

	bool TryComputeDurableInventoryHash(
		const TArray<FDurableArtifact>& Artifacts,
		FUeAgentResourceTracker& Resources,
		FString& OutInventoryHash,
		FAssetDurableError& OutError)
	{
		return TryComputeDurableInventoryHashImpl(
			Artifacts,
			true,
			Resources,
			OutInventoryHash,
			OutError);
	}

	bool TryAcquireAssetDurableFence(
		const FString& JournalRoot,
		const FString& StableJournalOwnerId,
		FUeAgentResourceTracker& Resources,
		int64& OutFencingToken,
		FAssetDurableError& OutError)
	{
		OutFencingToken = -1;
		OutError = FAssetDurableError();
		const FString OwnerId = NormalizedText(StableJournalOwnerId);
		const FString Root = FullPath(JournalRoot);
		const FString StatePath = Root / TEXT("fence-state.json");
		const FString TemporaryPath = StatePath + TEXT(".tmp");
		if (OwnerId.IsEmpty())
		{
			OutError = BackendFailure(TEXT("durable_fence_store_invalid"), TEXT("Acquiring a durability fence requires a non-empty journal owner."), FString());
			OutError.PropertyPath = TEXT("journal_owner_id");
			return false;
		}
		if (!IsWithin(StatePath, Root) || !IsWithin(TemporaryPath, Root))
		{
			OutError = BackendFailure(TEXT("durable_fence_store_invalid"), TEXT("The durability fence state or temporary file is outside its storage root."), FString());
			OutError.InternalContext = !IsWithin(StatePath, Root) ? StatePath : TemporaryPath;
			return false;
		}

		int64 LastToken = -1;
		if (IFileManager::Get().FileExists(*StatePath))
		{
			FString PayloadText;
			if (!TryReadOuterJournal(
					StatePath,
					Resources,
					PayloadText,
					OutError))
			{
				return false;
			}
			TSharedPtr<FJsonObject> Payload;
			const TSharedRef<TJsonReader<>> Reader =
				TJsonReaderFactory<>::Create(PayloadText);
			FString Schema;
			FString StoredOwnerId;
			double StoredToken = -1.0;
			if (!FJsonSerializer::Deserialize(Reader, Payload) || !Payload.IsValid())
			{
				OutError = BackendFailure(TEXT("durable_fence_store_integrity_invalid"), TEXT("The durability fence state payload is not a valid JSON object."), FString());
				OutError.InternalContext = StatePath;
				OutError.ActualEvidence = MakeShared<FJsonValueString>(Reader->GetErrorMessage());
				return false;
			}
			FJournalFields Fields{Payload, TEXT("fence_store"), OutError, TEXT("durable_fence_store_integrity_invalid")};
			if (!Fields.String(TEXT("schema"), Schema)) return false;
			if (Schema != FenceStateSchemaV1)
			{
				OutError = BackendFailure(TEXT("durable_fence_store_integrity_invalid"), TEXT("The durability fence state schema is unsupported."), FString());
				OutError.PropertyPath = TEXT("fence_store.schema");
				OutError.ActualValue = MakeShared<FJsonValueString>(Schema);
				OutError.ExpectedValue = MakeShared<FJsonValueString>(FenceStateSchemaV1);
				return false;
			}
			if (!Fields.String(TEXT("journal_owner_id"), StoredOwnerId)) return false;
			if (StoredOwnerId != OwnerId)
			{
				OutError = BackendFailure(TEXT("durable_fence_store_integrity_invalid"), TEXT("The stored durability fence owner differs from the owner requesting the fence."), FString());
				OutError.PropertyPath = TEXT("fence_store.journal_owner_id");
				OutError.ActualEvidence = MakeShared<FJsonValueString>(StoredOwnerId);
				OutError.ExpectedEvidence = MakeShared<FJsonValueString>(OwnerId);
				return false;
			}
			if (!Fields.Number(TEXT("last_fencing_token"), StoredToken)) return false;
			if (!FMath::IsFinite(StoredToken)
				|| StoredToken < 0.0
				|| StoredToken > 9007199254740990.0
				|| FMath::FloorToDouble(StoredToken) != StoredToken)
			{
				OutError = BackendFailure(TEXT("durable_fence_store_integrity_invalid"), TEXT("The stored fencing token must be a finite integer between 0 and 9007199254740990."), FString());
				OutError.PropertyPath = TEXT("fence_store.last_fencing_token");
				OutError.ActualEvidence = MakeShared<FJsonValueString>(FString::Printf(TEXT("%.17g"), StoredToken));
				return false;
			}
			LastToken = static_cast<int64>(StoredToken);
		}

		const int64 NextToken = LastToken + 1;
		const TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
		Payload->SetStringField(TEXT("schema"), FenceStateSchemaV1);
		Payload->SetStringField(TEXT("journal_owner_id"), OwnerId);
		Payload->SetNumberField(
			TEXT("last_fencing_token"),
			static_cast<double>(NextToken));
		FString PayloadText;
		if (!TrySerializeObject(Payload, PayloadText))
		{
			OutError = BackendFailure(TEXT("durable_fence_store_serialize_failed"), TEXT("The durability fence state payload could not be encoded as JSON."), FString());
			return false;
		}
		const TSharedRef<FJsonObject> Outer = MakeShared<FJsonObject>();
		Outer->SetStringField(TEXT("payload"), PayloadText);
		Outer->SetStringField(
			TEXT("integrity_sha256"),
			Sha256(PayloadText));
		FString Encoded;
		if (!TrySerializeObject(Outer, Encoded))
		{
			OutError = BackendFailure(TEXT("durable_fence_store_serialize_failed"), TEXT("The durability fence state envelope could not be encoded as JSON."), FString());
			return false;
		}
		const FTCHARToUTF8 Utf8(*Encoded);
		if (!TryCharge(
				Resources,
				TEXT("/c8d/fence/acquire"),
				1,
				0,
				0,
				Utf8.Length(),
				1,
				OutError))
		{
			return false;
		}
		IFileManager::Get().MakeDirectory(*Root, true);
		IFileManager::Get().Delete(*TemporaryPath, false, true, true);
		if (!FFileHelper::SaveStringToFile(
				Encoded,
				*TemporaryPath,
				FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			OutError = MakeFileOperationFailure(TEXT("durable_fence_store_write_failed"), TEXT("Writing the temporary durability fence state failed."), TEXT("asset_durable"), TemporaryPath);
			IFileManager::Get().Delete(*TemporaryPath, false, true, true);
			return false;
		}
		if (!IFileManager::Get().Move(
				*StatePath,
				*TemporaryPath,
				true,
				true,
				false,
				true))
		{
			OutError = MakeFileOperationFailure(TEXT("durable_fence_store_write_failed"), TEXT("Replacing the durability fence state with the written file failed."), TEXT("asset_durable"), StatePath);
			IFileManager::Get().Delete(*TemporaryPath, false, true, true);
			return false;
		}
		OutFencingToken = NextToken;
		return true;
	}

	bool TryPrepareAssetDurability(
		const FAssetDurableRequest& Request,
		IAssetDurableBackend& Backend,
		const FString& JournalRoot,
		FUeAgentResourceTracker& Resources,
		FAssetDurableSessionPtr& OutSession,
		FAssetDurableError& OutError)
	{
		OutSession.Reset();
		OutError = FAssetDurableError();
		if (!TryValidateRequest(Request, OutError)) return false;
		if (!Backend.SupportsAtomicity(Request.GetAtomicityMode()))
		{
			OutError = BackendFailure(TEXT("durable_atomicity_unsupported"), TEXT("The selected durability backend does not support the requested atomicity mode."), FString());
			OutError.PropertyPath = TEXT("atomicity_mode");
			OutError.ActualValue = MakeShared<FJsonValueString>(LexToString(Request.GetAtomicityMode()));
			return false;
		}
		if (!TryCharge(
				Resources,
				TEXT("/c8d/prepare/inventory"),
				1,
				Request.GetTargets().Num(),
				0,
				0,
				Request.GetTargets().Num() + 1,
				OutError))
		{
			return false;
		}
		FDurableInventory Inventory;
		if (!Backend.BuildInventory(
				Request,
				Resources,
				Inventory,
				OutError))
		{
			if (!(!OutError.IsEmpty()))
			{
				OutError = BackendFailure(TEXT("durable_inventory_failed"), TEXT("The durability backend failed to build the inventory without returning a diagnostic."), FString());
			}
			return false;
		}
		if (!TryValidateInventory(
			Request,
			Inventory,
			Backend.GetAllowedArtifactRoots(),
			Resources,
			OutError))
		{
			return false;
		}
		TArray<FDurableArtifactState> PreMutationStates;
		if (!TryCaptureArtifactStates(
				Inventory,
				TEXT("/c8d/prepare/pre_state"),
				Resources,
				PreMutationStates,
				OutError))
		{
			return false;
		}
		const FString Root = FullPath(JournalRoot);
		const FString ActiveRoot = Root / TEXT("active");
		const FString JournalId =
			FGuid::NewGuid().ToString(EGuidFormats::Digits);
		const FString TransactionRoot =
			ActiveRoot / JournalId;
		const FString StagingRoot =
			TransactionRoot / TEXT("staging");
		const FString JournalPath =
			TransactionRoot / TEXT("journal.json");
		if (!IsWithin(TransactionRoot, ActiveRoot))
		{
			OutError = BackendFailure(TEXT("durable_journal_root_invalid"), TEXT("The new durability transaction directory is outside the active journal root."), FString());
			OutError.InternalContext = TransactionRoot;
			return false;
		}
		// Preparation only captures originals into this newly owned directory.
		// A failed preparation has no session owner to retire its partial journal.
		bool bSessionTransferred = false;
		ON_SCOPE_EXIT
		{
			if (!bSessionTransferred
				&& IFileManager::Get().DirectoryExists(*TransactionRoot)
				&& !IFileManager::Get().DeleteDirectory(*TransactionRoot, false, true))
			{
				OutError.Append(MakeFileOperationFailure(TEXT("durable_prepare_cleanup_failed"), TEXT("Removing the incomplete durability transaction directory failed."), TEXT("asset_durable"), TransactionRoot));
			}
		};
		if (!IFileManager::Get().MakeDirectory(
			*StagingRoot,
			true))
		{
			OutError = MakeFileOperationFailure(TEXT("durable_journal_create_failed"), TEXT("Creating the durability transaction staging directory failed."), TEXT("asset_durable"), StagingRoot);
			return false;
		}
		FAssetDurableSessionRef Session =
			MakeShared<const FAssetDurableSession>(
				Request,
				MoveTemp(Inventory),
				JournalId,
				JournalPath,
				Root,
				TransactionRoot,
				StagingRoot,
				TArray<FString>{TEXT("inventory_ready")},
				MoveTemp(PreMutationStates));
		if (!TryWriteJournal(Session, Resources, OutError))
		{
			return false;
		}
		if (!Backend.CaptureBackups(
			Session,
			Resources,
			OutError))
		{
			if (!(!OutError.IsEmpty()))
			{
				OutError = BackendFailure(TEXT("durable_backup_failed"), TEXT("The durability backend failed to capture backups without returning a diagnostic."), FString());
			}
			return false;
		}
		Session = Advance(Session, TEXT("backups_ready"));
		if (!TryWriteJournal(Session, Resources, OutError))
		{
			return false;
		}
		Session = Advance(Session, TEXT("prepared"));
		if (!TryWriteJournal(Session, Resources, OutError))
		{
			return false;
		}
		OutSession = Session;
		bSessionTransferred = true;
		return true;
	}

	bool TryEnlistAssetDurableTargets(
		const FAssetDurableSessionRef& Session,
		const TArray<FAssetDurableTarget>& MutationTargets,
		IAssetDurableBackend& Backend,
		const int64 CurrentFencingToken,
		FUeAgentResourceTracker& Resources,
		FAssetDurableSessionPtr& OutSession,
		FAssetDurableError& OutError)
	{
		OutSession.Reset();
		OutError = FAssetDurableError();
		const FString& CurrentPhase = Session->GetCurrentPhase();
		if ((CurrentPhase != TEXT("prepared")
				&& CurrentPhase != TEXT("inventory_extended")
				&& CurrentPhase != TEXT("native_mutating"))
			|| !TryCheckFence(
				Session,
				CurrentFencingToken,
				OutError))
		{
			if (!(!OutError.IsEmpty()))
			{
				OutError = BackendFailure(TEXT("durable_enlist_phase_invalid"), TEXT("Additional durability targets require the prepared, inventory_extended, or native_mutating phase."), FString());
				OutError.PropertyPath = TEXT("phase");
				OutError.ActualValue = MakeShared<FJsonValueString>(CurrentPhase);
			}
			return false;
		}
		if (MutationTargets.IsEmpty())
		{
			OutSession = Session;
			return true;
		}

		TArray<FAssetDurableTarget> CombinedTargets =
			Session->GetRequest().GetTargets();
		for (int32 Index = 0; Index < MutationTargets.Num(); ++Index)
		{
			const FAssetDurableTarget& Candidate = MutationTargets[Index];
			if (!Candidate.IsValid()
				|| Candidate.GetRollbackGrade() == ERollbackGrade::None
				|| !TryValidateTargetAgainstPlan(
					Session->GetRequest().GetPlan(),
					Candidate,
					OutError))
			{
				if (!(!OutError.IsEmpty()))
				{
					if (!Candidate.IsValid()) OutError = Candidate.GetValidationDiagnostic();
					else
					{
						OutError = FAssetDurableError::Failure(TEXT("durable_enlist_target_invalid"), TEXT("The additional durable target does not support rollback."), TEXT("asset_durable"));
						OutError.TargetPath = Candidate.GetDurableLocation();
						OutError.PropertyPath = TEXT("rollback_grade");
						OutError.ActualValue = MakeShared<FJsonValueString>(TEXT("none"));
						OutError.SourceKind = TEXT("native_result");
					}
				}
				AddTargetIndex(OutError, TEXT("mutation_targets"), Index);
				return false;
			}
			const FString CandidateKey =
				Candidate.GetDurableTargetKey();
			const FAssetDurableTarget* Existing =
				CombinedTargets.FindByPredicate(
					[&CandidateKey](
						const FAssetDurableTarget& Target)
					{
						return Target.GetDurableTargetKey()
							== CandidateKey;
					});
			if (Existing)
			{
				if (Existing->GetIntent() != Candidate.GetIntent()
					|| Existing->GetPackageHeaderKind()
						!= Candidate.GetPackageHeaderKind())
				{
					OutError = FAssetDurableError::Failure(TEXT("durable_enlist_target_conflict"),
						Existing->GetIntent() != Candidate.GetIntent()
							? TEXT("The additional durable target requests a different operation intent than the existing target.")
							: TEXT("The additional durable target requires a different package header kind than the existing target."), TEXT("asset_durable"), CandidateKey);
					OutError.TargetPath = Candidate.GetDurableLocation();
					OutError.SourceKind = TEXT("native_result");
					if (Existing->GetIntent() != Candidate.GetIntent())
					{
						OutError.PropertyPath = TEXT("intent");
						OutError.ActualValue = MakeShared<FJsonValueString>(LexToString(Candidate.GetIntent()));
						OutError.ExpectedValue = MakeShared<FJsonValueString>(LexToString(Existing->GetIntent()));
					}
					else
					{
						const auto HeaderName = [](const EPackageHeaderKind Kind) -> FString
						{
							switch (Kind)
							{
							case EPackageHeaderKind::EvidenceRequired: return TEXT("evidence_required");
							case EPackageHeaderKind::Asset: return TEXT("asset");
							case EPackageHeaderKind::Map: return TEXT("map");
							default: return FString::Printf(TEXT("unknown (%d)"), static_cast<int32>(Kind));
							}
						};
						OutError.PropertyPath = TEXT("package_header_kind");
						OutError.ActualValue = MakeShared<FJsonValueString>(HeaderName(Candidate.GetPackageHeaderKind()));
						OutError.ExpectedValue = MakeShared<FJsonValueString>(HeaderName(Existing->GetPackageHeaderKind()));
					}
					AddTargetIndex(OutError, TEXT("mutation_targets"), Index);
					return false;
				}
				continue;
			}
			CombinedTargets.Add(Candidate);
		}

		if (CombinedTargets.Num()
			== Session->GetRequest().GetTargets().Num())
		{
			OutSession = Session;
			return true;
		}
		if (!TryCharge(
				Resources,
				TEXT("/c8d/enlist/inventory"),
				1,
				CombinedTargets.Num(),
				0,
				0,
				CombinedTargets.Num() + 1,
				OutError))
		{
			return false;
		}
		const FAssetDurableRequest CombinedRequest(
			Session->GetRequest().GetPlan(),
			MoveTemp(CombinedTargets),
			Session->GetRequest().GetFencingToken(),
			Session->GetRequest().GetJournalOwnerId(),
			Session->GetRequest().GetAtomicityMode());
		FDurableInventory CombinedInventory;
		if (!TryValidateRequest(CombinedRequest, OutError)) return false;
		if (!Backend.SupportsAtomicity(CombinedRequest.GetAtomicityMode()))
		{
			OutError = BackendFailure(TEXT("durable_atomicity_unsupported"), TEXT("The selected durability backend does not support the combined request atomicity mode."), FString());
			OutError.PropertyPath = TEXT("atomicity_mode");
			OutError.ActualValue = MakeShared<FJsonValueString>(LexToString(CombinedRequest.GetAtomicityMode()));
			return false;
		}
		if (!Backend.BuildInventory(
				CombinedRequest,
				Resources,
				CombinedInventory,
				OutError))
		{
			if (OutError.IsEmpty()) OutError = BackendFailure(TEXT("durable_enlist_inventory_failed"), TEXT("The durability backend failed to build the combined inventory without returning a diagnostic."), FString());
			return false;
		}
		if (!TryValidateInventory(
				CombinedRequest,
				CombinedInventory,
				Backend.GetAllowedArtifactRoots(),
				Resources,
				OutError))
		{
			return false;
		}

		TArray<FDurableArtifact> NewArtifacts;
		for (const FDurableArtifact& ExistingArtifact :
			Session->GetInventory().GetArtifacts())
		{
			const FDurableArtifact* CombinedArtifact =
				CombinedInventory.GetArtifacts().FindByPredicate(
					[&ExistingArtifact](
						const FDurableArtifact& Artifact)
					{
						return Artifact.GetArtifactId()
							== ExistingArtifact.GetArtifactId();
					});
			if (!CombinedArtifact)
			{
				OutError = BackendFailure(TEXT("durable_enlist_inventory_conflict"), TEXT("Adding durability targets removed an existing artifact from the combined inventory."), ExistingArtifact.GetDurableLocation());
				OutError.InternalContext = ExistingArtifact.GetArtifactId();
				return false;
			}
			OutError = CompareArtifacts(ExistingArtifact, *CombinedArtifact);
			if (!OutError.IsEmpty()) return false;
		}
		for (const FDurableArtifact& Artifact :
			CombinedInventory.GetArtifacts())
		{
			const bool bExisting = Session->GetInventory()
				.GetArtifacts().ContainsByPredicate(
					[&Artifact](const FDurableArtifact& Existing)
					{
						return Existing.GetArtifactId()
							== Artifact.GetArtifactId();
					});
			if (!bExisting)
			{
				NewArtifacts.Add(Artifact);
			}
		}
		if (NewArtifacts.IsEmpty())
		{
			OutError = BackendFailure(TEXT("durable_enlist_inventory_unchanged"), TEXT("The combined durability inventory contains no new artifacts for the additional targets."), FString());
			OutError.PropertyPath = TEXT("artifacts");
			return false;
		}

		FString DeltaInventoryHash;
		if (!TryComputeDurableInventoryHash(
				NewArtifacts,
				Resources,
				DeltaInventoryHash,
				OutError))
		{
			return false;
		}
		const FDurableInventory DeltaInventory(
			NewArtifacts,
			DeltaInventoryHash);
		TArray<FDurableArtifactState> DeltaPreMutationStates;
		if (!TryCaptureArtifactStates(
				DeltaInventory,
				TEXT("/c8d/enlist/pre_state"),
				Resources,
				DeltaPreMutationStates,
				OutError))
		{
			return false;
		}
		TArray<FDurableArtifactState> CombinedPreMutationStates =
			Session->GetPreMutationStates();
		CombinedPreMutationStates.Append(DeltaPreMutationStates);
		const FAssetDurableSessionRef DeltaSession =
			MakeShared<const FAssetDurableSession>(
				CombinedRequest,
				DeltaInventory,
				Session->GetJournalId(),
				Session->GetJournalPath(),
				Session->GetJournalRoot(),
				Session->GetTransactionRoot(),
				Session->GetStagingRoot(),
				Session->GetPhaseHistory(),
				MoveTemp(DeltaPreMutationStates));
		if (!Backend.CaptureBackups(
				DeltaSession,
				Resources,
				OutError))
		{
			if (!(!OutError.IsEmpty()))
			{
				OutError = BackendFailure(TEXT("durable_enlist_backup_failed"), TEXT("The durability backend failed to back up the newly added artifacts without returning a diagnostic."), FString());
			}
			return false;
		}

		FAssetDurableSessionRef ExtendedSession =
			MakeShared<const FAssetDurableSession>(
				CombinedRequest,
				MoveTemp(CombinedInventory),
				Session->GetJournalId(),
				Session->GetJournalPath(),
				Session->GetJournalRoot(),
				Session->GetTransactionRoot(),
				Session->GetStagingRoot(),
				Session->GetPhaseHistory(),
				MoveTemp(CombinedPreMutationStates),
				Session->GetExpectedPersistedStates());
		ExtendedSession = Advance(
			ExtendedSession,
			CurrentPhase == TEXT("native_mutating") ? TEXT("native_mutating") : TEXT("inventory_extended"));
		if (!TryWriteJournal(
				ExtendedSession,
				Resources,
				OutError))
		{
			return false;
		}
		OutSession = ExtendedSession;
		return true;
	}

	static bool TryRecordPersistedAssetDurability(FAssetDurableSessionRef& Working,
		IAssetDurableBackend& Backend, FUeAgentResourceTracker& Resources,
		FDurableReceipt& OutReceipt, FAssetDurableError& OutError);

	bool TryPersistAssetDurability(
		const FAssetDurableSessionRef& Session,
		IAssetDurableBackend& Backend,
		const int64 CurrentFencingToken,
		FUeAgentResourceTracker& Resources,
		FDurableReceipt& OutReceipt,
		FAssetDurableError& OutError)
	{
		OutReceipt = FDurableReceipt();
		OutError = FAssetDurableError();
		if (!TryCheckFence(
			Session,
			CurrentFencingToken,
			OutError))
		{
			return false;
		}
		FAssetDurableSessionRef Working = Session;
		FAssetDurableError BackendError;
		if (!Backend.Stage(
			Working,
			Resources,
			BackendError))
		{
			if (BackendError.IsEmpty()) BackendError = BackendFailure(TEXT("durable_persist_failed"), TEXT("The durability backend failed to stage artifacts without returning a diagnostic."), FString());
			return TryRestorePersistFailure(
				Working,
				Backend,
				Resources,
				BackendError,
				OutReceipt,
				OutError);
		}
		Working = Advance(Working, TEXT("staged"));
		if (!TryWriteJournal(Working, Resources, OutError))
		{
			return TryRestorePersistFailure(
				Working,
				Backend,
				Resources,
				OutError,
				OutReceipt,
				OutError);
		}
		if (!TryCheckFence(
			Working,
			CurrentFencingToken,
			OutError))
		{
			return false;
		}
		BackendError = FAssetDurableError();
		if (!Backend.Publish(
			Working,
			Resources,
			BackendError))
		{
			if (BackendError.IsEmpty()) BackendError = BackendFailure(TEXT("durable_persist_failed"), TEXT("The durability backend failed to publish artifacts without returning a diagnostic."), FString());
			return TryRestorePersistFailure(
				Working,
				Backend,
				Resources,
				BackendError,
				OutReceipt,
				OutError);
		}
		return TryRecordPersistedAssetDurability(Working, Backend, Resources, OutReceipt, OutError);
	}

	static bool TryRecordPersistedAssetDurability(FAssetDurableSessionRef& Working,
		IAssetDurableBackend& Backend, FUeAgentResourceTracker& Resources,
		FDurableReceipt& OutReceipt, FAssetDurableError& OutError)
	{
		TArray<FDurableArtifactState> ExpectedPersistedStates;
		if (!TryCaptureArtifactStates(
				Working->GetInventory(),
				TEXT("/c8d/persist/expected_state"),
				Resources,
				ExpectedPersistedStates,
				OutError))
		{
			return TryRestorePersistFailure(
				Working,
				Backend,
				Resources,
				OutError,
				OutReceipt,
				OutError);
		}
		Working = MakeShared<const FAssetDurableSession>(
			Working->GetRequest(),
			Working->GetInventory(),
			Working->GetJournalId(),
			Working->GetJournalPath(),
			Working->GetJournalRoot(),
			Working->GetTransactionRoot(),
			Working->GetStagingRoot(),
			Working->GetPhaseHistory(),
			Working->GetPreMutationStates(),
			MoveTemp(ExpectedPersistedStates));
		Working = Advance(Working, TEXT("published"));
		if (!TryWriteJournal(Working, Resources, OutError))
		{
			return TryRestorePersistFailure(
				Working,
				Backend,
				Resources,
				OutError,
				OutReceipt,
				OutError);
		}
		Working = Advance(
			Working,
			TEXT("persisted_unverified"));
		if (!TryWriteJournal(Working, Resources, OutError))
		{
			return TryRestorePersistFailure(
				Working,
				Backend,
				Resources,
				OutError,
				OutReceipt,
				OutError);
		}
		OutReceipt = Receipt(
			Working,
			EDurableStatus::PersistedUnverified,
			ERecoveryAction::None,
			true,
			false);
		return true;
	}

	bool TryBeginNativeAssetDurableMutation(
		FAssetDurableSessionPtr& Session, const int64 CurrentFencingToken,
		FUeAgentResourceTracker& Resources, FAssetDurableError& OutError)
	{
		OutError = FAssetDurableError();
		if (!Session)
		{
			OutError = BackendFailure(TEXT("durable_native_session_invalid"), TEXT("Starting a native durable mutation requires a prepared session."), FString());
			return false;
		}
		if (!TryCheckFence(Session.ToSharedRef(), CurrentFencingToken, OutError)) return false;
		if (Session->GetCurrentPhase() != TEXT("prepared") && Session->GetCurrentPhase() != TEXT("inventory_extended"))
		{
			OutError = BackendFailure(TEXT("durable_native_session_invalid"), TEXT("Starting a native durable mutation requires the prepared or inventory_extended phase."), FString());
			OutError.PropertyPath = TEXT("phase");
			OutError.ActualValue = MakeShared<FJsonValueString>(Session->GetCurrentPhase());
			return false;
		}
		// A crash after this point must restore before-images, even if the native
		// operation has not returned or the ordinary publication has not begun.
		Session = Advance(Session.ToSharedRef(), TEXT("native_mutating"));
		return TryWriteJournal(Session.ToSharedRef(), Resources, OutError);
	}

	bool TryPersistNativeAssetDurabilityForVerification(
		FAssetDurableSessionPtr& Session, IAssetDurableBackend& Backend,
		const int64 CurrentFencingToken,
		TFunctionRef<bool(FAssetDurableError&)> NativeOperation,
		FUeAgentResourceTracker& Resources, FDurableReceipt& OutReceipt, FAssetDurableError& OutError)
	{
		OutReceipt = FDurableReceipt();
		OutError = FAssetDurableError();
		if (!Session)
		{
			OutError = BackendFailure(TEXT("durable_native_session_invalid"), TEXT("Persisting a native durable mutation requires a prepared session."), FString());
			return false;
		}
		if (!TryCheckFence(Session.ToSharedRef(), CurrentFencingToken, OutError)) return false;
		if (Session->GetRequest().GetTargets().IsEmpty())
		{
			OutError = BackendFailure(TEXT("durable_native_session_invalid"), TEXT("A native durable mutation requires at least one target."), FString());
			OutError.PropertyPath = TEXT("targets");
			return false;
		}
		if (Session->GetCurrentPhase() != TEXT("prepared") && Session->GetCurrentPhase() != TEXT("inventory_extended"))
		{
			OutError = BackendFailure(TEXT("durable_native_session_invalid"), TEXT("Persisting a native durable mutation requires the prepared or inventory_extended phase."), FString());
			OutError.PropertyPath = TEXT("phase");
			OutError.ActualValue = MakeShared<FJsonValueString>(Session->GetCurrentPhase());
			return false;
		}
		for (int32 Index = 0; Index < Session->GetRequest().GetTargets().Num(); ++Index)
		{
			const FAssetDurableTarget& Target = Session->GetRequest().GetTargets()[Index];
			if (Target.GetIntent() != EAssetDurableIntent::Native)
			{
				OutError = BackendFailure(TEXT("durable_native_session_invalid"), TEXT("Every target in a native durable mutation must use native intent."), Target.GetDurableLocation());
				OutError.PropertyPath = FString::Printf(TEXT("targets[%d].intent"), Index);
				OutError.ActualValue = MakeShared<FJsonValueString>(LexToString(Target.GetIntent()));
				OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("native"));
				return false;
			}
		}
		if (!TryBeginNativeAssetDurableMutation(Session, CurrentFencingToken, Resources, OutError)) return false;
		FAssetDurableError NativeError;
		TMap<FString, FString> AttemptedSaves;
		TSet<FString> CompletedSaves;
		TMap<FString, TWeakObjectPtr<UPackage>> ModifiedPackages;
		const auto EnlistPackage = [&](UPackage* Package) {
			if ((!NativeError.IsEmpty())) return false;
			const FString PackageName = Package->GetName();
			if (Session->GetRequest().GetTargets().ContainsByPredicate(
				[&](const FAssetDurableTarget& Target) { return Target.GetPackageName() == PackageName; })) return true;
			const FAssetDurableTarget Target(Session->GetRequest().GetTargets()[0].GetIdentity(),
				PackageName, EPackageHeaderKind::EvidenceRequired, EAssetDurableIntent::Native, ERollbackGrade::DurableBackup);
			FAssetDurableSessionPtr Extended;
			if (!TryEnlistAssetDurableTargets(Session.ToSharedRef(), {Target}, Backend,
				CurrentFencingToken, Resources, Extended, NativeError)) return false;
			Session = Extended;
			return true;
		};
		const FDelegateHandle DirtyHandle = UPackage::PackageMarkedDirtyEvent.AddLambda(
			[&](UPackage* Package, bool) {
				if (!Package || Package == GetTransientPackage() || Package->HasAnyPackageFlags(PKG_CompiledIn)
					|| Package->HasAnyFlags(RF_Transient)) return;
				ModifiedPackages.Add(Package->GetName(), Package);
				EnlistPackage(Package);
			});
		const auto OriginalSaveCheck = FCoreUObjectDelegates::IsPackageOKToSaveDelegate;
		FCoreUObjectDelegates::IsPackageOKToSaveDelegate.BindLambda(
			[&](UPackage* Package, const FString& Filename, FOutputDevice* Output) {
				if (OriginalSaveCheck.IsBound() && !OriginalSaveCheck.Execute(Package, Filename, Output))
				{
					NativeError.Append(BackendFailure(TEXT("durable_native_save_rejected"), TEXT("The existing Unreal package save check rejected this package."), Package->GetName()));
					return false;
				}
				if (!EnlistPackage(Package)) return false;
				AttemptedSaves.Add(FullPath(Filename), Package->GetName());
				return true;
			});
		const FDelegateHandle SavedHandle = UPackage::PackageSavedWithContextEvent.AddLambda(
			[&](const FString& Filename, UPackage*, FObjectPostSaveContext) { CompletedSaves.Add(FullPath(Filename)); });
		ON_SCOPE_EXIT {
			FCoreUObjectDelegates::IsPackageOKToSaveDelegate = OriginalSaveCheck;
			UPackage::PackageSavedWithContextEvent.Remove(SavedHandle);
			UPackage::PackageMarkedDirtyEvent.Remove(DirtyHandle);
		};
		bool bNativeSucceeded;
		FString NativeFeedback;
		{
			FNativeOperationFeedback Feedback;
			bNativeSucceeded = NativeOperation(NativeError);
			NativeFeedback = Feedback.GetDetail();
		}
		for (const auto& Attempt : AttemptedSaves)
			if (!CompletedSaves.Contains(Attempt.Key) && NativeError.IsEmpty())
			{
				NativeError = BackendFailure(TEXT("durable_native_save_failed"), TEXT("A package save was attempted but did not report completion."), Attempt.Value);
				NativeError.InternalContext = Attempt.Key;
			}
		// RenameAssets can return true even when checkout/save skipped a referencer.
		// A live asset left dirty is not a persisted native result.
		for (const auto& Pair : ModifiedPackages)
			if (UPackage* Package = Pair.Value.Get(); Package && Package->IsDirty()
				&& Package->FindAssetInPackage() && !(!NativeError.IsEmpty()))
				NativeError = BackendFailure(TEXT("durable_native_package_unsaved"), TEXT("The native operation left this asset package with unsaved changes."), Pair.Key);
		if (!bNativeSucceeded || (!NativeError.IsEmpty()))
		{
			if (NativeError.IsEmpty()) NativeError = BackendFailure(TEXT("durable_native_operation_failed"), TEXT("The native durability operation failed without returning a diagnostic."), FString());
			if (!NativeFeedback.IsEmpty())
				NativeError.InternalContext += (NativeError.InternalContext.IsEmpty() ? TEXT("") : TEXT("\n")) + NativeFeedback;
			TryRestorePersistFailure(Session.ToSharedRef(), Backend, Resources, NativeError, OutReceipt, OutError);
			if (!(!OutError.IsEmpty())) OutError = NativeError;
			return false;
		}
		auto Working = Session.ToSharedRef();
		const bool bRecorded = TryRecordPersistedAssetDurability(Working, Backend, Resources, OutReceipt, OutError);
		Session = Working;
		if (!bRecorded) return false;
		if (OutReceipt.GetStatus() == EDurableStatus::PersistedUnverified) return true;
		OutError = OutReceipt.GetFailureDiagnostic();
		if (OutError.IsEmpty()) OutError = BackendFailure(TEXT("durable_persist_not_persisted"), TEXT("The native persistence receipt did not report the persisted_unverified state."), FString());
		OutError.TransactionStatus = LexToString(OutReceipt.GetStatus());
		OutError.RecoveryAction = LexToString(OutReceipt.GetRecoveryAction());
		return false;
	}

	bool TryPersistAssetDurabilityForVerification(
		const FAssetDurableSessionRef& Session,
		IAssetDurableBackend& Backend,
		const int64 CurrentFencingToken,
		FUeAgentResourceTracker& Resources,
		FDurableReceipt& OutReceipt,
		FAssetDurableError& OutError)
	{
		if (!TryPersistAssetDurability(Session, Backend, CurrentFencingToken,
			Resources, OutReceipt, OutError)) return false;
		if (OutReceipt.GetStatus() == EDurableStatus::PersistedUnverified
			&& OutReceipt.RequiresIndependentVerification()) return true;
		OutError = OutReceipt.GetFailureDiagnostic();
		if (OutError.IsEmpty()) OutError = BackendFailure(TEXT("durable_persist_not_persisted"), OutReceipt.GetStatus() != EDurableStatus::PersistedUnverified
			? TEXT("The durability receipt did not report the persisted_unverified state.")
			: TEXT("The durability receipt did not require the expected independent verification."), FString());
		OutError.TransactionStatus = LexToString(OutReceipt.GetStatus());
		OutError.RecoveryAction = LexToString(OutReceipt.GetRecoveryAction());
		return false;
	}

	bool TryRollbackAssetDurability(
		const FAssetDurableSessionRef& Session,
		IAssetDurableBackend& Backend,
		const int64 CurrentFencingToken,
		const bool bMemoryRollbackConfirmed,
		FUeAgentResourceTracker& Resources,
		FDurableReceipt& OutReceipt,
		FAssetDurableError& OutError)
	{
		return TryRollbackAssetDurability(Session, Backend, CurrentFencingToken,
			[bMemoryRollbackConfirmed]() { return bMemoryRollbackConfirmed; },
			Resources, OutReceipt, OutError);
	}

	bool TryRollbackAssetDurability(
		const FAssetDurableSessionRef& Session,
		IAssetDurableBackend& Backend,
		const int64 CurrentFencingToken,
		TFunctionRef<bool()> ConfirmMemoryRollback,
		FUeAgentResourceTracker& Resources,
		FDurableReceipt& OutReceipt,
		FAssetDurableError& OutError)
	{
		OutReceipt = FDurableReceipt();
		OutError = FAssetDurableError();
		if (!TryCheckFence(
			Session,
			CurrentFencingToken,
			OutError))
		{
			return false;
		}
		FBoundTerminalEvidence TerminalEvidence;
		if (!TryReadBoundCompletedTerminal(Session, Resources, TerminalEvidence, OutError))
			return false;
		if (TerminalEvidence.bPresent)
		{
			// Persist can already have restored and archived the disk state. Confirm
			// that same state and restore native memory without publishing it again.
			if (TerminalEvidence.Phase != TEXT("rolled_back")
				&& TerminalEvidence.Phase != TEXT("reconciled_original"))
			{
				OutError = BackendFailure(TEXT("durable_journal_already_terminal"), TEXT("Rollback requires completed evidence in the rolled_back or reconciled_original phase."), FString());
				OutError.PropertyPath = TEXT("phase");
				OutError.ActualValue = MakeShared<FJsonValueString>(TerminalEvidence.Phase);
				return false;
			}
			TArray<FDurableArtifactState> ObservedStates;
			if (!TryCaptureArtifactStates(Session->GetInventory(), TEXT("/c8d/rollback/terminal_state"),
				Resources, ObservedStates, OutError)) return false;
			OutError = CompareArtifactStates(Session->GetInventory(), Session->GetPreMutationStates(), ObservedStates, TEXT("original_state"));
			if (!OutError.IsEmpty()) return false;
			if (!ConfirmMemoryRollback())
			{
				OutError = BackendFailure(TEXT("durable_reconcile_readback_unconfirmed"), TEXT("The original file state was confirmed, but restoration of the in-memory state was not confirmed."), FString());
				return false;
			}
			OutReceipt = FDurableReceipt(Session->GetJournalId(),
				EDurableStatus::RolledBack,
				Session->GetRequest().GetPlan()->GetFingerprint(),
				Session->GetInventory().GetInventoryHash(),
				TerminalEvidence.PhaseHistory,
				ERecoveryAction::RestoredOriginal,
				false,
				false,
				UE::UAI::Contract::FDiagnostic(),
				FString(),
				Session->GetPreMutationStates(),
				Session->GetExpectedPersistedStates());
			return true;
		}
		bool bDurableConfirmed = false;
		FAssetDurableError BackendError;
		const bool bRestored = Backend.Restore(
				Session,
				Resources,
				bDurableConfirmed,
				BackendError);
		const bool bMemoryConfirmed = bRestored && bDurableConfirmed && ConfirmMemoryRollback();
		if (!bMemoryConfirmed)
		{
			if (BackendError.IsEmpty())
			{
				BackendError = FAssetDurableError::Failure(TEXT("durable_rollback_unconfirmed"), !bRestored
					? TEXT("The durability backend failed to restore the original state without reporting a diagnostic.")
					: !bDurableConfirmed ? TEXT("The durability backend did not confirm restoration of the original stored state.")
					: TEXT("The stored state was restored, but restoration of the in-memory state was not confirmed."), TEXT("asset_durable"));
				BackendError.SourceKind = TEXT("native_result");
			}
			BackendError.RollbackAttempted = true;
			BackendError.RollbackSucceeded = false;
			BackendError.bRetrySafe = false;
			return TryRecordUnknown(
				Session,
				Resources,
				false,
				OutReceipt,
				OutError,
				MoveTemp(BackendError));
		}
		const FAssetDurableSessionRef RolledBack =
			Advance(Session, TEXT("rolled_back"));
		if (!TryWriteJournal(RolledBack, Resources, OutError)
			|| !TryArchiveTerminal(
				RolledBack,
				Resources,
				OutError))
		{
			return false;
		}
		OutReceipt = Receipt(
			RolledBack,
			EDurableStatus::RolledBack,
			ERecoveryAction::RestoredOriginal,
			false,
			false);
		return true;
	}

	bool TryAcknowledgeAssetDurability(
		const FAssetDurableSessionRef& Session,
		const FDurableVerificationAcknowledgement& Acknowledgement,
		const int64 CurrentFencingToken,
		FUeAgentResourceTracker& Resources,
		FAssetDurableError& OutError)
	{
		OutError = FAssetDurableError();
		if (const FAssetDurableError* AcknowledgementError = Acknowledgement.GetValidationDiagnostic())
		{
			OutError = *AcknowledgementError;
			return false;
		}
		if (!TryCheckFence(
				Session,
				CurrentFencingToken,
				OutError))
		{
			return false;
		}
		if (Session->GetCurrentPhase()
			!= TEXT("persisted_unverified"))
		{
			OutError = BackendFailure(TEXT("durable_acknowledgement_phase_invalid"), TEXT("Durability acknowledgement requires the persisted_unverified phase."), FString());
			OutError.PropertyPath = TEXT("phase");
			OutError.ActualValue = MakeShared<FJsonValueString>(Session->GetCurrentPhase());
			OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("persisted_unverified"));
			return false;
		}
		const auto MatchesBinding = [&](const TCHAR* Field, const FString& Actual, const FString& Expected)
		{
			if (Actual == Expected) return true;
			OutError = BackendFailure(TEXT("durable_acknowledgement_binding_mismatch"), FString::Printf(TEXT("The durability acknowledgement %s differs from the current session."), Field), FString());
			OutError.PropertyPath = Field;
			OutError.ActualEvidence = MakeShared<FJsonValueString>(Actual);
			OutError.ExpectedEvidence = MakeShared<FJsonValueString>(Expected);
			return false;
		};
		if (!MatchesBinding(TEXT("journal_id"), Acknowledgement.GetJournalId(), Session->GetJournalId())
			|| !MatchesBinding(TEXT("plan_fingerprint"), Acknowledgement.GetPlanFingerprint(), Session->GetRequest().GetPlan()->GetFingerprint())
			|| !MatchesBinding(TEXT("inventory_hash"), Acknowledgement.GetInventoryHash(), Session->GetInventory().GetInventoryHash())
			|| !MatchesBinding(TEXT("journal_owner_id"), Acknowledgement.GetJournalOwnerId(), Session->GetRequest().GetJournalOwnerId())
			|| !MatchesBinding(TEXT("fencing_token"), FString::Printf(TEXT("%lld"), Acknowledgement.GetFencingToken()), FString::Printf(TEXT("%lld"), CurrentFencingToken))) return false;
		const FAssetDurableSessionRef Verified =
			Advance(Session, Acknowledgement.IsSemanticVerified()
				&& !UeAgentAuthoringDiagnostics::HasConsistencyWarnings() ? TEXT("verified") : TEXT("committed"));
		if (!TryWriteJournal(Verified, Resources, OutError)
			|| !TryArchiveTerminal(
				Verified,
				Resources,
				OutError))
		{
			return false;
		}
		return true;
	}

	bool TryRecoverAssetDurability(
		const FString& JournalRoot,
		IAssetDurableBackend& Backend,
		const int64 CurrentFencingToken,
		const FString& CurrentJournalOwnerId,
		FUeAgentResourceTracker& Resources,
		TArray<FDurableReceipt>& OutReceipts,
		FAssetDurableError& OutError)
	{
		OutReceipts.Reset();
		OutError = FAssetDurableError();
		const FString OwnerId =
			NormalizedText(CurrentJournalOwnerId);
		if (OwnerId.IsEmpty())
		{
			OutError = BackendFailure(TEXT("durable_journal_owner_invalid"), TEXT("Durability recovery requires a non-empty current journal owner."), FString());
			OutError.PropertyPath = TEXT("journal_owner_id");
			return false;
		}
		const FString Root = FullPath(JournalRoot);
		const TArray<FString> AllowedArtifactRoots =
			Backend.GetAllowedArtifactRoots();
		TArray<FString> JournalPaths;
		if (!TryEnumerateJournals(
			Root / TEXT("active"),
			Resources,
			JournalPaths,
			OutError))
		{
			return false;
		}
		for (const FString& JournalPath : JournalPaths)
		{
			FAssetDurableSessionPtr Session;
			FAssetDurableError ItemError;
			const bool bSessionLoaded = TryLoadSession(
				JournalPath,
				Root,
				AllowedArtifactRoots,
				Resources,
				Session,
				ItemError);
			if (!bSessionLoaded || !Session.IsValid())
			{
				if (ItemError.IsEmpty())
				{
					ItemError = JournalFailure(bSessionLoaded
						? TEXT("The durability journal loader reported success without producing a session.")
						: TEXT("The durability journal loader failed without returning a diagnostic."), TEXT("session"));
					ItemError.InternalContext = JournalPath;
				}
				if (IsResourceChargeError(ItemError))
				{
					OutError = ItemError;
					return false;
				}
				OutReceipts.Add(IsolationReceipt(JournalPath, ItemError));
				continue;
			}
			const FAssetDurableSessionRef Ref =
				Session.ToSharedRef();
			if (!TryCheckFence(
					Ref,
					CurrentFencingToken,
					ItemError))
			{
				OutReceipts.Emplace(
					Ref->GetJournalId(),
					EDurableStatus::StateUnknown,
					Ref->GetRequest().GetPlan()->GetFingerprint(),
					Ref->GetInventory().GetInventoryHash(),
					Ref->GetPhaseHistory(),
					ERecoveryAction::ManualIntervention,
					false,
					true,
					ItemError);
				continue;
			}
			if (Ref->GetRequest().GetJournalOwnerId() != OwnerId)
			{
				ItemError = BackendFailure(TEXT("durable_journal_owner_mismatch"), TEXT("The durability journal owner differs from the owner performing recovery."), FString());
				ItemError.PropertyPath = TEXT("journal_owner_id");
				ItemError.ActualEvidence = MakeShared<FJsonValueString>(Ref->GetRequest().GetJournalOwnerId());
				ItemError.ExpectedEvidence = MakeShared<FJsonValueString>(OwnerId);
				OutReceipts.Emplace(
					Ref->GetJournalId(),
					EDurableStatus::StateUnknown,
					Ref->GetRequest().GetPlan()->GetFingerprint(),
					Ref->GetInventory().GetInventoryHash(),
					Ref->GetPhaseHistory(),
					ERecoveryAction::ManualIntervention,
					false,
					true,
					ItemError);
				continue;
			}
			const FString& Phase = Ref->GetCurrentPhase();
			FDurableReceipt Current;
			bool bProcessed = true;
			if (Phase == TEXT("inventory_ready")
				|| Phase == TEXT("backups_ready")
				|| Phase == TEXT("prepared")
				|| Phase == TEXT("inventory_extended"))
			{
				const FAssetDurableSessionRef RolledBack =
					Advance(Ref, TEXT("rolled_back"));
				if (!TryWriteJournal(
					RolledBack,
					Resources,
					ItemError)
					|| !TryArchiveTerminal(
						RolledBack,
						Resources,
						ItemError))
				{
					bProcessed = false;
				}
				else
				{
					Current = Receipt(
						RolledBack,
						EDurableStatus::RolledBack,
						ERecoveryAction::AbortedBeforeMutation,
						false,
						true);
				}
			}
			else if (Phase == TEXT("staged") || Phase == TEXT("native_mutating"))
			{
				if (!TryRestoreAfterFailure(
					Ref,
					Backend,
					Resources,
					true,
					Current,
					ItemError))
				{
					bProcessed = false;
				}
			}
			else if (Phase == TEXT("published")
				|| Phase == TEXT("persisted_unverified"))
			{
				FString Observed;
				FAssetDurableError BackendError;
				const bool bInspected = Backend.Inspect(
					Ref,
					Resources,
					Observed,
					BackendError);
				if (!bInspected || Observed != TEXT("persisted"))
				{
					if (BackendError.IsEmpty())
					{
						BackendError = FAssetDurableError::Failure(TEXT("durable_recovery_inspection_unconfirmed"), bInspected
							? TEXT("The durability backend did not observe the expected persisted state during recovery.")
							: TEXT("Durability inspection failed without reporting a diagnostic."), TEXT("asset_durable"));
						BackendError.SourceKind = TEXT("native_result");
						if (bInspected)
						{
							BackendError.PropertyPath = TEXT("observed_state");
							BackendError.ActualValue = MakeShared<FJsonValueString>(Observed);
							BackendError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("persisted"));
						}
					}
					if (!TryRecordUnknown(
						Ref,
						Resources,
						true,
						Current,
						ItemError,
						MoveTemp(BackendError)))
					{
						bProcessed = false;
					}
				}
				else
				{
					const FAssetDurableSessionRef Recovery =
						Advance(
							Ref,
							TEXT("recovery_required"));
					if (!TryWriteJournal(
						Recovery,
						Resources,
						ItemError))
					{
						bProcessed = false;
					}
					else
					{
						Current = Receipt(
							Recovery,
							EDurableStatus::RecoveryRequired,
							ERecoveryAction::VerifyPersisted,
							true,
							true);
					}
				}
			}
			else if (Phase == TEXT("recovery_required"))
			{
				Current = Receipt(
					Ref,
					EDurableStatus::RecoveryRequired,
					ERecoveryAction::VerifyPersisted,
					true,
					true);
			}
			else if (Phase == TEXT("state_unknown"))
			{
				Current = Receipt(
					Ref,
					EDurableStatus::StateUnknown,
					ERecoveryAction::ManualIntervention,
					false,
					true);
			}
			else
			{
				ItemError = JournalFailure(TEXT("The recorded durability phase is not supported by recovery."), TEXT("phase"));
				ItemError.ActualValue = MakeShared<FJsonValueString>(Phase);
				bProcessed = false;
			}
			if (!bProcessed)
			{
				if (IsResourceChargeError(ItemError))
				{
					OutError = ItemError;
					return false;
				}
				OutReceipts.Emplace(
					Ref->GetJournalId(),
					EDurableStatus::StateUnknown,
					Ref->GetRequest().GetPlan()->GetFingerprint(),
					Ref->GetInventory().GetInventoryHash(),
					Ref->GetPhaseHistory(),
					ERecoveryAction::ManualIntervention,
					false,
					true,
					ItemError);
				continue;
			}
			OutReceipts.Add(MoveTemp(Current));
		}
		return true;
	}

	bool TryInspectAssetDurableJournal(
		const FString& JournalRoot,
		const FString& JournalId,
		const TArray<FString>& AllowedArtifactRoots,
		FUeAgentResourceTracker& Resources,
		FAssetDurableSessionPtr& OutSession,
		TArray<FDurableArtifactState>& OutObservedStates,
		FAssetDurableError& OutError)
	{
		OutSession.Reset();
		OutObservedStates.Reset();
		OutError = FAssetDurableError();
		const FString NormalizedJournalId = NormalizedText(JournalId);
		FGuid ParsedJournalId;
		if (!FGuid::ParseExact(
				NormalizedJournalId,
				EGuidFormats::Digits,
				ParsedJournalId))
		{
			OutError = BackendFailure(TEXT("durable_journal_id_invalid"), TEXT("The durability journal identifier must contain exactly 32 hexadecimal digits."), FString());
			OutError.PropertyPath = TEXT("journal_id");
			OutError.ActualEvidence = MakeShared<FJsonValueString>(NormalizedJournalId);
			return false;
		}

		const FString Root = FullPath(JournalRoot);
		const FString JournalPath = Root
			/ TEXT("active")
			/ NormalizedJournalId
			/ TEXT("journal.json");
		if (!IsWithin(JournalPath, Root / TEXT("active")))
		{
			OutError = BackendFailure(TEXT("durable_journal_root_invalid"), TEXT("The durability journal inspection path is outside the active journal root."), FString());
			OutError.InternalContext = JournalPath;
			return false;
		}
		FAssetDurableSessionPtr Loaded;
		const bool bLoaded = TryLoadSession(
				JournalPath,
				Root,
				AllowedArtifactRoots,
				Resources,
				Loaded,
				OutError);
		if (!bLoaded || !Loaded.IsValid())
		{
			if (OutError.IsEmpty())
			{
				OutError = JournalFailure(bLoaded
					? TEXT("The durability journal loader reported success without producing a session.")
					: TEXT("The durability journal loader failed without returning a diagnostic."), TEXT("session"));
				OutError.InternalContext = JournalPath;
			}
			return false;
		}
		if (Loaded->GetJournalId() != NormalizedJournalId)
		{
			OutError = BackendFailure(TEXT("durable_journal_identity_mismatch"), TEXT("The loaded durability journal identifier differs from the requested identifier."), FString());
			OutError.PropertyPath = TEXT("journal_id");
			OutError.ActualEvidence = MakeShared<FJsonValueString>(Loaded->GetJournalId());
			OutError.ExpectedEvidence = MakeShared<FJsonValueString>(NormalizedJournalId);
			return false;
		}

		TArray<FDurableArtifactState> PreMutationStates;
		if (!TryResolvePreMutationStates(
				Loaded.ToSharedRef(),
				Resources,
				PreMutationStates,
				OutError)
			|| !TryCaptureArtifactStates(
				Loaded->GetInventory(),
				TEXT("/c8d/inspect/observed_state"),
				Resources,
				OutObservedStates,
				OutError))
		{
			OutObservedStates.Reset();
			return false;
		}
		OutSession = MakeShared<const FAssetDurableSession>(
			Loaded->GetRequest(),
			Loaded->GetInventory(),
			Loaded->GetJournalId(),
			Loaded->GetJournalPath(),
			Loaded->GetJournalRoot(),
			Loaded->GetTransactionRoot(),
			Loaded->GetStagingRoot(),
			Loaded->GetPhaseHistory(),
			MoveTemp(PreMutationStates),
			Loaded->GetExpectedPersistedStates());
		return true;
	}

	bool TryReconcileAssetDurability(
		const FString& JournalRoot,
		const FString& JournalId,
		const TArray<FString>& AllowedArtifactRoots,
		const FString& ExpectedJournalOwnerId,
		const int64 ExpectedFencingToken,
		const FString& VerificationId,
		IAssetDurableReconciliationOracle& ReconciliationOracle,
		FUeAgentResourceTracker& Resources,
		FDurableReceipt& OutReceipt,
		FAssetDurableError& OutError)
	{
		OutReceipt = FDurableReceipt();
		OutError = FAssetDurableError();
		const FString NormalizedJournalId = NormalizedText(JournalId);
		const FString OwnerId = NormalizedText(ExpectedJournalOwnerId);
		const FString BoundVerificationId = NormalizedText(VerificationId);
		if (OwnerId.IsEmpty()
			|| ExpectedFencingToken < 0
			|| BoundVerificationId.IsEmpty())
		{
			OutError = BackendFailure(TEXT("durable_reconcile_request_invalid"), OwnerId.IsEmpty()
				? TEXT("Durability reconciliation requires a journal owner.")
				: ExpectedFencingToken < 0 ? TEXT("The durability reconciliation fencing token must not be negative.")
				: TEXT("Durability reconciliation requires a verification identifier."), FString());
			OutError.PropertyPath = OwnerId.IsEmpty() ? TEXT("journal_owner_id") : ExpectedFencingToken < 0 ? TEXT("fencing_token") : TEXT("verification_id");
			if (!OwnerId.IsEmpty() && ExpectedFencingToken < 0) OutError.ActualEvidence = MakeShared<FJsonValueString>(FString::Printf(TEXT("%lld"), ExpectedFencingToken));
			return false;
		}

		FAssetDurableSessionPtr Loaded;
		TArray<FDurableArtifactState> ObservedStates;
		if (!TryInspectAssetDurableJournal(
				JournalRoot,
				NormalizedJournalId,
				AllowedArtifactRoots,
				Resources,
				Loaded,
				ObservedStates,
				OutError))
		{
			return false;
		}
		FAssetDurableSessionRef Session = Loaded.ToSharedRef();
		const auto MatchesBinding = [&](const TCHAR* Field, const FString& Actual, const FString& Expected)
		{
			if (Actual == Expected) return true;
			OutError = BackendFailure(TEXT("durable_reconcile_binding_mismatch"), FString::Printf(TEXT("The loaded journal %s differs from the reconciliation request."), Field), FString());
			OutError.PropertyPath = Field;
			OutError.ActualEvidence = MakeShared<FJsonValueString>(Actual);
			OutError.ExpectedEvidence = MakeShared<FJsonValueString>(Expected);
			return false;
		};
		if (!MatchesBinding(TEXT("journal_id"), Session->GetJournalId(), NormalizedJournalId)
			|| !MatchesBinding(TEXT("journal_owner_id"), Session->GetRequest().GetJournalOwnerId(), OwnerId)
			|| !MatchesBinding(TEXT("fencing_token"), FString::Printf(TEXT("%lld"), Session->GetRequest().GetFencingToken()), FString::Printf(TEXT("%lld"), ExpectedFencingToken))) return false;
		const FString& Phase = Session->GetCurrentPhase();
		const bool bPrePublishPhase = IsPrePublishPhase(Phase);
		if (!bPrePublishPhase
			&& Phase != TEXT("published")
			&& Phase != TEXT("persisted_unverified")
			&& Phase != TEXT("recovery_required")
			&& Phase != TEXT("state_unknown"))
		{
			OutError = BackendFailure(TEXT("durable_reconcile_phase_invalid"), TEXT("Durability reconciliation requires a pre-publication phase, published, persisted_unverified, recovery_required, or state_unknown."), FString());
			OutError.PropertyPath = TEXT("phase");
			OutError.ActualValue = MakeShared<FJsonValueString>(Phase);
			return false;
		}

		FBoundTerminalEvidence TerminalEvidence;
		if (!TryReadBoundCompletedTerminal(
				Session,
				Resources,
				TerminalEvidence,
				OutError))
		{
			return false;
		}
		if (TerminalEvidence.bPresent)
		{
			const EDurableStatus ProposedStatus =
				TerminalEvidence.Phase == TEXT("verified")
					? EDurableStatus::Verified
					: TerminalEvidence.Phase == TEXT("committed") ? EDurableStatus::Committed : EDurableStatus::RolledBack;
			if (!ReconciliationOracle.TryConfirm(
					Session,
					ObservedStates,
					ProposedStatus,
					Resources,
					OutError))
			{
				if (!(!OutError.IsEmpty()))
				{
					OutError = BackendFailure(TEXT("durable_reconcile_readback_unconfirmed"), TEXT("The reconciliation provider did not confirm the completed journal state and returned no diagnostic."), FString());
					OutError.ExpectedValue = MakeShared<FJsonValueString>(LexToString(ProposedStatus));
				}
				return false;
			}
			const FAssetDurableSessionRef Terminal =
				Advance(Session, TerminalEvidence.Phase);
			if (!TryWriteJournal(Terminal, Resources, OutError)
				|| !TryArchiveTerminal(
					Terminal,
					Resources,
					OutError))
			{
				return false;
			}
			const FString PriorTerminalPhase =
				TerminalEvidence.PhaseHistory.Num() > 1
					? TerminalEvidence.PhaseHistory[
						TerminalEvidence.PhaseHistory.Num() - 2]
					: FString();
			const ERecoveryAction RecoveryAction =
				(TerminalEvidence.Phase == TEXT("verified") || TerminalEvidence.Phase == TEXT("committed"))
					? ERecoveryAction::VerifyPersisted
					: TerminalEvidence.Phase == TEXT("rolled_back")
						&& IsPrePublishPhase(PriorTerminalPhase)
							? ERecoveryAction::AbortedBeforeMutation
							: ERecoveryAction::RestoredOriginal;
			OutReceipt = FDurableReceipt(Terminal->GetJournalId(),
				ProposedStatus,
				Terminal->GetRequest().GetPlan()->GetFingerprint(),
				Terminal->GetInventory().GetInventoryHash(),
				Terminal->GetPhaseHistory(),
				RecoveryAction,
				false,
				false,
				UE::UAI::Contract::FDiagnostic(),
				BoundVerificationId,
				Terminal->GetPreMutationStates(),
				Terminal->GetExpectedPersistedStates());
			return true;
		}

		const FAssetDurableError OriginalDifference = CompareArtifactStates(
			Session->GetInventory(),
			Session->GetPreMutationStates(),
			ObservedStates, TEXT("original_state"));
		const FAssetDurableError PersistedDifference = CompareArtifactStates(
			Session->GetInventory(),
			Session->GetExpectedPersistedStates(),
			ObservedStates, TEXT("persisted_state"));
		const bool bMatchesOriginal = OriginalDifference.IsEmpty();
		const bool bMatchesPersisted = PersistedDifference.IsEmpty();
		if ((!bPrePublishPhase && bMatchesOriginal == bMatchesPersisted)
			|| (bPrePublishPhase && !bMatchesOriginal))
		{
			const FString FailureCode = bMatchesOriginal
				? TEXT("durable_reconcile_state_ambiguous")
				: TEXT("durable_reconcile_evidence_insufficient");
			FAssetDurableError Difference = BackendFailure(*FailureCode, bMatchesOriginal
				? TEXT("The observed artifacts match both the original and persisted states, so recovery cannot identify a unique outcome.")
				: bPrePublishPhase ? TEXT("The observed artifacts do not establish the original state required before publication.")
				: TEXT("The observed artifacts establish neither the original nor the persisted state."), FString());
			Difference.Append(OriginalDifference);
			if (!bPrePublishPhase) Difference.Append(PersistedDifference);
			const FAssetDurableSessionRef Unknown =
				Advance(Session, TEXT("state_unknown"));
			if (!TryWriteJournal(Unknown, Resources, OutError))
			{
				Difference.Append(OutError);
				OutError = MoveTemp(Difference);
				return false;
			}
			OutReceipt = FDurableReceipt(Unknown->GetJournalId(),
				EDurableStatus::StateUnknown,
				Unknown->GetRequest().GetPlan()->GetFingerprint(),
				Unknown->GetInventory().GetInventoryHash(),
				Unknown->GetPhaseHistory(),
				ERecoveryAction::ManualIntervention,
				false,
				false,
				MoveTemp(Difference),
				BoundVerificationId,
				Unknown->GetPreMutationStates(),
				Unknown->GetExpectedPersistedStates());
			return true;
		}

		const FString TerminalPhase = bMatchesOriginal
			? bPrePublishPhase
				? TEXT("rolled_back")
				: TEXT("reconciled_original")
			: TEXT("verified");
		const EDurableStatus ProposedStatus = bMatchesOriginal
			? EDurableStatus::RolledBack
			: EDurableStatus::Verified;
		if (!ReconciliationOracle.TryConfirm(
				Session,
				ObservedStates,
				ProposedStatus,
				Resources,
				OutError))
		{
			if (!(!OutError.IsEmpty()))
			{
				OutError = BackendFailure(TEXT("durable_reconcile_readback_unconfirmed"), TEXT("The reconciliation provider did not confirm the proposed recovered state and returned no diagnostic."), FString());
				OutError.ExpectedValue = MakeShared<FJsonValueString>(LexToString(ProposedStatus));
			}
			return false;
		}
		const FAssetDurableSessionRef Terminal =
			Advance(Session, TerminalPhase);
		if (!TryWriteJournal(Terminal, Resources, OutError)
			|| !TryArchiveTerminal(Terminal, Resources, OutError))
		{
			return false;
		}
		OutReceipt = FDurableReceipt(Terminal->GetJournalId(),
				ProposedStatus,
				Terminal->GetRequest().GetPlan()->GetFingerprint(),
				Terminal->GetInventory().GetInventoryHash(),
				Terminal->GetPhaseHistory(),
				bMatchesOriginal
				? bPrePublishPhase
					? ERecoveryAction::AbortedBeforeMutation
					: ERecoveryAction::RestoredOriginal
				: ERecoveryAction::VerifyPersisted,
				false,
				false,
				UE::UAI::Contract::FDiagnostic(),
				BoundVerificationId,
				Terminal->GetPreMutationStates(),
				Terminal->GetExpectedPersistedStates());
		return true;
	}
}

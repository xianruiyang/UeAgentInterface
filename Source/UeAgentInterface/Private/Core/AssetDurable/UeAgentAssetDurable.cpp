#include "Core/AssetDurable/UeAgentAssetDurable.h"
#include "Core/AssetDurable/UeAgentPackageHeader.h"

#include "Core/Contract/UeAgentContract.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/LinkerLoad.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectGlobals.h"

#include <openssl/sha.h>

namespace UE::UAI::AssetDurable
{
	using UE::UAI::ContextResource::EUeAgentResourceDimension;
	using UE::UAI::ContextResource::FUeAgentResourceBudgetExceeded;
	using UE::UAI::Contract::FUeAgentDescriptorIdentity;
	using UE::UAI::PlanningPreflight::FChangeSet;
	using UE::UAI::PlanningPreflight::FImpactSet;
	using UE::UAI::PlanningPreflight::FPlannedChange;
	using UE::UAI::PlanningPreflight::FPlanningPreflightPlan;
	using UE::UAI::StateSemantic::FUeAgentStateRevision;

	namespace
	{
		const TCHAR* JournalSchemaV1 =
			TEXT("uai.c8d.asset_durable.journal.v1");
		const TCHAR* JournalSchemaV2 =
			TEXT("uai.c8d.asset_durable.journal.v2");

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

		bool AreSameArtifact(
			const FDurableArtifact& Left,
			const FDurableArtifact& Right)
		{
			return Left.GetArtifactId() == Right.GetArtifactId()
				&& Left.GetTargetIdentity().GetCanonicalKey()
					== Right.GetTargetIdentity().GetCanonicalKey()
				&& Left.GetArtifactKind() == Right.GetArtifactKind()
				&& Left.GetDurableLocation()
					== Right.GetDurableLocation()
				&& Left.GetFinalPath() == Right.GetFinalPath()
				&& Left.GetIntent() == Right.GetIntent()
				&& Left.GetRole() == Right.GetRole();
		}

		bool FilesEqual(
			const FString& LeftPath,
			const FString& RightPath)
		{
			TArray<uint8> Left;
			TArray<uint8> Right;
			return FFileHelper::LoadFileToArray(Left, *LeftPath)
				&& FFileHelper::LoadFileToArray(Right, *RightPath)
				&& Left == Right;
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

		FAssetDurableError ResourceError(
			const FUeAgentResourceBudgetExceeded& Error)
		{
			return FAssetDurableError(
				Error.Code.IsEmpty()
					? TEXT("resource_budget_exceeded")
					: Error.Code,
				FString::Printf(
					TEXT("%s exceeded at %s: observed=%lld limit=%lld"),
					*UE::UAI::ContextResource::LexToString(
						Error.Dimension),
					*Error.Path,
					Error.Observed,
					Error.Limit));
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
			FUeAgentResourceBudgetExceeded Error;
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
				|| Phase == TEXT("published")
				|| Phase == TEXT("persisted_unverified")
				|| Phase == TEXT("verified")
				|| Phase == TEXT("rolled_back")
				|| Phase == TEXT("recovery_required")
				|| Phase == TEXT("state_unknown");
		}

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
				MoveTemp(History));
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

		bool TryParseIdentity(
			const TSharedPtr<FJsonObject>& Object,
			FUeAgentCanonicalIdentity& OutIdentity,
			FAssetDurableError& OutError)
		{
			FString SemanticId;
			FString TargetKind;
			FString OwnerId;
			FString Selector;
			FString ObjectRole;
			FString Generation;
			if (!Object.IsValid()
				|| !Object->TryGetStringField(
					TEXT("semantic_id"),
					SemanticId)
				|| !Object->TryGetStringField(
					TEXT("target_kind"),
					TargetKind)
				|| !Object->TryGetStringField(
					TEXT("owner_id"),
					OwnerId)
				|| !Object->TryGetStringField(
					TEXT("selector"),
					Selector)
				|| !Object->TryGetStringField(
					TEXT("object_role"),
					ObjectRole)
				|| !Object->TryGetStringField(
					TEXT("generation"),
					Generation))
			{
				OutError = FAssetDurableError(
					TEXT("durable_journal_integrity_invalid"),
					TEXT("artifact identity is incomplete"));
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
				OutError = FAssetDurableError(
					TEXT("durable_journal_integrity_invalid"),
					TEXT("artifact identity is invalid"));
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

		TSharedRef<FJsonObject> BuildPayload(
			const FAssetDurableSessionRef& Session)
		{
			const TSharedRef<FJsonObject> Payload =
				MakeShared<FJsonObject>();
			Payload->SetStringField(
				TEXT("schema"),
				JournalSchemaV2);
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
				OutError = FAssetDurableError(
					TEXT("durable_journal_root_invalid"));
				return false;
			}
			FString PayloadText;
			if (!TrySerializeObject(BuildPayload(Session), PayloadText))
			{
				OutError = FAssetDurableError(
					TEXT("durable_journal_serialize_failed"));
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
				OutError = FAssetDurableError(
					TEXT("durable_journal_serialize_failed"));
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
				FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)
				|| !IFileManager::Get().Move(
					*Session->GetJournalPath(),
					*Temporary,
					true,
					true,
					false,
					true))
			{
				IFileManager::Get().Delete(*Temporary);
				OutError = FAssetDurableError(
					TEXT("durable_journal_write_failed"),
					Session->GetJournalPath());
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
				OutError = FAssetDurableError(
					TEXT("durable_target_not_in_plan"),
					IdentityKey);
				return false;
			}
			const bool bTargetDeletes =
				Target.GetIntent() == EAssetDurableIntent::Delete;
			const bool bPlanDeletes =
				PlannedChange->GetIntent()
				== UE::UAI::PlanningPreflight::EChangeIntent::Delete;
			if (bTargetDeletes != bPlanDeletes)
			{
				OutError = FAssetDurableError(
					TEXT("durable_intent_plan_mismatch"),
					IdentityKey);
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
				OutError = FAssetDurableError(
					TEXT("durable_request_invalid"));
				return false;
			}
			TSet<FString> Seen;
			TSet<FString> Owners;
			for (const FAssetDurableTarget& Target :
				Request.GetTargets())
			{
				const FString Key = Target.GetDurableTargetKey();
				if (!TryValidateTargetAgainstPlan(
						Request.GetPlan(),
						Target,
						OutError))
				{
					return false;
				}
				if (Seen.Contains(Key))
				{
					OutError = FAssetDurableError(
						TEXT("durable_target_duplicate"),
						Key);
					return false;
				}
				Seen.Add(Key);
				Owners.Add(
					LexToString(Target.GetArtifactKind())
						+ TEXT(":")
						+ Target.GetDurableLocation());
				if (Target.GetRollbackGrade() == ERollbackGrade::None)
				{
					OutError = FAssetDurableError(
						TEXT("durable_rollback_grade_unsupported"),
						Key);
					return false;
				}
			}
			if (Request.GetAtomicityMode()
					== EAtomicityMode::SingleOwnerAtomic
				&& Owners.Num() != 1)
			{
				OutError = FAssetDurableError(
					TEXT("durable_atomicity_mismatch"));
				return false;
			}
			if (Request.GetAtomicityMode()
				== EAtomicityMode::NonAtomicRejected)
			{
				OutError = FAssetDurableError(
					TEXT("durable_atomicity_mismatch"));
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
			if (!Inventory.IsValid() || RawAllowedRoots.IsEmpty())
			{
				OutError = FAssetDurableError(
					TEXT("durable_inventory_invalid"));
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
				OutError = FAssetDurableError(
					TEXT("durable_artifact_root_unavailable"));
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
			for (const FDurableArtifact& Artifact :
				Inventory.GetArtifacts())
			{
				const FString Key =
					LexToString(Artifact.GetArtifactKind())
					+ TEXT(":")
					+ Artifact.GetDurableLocation().ToLower();
				const FAssetDurableTarget* const* TargetPtr =
					Targets.Find(Key);
				if (!Artifact.IsValid()
					|| !TargetPtr
					|| (*TargetPtr)->GetIdentity().GetCanonicalKey()
						!= Artifact.GetTargetIdentity()
							.GetCanonicalKey()
					|| (*TargetPtr)->GetArtifactKind()
						!= Artifact.GetArtifactKind()
					|| (*TargetPtr)->GetDurableLocation()
						!= Artifact.GetDurableLocation()
					|| (*TargetPtr)->GetIntent()
						!= Artifact.GetIntent()
					|| Seen.Contains(Artifact.GetArtifactId())
					|| (!PreviousId.IsEmpty()
						&& Artifact.GetArtifactId() < PreviousId))
				{
					OutError = FAssetDurableError(
						TEXT("durable_inventory_invalid"),
						Artifact.GetArtifactId());
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
					OutError = FAssetDurableError(
						TEXT("durable_artifact_path_invalid"),
						Artifact.GetFinalPath());
					return false;
				}
				Seen.Add(Artifact.GetArtifactId());
				PreviousId = Artifact.GetArtifactId();
			}
			FString ExpectedHash;
			if (!TryComputeDurableInventoryHash(
					Inventory.GetArtifacts(),
					Resources,
					ExpectedHash,
					OutError)
				|| Inventory.GetInventoryHash() != ExpectedHash)
			{
				if (!OutError.IsPresent())
				{
					OutError = FAssetDurableError(
						TEXT("durable_inventory_invalid"),
						TEXT("inventory hash mismatch"));
				}
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
				OutError = FAssetDurableError(
					TEXT("durable_fence_stale"));
				return false;
			}
			return true;
		}

		FDurableReceipt Receipt(
			const FAssetDurableSessionRef& Session,
			const EDurableStatus Status,
			const ERecoveryAction Action,
			const bool bRequiresIndependent,
			const bool bReplayed)
		{
			return FDurableReceipt(
				Session->GetJournalId(),
				Status,
				Session->GetRequest().GetPlan()->GetFingerprint(),
				Session->GetInventory().GetInventoryHash(),
				Session->GetPhaseHistory(),
				Action,
				bRequiresIndependent,
				bReplayed);
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
			if (!IsWithin(CompletedPath, CompletedRoot)
				|| !IsWithin(
					Session->GetTransactionRoot(),
					Session->GetJournalRoot() / TEXT("active")))
			{
				OutError = FAssetDurableError(
					TEXT("durable_journal_root_invalid"));
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
			if (IFileManager::Get().Copy(
				*CompletedPath,
				*Session->GetJournalPath(),
				true,
				true)
				!= COPY_OK
				|| !IFileManager::Get().DeleteDirectory(
					*Session->GetTransactionRoot(),
					false,
					true))
			{
				OutError = FAssetDurableError(
					TEXT("durable_journal_archive_failed"));
				return false;
			}
			return true;
		}

		bool TryRecordUnknown(
			const FAssetDurableSessionRef& Session,
			FUeAgentResourceTracker& Resources,
			const bool bReplayed,
			FDurableReceipt& OutReceipt,
			FAssetDurableError& OutError)
		{
			const FAssetDurableSessionRef Unknown =
				Advance(Session, TEXT("state_unknown"));
			if (!TryWriteJournal(Unknown, Resources, OutError))
			{
				return false;
			}
			OutReceipt = Receipt(
				Unknown,
				EDurableStatus::StateUnknown,
				ERecoveryAction::ManualIntervention,
				false,
				bReplayed);
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
			if (!Backend.Restore(
				Session,
				Resources,
				bConfirmed,
				RestoreError)
				|| !bConfirmed)
			{
				return TryRecordUnknown(
					Session,
					Resources,
					bReplayed,
					OutReceipt,
					OutError);
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

		bool TryReadOuterJournal(
			const FString& JournalPath,
			FUeAgentResourceTracker& Resources,
			FString& OutPayloadText,
			FAssetDurableError& OutError)
		{
			FString Encoded;
			if (!FFileHelper::LoadFileToString(Encoded, *JournalPath))
			{
				OutError = FAssetDurableError(
					TEXT("durable_journal_integrity_invalid"),
					JournalPath);
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
			if (!FJsonSerializer::Deserialize(Reader, Outer)
				|| !Outer.IsValid()
				|| !Outer->TryGetStringField(
					TEXT("payload"),
					OutPayloadText)
				|| !Outer->TryGetStringField(
					TEXT("integrity_sha256"),
					Integrity)
				|| Integrity != Sha256(OutPayloadText))
			{
				OutError = FAssetDurableError(
					TEXT("durable_journal_integrity_invalid"),
					JournalPath);
				return false;
			}
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
			IAssetDurableBackend& Backend,
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
				OutError = FAssetDurableError(
					TEXT("durable_journal_integrity_invalid"),
					TEXT("payload is not valid JSON"));
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
			if (!Payload->TryGetStringField(TEXT("schema"), Schema)
				|| (Schema != JournalSchemaV1
					&& Schema != JournalSchemaV2)
				|| !Payload->TryGetStringField(
					TEXT("journal_id"),
					JournalId)
				|| !Payload->TryGetStringField(
					TEXT("journal_owner_id"),
					OwnerId)
				|| !Payload->TryGetNumberField(
					TEXT("fencing_token"),
					FencingTokenNumber)
				|| !Payload->TryGetStringField(
					TEXT("phase"),
					Phase)
				|| !Payload->TryGetStringField(
					TEXT("plan_fingerprint"),
					Fingerprint)
				|| !Payload->TryGetStringField(
					TEXT("inventory_hash"),
					InventoryHash)
				|| !Payload->TryGetStringField(
					TEXT("transaction_root"),
					TransactionRoot)
				|| !Payload->TryGetStringField(
					TEXT("staging_root"),
					StagingRoot)
				|| !Payload->TryGetArrayField(
					TEXT("phase_history"),
					HistoryValues)
				|| !Payload->TryGetArrayField(
					TEXT("artifacts"),
					ArtifactValues)
				|| !FMath::IsFinite(FencingTokenNumber)
				|| FencingTokenNumber < 0.0
				|| FMath::FloorToDouble(FencingTokenNumber)
					!= FencingTokenNumber)
			{
				OutError = FAssetDurableError(
					TEXT("durable_journal_integrity_invalid"),
					TEXT("required journal fields are invalid"));
				return false;
			}
			const bool bTypedJournal =
				Schema == JournalSchemaV2;
			TArray<FString> History;
			for (const TSharedPtr<FJsonValue>& Value : *HistoryValues)
			{
				FString Item;
				if (!Value.IsValid()
					|| !Value->TryGetString(Item)
					|| !IsKnownPhase(Item))
				{
					OutError = FAssetDurableError(
						TEXT("durable_journal_integrity_invalid"),
						TEXT("phase history is invalid"));
					return false;
				}
				History.Add(MoveTemp(Item));
			}
			if (History.IsEmpty()
				|| History.Last() != Phase
				|| !IsKnownPhase(Phase))
			{
				OutError = FAssetDurableError(
					TEXT("durable_journal_integrity_invalid"),
					TEXT("current phase is invalid"));
				return false;
			}
			const FString ExpectedTransaction =
				FullPath(FPaths::GetPath(JournalPath));
			if (!FullPath(TransactionRoot).Equals(
					ExpectedTransaction,
					ESearchCase::IgnoreCase)
				|| !IsWithin(
					TransactionRoot,
					JournalRoot / TEXT("active"))
				|| !IsWithin(StagingRoot, TransactionRoot))
			{
				OutError = FAssetDurableError(
					TEXT("durable_journal_integrity_invalid"),
					TEXT("transaction roots are invalid"));
				return false;
			}
			TArray<FString> AllowedRoots =
				Backend.GetAllowedArtifactRoots();
			if (AllowedRoots.IsEmpty())
			{
				OutError = FAssetDurableError(
					TEXT("durable_journal_integrity_invalid"),
					TEXT("artifact roots are unavailable"));
				return false;
			}
			TArray<FDurableArtifact> Artifacts;
			TMap<FString, FAssetDurableTarget> TargetsByKey;
			TSet<FString> ArtifactIds;
			FString PreviousArtifactId;
			for (const TSharedPtr<FJsonValue>& Value : *ArtifactValues)
			{
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
				const bool bHasTypedArtifact =
					bTypedJournal
					&& ArtifactObject.IsValid()
					&& ArtifactObject->TryGetStringField(
						TEXT("artifact_kind"),
						ArtifactKindText)
					&& ArtifactObject->TryGetStringField(
						TEXT("durable_location"),
						DurableLocation);
				if (!ArtifactObject.IsValid()
					|| !ArtifactObject->TryGetStringField(
						TEXT("artifact_id"),
						ArtifactId)
					|| !ArtifactObject->TryGetObjectField(
						TEXT("target_identity"),
						IdentityPtr)
					|| !ArtifactObject->TryGetStringField(
						TEXT("package_name"),
						PackageName)
					|| !ArtifactObject->TryGetStringField(
						TEXT("final_path"),
						FinalPathValue)
					|| !ArtifactObject->TryGetStringField(
						TEXT("intent"),
						IntentText)
					|| !ArtifactObject->TryGetStringField(
						TEXT("role"),
						Role)
					|| (bTypedJournal
						&& !bHasTypedArtifact))
				{
					OutError = FAssetDurableError(
						TEXT("durable_journal_integrity_invalid"),
						TEXT("artifact fields are incomplete"));
					return false;
				}
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
					OutError = FAssetDurableError(
						TEXT("durable_journal_integrity_invalid"),
						TEXT("artifact path escapes allowed roots"));
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
				if (!bHasTypedArtifact)
				{
					DurableLocation = PackageName;
				}
				if (!TryParseIdentity(
						IdentityPtr ? *IdentityPtr : nullptr,
						Identity,
						OutError)
					|| !TryIntentFromString(IntentText, Intent)
					|| (bHasTypedArtifact
						&& !TryArtifactKindFromString(
							ArtifactKindText,
							ArtifactKind)))
				{
					if (!OutError.IsPresent())
					{
						OutError = FAssetDurableError(
							TEXT("durable_journal_integrity_invalid"),
							TEXT(
								"artifact kind or intent is invalid"));
					}
					return false;
				}
				if (ArtifactKind == EDurableArtifactKind::Package
					&& PackageName != DurableLocation)
				{
					OutError = FAssetDurableError(
						TEXT("durable_journal_integrity_invalid"),
						TEXT(
							"package artifact location is inconsistent"));
					return false;
				}
				if (ArtifactIds.Contains(ArtifactId)
					|| (!PreviousArtifactId.IsEmpty()
						&& ArtifactId < PreviousArtifactId))
				{
					OutError = FAssetDurableError(
						TEXT("durable_journal_integrity_invalid"),
						TEXT("artifact inventory is not canonical"));
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
					OutError = FAssetDurableError(
						TEXT("durable_journal_integrity_invalid"),
						TEXT("target artifact facts conflict"));
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
				ArtifactIds.Add(ArtifactId);
				PreviousArtifactId = ArtifactId;
				TargetsByKey.Add(
					TargetKey,
					ParsedTarget);
			}
			if (Artifacts.IsEmpty())
			{
				OutError = FAssetDurableError(
					TEXT("durable_journal_integrity_invalid"),
					TEXT("artifact inventory is empty"));
				return false;
			}
			FString ExpectedInventoryHash;
			if (!TryComputeDurableInventoryHashImpl(
					Artifacts,
					bTypedJournal,
					Resources,
					ExpectedInventoryHash,
					OutError)
				|| ExpectedInventoryHash != InventoryHash)
			{
				if (!OutError.IsPresent())
				{
					OutError = FAssetDurableError(
						TEXT("durable_journal_integrity_invalid"),
						TEXT("inventory hash mismatch"));
				}
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
				MoveTemp(History));
			return true;
		}

		bool TryLoadSession(
			const FString& JournalPath,
			const FString& JournalRoot,
			IAssetDurableBackend& Backend,
			FUeAgentResourceTracker& Resources,
			FAssetDurableSessionPtr& OutSession,
			FAssetDurableError& OutError)
		{
			if (!IsWithin(
				JournalPath,
				JournalRoot / TEXT("active")))
			{
				OutError = FAssetDurableError(
					TEXT("durable_journal_integrity_invalid"),
					TEXT("journal path escapes active root"));
				return false;
			}
			FString PayloadText;
			return TryReadOuterJournal(
					JournalPath,
					Resources,
					PayloadText,
					OutError)
				&& TryParseSession(
					PayloadText,
					JournalPath,
					JournalRoot,
					Backend,
					Resources,
					OutSession,
					OutError);
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
				OutError = FAssetDurableError(
					TEXT("durable_inventory_invalid"),
					TEXT("artifact inventory is empty"));
				return false;
			}
			FString Material;
			for (int32 Index = 0;
				Index < Artifacts.Num();
				++Index)
			{
				const FDurableArtifact& Artifact =
					Artifacts[Index];
				if (!Artifact.IsValid())
				{
					OutError = FAssetDurableError(
						TEXT("durable_inventory_invalid"),
						Artifact.GetArtifactId());
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

	FAssetDurableError::FAssetDurableError(
		FString InCode,
		FString InDetail)
		: Code(NormalizedText(MoveTemp(InCode)))
		, Detail(NormalizedText(MoveTemp(InDetail)))
	{
	}

	const FString& FAssetDurableError::GetCode() const
	{
		return Code;
	}

	const FString& FAssetDurableError::GetDetail() const
	{
		return Detail;
	}

	bool FAssetDurableError::IsPresent() const
	{
		return !Code.IsEmpty();
	}

	FString LexToString(const EAssetDurableIntent Value)
	{
		switch (Value)
		{
		case EAssetDurableIntent::Save: return TEXT("save");
		case EAssetDurableIntent::Delete: return TEXT("delete");
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
		case EDurableStatus::RolledBack: return TEXT("rolled_back");
		case EDurableStatus::RecoveryRequired:
			return TEXT("recovery_required");
		case EDurableStatus::StateUnknown:
			return TEXT("state_unknown");
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
		const EAssetDurableIntent InIntent,
		const ERollbackGrade InRollbackGrade)
		: FAssetDurableTarget(
			MoveTemp(InIdentity),
			EDurableArtifactKind::Package,
			MoveTemp(InPackageName),
			InIntent,
			InRollbackGrade)
	{
	}

	FAssetDurableTarget::FAssetDurableTarget(
		FUeAgentCanonicalIdentity InIdentity,
		const EDurableArtifactKind InArtifactKind,
		FString InDurableLocation,
		const EAssetDurableIntent InIntent,
		const ERollbackGrade InRollbackGrade)
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
		, bValid(Identity.IsValid() && !DurableLocation.IsEmpty())
	{
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
		return bValid;
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
		bValid = !Targets.IsEmpty()
			&& FencingToken >= 0
			&& !JournalOwnerId.IsEmpty();
		for (const FAssetDurableTarget& Target : Targets)
		{
			bValid = bValid && Target.IsValid();
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
		return bValid;
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
		, bValid(!ArtifactId.IsEmpty()
			&& TargetIdentity.IsValid()
			&& !DurableLocation.IsEmpty()
			&& !FinalPath.IsEmpty()
			&& !Role.IsEmpty())
	{
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
		return bValid;
	}

	FDurableInventory::FDurableInventory(
		TArray<FDurableArtifact> InArtifacts,
		FString InInventoryHash)
		: Artifacts(MoveTemp(InArtifacts))
		, InventoryHash(
			NormalizedText(MoveTemp(InInventoryHash)))
	{
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
		if (Artifacts.IsEmpty()
			|| !InventoryHash.StartsWith(TEXT("sha256:")))
		{
			return false;
		}
		for (const FDurableArtifact& Artifact : Artifacts)
		{
			if (!Artifact.IsValid())
			{
				return false;
			}
		}
		return true;
	}

	FAssetDurableSession::FAssetDurableSession(
		FAssetDurableRequest InRequest,
		FDurableInventory InInventory,
		FString InJournalId,
		FString InJournalPath,
		FString InJournalRoot,
		FString InTransactionRoot,
		FString InStagingRoot,
		TArray<FString> InPhaseHistory)
		: Request(MoveTemp(InRequest))
		, Inventory(MoveTemp(InInventory))
		, JournalId(NormalizedText(MoveTemp(InJournalId)))
		, JournalPath(FullPath(InJournalPath))
		, JournalRoot(FullPath(InJournalRoot))
		, TransactionRoot(FullPath(InTransactionRoot))
		, StagingRoot(FullPath(InStagingRoot))
		, PhaseHistory(MoveTemp(InPhaseHistory))
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

	FDurableReceipt::FDurableReceipt(
		FString InJournalId,
		const EDurableStatus InStatus,
		FString InPlanFingerprint,
		FString InInventoryHash,
		TArray<FString> InPhaseHistory,
		const ERecoveryAction InRecoveryAction,
		const bool bInRequiresIndependentVerification,
		const bool bInReplayed)
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
	{
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

	FDurableVerificationAcknowledgement::
	FDurableVerificationAcknowledgement(
		FString InJournalId,
		FString InPlanFingerprint,
		FString InInventoryHash,
		FString InVerificationId,
		FString InJournalOwnerId,
		const int64 InFencingToken)
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
	{
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
		return !JournalId.IsEmpty()
			&& PlanFingerprint.StartsWith(TEXT("sha256:"))
			&& InventoryHash.StartsWith(TEXT("sha256:"))
			&& !VerificationId.IsEmpty()
			&& !JournalOwnerId.IsEmpty()
			&& FencingToken >= 0;
	}

	FPackageAssetDurableBackend::FPackageAssetDurableBackend(
		FString InAllowedContentRoot)
		: AllowedContentRoot(
			FullPath(
				InAllowedContentRoot.TrimStartAndEnd().IsEmpty()
					? FPaths::ProjectContentDir()
					: InAllowedContentRoot))
	{
	}

	TArray<FString>
	FPackageAssetDurableBackend::GetAllowedArtifactRoots() const
	{
		return {AllowedContentRoot};
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
				OutError = FAssetDurableError(
					TEXT("durable_artifact_kind_unsupported"),
					LexToString(Target.GetArtifactKind()));
				return false;
			}
			const FString PackageName =
				Target.GetPackageName().TrimStartAndEnd();
			if (!FPackageName::IsValidLongPackageName(PackageName)
				|| FPackageName::GetPackageMountPoint(PackageName)
					.IsNone())
			{
				OutError = FAssetDurableError(
					TEXT("durable_package_name_invalid"),
					PackageName);
				return false;
			}
			FString HeaderPath;
			FString HeaderErrorCode;
			if (!TryResolvePackageHeaderFilename(
					PackageName,
					HeaderPath,
					HeaderErrorCode))
			{
				OutError = FAssetDurableError(
					HeaderErrorCode.IsEmpty()
						? TEXT("durable_package_path_unavailable")
						: HeaderErrorCode,
					PackageName);
				return false;
			}
			HeaderPath = FullPath(HeaderPath);
			const FString FoldedPath = HeaderPath.ToLower();
			if (PackagePaths.Contains(FoldedPath))
			{
				OutError = FAssetDurableError(
					TEXT("durable_package_duplicate"),
					PackageName);
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
			OutError = FAssetDurableError(
				TEXT("durable_backup_root_unavailable"),
				BackupRoot);
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
			if ((bExists
					&& IFileManager::Get().Copy(
						*DataPath,
						*Artifact.GetFinalPath(),
						true,
						true)
						!= COPY_OK)
				|| !FFileHelper::SaveStringToFile(
					bExists ? TEXT("present") : TEXT("absent"),
					*MarkerPath,
					FFileHelper::EEncodingOptions::
						ForceUTF8WithoutBOM))
			{
				OutError = FAssetDurableError(
					TEXT("durable_backup_failed"),
					Artifact.GetArtifactId());
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
			OutError = FAssetDurableError(
				TEXT("durable_stage_root_unavailable"),
				Session->GetStagingRoot());
			return false;
		}
		for (const FAssetDurableTarget& Target :
			Session->GetRequest().GetTargets())
		{
			UPackage* const Package =
				FindPackage(nullptr, *Target.GetPackageName());
			if (Package)
			{
				ResetLoaders(Package);
			}
			if (Target.GetIntent()
				== EAssetDurableIntent::Delete)
			{
				continue;
			}
			if (!Package || Package == GetTransientPackage())
			{
				OutError = FAssetDurableError(
					TEXT("durable_package_not_loaded"),
					Target.GetPackageName());
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
				OutError = FAssetDurableError(
					TEXT("durable_inventory_invalid"),
					Target.GetPackageName());
				return false;
			}
			FString PreparedHeaderPath;
			FString HeaderErrorCode;
			if (!TryPrepareLoadedPackageHeaderForSave(
					*Package,
					TopLevelAsset,
					PreparedHeaderPath,
					HeaderErrorCode))
			{
				OutError = FAssetDurableError(
					HeaderErrorCode.IsEmpty()
						? TEXT("durable_package_header_prepare_failed")
						: HeaderErrorCode,
					Target.GetPackageName());
				return false;
			}
			if (!FullPath(PreparedHeaderPath).Equals(
					FullPath(Header->GetFinalPath()),
					ESearchCase::IgnoreCase))
			{
				OutError = FAssetDurableError(
					TEXT("durable_package_inventory_header_mismatch"),
					Target.GetPackageName());
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
				SAVE_NoError | SAVE_KeepDirty | SAVE_FromAutosave;
			if (!UPackage::SavePackage(
				Package,
				TopLevelAsset,
				*ScratchHeader,
				SaveArgs)
				|| !IFileManager::Get().Move(
					*HeaderStage,
					*ScratchHeader,
					true,
					true,
					false,
					true))
			{
				IFileManager::Get().Delete(*ScratchHeader);
				IFileManager::Get().Delete(*ScratchPayload);
				OutError = FAssetDurableError(
					TEXT("durable_package_stage_failed"),
					Target.GetPackageName());
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
				IFileManager::Get().Delete(*HeaderStage);
				IFileManager::Get().Delete(*ScratchPayload);
				OutError = FAssetDurableError(
					TEXT("durable_package_stage_failed"),
					Payload->GetArtifactId());
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
					OutError = FAssetDurableError(
						TEXT("durable_package_delete_failed"),
						Artifact.GetArtifactId());
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
					OutError = FAssetDurableError(
						TEXT("durable_package_stage_missing"),
						Artifact.GetArtifactId());
					return false;
				}
				if (IFileManager::Get().FileExists(*FinalPath)
					&& !IFileManager::Get().Delete(
						*FinalPath,
						false,
						true,
						true))
				{
					OutError = FAssetDurableError(
						TEXT("durable_package_publish_failed"),
						Artifact.GetArtifactId());
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
				OutError = FAssetDurableError(
					TEXT("durable_package_publish_failed"),
					Artifact.GetArtifactId());
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
				OutError = FAssetDurableError(
					TEXT("durable_backup_marker_missing"),
					Artifact.GetArtifactId());
				return false;
			}
			Marker.TrimStartAndEndInline();
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
						!= COPY_OK
					|| !FilesEqual(
						Artifact.GetFinalPath(),
						DataPath))
				{
					OutError = FAssetDurableError(
						TEXT("durable_restore_failed"),
						Artifact.GetArtifactId());
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
					OutError = FAssetDurableError(
						TEXT("durable_restore_failed"),
						Artifact.GetArtifactId());
					return false;
				}
			}
			else
			{
				OutError = FAssetDurableError(
					TEXT("durable_backup_marker_invalid"),
					Artifact.GetArtifactId());
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
				OutError = FAssetDurableError(
					TEXT("durable_artifact_kind_unsupported"),
					LexToString(Target.GetArtifactKind()));
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
				OutError = FAssetDurableError(
					TEXT("durable_project_config_path_invalid"),
					FinalPath);
				return false;
			}
			if (FinalPaths.Contains(FoldedPath))
			{
				OutError = FAssetDurableError(
					TEXT("durable_project_config_duplicate"),
					FinalPath);
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
			OutError = FAssetDurableError(
				TEXT("durable_backup_root_unavailable"),
				BackupRoot);
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
			if ((bExists
					&& IFileManager::Get().Copy(
						*DataPath,
						*Artifact.GetFinalPath(),
						true,
						true)
						!= COPY_OK)
				|| !FFileHelper::SaveStringToFile(
					bExists ? TEXT("present") : TEXT("absent"),
					*MarkerPath,
					FFileHelper::EEncodingOptions::
						ForceUTF8WithoutBOM))
			{
				OutError = FAssetDurableError(
					TEXT("durable_backup_failed"),
					Artifact.GetArtifactId());
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
			OutError = FAssetDurableError(
				TEXT("durable_stage_root_unavailable"),
				Session->GetStagingRoot());
			return false;
		}
		for (const FAssetDurableTarget& Target :
			Session->GetRequest().GetTargets())
		{
			if (Target.GetArtifactKind()
				!= EDurableArtifactKind::ProjectConfig)
			{
				OutError = FAssetDurableError(
					TEXT("durable_artifact_kind_unsupported"),
					LexToString(Target.GetArtifactKind()));
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
				OutError = FAssetDurableError(
					TEXT("durable_inventory_invalid"),
					Target.GetIdentity().GetCanonicalKey());
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
					OutError)
				|| !StageSource->TryWriteStagedArtifact(
					Target,
					StagePath,
					Resources,
					OutError)
				|| !IFileManager::Get().FileExists(
					*StagePath))
			{
				if (!OutError.IsPresent())
				{
					OutError = FAssetDurableError(
						TEXT(
							"durable_project_config_stage_failed"),
						Target.GetDurableLocation());
				}
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
					OutError = FAssetDurableError(
						TEXT(
							"durable_project_config_delete_failed"),
						Artifact.GetArtifactId());
					return false;
				}
				continue;
			}
			const FString StagePath =
				StagedArtifactPath(Session, Artifact);
			if (!IFileManager::Get().FileExists(*StagePath))
			{
				OutError = FAssetDurableError(
					TEXT(
						"durable_project_config_stage_missing"),
					Artifact.GetArtifactId());
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
				OutError = FAssetDurableError(
					TEXT(
						"durable_project_config_publish_failed"),
					Artifact.GetArtifactId());
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
				OutError = FAssetDurableError(
					TEXT("durable_backup_marker_missing"),
					Artifact.GetArtifactId());
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
						!= COPY_OK
					|| !FilesEqual(
						Artifact.GetFinalPath(),
						DataPath))
				{
					OutError = FAssetDurableError(
						TEXT("durable_restore_failed"),
						Artifact.GetArtifactId());
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
					OutError = FAssetDurableError(
						TEXT("durable_restore_failed"),
						Artifact.GetArtifactId());
					return false;
				}
			}
			else
			{
				OutError = FAssetDurableError(
					TEXT("durable_backup_marker_invalid"),
					Artifact.GetArtifactId());
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
		if (!TryValidateRequest(Request, OutError)
			|| !Backend.SupportsAtomicity(
				Request.GetAtomicityMode())
			|| !TryCharge(
				Resources,
				TEXT("/c8d/prepare/inventory"),
				1,
				Request.GetTargets().Num(),
				0,
				0,
				Request.GetTargets().Num() + 1,
				OutError))
		{
			if (!OutError.IsPresent()
				&& !Backend.SupportsAtomicity(
					Request.GetAtomicityMode()))
			{
				OutError = FAssetDurableError(
					TEXT("durable_atomicity_unsupported"),
					LexToString(Request.GetAtomicityMode()));
			}
			return false;
		}
		FDurableInventory Inventory;
		if (!Backend.BuildInventory(
				Request,
				Resources,
				Inventory,
				OutError))
		{
			if (!OutError.IsPresent())
			{
				OutError = FAssetDurableError(
					TEXT("durable_inventory_failed"));
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
			OutError = FAssetDurableError(
				TEXT("durable_journal_root_invalid"));
			return false;
		}
		if (!IFileManager::Get().MakeDirectory(
			*StagingRoot,
			true))
		{
			OutError = FAssetDurableError(
				TEXT("durable_journal_create_failed"));
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
				TArray<FString>{TEXT("inventory_ready")});
		if (!TryWriteJournal(Session, Resources, OutError))
		{
			return false;
		}
		if (!Backend.CaptureBackups(
			Session,
			Resources,
			OutError))
		{
			if (!OutError.IsPresent())
			{
				OutError = FAssetDurableError(
					TEXT("durable_backup_failed"));
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
				&& CurrentPhase != TEXT("inventory_extended"))
			|| !TryCheckFence(
				Session,
				CurrentFencingToken,
				OutError))
		{
			if (!OutError.IsPresent())
			{
				OutError = FAssetDurableError(
					TEXT("durable_enlist_phase_invalid"),
					CurrentPhase);
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
		for (const FAssetDurableTarget& Candidate : MutationTargets)
		{
			if (!Candidate.IsValid()
				|| Candidate.GetRollbackGrade() == ERollbackGrade::None
				|| !TryValidateTargetAgainstPlan(
					Session->GetRequest().GetPlan(),
					Candidate,
					OutError))
			{
				if (!OutError.IsPresent())
				{
					OutError = FAssetDurableError(
						TEXT("durable_enlist_target_invalid"),
						Candidate.GetDurableTargetKey());
				}
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
				if (Existing->GetIntent() != Candidate.GetIntent())
				{
					OutError = FAssetDurableError(
						TEXT("durable_enlist_target_conflict"),
						CandidateKey);
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
		if (!TryValidateRequest(CombinedRequest, OutError)
			|| !Backend.SupportsAtomicity(
				CombinedRequest.GetAtomicityMode())
			|| !Backend.BuildInventory(
				CombinedRequest,
				Resources,
				CombinedInventory,
				OutError)
			|| !TryValidateInventory(
				CombinedRequest,
				CombinedInventory,
				Backend.GetAllowedArtifactRoots(),
				Resources,
				OutError))
		{
			if (!OutError.IsPresent())
			{
				OutError = FAssetDurableError(
					TEXT("durable_enlist_inventory_failed"));
			}
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
			if (!CombinedArtifact
				|| !AreSameArtifact(
					ExistingArtifact,
					*CombinedArtifact))
			{
				OutError = FAssetDurableError(
					TEXT("durable_enlist_inventory_conflict"),
					ExistingArtifact.GetArtifactId());
				return false;
			}
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
			OutError = FAssetDurableError(
				TEXT("durable_enlist_inventory_unchanged"));
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
		const FAssetDurableSessionRef DeltaSession =
			MakeShared<const FAssetDurableSession>(
				CombinedRequest,
				FDurableInventory(
					MoveTemp(NewArtifacts),
					MoveTemp(DeltaInventoryHash)),
				Session->GetJournalId(),
				Session->GetJournalPath(),
				Session->GetJournalRoot(),
				Session->GetTransactionRoot(),
				Session->GetStagingRoot(),
				Session->GetPhaseHistory());
		if (!Backend.CaptureBackups(
				DeltaSession,
				Resources,
				OutError))
		{
			if (!OutError.IsPresent())
			{
				OutError = FAssetDurableError(
					TEXT("durable_enlist_backup_failed"));
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
				Session->GetPhaseHistory());
		ExtendedSession = Advance(
			ExtendedSession,
			TEXT("inventory_extended"));
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
			return TryRestoreAfterFailure(
				Working,
				Backend,
				Resources,
				false,
				OutReceipt,
				OutError);
		}
		Working = Advance(Working, TEXT("staged"));
		if (!TryWriteJournal(Working, Resources, OutError))
		{
			return TryRestoreAfterFailure(
				Working,
				Backend,
				Resources,
				false,
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
			return TryRestoreAfterFailure(
				Working,
				Backend,
				Resources,
				false,
				OutReceipt,
				OutError);
		}
		Working = Advance(Working, TEXT("published"));
		if (!TryWriteJournal(Working, Resources, OutError))
		{
			return TryRestoreAfterFailure(
				Working,
				Backend,
				Resources,
				false,
				OutReceipt,
				OutError);
		}
		Working = Advance(
			Working,
			TEXT("persisted_unverified"));
		if (!TryWriteJournal(Working, Resources, OutError))
		{
			return TryRestoreAfterFailure(
				Working,
				Backend,
				Resources,
				false,
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

	bool TryRollbackAssetDurability(
		const FAssetDurableSessionRef& Session,
		IAssetDurableBackend& Backend,
		const int64 CurrentFencingToken,
		const bool bMemoryRollbackConfirmed,
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
		bool bDurableConfirmed = false;
		FAssetDurableError BackendError;
		if (!Backend.Restore(
				Session,
				Resources,
				bDurableConfirmed,
				BackendError)
			|| !bDurableConfirmed
			|| !bMemoryRollbackConfirmed)
		{
			return TryRecordUnknown(
				Session,
				Resources,
				false,
				OutReceipt,
				OutError);
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
		if (!Acknowledgement.IsValid())
		{
			OutError = FAssetDurableError(
				TEXT("durable_acknowledgement_invalid"));
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
			OutError = FAssetDurableError(
				TEXT("durable_acknowledgement_phase_invalid"));
			return false;
		}
		if (Acknowledgement.GetJournalId()
				!= Session->GetJournalId()
			|| Acknowledgement.GetPlanFingerprint()
				!= Session->GetRequest().GetPlan()->GetFingerprint()
			|| Acknowledgement.GetInventoryHash()
				!= Session->GetInventory().GetInventoryHash()
			|| Acknowledgement.GetJournalOwnerId()
				!= Session->GetRequest().GetJournalOwnerId()
			|| Acknowledgement.GetFencingToken()
				!= CurrentFencingToken)
		{
			OutError = FAssetDurableError(
				TEXT("durable_acknowledgement_binding_mismatch"));
			return false;
		}
		const FAssetDurableSessionRef Verified =
			Advance(Session, TEXT("verified"));
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
			OutError = FAssetDurableError(
				TEXT("durable_journal_owner_invalid"));
			return false;
		}
		const FString Root = FullPath(JournalRoot);
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
			if (!TryLoadSession(
				JournalPath,
				Root,
				Backend,
				Resources,
				Session,
				OutError)
				|| !Session.IsValid()
				|| !TryCheckFence(
					Session.ToSharedRef(),
					CurrentFencingToken,
					OutError))
			{
				OutReceipts.Reset();
				return false;
			}
			const FAssetDurableSessionRef Ref =
				Session.ToSharedRef();
			if (Ref->GetRequest().GetJournalOwnerId() != OwnerId)
			{
				OutReceipts.Reset();
				OutError = FAssetDurableError(
					TEXT("durable_journal_owner_mismatch"));
				return false;
			}
			const FString& Phase = Ref->GetCurrentPhase();
			FDurableReceipt Current;
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
					OutError)
					|| !TryArchiveTerminal(
						RolledBack,
						Resources,
						OutError))
				{
					OutReceipts.Reset();
					return false;
				}
				Current = Receipt(
					RolledBack,
					EDurableStatus::RolledBack,
					ERecoveryAction::AbortedBeforeMutation,
					false,
					true);
			}
			else if (Phase == TEXT("staged"))
			{
				if (!TryRestoreAfterFailure(
					Ref,
					Backend,
					Resources,
					true,
					Current,
					OutError))
				{
					OutReceipts.Reset();
					return false;
				}
			}
			else if (Phase == TEXT("published")
				|| Phase == TEXT("persisted_unverified"))
			{
				FString Observed;
				FAssetDurableError BackendError;
				if (!Backend.Inspect(
					Ref,
					Resources,
					Observed,
					BackendError)
					|| Observed != TEXT("persisted"))
				{
					if (!TryRecordUnknown(
						Ref,
						Resources,
						true,
						Current,
						OutError))
					{
						OutReceipts.Reset();
						return false;
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
						OutError))
					{
						OutReceipts.Reset();
						return false;
					}
					Current = Receipt(
						Recovery,
						EDurableStatus::RecoveryRequired,
						ERecoveryAction::VerifyPersisted,
						true,
						true);
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
				OutReceipts.Reset();
				OutError = FAssetDurableError(
					TEXT("durable_journal_integrity_invalid"),
					TEXT("unsupported recovery phase"));
				return false;
			}
			OutReceipts.Add(MoveTemp(Current));
		}
		return true;
	}
}

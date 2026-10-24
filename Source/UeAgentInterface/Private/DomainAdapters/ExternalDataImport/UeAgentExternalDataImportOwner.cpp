#include "DomainAdapters/ExternalDataImport/UeAgentExternalDataImportOwner.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "DomainAdapters/ExternalImport/Factory/UeAgentNativeImportFactory.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "Animation/SkinWeightProfile.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "Core/AssetDurable/UeAgentAssetDurable.h"
#include "Core/ContextResource/UeAgentContextResource.h"
#include "Core/Contract/UeAgentContractJson.h"
#include "Core/PlanningPreflight/UeAgentPlanningPreflight.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureLightProfile.h"
#include "Factories/FbxFactory.h"
#include "Factories/FbxImportUI.h"
#include "GeometryCache.h"
#include "HAL/FileManager.h"
#include "HAL/ThreadSafeCounter.h"
#include "IAssetTools.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Modules/ModuleManager.h"
#include "ObjectTools.h"
#include "SkinWeightsUtilities.h"
#include "Sound/SoundWave.h"
#include "SparseVolumeTexture/SparseVolumeTexture.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectIterator.h"
#include "UObject/StrongObjectPtr.h"
#include "Core/AssetDurable/UeAgentPackageSave.h"
#include "Utilities/AuthoringOperationApply/UeAgentAuthoringPackageReadback.h"

#include <openssl/sha.h>

namespace UE::UAI::DomainAdapters::ExternalDataImport
{
	using namespace AssetDurable;
	using namespace ContextResource;
	using namespace Contract;
	using namespace PlanningPreflight;
	using namespace StateSemantic;

	namespace
	{

		FDiagnostic ImportFact(const TCHAR* Code, const TCHAR* Reason, const FString& Path,
			TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected,
			const TCHAR* Stage = TEXT("external_import"))
		{
			auto Error = FDiagnostic::Failure(Code, Reason, Stage);
			Error.SourceKind = TEXT("native_result");
			Error.PropertyPath = Path;
			Error.ActualValue = MoveTemp(Actual);
			Error.ExpectedValue = MoveTemp(Expected);
			Error.ActualValuePresent = Error.ActualValue.IsValid();
			Error.ExpectedValuePresent = Error.ExpectedValue.IsValid();
			return Error;
		}

		FDiagnostic ImportInput(const TCHAR* Code, const TCHAR* Reason, const TCHAR* Field,
			const FString& Actual, const FString& Expected)
		{
			auto Error = ImportFact(Code, Reason, FString(TEXT("/")) + Field,
				MakeShared<FJsonValueString>(Actual), MakeShared<FJsonValueString>(Expected), TEXT("parameters"));
			Error.SourceKind = TEXT("command_input");
			Error.Source.JsonPath = Error.PropertyPath == TEXT("/command_id")
				? Error.PropertyPath : FString(TEXT("/params")) + Error.PropertyPath;
			return Error;
		}

		FDiagnostic ImportCondition(const TCHAR* Code, const TCHAR* Reason, const FString& Path,
			bool Actual, bool Expected, const FString& Filename = FString())
		{
			auto Error = ImportFact(Code, Reason, Path, MakeShared<FJsonValueBoolean>(Actual),
				MakeShared<FJsonValueBoolean>(Expected));
			Error.Source.SourceFile = Filename;
			return Error;
		}

		TSharedPtr<FJsonValue> ImportedObjectFacts(const TArray<UObject*>& Objects)
		{
			TArray<TSharedPtr<FJsonValue>> Values;
			for (UObject* Object : Objects)
			{
				if (!IsValid(Object)) { Values.Add(MakeShared<FJsonValueNull>()); continue; }
				auto Value = MakeShared<FJsonObject>();
				Value->SetStringField(TEXT("class_path"), Object->GetClass()->GetPathName());
				if (Object->GetOutermost()) Value->SetStringField(TEXT("package"), Object->GetOutermost()->GetName());
				Values.Add(MakeShared<FJsonValueObject>(Value));
			}
			return MakeShared<FJsonValueArray>(MoveTemp(Values));
		}
		FUeAgentResourceTracker MakeTracker()
		{
			return FUeAgentResourceTracker();
		}

		bool TryPackageName(FString Ref, FString& OutPackageName)
		{
			Ref.TrimStartAndEndInline();
			OutPackageName = Ref.Contains(TEXT("."))
				? FPackageName::ObjectPathToPackageName(Ref)
				: MoveTemp(Ref);
			return OutPackageName.StartsWith(TEXT("/Game/"))
				&& FPackageName::IsValidLongPackageName(OutPackageName, true);
		}

		FString ObjectPath(const FString& PackageName)
		{
			return PackageName + TEXT(".")
				+ FPackageName::GetLongPackageAssetName(PackageName);
		}

		bool PackageExists(const FString& PackageName)
		{
			FString Filename;
			return FindPackage(nullptr, *PackageName) != nullptr
				|| FPackageName::DoesPackageExist(PackageName, &Filename);
		}

		UObject* ReloadForIndependentReadback(const FString& PackageName, FDiagnostic& Error)
		{
			if (!UeAgentAuthoringOperationApply::TryReloadPersistedPackage(PackageName, Error)) return nullptr;
			UObject* Object = FindObject<UObject>(nullptr, *ObjectPath(PackageName));
			if (!Object)
			{
				Error = ImportFact(TEXT("external_import_readback_object_missing"),
					TEXT("The persisted package was reloaded, but its expected asset object was not found."),
					TEXT("/readback_object"), MakeShared<FJsonValueNull>(),
					MakeShared<FJsonValueString>(ObjectPath(PackageName)), TEXT("verification"));
				Error.TargetPath = PackageName;
			}
			return Object;
		}

		bool TryNormalizeSource(FString SourceRef, const TSet<FString>& AllowedExtensions,
			FString& OutSource, FDiagnostic& OutError)
		{
			const FString Input = SourceRef;
			SourceRef.TrimStartAndEndInline();
			if (SourceRef.IsEmpty())
			{
				OutError = ImportInput(TEXT("external_import_source_missing"),
					TEXT("The external import source reference must be a non-empty file path."),
					TEXT("source_ref"), Input, TEXT("a non-empty source file path"));
				return false;
			}
			OutSource = FPaths::IsRelative(SourceRef)
				? FPaths::ConvertRelativePathToFull(FPaths::ProjectDir(), SourceRef)
				: FPaths::ConvertRelativePathToFull(SourceRef);
			FPaths::NormalizeFilename(OutSource);
			if (!FPaths::FileExists(OutSource))
			{
				OutError = ImportInput(TEXT("external_import_source_not_found"),
					TEXT("The external import source file does not exist at the resolved path."),
					TEXT("source_ref"), Input, TEXT("an existing source file"));
				OutError.TargetPath = OutSource;
				return false;
			}
			const FString Extension = FPaths::GetExtension(OutSource).ToLower();
			if (!AllowedExtensions.Contains(Extension))
			{
				OutError = ImportInput(TEXT("external_import_source_extension_invalid"),
					TEXT("The source file extension is not supported by this import command."),
					TEXT("source_ref"), Input, TEXT("a source file with an allowed extension"));
				OutError.TargetPath = OutSource;
				OutError.ActualValue = MakeShared<FJsonValueString>(Extension);
				TArray<FString> Extensions = AllowedExtensions.Array();
				Extensions.Sort();
				TArray<TSharedPtr<FJsonValue>> Choices;
				for (const auto& Value : Extensions) Choices.Add(MakeShared<FJsonValueString>(Value));
				OutError.ExpectedValue = MakeShared<FJsonValueArray>(MoveTemp(Choices));
				return false;
			}
			return true;
		}

		bool TryHashFileSha256(const FString& Filename, FString& OutHash, FDiagnostic& OutError,
			const TCHAR* Stage = TEXT("source_read"))
		{
			OutHash.Reset();
			TArray<uint8> Bytes;
			if (!FFileHelper::LoadFileToArray(Bytes, *Filename))
			{
				OutError = ImportCondition(TEXT("external_import_source_hash_failed"),
					TEXT("The file could not be read to compute its SHA256 digest."),
					TEXT("/file_read_succeeded"), false, true, Filename);
				OutError.Stage = Stage;
				return false;
			}
			FSHA256Signature Signature{};
			const uint8 Empty = 0;
			const uint8* Data = Bytes.IsEmpty() ? &Empty : Bytes.GetData();
			if (!SHA256(Data, static_cast<size_t>(Bytes.Num()), Signature.Signature))
			{
				OutError = ImportCondition(TEXT("external_import_source_hash_failed"),
					TEXT("SHA256 digest calculation failed after the file was read."),
					TEXT("/hash_succeeded"), false, true, Filename);
				OutError.Stage = Stage;
				return false;
			}
			OutHash = TEXT("sha256:") + Signature.ToString().ToLower();
			return true;
		}

		FUeAgentCanonicalIdentity IdentityFor(const FString& Target)
		{
			return FUeAgentCanonicalIdentity(
				TEXT("uai.external.import"), TEXT("external_import"), Target,
				TEXT("content"), TEXT("asset"), TEXT("external-import-v1"));
		}

		FPlanningPreflightPlanRef MakePlan(
			const FUeAgentExternalDataImportRequest& Request,
			const FString& Target,
			const FString& OwnerId,
			const FUeAgentCanonicalIdentity& Identity)
		{
			TArray<FPlannedChange> Changes;
			Changes.Emplace(
				Identity, EChangeIntent::Create, FUeAgentCanonicalValuePtr(),
				FUeAgentCanonicalValuePtr(),
				TArray<FString>{TEXT("uai:external-import:write")},
				TEXT("external-import:native"));
			TArray<FImpact> Impacts;
			Impacts.Emplace(
				Identity, Identity, TEXT("direct"),
				TEXT("external-import:native"));
			return MakeShared<FPlanningPreflightPlan>(
				FUeAgentDescriptorIdentity(
					TEXT("uai.external.import/v1"), 1,
					Json::Sha256Utf8(TEXT("uai.external.import/v1"))),
				Request.RequestId,
				TEXT("external-import:") + Request.RequestId,
				Request.CallerId,
				TArray<FString>{TEXT("uai:external-import:write")},
				TEXT("external-import:preflight:") + Request.RequestId,
				FUeAgentStateRevision(OwnerId, TEXT("external-import-v1"), 1),
				TEXT("external-import:preflight:native"),
				FChangeSet(MoveTemp(Changes)), FImpactSet(MoveTemp(Impacts)),
				TArray<FPrecondition>(),
				Json::Sha256Utf8(
					Request.CommandId + TEXT(":") + Request.RequestId
					+ TEXT(":") + Target + TEXT(":") + Request.SourceRef));
		}

		class FDurableImport final
		{
		public:
			bool Prepare(
				const FUeAgentExternalDataImportRequest& Request,
				const FString& PackageName,
				const FString& OwnerId,
				const FString& JournalRoot,
				const int64 FencingToken,
				UE::UAI::Contract::FDiagnostic& OutError)
			{
				Identity = IdentityFor(PackageName);
				Token = FencingToken;
				Plan = MakePlan(Request, PackageName, OwnerId, Identity);
				const FAssetDurableRequest DurableRequest(
					Plan.ToSharedRef(),
					{FAssetDurableTarget(
						Identity, PackageName, EPackageHeaderKind::Asset,
						EAssetDurableIntent::Save,
						ERollbackGrade::DurableBackup)},
					Token, OwnerId, EAtomicityMode::CompensatedSaga);
				FAssetDurableError Error;
				if (!TryPrepareAssetDurability(
						DurableRequest, Backend, JournalRoot, Resources,
						Session, Error)
					|| !Session.IsValid())
				{
					OutError = Error;
					if (OutError.IsEmpty())
					{
						OutError = ImportCondition(TEXT("external_import_durable_session_missing"),
							TEXT("Durability preparation returned no import session."), TEXT("/durable_session_present"), false, true);
						OutError.TargetPath = PackageName;
					}
					return false;
				}
				return true;
			}

			bool Persist(UE::UAI::Contract::FDiagnostic& OutError)
			{
				FAssetDurableError Error;
				if (!TryPersistAssetDurabilityForVerification(
						Session.ToSharedRef(), Backend, Token, Resources,
						Receipt, Error))
				{
					OutError = Error;
					return false;
				}
				return true;
			}

			bool Acknowledge(const FString& VerificationId, UE::UAI::Contract::FDiagnostic& OutError)
			{
				const FAssetDurableSessionRef Persisted =
					MakeShared<const FAssetDurableSession>(
						Session->GetRequest(), Session->GetInventory(),
						Session->GetJournalId(), Session->GetJournalPath(),
						Session->GetJournalRoot(), Session->GetTransactionRoot(),
						Session->GetStagingRoot(), Receipt.GetPhaseHistory());
				const FDurableVerificationAcknowledgement Ack(
					Receipt.GetJournalId(), Receipt.GetPlanFingerprint(),
					Receipt.GetInventoryHash(), VerificationId,
					Session->GetRequest().GetJournalOwnerId(), Token);
				FAssetDurableError Error;
				if (!TryAcknowledgeAssetDurability(
						Persisted, Ack, Token, Resources, Error))
				{
					OutError = Error;
					return false;
				}
				return true;
			}

			bool Rollback(const bool bMemoryConfirmed, FDiagnostic& OutError)
			{
				if (!Session.IsValid())
				{
					OutError = ImportCondition(TEXT("external_import_rollback_session_missing"),
						TEXT("Durable import rollback has no prepared session."), TEXT("/durable_session_present"), false, true);
					return false;
				}
				FDurableReceipt RollbackReceipt;
				return TryRollbackAssetDurability(Session.ToSharedRef(), Backend, Token, bMemoryConfirmed,
					Resources, RollbackReceipt, OutError);
			}

			FString JournalId() const
			{
				return Session.IsValid() ? Session->GetJournalId() : FString();
			}

		private:
			FUeAgentResourceTracker Resources = MakeTracker();
			FPackageAssetDurableBackend Backend;
			FUeAgentCanonicalIdentity Identity = IdentityFor(TEXT("/Game/Invalid"));
			FPlanningPreflightPlanPtr Plan;
			FAssetDurableSessionPtr Session;
			FDurableReceipt Receipt;
			int64 Token = -1;
		};

		bool DeleteImportedObjects(const TArray<UObject*>& Objects, FDiagnostic& OutError)
		{
			TArray<UObject*> Owned;
			TSet<FString> PackageNames;
			for (UObject* Object : Objects)
			{
				if (!IsValid(Object)) continue;
				Owned.AddUnique(Object);
				if (Object->GetOutermost())
				{
					PackageNames.Add(Object->GetOutermost()->GetName());
				}
			}
			if (!Owned.IsEmpty())
			{
				TGuardValue<bool> Guard(GIsRunningUnattendedScript, true);
				ObjectTools::DeleteObjectsUnchecked(Owned);
			}
			CollectGarbage(RF_NoFlags, true);
			bool bRemoved = true;
			for (const FString& PackageName : PackageNames)
			{
				FString Filename;
				if (FPackageName::DoesPackageExist(PackageName, &Filename))
				{
					const bool bDeleted = IFileManager::Get().Delete(*Filename, false, true);
					bRemoved = bDeleted && bRemoved;
					if (!bDeleted) OutError.Append(ImportCondition(TEXT("external_import_cleanup_file_delete_failed"),
						TEXT("An imported asset package file could not be removed during rollback."),
						TEXT("/file_removed"), false, true, Filename));
				}
			}
			return bRemoved;
		}

		void RollbackImported(FDurableImport& Durable, const TArray<UObject*>& Imported,
			FUeAgentExternalDataImportResult& Result)
		{
			FDiagnostic RemovalError;
			const bool bRemoved = DeleteImportedObjects(Imported, RemovalError);
			FDiagnostic RollbackError;
			const bool bRestored = Durable.Rollback(bRemoved, RollbackError);
			const bool bConfirmed = bRemoved && bRestored;
			Result.TerminalState = bConfirmed ? TEXT("rolled_back") : TEXT("state_unknown");
			Result.Diagnostic.RollbackAttempted = true;
			Result.Diagnostic.RollbackSucceeded = bConfirmed;
			Result.Diagnostic.TransactionStatus = Result.TerminalState;
			Result.Diagnostic.MutationMayHaveStarted = true;
			Result.Diagnostic.Append(RemovalError);
			Result.Diagnostic.Append(RollbackError);
		}

		void RollbackSkinProfile(FDurableImport& Durable, FUeAgentExternalDataImportResult& Result, bool bMutationConfirmed)
		{
			FDiagnostic RollbackError;
			const bool bRestored = Durable.Rollback(false, RollbackError);
			Result.TerminalState = TEXT("state_unknown");
			Result.Diagnostic.RollbackAttempted = true;
			Result.Diagnostic.RollbackSucceeded = bRestored;
			Result.Diagnostic.TransactionStatus = Result.TerminalState;
			if (bMutationConfirmed) Result.Diagnostic.MutationStarted = true;
			else Result.Diagnostic.MutationMayHaveStarted = true;
			Result.Diagnostic.Append(RollbackError);
		}

		bool SavePackage(UPackage& Package, UE::UAI::Contract::FDiagnostic& OutDetail)
		{
			const FString Filename = FPackageName::LongPackageNameToFilename(
				Package.GetName(), FPackageName::GetAssetPackageExtension());
			FSavePackageArgs Args;
			Args.TopLevelFlags = RF_Public | RF_Standalone;
			return UE::UAI::AssetDurable::TrySaveLoadedPackage(Package, nullptr, Filename, Args, OutDetail);
		}

		UClass* ExpectedClassFor(const FString& CommandId)
		{
			if (CommandId == TEXT("external.fbx.animation.import"))
				return UAnimSequence::StaticClass();
			if (CommandId == TEXT("external.fbx.skeletal_mesh.import"))
				return USkeletalMesh::StaticClass();
			if (CommandId == TEXT("external.geometry_cache.import"))
				return UGeometryCache::StaticClass();
			if (CommandId == TEXT("external.texture.import"))
				return UTexture2D::StaticClass();
			if (CommandId == TEXT("external.light_profile.import"))
				return UTextureLightProfile::StaticClass();
			if (CommandId == TEXT("external.sparse_volume_texture.import"))
				return USparseVolumeTexture::StaticClass();
			if (CommandId == TEXT("external.sound_wave.import"))
				return USoundWave::StaticClass();
			return nullptr;
		}

		UFactory* CreateFactory(
			const FUeAgentExternalDataImportRequest& Request,
			const FString& Source,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			if (Request.CommandId != TEXT("external.fbx.animation.import")
				&& Request.CommandId
					!= TEXT("external.fbx.skeletal_mesh.import"))
			{
				UFactory* Factory = ExternalImport::Factory::SelectNativeImportFactory(ExpectedClassFor(Request.CommandId), Source);
				if (!Factory)
				{
					const UClass* ExpectedClass = ExpectedClassFor(Request.CommandId);
					OutError = ImportFact(TEXT("external_import_native_factory_unavailable"),
						TEXT("No native import factory supports this source extension and expected asset class."),
						TEXT("/source_extension"), MakeShared<FJsonValueString>(FPaths::GetExtension(Source)),
						MakeShared<FJsonValueString>(ExpectedClass ? ExpectedClass->GetPathName() : TEXT("a registered asset class")));
					OutError.Source.SourceFile = Source;
				}
				return Factory;
			}
			UFbxFactory* const Factory = NewObject<UFbxFactory>();
			if (!Factory || !Factory->ImportUI)
			{
				OutError = ImportCondition(TEXT("external_import_fbx_factory_unavailable"),
					Factory ? TEXT("The FBX import factory has no import settings object.") : TEXT("The FBX import factory could not be created."),
					Factory ? TEXT("/factory/import_ui_present") : TEXT("/factory_present"), false, true, Source);
				OutError.CandidateName = TEXT("FbxFactory");
				return nullptr;
			}
			Factory->SetDetectImportTypeOnImport(false);
			Factory->ImportUI->bAutomatedImportShouldDetectType = false;
			Factory->ImportUI->bImportAsSkeletal = true;
			Factory->ImportUI->bIsReimport = false;
			Factory->ImportUI->ReimportMesh = nullptr;
			Factory->ImportUI->bAllowContentTypeImport = true;
			Factory->ImportUI->bImportMaterials = false;
			Factory->ImportUI->bImportTextures = false;
			Factory->ImportUI->bCreatePhysicsAsset = false;
			Factory->ImportUI->bImportRigidMesh = false;
			Factory->ImportUI->bIsObjImport = false;
			Factory->ImportUI->bOverrideFullName = true;
			if (Request.CommandId == TEXT("external.fbx.animation.import"))
			{
				USkeleton* const Skeleton = LoadObject<USkeleton>(
					nullptr, *Request.SkeletonRef);
				if (!Skeleton)
				{
					OutError = ImportInput(TEXT("external_import_skeleton_not_found"),
						TEXT("The animation import skeleton reference could not be loaded as a Skeleton asset."),
						TEXT("skeleton_ref"), Request.SkeletonRef, USkeleton::StaticClass()->GetPathName());
					return nullptr;
				}
				Factory->ImportUI->MeshTypeToImport = FBXIT_Animation;
				Factory->ImportUI->OriginalImportType = FBXIT_Animation;
				Factory->ImportUI->bImportAnimations = true;
				Factory->ImportUI->bImportMesh = false;
				Factory->ImportUI->Skeleton = Skeleton;
				Factory->ImportUI->OverrideAnimationName =
					FPackageName::GetLongPackageAssetName(Request.DestinationRef);
			}
			else
			{
				Factory->ImportUI->MeshTypeToImport = FBXIT_SkeletalMesh;
				Factory->ImportUI->OriginalImportType = FBXIT_SkeletalMesh;
				Factory->ImportUI->bImportAnimations = false;
				Factory->ImportUI->bImportMesh = true;
				Factory->ImportUI->Skeleton = nullptr;
			}
			return Factory;
		}

		TSet<FString> ExtensionsFor(const FString& CommandId)
		{
			if (CommandId.StartsWith(TEXT("external.fbx."))
				|| CommandId == TEXT("external.skin_weight_profile.import"))
				return {TEXT("fbx")};
			if (CommandId == TEXT("external.localization.po.import"))
				return {TEXT("po")};
			return ExternalImport::Factory::NativeImportExtensions(ExpectedClassFor(CommandId));
		}

		bool IsSafeLocalizationSegment(const FString& Segment)
		{
			if (Segment.IsEmpty()) return false;
			for (const TCHAR Character : Segment)
			{
				if (!FChar::IsAlnum(Character)
					&& Character != TEXT('_') && Character != TEXT('-'))
				{
					return false;
				}
			}
			return true;
		}

		bool TryCultureFromPo(const FString& Source, FString& OutCulture, FDiagnostic& OutError)
		{
			FString Text;
			if (!FFileHelper::LoadFileToString(Text, *Source))
			{
				OutError = ImportCondition(TEXT("external_localization_source_read_failed"),
					TEXT("The PO source file could not be read to obtain its Language header."),
					TEXT("/file_read_succeeded"), false, true, Source);
				return false;
			}
			TArray<FString> Lines;
			Text.ParseIntoArrayLines(Lines, false);
			for (int32 Index = 0; Index < Lines.Num(); ++Index)
			{
				FString Line = Lines[Index];
				Line.TrimStartAndEndInline();
				Line.RemoveFromStart(TEXT("\""));
				if (!Line.StartsWith(TEXT("Language:"))) continue;
				OutCulture = Line.Mid(9).TrimStartAndEnd();
				OutCulture.RemoveFromEnd(TEXT("\\n\""));
				OutCulture.RemoveFromEnd(TEXT("\""));
				OutCulture.TrimStartAndEndInline();
				if (IsSafeLocalizationSegment(OutCulture)) return true;
				OutError = ImportFact(TEXT("external_localization_culture_invalid"),
					TEXT("The PO Language header must contain a non-empty culture using only letters, digits, underscore or hyphen."),
					TEXT("/Language"), MakeShared<FJsonValueString>(OutCulture),
					MakeShared<FJsonValueString>(TEXT("non-empty letters, digits, underscore or hyphen")));
				OutError.SourceKind = TEXT("related_input");
				OutError.Source.SourceFile = Source;
				OutError.Source.Line = Index + 1;
				int32 ValueStart = Lines[Index].Find(TEXT("Language:")) + 9;
				while (ValueStart < Lines[Index].Len() && FChar::IsWhitespace(Lines[Index][ValueStart])) ++ValueStart;
				OutError.Source.Column = ValueStart + 1;
				return false;
			}
			OutError = ImportCondition(TEXT("external_localization_culture_missing"),
				TEXT("The PO source file has no Language header."), TEXT("/Language_present"), false, true, Source);
			OutError.SourceKind = TEXT("related_input");
			return false;
		}

		class FExternalDataImportOwner final
			: public IUeAgentExternalDataImportOwner
		{
		public:
			FExternalDataImportOwner(FString InJournalRoot, FString InOwnerId)
				: JournalRoot(MoveTemp(InJournalRoot)), OwnerId(MoveTemp(InOwnerId))
			{
			}

			FUeAgentExternalDataImportResult Execute(
				const FUeAgentExternalDataImportRequest& Request,
				const TSharedRef<IUeAgentCancellationSignal>& Cancellation) override
			{
				FUeAgentExternalDataImportResult Result;
				Result.CommandId = Request.CommandId;
				Result.SourceRef = Request.SourceRef;
				Result.ResultRef = Request.DestinationRef.IsEmpty()
					? Request.TargetRef : Request.DestinationRef;
				auto Complete = [](FUeAgentExternalDataImportResult Value)
				{
					if (!Value.Diagnostic.IsEmpty())
					{
						if (Value.Diagnostic.TargetPath.IsEmpty()) Value.Diagnostic.TargetPath = Value.ResultRef;
					}
					return Value;
				};
				auto Fail = [&Result, &Complete](FDiagnostic Diagnostic)
				{
					Result.Diagnostic = MoveTemp(Diagnostic);
					return Complete(MoveTemp(Result));
				};
				if (!IsInGameThread())
					return Fail(ImportCondition(TEXT("external_import_game_thread_required"),
						TEXT("External native import must execute on the game thread."), TEXT("/is_game_thread"), false, true));
				if (Cancellation->IsCancelled())
				{
					auto Cancelled = ImportCondition(TEXT("external_import_cancelled"),
						TEXT("Cancellation was requested before external import started."), TEXT("/cancellation_requested"), true, false);
					Cancelled.SubjectId = Request.RequestId;
					Cancelled.MutationStarted = false;
					return Fail(MoveTemp(Cancelled));
				}
				const TSet<FString> Extensions = ExtensionsFor(Request.CommandId);
				if (Extensions.IsEmpty())
					return Fail(ImportInput(TEXT("external_import_command_unknown"),
						TEXT("No supported source file extensions are registered for this import command."),
						TEXT("command_id"), Request.CommandId, TEXT("an import command with registered source extensions")));
				FString Source;
				UE::UAI::Contract::FDiagnostic Error;
				if (!TryNormalizeSource(Request.SourceRef, Extensions, Source, Error))
					return Fail(Error);
				Result.SourceRef = Source;
				if (!TryHashFileSha256(Source, Result.SourceSha256, Error)) return Fail(MoveTemp(Error));
				if (Request.CommandId == TEXT("external.localization.po.import"))
					return Complete(ImportLocalization(Request, Source, MoveTemp(Result)));
				if (Request.CommandId == TEXT("external.skin_weight_profile.import"))
					return Complete(ImportSkinWeightProfile(Request, Source, MoveTemp(Result)));
				return Complete(ImportAsset(Request, Source, MoveTemp(Result)));
			}

		private:
			FUeAgentExternalDataImportResult ImportAsset(
				const FUeAgentExternalDataImportRequest& Request,
				const FString& Source,
				FUeAgentExternalDataImportResult Result)
			{
				FString Destination;
				if (!TryPackageName(Request.DestinationRef, Destination))
				{
					Result.Diagnostic = ImportInput(TEXT("external_import_destination_invalid"),
						TEXT("The import destination must resolve to a valid asset package below /Game/."),
						TEXT("destination_ref"), Request.DestinationRef, TEXT("a valid /Game/ asset package or object path"));
					return Result;
				}
				Result.ResultRef = Destination;
				if (PackageExists(Destination))
				{
					Result.Diagnostic = ImportCondition(TEXT("external_import_destination_conflict"),
						TEXT("The import destination package already exists; this command creates a new asset."),
						TEXT("/destination_package_exists"), true, false);
					Result.Diagnostic.TargetPath = Destination;
					return Result;
				}
				const FString DestinationName =
					FPackageName::GetLongPackageAssetName(Destination);
				const UClass* const ExpectedClass = ExpectedClassFor(Request.CommandId);
				if (!ExpectedClass)
				{
					Result.Diagnostic = ImportInput(TEXT("external_import_expected_class_missing"),
						TEXT("No expected native asset class is registered for this import command."),
						TEXT("command_id"), Request.CommandId, TEXT("an import command with a registered native asset class"));
					return Result;
				}
				UE::UAI::Contract::FDiagnostic FactoryError;
				TStrongObjectPtr<UFactory> Factory(CreateFactory(Request, Source, FactoryError));
				if (!Factory.IsValid())
				{
					Result.Diagnostic = FactoryError.IsEmpty()
						? ImportCondition(TEXT("external_import_native_factory_failed"), TEXT("The native asset import factory could not be created."),
							TEXT("/factory_present"), false, true, Source) : FactoryError;
					return Result;
				}

				FDurableImport Durable;
				UE::UAI::Contract::FDiagnostic Error;
				if (!Durable.Prepare(
						Request, Destination, OwnerId, JournalRoot,
						Counter.Increment(), Error))
				{
					Result.Diagnostic = Error;
					return Result;
				}
				Result.JournalId = Durable.JournalId();
				TArray<UObject*> Imported = ExternalImport::Factory::ImportNativeAsset(Factory.Get(), Source, Destination);
				UObject* Primary = nullptr;
				for (UObject* Object : Imported)
				{
					if (IsValid(Object) && Object->IsA(ExpectedClass)
						&& Object->GetOutermost()
						&& Object->GetOutermost()->GetName() == Destination)
					{
						Primary = Object;
						break;
					}
				}
				if (!Primary)
				{
					Result.Diagnostic = ImportFact(TEXT("external_import_native_failed"),
						TEXT("Native import produced no valid asset with the expected class in the destination package."),
						TEXT("/imported_objects"), ImportedObjectFacts(Imported),
						MakeShared<FJsonValueString>(ExpectedClass->GetPathName()));
					Result.Diagnostic.Source.SourceFile = Source;
					Result.Diagnostic.TargetPath = Destination;
					RollbackImported(Durable, Imported, Result);
					return Result;
				}

				TSet<UPackage*> ExtraPackages;
				for (UObject* Object : Imported)
				{
					if (IsValid(Object) && Object->GetOutermost()
						&& Object->GetOutermost()->GetName() != Destination)
					{
						ExtraPackages.Add(Object->GetOutermost());
					}
				}
				for (UPackage* Package : ExtraPackages)
				{
					UE::UAI::Contract::FDiagnostic SaveDetail;
					if (!Package || !Package->GetName().StartsWith(
							FPackageName::GetLongPackagePath(Destination) + TEXT("/"))
						|| !SavePackage(*Package, SaveDetail))
					{
						Result.Diagnostic = SaveDetail.IsEmpty()
							? ImportFact(TEXT("external_import_related_package_invalid"),
								Package ? TEXT("The imported related package is outside the destination directory.")
									: TEXT("The imported related package is missing."),
								TEXT("/related_package"),
								Package ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Package->GetName()))
									: TSharedPtr<FJsonValue>(MakeShared<FJsonValueNull>()),
								MakeShared<FJsonValueString>(FPackageName::GetLongPackagePath(Destination) + TEXT("/")))
							: SaveDetail;
						RollbackImported(Durable, Imported, Result);
						return Result;
					}
				}
				if (!Durable.Persist(Error))
				{
					Result.Diagnostic = Error;
					RollbackImported(Durable, Imported, Result);
					return Result;
				}
				Result.bPersisted = true;
				Result.bChanged = true;
				Result.VerificationId =
					TEXT("external-import-readback:") + Request.RequestId;
				UObject* const Reloaded = ReloadForIndependentReadback(Destination, Error);
				Result.bReadbackVerified = Reloaded && Reloaded->IsA(ExpectedClass);
				if (Reloaded)
					Result.ClassPath = Reloaded->GetClass()->GetPathName();
				if (!Result.bReadbackVerified && Error.IsEmpty())
				{
					Error = ImportFact(TEXT("external_import_readback_class_mismatch"),
						TEXT("The independently reloaded asset does not have the expected native class."),
						TEXT("/readback_class"),
						Reloaded ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Reloaded->GetClass()->GetPathName()))
							: TSharedPtr<FJsonValue>(MakeShared<FJsonValueNull>()),
						MakeShared<FJsonValueString>(ExpectedClass->GetPathName()), TEXT("verification"));
					Error.TargetPath = Destination;
				}
				if (!Result.bReadbackVerified
					|| !Durable.Acknowledge(Result.VerificationId, Error))
				{
					Result.TerminalState = TEXT("state_unknown");
					Result.Diagnostic = Error;
					return Result;
				}
				Result.PersistenceScope = TEXT("package");
				Result.TerminalState = TEXT("committed");
				return Result;
			}

			FUeAgentExternalDataImportResult ImportSkinWeightProfile(
				const FUeAgentExternalDataImportRequest& Request,
				const FString& Source,
				FUeAgentExternalDataImportResult Result)
			{
				FString PackageName;
				if (!TryPackageName(Request.TargetRef, PackageName))
				{
					Result.Diagnostic = ImportInput(TEXT("external_skin_profile_target_invalid"),
						TEXT("The skin weight profile target must resolve to a valid skeletal mesh package below /Game/."),
						TEXT("target_ref"), Request.TargetRef, TEXT("a valid /Game/ asset package or object path"));
					return Result;
				}
				USkeletalMesh* const Mesh = LoadObject<USkeletalMesh>(
					nullptr, *ObjectPath(PackageName));
				if (!Mesh)
				{
					Result.Diagnostic = ImportInput(TEXT("external_skin_profile_target_missing"),
						TEXT("The skin weight profile target could not be loaded as a SkeletalMesh asset."),
						TEXT("target_ref"), Request.TargetRef, USkeletalMesh::StaticClass()->GetPathName());
					Result.Diagnostic.TargetPath = PackageName;
					return Result;
				}
				if (Request.LodIndex < 0 || Request.LodIndex >= Mesh->GetLODNum())
				{
					Result.Diagnostic = ImportFact(TEXT("external_skin_profile_lod_invalid"),
						TEXT("The skin weight profile LOD index is outside the target mesh LOD range."),
						TEXT("/lod_index"), MakeShared<FJsonValueNumber>(Request.LodIndex),
						MakeShared<FJsonValueString>(FString::Printf(TEXT("0 <= lod_index < %d"), Mesh->GetLODNum())), TEXT("parameters"));
					Result.Diagnostic.SourceKind = TEXT("command_input");
					Result.Diagnostic.Source.JsonPath = TEXT("/params/lod_index");
					Result.Diagnostic.TargetPath = PackageName;
					return Result;
				}
				const FName ProfileName(*Request.ProfileName);
				if (ProfileName.IsNone()
					|| Mesh->GetSkinWeightProfiles().ContainsByPredicate(
						[ProfileName](const FSkinWeightProfileInfo& Profile)
						{
							return Profile.Name == ProfileName;
						}))
				{
					Result.Diagnostic = ImportInput(TEXT("external_skin_profile_name_conflict"),
						ProfileName.IsNone() ? TEXT("The skin weight profile name resolves to the reserved None name.")
							: TEXT("A skin weight profile with this name already exists on the target mesh."),
						TEXT("profile_name"), Request.ProfileName, TEXT("a non-None profile name unique on the target mesh"));
					Result.Diagnostic.TargetPath = PackageName;
					return Result;
				}
				FDurableImport Durable;
				UE::UAI::Contract::FDiagnostic Error;
				if (!Durable.Prepare(
						Request, PackageName, OwnerId, JournalRoot,
						Counter.Increment(), Error))
				{
					Result.Diagnostic = Error;
					return Result;
				}
				Result.JournalId = Durable.JournalId();
				Mesh->Modify();
				if (!FSkinWeightsUtilities::ImportAlternateSkinWeight(
						Mesh, Source, Request.LodIndex, ProfileName, false))
				{
					Result.Diagnostic = ImportCondition(TEXT("external_skin_profile_native_failed"),
						TEXT("The native alternate skin weight import failed for the selected profile and LOD."),
						FString::Printf(TEXT("/lods/%d/skin_weight_profiles"), Request.LodIndex), false, true, Source);
					Result.Diagnostic.CandidateName = Request.ProfileName;
					Result.Diagnostic.TargetPath = PackageName;
					RollbackSkinProfile(Durable, Result, false);
					return Result;
				}
				Mesh->PostEditChange();
				Mesh->MarkPackageDirty();
				if (!Durable.Persist(Error))
				{
					Result.Diagnostic = Error;
					RollbackSkinProfile(Durable, Result, true);
					return Result;
				}
				Result.bChanged = true;
				Result.bPersisted = true;
				Result.VerificationId =
					TEXT("external-skin-profile-readback:") + Request.RequestId;
				UObject* const ReloadedObject = ReloadForIndependentReadback(PackageName, Error);
				USkeletalMesh* const Reloaded = Cast<USkeletalMesh>(ReloadedObject);
				Result.bReadbackVerified = Reloaded
					&& Reloaded->GetSkinWeightProfiles().ContainsByPredicate(
						[ProfileName](const FSkinWeightProfileInfo& Profile)
						{
							return Profile.Name == ProfileName;
						});
				if (!Result.bReadbackVerified && Error.IsEmpty())
				{
					if (!Reloaded)
						Error = ImportFact(TEXT("external_skin_profile_readback_class_mismatch"),
							TEXT("The independently reloaded skin weight target is not a SkeletalMesh asset."),
							TEXT("/readback_class"),
							ReloadedObject ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(ReloadedObject->GetClass()->GetPathName()))
								: TSharedPtr<FJsonValue>(MakeShared<FJsonValueNull>()),
							MakeShared<FJsonValueString>(USkeletalMesh::StaticClass()->GetPathName()), TEXT("verification"));
					else
					{
						Error = ImportCondition(TEXT("external_skin_profile_readback_profile_missing"),
							TEXT("The requested skin weight profile is absent after independently reloading the target mesh."),
							TEXT("/skin_weight_profile_present"), false, true);
						Error.Stage = TEXT("verification");
						Error.CandidateName = Request.ProfileName;
					}
					Error.TargetPath = PackageName;
				}
				if (!Result.bReadbackVerified
					|| !Durable.Acknowledge(Result.VerificationId, Error))
				{
					Result.TerminalState = TEXT("state_unknown");
					Result.Diagnostic = Error;
					return Result;
				}
				Result.ResultRef = PackageName;
				Result.ClassPath = USkeletalMesh::StaticClass()->GetPathName();
				Result.PersistenceScope = TEXT("package");
				Result.TerminalState = TEXT("committed");
				return Result;
			}

			FUeAgentExternalDataImportResult ImportLocalization(
				const FUeAgentExternalDataImportRequest& Request,
				const FString& Source,
				FUeAgentExternalDataImportResult Result)
			{
				FString Target = Request.TargetRef;
				Target.TrimStartAndEndInline();
				FString Culture;
				FDiagnostic CultureError;
				if (!TryCultureFromPo(Source, Culture, CultureError))
				{
					Result.Diagnostic = MoveTemp(CultureError);
					return Result;
				}
				if (!IsSafeLocalizationSegment(Target))
				{
					Result.Diagnostic = ImportInput(TEXT("external_localization_target_invalid"),
						TEXT("The localization target must be a non-empty segment containing only letters, digits, underscore or hyphen."),
						TEXT("target_ref"), Request.TargetRef, TEXT("non-empty letters, digits, underscore or hyphen"));
					return Result;
				}
				const FString Destination = FPaths::ConvertRelativePathToFull(
					FPaths::ProjectContentDir() / TEXT("Localization")
					/ Target / Culture / FPaths::GetCleanFilename(Source));
				if (FPaths::FileExists(Destination))
				{
					Result.Diagnostic = ImportCondition(TEXT("external_localization_target_conflict"),
						TEXT("The localization destination file already exists."), TEXT("/destination_exists"), true, false, Destination);
					Result.Diagnostic.TargetPath = Destination;
					return Result;
				}
				const FString Temp = Destination + TEXT(".uai-")
					+ FGuid::NewGuid().ToString(EGuidFormats::Digits) + TEXT(".tmp");
				IFileManager::Get().MakeDirectory(*FPaths::GetPath(Destination), true);
				const uint32 CopyResult = IFileManager::Get().Copy(*Temp, *Source, false, true);
				if (CopyResult != COPY_OK)
				{
					Result.Diagnostic = ImportFact(TEXT("external_localization_stage_failed"),
						TEXT("The localization source file could not be copied to the import staging file."),
						TEXT("/copy_result"), MakeShared<FJsonValueNumber>(CopyResult), MakeShared<FJsonValueNumber>(COPY_OK), TEXT("staging"));
					Result.Diagnostic.Source.SourceFile = Source;
					Result.Diagnostic.TargetPath = Destination;
					Result.Diagnostic.InternalContext = Temp;
					return Result;
				}
				if (!IFileManager::Get().Move(
						*Destination, *Temp, false, false, false, true))
				{
					const bool bTempRemoved = IFileManager::Get().Delete(*Temp, false, true);
					Result.Diagnostic = ImportCondition(TEXT("external_localization_publish_failed"),
						TEXT("The staged localization file could not be moved to its destination."),
						TEXT("/file_published"), false, true, Source);
					Result.Diagnostic.Stage = TEXT("publish");
					Result.Diagnostic.TargetPath = Destination;
					Result.Diagnostic.InternalContext = Temp;
					if (!bTempRemoved)
					{
						auto CleanupError = ImportCondition(TEXT("external_localization_cleanup_failed"),
							TEXT("The temporary localization import file could not be removed after publication failed."),
							TEXT("/temporary_file_removed"), false, true);
						CleanupError.Source.WorkspaceSource = Temp;
						CleanupError.Stage = TEXT("cleanup");
						Result.Diagnostic.Append(CleanupError);
					}
					return Result;
				}
				Result.bChanged = true;
				Result.bPersisted = true;
				Result.ResultRef = Destination;
				Result.JournalId = TEXT("external-localization:") + Request.RequestId;
				Result.VerificationId =
					TEXT("external-localization-readback:") + Request.RequestId;
				FString ReadbackHash;
				FDiagnostic ReadbackError;
				const bool bReadbackHashed = TryHashFileSha256(Destination, ReadbackHash, ReadbackError, TEXT("verification"));
				Result.bReadbackVerified = bReadbackHashed && ReadbackHash == Result.SourceSha256;
				if (!Result.bReadbackVerified)
				{
					const bool bRemoved =
						IFileManager::Get().Delete(*Destination, false, true);
					Result.TerminalState = bRemoved
						? TEXT("rolled_back") : TEXT("state_unknown");
					Result.Diagnostic = ImportCondition(TEXT("external_localization_readback_failed"),
						bReadbackHashed ? TEXT("The published localization file differs from the imported source file.")
							: TEXT("The published localization file could not be read and hashed for verification."),
						TEXT("/source_content_matches"), false, true, Destination);
					Result.Diagnostic.Stage = TEXT("verification");
					Result.Diagnostic.TargetPath = Destination;
					Result.Diagnostic.ActualEvidence = MakeShared<FJsonValueString>(ReadbackHash);
					Result.Diagnostic.ExpectedEvidence = MakeShared<FJsonValueString>(Result.SourceSha256);
					Result.Diagnostic.Append(ReadbackError);
					if (!bRemoved) Result.Diagnostic.Append(ImportCondition(TEXT("external_localization_rollback_delete_failed"),
						TEXT("The unverified localization destination file could not be removed during rollback."),
						TEXT("/file_removed"), false, true, Destination));
					Result.Diagnostic.RollbackAttempted = true;
					Result.Diagnostic.RollbackSucceeded = bRemoved;
					Result.Diagnostic.TransactionStatus = Result.TerminalState;
					return Result;
				}
				Result.ClassPath = TEXT("project.localization.po");
				Result.PersistenceScope = TEXT("project_file");
				Result.TerminalState = TEXT("committed");
				return Result;
			}

			FString JournalRoot;
			FString OwnerId;
			FThreadSafeCounter Counter;
		};
	}

	bool TryCreateExternalDataImportOwner(
		FString JournalRoot,
		FString JournalOwnerId,
		TSharedPtr<IUeAgentExternalDataImportOwner>& OutOwner,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutOwner.Reset();
		OutError.Reset();
		JournalRoot.TrimStartAndEndInline();
		JournalOwnerId.TrimStartAndEndInline();
		if (JournalRoot.IsEmpty() || JournalOwnerId.IsEmpty())
		{
			OutError = ImportCondition(TEXT("external_import_owner_config_invalid"),
				JournalRoot.IsEmpty() ? TEXT("The external import journal root configuration is empty.")
					: TEXT("The external import journal owner identifier configuration is empty."),
				JournalRoot.IsEmpty() ? TEXT("/journal_root_present") : TEXT("/journal_owner_present"), false, true);
			OutError.Stage = TEXT("initialization");
			OutError.SubjectId = JournalOwnerId;
			OutError.InternalContext = JournalRoot;
			return false;
		}
		if (!IFileManager::Get().MakeDirectory(*JournalRoot, true)
			&& !IFileManager::Get().DirectoryExists(*JournalRoot))
		{
			OutError = ImportCondition(TEXT("external_import_journal_root_unavailable"),
				TEXT("The external import journal directory could not be created and does not exist."),
				TEXT("/journal_directory_available"), false, true);
			OutError.Stage = TEXT("initialization");
			OutError.InternalContext = JournalRoot;
			return false;
		}
		OutOwner = MakeShared<FExternalDataImportOwner>(
			MoveTemp(JournalRoot), MoveTemp(JournalOwnerId));
		return true;
	}
}

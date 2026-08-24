#include "DomainAdapters/ExternalDataImport/UeAgentExternalDataImportOwner.h"

#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "Animation/SkinWeightProfile.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "AutomatedAssetImportData.h"
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
#include "PackageTools.h"
#include "SkinWeightsUtilities.h"
#include "Sound/SoundWave.h"
#include "SparseVolumeTexture/SparseVolumeTexture.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

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
		FUeAgentResourceTracker MakeTracker()
		{
			return FUeAgentResourceTracker(FUeAgentResourceBudget({
				{EUeAgentResourceDimension::InputBytes, 64 * 1024 * 1024},
				{EUeAgentResourceDimension::OutputBytes, 4 * 1024 * 1024},
				{EUeAgentResourceDimension::StringBytes, 4 * 1024 * 1024},
				{EUeAgentResourceDimension::Nodes, 8192},
				{EUeAgentResourceDimension::Items, 8192},
				{EUeAgentResourceDimension::Files, 512},
				{EUeAgentResourceDimension::ReferenceEdges, 8192},
				{EUeAgentResourceDimension::WorkUnits, 262144},
				{EUeAgentResourceDimension::EstimatedMemoryBytes,
					128 * 1024 * 1024},
			}));
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

		UObject* ReloadForIndependentReadback(const FString& PackageName)
		{
			if (UPackage* const Package = FindPackage(nullptr, *PackageName))
			{
				Package->SetDirtyFlag(false);
				FText Error;
				if (!UPackageTools::UnloadPackages({Package}, Error, true))
				{
					return nullptr;
				}
			}
			CollectGarbage(RF_NoFlags, true);
			return LoadObject<UObject>(nullptr, *ObjectPath(PackageName));
		}

		bool TryNormalizeSource(
			FString SourceRef,
			const TSet<FString>& AllowedExtensions,
			FString& OutSource,
			FString& OutError)
		{
			SourceRef.TrimStartAndEndInline();
			if (SourceRef.IsEmpty())
			{
				OutError = TEXT("external_import_source_missing");
				return false;
			}
			OutSource = FPaths::IsRelative(SourceRef)
				? FPaths::ConvertRelativePathToFull(FPaths::ProjectDir(), SourceRef)
				: FPaths::ConvertRelativePathToFull(SourceRef);
			FPaths::NormalizeFilename(OutSource);
			if (!FPaths::FileExists(OutSource))
			{
				OutError = TEXT("external_import_source_not_found");
				return false;
			}
			const FString Extension = FPaths::GetExtension(OutSource).ToLower();
			if (!AllowedExtensions.Contains(Extension))
			{
				OutError = TEXT("external_import_source_extension_invalid:")
					+ Extension;
				return false;
			}
			return true;
		}

		FString HashFileSha256(const FString& Filename)
		{
			TArray<uint8> Bytes;
			if (!FFileHelper::LoadFileToArray(Bytes, *Filename))
			{
				return FString();
			}
			FSHA256Signature Signature{};
			const uint8 Empty = 0;
			const uint8* Data = Bytes.IsEmpty() ? &Empty : Bytes.GetData();
			if (!SHA256(Data, static_cast<size_t>(Bytes.Num()), Signature.Signature))
			{
				return FString();
			}
			return TEXT("sha256:") + Signature.ToString().ToLower();
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
				FString& OutError)
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
					OutError = Error.GetCode() + TEXT(":") + Error.GetDetail();
					return false;
				}
				return true;
			}

			bool Persist(FString& OutError)
			{
				FAssetDurableError Error;
				if (!TryPersistAssetDurability(
						Session.ToSharedRef(), Backend, Token, Resources,
						Receipt, Error))
				{
					OutError = Error.GetCode() + TEXT(":") + Error.GetDetail();
					return false;
				}
				return true;
			}

			bool Acknowledge(const FString& VerificationId, FString& OutError)
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
					OutError = Error.GetCode() + TEXT(":") + Error.GetDetail();
					return false;
				}
				return true;
			}

			void Rollback(const bool bMemoryConfirmed)
			{
				if (!Session.IsValid()) return;
				FDurableReceipt RollbackReceipt;
				FAssetDurableError Error;
				TryRollbackAssetDurability(
					Session.ToSharedRef(), Backend, Token, bMemoryConfirmed,
					Resources, RollbackReceipt, Error);
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

		bool DeleteImportedObjects(const TArray<UObject*>& Objects)
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
					bRemoved = IFileManager::Get().Delete(*Filename, false, true)
						&& bRemoved;
				}
			}
			return bRemoved;
		}

		bool SavePackage(UPackage& Package)
		{
			const FString Filename = FPackageName::LongPackageNameToFilename(
				Package.GetName(), FPackageName::GetAssetPackageExtension());
			FSavePackageArgs Args;
			Args.TopLevelFlags = RF_Public | RF_Standalone;
			return UPackage::SavePackage(&Package, nullptr, *Filename, Args);
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
			FString& OutError)
		{
			if (Request.CommandId != TEXT("external.fbx.animation.import")
				&& Request.CommandId
					!= TEXT("external.fbx.skeletal_mesh.import"))
			{
				return nullptr;
			}
			UFbxFactory* const Factory = NewObject<UFbxFactory>();
			if (!Factory || !Factory->ImportUI)
			{
				OutError = TEXT("external_import_fbx_factory_unavailable");
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
					OutError = TEXT("external_import_skeleton_not_found");
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
			if (CommandId == TEXT("external.geometry_cache.import"))
				return {TEXT("abc")};
			if (CommandId == TEXT("external.texture.import"))
				return {TEXT("png"), TEXT("jpg"), TEXT("jpeg"), TEXT("tga"),
					TEXT("bmp"), TEXT("exr"), TEXT("hdr"), TEXT("dds"),
					TEXT("psd"), TEXT("tif"), TEXT("tiff")};
			if (CommandId == TEXT("external.light_profile.import"))
				return {TEXT("ies")};
			if (CommandId == TEXT("external.sparse_volume_texture.import"))
				return {TEXT("vdb")};
			if (CommandId == TEXT("external.sound_wave.import"))
				return {TEXT("wav")};
			if (CommandId == TEXT("external.localization.po.import"))
				return {TEXT("po")};
			return {};
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

		bool TryCultureFromPo(const FString& Source, FString& OutCulture)
		{
			FString Text;
			if (!FFileHelper::LoadFileToString(Text, *Source)) return false;
			TArray<FString> Lines;
			Text.ParseIntoArrayLines(Lines, false);
			for (FString Line : Lines)
			{
				Line.TrimStartAndEndInline();
				Line.RemoveFromStart(TEXT("\""));
				if (!Line.StartsWith(TEXT("Language:"))) continue;
				OutCulture = Line.Mid(9).TrimStartAndEnd();
				OutCulture.RemoveFromEnd(TEXT("\\n\""));
				OutCulture.RemoveFromEnd(TEXT("\""));
				OutCulture.TrimStartAndEndInline();
				return IsSafeLocalizationSegment(OutCulture);
			}
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
				auto Fail = [&Result](const FString& Code, const FString& Detail = FString())
				{
					Result.ErrorCode = Code;
					Result.ErrorDetail = Detail;
					return Result;
				};
				if (!IsInGameThread())
					return Fail(TEXT("external_import_game_thread_required"));
				if (Cancellation->IsCancelled())
					return Fail(TEXT("external_import_cancelled"), Request.RequestId);
				const TSet<FString> Extensions = ExtensionsFor(Request.CommandId);
				if (Extensions.IsEmpty())
					return Fail(TEXT("external_import_command_unknown"), Request.CommandId);
				FString Source;
				FString Error;
				if (!TryNormalizeSource(Request.SourceRef, Extensions, Source, Error))
					return Fail(Error);
				Result.SourceRef = Source;
				Result.SourceSha256 = HashFileSha256(Source);
				if (Result.SourceSha256.IsEmpty())
					return Fail(TEXT("external_import_source_hash_failed"));
				if (Request.CommandId == TEXT("external.localization.po.import"))
					return ImportLocalization(Request, Source, MoveTemp(Result));
				if (Request.CommandId == TEXT("external.skin_weight_profile.import"))
					return ImportSkinWeightProfile(Request, Source, MoveTemp(Result));
				return ImportAsset(Request, Source, MoveTemp(Result));
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
					Result.ErrorCode = TEXT("external_import_destination_invalid");
					return Result;
				}
				Result.ResultRef = Destination;
				if (PackageExists(Destination))
				{
					Result.ErrorCode = TEXT("external_import_destination_conflict");
					return Result;
				}
				const FString DestinationName =
					FPackageName::GetLongPackageAssetName(Destination);
				if (FPaths::GetBaseFilename(Source) != DestinationName)
				{
					Result.ErrorCode = TEXT("external_import_source_name_mismatch");
					Result.ErrorDetail =
						TEXT("source basename must match destination asset name");
					return Result;
				}
				const UClass* const ExpectedClass = ExpectedClassFor(Request.CommandId);
				if (!ExpectedClass)
				{
					Result.ErrorCode = TEXT("external_import_expected_class_missing");
					return Result;
				}
				FString FactoryError;
				UFactory* const Factory = CreateFactory(Request, FactoryError);
				if (Request.CommandId.StartsWith(TEXT("external.fbx.")) && !Factory)
				{
					Result.ErrorCode = FactoryError.IsEmpty()
						? TEXT("external_import_fbx_factory_failed") : FactoryError;
					return Result;
				}

				FDurableImport Durable;
				FString Error;
				if (!Durable.Prepare(
						Request, Destination, OwnerId, JournalRoot,
						Counter.Increment(), Error))
				{
					Result.ErrorCode = TEXT("external_import_prepare_failed");
					Result.ErrorDetail = Error;
					return Result;
				}
				Result.JournalId = Durable.JournalId();
				UAutomatedAssetImportData* const ImportData =
					NewObject<UAutomatedAssetImportData>();
				if (!ImportData)
				{
					Durable.Rollback(true);
					Result.ErrorCode = TEXT("external_import_data_unavailable");
					return Result;
				}
				ImportData->AddToRoot();
				if (Factory) Factory->AddToRoot();
				ImportData->Filenames = {Source};
				ImportData->DestinationPath =
					FPackageName::GetLongPackagePath(Destination);
				ImportData->bReplaceExisting = false;
				if (Factory)
				{
					ImportData->FactoryName = Factory->GetClass()->GetName();
					ImportData->Factory = Factory;
				}
				TArray<UObject*> Imported;
				{
					TGuardValue<bool> Guard(GIsRunningUnattendedScript, true);
					Imported = FModuleManager::LoadModuleChecked<FAssetToolsModule>(
						TEXT("AssetTools")).Get().ImportAssetsAutomated(ImportData);
				}
				ImportData->RemoveFromRoot();
				if (Factory) Factory->RemoveFromRoot();
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
					const bool bRemoved = DeleteImportedObjects(Imported);
					Durable.Rollback(bRemoved);
					Result.TerminalState = bRemoved
						? TEXT("rolled_back") : TEXT("state_unknown");
					Result.ErrorCode = TEXT("external_import_native_failed");
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
					if (!Package || !Package->GetName().StartsWith(
							FPackageName::GetLongPackagePath(Destination) + TEXT("/"))
						|| !SavePackage(*Package))
					{
						const bool bRemoved = DeleteImportedObjects(Imported);
						Durable.Rollback(bRemoved);
						Result.TerminalState = bRemoved
							? TEXT("rolled_back") : TEXT("state_unknown");
						Result.ErrorCode = TEXT("external_import_related_persist_failed");
						return Result;
					}
				}
				if (!Durable.Persist(Error))
				{
					const bool bRemoved = DeleteImportedObjects(Imported);
					Durable.Rollback(bRemoved);
					Result.TerminalState = bRemoved
						? TEXT("rolled_back") : TEXT("state_unknown");
					Result.ErrorCode = TEXT("external_import_persist_failed");
					Result.ErrorDetail = Error;
					return Result;
				}
				Result.bPersisted = true;
				Result.bChanged = true;
				Result.VerificationId =
					TEXT("external-import-readback:") + Request.RequestId;
				UObject* const Reloaded = ReloadForIndependentReadback(Destination);
				Result.bReadbackVerified = Reloaded && Reloaded->IsA(ExpectedClass);
				if (Reloaded)
					Result.ClassPath = Reloaded->GetClass()->GetPathName();
				if (!Result.bReadbackVerified
					|| !Durable.Acknowledge(Result.VerificationId, Error))
				{
					Result.TerminalState = TEXT("state_unknown");
					Result.ErrorCode = TEXT("external_import_readback_failed");
					Result.ErrorDetail = Error;
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
					Result.ErrorCode = TEXT("external_skin_profile_target_invalid");
					return Result;
				}
				USkeletalMesh* const Mesh = LoadObject<USkeletalMesh>(
					nullptr, *ObjectPath(PackageName));
				if (!Mesh)
				{
					Result.ErrorCode = TEXT("external_skin_profile_target_missing");
					return Result;
				}
				if (Request.LodIndex < 0 || Request.LodIndex >= Mesh->GetLODNum())
				{
					Result.ErrorCode = TEXT("external_skin_profile_lod_invalid");
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
					Result.ErrorCode = TEXT("external_skin_profile_name_conflict");
					return Result;
				}
				FDurableImport Durable;
				FString Error;
				if (!Durable.Prepare(
						Request, PackageName, OwnerId, JournalRoot,
						Counter.Increment(), Error))
				{
					Result.ErrorCode = TEXT("external_skin_profile_prepare_failed");
					Result.ErrorDetail = Error;
					return Result;
				}
				Result.JournalId = Durable.JournalId();
				Mesh->Modify();
				if (!FSkinWeightsUtilities::ImportAlternateSkinWeight(
						Mesh, Source, Request.LodIndex, ProfileName, false))
				{
					Durable.Rollback(false);
					Result.TerminalState = TEXT("state_unknown");
					Result.ErrorCode = TEXT("external_skin_profile_native_failed");
					return Result;
				}
				Mesh->PostEditChange();
				Mesh->MarkPackageDirty();
				if (!Durable.Persist(Error))
				{
					Durable.Rollback(false);
					Result.TerminalState = TEXT("state_unknown");
					Result.ErrorCode = TEXT("external_skin_profile_persist_failed");
					Result.ErrorDetail = Error;
					return Result;
				}
				Result.bChanged = true;
				Result.bPersisted = true;
				Result.VerificationId =
					TEXT("external-skin-profile-readback:") + Request.RequestId;
				USkeletalMesh* const Reloaded = Cast<USkeletalMesh>(
					ReloadForIndependentReadback(PackageName));
				Result.bReadbackVerified = Reloaded
					&& Reloaded->GetSkinWeightProfiles().ContainsByPredicate(
						[ProfileName](const FSkinWeightProfileInfo& Profile)
						{
							return Profile.Name == ProfileName;
						});
				if (!Result.bReadbackVerified
					|| !Durable.Acknowledge(Result.VerificationId, Error))
				{
					Result.TerminalState = TEXT("state_unknown");
					Result.ErrorCode = TEXT("external_skin_profile_readback_failed");
					Result.ErrorDetail = Error;
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
				if (!TryCultureFromPo(Source, Culture)
					|| !IsSafeLocalizationSegment(Target))
				{
					Result.ErrorCode = TEXT("external_localization_target_invalid");
					return Result;
				}
				const FString Destination = FPaths::ConvertRelativePathToFull(
					FPaths::ProjectContentDir() / TEXT("Localization")
					/ Target / Culture / FPaths::GetCleanFilename(Source));
				if (FPaths::FileExists(Destination))
				{
					Result.ErrorCode = TEXT("external_localization_target_conflict");
					return Result;
				}
				const FString Temp = Destination + TEXT(".uai-")
					+ FGuid::NewGuid().ToString(EGuidFormats::Digits) + TEXT(".tmp");
				IFileManager::Get().MakeDirectory(*FPaths::GetPath(Destination), true);
				if (IFileManager::Get().Copy(*Temp, *Source, false, true)
					!= COPY_OK)
				{
					Result.ErrorCode = TEXT("external_localization_stage_failed");
					return Result;
				}
				if (!IFileManager::Get().Move(
						*Destination, *Temp, false, false, false, true))
				{
					IFileManager::Get().Delete(*Temp, false, true);
					Result.ErrorCode = TEXT("external_localization_publish_failed");
					return Result;
				}
				Result.bChanged = true;
				Result.bPersisted = true;
				Result.ResultRef = Destination;
				Result.JournalId = TEXT("external-localization:") + Request.RequestId;
				Result.VerificationId =
					TEXT("external-localization-readback:") + Request.RequestId;
				Result.bReadbackVerified =
					HashFileSha256(Destination) == Result.SourceSha256;
				if (!Result.bReadbackVerified)
				{
					const bool bRemoved =
						IFileManager::Get().Delete(*Destination, false, true);
					Result.TerminalState = bRemoved
						? TEXT("rolled_back") : TEXT("state_unknown");
					Result.ErrorCode = TEXT("external_localization_readback_failed");
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
		FString& OutError)
	{
		OutOwner.Reset();
		OutError.Reset();
		JournalRoot.TrimStartAndEndInline();
		JournalOwnerId.TrimStartAndEndInline();
		if (JournalRoot.IsEmpty() || JournalOwnerId.IsEmpty())
		{
			OutError = TEXT("external_import_owner_config_invalid");
			return false;
		}
		if (!IFileManager::Get().MakeDirectory(*JournalRoot, true)
			&& !IFileManager::Get().DirectoryExists(*JournalRoot))
		{
			OutError = TEXT("external_import_journal_root_unavailable");
			return false;
		}
		OutOwner = MakeShared<FExternalDataImportOwner>(
			MoveTemp(JournalRoot), MoveTemp(JournalOwnerId));
		return true;
	}
}

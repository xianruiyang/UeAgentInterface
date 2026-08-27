#include "DomainAdapters/AssetDestructiveImport/UeAgentAssetDeleteOwner.h"

#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/AnimSequenceBase.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Core/AssetDurable/UeAgentAssetDurable.h"
#include "Core/ContextResource/UeAgentContextResource.h"
#include "Core/Contract/UeAgentContractJson.h"
#include "Core/PlanningPreflight/UeAgentPlanningPreflight.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "DomainAdapters/ContentOperations/UeAgentDestructiveContentPlanningBoundary.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "HAL/ThreadSafeCounter.h"
#include "Internationalization/StringTable.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "ObjectTools.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"

namespace UE::UAI::DomainAdapters::AssetDestructiveImport
{
	using namespace AssetDurable;
	using namespace ContextResource;
	using namespace Contract;
	using namespace DomainAdapters::ContentOperations;
	using namespace PlanningPreflight;
	using namespace StateSemantic;

	namespace
	{
		FUeAgentResourceTracker MakeTracker()
		{
			return FUeAgentResourceTracker(FUeAgentResourceBudget({
				{EUeAgentResourceDimension::InputBytes, 1024 * 1024},
				{EUeAgentResourceDimension::OutputBytes, 1024 * 1024},
				{EUeAgentResourceDimension::StringBytes, 1024 * 1024},
				{EUeAgentResourceDimension::Nodes, 4096},
				{EUeAgentResourceDimension::Items, 4096},
				{EUeAgentResourceDimension::Files, 256},
				{EUeAgentResourceDimension::ReferenceEdges, 4096},
				{EUeAgentResourceDimension::WorkUnits, 65536},
				{EUeAgentResourceDimension::EstimatedMemoryBytes, 16 * 1024 * 1024},
			}));
		}

		bool TryNormalizeTarget(
			FString TargetRef,
			FString& OutPackageName,
			FString& OutObjectPath)
		{
			TargetRef.TrimStartAndEndInline();
			OutPackageName.Reset();
			OutObjectPath.Reset();
			if (TargetRef.Contains(TEXT(".")))
			{
				OutPackageName = FPackageName::ObjectPathToPackageName(TargetRef);
				OutObjectPath = MoveTemp(TargetRef);
			}
			else
			{
				OutPackageName = MoveTemp(TargetRef);
			}
			if (!OutPackageName.StartsWith(TEXT("/Game/"))
				|| !FPackageName::IsValidLongPackageName(OutPackageName, true))
			{
				OutPackageName.Reset();
				return false;
			}
			return true;
		}

		bool TryResolveTargetObjectPath(
			const FString& PackageName,
			FString& InOutObjectPath,
			bool& bOutRegistryQuerySucceeded,
			TArray<FAssetData>& OutRegistryAssets,
			FString& OutErrorCode,
			FString& OutErrorDetail)
		{
			bOutRegistryQuerySucceeded = false;
			OutRegistryAssets.Reset();
			OutErrorCode.Reset();
			OutErrorDetail.Reset();
			const bool bPackageNameInput = InOutObjectPath.IsEmpty();
			bOutRegistryQuerySucceeded =
				FAssetRegistryModule::GetRegistry().GetAssetsByPackageName(
					FName(*PackageName), OutRegistryAssets, false);
			if (!bOutRegistryQuerySucceeded)
			{
				if (bPackageNameInput)
				{
					OutErrorCode = TEXT("asset_delete_registry_query_failed");
					OutErrorDetail = PackageName;
					return false;
				}
				return true;
			}
			if (!bPackageNameInput)
			{
				return true;
			}
			if (OutRegistryAssets.IsEmpty())
			{
				OutErrorCode = TEXT("asset_delete_target_missing");
				OutErrorDetail = PackageName;
				return false;
			}
			if (OutRegistryAssets.Num() != 1)
			{
				TArray<FString> ObjectPaths;
				ObjectPaths.Reserve(OutRegistryAssets.Num());
				for (const FAssetData& Asset : OutRegistryAssets)
				{
					ObjectPaths.Add(Asset.GetSoftObjectPath().ToString());
				}
				ObjectPaths.Sort();
				OutErrorCode = TEXT("asset_delete_target_ambiguous");
				OutErrorDetail = PackageName + TEXT(":")
					+ FString::Join(ObjectPaths, TEXT("|"));
				return false;
			}

			InOutObjectPath =
				OutRegistryAssets[0].GetSoftObjectPath().ToString();
			return true;
		}

		bool TryRequireNoOnDiskReferencers(
			const FString& PackageName,
			FString& OutErrorCode,
			FString& OutDetail)
		{
			OutErrorCode.Reset();
			OutDetail.Reset();
			TArray<FName> Referencers;
			if (!FAssetRegistryModule::GetRegistry().GetReferencers(
					FName(*PackageName), Referencers))
			{
				OutErrorCode = TEXT("asset_delete_referencer_query_failed");
				OutDetail = PackageName;
				return false;
			}
			Referencers.Sort(FNameLexicalLess());
			if (Referencers.IsEmpty())
			{
				return true;
			}
			OutErrorCode = TEXT("asset_delete_package_referenced");
			TArray<FString> StableReferencers;
			StableReferencers.Reserve(Referencers.Num());
			for (const FName Referencer : Referencers)
			{
				StableReferencers.Add(Referencer.ToString());
			}
			OutDetail = PackageName + TEXT(":")
				+ FString::Join(StableReferencers, TEXT("|"));
			return false;
		}

		FPlanningPreflightPlanRef MakeDeletePlan(
			const FUeAgentAssetDeleteRequest& Request,
			const FString& PackageName,
			const FString& JournalOwnerId,
			const TArray<FAssetDurableTarget>& Targets)
		{
			TArray<FPlannedChange> Changes;
			TArray<FImpact> Impacts;
			Changes.Reserve(Targets.Num());
			Impacts.Reserve(Targets.Num());
			for (const FAssetDurableTarget& Target : Targets)
			{
				const FUeAgentCanonicalIdentity& Identity = Target.GetIdentity();
				Changes.Emplace(
					Identity,
					EChangeIntent::Delete,
					FUeAgentCanonicalValuePtr(),
					FUeAgentCanonicalValuePtr(),
					TArray<FString>{TEXT("uai:asset:delete")},
					TEXT("asset-delete:direct"));
				Impacts.Emplace(
					Identity,
					Identity,
					TEXT("direct"),
					TEXT("asset-delete:direct"));
			}
			const FString Fingerprint = Json::Sha256Utf8(
				Request.RequestId + TEXT(":") + PackageName);
			return MakeShared<FPlanningPreflightPlan>(
				FUeAgentDescriptorIdentity(
					TEXT("uai.asset.delete/v1"),
					1,
					Json::Sha256Utf8(TEXT("uai.asset.delete/v1"))),
				Request.RequestId,
				TEXT("asset-delete:") + Request.RequestId,
				Request.CallerId,
				TArray<FString>{TEXT("uai:asset:delete")},
				TEXT("asset-delete:preflight:") + Request.RequestId,
				FUeAgentStateRevision(JournalOwnerId, TEXT("asset-delete-v1"), 1),
				TEXT("asset-delete:preflight:filesystem-registry"),
				FChangeSet(MoveTemp(Changes)),
				FImpactSet(MoveTemp(Impacts)),
				TArray<FPrecondition>(),
				Fingerprint);
		}

		bool TryReadAbsence(
			const FString& PackageName,
			const FString& ObjectPath,
			FUeAgentAssetDeleteResult& Result,
			FString& OutError)
		{
			OutError.Reset();
			FString ExistingFilename;
			Result.bFileAbsent = !FPackageName::DoesPackageExist(
				PackageName, &ExistingFilename);
			Result.bPackageAbsent = FindPackage(nullptr, *PackageName) == nullptr;
			TArray<FAssetData> Assets;
			if (!FAssetRegistryModule::GetRegistry().GetAssetsByPackageName(
					FName(*PackageName), Assets, false))
			{
				Result.bRegistryAbsent = false;
				OutError = PackageName;
				return false;
			}
			Result.bRegistryAbsent = Assets.IsEmpty();
			Result.bObjectAbsent =
				FindObject<UObject>(nullptr, *ObjectPath) == nullptr;
			return true;
		}

		void RefreshExactRegistryFiles(const TArray<FString>& Filenames)
		{
			if (!Filenames.IsEmpty())
			{
				FAssetRegistryModule::GetRegistry().ScanModifiedAssetFiles(
					Filenames);
			}
		}

		FString ReferencerSummary(UObject* Asset)
		{
			TArray<UObject*> DeleteSet{Asset};
			ObjectTools::AddExtraObjectsToDelete(DeleteSet);
			TArray<FString> Summaries;
			const int32 ObjectLimit = FMath::Min(DeleteSet.Num(), 8);
			Summaries.Reserve(ObjectLimit);
			for (int32 ObjectIndex = 0; ObjectIndex < ObjectLimit; ++ObjectIndex)
			{
				UObject* const Candidate = DeleteSet[ObjectIndex];
				if (!Candidate)
				{
					Summaries.Add(TEXT("<null>"));
					continue;
				}
				bool bIsReferenced = false;
				bool bIsReferencedByUndo = false;
				FReferencerInformationList References;
				ObjectTools::GatherObjectReferencersForDeletion(
					Candidate,
					bIsReferenced,
					bIsReferencedByUndo,
					&References,
					false);
				TArray<FString> External;
				const int32 Limit =
					FMath::Min(References.ExternalReferences.Num(), 8);
				External.Reserve(Limit);
				for (int32 Index = 0; Index < Limit; ++Index)
				{
					const FReferencerInformation& Reference =
						References.ExternalReferences[Index];
					External.Add(Reference.Referencer
						? Reference.Referencer->GetFullName()
						: TEXT("<null>"));
				}
				TArray<FName> OnDiskReferencers;
				FAssetRegistryModule::GetRegistry().GetReferencers(
					Candidate->GetOutermost()->GetFName(), OnDiskReferencers);
				OnDiskReferencers.Sort(FNameLexicalLess());
				TArray<FString> OnDisk;
				const int32 OnDiskLimit = FMath::Min(OnDiskReferencers.Num(), 8);
				OnDisk.Reserve(OnDiskLimit);
				for (int32 Index = 0; Index < OnDiskLimit; ++Index)
				{
					OnDisk.Add(OnDiskReferencers[Index].ToString());
				}
				Summaries.Add(FString::Printf(
					TEXT("%s{referenced=%s,undo=%s,external=%s,on_disk=%s}"),
					*Candidate->GetFullName(),
					bIsReferenced ? TEXT("true") : TEXT("false"),
					bIsReferencedByUndo ? TEXT("true") : TEXT("false"),
					*FString::Join(External, TEXT("|")),
					*FString::Join(OnDisk, TEXT("|"))));
			}
			return FString::Printf(
				TEXT(";delete_set_count=%d;delete_set=%s"),
				DeleteSet.Num(),
				*FString::Join(Summaries, TEXT("||")));
		}

		void ReleaseTransientAssetControllers(UObject* Asset)
		{
			if (UAnimSequenceBase* const Animation =
					Cast<UAnimSequenceBase>(Asset))
			{
				Animation->GetController().SetModel(
					TScriptInterface<IAnimationDataModel>());
			}
		}

		bool HasOnlyEngineStringTableRegistrationReference(UObject* Asset)
		{
			if (!Cast<UStringTable>(Asset))
			{
				return false;
			}
			bool bIsReferenced = false;
			bool bIsReferencedByUndo = false;
			FReferencerInformationList References;
			ObjectTools::GatherObjectReferencersForDeletion(
				Asset,
				bIsReferenced,
				bIsReferencedByUndo,
				&References,
				false);
			if (!bIsReferenced || bIsReferencedByUndo
				|| References.ExternalReferences.IsEmpty())
			{
				return false;
			}
			for (const FReferencerInformation& Reference
				: References.ExternalReferences)
			{
				if (!Reference.Referencer
					|| Reference.Referencer->GetClass()->GetFName()
						!= FName(TEXT("GCObjectReferencer")))
				{
					return false;
				}
			}
			TArray<FName> OnDiskReferencers;
			FAssetRegistryModule::GetRegistry().GetReferencers(
				Asset->GetOutermost()->GetFName(),
				OnDiskReferencers);
			return OnDiskReferencers.IsEmpty();
		}

		bool IsOwnedWorldExternalDeleteSet(
			UObject* Asset,
			const FString& WorldPackageName,
			const TArray<UObject*>& DeleteSet)
		{
			if (!Cast<UWorld>(Asset) || DeleteSet.Num() <= 1)
			{
				return false;
			}
			FString RelativeWorldPath = WorldPackageName;
			if (!RelativeWorldPath.RemoveFromStart(TEXT("/Game/")))
			{
				return false;
			}
			const FString ExternalActorPrefix =
				TEXT("/Game/__ExternalActors__/") + RelativeWorldPath + TEXT("/");
			const FString ExternalObjectPrefix =
				TEXT("/Game/__ExternalObjects__/") + RelativeWorldPath + TEXT("/");
			for (UObject* const Candidate : DeleteSet)
			{
				if (!Candidate || Candidate == Asset)
				{
					continue;
				}
				UPackage* const CandidatePackage = Candidate->GetOutermost();
				const FString CandidatePackageName = CandidatePackage
					? CandidatePackage->GetName()
					: FString();
				if (!CandidatePackageName.StartsWith(ExternalActorPrefix)
					&& !CandidatePackageName.StartsWith(ExternalObjectPrefix))
				{
					return false;
				}
			}
			return true;
		}

		class FAssetDeleteOwner final : public IUeAgentAssetDeleteOwner
		{
		public:
			FAssetDeleteOwner(FString InJournalRoot, FString InJournalOwnerId)
				: JournalRoot(MoveTemp(InJournalRoot))
				, JournalOwnerId(MoveTemp(InJournalOwnerId))
			{
			}

			FUeAgentAssetDeleteResult Delete(
				const FUeAgentAssetDeleteRequest& Request,
				const TSharedRef<IUeAgentCancellationSignal>& Cancellation) override
			{
				FUeAgentAssetDeleteResult Result;
				Result.TargetRef = Request.TargetRef;
				auto Fail = [&Result](const TCHAR* Code, const FString& Detail)
				{
					Result.ErrorCode = Code;
					Result.ErrorDetail = Detail;
					return Result;
				};
				if (!IsInGameThread())
				{
					return Fail(TEXT("asset_delete_game_thread_required"),
						TEXT("native asset deletion must execute on the game thread"));
				}
				if (Cancellation->IsCancelled())
				{
					return Fail(TEXT("asset_delete_cancelled"), Request.RequestId);
				}
				FString ObjectPath;
				if (!TryNormalizeTarget(
						Request.TargetRef, Result.PackageName, ObjectPath))
				{
					return Fail(TEXT("asset_delete_target_invalid"), Request.TargetRef);
				}

				FString ExistingFilename;
				if (!FPackageName::DoesPackageExist(
						Result.PackageName, &ExistingFilename))
				{
					FString ReadbackError;
					if (!TryReadAbsence(
							Result.PackageName, ObjectPath, Result, ReadbackError))
					{
						return Fail(
							TEXT("asset_delete_registry_query_failed"),
							ReadbackError);
					}
					return Fail(TEXT("asset_delete_target_missing"), Result.PackageName);
				}
				FString ResolutionErrorCode;
				FString ResolutionErrorDetail;
				bool bRegistryQuerySucceeded = false;
				TArray<FAssetData> ResolvedAssets;
				if (!TryResolveTargetObjectPath(
						Result.PackageName,
						ObjectPath,
						bRegistryQuerySucceeded,
						ResolvedAssets,
						ResolutionErrorCode,
						ResolutionErrorDetail))
				{
					return Fail(*ResolutionErrorCode, ResolutionErrorDetail);
				}
				UPackage* const LoadedPackage = FindPackage(
					nullptr, *Result.PackageName);
				UObject* const LoadedObject = FindObject<UObject>(
					nullptr, *ObjectPath);
				const bool bFullyUnloaded = LoadedPackage == nullptr
					&& LoadedObject == nullptr;
				if (bFullyUnloaded && !bRegistryQuerySucceeded)
				{
					return Fail(
						TEXT("asset_delete_registry_query_failed"),
						Result.PackageName);
				}
				if (bFullyUnloaded && ResolvedAssets.IsEmpty())
				{
					return Fail(TEXT("asset_delete_target_missing"),
						Result.PackageName);
				}
				if (bFullyUnloaded && ResolvedAssets.Num() != 1)
				{
					TArray<FString> ObjectPaths;
					ObjectPaths.Reserve(ResolvedAssets.Num());
					for (const FAssetData& ResolvedAsset : ResolvedAssets)
					{
						ObjectPaths.Add(
							ResolvedAsset.GetSoftObjectPath().ToString());
					}
					ObjectPaths.Sort();
					return Fail(TEXT("asset_delete_target_ambiguous"),
						Result.PackageName + TEXT(":")
						+ FString::Join(ObjectPaths, TEXT("|")));
				}
				if (bFullyUnloaded
					&& ResolvedAssets[0].GetSoftObjectPath().ToString()
						!= ObjectPath)
				{
					return Fail(
						TEXT("asset_delete_registry_identity_mismatch"),
						ObjectPath + TEXT(":")
						+ ResolvedAssets[0].GetSoftObjectPath().ToString());
				}
				const bool bPackageOnlyDelete = bFullyUnloaded;
				if (bPackageOnlyDelete)
				{
					FString ReferencerErrorCode;
					FString ReferencerDetail;
					if (!TryRequireNoOnDiskReferencers(
							Result.PackageName,
							ReferencerErrorCode,
							ReferencerDetail))
					{
						return Fail(
							*ReferencerErrorCode,
							ReferencerDetail);
					}
				}
				UObject* Asset = bPackageOnlyDelete
					? nullptr
					: IsValid(LoadedObject)
						? LoadedObject
						: LoadObject<UObject>(nullptr, *ObjectPath);
				if (!IsValid(Asset) && !bPackageOnlyDelete)
				{
					return Fail(TEXT("asset_delete_target_missing"), ObjectPath);
				}
				UPackage* Package = Asset ? Asset->GetOutermost() : nullptr;
				if ((!Package && !bPackageOnlyDelete)
					|| (Package && Package->IsDirty()))
				{
					return Fail(TEXT("asset_delete_dirty_package"), Result.PackageName);
				}
				if (GEditor)
				{
					if (UAssetEditorSubsystem* const Editors =
						GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
					{
						for (UObject* const Edited : Editors->GetAllEditedAssets())
						{
							if (Edited && Edited->GetOutermost() == Package)
							{
								return Fail(TEXT("asset_delete_asset_editor_open"),
									ObjectPath);
							}
						}
					}
				}

				FUeAgentResourceTracker Resources = MakeTracker();
				TArray<UObject*> NativeDeleteSet;
				if (Asset)
				{
					NativeDeleteSet.Add(Asset);
					ObjectTools::AddExtraObjectsToDelete(NativeDeleteSet);
				}
				TArray<FString> OwnedExternalPackages;
				FString ExternalPackageError;
				if (!TryCollectOwnedWorldExternalPackages(
						Result.PackageName,
						4096,
						OwnedExternalPackages,
						ExternalPackageError))
				{
					return Fail(TEXT("asset_delete_external_scope_failed"),
						ExternalPackageError);
				}
				for (const FString& ExternalPackageName : OwnedExternalPackages)
				{
					if (bPackageOnlyDelete)
					{
						continue;
					}
					UPackage* const ExternalPackage = LoadPackage(
						nullptr, *ExternalPackageName, LOAD_None);
					if (!ExternalPackage)
					{
						return Fail(TEXT("asset_delete_external_package_load_failed"),
							ExternalPackageName);
					}
					NativeDeleteSet.AddUnique(ExternalPackage);
				}
				TArray<FAssetDurableTarget> DurableTargets;
				TSet<FString> DurablePackageNames;
				for (UObject* const Candidate : NativeDeleteSet)
				{
					UPackage* const CandidatePackage = Candidate
						? Candidate->GetOutermost()
						: nullptr;
					const FString CandidatePackageName = CandidatePackage
						? CandidatePackage->GetName()
						: FString();
					if (!CandidatePackageName.StartsWith(TEXT("/Game/"))
						|| DurablePackageNames.Contains(CandidatePackageName))
					{
						continue;
					}
					if (CandidatePackage->IsDirty())
					{
						return Fail(TEXT("asset_delete_dirty_package"),
							CandidatePackageName);
					}
					DurablePackageNames.Add(CandidatePackageName);
					DurableTargets.Emplace(
						FUeAgentCanonicalIdentity(
							TEXT("uai.asset.presence"),
							TEXT("asset"),
							CandidatePackageName,
							TEXT("presence"),
							TEXT("asset"),
							TEXT("asset-delete-v1")),
						CandidatePackageName,
						EPackageHeaderKind::EvidenceRequired,
						EAssetDurableIntent::Delete,
						ERollbackGrade::DurableBackup);
				}
				if (bPackageOnlyDelete)
				{
					DurablePackageNames.Add(Result.PackageName);
					DurableTargets.Emplace(
						FUeAgentCanonicalIdentity(
							TEXT("uai.asset.presence"),
							TEXT("asset"),
							Result.PackageName,
							TEXT("presence"),
							TEXT("asset"),
							TEXT("asset-delete-v1")),
						Result.PackageName,
						EPackageHeaderKind::EvidenceRequired,
						EAssetDurableIntent::Delete,
						ERollbackGrade::DurableBackup);
					for (const FString& ExternalPackageName
						: OwnedExternalPackages)
					{
						if (DurablePackageNames.Contains(ExternalPackageName))
						{
							continue;
						}
						DurablePackageNames.Add(ExternalPackageName);
						DurableTargets.Emplace(
							FUeAgentCanonicalIdentity(
								TEXT("uai.asset.presence"),
								TEXT("asset"),
								ExternalPackageName,
								TEXT("presence"),
								TEXT("asset"),
								TEXT("asset-delete-v1")),
							ExternalPackageName,
							EPackageHeaderKind::EvidenceRequired,
							EAssetDurableIntent::Delete,
							ERollbackGrade::DurableBackup);
					}
				}
				if (DurableTargets.IsEmpty())
				{
					return Fail(TEXT("asset_delete_target_missing"), ObjectPath);
				}
				const int64 FencingToken = FencingCounter.Increment();
				const FPlanningPreflightPlanRef Plan = MakeDeletePlan(
					Request, Result.PackageName, JournalOwnerId, DurableTargets);
				const FAssetDurableRequest DurableRequest(
					Plan,
					DurableTargets,
					FencingToken,
					JournalOwnerId,
					EAtomicityMode::CompensatedSaga);
				FAssetDurableSessionPtr Session;
				FAssetDurableError DurableError;
				if (!TryPrepareAssetDurability(
						DurableRequest,
						Backend,
						JournalRoot,
						Resources,
						Session,
						DurableError)
					|| !Session.IsValid())
				{
					return Fail(DurableError.GetCode().IsEmpty()
							? TEXT("asset_delete_prepare_failed")
							: *DurableError.GetCode(),
						DurableError.GetDetail());
				}
				Result.JournalId = Session->GetJournalId();
				TArray<FString> RegistryRefreshFiles;
				RegistryRefreshFiles.Reserve(DurableTargets.Num());
				for (const FAssetDurableTarget& Target : DurableTargets)
				{
					FString TargetFilename;
					if (FPackageName::DoesPackageExist(
							Target.GetPackageName(), &TargetFilename))
					{
						RegistryRefreshFiles.AddUnique(TargetFilename);
					}
				}
				auto RollbackAndRefresh = [&]()
				{
					FDurableReceipt RollbackReceipt;
					const bool bRolledBack = TryRollbackAssetDurability(
						Session.ToSharedRef(), Backend, FencingToken, true,
						Resources, RollbackReceipt, DurableError);
					RefreshExactRegistryFiles(RegistryRefreshFiles);
					FString ReadbackError;
					TryReadAbsence(
						Result.PackageName, ObjectPath, Result, ReadbackError);
					return bRolledBack;
				};
				if (Cancellation->IsCancelled())
				{
					RollbackAndRefresh();
					return Fail(TEXT("asset_delete_cancelled"), Request.RequestId);
				}

				TGuardValue<bool> UnattendedGuard(GIsRunningUnattendedScript, true);
				if (Asset)
				{
					ReleaseTransientAssetControllers(Asset);
				}
				const FString ReferenceSummaryBeforeDelete = Asset
					? ReferencerSummary(Asset)
					: TEXT(";package_only_delete=true");
				const bool bOwnedWorldExternalDeleteSet =
					IsOwnedWorldExternalDeleteSet(
						Asset, Result.PackageName, NativeDeleteSet);
				const bool bEngineRegisteredStringTable =
					HasOnlyEngineStringTableRegistrationReference(Asset);
				const int32 DeletedCount = bPackageOnlyDelete
					? 0
					: bOwnedWorldExternalDeleteSet
					? ObjectTools::DeleteObjectsUnchecked(NativeDeleteSet)
					: bEngineRegisteredStringTable
						? ObjectTools::ForceDeleteObjects({Asset}, false)
						: ObjectTools::DeleteObjects(
						NativeDeleteSet,
						false,
						ObjectTools::EAllowCancelDuringDelete::CancelNotAllowed);
				if (!bPackageOnlyDelete)
				{
					CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
				}
				if (DeletedCount != NativeDeleteSet.Num())
				{
					RollbackAndRefresh();
					return Fail(
						TEXT("asset_delete_native_delete_failed"),
						FString::Printf(
							TEXT("%s;expected_delete_count=%d;actual_delete_count=%d%s"),
							*ObjectPath,
							NativeDeleteSet.Num(),
							DeletedCount,
							*ReferenceSummaryBeforeDelete));
				}

				FDurableReceipt PersistReceipt;
				if (!TryPersistAssetDurability(
						Session.ToSharedRef(), Backend, FencingToken, Resources,
						PersistReceipt, DurableError))
				{
					FDurableReceipt RollbackReceipt;
					TryRollbackAssetDurability(
						Session.ToSharedRef(), Backend, FencingToken, false,
						Resources, RollbackReceipt, DurableError);
					RefreshExactRegistryFiles(RegistryRefreshFiles);
					FString ReadbackError;
					TryReadAbsence(
						Result.PackageName, ObjectPath, Result, ReadbackError);
					Result.TerminalState = TEXT("state_unknown");
					return Fail(TEXT("asset_delete_persist_failed"),
						DurableError.GetCode() + TEXT(":") + DurableError.GetDetail());
				}
				RefreshExactRegistryFiles(RegistryRefreshFiles);

				FString ReadbackError;
				if (!TryReadAbsence(
						Result.PackageName,
						ObjectPath,
						Result,
						ReadbackError))
				{
					Result.TerminalState = TEXT("state_unknown");
					return Fail(
						TEXT("asset_delete_registry_readback_failed"),
						ReadbackError);
				}
				TArray<FString> RemainingExternalPackages;
				if (!TryCollectOwnedWorldExternalPackages(
						Result.PackageName,
						4096,
						RemainingExternalPackages,
						ExternalPackageError))
				{
					Result.TerminalState = TEXT("state_unknown");
					return Fail(TEXT("asset_delete_external_readback_failed"),
						ExternalPackageError);
				}
				if (!(Result.bFileAbsent && Result.bPackageAbsent
					&& Result.bRegistryAbsent && Result.bObjectAbsent
					&& RemainingExternalPackages.IsEmpty()))
				{
					Result.TerminalState = TEXT("state_unknown");
					return Fail(TEXT("asset_delete_readback_failed"),
						ObjectPath + TEXT(";remaining_external_packages=")
						+ FString::Join(RemainingExternalPackages, TEXT("|")));
				}

				const FAssetDurableSessionRef PersistedSession =
					MakeShared<const FAssetDurableSession>(
						Session->GetRequest(),
						Session->GetInventory(),
						Session->GetJournalId(),
						Session->GetJournalPath(),
						Session->GetJournalRoot(),
						Session->GetTransactionRoot(),
						Session->GetStagingRoot(),
						PersistReceipt.GetPhaseHistory());
				Result.VerificationId = TEXT("asset-delete-readback:")
					+ Request.RequestId;
				const FDurableVerificationAcknowledgement Acknowledgement(
					PersistReceipt.GetJournalId(),
					PersistReceipt.GetPlanFingerprint(),
					PersistReceipt.GetInventoryHash(),
					Result.VerificationId,
					JournalOwnerId,
					FencingToken);
				if (!TryAcknowledgeAssetDurability(
						PersistedSession,
						Acknowledgement,
						FencingToken,
						Resources,
						DurableError))
				{
					Result.TerminalState = TEXT("state_unknown");
					return Fail(TEXT("asset_delete_acknowledgement_failed"),
						DurableError.GetCode() + TEXT(":") + DurableError.GetDetail());
				}

				Result.bDeleted = true;
				Result.TerminalState = TEXT("committed");
				return Result;
			}

		private:
			FString JournalRoot;
			FString JournalOwnerId;
			FPackageAssetDurableBackend Backend;
			FThreadSafeCounter64 FencingCounter;
		};
	}

	bool TryCreateAssetDeleteOwner(
		FString JournalRoot,
		FString JournalOwnerId,
		TSharedPtr<IUeAgentAssetDeleteOwner>& OutOwner,
		FString& OutError)
	{
		OutOwner.Reset();
		OutError.Reset();
		JournalRoot = FPaths::ConvertRelativePathToFull(JournalRoot);
		JournalOwnerId.TrimStartAndEndInline();
		if (JournalRoot.IsEmpty() || JournalOwnerId.IsEmpty())
		{
			OutError = TEXT("asset_delete_owner_config_invalid");
			return false;
		}
		OutOwner = MakeShared<FAssetDeleteOwner>(
			MoveTemp(JournalRoot), MoveTemp(JournalOwnerId));
		return true;
	}
}

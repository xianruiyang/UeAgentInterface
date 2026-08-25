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
			bool& bOutResolvedFromPackage,
			FString& OutErrorCode,
			FString& OutErrorDetail)
		{
			bOutResolvedFromPackage = false;
			OutErrorCode.Reset();
			OutErrorDetail.Reset();
			if (!InOutObjectPath.IsEmpty())
			{
				return true;
			}

			TArray<FAssetData> Assets;
			FAssetRegistryModule::GetRegistry().GetAssetsByPackageName(
				FName(*PackageName), Assets, false);
			if (Assets.IsEmpty())
			{
				OutErrorCode = TEXT("asset_delete_target_missing");
				OutErrorDetail = PackageName;
				return false;
			}
			if (Assets.Num() != 1)
			{
				TArray<FString> ObjectPaths;
				ObjectPaths.Reserve(Assets.Num());
				for (const FAssetData& Asset : Assets)
				{
					ObjectPaths.Add(Asset.GetSoftObjectPath().ToString());
				}
				ObjectPaths.Sort();
				OutErrorCode = TEXT("asset_delete_target_ambiguous");
				OutErrorDetail = PackageName + TEXT(":")
					+ FString::Join(ObjectPaths, TEXT("|"));
				return false;
			}

			InOutObjectPath = Assets[0].GetSoftObjectPath().ToString();
			bOutResolvedFromPackage = true;
			return true;
		}

		bool IsExternalPackageName(const FString& PackageName)
		{
			return PackageName.StartsWith(TEXT("/Game/__ExternalActors__/"))
				|| PackageName.StartsWith(TEXT("/Game/__ExternalObjects__/"));
		}

		bool TryRequireNoOnDiskReferencers(
			const FString& PackageName,
			FString& OutDetail)
		{
			TArray<FName> Referencers;
			FAssetRegistryModule::GetRegistry().GetReferencers(
				FName(*PackageName), Referencers);
			Referencers.Sort(FNameLexicalLess());
			if (Referencers.IsEmpty())
			{
				OutDetail.Reset();
				return true;
			}
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

		void ReadAbsence(
			const FString& PackageName,
			const FString& ObjectPath,
			FUeAgentAssetDeleteResult& Result)
		{
			FString ExistingFilename;
			Result.bFileAbsent = !FPackageName::DoesPackageExist(
				PackageName, &ExistingFilename);
			Result.bPackageAbsent = FindPackage(nullptr, *PackageName) == nullptr;
			TArray<FAssetData> Assets;
			FAssetRegistryModule::GetRegistry().GetAssetsByPackageName(
				FName(*PackageName), Assets, false);
			Result.bRegistryAbsent = Assets.IsEmpty();
			Result.bObjectAbsent =
				FindObject<UObject>(nullptr, *ObjectPath) == nullptr;
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
					ReadAbsence(Result.PackageName, ObjectPath, Result);
					return Fail(TEXT("asset_delete_target_missing"), Result.PackageName);
				}
				FString ResolutionErrorCode;
				FString ResolutionErrorDetail;
				bool bResolvedFromPackage = false;
				if (!TryResolveTargetObjectPath(
						Result.PackageName,
						ObjectPath,
						bResolvedFromPackage,
						ResolutionErrorCode,
						ResolutionErrorDetail))
				{
					return Fail(*ResolutionErrorCode, ResolutionErrorDetail);
				}
				UObject* Asset = LoadObject<UObject>(nullptr, *ObjectPath);
				const bool bOrphanExternalPackage = !IsValid(Asset)
					&& bResolvedFromPackage
					&& IsExternalPackageName(Result.PackageName)
					&& FindPackage(nullptr, *Result.PackageName) == nullptr;
				if (!IsValid(Asset) && !bOrphanExternalPackage)
				{
					return Fail(TEXT("asset_delete_target_missing"), ObjectPath);
				}
				if (bOrphanExternalPackage)
				{
					FString ReferencerDetail;
					if (!TryRequireNoOnDiskReferencers(
							Result.PackageName, ReferencerDetail))
					{
						return Fail(
							TEXT("asset_delete_orphan_package_referenced"),
							ReferencerDetail);
					}
				}
				UPackage* Package = Asset ? Asset->GetOutermost() : nullptr;
				if ((!Package && !bOrphanExternalPackage)
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
				if (bOrphanExternalPackage)
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
				if (Cancellation->IsCancelled())
				{
					FDurableReceipt RollbackReceipt;
					TryRollbackAssetDurability(
						Session.ToSharedRef(), Backend, FencingToken, true,
						Resources, RollbackReceipt, DurableError);
					return Fail(TEXT("asset_delete_cancelled"), Request.RequestId);
				}

				TGuardValue<bool> UnattendedGuard(GIsRunningUnattendedScript, true);
				if (Asset)
				{
					ReleaseTransientAssetControllers(Asset);
				}
				const FString ReferenceSummaryBeforeDelete = Asset
					? ReferencerSummary(Asset)
					: TEXT(";orphan_external_package=true");
				const bool bOwnedWorldExternalDeleteSet =
					IsOwnedWorldExternalDeleteSet(
						Asset, Result.PackageName, NativeDeleteSet);
				const bool bEngineRegisteredStringTable =
					HasOnlyEngineStringTableRegistrationReference(Asset);
				const int32 DeletedCount = bOrphanExternalPackage
					? 0
					: bOwnedWorldExternalDeleteSet
					? ObjectTools::DeleteObjectsUnchecked(NativeDeleteSet)
					: bEngineRegisteredStringTable
						? ObjectTools::ForceDeleteObjects({Asset}, false)
						: ObjectTools::DeleteObjects(
						{Asset},
						false,
						ObjectTools::EAllowCancelDuringDelete::CancelNotAllowed);
				CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
				if (DeletedCount != NativeDeleteSet.Num())
				{
					FDurableReceipt RollbackReceipt;
					TryRollbackAssetDurability(
						Session.ToSharedRef(), Backend, FencingToken, true,
						Resources, RollbackReceipt, DurableError);
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
					Result.TerminalState = TEXT("state_unknown");
					return Fail(TEXT("asset_delete_persist_failed"),
						DurableError.GetCode() + TEXT(":") + DurableError.GetDetail());
				}
				if (bOrphanExternalPackage)
				{
					FAssetRegistryModule::GetRegistry().ScanModifiedAssetFiles(
						{ExistingFilename});
				}

				ReadAbsence(Result.PackageName, ObjectPath, Result);
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

#include "DomainAdapters/AssetDestructiveImport/UeAgentAssetDeleteOwner.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "DomainAdapters/AssetDestructiveImport/UeAgentAssetDeletePreflight.h"
#include "DomainAdapters/AssetDestructiveImport/UeAgentSoundSubmixDeleteLifecycle.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/AnimSequenceBase.h"
#include "AssetDeleteModel.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Core/AssetDurable/UeAgentAssetDurable.h"
#include "Core/AssetDurable/UeAgentPackageSave.h"
#include "Core/ContextResource/UeAgentContextResource.h"
#include "Core/Contract/UeAgentContractJson.h"
#include "Core/PlanningPreflight/UeAgentPlanningPreflight.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "DomainAdapters/ContentOperations/UeAgentDestructiveContentPlanningBoundary.h"
#include "Engine/World.h"
#include "Editor.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "HAL/FileManager.h"
#include "HAL/ThreadSafeCounter.h"
#include "Internationalization/StringTable.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Rig/IKRigDefinition.h"
#include "RigEditor/IKRigController.h"
#include "UObject/StrongObjectPtr.h"
#include "ObjectTools.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "Utilities/AuthoringOperationApply/UeAgentAuthoringPackageReadback.h"

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
		FDiagnostic DeleteFact(const FString& Code, const FString& Reason, const FString& Target,
			const FString& Path, TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected,
			const FString& Stage = TEXT("asset_delete"))
		{
			auto Diagnostic = FDiagnostic::Failure(Code, Reason, Stage);
			Diagnostic.TargetPath = Target;
			Diagnostic.PropertyPath = Path;
			Diagnostic.SourceKind = TEXT("native_result");
			Diagnostic.ActualValue = MoveTemp(Actual);
			Diagnostic.ExpectedValue = MoveTemp(Expected);
			return Diagnostic;
		}

		void AssignDeleteTargetDiagnostic(FUeAgentAssetDeleteTargetResult& Target, FDiagnostic Diagnostic)
		{
			if (Diagnostic.TargetPath.IsEmpty()) Diagnostic.TargetPath = Target.ObjectPath;
			Target.Diagnostic = MoveTemp(Diagnostic);
		}

		TSharedPtr<FJsonValue> DeleteStringArray(const TArray<FString>& Values)
		{
			TArray<TSharedPtr<FJsonValue>> Result;
			for (const auto& Value : Values) Result.Add(MakeShared<FJsonValueString>(Value));
			return MakeShared<FJsonValueArray>(MoveTemp(Result));
		}

		TSharedPtr<FJsonValue> DeleteAbsenceFacts(const FUeAgentAssetDeleteTargetResult& Target)
		{
			auto Facts = MakeShared<FJsonObject>();
			Facts->SetBoolField(TEXT("file_absent"), Target.bFileAbsent);
			Facts->SetBoolField(TEXT("registry_absent"), Target.bRegistryAbsent);
			Facts->SetBoolField(TEXT("package_absent_or_empty"), Target.bPackageAbsent || Target.bPackageEmpty);
			Facts->SetBoolField(TEXT("object_absent_or_retired"), Target.bObjectAbsent || Target.bObjectRetired);
			return MakeShared<FJsonValueObject>(Facts);
		}

		TSharedPtr<FJsonValue> ExpectedDeleteAbsence()
		{
			auto Facts = MakeShared<FJsonObject>();
			Facts->SetBoolField(TEXT("file_absent"), true);
			Facts->SetBoolField(TEXT("registry_absent"), true);
			Facts->SetBoolField(TEXT("package_absent_or_empty"), true);
			Facts->SetBoolField(TEXT("object_absent_or_retired"), true);
			return MakeShared<FJsonValueObject>(Facts);
		}

		FUeAgentResourceTracker MakeTracker()
		{
			return FUeAgentResourceTracker();
		}

		FPlanningPreflightPlanRef MakeDeletePlan(
			const FUeAgentAssetDeleteRequest& Request,
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
			TArray<FString> DurablePackageNames;
			DurablePackageNames.Reserve(Targets.Num());
			for (const FAssetDurableTarget& Target : Targets)
			{
				DurablePackageNames.Add(Target.GetPackageName());
			}
			DurablePackageNames.Sort();
			const FString Fingerprint = Json::Sha256Utf8(
				Request.RequestId + TEXT(":")
				+ FString::Join(DurablePackageNames, TEXT("|")));
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
			FUeAgentAssetDeleteTargetResult& Result,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			OutError.Reset();
			FString ExistingFilename;
			Result.bFileAbsent = !FPackageName::DoesPackageExist(
				PackageName, &ExistingFilename);
			UPackage* RemainingPackage = FindPackage(nullptr, *PackageName);
			Result.bPackageAbsent = RemainingPackage == nullptr;
			// UE may retain an empty UPackage after deleting its final asset.
			// Its residency is distinct from the asset, registry entry and file.
			Result.bPackageEmpty = RemainingPackage && UPackage::IsEmptyPackage(RemainingPackage);
			TArray<FAssetData> Assets;
			if (!FAssetRegistryModule::GetRegistry().GetAssetsByPackageName(
					FName(*PackageName), Assets, false))
			{
				Result.bRegistryAbsent = false;
				OutError = DeleteFact(TEXT("asset_delete_registry_readback_failed"), TEXT("The asset registry query could not confirm the package contents."),
					ObjectPath, TEXT("/readback/registry/query_succeeded"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), TEXT("asset_delete_readback"));
				return false;
			}
			Result.bRegistryAbsent = Assets.IsEmpty();
			UObject* RemainingObject = FindObject<UObject>(nullptr, *ObjectPath);
			Result.bObjectAbsent = !IsValid(RemainingObject);
			// DeleteSingleObject removes asset/public ownership. A native render or
			// async reference may keep that retired UObject alive until a later GC;
			// do not resurrect its file merely because memory reclamation is delayed.
			Result.bObjectRetired = IsValid(RemainingObject) && !RemainingObject->IsAsset()
				&& !RemainingObject->HasAnyFlags(RF_Public | RF_Standalone);
			if (!Result.IsAbsenceVerified())
			{
				UE_LOG(LogTemp, Warning, TEXT("Asset deletion absence readback %s: file_absent=%d package_absent=%d registry_absent=%d object_absent=%d package_empty=%d object_flags=%u internal_flags=%u"),
					*ObjectPath, Result.bFileAbsent, Result.bPackageAbsent,
					Result.bRegistryAbsent, Result.bObjectAbsent, Result.bPackageEmpty,
					RemainingObject ? static_cast<uint32>(RemainingObject->GetFlags()) : 0,
					RemainingObject ? static_cast<uint32>(RemainingObject->GetInternalFlags()) : 0);
			}
			return true;
		}

		void RefreshExactRegistryFiles(const TArray<FString>& Filenames)
		{
			UE::UAI::AssetDurable::RefreshPublishedPackageFiles(Filenames);
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
				Result.RequestedTargets = Request.TargetRefs;
				Result.bRecursive = Request.bRecursive;
				auto AddRequestFailure = [&Result, &Request](
					UE::UAI::Contract::FDiagnostic Diagnostic)
				{
					FUeAgentAssetDeleteTargetResult& Target =
						Result.Targets.AddDefaulted_GetRef();
					Target.TargetRef = Request.TargetRefs.IsEmpty()
						? FString()
						: Request.TargetRefs[0];
					if (Diagnostic.TargetPath.IsEmpty()) Diagnostic.TargetPath = Target.TargetRef;
					Target.Diagnostic = MoveTemp(Diagnostic);
					return Result;
				};
				if (!IsInGameThread())
				{
					return AddRequestFailure(DeleteFact(TEXT("asset_delete_game_thread_required"), TEXT("Native asset deletion requires the game thread."),
						FString(), TEXT("/execution/on_game_thread"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)));
				}
				if (Cancellation->IsCancelled())
				{
					auto Diagnostic = DeleteFact(TEXT("asset_delete_cancelled"), TEXT("Asset deletion was cancelled before preflight."),
						FString(), TEXT("/execution/cancel_requested"), MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false));
					Diagnostic.SubjectId = Request.RequestId;
					return AddRequestFailure(MoveTemp(Diagnostic));
				}

				const FUeAgentAssetDeletePreflightResult Preflight =
					PrepareAssetDeleteSelection(
						{Request.TargetRefs, Request.bRecursive});
				if (!Preflight.IsSuccess())
				{
					return AddRequestFailure(Preflight.Diagnostic);
				}

				struct FTargetWork final
				{
					FString PackageName;
					FString ObjectPath;
					TArray<FString> Referencers;
					TArray<FString> OwnedExternalPackages;
					UObject* Asset = nullptr;
					bool bBlocked = false;
				};
				TArray<FTargetWork> Work;
				Work.Reserve(Preflight.Targets.Num());
				Result.Targets.Reserve(Preflight.Targets.Num());
				TMap<FString, int32> SelectedPackageToIndex;
				for (const FUeAgentAssetDeletePreparedTarget& Prepared
					: Preflight.Targets)
				{
					FTargetWork& TargetWork = Work.AddDefaulted_GetRef();
					TargetWork.PackageName = Prepared.PackageName;
					TargetWork.ObjectPath = Prepared.ObjectPath;
					TargetWork.Referencers = Prepared.Referencers;
					TargetWork.OwnedExternalPackages =
						Prepared.OwnedExternalPackages;
					TargetWork.bBlocked = !Prepared.IsExecutable();
					SelectedPackageToIndex.Add(
						Prepared.PackageName, Work.Num() - 1);

					FUeAgentAssetDeleteTargetResult& TargetResult =
						Result.Targets.AddDefaulted_GetRef();
					TargetResult.TargetRef = Prepared.ObjectPath;
					TargetResult.PackageName = Prepared.PackageName;
					TargetResult.ObjectPath = Prepared.ObjectPath;
					if (TargetWork.bBlocked)
					{
						TargetResult.Status = TEXT("blocked");
						TargetResult.Diagnostic = Prepared.Diagnostic;
					}
				}

				auto Block = [&Result, &Work](
					const int32 Index, UE::UAI::Contract::FDiagnostic Diagnostic)
				{
					if (Work[Index].bBlocked)
					{
						return;
					}
					Work[Index].bBlocked = true;
					FUeAgentAssetDeleteTargetResult& Target = Result.Targets[Index];
					Target.Status = TEXT("blocked");
					Target.Diagnostic = MoveTemp(Diagnostic);
					if (Target.Diagnostic.TargetPath.IsEmpty()) Target.Diagnostic.TargetPath = Target.ObjectPath;
				};

				FSoundSubmixDeleteLifecycle SubmixLifecycle;
				TArray<UObject*> NativeDeleteSet;
				TArray<FAssetDurableTarget> DurableTargets;
				TSet<FString> DurablePackageNames;
				auto AddDurablePackage = [&DurableTargets, &DurablePackageNames](
					const FString& PackageName)
				{
					if (!DurablePackageNames.Contains(PackageName))
					{
						DurablePackageNames.Add(PackageName);
						DurableTargets.Emplace(
							FUeAgentCanonicalIdentity(
								TEXT("uai.asset.presence"), TEXT("asset"),
								PackageName, TEXT("presence"), TEXT("asset"),
								TEXT("asset-delete-v1")),
							PackageName,
							EPackageHeaderKind::EvidenceRequired,
							EAssetDurableIntent::Delete,
							ERollbackGrade::DurableBackup);
					}
				};
				// The native model filters references against its UObject pending set,
				// not the registry selection. Include selected referencers of every
				// loaded target; unrelated unloaded packages keep the durable-only path.
				TSet<int32> NativeTargetIndices;
				bool bNativeClosureChanged = true;
				while (bNativeClosureChanged)
				{
					bNativeClosureChanged = false;
					for (int32 Index = 0; Index < Work.Num(); ++Index)
					{
						FTargetWork& Target = Work[Index];
						if (Target.bBlocked) continue;
						if (!NativeTargetIndices.Contains(Index)
							&& FindPackage(nullptr, *Target.PackageName))
						{
							NativeTargetIndices.Add(Index);
							bNativeClosureChanged = true;
						}
						if (!NativeTargetIndices.Contains(Index)) continue;
						if (!Target.Asset)
						{
							Target.Asset = LoadObject<UObject>(nullptr, *Target.ObjectPath);
							if (!IsValid(Target.Asset))
							{
								Block(Index, DeleteFact(TEXT("asset_delete_target_load_failed"), TEXT("The selected asset could not be loaded while expanding the native delete set."),
									Target.ObjectPath, TEXT("/target/available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), TEXT("asset_delete_preflight")));
								continue;
							}
							bNativeClosureChanged = true;
						}
						for (const FString& Referencer : Target.Referencers)
						{
							const int32* ReferencerIndex = SelectedPackageToIndex.Find(Referencer);
							if (ReferencerIndex && !Work[*ReferencerIndex].bBlocked
								&& !NativeTargetIndices.Contains(*ReferencerIndex))
							{
								NativeTargetIndices.Add(*ReferencerIndex);
								bNativeClosureChanged = true;
							}
						}
					}
					// Loading a referencer can also load selected dependencies; the
					// next pass discovers them before any native deletion begins.
				}
				for (int32 Index = 0; Index < Work.Num(); ++Index)
				{
					if (Work[Index].bBlocked)
					{
						continue;
					}
					UObject* Asset = FindObject<UObject>(
						nullptr, *Work[Index].ObjectPath);
					UPackage* const LoadedPackage =
						FindPackage(nullptr, *Work[Index].PackageName);
					if (!Asset && LoadedPackage)
					{
						Asset = LoadObject<UObject>(
							nullptr, *Work[Index].ObjectPath);
					}
					Work[Index].Asset = Asset;
					if (LoadedPackage && !IsValid(Asset))
					{
						Block(Index, DeleteFact(TEXT("asset_delete_target_missing"), TEXT("The selected package is loaded but its target asset is absent or invalid."),
							Work[Index].ObjectPath, TEXT("/target/valid"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), TEXT("asset_delete_preflight")));
						continue;
					}
					UE::UAI::Contract::FDiagnostic LifecycleError;
					if (Asset && !SubmixLifecycle.Prepare(*Asset, LifecycleError))
					{
						Block(Index, LifecycleError);
						continue;
					}
					AddDurablePackage(Work[Index].PackageName);
					if (Asset)
					{
						NativeDeleteSet.AddUnique(Asset);
						TArray<UObject*> ExtraObjects{Asset};
						ObjectTools::AddExtraObjectsToDelete(ExtraObjects);
						for (UObject* const Extra : ExtraObjects)
						{
							NativeDeleteSet.AddUnique(Extra);
						}
					}
					for (const FString& External : Work[Index].OwnedExternalPackages)
					{
						AddDurablePackage(External);
						FString ExternalFilename;
						if (FPackageName::DoesPackageExist(
								External, &ExternalFilename)
							&& IFileManager::Get().IsReadOnly(*ExternalFilename))
						{
							auto Diagnostic = DeleteFact(TEXT("asset_delete_package_read_only"), TEXT("An external package owned by the selected world is read-only."),
								Work[Index].ObjectPath, TEXT("/external_package/read_only"), MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false), TEXT("asset_delete_preflight"));
							Diagnostic.CandidateName = External;
							Diagnostic.InternalContext = ExternalFilename;
							Block(Index, MoveTemp(Diagnostic));
							break;
						}
						if (Asset)
						{
							UPackage* const ExternalPackage =
								LoadPackage(nullptr, *External, LOAD_None);
							if (!ExternalPackage || ExternalPackage->IsDirty())
							{
								auto Diagnostic = DeleteFact(ExternalPackage ? TEXT("asset_delete_dirty_package") : TEXT("asset_delete_external_package_load_failed"),
									ExternalPackage ? TEXT("An external package owned by the selected world has unsaved changes.") : TEXT("An external package owned by the selected world could not be loaded."),
									Work[Index].ObjectPath, ExternalPackage ? TEXT("/external_package/dirty") : TEXT("/external_package/available"),
									MakeShared<FJsonValueBoolean>(ExternalPackage != nullptr), MakeShared<FJsonValueBoolean>(ExternalPackage == nullptr), TEXT("asset_delete_preflight"));
								Diagnostic.CandidateName = External;
								Block(Index, MoveTemp(Diagnostic));
								break;
							}
							NativeDeleteSet.AddUnique(ExternalPackage);
						}
					}
				}

				// A late external-package failure changes the executable set and must
				// participate in the same referencer closure.
				bool bClosureChanged = true;
				while (bClosureChanged)
				{
					bClosureChanged = false;
					for (int32 Index = 0; Index < Work.Num(); ++Index)
					{
						if (Work[Index].bBlocked)
						{
							continue;
						}
						for (const FString& Referencer : Work[Index].Referencers)
						{
							if (const int32* const ReferencerIndex =
								SelectedPackageToIndex.Find(Referencer))
							{
								if (Work[*ReferencerIndex].bBlocked)
								{
									auto Diagnostic = DeleteFact(TEXT("asset_delete_package_referenced"), TEXT("A referencing package became blocked during native delete preflight."),
										Work[Index].ObjectPath, TEXT("/referencers/blocking_package"), MakeShared<FJsonValueString>(Referencer),
										MakeShared<FJsonValueString>(TEXT("referencer included in the executable delete selection")), TEXT("asset_delete_preflight"));
									Diagnostic.PublicCause = MakeShared<FDiagnostic>(Result.Targets[*ReferencerIndex].Diagnostic);
									Block(Index, MoveTemp(Diagnostic));
									bClosureChanged = true;
									break;
								}
							}
						}
					}
				}

				// Rebuild shared execution sets after the closure has stabilized.
				NativeDeleteSet.Reset();
				DurableTargets.Reset();
				DurablePackageNames.Reset();
				for (int32 Index = 0; Index < Work.Num(); ++Index)
				{
					if (Work[Index].bBlocked)
					{
						continue;
					}
					AddDurablePackage(Work[Index].PackageName);
					if (Work[Index].Asset)
					{
						TArray<UObject*> DeleteObjects{Work[Index].Asset};
						ObjectTools::AddExtraObjectsToDelete(DeleteObjects);
						for (UObject* const Object : DeleteObjects)
						{
							NativeDeleteSet.AddUnique(Object);
						}
					}
					for (const FString& External : Work[Index].OwnedExternalPackages)
					{
						AddDurablePackage(External);
						if (Work[Index].Asset)
						{
							if (UPackage* const ExternalPackage =
								FindPackage(nullptr, *External))
							{
								NativeDeleteSet.AddUnique(ExternalPackage);
							}
						}
					}
				}

				if (DurableTargets.IsEmpty())
				{
					return Result;
				}
				DurableTargets.Sort([](
					const FAssetDurableTarget& A, const FAssetDurableTarget& B)
				{
					return A.GetPackageName() < B.GetPackageName();
				});
				FUeAgentResourceTracker Resources = MakeTracker();
				const int64 FencingToken = FencingCounter.Increment();
				const FPlanningPreflightPlanRef Plan = MakeDeletePlan(
					Request, JournalOwnerId, DurableTargets);
				const FAssetDurableRequest DurableRequest(
					Plan, DurableTargets, FencingToken, JournalOwnerId,
					EAtomicityMode::CompensatedSaga);
				FAssetDurableSessionPtr Session;
				FAssetDurableError DurableError;
				if (!TryPrepareAssetDurability(
						DurableRequest, Backend, JournalRoot, Resources,
						Session, DurableError))
				{
					for (int32 Index = 0; Index < Work.Num(); ++Index)
					{
						if (!Work[Index].bBlocked)
						{
							AssignDeleteTargetDiagnostic(Result.Targets[Index], DurableError);
						}
					}
					return Result;
				}

				TArray<FString> RegistryRefreshFiles;
				for (const FAssetDurableTarget& Target : DurableTargets)
				{
					FString Filename;
					if (FPackageName::DoesPackageExist(
							Target.GetPackageName(), &Filename))
					{
						RegistryRefreshFiles.AddUnique(Filename);
					}
				}
				for (int32 Index = 0; Index < Work.Num(); ++Index)
				{
					if (!Work[Index].bBlocked)
					{
						Result.Targets[Index].JournalId = Session->GetJournalId();
					}
				}
				bool bNativeDeletionStarted = false;
				auto RollbackAndReadback = [&]()
				{
					UE::UAI::Contract::FDiagnostic RecoveryError;
					const auto ConfirmRestoredState = [&](UE::UAI::Contract::FDiagnostic& Error)
					{
						if (!SubmixLifecycle.Restore(Error)) return false;
						RefreshExactRegistryFiles(RegistryRefreshFiles);
						for (int32 Index = 0; Index < Work.Num(); ++Index)
						{
							if (Work[Index].bBlocked) continue;
							auto& Target = Result.Targets[Index];
							if (!TryReadAbsence(Work[Index].PackageName, Work[Index].ObjectPath, Target, Error)) return false;
							if (Target.bFileAbsent || Target.bRegistryAbsent ||
								(NativeTargetIndices.Contains(Index) && (Target.bObjectAbsent || Target.bObjectRetired)))
							{
								auto Actual = MakeShared<FJsonObject>();
								Actual->SetBoolField(TEXT("file_absent"), Target.bFileAbsent);
								Actual->SetBoolField(TEXT("registry_absent"), Target.bRegistryAbsent);
								auto Expected = MakeShared<FJsonObject>();
								Expected->SetBoolField(TEXT("file_absent"), false);
								Expected->SetBoolField(TEXT("registry_absent"), false);
								if (NativeTargetIndices.Contains(Index))
								{
									Actual->SetBoolField(TEXT("object_active"), !Target.bObjectAbsent && !Target.bObjectRetired);
									Expected->SetBoolField(TEXT("object_active"), true);
								}
								Error = DeleteFact(TEXT("asset_delete_restore_readback_failed"), TEXT("The restored asset does not match the required pre-deletion presence state."),
									Work[Index].ObjectPath, TEXT("/restore/readback"), MakeShared<FJsonValueObject>(Actual), MakeShared<FJsonValueObject>(Expected), TEXT("asset_delete_restore"));
								return false;
							}
						}
						return true;
					};
					bool bRolledBack;
					if (bNativeDeletionStarted)
					{
						bRolledBack = UeAgentAuthoringOperationApply::TryRestorePackageDurability(
							DurableTargets[0].GetPackageName(), false, Session.ToSharedRef(), Backend,
							FencingToken, Resources, RecoveryError, ConfirmRestoredState);
					}
					else
					{
						FDurableReceipt RollbackReceipt;
						FAssetDurableError RollbackError;
						bRolledBack = TryRollbackAssetDurability(Session.ToSharedRef(), Backend, FencingToken,
							[&]() { return ConfirmRestoredState(RecoveryError); }, Resources, RollbackReceipt, RollbackError);
						if (bRolledBack && RollbackReceipt.GetStatus() != EDurableStatus::RolledBack)
						{
							bRolledBack = false;
							RollbackError = RollbackReceipt.GetFailureDiagnostic();
						}
						if (!bRolledBack) RecoveryError.Append(RollbackError);
					}
					if (!bRolledBack)
						{
                        RecoveryError.RollbackAttempted = true;
                        RecoveryError.RollbackSucceeded = false;
                        Result.Diagnostic.Append(RecoveryError);
                    }
					Result.TerminalState = bRolledBack ? TEXT("rolled_back") : TEXT("state_unknown");
					for (int32 Index = 0; Index < Work.Num(); ++Index)
					{
						if (!Work[Index].bBlocked)
						{
							UE::UAI::Contract::FDiagnostic ReadbackError;
							if (!TryReadAbsence(Work[Index].PackageName, Work[Index].ObjectPath, Result.Targets[Index], ReadbackError))
							{
								Result.Diagnostic.Append(ReadbackError);
								Result.Targets[Index].Diagnostic.Append(ReadbackError);
							}
							Result.Targets[Index].Status = TEXT("remaining");
							Result.Targets[Index].bRemaining = true;
							Result.Targets[Index].bDeleted = false;
							Result.Targets[Index].TerminalState = bRolledBack ? TEXT("rolled_back") : TEXT("state_unknown");
						}
					}
					return bRolledBack;
				};
				if (Cancellation->IsCancelled())
				{
					RollbackAndReadback();
					for (int32 Index = 0; Index < Work.Num(); ++Index)
					{
						if (!Work[Index].bBlocked)
						{
							Result.Targets[Index].Diagnostic = DeleteFact(TEXT("asset_delete_cancelled"), TEXT("Asset deletion was cancelled before native deletion began."),
								Work[Index].ObjectPath, TEXT("/execution/cancel_requested"), MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false));
							Result.Targets[Index].Diagnostic.SubjectId = Request.RequestId;
						}
					}
					return Result;
				}

				if (!NativeDeleteSet.IsEmpty() && !TryBeginNativeAssetDurableMutation(Session, FencingToken, Resources, DurableError))
				{
					RollbackAndReadback();
					for (int32 Index = 0; Index < Work.Num(); ++Index)
						if (!Work[Index].bBlocked)
						{
							AssignDeleteTargetDiagnostic(Result.Targets[Index], DurableError);
						}
					return Result;
				}

				struct FReleasedRigController
				{
					FString ObjectPath;
					TStrongObjectPtr<UIKRigController> Controller;
				};
				TArray<FReleasedRigController> ReleasedRigControllers;
				ON_SCOPE_EXIT
				{
					for (const FReleasedRigController& Released : ReleasedRigControllers)
					{
						// Rollback may replace the package and object; never rebind a stale asset pointer.
						UIKRigDefinition* Restored = FindObject<UIKRigDefinition>(nullptr, *Released.ObjectPath);
						if (!Restored && FPackageName::DoesPackageExist(FPackageName::ObjectPathToPackageName(Released.ObjectPath)))
							Restored = LoadObject<UIKRigDefinition>(nullptr, *Released.ObjectPath);
						if (Restored && Released.Controller.IsValid())
						{
							Released.Controller->GetAssetPtr() = Restored;
							Restored->Controller = Released.Controller.Get();
						}
					}
				};
				// Close only selected, preflight-clean assets before UE scans memory
				// references. DeleteSingleObject closes too late for that scan.
				if (auto* Editors = GEditor ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr)
				{
					for (int32 Index = 0; Index < Work.Num(); ++Index)
					{
						const auto& Target = Work[Index];
						if (Target.bBlocked || !Target.Asset) continue;
						Editors->CloseAllEditorsForAsset(Target.Asset);
						const auto RemainingEditors = Editors->FindEditorsForAssetAndSubObjects(Target.Asset);
						if (!RemainingEditors.IsEmpty())
						{
							Result.Diagnostic = DeleteFact(TEXT("asset_delete_editor_close_refused"), TEXT("Native asset editors remain open after the close request."),
								Target.ObjectPath, TEXT("/asset_editors/remaining_count"), MakeShared<FJsonValueNumber>(RemainingEditors.Num()), MakeShared<FJsonValueNumber>(0));
							RollbackAndReadback();
							Result.Targets[Index].Diagnostic = Result.Diagnostic;
							return Result;
						}
					}
				}
				for (FTargetWork& Target : Work)
				{
					if (!Target.bBlocked && Target.Asset)
					{
						ReleaseTransientAssetControllers(Target.Asset);
						if (UIKRigDefinition* Rig = Cast<UIKRigDefinition>(Target.Asset))
						{
							if (UIKRigController* Controller = Cast<UIKRigController>(Rig->Controller))
							{
								ReleasedRigControllers.Add({Rig->GetPathName(), TStrongObjectPtr<UIKRigController>(Controller)});
								Controller->GetAssetPtr() = nullptr;
								Rig->Controller = nullptr;
							}
						}
					}
				}
				TGuardValue<bool> UnattendedGuard(GIsRunningUnattendedScript, true);
				bool bHasOwnedWorldSet = false;
				bool bAllEngineStringTables = !NativeDeleteSet.IsEmpty();
				for (const FTargetWork& Target : Work)
				{
					if (Target.bBlocked || !Target.Asset)
					{
						continue;
					}
					bHasOwnedWorldSet |= Cast<UWorld>(Target.Asset) != nullptr
						&& !Target.OwnedExternalPackages.IsEmpty();
					bAllEngineStringTables &=
						HasOnlyEngineStringTableRegistrationReference(Target.Asset);
				}
				TArray<TWeakObjectPtr<UObject>> NativeObjectsBeforeDelete;
				for (UObject* Object : NativeDeleteSet) NativeObjectsBeforeDelete.Add(Object);
				bNativeDeletionStarted = !NativeDeleteSet.IsEmpty();
				const int32 DeletedCount = NativeDeleteSet.IsEmpty()
					? 0
					: bHasOwnedWorldSet
					? ObjectTools::DeleteObjectsUnchecked(NativeDeleteSet)
					: bAllEngineStringTables
					? ObjectTools::ForceDeleteObjects(NativeDeleteSet, false)
					: ObjectTools::DeleteObjects(
						NativeDeleteSet, false,
						ObjectTools::EAllowCancelDuringDelete::CancelNotAllowed);
				if (DeletedCount != NativeDeleteSet.Num())
				{
					// Diagnose against UE's actual pending-delete containment rules.
					// Raw referencer lists include same-package/generated objects and
					// cannot be interpreted as blockers without this filtering.
					TArray<UObject*> LiveNativeObjects;
					for (const TWeakObjectPtr<UObject>& Object : NativeObjectsBeforeDelete)
						if (Object.IsValid()) LiveNativeObjects.Add(Object.Get());
					TMap<FString, FDiagnostic> ReferenceFailuresByPackage;
					TArray<FString> BlockingObjects;
					if (!LiveNativeObjects.IsEmpty())
					{
						FAssetDeleteModel ReferenceModel(LiveNativeObjects);
						while (ReferenceModel.GetState() != FAssetDeleteModel::Finished)
							ReferenceModel.Tick(0);
						for (const TSharedPtr<FPendingDelete>& Pending : *ReferenceModel.GetPendingDeletedAssets())
						{
							if (Pending->RemainingDiskReferences == 0 && Pending->RemainingMemoryReferences == 0) continue;
							TArray<FString> DiskReferencers;
							TArray<FString> MemoryReferencers;
							for (FName Reference : Pending->DiskReferences)
								if (!ReferenceModel.IsAssetInPendingDeletes(Reference)) DiskReferencers.AddUnique(Reference.ToString());
							for (const FReferencerInformation& Reference : Pending->MemoryReferences.ExternalReferences)
								if (!ReferenceModel.IsObjectInPendingDeletes(Reference.Referencer)) MemoryReferencers.AddUnique(GetPathNameSafe(Reference.Referencer));
							DiskReferencers.Sort();
							MemoryReferencers.Sort();
							UObject* Object = Pending->GetObject();
							BlockingObjects.Add(Object->GetPathName());
							auto Actual = MakeShared<FJsonObject>();
							Actual->SetNumberField(TEXT("disk_reference_count"), Pending->RemainingDiskReferences);
							Actual->SetNumberField(TEXT("memory_reference_count"), Pending->RemainingMemoryReferences);
							Actual->SetField(TEXT("disk_referencers"), DeleteStringArray(DiskReferencers));
							Actual->SetField(TEXT("memory_referencers"), DeleteStringArray(MemoryReferencers));
							auto Expected = MakeShared<FJsonObject>();
							Expected->SetNumberField(TEXT("disk_reference_count"), 0);
							Expected->SetNumberField(TEXT("memory_reference_count"), 0);
							auto Diagnostic = DeleteFact(TEXT("asset_delete_native_delete_failed"), TEXT("The native delete model reports remaining references to this object."),
								Object->GetPathName(), TEXT("/native_delete/references"), MakeShared<FJsonValueObject>(Actual), MakeShared<FJsonValueObject>(Expected));
							ReferenceFailuresByPackage.FindOrAdd(Object->GetOutermost()->GetName()).Append(Diagnostic);
							UE_LOG(LogTemp, Warning, TEXT("Asset native delete remaining references: object=%s disk=%d memory=%d disk_referencers=[%s] memory_referencers=[%s]"),
								*Object->GetPathName(), Pending->RemainingDiskReferences, Pending->RemainingMemoryReferences,
								*FString::Join(DiskReferencers, TEXT(",")), *FString::Join(MemoryReferencers, TEXT(",")));
						}
					}
					BlockingObjects.Sort();
					const auto NativeCountFailure = DeleteFact(TEXT("asset_delete_native_delete_failed"),
						TEXT("Native deletion reported a deleted-object count different from the requested set."),
						FString(), TEXT("/native_delete/deleted_object_count"), MakeShared<FJsonValueNumber>(DeletedCount), MakeShared<FJsonValueNumber>(NativeDeleteSet.Num()));
					Result.Diagnostic = NativeCountFailure;
					for (const auto& Pair : ReferenceFailuresByPackage) Result.Diagnostic.Append(Pair.Value);
					Result.Diagnostic.ActualEvidence = DeleteStringArray(BlockingObjects);
					RollbackAndReadback();
					for (int32 Index = 0; Index < Work.Num(); ++Index)
					{
						if (!Work[Index].bBlocked)
						{
							const FDiagnostic* ReferenceFailure = ReferenceFailuresByPackage.Find(Work[Index].PackageName);
							Result.Targets[Index].Diagnostic = ReferenceFailure ? *ReferenceFailure : NativeCountFailure;
							if (Result.Targets[Index].Diagnostic.TargetPath.IsEmpty()) Result.Targets[Index].Diagnostic.TargetPath = Work[Index].ObjectPath;
						}
					}
					return Result;
				}

				// Native reference/delegate refusal occurs before releasing device state.
				// Registered submixes remain rooted after native flag removal, so resolve
				// only the prepared exact paths, unregister, then permit collection.
				TArray<FString> SelectedPaths;
				for (const FTargetWork& Target : Work) if (!Target.bBlocked) SelectedPaths.Add(Target.ObjectPath);
				UE::UAI::Contract::FDiagnostic NativeLifecycleError;
				const bool bReleased = SubmixLifecycle.Release(SelectedPaths, NativeLifecycleError);
				if (bReleased && !NativeDeleteSet.IsEmpty()) CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
				const bool bCancelledAfterRelease = bReleased && Cancellation->IsCancelled();
				if (!bReleased || bCancelledAfterRelease)
				{
					RollbackAndReadback();
					for (int32 Index = 0; Index < Work.Num(); ++Index)
					{
						if (!Work[Index].bBlocked)
						{
							Result.Targets[Index].Diagnostic = bCancelledAfterRelease
								? DeleteFact(TEXT("asset_delete_cancelled"), TEXT("Asset deletion was cancelled after releasing native lifecycle state."),
									Work[Index].ObjectPath, TEXT("/execution/cancel_requested"), MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false))
								: NativeLifecycleError;
							if (Result.Targets[Index].Diagnostic.TargetPath.IsEmpty()) Result.Targets[Index].Diagnostic.TargetPath = Work[Index].ObjectPath;
						}
					}
					return Result;
				}

				FDurableReceipt PersistReceipt;
				if (!TryPersistAssetDurabilityForVerification(
						Session.ToSharedRef(), Backend, FencingToken, Resources,
						PersistReceipt, DurableError))
				{
					RollbackAndReadback();
					for (int32 Index = 0; Index < Work.Num(); ++Index)
					{
						if (!Work[Index].bBlocked)
						{
							AssignDeleteTargetDiagnostic(Result.Targets[Index], DurableError);
						}
					}
					return Result;
				}
				RefreshExactRegistryFiles(RegistryRefreshFiles);

				int32 DeletedTargetCount = 0;
				bool bReadbackFailed = false;
				for (int32 Index = 0; Index < Work.Num(); ++Index)
				{
					if (Work[Index].bBlocked)
					{
						continue;
					}
					UE::UAI::Contract::FDiagnostic ReadbackError;
					FUeAgentAssetDeleteTargetResult& Target = Result.Targets[Index];
					if (!TryReadAbsence(Work[Index].PackageName,
							Work[Index].ObjectPath, Target, ReadbackError)
						|| !Target.IsAbsenceVerified())
					{
						Result.TerminalState = TEXT("state_unknown");
						Target.TerminalState = TEXT("state_unknown");
						Target.Diagnostic = ReadbackError.IsEmpty()
							? DeleteFact(TEXT("asset_delete_readback_failed"), TEXT("Independent readback did not confirm complete asset absence."),
								Target.ObjectPath, TEXT("/readback/absence"), DeleteAbsenceFacts(Target), ExpectedDeleteAbsence(), TEXT("asset_delete_readback"))
							: ReadbackError;
						if (Target.Diagnostic.TargetPath.IsEmpty()) Target.Diagnostic.TargetPath = Target.ObjectPath;
						bReadbackFailed = true;
						continue;
					}
					Target.Status = TEXT("deleted");
					Target.bDeleted = true;
					Target.bRemaining = false;
					++DeletedTargetCount;
				}
				if (bReadbackFailed)
				{
					RollbackAndReadback();
					return Result;
				}

				const FAssetDurableSessionRef PersistedSession =
					MakeShared<const FAssetDurableSession>(
						Session->GetRequest(), Session->GetInventory(),
						Session->GetJournalId(), Session->GetJournalPath(),
						Session->GetJournalRoot(), Session->GetTransactionRoot(),
						Session->GetStagingRoot(), PersistReceipt.GetPhaseHistory());
				const FString BatchVerificationId =
					TEXT("asset-delete-readback:") + Request.RequestId;
				const FDurableVerificationAcknowledgement Acknowledgement(
					PersistReceipt.GetJournalId(), PersistReceipt.GetPlanFingerprint(),
					PersistReceipt.GetInventoryHash(), BatchVerificationId,
					JournalOwnerId, FencingToken);
				if (!TryAcknowledgeAssetDurability(
						PersistedSession, Acknowledgement, FencingToken,
						Resources, DurableError))
				{
					Result.TerminalState = TEXT("state_unknown");
					for (int32 Index = 0; Index < Work.Num(); ++Index)
					{
						if (!Work[Index].bBlocked)
						{
							Result.Targets[Index].TerminalState = TEXT("state_unknown");
							AssignDeleteTargetDiagnostic(Result.Targets[Index], DurableError);
						}
					}
					return Result;
				}

				for (int32 Index = 0; Index < Work.Num(); ++Index)
				{
					FUeAgentAssetDeleteTargetResult& Target = Result.Targets[Index];
					if (Work[Index].bBlocked)
					{
						Target.TerminalState = TEXT("rolled_back");
						continue;
					}
					Target.VerificationId = BatchVerificationId + TEXT(":")
						+ Json::Sha256Utf8(Work[Index].PackageName).Left(16);
					Target.TerminalState = Target.bDeleted
						? TEXT("committed")
						: TEXT("state_unknown");
				}
				Result.TerminalState = DeletedTargetCount == Work.Num()
					? TEXT("committed")
					: DeletedTargetCount > 0
					? TEXT("partially_committed")
					: TEXT("rolled_back");
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
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutOwner.Reset();
		OutError.Reset();
		JournalRoot = FPaths::ConvertRelativePathToFull(JournalRoot);
		JournalOwnerId.TrimStartAndEndInline();
		if (JournalRoot.IsEmpty() || JournalOwnerId.IsEmpty())
		{
			auto Actual = MakeShared<FJsonObject>();
			Actual->SetBoolField(TEXT("journal_root_available"), !JournalRoot.IsEmpty());
			Actual->SetBoolField(TEXT("journal_owner_available"), !JournalOwnerId.IsEmpty());
			auto Expected = MakeShared<FJsonObject>();
			Expected->SetBoolField(TEXT("journal_root_available"), true);
			Expected->SetBoolField(TEXT("journal_owner_available"), true);
			OutError = DeleteFact(TEXT("asset_delete_owner_config_invalid"), TEXT("The asset delete owner requires a journal root and journal owner."),
				FString(), TEXT("/owner/configuration"), MakeShared<FJsonValueObject>(Actual), MakeShared<FJsonValueObject>(Expected), TEXT("asset_delete_config"));
			auto Internal = MakeShared<FJsonObject>();
			Internal->SetStringField(TEXT("journal_root"), JournalRoot);
			Internal->SetStringField(TEXT("journal_owner"), JournalOwnerId);
			OutError.ActualEvidence = MakeShared<FJsonValueObject>(Internal);
			return false;
		}
		OutOwner = MakeShared<FAssetDeleteOwner>(
			MoveTemp(JournalRoot), MoveTemp(JournalOwnerId));
		return true;
	}
}

#include "Composition/SimpleAuthoring/UeAgentSimpleAuthoringDurableService.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeLock.h"
#include "UObject/Package.h"

namespace UE::UAI::Composition::SimpleAuthoring
{
	namespace
	{
		using namespace AssetDurable;
		using namespace ContextResource;

		void SetError(
			FUeAgentSimpleAuthoringProductionError& OutError,
			FString Code,
			FString Detail = FString())
		{
			OutError.Code = MoveTemp(Code);
			OutError.Detail = MoveTemp(Detail);
		}

		class FSimpleAuthoringReconciliationOracle final
			: public IAssetDurableReconciliationOracle
		{
		public:
			virtual bool TryConfirm(
				const FAssetDurableSessionRef& Session,
				const TArray<FDurableArtifactState>& ObservedStates,
				const EDurableStatus ProposedStatus,
				FUeAgentResourceTracker& Resources,
				FAssetDurableError& OutError) override
			{
				OutError = FAssetDurableError();
				if (ProposedStatus != EDurableStatus::RolledBack
					&& ProposedStatus != EDurableStatus::Verified)
				{
					OutError = FAssetDurableError(
						TEXT("durable_reconcile_status_invalid"));
					return false;
				}
				TSet<FString> CheckedPackages;
				for (const FDurableArtifact& Artifact :
					Session->GetInventory().GetArtifacts())
				{
					if (Artifact.GetArtifactKind()
							!= EDurableArtifactKind::Package
						|| CheckedPackages.Contains(
							Artifact.GetDurableLocation()))
					{
						continue;
					}
					const FString PackageName =
						Artifact.GetDurableLocation();
					CheckedPackages.Add(PackageName);
					const FDurableArtifact* HeaderArtifact =
						Session->GetInventory().GetArtifacts()
						.FindByPredicate(
							[&PackageName](
								const FDurableArtifact& Candidate)
							{
								return Candidate.GetArtifactKind()
										== EDurableArtifactKind::Package
									&& Candidate.GetDurableLocation()
										== PackageName
									&& Candidate.GetRole().Contains(
										TEXT("header"));
							});
					const FDurableArtifactState* HeaderState =
						HeaderArtifact
						? ObservedStates.FindByPredicate(
							[HeaderArtifact](
								const FDurableArtifactState& State)
							{
								return State.GetArtifactId()
									== HeaderArtifact->GetArtifactId();
							})
						: nullptr;
					if (!HeaderState || !HeaderState->IsKnown())
					{
						OutError = FAssetDurableError(
							TEXT("durable_reconcile_readback_unconfirmed"),
							PackageName + TEXT(":header_state"));
						return false;
					}

					FString ExistingFilename;
					const bool bFileExists =
						FPackageName::DoesPackageExist(
							PackageName,
							&ExistingFilename);
					UPackage* const LoadedPackage =
						FindPackage(nullptr, *PackageName);
					TArray<FAssetData> Assets;
					FAssetRegistryModule::GetRegistry()
						.GetAssetsByPackageName(
							FName(*PackageName),
							Assets,
							false);
					const bool bExpectedPresent =
						HeaderState->GetPresence()
							== EDurableStatePresence::Present;
					const bool bRegistryConsistent = bExpectedPresent
						? !Assets.IsEmpty()
						: Assets.IsEmpty();
					const bool bLoadedConsistent = bExpectedPresent
						? !LoadedPackage
							|| !LoadedPackage->IsDirty()
						: LoadedPackage == nullptr;
					if (bFileExists != bExpectedPresent
						|| !bRegistryConsistent
						|| !bLoadedConsistent)
					{
						OutError = FAssetDurableError(
							TEXT("durable_reconcile_readback_unconfirmed"),
							FString::Printf(
								TEXT("%s:file=%d,registry=%d,loaded=%d,dirty=%d"),
								*PackageName,
								bFileExists ? 1 : 0,
								Assets.IsEmpty() ? 0 : 1,
								LoadedPackage ? 1 : 0,
								LoadedPackage && LoadedPackage->IsDirty()
									? 1
									: 0));
						return false;
					}
				}
				return true;
			}
		};

		class FSimpleAuthoringDurableService final
			: public IUeAgentSimpleAuthoringAssetDurableService
		{
		public:
			explicit FSimpleAuthoringDurableService(
				FUeAgentSimpleAuthoringDurableServiceConfig InConfig)
				: Config(MoveTemp(InConfig))
			{
			}

			bool TryPrepare(
				const FAssetDurableRequest& Request,
				FUeAgentResourceTracker& Resources,
				FAssetDurableSessionPtr& OutSession,
				FAssetDurableError& OutError) override
			{
				IAssetDurableBackend* Backend = nullptr;
				if (!TrySelectBackend(
						Request,
						Backend,
						OutError))
				{
					return false;
				}
				int64 DurableFencingToken = -1;
				{
					FScopeLock Lock(&Mutex);
					if (!TryAcquireAssetDurableFence(
							Config.JournalRoot,
							Config.StableJournalOwnerId,
							Resources,
							DurableFencingToken,
							OutError))
					{
						return false;
					}
				}
				const FAssetDurableRequest DurableRequest(
					Request.GetPlan(),
					Request.GetTargets(),
					DurableFencingToken,
					Config.StableJournalOwnerId,
					Request.GetAtomicityMode());
				return TryPrepareAssetDurability(
					DurableRequest,
					*Backend,
					Config.JournalRoot,
					Resources,
					OutSession,
					OutError);
			}

			bool TryEnlist(
				const FAssetDurableSessionRef& Session,
				const TArray<FAssetDurableTarget>& MutationTargets,
				const int64 CurrentFencingToken,
				FUeAgentResourceTracker& Resources,
				FAssetDurableSessionPtr& OutSession,
				FAssetDurableError& OutError) override
			{
				IAssetDurableBackend* Backend = nullptr;
				if (!TrySelectBackend(
						Session->GetRequest(),
						Backend,
						OutError)
					|| CurrentFencingToken < 0)
				{
					if (!OutError.IsPresent())
					{
						OutError = FAssetDurableError(
							TEXT("simple_authoring_operation_fence_invalid"));
					}
					return false;
				}
				return TryEnlistAssetDurableTargets(
					Session,
					MutationTargets,
					*Backend,
					Session->GetRequest().GetFencingToken(),
					Resources,
					OutSession,
					OutError);
			}

			bool TryPersist(
				const FAssetDurableSessionRef& Session,
				const int64 CurrentFencingToken,
				FUeAgentResourceTracker& Resources,
				FDurableReceipt& OutReceipt,
				FAssetDurableError& OutError) override
			{
				IAssetDurableBackend* Backend = nullptr;
				if (!TrySelectBackend(
						Session->GetRequest(),
						Backend,
						OutError)
					|| CurrentFencingToken < 0
					|| !TryPersistAssetDurability(
						Session,
						*Backend,
						Session->GetRequest().GetFencingToken(),
						Resources,
						OutReceipt,
						OutError))
				{
					return false;
				}
				if (OutReceipt.GetStatus()
						!= EDurableStatus::PersistedUnverified
					|| !OutReceipt
						.RequiresIndependentVerification())
				{
					OutError = FAssetDurableError(
						TEXT(
							"simple_authoring_durable_"
							"persist_not_persisted"),
						LexToString(OutReceipt.GetStatus()));
					return false;
				}

				const FAssetDurableSessionRef Persisted =
					MakeShared<const FAssetDurableSession>(
						Session->GetRequest(),
						Session->GetInventory(),
						Session->GetJournalId(),
						Session->GetJournalPath(),
						Session->GetJournalRoot(),
						Session->GetTransactionRoot(),
						Session->GetStagingRoot(),
						OutReceipt.GetPhaseHistory(),
						OutReceipt.GetPreMutationStates(),
						OutReceipt.GetExpectedPersistedStates());
				{
					FScopeLock Lock(&Mutex);
					PersistedByJournal.Add(
						Session->GetJournalId(),
						Persisted);
				}
				for (const FAssetDurableTarget& Target :
					Session->GetRequest().GetTargets())
				{
					if (Target.GetArtifactKind()
							== EDurableArtifactKind::Package
						&& Target.GetIntent()
							== EAssetDurableIntent::Save)
					{
						if (UPackage* Package = FindPackage(
								nullptr,
								*Target.GetPackageName()))
						{
							Package->SetDirtyFlag(false);
						}
					}
				}
				return true;
			}

			bool TryRollback(
				const FAssetDurableSessionRef& Session,
				const int64 CurrentFencingToken,
				const bool bMemoryRollbackConfirmed,
				FUeAgentResourceTracker& Resources,
				FDurableReceipt& OutReceipt,
				FAssetDurableError& OutError) override
			{
				IAssetDurableBackend* Backend = nullptr;
				if (!TrySelectBackend(
						Session->GetRequest(),
						Backend,
						OutError)
					|| CurrentFencingToken < 0
					|| !TryRollbackAssetDurability(
						Session,
						*Backend,
						Session->GetRequest().GetFencingToken(),
						bMemoryRollbackConfirmed,
						Resources,
						OutReceipt,
						OutError))
				{
					return false;
				}
				if (OutReceipt.GetStatus()
					!= EDurableStatus::RolledBack)
				{
					OutError = FAssetDurableError(
						TEXT(
							"simple_authoring_durable_"
							"rollback_unconfirmed"),
						LexToString(OutReceipt.GetStatus()));
					return false;
				}
				FScopeLock Lock(&Mutex);
				PersistedByJournal.Remove(
					Session->GetJournalId());
				return true;
			}

			bool TryInspect(
				const FString& JournalId,
				FUeAgentResourceTracker& Resources,
				FAssetDurableSessionPtr& OutSession,
				TArray<FDurableArtifactState>& OutObservedStates,
				FAssetDurableError& OutError) override
			{
				return TryInspectAssetDurableJournal(
					Config.JournalRoot,
					JournalId,
					GetAllowedArtifactRoots(),
					Resources,
					OutSession,
					OutObservedStates,
					OutError);
			}

			bool TryReconcile(
				const FString& JournalId,
				const FString& ExpectedJournalOwnerId,
				const int64 ExpectedFencingToken,
				const FString& VerificationId,
				FUeAgentResourceTracker& Resources,
				FDurableReceipt& OutReceipt,
				FAssetDurableError& OutError) override
			{
				FSimpleAuthoringReconciliationOracle Oracle;
				return TryReconcileAssetDurability(
					Config.JournalRoot,
					JournalId,
					GetAllowedArtifactRoots(),
					ExpectedJournalOwnerId,
					ExpectedFencingToken,
					VerificationId,
					Oracle,
					Resources,
					OutReceipt,
					OutError);
			}

			bool TryAcknowledge(
				const FAssetDurableSessionRef& Session,
				const FDurableVerificationAcknowledgement&
					Acknowledgement,
				FUeAgentResourceTracker& Resources,
				FAssetDurableError& OutError) override
			{
				FAssetDurableSessionPtr Persisted;
				{
					FScopeLock Lock(&Mutex);
					Persisted = PersistedByJournal.FindRef(
						Session->GetJournalId());
				}
				if (!Persisted.IsValid())
				{
					OutError = FAssetDurableError(
						TEXT(
							"simple_authoring_durable_"
							"persisted_session_missing"),
						Session->GetJournalId());
					return false;
				}
				if (!TryAcknowledgeAssetDurability(
						Persisted.ToSharedRef(),
						Acknowledgement,
						Acknowledgement.GetFencingToken(),
						Resources,
						OutError))
				{
					return false;
				}
				FScopeLock Lock(&Mutex);
				PersistedByJournal.Remove(
					Session->GetJournalId());
				return true;
			}

		private:
			TArray<FString> GetAllowedArtifactRoots() const
			{
				TArray<FString> Result;
				for (const TSharedPtr<IAssetDurableBackend,
					ESPMode::ThreadSafe>& Backend :
					{Config.PackageBackend, Config.ProjectConfigBackend})
				{
					if (!Backend.IsValid())
					{
						continue;
					}
					for (const FString& Root :
						Backend->GetAllowedArtifactRoots())
					{
						Result.AddUnique(Root);
					}
				}
				return Result;
			}

			bool TrySelectBackend(
				const FAssetDurableRequest& Request,
				IAssetDurableBackend*& OutBackend,
				FAssetDurableError& OutError) const
			{
				OutBackend = nullptr;
				OutError = FAssetDurableError();
				if (!Request.IsValid()
					|| Request.GetTargets().IsEmpty())
				{
					OutError = FAssetDurableError(
						TEXT(
							"simple_authoring_durable_"
							"request_invalid"));
					return false;
				}
				const EDurableArtifactKind ArtifactKind =
					Request.GetTargets()[0].GetArtifactKind();
				for (const FAssetDurableTarget& Target :
					Request.GetTargets())
				{
					if (Target.GetArtifactKind()
						!= ArtifactKind)
					{
						OutError = FAssetDurableError(
							TEXT(
								"simple_authoring_durable_"
								"artifact_kinds_mixed"));
						return false;
					}
				}
				const TSharedPtr<
					IAssetDurableBackend,
					ESPMode::ThreadSafe>& Selected =
					ArtifactKind
							== EDurableArtifactKind::Package
						? Config.PackageBackend
						: Config.ProjectConfigBackend;
				if (!Selected.IsValid())
				{
					OutError = FAssetDurableError(
						TEXT(
							"simple_authoring_durable_"
							"backend_missing"),
						LexToString(ArtifactKind));
					return false;
				}
				OutBackend = Selected.Get();
				return true;
			}

			FUeAgentSimpleAuthoringDurableServiceConfig Config;
			mutable FCriticalSection Mutex;
			TMap<FString, FAssetDurableSessionPtr>
				PersistedByJournal;
		};
	}

	bool FUeAgentSimpleAuthoringDurableServiceFactory::TryCreate(
		const FUeAgentSimpleAuthoringDurableServiceConfig& Config,
		TSharedPtr<
			IUeAgentSimpleAuthoringAssetDurableService,
			ESPMode::ThreadSafe>& OutService,
		FUeAgentSimpleAuthoringProductionError& OutError)
	{
		OutService.Reset();
		OutError = FUeAgentSimpleAuthoringProductionError();
		if (Config.JournalRoot.TrimStartAndEnd().IsEmpty())
		{
			SetError(
				OutError,
				TEXT(
					"simple_authoring_durable_"
					"journal_root_missing"));
			return false;
		}
		if (Config.StableJournalOwnerId.TrimStartAndEnd().IsEmpty())
		{
			SetError(
				OutError,
				TEXT(
					"simple_authoring_durable_"
					"stable_owner_missing"));
			return false;
		}
		if (!Config.PackageBackend.IsValid()
			&& !Config.ProjectConfigBackend.IsValid())
		{
			SetError(
				OutError,
				TEXT(
					"simple_authoring_durable_"
					"backend_missing"));
			return false;
		}
		OutService = MakeShared<
			FSimpleAuthoringDurableService,
			ESPMode::ThreadSafe>(Config);
		return OutService.IsValid();
	}
}

#include "Composition/SimpleAuthoring/UeAgentSimpleAuthoringDurableService.h"

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
				return TryPrepareAssetDurability(
					Request,
					*Backend,
					Config.JournalRoot,
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
					|| !TryPersistAssetDurability(
						Session,
						*Backend,
						CurrentFencingToken,
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
						OutReceipt.GetPhaseHistory());
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
					|| !TryRollbackAssetDurability(
						Session,
						*Backend,
						CurrentFencingToken,
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

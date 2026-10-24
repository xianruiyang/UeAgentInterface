#include "Composition/SimpleAuthoring/UeAgentSimpleAuthoringDurableService.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeLock.h"
#include "UObject/Package.h"

namespace UE::UAI::Composition::SimpleAuthoring
{
	namespace
	{
		using namespace AssetDurable;
		using namespace ContextResource;

		Contract::FDiagnostic DurableFailure(const FString& Code, const FString& Reason)
		{
			auto Diagnostic = Contract::FDiagnostic::Failure(Code, Reason, TEXT("asset_durable"));
			Diagnostic.SourceKind = TEXT("native_result");
			return Diagnostic;
		}

		bool TryCheckCurrentFence(const int64 Token, FAssetDurableError& OutError)
		{
			if (Token >= 0) return true;
			OutError = DurableFailure(TEXT("simple_authoring_operation_fence_invalid"),
				TEXT("The current operation fencing token must not be negative."));
			OutError.PropertyPath = TEXT("current_fencing_token");
			OutError.ActualValue = MakeShared<FJsonValueString>(::LexToString(Token));
			OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT(">= 0"));
			return false;
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
					&& ProposedStatus != EDurableStatus::Verified
					&& ProposedStatus != EDurableStatus::Committed)
				{
					OutError = DurableFailure(TEXT("durable_reconcile_status_invalid"),
						TEXT("Package reconciliation requires a rolled-back, verified or committed durable status."));
					OutError.PropertyPath = TEXT("proposed_status");
					OutError.ActualValue = MakeShared<FJsonValueString>(LexToString(ProposedStatus));
					OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("rolled_back, verified or committed"));
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
						OutError = DurableFailure(TEXT("durable_reconcile_readback_unconfirmed"),
							!HeaderArtifact ? TEXT("The package inventory contains no header artifact.")
								: !HeaderState ? TEXT("No observed state was returned for the package header artifact.")
									: TEXT("The observed package header state is unknown."));
						OutError.TargetPath = PackageName;
						OutError.PropertyPath = !HeaderArtifact ? TEXT("header_artifact")
							: !HeaderState ? TEXT("header_state") : TEXT("header_state/known");
						OutError.ActualValuePresent = HeaderState != nullptr;
						OutError.ExpectedValuePresent = true;
						if (HeaderState)
						{
							OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
							OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
						}
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
						const auto AddMismatch = [&](const FString& Field, const FString& Reason,
							TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected)
						{
							auto Diagnostic = DurableFailure(TEXT("durable_reconcile_readback_unconfirmed"), Reason);
							Diagnostic.TargetPath = PackageName;
							Diagnostic.PropertyPath = Field;
							Diagnostic.ActualValue = MoveTemp(Actual);
							Diagnostic.ExpectedValue = MoveTemp(Expected);
							OutError.Append(Diagnostic);
						};
						if (bFileExists != bExpectedPresent)
							AddMismatch(TEXT("file/present"), TEXT("Package file presence differs from the observed header state."),
								MakeShared<FJsonValueBoolean>(bFileExists), MakeShared<FJsonValueBoolean>(bExpectedPresent));
						if (!bRegistryConsistent)
							AddMismatch(TEXT("asset_registry/asset_count"), TEXT("Asset registry entries differ from the observed package header presence."),
								MakeShared<FJsonValueNumber>(Assets.Num()),
								MakeShared<FJsonValueString>(bExpectedPresent ? TEXT("at least one asset") : TEXT("zero assets")));
						if (!bLoadedConsistent)
							AddMismatch(bExpectedPresent ? TEXT("loaded_package/dirty") : TEXT("loaded_package/present"),
								bExpectedPresent ? TEXT("The loaded package remains dirty after durable reconciliation.")
									: TEXT("The package remains loaded although the observed header is absent."),
								MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false));
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
					|| !TryCheckCurrentFence(CurrentFencingToken, OutError))
				{
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
					|| !TryCheckCurrentFence(CurrentFencingToken, OutError)
					|| !TryPersistAssetDurabilityForVerification(
						Session,
						*Backend,
						Session->GetRequest().GetFencingToken(),
						Resources,
						OutReceipt,
						OutError))
				{
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
							// Autosave staging preserves this flag; publication has now
							// completed the first real save of the resident package.
							Package->ClearPackageFlags(PKG_NewlyCreated);
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
					|| !TryCheckCurrentFence(CurrentFencingToken, OutError)
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
					OutError = OutReceipt.GetFailureDiagnostic();
					if (OutError.IsEmpty())
					{
						OutError = DurableFailure(TEXT("simple_authoring_durable_rollback_unconfirmed"),
							TEXT("The durable rollback returned a receipt whose status does not confirm rollback."));
						OutError.PropertyPath = TEXT("receipt/status");
						OutError.ActualValue = MakeShared<FJsonValueString>(LexToString(OutReceipt.GetStatus()));
						OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("rolled_back"));
					}
					if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Session->GetJournalId();
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
					OutError = DurableFailure(TEXT("simple_authoring_durable_persisted_session_missing"),
						TEXT("No persisted session is available for this journal's verification acknowledgement."));
					OutError.SubjectId = Session->GetJournalId();
					OutError.PropertyPath = TEXT("persisted_session");
					OutError.ActualValuePresent = false;
					OutError.ExpectedValuePresent = true;
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
				if (!Request.IsValid())
				{
					OutError = Request.GetValidationDiagnostic();
					return false;
				}
				const EDurableArtifactKind ArtifactKind =
					Request.GetTargets()[0].GetArtifactKind();
				for (int32 TargetIndex = 0; TargetIndex < Request.GetTargets().Num(); ++TargetIndex)
				{
					const FAssetDurableTarget& Target = Request.GetTargets()[TargetIndex];
					if (Target.GetArtifactKind()
						!= ArtifactKind)
					{
						OutError = DurableFailure(TEXT("simple_authoring_durable_artifact_kinds_mixed"),
							TEXT("All targets in one authoring durable request must use the same artifact kind."));
						OutError.PropertyPath = FString::Printf(TEXT("targets/%d/artifact_kind"), TargetIndex);
						OutError.ActualValue = MakeShared<FJsonValueString>(LexToString(Target.GetArtifactKind()));
						OutError.ExpectedValue = MakeShared<FJsonValueString>(LexToString(ArtifactKind));
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
					OutError = DurableFailure(TEXT("simple_authoring_durable_backend_missing"),
						FString::Printf(TEXT("No durable backend is configured for artifact kind '%s'."), *LexToString(ArtifactKind)));
					OutError.PropertyPath = ArtifactKind == EDurableArtifactKind::Package
						? TEXT("package_backend") : TEXT("project_config_backend");
					OutError.ActualValuePresent = false;
					OutError.ExpectedValuePresent = true;
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
			OutError = DurableFailure(TEXT("simple_authoring_durable_journal_root_missing"),
				TEXT("The durable journal root must not be empty or whitespace."));
			OutError.PropertyPath = TEXT("journal_root");
			OutError.ActualValue = MakeShared<FJsonValueString>(Config.JournalRoot);
			OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("non-whitespace journal root"));
			return false;
		}
		if (Config.StableJournalOwnerId.TrimStartAndEnd().IsEmpty())
		{
			OutError = DurableFailure(TEXT("simple_authoring_durable_stable_owner_missing"),
				TEXT("The durable journal owner ID must not be empty or whitespace."));
			OutError.PropertyPath = TEXT("stable_journal_owner_id");
			OutError.ActualValue = MakeShared<FJsonValueString>(Config.StableJournalOwnerId);
			OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("non-whitespace owner ID"));
			return false;
		}
		if (!Config.PackageBackend.IsValid()
			&& !Config.ProjectConfigBackend.IsValid())
		{
			OutError = DurableFailure(TEXT("simple_authoring_durable_backend_missing"),
				TEXT("At least one package or project-config durable backend must be configured."));
			OutError.PropertyPath = TEXT("backends");
			auto Actual = MakeShared<FJsonObject>();
			Actual->SetBoolField(TEXT("package"), false);
			Actual->SetBoolField(TEXT("project_config"), false);
			OutError.ActualValue = MakeShared<FJsonValueObject>(Actual);
			OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("at least one configured backend"));
			return false;
		}
		OutService = MakeShared<
			FSimpleAuthoringDurableService,
			ESPMode::ThreadSafe>(Config);
		return true;
	}
}

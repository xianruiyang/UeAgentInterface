#include "Composition/ReferenceCrud/UeAgentReferenceCrudApplication.h"

#include "Async/Async.h"
#include "Core/AssetDurable/UeAgentAssetDurable.h"
#include "Core/BindingGateway/Operation/UeAgentOperationBinding.h"
#include "Core/BindingGateway/Ticket/UeAgentBindingTicketJson.h"
#include "Core/ContextResource/UeAgentContextResource.h"
#include "Core/ExecutionMutation/UeAgentExecutionMutation.h"
#include "Core/PlanningPreflight/UeAgentPlanningPreflight.h"
#include "Core/Projection/UeAgentProjection.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "Core/Verification/UeAgentVerification.h"
#include "DomainAdapters/GenericEffect/UeAgentGenericEffectAdapter.h"
#include "DomainAdapters/ReferenceAsset/UeAgentReferenceAssetAdapter.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/Event.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/DateTime.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Package.h"

namespace UE::UAI::Composition::ReferenceCrud
{
	namespace
	{
		using namespace AssetDurable;
		using namespace BindingGateway::Operation;
		using namespace BindingGateway::Ticket;
		using namespace ContextResource;
		using namespace Contract;
		using namespace Discovery;
		using namespace DomainAdapters::GenericEffect;
		using namespace DomainAdapters::ReferenceAsset;
		using namespace ExecutionMutation;
		using namespace PlanningPreflight;
		using namespace Projection;
		using namespace StateSemantic;
		using namespace Verification;

		const FString RequiredWriteScope =
			TEXT("uai:reference:write");

		void SetError(
			FUeAgentReferenceCrudApplicationError& OutError,
			const FString& Code,
			const FString& Detail)
		{
			OutError = {Code, Detail};
		}

		FString JsonText(const TSharedRef<FJsonObject>& Object)
		{
			FString Text;
			const TSharedRef<
				TJsonWriter<
					TCHAR,
					TCondensedJsonPrintPolicy<TCHAR>>> Writer =
				TJsonWriterFactory<
					TCHAR,
					TCondensedJsonPrintPolicy<TCHAR>>::Create(
						&Text);
			FJsonSerializer::Serialize(Object, Writer);
			return Text;
		}

		FUeAgentOperationResult ErrorResult(
			const FUeAgentOperationRequest& Request,
			const FString& Code,
			const FString& SubjectId,
			const FString& Detail)
		{
			TSharedRef<FJsonObject> Error = MakeShared<FJsonObject>();
			Error->SetStringField(TEXT("code"), Code);
			Error->SetStringField(TEXT("subject_id"), SubjectId);
			Error->SetStringField(TEXT("detail"), Detail);
			TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
			Root->SetObjectField(TEXT("operation_error"), Error);
			return FUeAgentOperationResult(
				Request.GetDescriptorIdentity(),
				Request.GetRequestId(),
				FUeAgentContractValue(JsonText(Root)));
		}

		TSharedPtr<FJsonValue> CanonicalValueToJson(
			const FUeAgentCanonicalValue& Value)
		{
			switch (Value.GetKind())
			{
			case EUeAgentValueKind::Null:
				return MakeShared<FJsonValueNull>();
			case EUeAgentValueKind::Boolean:
				return MakeShared<FJsonValueBoolean>(
					Value.GetBoolean());
			case EUeAgentValueKind::String:
				return MakeShared<FJsonValueString>(
					Value.GetText());
			case EUeAgentValueKind::Integer:
			case EUeAgentValueKind::Decimal:
				return MakeShared<FJsonValueString>(
					Value.GetLexeme());
			case EUeAgentValueKind::Array:
			{
				TArray<TSharedPtr<FJsonValue>> Items;
				for (const FUeAgentCanonicalValueRef& Item :
					Value.GetArrayItems())
				{
					Items.Add(CanonicalValueToJson(*Item));
				}
				return MakeShared<FJsonValueArray>(MoveTemp(Items));
			}
			case EUeAgentValueKind::Object:
			{
				TSharedRef<FJsonObject> Object =
					MakeShared<FJsonObject>();
				for (const TPair<
					FString,
					FUeAgentCanonicalValueRef>& Item :
					Value.GetObjectItems())
				{
					Object->SetField(
						Item.Key,
						CanonicalValueToJson(*Item.Value));
				}
				return MakeShared<FJsonValueObject>(Object);
			}
			case EUeAgentValueKind::Temporal:
			{
				TSharedRef<FJsonObject> Object =
					MakeShared<FJsonObject>();
				if (Value.GetTemporal().IsValid())
				{
					Object->SetStringField(
						TEXT("clock_id"),
						Value.GetTemporal()->GetClockId());
					Object->SetStringField(
						TEXT("tick"),
						Value.GetTemporal()->GetTick());
					Object->SetStringField(
						TEXT("rate_numerator"),
						Value.GetTemporal()
							->GetRateNumerator());
					Object->SetStringField(
						TEXT("rate_denominator"),
						Value.GetTemporal()
							->GetRateDenominator());
					Object->SetStringField(
						TEXT("state"),
						Value.GetTemporal()->GetState());
				}
				return MakeShared<FJsonValueObject>(Object);
			}
			case EUeAgentValueKind::Spatial:
			default:
				return MakeShared<FJsonValueNull>();
			}
		}

		FUeAgentResourceBudget OperationBudget(
			const int64 MaxFrameBytes)
		{
			const int64 ByteLimit = FMath::Max<int64>(
				MaxFrameBytes * 8,
				8 * 1024 * 1024);
			return FUeAgentResourceBudget({
				{EUeAgentResourceDimension::InputBytes, ByteLimit},
				{EUeAgentResourceDimension::OutputBytes, ByteLimit},
				{EUeAgentResourceDimension::StringBytes, ByteLimit},
				{EUeAgentResourceDimension::Nodes, 32768},
				{EUeAgentResourceDimension::Items, 32768},
				{EUeAgentResourceDimension::Files, 256},
				{EUeAgentResourceDimension::ReferenceEdges, 32768},
				{EUeAgentResourceDimension::WorkUnits, 262144},
				{
					EUeAgentResourceDimension::EstimatedMemoryBytes,
					ByteLimit * 8,
				},
			});
		}

		bool ParseIntent(
			const FString& Text,
			EChangeIntent& OutIntent)
		{
			if (Text.Equals(TEXT("create"), ESearchCase::IgnoreCase))
			{
				OutIntent = EChangeIntent::Create;
				return true;
			}
			if (Text.Equals(TEXT("update"), ESearchCase::IgnoreCase))
			{
				OutIntent = EChangeIntent::Update;
				return true;
			}
			if (Text.Equals(TEXT("replace"), ESearchCase::IgnoreCase))
			{
				OutIntent = EChangeIntent::Replace;
				return true;
			}
			if (Text.Equals(TEXT("delete"), ESearchCase::IgnoreCase))
			{
				OutIntent = EChangeIntent::Delete;
				return true;
			}
			return false;
		}

		bool ParseProjection(
			const TSharedPtr<FJsonObject>& Object,
			TOptional<FUeAgentProjectionRequest>& OutProjection)
		{
			if (!Object.IsValid())
			{
				return false;
			}
			FString ProfileId = TEXT("full");
			bool bDefaults = true;
			bool bDetails = true;
			bool bProvenance = true;
			double MaxItems = 25.0;
			double TokenBudget = 4096.0;
			Object->TryGetStringField(
				TEXT("profile_id"),
				ProfileId);
			Object->TryGetBoolField(
				TEXT("include_defaults"),
				bDefaults);
			Object->TryGetBoolField(
				TEXT("include_details"),
				bDetails);
			Object->TryGetBoolField(
				TEXT("include_provenance"),
				bProvenance);
			Object->TryGetNumberField(
				TEXT("max_items"),
				MaxItems);
			Object->TryGetNumberField(
				TEXT("token_budget"),
				TokenBudget);
			OutProjection.Emplace(
				FUeAgentProjectionProfile(
					ProfileId,
					bDefaults,
					bDetails,
					bProvenance),
				FUeAgentPageRequest(
					FMath::Max(1, static_cast<int32>(MaxItems)),
					FString()),
				TOptional<int64>(
					FMath::Max<int64>(
						1,
						static_cast<int64>(TokenBudget))),
				TArray<FUeAgentArtifactDescriptor>(),
				TArray<FUeAgentProjectionDiagnostic>());
			return OutProjection.IsSet()
				&& OutProjection->IsValid();
		}

		bool ParseProposedValue(
			const TSharedPtr<FJsonValue>& JsonValue,
			EChangeIntent Intent,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FString& OutError)
		{
			OutValue.Reset();
			if (Intent == EChangeIntent::Delete)
			{
				if (JsonValue.IsValid()
					&& JsonValue->Type != EJson::Null)
				{
					OutError =
						TEXT("reference_crud_delete_value_forbidden");
					return false;
				}
				return true;
			}
			if (!JsonValue.IsValid()
				|| JsonValue->Type != EJson::Object)
			{
				OutError =
					TEXT("reference_crud_proposed_value_missing");
				return false;
			}
			const TSharedPtr<FJsonObject> Object =
				JsonValue->AsObject();
			FString Kind;
			FString Text;
			if (!Object.IsValid()
				|| !Object->TryGetStringField(
					TEXT("kind"),
					Kind)
				|| Kind != TEXT("string")
				|| !Object->TryGetStringField(
					TEXT("text"),
					Text))
			{
				OutError =
					TEXT("reference_crud_proposed_value_invalid");
				return false;
			}
			FUeAgentStateSemanticError StateError;
			if (!FUeAgentCanonicalValue::TryString(
				Text,
				Resources,
				OutValue,
				StateError))
			{
				OutError = StateError.Code;
				return false;
			}
			return true;
		}

		class FReferenceSessionSource final
			: public IUeAgentOperationSessionSource
		{
		public:
			explicit FReferenceSessionSource(
				FUeAgentOperationSessionIdentity InSession)
				: Session(MoveTemp(InSession))
			{
			}

			FUeAgentOperationSessionIdentity GetCurrent()
				const override
			{
				return Session;
			}

		private:
			FUeAgentOperationSessionIdentity Session;
		};

		class FReferencePlanner final
			: public IUeAgentOperationPlanner
		{
		public:
			bool TryPlan(
				const FPlanningRequest& Request,
				const FUeAgentOperationContext& Context,
				FPlanningResult& OutResult,
				FUeAgentOperationBindingError& OutError) override
			{
				FPlanningPreflightError Error;
				if (!TryPlanChanges(
					Request,
					Context.GetResourceTracker(),
					OutResult,
					Error))
				{
					OutError.Code = Error.GetCode();
					OutError.SubjectId =
						Error.GetIdentityKey();
					OutError.Detail = Error.GetDetail();
					return false;
				}
				return true;
			}
		};

		class FInlineGameThreadScheduler final
			: public IExecutionScheduler
		{
		public:
			bool Schedule(
				EExecutionThreadAffinity Affinity,
				const FString& OperationId,
				TFunction<void()> Callback,
				FExecutionMutationError& OutError) override
			{
				if (Affinity
						!= EExecutionThreadAffinity::GameThread
					|| !IsInGameThread()
					|| !Callback)
				{
					OutError = FExecutionMutationError(
						TEXT(
							"reference_crud_scheduler_thread_invalid"),
						OperationId);
					return false;
				}
				Callback();
				return true;
			}

			bool IsCurrentThread(
				EExecutionThreadAffinity Affinity)
				const override
			{
				return Affinity
						== EExecutionThreadAffinity::GameThread
					&& IsInGameThread();
			}
		};

		class FReferenceDurableSession final
			: public IExecutionDurableSession
		{
		public:
			explicit FReferenceDurableSession(
				FAssetDurableSessionRef InSession)
				: Session(MoveTemp(InSession))
			{
			}

			FAssetDurableSessionRef Session;
		};

		class FReferenceDurablePort final
			: public IDurableExecutionPort
			, public IDurableVerificationAcknowledgerPort
		{
		public:
			explicit FReferenceDurablePort(FString InJournalRoot)
				: JournalRoot(MoveTemp(InJournalRoot))
			{
			}

			bool Prepare(
				const FExecutionMutationRequest& Request,
				int64 FencingToken,
				FUeAgentResourceTracker& Resources,
				FExecutionDurableSessionPtr& OutSession,
				FExecutionMutationError& OutError) override
			{
				OutSession.Reset();
				TArray<FAssetDurableTarget> Targets;
				for (const FPlannedChange& Change :
					Request.GetPlan()
						->GetChangeSet()
						.GetChanges())
				{
					FString PackageName;
					FString ObjectPath;
					FString PathError;
					if (!TryNormalizeReferenceAssetPath(
						Change.GetIdentity().GetOwnerId(),
						PackageName,
						ObjectPath,
						PathError))
					{
						OutError = FExecutionMutationError(
							PathError,
							Change.GetIdentity()
								.GetOwnerId());
						return false;
					}
					Targets.Emplace(
						Change.GetIdentity(),
						PackageName,
						Change.GetIntent()
								== EChangeIntent::Delete
							? EAssetDurableIntent::Delete
							: EAssetDurableIntent::Save,
						ERollbackGrade::DurableBackup);
				}
				const FAssetDurableRequest DurableRequest(
					Request.GetPlan(),
					MoveTemp(Targets),
					FencingToken,
					Request.GetOwnerLeaseId(),
					EAtomicityMode::CompensatedSaga);
				FAssetDurableSessionPtr Session;
				FAssetDurableError Error;
				if (!TryPrepareAssetDurability(
					DurableRequest,
					Backend,
					JournalRoot,
					Resources,
					Session,
					Error)
					|| !Session.IsValid())
				{
					OutError = FExecutionMutationError(
						Error.GetCode(),
						Error.GetDetail());
					return false;
				}
				OutSession = MakeShared<
					FReferenceDurableSession,
					ESPMode::ThreadSafe>(
					Session.ToSharedRef());
				return true;
			}

			bool Persist(
				const FExecutionDurableSessionRef& Session,
				int64 FencingToken,
				const FMutationFacts& Facts,
				FUeAgentResourceTracker& Resources,
				FDurableReceipt& OutReceipt,
				FExecutionMutationError& OutError) override
			{
				(void)Facts;
				const TSharedRef<FReferenceDurableSession>
					Typed = StaticCastSharedRef<
						FReferenceDurableSession>(Session);
				FAssetDurableError Error;
				if (!TryPersistAssetDurability(
					Typed->Session,
					Backend,
					FencingToken,
					Resources,
					OutReceipt,
					Error))
				{
					OutError = FExecutionMutationError(
						Error.GetCode(),
						Error.GetDetail());
					return false;
				}
				const FAssetDurableSessionRef Persisted =
					MakeShared<const FAssetDurableSession>(
						Typed->Session->GetRequest(),
						Typed->Session->GetInventory(),
						Typed->Session->GetJournalId(),
						Typed->Session->GetJournalPath(),
						Typed->Session->GetJournalRoot(),
						Typed->Session->GetTransactionRoot(),
						Typed->Session->GetStagingRoot(),
						OutReceipt.GetPhaseHistory());
				{
					FScopeLock Lock(&Mutex);
					PersistedByJournal.Add(
						OutReceipt.GetJournalId(),
						Persisted);
				}
				for (const FAssetDurableTarget& Target :
					Typed->Session->GetRequest().GetTargets())
				{
					if (UPackage* Package = FindPackage(
						nullptr,
						*Target.GetPackageName()))
					{
						Package->SetDirtyFlag(false);
					}
				}
				return true;
			}

			bool Rollback(
				const FExecutionDurableSessionRef& Session,
				int64 FencingToken,
				bool bMemoryRollbackConfirmed,
				FUeAgentResourceTracker& Resources,
				FDurableReceipt& OutReceipt,
				FExecutionMutationError& OutError) override
			{
				const TSharedRef<FReferenceDurableSession>
					Typed = StaticCastSharedRef<
						FReferenceDurableSession>(Session);
				FAssetDurableError Error;
				if (!TryRollbackAssetDurability(
					Typed->Session,
					Backend,
					FencingToken,
					bMemoryRollbackConfirmed,
					Resources,
					OutReceipt,
					Error))
				{
					OutError = FExecutionMutationError(
						Error.GetCode(),
						Error.GetDetail());
					return false;
				}
				return true;
			}

			bool TryAcknowledge(
				const FDurableVerificationAcknowledgement&
					Acknowledgement,
				FUeAgentResourceTracker& Resources,
				FAssetDurableError& OutError) override
			{
				FAssetDurableSessionPtr Session;
				{
					FScopeLock Lock(&Mutex);
					Session = PersistedByJournal.FindRef(
						Acknowledgement.GetJournalId());
				}
				if (!Session.IsValid())
				{
					OutError = FAssetDurableError(
						TEXT(
							"reference_crud_durable_session_missing"),
						Acknowledgement.GetJournalId());
					return false;
				}
				if (!TryAcknowledgeAssetDurability(
					Session.ToSharedRef(),
					Acknowledgement,
					Acknowledgement.GetFencingToken(),
					Resources,
					OutError))
				{
					return false;
				}
				FScopeLock Lock(&Mutex);
				PersistedByJournal.Remove(
					Acknowledgement.GetJournalId());
				return true;
			}

			bool TryGetContext(
				const FString& JournalId,
				FDurableInventory& OutInventory,
				FString& OutOwnerId,
				int64& OutFencingToken) const
			{
				FScopeLock Lock(&Mutex);
				const FAssetDurableSessionPtr* Found =
					PersistedByJournal.Find(JournalId);
				if (!Found || !Found->IsValid())
				{
					return false;
				}
				OutInventory = (*Found)->GetInventory();
				OutOwnerId =
					(*Found)->GetRequest().GetJournalOwnerId();
				OutFencingToken =
					(*Found)->GetRequest().GetFencingToken();
				return true;
			}

		private:
			FString JournalRoot;
			FPackageAssetDurableBackend Backend;
			mutable FCriticalSection Mutex;
			TMap<FString, FAssetDurableSessionPtr>
				PersistedByJournal;
		};

		struct FReferenceExecutionRecord
		{
			FPlanningPreflightPlanPtr Plan;
			TArray<FUeAgentOperationTargetBinding> Targets;
		};

		class FReferenceExecutionStore final
		{
		public:
			void Put(
				const FString& OperationId,
				FReferenceExecutionRecord Record)
			{
				FScopeLock Lock(&Mutex);
				Records.Add(OperationId, MoveTemp(Record));
			}

			bool Get(
				const FString& OperationId,
				FReferenceExecutionRecord& OutRecord) const
			{
				FScopeLock Lock(&Mutex);
				const FReferenceExecutionRecord* Found =
					Records.Find(OperationId);
				if (!Found)
				{
					return false;
				}
				OutRecord = *Found;
				return true;
			}

			void Remove(const FString& OperationId)
			{
				FScopeLock Lock(&Mutex);
				Records.Remove(OperationId);
			}

		private:
			mutable FCriticalSection Mutex;
			TMap<FString, FReferenceExecutionRecord> Records;
		};

		class FReferenceExecutor final
			: public IUeAgentOperationExecutor
		{
		public:
			FReferenceExecutor(
				TSharedRef<
					FReferenceExecutionStore,
					ESPMode::ThreadSafe> InStore,
				TSharedRef<
					FReferenceDurablePort,
					ESPMode::ThreadSafe> InDurable,
				TSharedRef<
					FUeAgentReferenceAssetEffectHook,
					ESPMode::ThreadSafe> InEffect)
				: Store(MoveTemp(InStore))
				, Durable(MoveTemp(InDurable))
				, Effect(MoveTemp(InEffect))
				, Scheduler(
					MakeShared<
						FInlineGameThreadScheduler,
						ESPMode::ThreadSafe>())
				, LockFence(
					MakeShared<
						FMonotonicLockFenceProvider,
						ESPMode::ThreadSafe>())
				, Barrier(
					MakeShared<
						FUeAgentGenericEffectBarrierProvider,
						ESPMode::ThreadSafe>(Effect))
				, Coordinator(
					Scheduler,
					LockFence,
					Barrier,
					Durable,
					64)
			{
			}

			bool TryExecute(
				const FPlanningResult& Planning,
				const TArray<FUeAgentOperationTargetBinding>&
					Targets,
				const FUeAgentOperationContext& Context,
				FExecutionReceipt& OutReceipt,
				FUeAgentOperationBindingError& OutError)
				override
			{
				if (!IsInGameThread()
					|| !Planning.GetPlan().IsValid())
				{
					OutError.Code =
						TEXT(
							"reference_crud_executor_thread_invalid");
					return false;
				}
				const FPlanningPreflightPlanRef Plan =
					Planning.GetPlan().ToSharedRef();
				const FString OperationId =
					TEXT("reference-crud:")
					+ Plan->GetRequestId();
				TArray<FUeAgentGenericEffectBinding> Bindings;
				for (const FUeAgentOperationTargetBinding& Target :
					Targets)
				{
					Bindings.Emplace(
						Target.GetBinding(),
						GetUeStringPropertySourceTypeId(),
						FUeAgentCanonicalType(
							TEXT("core.string"),
							EUeAgentValueKind::String,
							false,
							TEXT("scalar")),
						TEXT(
							"ue-reflection:UBlueprint.BlueprintDisplayName"));
				}
				const TSharedRef<
					FUeAgentGenericEffectMutationHook,
					ESPMode::ThreadSafe> Hook =
					MakeShared<
						FUeAgentGenericEffectMutationHook,
						ESPMode::ThreadSafe>(
						MoveTemp(Bindings),
						Effect);
				const FExecutionMutationRequest Request(
					OperationId,
					TEXT("reference-crud-owner"),
					Plan,
					Context.GetExecutionContextOwner(),
					Context.GetResourceTrackerOwner(),
					Context.GetDeadline(),
					Context.GetCancellationToken(),
					EExecutionThreadAffinity::GameThread,
					EExecutionReentrancyPolicy::
						RejectSameTarget);
				Store->Put(
					OperationId,
					{Planning.GetPlan(), Targets});
				FString SubmittedId;
				FExecutionMutationError Error;
				if (!Coordinator.Submit(
					Request,
					Hook,
					SubmittedId,
					Error))
				{
					Store->Remove(OperationId);
					OutError.Code = Error.GetCode();
					OutError.Detail = Error.GetDetail();
					return false;
				}
				Coordinator.DispatchReady();
				if (!Coordinator.GetReceipt(
					SubmittedId,
					OutReceipt,
					Error))
				{
					Store->Remove(OperationId);
					OutError.Code = Error.GetCode();
					OutError.Detail = Error.GetDetail();
					return false;
				}
				return true;
			}

		private:
			TSharedRef<
				FReferenceExecutionStore,
				ESPMode::ThreadSafe> Store;
			TSharedRef<
				FReferenceDurablePort,
				ESPMode::ThreadSafe> Durable;
			TSharedRef<
				FUeAgentReferenceAssetEffectHook,
				ESPMode::ThreadSafe> Effect;
			TSharedRef<
				FInlineGameThreadScheduler,
				ESPMode::ThreadSafe> Scheduler;
			TSharedRef<
				FMonotonicLockFenceProvider,
				ESPMode::ThreadSafe> LockFence;
			TSharedRef<
				FUeAgentGenericEffectBarrierProvider,
				ESPMode::ThreadSafe> Barrier;
			FExecutionMutationCoordinator Coordinator;
		};

		class FReferenceAbsenceOracle final
			: public IAbsenceOracle
		{
		public:
			bool TryObserve(
				const FUeAgentCanonicalIdentity& Identity,
				const TArray<FDurableArtifact>& Artifacts,
				FUeAgentResourceTracker& Resources,
				const FUeAgentOperationDeadline& Deadline,
				const FUeAgentCancellationToken& Cancellation,
				FAbsenceObservation& OutObservation,
				FVerificationError& OutError) override
			{
				bool bAbsent = !Artifacts.IsEmpty();
				for (const FDurableArtifact& Artifact : Artifacts)
				{
					bAbsent = bAbsent
						&& !IFileManager::Get().FileExists(
							*Artifact.GetFinalPath());
				}
				OutObservation = FAbsenceObservation(
					Identity,
					bAbsent,
					false,
					TEXT("reference-asset-absence:")
						+ FGuid::NewGuid().ToString(
							EGuidFormats::Digits));
				return true;
			}
		};

		class FReferenceReferenceOracle final
			: public IReferenceOracle
		{
		public:
			bool TryObserve(
				const FUeAgentCanonicalIdentity& Identity,
				FUeAgentResourceTracker& Resources,
				const FUeAgentOperationDeadline& Deadline,
				const FUeAgentCancellationToken& Cancellation,
				FReferenceObservation& OutObservation,
				FVerificationError& OutError) override
			{
				OutObservation = FReferenceObservation(
					Identity,
					{},
					false,
					TEXT("reference-asset-references:")
						+ FGuid::NewGuid().ToString(
							EGuidFormats::Digits));
				return true;
			}
		};

		bool BuildExpectedSnapshot(
			const FPlannedChange& Change,
			const FPlanningPreflightPlan& Plan,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalSnapshotPtr& OutSnapshot,
			FString& OutError)
		{
			FUeAgentCanonicalValuePtr DefaultValue;
			FUeAgentStateSemanticError StateError;
			if (!FUeAgentCanonicalValue::TryString(
				FString(),
				Resources,
				DefaultValue,
				StateError))
			{
				OutError = StateError.Code;
				return false;
			}
			TArray<FUeAgentStateEntry> Entries;
			if (Change.GetAfter().IsValid())
			{
				Entries.Emplace(
					Change.GetIdentity(),
					FUeAgentCanonicalType(
						TEXT("core.string"),
						EUeAgentValueKind::String,
						false,
						TEXT("scalar")),
					Change.GetAfter().ToSharedRef(),
					FUeAgentDefaultValue(
						EUeAgentDefaultSourceKind::
							ClassDefaultObject,
						TEXT("/Script/Engine.Blueprint"),
						TEXT(
							"UBlueprint.BlueprintDisplayName"),
						DefaultValue.ToSharedRef()),
					FUeAgentStateDetails(
						TEXT("Blueprint Display Name"),
						TEXT(
							"Controlled reference CRUD string property"),
						{TEXT("UAI"), TEXT("ReferenceCrud")},
						true,
						TEXT("available"),
						{{
							TEXT("allowed_root"),
							GetReferenceAssetAllowedRoot(),
						}}),
					Plan.GetObservedRevision());
			}
			if (!FUeAgentCanonicalSnapshot::TryCreateExpected(
				TEXT("reference-crud-expected:")
					+ Plan.GetRequestId(),
				EUeAgentStatePlane::Persisted,
				Plan.GetObservedRevision(),
				FUeAgentTemporalPoint(
					TEXT("uai-operation"),
					FString::Printf(
						TEXT("%lld"),
						Plan.GetObservedRevision()
							.GetSequence())),
				FUeAgentStateProvenance(
					TEXT("operation_expected_state"),
					Plan.GetRequestId(),
					TEXT("reference-crud-expected:")
						+ Plan.GetRequestId()),
				Entries,
				Resources,
				OutSnapshot,
				StateError))
			{
				OutError = StateError.Code;
				return false;
			}
			return true;
		}

		class FReferenceVerifier final
			: public IUeAgentOperationVerifier
		{
		public:
			FReferenceVerifier(
				TSharedRef<
					FReferenceExecutionStore,
					ESPMode::ThreadSafe> InStore,
				TSharedRef<
					FReferenceDurablePort,
					ESPMode::ThreadSafe> InDurable,
				TSharedRef<IUeAgentStateReadPort> InReadPort,
				TSharedRef<
					FUeAgentReferenceAssetEffectHook,
					ESPMode::ThreadSafe> InEffect)
				: Store(MoveTemp(InStore))
				, Durable(MoveTemp(InDurable))
				, ReadPort(MoveTemp(InReadPort))
				, Effect(MoveTemp(InEffect))
			{
			}

			bool TryVerify(
				const FExecutionReceipt& Receipt,
				const TArray<FUeAgentOperationTargetBinding>&
					Targets,
				const FUeAgentOperationContext& Context,
				FVerificationOutcome& OutOutcome,
				FUeAgentOperationBindingError& OutError)
				override
			{
				FUeAgentResourceTracker& Resources =
					Context.GetResourceTracker();
				const FUeAgentOperationDeadline& Deadline =
					Context.GetDeadline();
				const FUeAgentCancellationToken& Cancellation =
					Context.GetCancellationToken();
				const FDurableReceipt* DurableReceipt =
					Receipt.GetDurableReceipt();
				FReferenceExecutionRecord Record;
				if (!DurableReceipt)
				{
					OutError.Code =
						Receipt.GetErrorCode().IsEmpty()
							? TEXT(
								"reference_crud_execution_not_durable")
							: Receipt.GetErrorCode();
					OutError.SubjectId =
						Receipt.GetOperationId();
					OutError.Detail =
						TEXT(
							"execution terminated before C8D produced a durable receipt");
					return false;
				}
				if (!Store->Get(
					Receipt.GetOperationId(),
					Record))
				{
					OutError.Code =
						TEXT(
							"reference_crud_verification_context_missing");
					OutError.SubjectId =
						Receipt.GetOperationId();
					return false;
				}
				FDurableInventory Inventory;
				FString OwnerId;
				int64 FencingToken = -1;
				if (!Durable->TryGetContext(
					DurableReceipt->GetJournalId(),
					Inventory,
					OwnerId,
					FencingToken))
				{
					OutError.Code =
						Receipt.GetErrorCode().IsEmpty()
							? TEXT(
								"reference_crud_durable_context_missing")
							: Receipt.GetErrorCode();
					OutError.SubjectId =
						Receipt.GetOperationId();
					OutError.Detail =
						DurableReceipt->GetJournalId();
					return false;
				}
				TArray<FVerificationTarget> VerificationTargets;
				for (const FPlannedChange& Change :
					Record.Plan->GetChangeSet().GetChanges())
				{
					const FUeAgentOperationTargetBinding*
						Target = nullptr;
					for (const FUeAgentOperationTargetBinding&
						Candidate : Targets)
					{
						if (Candidate.GetBinding()
								.GetSummary()
								.GetIdentity()
							== Change.GetIdentity())
						{
							Target = &Candidate;
							break;
						}
					}
					if (!Target)
					{
						OutError.Code =
							TEXT(
								"reference_crud_verification_target_missing");
						return false;
					}
					TArray<FString> ArtifactIds;
					for (const FDurableArtifact& Artifact :
						Inventory.GetArtifacts())
					{
						if (Artifact.GetTargetIdentity()
							== Change.GetIdentity())
						{
							ArtifactIds.Add(
								Artifact.GetArtifactId());
						}
					}
					FUeAgentCanonicalSnapshotPtr Expected;
					if (Change.GetIntent()
							!= EChangeIntent::Delete
						&& !BuildExpectedSnapshot(
							Change,
						*Record.Plan,
							Resources,
							Expected,
							OutError.Detail))
					{
						OutError.Code =
							OutError.Detail;
						return false;
					}
					VerificationTargets.Emplace(
						Change.GetIdentity(),
						Target->GetBinding(),
						Change.GetIntent()
								== EChangeIntent::Delete
							? EVerificationOracleKind::Absence
							: EVerificationOracleKind::Persisted,
						Expected,
						MoveTemp(ArtifactIds),
						TArray<FString>());
				}
				FStateReadPortPersistedStateOracle PersistedOracle(
					*ReadPort);
				FVerificationService Service(
					PersistedOracle,
					Absence,
					Reference,
					*Durable);
				FVerificationError Error;
				const bool bVerified = Service.TryVerify(
					FVerificationRequest(
						Receipt.GetOperationId(),
						Record.Plan
							->GetDescriptorIdentity(),
						*DurableReceipt,
						Inventory,
						OwnerId,
						FencingToken,
						MoveTemp(VerificationTargets)),
					Resources,
					Deadline,
					Cancellation,
					OutOutcome,
					Error);
				Store->Remove(Receipt.GetOperationId());
				if (!bVerified)
				{
					OutError.Code = Error.GetCode();
					OutError.Detail = Error.GetDetail();
					return false;
				}
				Effect->ReleaseVerifiedOperation(
					Receipt.GetOperationId());
				return true;
			}

		private:
			TSharedRef<
				FReferenceExecutionStore,
				ESPMode::ThreadSafe> Store;
			TSharedRef<
				FReferenceDurablePort,
				ESPMode::ThreadSafe> Durable;
			TSharedRef<IUeAgentStateReadPort> ReadPort;
			TSharedRef<
				FUeAgentReferenceAssetEffectHook,
				ESPMode::ThreadSafe> Effect;
			FReferenceAbsenceOracle Absence;
			FReferenceReferenceOracle Reference;
		};

		class FReferenceTokenEstimator final
			: public IUeAgentTokenEstimator
		{
		public:
			bool TryEstimate(
				const FUeAgentProjectedPayload& Payload,
				const TArray<FUeAgentArtifactDescriptor>& Artifacts,
				const TArray<FUeAgentProjectionDiagnostic>&
					Diagnostics,
				FUeAgentResourceTracker& Resources,
				const FUeAgentOperationDeadline& Deadline,
				const FUeAgentCancellationToken& Cancellation,
				FUeAgentTokenEstimate& OutEstimate,
				FUeAgentProjectionError& OutError) override
			{
				OutEstimate = FUeAgentTokenEstimate(
					EUeAgentTokenEstimateKind::Conservative,
					2048,
					6144);
				return true;
			}
		};

		struct FDecodedOperation
		{
			TOptional<FUeAgentOperationSessionIdentity> Session;
			TOptional<FUeAgentOperationTargetBinding> Target;
			EChangeIntent Intent = EChangeIntent::Update;
			FUeAgentCanonicalValuePtr ProposedAfter;
			TOptional<FUeAgentProjectionRequest> Projection;
		};

		bool DecodeOperation(
			const FUeAgentOperationRequest& Request,
			FUeAgentResourceTracker& Resources,
			FDecodedOperation& OutOperation,
			FString& OutCode,
			FString& OutDetail)
		{
			TSharedPtr<FJsonObject> Root;
			const TSharedRef<TJsonReader<>> Reader =
				TJsonReaderFactory<>::Create(
					Request.GetPayload().GetJsonValue());
			if (!FJsonSerializer::Deserialize(Reader, Root)
				|| !Root.IsValid())
			{
				OutCode =
					TEXT("reference_crud_payload_invalid");
				return false;
			}
			const TSharedPtr<FJsonObject>* SessionObject = nullptr;
			const TSharedPtr<FJsonObject>* TargetObject = nullptr;
			const TSharedPtr<FJsonObject>* ProjectionObject = nullptr;
			FString IntentText;
			if (!Root->TryGetObjectField(
					TEXT("session"),
					SessionObject)
				|| !Root->TryGetObjectField(
					TEXT("target"),
					TargetObject)
				|| !Root->TryGetObjectField(
					TEXT("projection"),
					ProjectionObject)
				|| !Root->TryGetStringField(
					TEXT("intent"),
					IntentText)
				|| !TargetObject
				|| !TargetObject->IsValid())
			{
				OutCode =
					TEXT("reference_crud_payload_fields_missing");
				return false;
			}
			TOptional<FUeAgentOperationSessionIdentity> Session;
			TOptional<FUeAgentOperationTicket> Ticket;
			TOptional<FUeAgentCandidateBinding> Binding;
			FUeAgentBindingTicketJsonError CodecError;
			const TSharedPtr<FJsonObject>* TicketObject = nullptr;
			const TSharedPtr<FJsonObject>* BindingObject = nullptr;
			if (!FUeAgentBindingTicketJsonCodec::TryParseSession(
					*SessionObject,
					Session,
					CodecError)
				|| !(*TargetObject)->TryGetObjectField(
					TEXT("ticket"),
					TicketObject)
				|| !(*TargetObject)->TryGetObjectField(
					TEXT("binding"),
					BindingObject)
				|| !TicketObject
				|| !BindingObject
				|| !FUeAgentBindingTicketJsonCodec::TryParseTicket(
					*TicketObject,
					Ticket,
					CodecError)
				|| !FUeAgentBindingTicketJsonCodec::TryParseBinding(
					*BindingObject,
					Binding,
					CodecError)
				|| !Session.IsSet()
				|| !Ticket.IsSet()
				|| !Binding.IsSet()
				|| !ParseIntent(IntentText, OutOperation.Intent)
				|| !ParseProjection(
					ProjectionObject
						? *ProjectionObject
						: nullptr,
					OutOperation.Projection))
			{
				OutCode = CodecError.Code.IsEmpty()
					? TEXT("reference_crud_payload_invalid")
					: CodecError.Code;
				OutDetail = CodecError.Detail;
				return false;
			}
			const TSharedPtr<FJsonValue> Proposed =
				Root->TryGetField(TEXT("proposed_after"));
			if (!ParseProposedValue(
				Proposed,
				OutOperation.Intent,
				Resources,
				OutOperation.ProposedAfter,
				OutCode))
			{
				return false;
			}
			OutOperation.Session.Emplace(Session.GetValue());
			OutOperation.Target.Emplace(
					Ticket.GetValue(),
					Binding.GetValue());
			return OutOperation.Target.IsSet()
				&& OutOperation.Target->IsValid();
		}
	}

	class FUeAgentReferenceCrudApplication::FState final
	{
	public:
		FUeAgentReferenceCrudApplicationConfig Config;
		TSharedRef<const FUeAgentOperationTicketService>
			TicketService;
		TSharedRef<FUeAgentReferenceAssetCatalogSource>
			CatalogSource;
		TSharedRef<IUeAgentStateReadPort> ReadPort;
		TSharedRef<const FUeAgentOperationBinding> Binding;

		FState(
			FUeAgentReferenceCrudApplicationConfig InConfig,
			TSharedRef<const FUeAgentOperationTicketService>
				InTicketService,
			TSharedRef<FUeAgentReferenceAssetCatalogSource>
				InCatalogSource,
			TSharedRef<IUeAgentStateReadPort> InReadPort,
			TSharedRef<const FUeAgentOperationBinding> InBinding)
			: Config(MoveTemp(InConfig))
			, TicketService(MoveTemp(InTicketService))
			, CatalogSource(MoveTemp(InCatalogSource))
			, ReadPort(MoveTemp(InReadPort))
			, Binding(MoveTemp(InBinding))
		{
		}
	};

	bool FUeAgentReferenceCrudCommand::IsValid() const
	{
		const bool bIntentValid =
			Intent == TEXT("create")
			|| Intent == TEXT("update")
			|| Intent == TEXT("delete");
		const bool bProposedValid =
			Intent == TEXT("delete")
			|| ProposedText.IsSet();
		return !RequestId.IsEmpty()
			&& !IdempotencyKey.IsEmpty()
			&& !ObjectPath.IsEmpty()
			&& bIntentValid
			&& bProposedValid;
	}

	FUeAgentReferenceCrudApplication::
		FUeAgentReferenceCrudApplication(
			TSharedRef<FState, ESPMode::ThreadSafe> InState)
		: State(MoveTemp(InState))
	{
	}

	FUeAgentReferenceCrudApplication::
		~FUeAgentReferenceCrudApplication() = default;

	TSharedRef<const FUeAgentOperationBinding>
	FUeAgentReferenceCrudApplication::GetOperationBinding() const
	{
		return State->Binding;
	}

	bool FUeAgentReferenceCrudApplication::TryCreate(
		const FUeAgentReferenceCrudApplicationConfig& Config,
		TSharedRef<const FUeAgentOperationTicketService>
			TicketService,
		TSharedRef<IUeAgentStateReadPort> ReadPort,
		TSharedPtr<FUeAgentReferenceCrudApplication>&
			OutApplication,
		FUeAgentReferenceCrudApplicationError& OutError)
	{
		OutApplication.Reset();
		OutError = {};
		const FUeAgentOperationDescriptor* OperationDescriptor =
			Config.OperationDescriptor.IsSet()
				&& Config.OperationDescriptorRegistry.IsValid()
				? Config.OperationDescriptorRegistry->FindExact(
					Config.OperationDescriptor.GetValue())
				: nullptr;
		if (!Config.OperationDescriptor.IsSet()
			|| !Config.OperationDescriptor->IsValid()
			|| !Config.OperationDescriptorRegistry.IsValid()
			|| !OperationDescriptor
			|| OperationDescriptor->GetOperationKind()
				!= EUeAgentDescriptorOperationKind::Operation
			|| Config.ServerSessionId.IsEmpty()
			|| Config.RegistryEpoch < 1
			|| Config.TimeoutSeconds <= 0.0
			|| Config.MaxFrameBytes <= 0
			|| Config.JournalRoot.IsEmpty()
			|| ReadPort->GetPortId()
				!= GetReferenceAssetReadPortId())
		{
			SetError(
				OutError,
				TEXT("reference_crud_application_config_invalid"),
				TEXT(
					"reference CRUD application configuration is incomplete"));
			return false;
		}
		FUeAgentExecutionContextRefPtr ExpectedContext;
		FUeAgentExecutionContextError ContextError;
		if (!FUeAgentExecutionContextRef::TryCreate(
				Config.ExecutionContext,
				OperationDescriptor->GetIdentity(),
				EUeAgentRequestKind::Operation,
				OperationDescriptor
					->GetExecutionContextPolicy(),
				ExpectedContext,
				ContextError))
		{
			SetError(
				OutError,
				ContextError.Code,
				TEXT(
					"reference CRUD execution context is invalid"));
			return false;
		}
		const TSharedRef<
			FReferenceExecutionStore,
			ESPMode::ThreadSafe> Store =
			MakeShared<
				FReferenceExecutionStore,
				ESPMode::ThreadSafe>();
		const TSharedRef<
			FReferenceDurablePort,
			ESPMode::ThreadSafe> Durable =
			MakeShared<
				FReferenceDurablePort,
				ESPMode::ThreadSafe>(
				Config.JournalRoot);
		const TSharedRef<
			FUeAgentReferenceAssetEffectHook,
			ESPMode::ThreadSafe> Effect =
			MakeShared<
				FUeAgentReferenceAssetEffectHook,
				ESPMode::ThreadSafe>();
		const TSharedRef<FReferencePlanner> Planner =
			MakeShared<FReferencePlanner>();
		const TSharedRef<FReferenceExecutor> Executor =
			MakeShared<FReferenceExecutor>(
				Store,
				Durable,
				Effect);
		const TSharedRef<FReferenceVerifier> Verifier =
			MakeShared<FReferenceVerifier>(
				Store,
				Durable,
				ReadPort,
				Effect);
		const TSharedRef<FReferenceTokenEstimator> Estimator =
			MakeShared<FReferenceTokenEstimator>();
		TSharedPtr<const FUeAgentResponseProjector> Projector;
		FUeAgentProjectionError ProjectionError;
		if (!FUeAgentResponseProjector::TryCreate(
			Estimator,
			Projector,
			ProjectionError))
		{
			SetError(
				OutError,
				ProjectionError.Code,
				ProjectionError.Detail);
			return false;
		}
		const TSharedRef<FReferenceSessionSource> SessionSource =
			MakeShared<FReferenceSessionSource>(
				FUeAgentOperationSessionIdentity(
					Config.ServerSessionId,
					Config.RegistryEpoch,
					Config.OperationDescriptorRegistry
						->GetRegistryHash()));
		const TSharedRef<
			FUeAgentExactExecutionContextValidator>
			ContextValidator =
				MakeShared<
					FUeAgentExactExecutionContextValidator>(
					ExpectedContext.ToSharedRef());
		TSharedPtr<const FUeAgentOperationBinding> Binding;
		FUeAgentOperationBindingError BindingError;
		if (!FUeAgentOperationBinding::TryCreate(
			Config.OperationDescriptorRegistry.ToSharedRef(),
			Config.OperationDescriptor.GetValue(),
			ContextValidator,
			Planner,
			Executor,
			Verifier,
			Projector.ToSharedRef(),
			SessionSource,
			TicketService,
			Binding,
			BindingError))
		{
			SetError(
				OutError,
				BindingError.Code,
				BindingError.Detail);
			return false;
		}
		const TSharedRef<FState, ESPMode::ThreadSafe> NewState =
			MakeShared<FState, ESPMode::ThreadSafe>(
				Config,
				TicketService,
				MakeShared<FUeAgentReferenceAssetCatalogSource>(
					Config.OperationDescriptor.GetValue(),
					Config.ServerSessionId),
				ReadPort,
				Binding.ToSharedRef());
		OutApplication =
			MakeShareable(
				new FUeAgentReferenceCrudApplication(NewState));
		return true;
	}

	FUeAgentOperationResult
	FUeAgentReferenceCrudApplication::Submit(
		const FUeAgentOperationRequest& Request,
		const FUeAgentCallerContext& CallerContext,
		const TSharedRef<IUeAgentCancellationSignal>&
			Cancellation)
	{
		if (!IsInGameThread())
		{
			const TSharedRef<TOptional<FUeAgentOperationResult>>
				Result =
					MakeShared<TOptional<FUeAgentOperationResult>>();
			FEvent* Completed =
				FPlatformProcess::GetSynchEventFromPool(true);
			const TSharedRef<FState, ESPMode::ThreadSafe> StateRef =
				State;
			AsyncTask(
				ENamedThreads::GameThread,
				[StateRef,
				 Request,
				 CallerContext,
				 Cancellation,
				 Result,
				 Completed]()
				{
					FUeAgentReferenceCrudApplication Application(
						StateRef);
					Result->Emplace(
						Application.Submit(
							Request,
							CallerContext,
							Cancellation));
					Completed->Trigger();
				});
			Completed->Wait();
			FPlatformProcess::ReturnSynchEventToPool(Completed);
			return Result->GetValue();
		}

		const TSharedRef<
			FUeAgentResourceTracker,
			ESPMode::ThreadSafe> Resources =
			MakeShared<
				FUeAgentResourceTracker,
				ESPMode::ThreadSafe>(
				OperationBudget(
					State->Config.MaxFrameBytes));
		const auto Clock = []()
		{
			return FPlatformTime::Seconds();
		};
		FUeAgentCancellationSource CancellationSource(Clock);
		if (Cancellation->IsCancelled())
		{
			CancellationSource.Request(
				TEXT("protocol_cancellation_requested"));
		}
		const FUeAgentOperationDeadline Deadline =
			FUeAgentOperationDeadline::FromTimeout(
				State->Config.TimeoutSeconds,
				Clock);
		return SubmitWithExecutionContext(
			Request,
			CallerContext,
			Resources,
			Deadline,
			CancellationSource.GetToken());
	}

	FUeAgentOperationResult
	FUeAgentReferenceCrudApplication::SubmitCommand(
		const FUeAgentReferenceCrudCommand& Command,
		const FUeAgentCallerContext& CallerContext,
		const TSharedRef<IUeAgentCancellationSignal>&
			Cancellation)
	{
		if (!IsInGameThread())
		{
			const TSharedRef<TOptional<FUeAgentOperationResult>>
				Result =
					MakeShared<TOptional<FUeAgentOperationResult>>();
			FEvent* Completed =
				FPlatformProcess::GetSynchEventFromPool(true);
			const TSharedRef<FState, ESPMode::ThreadSafe> StateRef =
				State;
			AsyncTask(
				ENamedThreads::GameThread,
				[StateRef,
				 Command,
				 CallerContext,
				 Cancellation,
				 Result,
				 Completed]()
				{
					FUeAgentReferenceCrudApplication Application(
						StateRef);
					Result->Emplace(
						Application.SubmitCommand(
							Command,
							CallerContext,
							Cancellation));
					Completed->Trigger();
				});
			Completed->Wait();
			FPlatformProcess::ReturnSynchEventToPool(Completed);
			return Result->GetValue();
		}

		const FString ResultRequestId =
			Command.RequestId.IsEmpty()
				? TEXT("invalid-reference-command")
				: Command.RequestId;
		const FString ResultIdempotencyKey =
			Command.IdempotencyKey.IsEmpty()
				? ResultRequestId + TEXT("-idempotency")
				: Command.IdempotencyKey;
		const FUeAgentOperationRequest ResultShell(
			State->Config.OperationDescriptor.GetValue(),
			ResultRequestId,
			ResultIdempotencyKey,
			FUeAgentContractValue(TEXT("{}")));
		if (!Command.IsValid())
		{
			return ErrorResult(
				ResultShell,
				TEXT("reference_crud_command_invalid"),
				Command.ObjectPath,
				TEXT(
					"reference command parameters are incomplete"));
		}
		if (!CallerContext.GetAuthorityScopes().Contains(
			RequiredWriteScope))
		{
			return ErrorResult(
				ResultShell,
				TEXT("authority_scope_missing"),
				RequiredWriteScope,
				TEXT(
					"reference CRUD requires uai:reference:write"));
		}

		const TSharedRef<
			FUeAgentResourceTracker,
			ESPMode::ThreadSafe> Resources =
			MakeShared<
				FUeAgentResourceTracker,
				ESPMode::ThreadSafe>(
				OperationBudget(
					State->Config.MaxFrameBytes));
		const auto Clock = []()
		{
			return FPlatformTime::Seconds();
		};
		FUeAgentCancellationSource CancellationSource(Clock);
		if (Cancellation->IsCancelled())
		{
			CancellationSource.Request(
				TEXT("protocol_cancellation_requested"));
		}
		const FUeAgentOperationDeadline Deadline =
			FUeAgentOperationDeadline::FromTimeout(
				State->Config.TimeoutSeconds,
				Clock);

		TSharedPtr<const FUeAgentCatalogSnapshot> Catalog;
		FUeAgentDiscoveryError DiscoveryError;
		if (!State->CatalogSource->TrySearch(
				FUeAgentDiscoveryQuery(
					GetReferenceAssetCatalogKind(),
					Command.ObjectPath,
					EUeAgentDiscoveryMatchMode::Exact,
					EUeAgentMaterializationPolicy::IndexOnly),
				*Resources,
				Deadline,
				CancellationSource.GetToken(),
				Catalog,
				DiscoveryError)
			|| !Catalog.IsValid()
			|| Catalog->GetCandidates().Num() != 1)
		{
			return ErrorResult(
				ResultShell,
				DiscoveryError.Code.IsEmpty()
					? TEXT(
						"reference_crud_candidate_unavailable")
					: DiscoveryError.Code,
				Command.ObjectPath,
				DiscoveryError.Detail);
		}

		TSharedPtr<const FUeAgentCandidateBinding> Binding;
		if (!State->CatalogSource->TryResolveExact(
				Catalog->GetCandidates()[0]
					.GetIdentity()
					.GetCanonicalKey(),
				Catalog->GetRevision(),
				*Resources,
				Deadline,
				CancellationSource.GetToken(),
				Binding,
				DiscoveryError)
			|| !Binding.IsValid())
		{
			return ErrorResult(
				ResultShell,
				DiscoveryError.Code.IsEmpty()
					? TEXT(
						"reference_crud_binding_unavailable")
					: DiscoveryError.Code,
				Command.ObjectPath,
				DiscoveryError.Detail);
		}

		TSharedPtr<const FUeAgentCanonicalSnapshot> Snapshot;
		if (!State->ReadPort->TryRead(
				FUeAgentStateReadRequest(
					*Binding,
					EUeAgentStatePlane::Persisted),
				*Resources,
				Deadline,
				CancellationSource.GetToken(),
				Snapshot,
				DiscoveryError)
			|| !Snapshot.IsValid())
		{
			return ErrorResult(
				ResultShell,
				DiscoveryError.Code.IsEmpty()
					? TEXT(
						"reference_crud_prestate_unavailable")
					: DiscoveryError.Code,
				Command.ObjectPath,
				DiscoveryError.Detail);
		}

		const FUeAgentOperationSessionIdentity Session(
			State->Config.ServerSessionId,
			State->Config.RegistryEpoch,
			State->Config.OperationDescriptorRegistry
				->GetRegistryHash());
		TOptional<FUeAgentOperationTicket> Ticket;
		FUeAgentOperationTicketError TicketError;
		if (!State->TicketService->TryIssue(
				*Binding,
				State->Config.OperationDescriptor.GetValue(),
				Session,
				CallerContext,
				Snapshot->GetRevision(),
				Ticket,
				TicketError)
			|| !Ticket.IsSet())
		{
			return ErrorResult(
				ResultShell,
				TicketError.Code.IsEmpty()
					? TEXT("reference_crud_ticket_issue_failed")
					: TicketError.Code,
				Command.ObjectPath,
				TicketError.Detail);
		}

		TSharedRef<FJsonObject> Target = MakeShared<FJsonObject>();
		Target->SetObjectField(
			TEXT("ticket"),
			FUeAgentBindingTicketJsonCodec::TicketToJson(
				Ticket.GetValue()));
		Target->SetObjectField(
			TEXT("binding"),
			FUeAgentBindingTicketJsonCodec::BindingToJson(
				*Binding));
		TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetObjectField(
			TEXT("session"),
			FUeAgentBindingTicketJsonCodec::SessionToJson(
				Session));
		Root->SetObjectField(TEXT("target"), Target);
		Root->SetStringField(TEXT("intent"), Command.Intent);
		if (Command.Intent == TEXT("delete"))
		{
			Root->SetField(
				TEXT("proposed_after"),
				MakeShared<FJsonValueNull>());
		}
		else
		{
			TSharedRef<FJsonObject> Proposed =
				MakeShared<FJsonObject>();
			Proposed->SetStringField(TEXT("kind"), TEXT("string"));
			Proposed->SetStringField(
				TEXT("text"),
				Command.ProposedText.GetValue());
			Root->SetObjectField(
				TEXT("proposed_after"),
				Proposed);
		}
		Root->SetObjectField(
			TEXT("projection"),
			MakeShared<FJsonObject>());
		const FUeAgentOperationRequest Request(
			State->Config.OperationDescriptor.GetValue(),
			Command.RequestId,
			Command.IdempotencyKey,
			FUeAgentContractValue(JsonText(Root)));
		return SubmitWithExecutionContext(
			Request,
			CallerContext,
			Resources,
			Deadline,
			CancellationSource.GetToken());
	}

	FUeAgentOperationResult
	FUeAgentReferenceCrudApplication::
		SubmitWithExecutionContext(
			const FUeAgentOperationRequest& Request,
			const FUeAgentCallerContext& CallerContext,
			const TSharedRef<
				FUeAgentResourceTracker,
				ESPMode::ThreadSafe>& Resources,
			const FUeAgentOperationDeadline& Deadline,
			const FUeAgentCancellationToken& Cancellation)
	{
		FDecodedOperation Decoded;
		FString Code;
		FString Detail;
		if (!DecodeOperation(
			Request,
			*Resources,
			Decoded,
			Code,
			Detail))
		{
			return ErrorResult(
				Request,
				Code,
				Request.GetRequestId(),
				Detail);
		}
		if (!CallerContext.GetAuthorityScopes().Contains(
			RequiredWriteScope))
		{
			return ErrorResult(
				Request,
				TEXT("authority_scope_missing"),
				RequiredWriteScope,
				TEXT(
					"reference CRUD requires uai:reference:write"));
		}
		TSharedPtr<const FUeAgentCanonicalSnapshot> Snapshot;
		FUeAgentDiscoveryError ReadError;
		if (!State->ReadPort->TryRead(
			FUeAgentStateReadRequest(
				Decoded.Target->GetBinding(),
				EUeAgentStatePlane::Persisted),
			*Resources,
			Deadline,
			Cancellation,
			Snapshot,
			ReadError)
			|| !Snapshot.IsValid())
		{
			return ErrorResult(
				Request,
				ReadError.Code.IsEmpty()
					? TEXT(
						"reference_crud_prestate_unavailable")
					: ReadError.Code,
				Decoded.Target->GetBinding().GetBindingId(),
				ReadError.Detail);
		}
		const FString IdentityKey =
			Decoded.Target->GetBinding()
				.GetSummary()
				.GetIdentity()
				.GetCanonicalKey();
		FUeAgentCanonicalValuePtr Before;
		for (const FUeAgentStateEntry& Entry :
			Snapshot->GetEntries())
		{
			if (Entry.GetIdentity().GetCanonicalKey()
				== IdentityKey)
			{
				Before = Entry.GetValue();
				break;
			}
		}
		const bool bExists = Before.IsValid();
		if ((Decoded.Intent == EChangeIntent::Create && bExists)
			|| (Decoded.Intent != EChangeIntent::Create
				&& !bExists))
		{
			return ErrorResult(
				Request,
				TEXT(
					"reference_crud_existence_precondition_failed"),
				IdentityKey,
				bExists
					? TEXT("target already exists")
					: TEXT("target does not exist"));
		}
		const FChangeDraft Change(
			Decoded.Target->GetBinding()
				.GetSummary()
				.GetIdentity(),
			Decoded.Intent,
			Before,
			Decoded.ProposedAfter,
			{RequiredWriteScope},
			TEXT("reference-crud-direct-impact:")
				+ Request.GetRequestId(),
			{Decoded.Target->GetBinding()
				.GetSummary()
				.GetIdentity()});
		const FChangeSetDraft Draft(
			Request.GetDescriptorIdentity(),
			Request.GetRequestId(),
			Request.GetIdempotencyKey(),
			Snapshot.ToSharedRef(),
			{Change});
		const FPlanningRequest Planning(
			Draft,
			CallerContext,
			false);
		const FUeAgentOperationBindingPayload Payload(
			Decoded.Session.GetValue(),
			{Decoded.Target.GetValue()},
			State->Config.ExecutionContext,
			Planning,
			Decoded.Projection.GetValue());
		const FUeAgentOperationBindingRequest TypedRequest(
			Request.GetDescriptorIdentity(),
			Request.GetRequestId(),
			Request.GetIdempotencyKey(),
			Payload);
		TSharedPtr<const FUeAgentOperationBindingResult> Result;
		FUeAgentOperationBindingError Error;
		if (!State->Binding->TrySubmit(
			TypedRequest,
			CallerContext,
			Resources,
			Deadline,
			Cancellation,
			Result,
			Error)
			|| !Result.IsValid())
		{
			return ErrorResult(
				Request,
				Error.Code.IsEmpty()
					? TEXT("reference_crud_failed")
					: Error.Code,
				Error.SubjectId,
				Error.Detail);
		}
		const TSharedPtr<
			const FUeAgentProjectedResponse>& Projection =
				Result->GetValue().GetProjection();
		if (!Projection.IsValid()
			|| !Projection->GetPayload().GetValue().IsValid())
		{
			return ErrorResult(
				Request,
				TEXT(
					"reference_crud_projection_missing"),
				Request.GetRequestId(),
				TEXT("C3O returned no canonical C9 outcome"));
		}
		TSharedRef<FJsonObject> ProjectionJson =
			MakeShared<FJsonObject>();
		ProjectionJson->SetStringField(
			TEXT("profile_id"),
			Projection->GetProfileId());
		ProjectionJson->SetField(
			TEXT("payload"),
			CanonicalValueToJson(
				*Projection->GetPayload().GetValue()));
		TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(
			TEXT("operation_id"),
			Result->GetValue().GetOperationId());
		Root->SetObjectField(
			TEXT("projection"),
			ProjectionJson);
		return FUeAgentOperationResult(
			Request.GetDescriptorIdentity(),
			Request.GetRequestId(),
			FUeAgentContractValue(JsonText(Root)));
	}
}

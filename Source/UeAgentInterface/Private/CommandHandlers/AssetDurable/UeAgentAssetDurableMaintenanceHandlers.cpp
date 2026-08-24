#include "CommandHandlers/AssetDurable/UeAgentAssetDurableMaintenanceHandlers.h"

#include "Composition/SimpleAuthoring/OperationHost/UeAgentSimpleAuthoringOperationHost.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Core/ContextResource/UeAgentContextResource.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace UE::UAI::CommandHandlers::AssetDurable
{
	using namespace CommandEdge;
	using namespace Contract;
	using namespace ContextResource;
	using namespace UE::UAI::AssetDurable;

	namespace
	{
		const FString Owner = TEXT("asset-durable-maintenance-owner");
		constexpr double MaxSafeJsonInteger = 9007199254740990.0;

		FUeAgentResourceTracker MakeResources()
		{
			return FUeAgentResourceTracker(FUeAgentResourceBudget({
				{EUeAgentResourceDimension::InputBytes, 8ll * 1024 * 1024},
				{EUeAgentResourceDimension::OutputBytes, 8ll * 1024 * 1024},
				{EUeAgentResourceDimension::StringBytes, 8ll * 1024 * 1024},
				{EUeAgentResourceDimension::Nodes, 4096},
				{EUeAgentResourceDimension::Items, 4096},
				{EUeAgentResourceDimension::Files, 256},
				{EUeAgentResourceDimension::ReferenceEdges, 4096},
				{EUeAgentResourceDimension::WorkUnits, 1048576},
				{EUeAgentResourceDimension::EstimatedMemoryBytes,
					64ll * 1024 * 1024},
			}));
		}

		bool Canonical(
			const TSharedPtr<FJsonObject>& Object,
			FString& OutJson,
			FString& OutError)
		{
			return CommandContract::Json::TryCanonicalize(
				Object, OutJson, OutError);
		}

		FUeAgentCommandResult Failure(
			const FUeAgentCommandRequest& Request,
			const FString& Code,
			const FString& Detail)
		{
			const TSharedPtr<FJsonObject> Error = MakeShared<FJsonObject>();
			Error->SetStringField(TEXT("code"), Code);
			Error->SetStringField(TEXT("detail"), Detail);
			FString ErrorJson;
			FString SerializationError;
			if (!Canonical(Error, ErrorJson, SerializationError))
			{
				ErrorJson = TEXT(
					"{\"code\":\"durable_maintenance_error_serialization_failed\"}");
			}
			return FUeAgentCommandResult::Failure(
				Request, TEXT("{}"), ErrorJson);
		}

		TArray<TSharedPtr<FJsonValue>> Strings(
			const TArray<FString>& Values)
		{
			TArray<TSharedPtr<FJsonValue>> Result;
			for (const FString& Value : Values)
			{
				Result.Add(MakeShared<FJsonValueString>(Value));
			}
			return Result;
		}

		TArray<TSharedPtr<FJsonValue>> States(
			const TArray<FDurableArtifactState>& Values)
		{
			TArray<TSharedPtr<FJsonValue>> Result;
			for (const FDurableArtifactState& Value : Values)
			{
				const TSharedPtr<FJsonObject> State = MakeShared<FJsonObject>();
				State->SetStringField(
					TEXT("artifact_id"), Value.GetArtifactId());
				State->SetStringField(
					TEXT("presence"), LexToString(Value.GetPresence()));
				if (!Value.GetContentHash().IsEmpty())
				{
					State->SetStringField(
						TEXT("content_hash"), Value.GetContentHash());
				}
				Result.Add(MakeShared<FJsonValueObject>(State));
			}
			return Result;
		}

		bool ReadSafeInteger(
			const TSharedPtr<FJsonObject>& Params,
			const FString& Name,
			int64& OutValue)
		{
			double Number = 0.0;
			if (!Params.IsValid()
				|| !Params->TryGetNumberField(Name, Number)
				|| !FMath::IsFinite(Number)
				|| Number != FMath::FloorToDouble(Number)
				|| Number < 0.0
				|| Number > MaxSafeJsonInteger)
			{
				return false;
			}
			OutValue = static_cast<int64>(Number);
			return true;
		}

		class FProductionMaintenancePort final
			: public IUeAgentAssetDurableMaintenancePort
		{
		public:
			virtual bool TryInspect(
				const FString& JournalId,
				FUeAgentResourceTracker& Resources,
				FAssetDurableSessionPtr& OutSession,
				TArray<FDurableArtifactState>& OutObservedStates,
				FAssetDurableError& OutError) override
			{
				return Composition::SimpleAuthoring::OperationHost::
					TryInspectDurableJournal(
						JournalId,
						Resources,
						OutSession,
						OutObservedStates,
						OutError);
			}

			virtual bool TryReconcile(
				const FString& JournalId,
				const FString& ExpectedJournalOwnerId,
				const int64 ExpectedFencingToken,
				const FString& VerificationId,
				FUeAgentResourceTracker& Resources,
				FDurableReceipt& OutReceipt,
				FAssetDurableError& OutError) override
			{
				return Composition::SimpleAuthoring::OperationHost::
					TryReconcileDurableJournal(
						JournalId,
						ExpectedJournalOwnerId,
						ExpectedFencingToken,
						VerificationId,
						Resources,
						OutReceipt,
						OutError);
			}
		};

		class FJournalReadHandler final
			: public IUeAgentExactCommandHandler
		{
		public:
			explicit FJournalReadHandler(
				TSharedRef<IUeAgentAssetDurableMaintenancePort> InPort)
				: Port(MoveTemp(InPort))
			{
			}

			virtual const FString& GetCommandId() const override
			{
				static const FString Id =
					TEXT("asset_durable.journal.read");
				return Id;
			}

			virtual const FString& GetPublicOwner() const override
			{
				return Owner;
			}

			virtual FUeAgentCommandResult Submit(
				const FUeAgentCommandRequest& Request,
				const FUeAgentCallerContext& Caller,
				const TSharedRef<IUeAgentCancellationSignal>& Cancellation)
				override
			{
				if (!Caller.IsValid()
					|| !Caller.GetAuthorityScopes().Contains(TEXT("uai:query")))
				{
					return Failure(
						Request,
						TEXT("asset_durable_query_authority_required"),
						TEXT("uai:query is required"));
				}
				if (Cancellation->IsCancelled())
				{
					return Failure(
						Request,
						TEXT("asset_durable_maintenance_cancelled"),
						TEXT("request was cancelled"));
				}
				FString JournalId;
				const TSharedPtr<FJsonObject>& Params = Request.GetParams();
				if (!Params.IsValid()
					|| !Params->TryGetStringField(
						TEXT("journal_id"), JournalId)
					|| JournalId.IsEmpty())
				{
					return Failure(
						Request,
						TEXT("asset_durable_journal_read_params_invalid"),
						TEXT("journal_id is required"));
				}

				FUeAgentResourceTracker Resources = MakeResources();
				FAssetDurableSessionPtr Session;
				TArray<FDurableArtifactState> ObservedStates;
				FAssetDurableError Error;
				if (!Port->TryInspect(
						JournalId,
						Resources,
						Session,
						ObservedStates,
						Error)
					|| !Session.IsValid())
				{
					return Failure(
						Request,
						Error.IsPresent()
							? Error.GetCode()
							: TEXT("asset_durable_journal_read_failed"),
						Error.GetDetail());
				}

				const TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
				Payload->SetStringField(
					TEXT("schema"),
					TEXT("uai.asset_durable.journal_inspection.v1"));
				Payload->SetStringField(
					TEXT("journal_id"), Session->GetJournalId());
				Payload->SetStringField(
					TEXT("journal_owner_id"),
					Session->GetRequest().GetJournalOwnerId());
				Payload->SetNumberField(
					TEXT("fencing_token"),
					static_cast<double>(
						Session->GetRequest().GetFencingToken()));
				Payload->SetStringField(
					TEXT("plan_fingerprint"),
					Session->GetRequest().GetPlan()->GetFingerprint());
				Payload->SetStringField(
					TEXT("inventory_hash"),
					Session->GetInventory().GetInventoryHash());
				Payload->SetStringField(
					TEXT("phase"), Session->GetCurrentPhase());
				Payload->SetArrayField(
					TEXT("phase_history"),
					Strings(Session->GetPhaseHistory()));
				Payload->SetArrayField(
					TEXT("pre_mutation_states"),
					States(Session->GetPreMutationStates()));
				Payload->SetArrayField(
					TEXT("expected_persisted_states"),
					States(Session->GetExpectedPersistedStates()));
				Payload->SetArrayField(
					TEXT("observed_states"), States(ObservedStates));
				Payload->SetBoolField(TEXT("read_only"), true);
				FString PayloadJson;
				FString SerializationError;
				if (!Canonical(Payload, PayloadJson, SerializationError))
				{
					return Failure(
						Request,
						TEXT("asset_durable_result_serialization_failed"),
						SerializationError);
				}
				return FUeAgentCommandResult::Success(
					Request, MoveTemp(PayloadJson));
			}

		private:
			TSharedRef<IUeAgentAssetDurableMaintenancePort> Port;
		};

		class FJournalReconcileHandler final
			: public IUeAgentExactCommandHandler
		{
		public:
			explicit FJournalReconcileHandler(
				TSharedRef<IUeAgentAssetDurableMaintenancePort> InPort)
				: Port(MoveTemp(InPort))
			{
			}

			virtual const FString& GetCommandId() const override
			{
				static const FString Id =
					TEXT("asset_durable.journal.reconcile");
				return Id;
			}

			virtual const FString& GetPublicOwner() const override
			{
				return Owner;
			}

			virtual FUeAgentCommandResult Submit(
				const FUeAgentCommandRequest& Request,
				const FUeAgentCallerContext& Caller,
				const TSharedRef<IUeAgentCancellationSignal>& Cancellation)
				override
			{
				if (!Caller.IsValid()
					|| !Caller.GetAuthorityScopes().Contains(TEXT("uai:operate")))
				{
					return Failure(
						Request,
						TEXT("asset_durable_operate_authority_required"),
						TEXT("uai:operate is required"));
				}
				if (Cancellation->IsCancelled())
				{
					return Failure(
						Request,
						TEXT("asset_durable_maintenance_cancelled"),
						TEXT("request was cancelled"));
				}
				FString JournalId;
				FString OwnerId;
				FString VerificationId;
				int64 FencingToken = -1;
				const TSharedPtr<FJsonObject>& Params = Request.GetParams();
				if (!Params.IsValid()
					|| !Params->TryGetStringField(
						TEXT("journal_id"), JournalId)
					|| !Params->TryGetStringField(
						TEXT("expected_journal_owner_id"), OwnerId)
					|| !ReadSafeInteger(
						Params, TEXT("expected_fencing_token"), FencingToken)
					|| !Params->TryGetStringField(
						TEXT("verification_id"), VerificationId)
					|| JournalId.IsEmpty()
					|| OwnerId.IsEmpty()
					|| VerificationId.IsEmpty())
				{
					return Failure(
						Request,
						TEXT("asset_durable_journal_reconcile_params_invalid"),
						TEXT("journal_id, expected owner/fence and verification_id are required"));
				}

				FUeAgentResourceTracker Resources = MakeResources();
				FDurableReceipt Receipt;
				FAssetDurableError Error;
				if (!Port->TryReconcile(
						JournalId,
						OwnerId,
						FencingToken,
						VerificationId,
						Resources,
						Receipt,
						Error))
				{
					return Failure(
						Request,
						Error.IsPresent()
							? Error.GetCode()
							: TEXT("asset_durable_journal_reconcile_failed"),
						Error.GetDetail());
				}

				const TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
				Payload->SetStringField(
					TEXT("schema"),
					TEXT("uai.asset_durable.reconciliation_receipt.v1"));
				Payload->SetStringField(
					TEXT("journal_id"), Receipt.GetJournalId());
				Payload->SetStringField(
					TEXT("status"), LexToString(Receipt.GetStatus()));
				Payload->SetStringField(
					TEXT("plan_fingerprint"),
					Receipt.GetPlanFingerprint());
				Payload->SetStringField(
					TEXT("inventory_hash"), Receipt.GetInventoryHash());
				Payload->SetArrayField(
					TEXT("phase_history"),
					Strings(Receipt.GetPhaseHistory()));
				Payload->SetStringField(
					TEXT("recovery_action"),
					LexToString(Receipt.GetRecoveryAction()));
				Payload->SetBoolField(
					TEXT("requires_independent_verification"),
					Receipt.RequiresIndependentVerification());
				Payload->SetBoolField(
					TEXT("replayed"), Receipt.IsReplayed());
				Payload->SetStringField(
					TEXT("verification_id"),
					Receipt.GetVerificationId());
				Payload->SetArrayField(
					TEXT("pre_mutation_states"),
					States(Receipt.GetPreMutationStates()));
				Payload->SetArrayField(
					TEXT("expected_persisted_states"),
					States(Receipt.GetExpectedPersistedStates()));
				if (!Receipt.GetFailureCode().IsEmpty())
				{
					const TSharedPtr<FJsonObject> FailureObject =
						MakeShared<FJsonObject>();
					FailureObject->SetStringField(
						TEXT("code"), Receipt.GetFailureCode());
					FailureObject->SetStringField(
						TEXT("detail"), Receipt.GetFailureDetail());
					Payload->SetObjectField(TEXT("failure"), FailureObject);
				}
				FString PayloadJson;
				FString SerializationError;
				if (!Canonical(Payload, PayloadJson, SerializationError))
				{
					return Failure(
						Request,
						TEXT("asset_durable_result_serialization_failed"),
						SerializationError);
				}
				return FUeAgentCommandResult::Success(
					Request, MoveTemp(PayloadJson));
			}

		private:
			TSharedRef<IUeAgentAssetDurableMaintenancePort> Port;
		};
	}

	TSharedRef<IUeAgentAssetDurableMaintenancePort>
	CreateProductionAssetDurableMaintenancePort()
	{
		return MakeShared<FProductionMaintenancePort>();
	}

	bool TryCreateAssetDurableMaintenanceHandlers(
		TSharedRef<IUeAgentAssetDurableMaintenancePort> Port,
		TArray<TSharedRef<IUeAgentExactCommandHandler>>& OutHandlers,
		FString& OutError)
	{
		OutHandlers.Reset();
		OutError.Reset();
		OutHandlers = {
			MakeShared<FJournalReadHandler>(Port),
			MakeShared<FJournalReconcileHandler>(MoveTemp(Port)),
		};
		return true;
	}
}

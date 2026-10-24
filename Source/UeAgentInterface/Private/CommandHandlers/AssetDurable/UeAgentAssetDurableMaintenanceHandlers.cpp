#include "CommandHandlers/AssetDurable/UeAgentAssetDurableMaintenanceHandlers.h"

#include "Utilities/UeAgentJsonDiagnostics.h"

#include "Core/Contract/UeAgentDiagnostic.h"

#include "Composition/SimpleAuthoring/OperationHost/UeAgentSimpleAuthoringOperationHost.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Core/ContextResource/UeAgentContextResource.h"
#include "Utilities/UeAgentPropertyValueCodec.h"
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
			return FUeAgentResourceTracker();
		}

		bool Canonical(
			const TSharedPtr<FJsonObject>& Object,
			FString& OutJson,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			return CommandContract::Json::TryCanonicalize(
				Object, OutJson, OutError);
		}

		FUeAgentCommandResult Failure(
            const FUeAgentCommandRequest& Request, UE::UAI::Contract::FDiagnostic Diagnostic)
        {
            if (Diagnostic.SubjectId.IsEmpty()) Diagnostic.SubjectId = Request.GetCommandId();
            return FUeAgentCommandResult::Failure(Request, TEXT("{}"), MoveTemp(Diagnostic));
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
			UE::UAI::Contract::FDiagnostic NumberError;
			return Params.IsValid() && UeAgentPropertyValueCodec::TryReadIntegerNumber(
				Params->TryGetField(Name), 0, static_cast<int64>(MaxSafeJsonInteger), OutValue, NumberError);
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
					return Failure(Request, UE::UAI::Contract::FDiagnostic::Failure(TEXT("asset_durable_query_authority_required"), TEXT("uai:query is required"), TEXT("durable_maintenance")));
				}
				if (Cancellation->IsCancelled())
				{
					return Failure(Request, UE::UAI::Contract::FDiagnostic::Failure(TEXT("asset_durable_maintenance_cancelled"), TEXT("request was cancelled"), TEXT("durable_maintenance")));
				}
				FString JournalId;
				const TSharedPtr<FJsonObject>& Params = Request.GetParams();
				if (!Params.IsValid()
					|| !Params->TryGetStringField(
						TEXT("journal_id"), JournalId)
					|| JournalId.IsEmpty())
				{
					auto Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(TEXT("asset_durable_journal_read_params_invalid"),
						TEXT("The journal_id field must be a non-empty string."), TEXT("durable_maintenance"), FString(), Request.GetCommandId());
					Diagnostic.PropertyPath = TEXT("journal_id");
					Diagnostic.CandidateName = Request.GetCommandId();
					const auto Value = Params.IsValid() ? Params->TryGetField(TEXT("journal_id")) : nullptr;
					Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("non-empty string"));
					Diagnostic.ActualValuePresent = Value.IsValid();
					const auto Actual = MakeShared<FJsonObject>();
					Actual->SetStringField(TEXT("type"), UeAgentJsonDiagnostics::JsonValueTypeToString(Value));
					if (Value.IsValid() && Value->Type == EJson::String) Actual->SetBoolField(TEXT("empty"), Value->AsString().IsEmpty());
					Diagnostic.ActualValue = MakeShared<FJsonValueObject>(Actual);
					return Failure(Request, Diagnostic);
				}

				FUeAgentResourceTracker Resources = MakeResources();
				FAssetDurableSessionPtr Session;
				TArray<FDurableArtifactState> ObservedStates;
				FAssetDurableError Error;
				const bool bInspected = Port->TryInspect(
						JournalId,
						Resources,
						Session,
						ObservedStates,
						Error);
				if (!bInspected || !Session.IsValid())
				{
					auto Diagnostic = Error;
					if (Error.IsEmpty())
					{
						Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(TEXT("asset_durable_journal_read_failed"), bInspected ? TEXT("The durable journal inspection reported success but returned no session.") : TEXT("The durable journal inspection failed without a diagnostic."), TEXT("durable_maintenance"));
						Diagnostic.CandidateName = Request.GetCommandId();
					}
					return Failure(Request, Diagnostic);
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
				UE::UAI::Contract::FDiagnostic SerializationError;
				if (!Canonical(Payload, PayloadJson, SerializationError))
				{
					return Failure(Request, SerializationError);
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
					return Failure(Request, UE::UAI::Contract::FDiagnostic::Failure(TEXT("asset_durable_operate_authority_required"), TEXT("uai:operate is required"), TEXT("durable_maintenance")));
				}
				if (Cancellation->IsCancelled())
				{
					return Failure(Request, UE::UAI::Contract::FDiagnostic::Failure(TEXT("asset_durable_maintenance_cancelled"), TEXT("request was cancelled"), TEXT("durable_maintenance")));
				}
				FString JournalId;
				FString OwnerId;
				FString VerificationId;
				int64 FencingToken = -1;
				const TSharedPtr<FJsonObject>& Params = Request.GetParams();
				FString InvalidField;
				FString Expected;
				if (!Params.IsValid())
				{
					InvalidField = TEXT("params");
					Expected = TEXT("object");
				}
				else
				{
					for (const auto& Field : {TPair<const TCHAR*, FString*>(TEXT("journal_id"), &JournalId),
						TPair<const TCHAR*, FString*>(TEXT("expected_journal_owner_id"), &OwnerId),
						TPair<const TCHAR*, FString*>(TEXT("verification_id"), &VerificationId)})
					{
						if (!Params->TryGetStringField(Field.Key, *Field.Value) || Field.Value->IsEmpty())
						{
							InvalidField = Field.Key;
							Expected = TEXT("non-empty string");
							break;
						}
					}
					if (InvalidField.IsEmpty() && !ReadSafeInteger(Params, TEXT("expected_fencing_token"), FencingToken))
					{
						InvalidField = TEXT("expected_fencing_token");
						Expected = FString::Printf(TEXT("integer in 0..%lld"), static_cast<int64>(MaxSafeJsonInteger));
					}
				}
				if (!InvalidField.IsEmpty())
				{
					auto Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(TEXT("asset_durable_journal_reconcile_params_invalid"),
						TEXT("The journal reconciliation field is missing or does not satisfy its required type."), TEXT("durable_maintenance"), FString(), Request.GetCommandId());
					Diagnostic.PropertyPath = InvalidField;
					Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(Expected);
					Diagnostic.CandidateName = Request.GetCommandId();
					const auto Value = Params.IsValid() ? Params->TryGetField(InvalidField) : nullptr;
					Diagnostic.ActualValuePresent = Value.IsValid();
					const auto Actual = MakeShared<FJsonObject>();
					Actual->SetStringField(TEXT("type"), UeAgentJsonDiagnostics::JsonValueTypeToString(Value));
					if (Value.IsValid() && Value->Type == EJson::String) Actual->SetBoolField(TEXT("empty"), Value->AsString().IsEmpty());
					Diagnostic.ActualValue = MakeShared<FJsonValueObject>(Actual);
					Diagnostic.PublicMessage = Value.IsValid() ? TEXT("The reconciliation field does not satisfy its required type or value constraint.") : TEXT("A required reconciliation field is missing.");
					return Failure(Request, Diagnostic);
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
					auto Diagnostic = Error;
					if (Error.IsEmpty())
					{
						Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(TEXT("asset_durable_journal_reconcile_failed"), TEXT("The durable journal reconciliation failed without a diagnostic."), TEXT("durable_maintenance"));
						Diagnostic.CandidateName = Request.GetCommandId();
					}
					return Failure(Request, Diagnostic);
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
						TEXT("detail"), Receipt.GetFailureDiagnostic().PublicMessage);
					FailureObject->SetObjectField(TEXT("diagnostic"), Receipt.GetFailureDiagnostic().ToPublicJson());
					Payload->SetObjectField(TEXT("failure"), FailureObject);
				}
				FString PayloadJson;
				UE::UAI::Contract::FDiagnostic SerializationError;
				if (!Canonical(Payload, PayloadJson, SerializationError))
				{
					return Failure(Request, SerializationError);
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
		UE::UAI::Contract::FDiagnostic& OutError)
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

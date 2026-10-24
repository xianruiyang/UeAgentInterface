#include "Core/DomainCapability/DelegatedCommandOperation/UeAgentDelegatedCommandOperation.h"

#include "Dom/JsonObject.h"
#include "Misc/SecureHash.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

THIRD_PARTY_INCLUDES_START
#include <openssl/sha.h>
THIRD_PARTY_INCLUDES_END

namespace UE::UAI::DomainCapability::DelegatedCommandOperation
{
	using namespace BindingGateway::Operation;
	using namespace CommandEdge;

	namespace
	{
		FString Sha256(const FString& Value)
		{
			const FTCHARToUTF8 Utf8(*Value);
			FSHA256Signature Signature{};
			const uint8 Empty = 0;
			const uint8* Data = Utf8.Length() > 0
				? reinterpret_cast<const uint8*>(Utf8.Get()) : &Empty;
			if (!SHA256(Data, static_cast<size_t>(Utf8.Length()),
				Signature.Signature))
			{
				return FString();
			}
			return TEXT("sha256:") + Signature.ToString().ToLower();
		}

		bool TryReadObject(const FString& Json, TSharedPtr<FJsonObject>& OutObject)
		{
			OutObject.Reset();
			return FJsonSerializer::Deserialize(
				TJsonReaderFactory<>::Create(Json), OutObject,
				FJsonSerializer::EFlags::StoreNumbersAsStrings)
				&& OutObject.IsValid();
		}

		void ReadFailure(const FUeAgentCommandResult& Result,
			UE::UAI::Contract::FDiagnostic& OutDiagnostic, bool& bOutMutationMayHaveStarted, bool& bOutRollbackConfirmed)
		{
			OutDiagnostic = Result.GetDiagnostic();
			if (OutDiagnostic.IsEmpty())
				OutDiagnostic = UE::UAI::Contract::FDiagnostic::Failure(TEXT("delegated_command_failed"), TEXT("The delegated command failed without a diagnostic."), TEXT("delegated_execute"));
			bOutRollbackConfirmed = OutDiagnostic.RollbackSucceeded.Get(false)
				|| OutDiagnostic.TerminalStatus == TEXT("rolled_back")
				|| OutDiagnostic.ExecutionStatus == TEXT("rolled_back")
				|| OutDiagnostic.TransactionStatus == TEXT("rolled_back");
			bOutMutationMayHaveStarted = OutDiagnostic.MutationMayHaveStarted.Get(false)
				|| OutDiagnostic.MutationStarted.Get(false) || bOutRollbackConfirmed
				|| OutDiagnostic.TerminalStatus == TEXT("state_unknown")
				|| OutDiagnostic.ExecutionStatus == TEXT("state_unknown")
				|| OutDiagnostic.TransactionStatus == TEXT("state_unknown");
		}

		class FInvocationState final
			: public IOperationVersionFencePort
			, public IOperationDomainMutationPort
			, public IOperationStabilityPort
			, public IOperationPersistencePort
			, public IOperationIndependentVerificationPort
			, public IDomainOperationInvocationContext
		{
		public:
			FInvocationState(
				TSharedRef<IUeAgentCommandHandler> InHandler,
				FUeAgentCommandRequest InRequest,
				Contract::FUeAgentCallerContext InCaller,
				TSharedRef<Contract::IUeAgentCancellationSignal> InCancellation,
				bool bInPersists,
				bool bInIndependentReadback)
				: Handler(MoveTemp(InHandler))
				, Request(MoveTemp(InRequest))
				, Caller(MoveTemp(InCaller))
				, Cancellation(MoveTemp(InCancellation))
				, bPersists(bInPersists)
				, bIndependentReadback(bInIndependentReadback)
			{
			}

			bool Check(const FOperationLifecyclePlan& Plan,
				EOperationLifecycleStage, bool& bOutCurrent,
				FOperationLifecycleError& OutError) override
			{
				bOutCurrent = false;
				OutError = {};
				if (Cancellation->IsCancelled())
				{
					OutError = FOperationLifecycleError::Failure(TEXT("operation_cancelled"), FString(), TEXT("operation_lifecycle"), Request.GetCommandId(), FString());
					return false;
				}
				const FString Scope = Plan.GetImpactScope().IsEmpty()
					? FString() : Plan.GetImpactScope()[0];
				const FString* Expected = Plan.GetExpectedRevisions().Find(Scope);
				if (!Expected || *Expected != Request.GetCommandBindingHash())
				{
					OutError = FOperationLifecycleError::Failure(TEXT("operation_version_conflict"), FString(), TEXT("operation_lifecycle"), Request.GetCommandId(), FString());
					return false;
				}
				bOutCurrent = true;
				return true;
			}

			bool Execute(const FOperationLifecyclePlan& Plan,
				FOperationMutationFacts& OutFacts,
				FOperationLifecycleError& OutError) override
			{
				OutFacts = {};
				OutError = {};
				Result = Handler->Submit(Request, Caller, Cancellation);
				if (!Result->IsOk())
				{
					ReadFailure(Result.GetValue(), FailureDiagnostic,
						bMutationMayHaveStarted, bRollbackConfirmed);
					if (bMutationMayHaveStarted)
					{
						OutFacts = FOperationMutationFacts(
							TEXT("delegated-failure:") + Plan.GetPlanId(),
							Plan.GetImpactScope(), true);
					}
					OutError = FailureDiagnostic;
					return false;
				}
				TSharedPtr<FJsonObject> Payload;
				if (!TryReadObject(Result->GetPayloadJson(), Payload))
				{
					OutError = FOperationLifecycleError::Failure(TEXT("delegated_command_result_invalid"), FString(), TEXT("operation_lifecycle"), Request.GetCommandId(), FString());
					return false;
				}
				Payload->TryGetStringField(TEXT("operation_id"), OperationId);
				if (OperationId.IsEmpty())
				{
					OperationId = TEXT("delegated:") + Request.GetRequestId();
				}
				OutFacts = FOperationMutationFacts(
					OperationId, Plan.GetImpactScope(), false);
				return true;
			}

			bool Rollback(const FOperationLifecyclePlan&,
				const FOperationMutationFacts&, bool& bOutConfirmed,
				FOperationLifecycleError& OutError) override
			{
				bOutConfirmed = bRollbackConfirmed;
				OutError = bOutConfirmed ? FOperationLifecycleError()
					: (!FailureDiagnostic.IsEmpty() ? FailureDiagnostic : UE::UAI::Contract::FDiagnostic::Failure(TEXT("delegated_rollback_unconfirmed"), TEXT("The operation rollback could not be confirmed."), TEXT("rollback")));
				return bOutConfirmed;
			}

			bool Settle(const FOperationLifecyclePlan&,
				const FOperationMutationFacts&,
				FOperationLifecycleError& OutError) override
			{
				OutError = {};
				return Result.IsSet() && Result->IsOk();
			}

			bool Persist(const FOperationLifecyclePlan&,
				const FOperationMutationFacts&,
				FString& OutPersistenceReceiptId,
				FOperationLifecycleError& OutError) override
			{
				OutError = {};
				OutPersistenceReceiptId = (bPersists
					? FString(TEXT("persisted:")) : FString(TEXT("observed:")))
					+ OperationId;
				return !OperationId.IsEmpty();
			}

			bool Verify(const FOperationLifecyclePlan&,
				const FOperationMutationFacts&,
				const FString& PersistenceReceiptId,
				bool& bOutEquivalent,
				FOperationLifecycleError& OutError) override
			{
				bOutEquivalent = false;
				OutError = {};
				TSharedPtr<FJsonObject> Payload;
				if (!Result.IsSet() || !Result->IsOk()
					|| PersistenceReceiptId.IsEmpty()
					|| !TryReadObject(Result->GetPayloadJson(), Payload))
				{
					OutError = FOperationLifecycleError::Failure(TEXT("delegated_independent_readback_invalid"), FString(), TEXT("operation_lifecycle"), Request.GetCommandId(), FString());
					return false;
				}
				// The delegate contract guarantees success is emitted only after its
				// own independent reader. This adapter verifies the immutable terminal
				// payload is still present; it does not create a second domain reader.
				bOutEquivalent = bIndependentReadback;
				if (!bOutEquivalent)
				{
					OutError = FOperationLifecycleError::Failure(TEXT("delegated_independent_readback_not_declared"), FString(), TEXT("operation_lifecycle"), Request.GetCommandId(), FString());
				}
				return bOutEquivalent;
			}

			bool TryBuildSuccessPayload(const FOperationLifecycleReceipt& Receipt,
				FString& OutPayloadJson, UE::UAI::Contract::FDiagnostic& OutError) const override
			{
				OutPayloadJson.Reset();
				OutError.Reset();
				if (!Result.IsSet() || !Result->IsOk()
					|| Receipt.GetStatus() != EOperationLifecycleStatus::Succeeded)
				{
					OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("delegated_command_result_missing"), TEXT("The operation has no successful result payload."), TEXT("result_projection"));
					return false;
				}
				OutPayloadJson = Result->GetPayloadJson();
				return !OutPayloadJson.IsEmpty();
			}

			

		private:
			TSharedRef<IUeAgentCommandHandler> Handler;
			FUeAgentCommandRequest Request;
			Contract::FUeAgentCallerContext Caller;
			TSharedRef<Contract::IUeAgentCancellationSignal> Cancellation;
			bool bPersists = false;
			bool bIndependentReadback = false;
			TOptional<FUeAgentCommandResult> Result;
			FString OperationId;
			UE::UAI::Contract::FDiagnostic FailureDiagnostic;
			bool bMutationMayHaveStarted = false;
			bool bRollbackConfirmed = false;
		};

		class FFactory final : public IDomainOperationInvocationFactory
		{
		public:
			FFactory(FString InOperationId, FMechanicsResolver InResolver,
				bool bInPersists, bool bInIndependentReadback)
				: OperationId(MoveTemp(InOperationId))
				, Resolver(InResolver)
				, bPersists(bInPersists)
				, bIndependentReadback(bInIndependentReadback)
			{
			}

			const FString& GetOperationId() const override { return OperationId; }

			bool TryCreate(const FUeAgentCommandRequest& Request,
				const Contract::FUeAgentCallerContext& Caller,
				const TSharedRef<Contract::IUeAgentCancellationSignal>& Cancellation,
				FDomainOperationInvocation& OutInvocation,
				FDomainOperationInvocationError& OutError) const override
			{
				OutInvocation = {};
				OutError = {};
				if (Request.GetCommandId() != OperationId || !Resolver
					|| !bIndependentReadback)
				{
					OutError = FDomainOperationInvocationError::Failure(TEXT("delegated_operation_contract_invalid"), FString(), TEXT("operation_lifecycle"), OperationId, Request.GetCommandId());
					return false;
				}
				TSharedPtr<IUeAgentCommandHandler> Handler;
				TArray<FString> MechanicsScopes;
				UE::UAI::Contract::FDiagnostic Error;
				if (!Resolver(OperationId, Handler, MechanicsScopes, Error)
					|| !Handler.IsValid())
				{
					OutError = Error.IsValid() ? Error : UE::UAI::Contract::FDiagnostic::Failure(TEXT("delegated_operation_runtime_unavailable"), TEXT("The delegated operation handler is unavailable."), TEXT("operation_lifecycle"), FString(), OperationId);
					return false;
				}
				TArray<FString> Scopes = Caller.GetAuthorityScopes();
				for (const FString& Scope : MechanicsScopes)
				{
					Scopes.AddUnique(Scope);
				}
				const Contract::FUeAgentCallerContext EffectiveCaller(
					Caller.GetCallerId(), MoveTemp(Scopes), Caller.GetCorrelationId());
				const TSharedRef<FInvocationState> State = MakeShared<FInvocationState>(
					Handler.ToSharedRef(), Request, EffectiveCaller, Cancellation,
					bPersists, bIndependentReadback);
				FString Target = OperationId;
				if (Request.GetParams().IsValid())
				{
					for (const TCHAR* Field : {TEXT("target_ref"), TEXT("target"),
						TEXT("object_path"), TEXT("physical_material_ref"),
						TEXT("physics_asset_ref")})
					{
						if (Request.GetParams()->TryGetStringField(Field, Target))
						{
							break;
						}
					}
				}
				const FString Scope = TEXT("delegated:") + OperationId
					+ TEXT(":") + Target;
				const FString Fingerprint = Sha256(OperationId + TEXT("\n")
					+ Request.GetCommandBindingHash() + TEXT("\n")
					+ Request.GetRequestId() + TEXT("\n") + Request.GetParamsJson());
				const TSharedRef<const FOperationLifecyclePlan> Plan =
					MakeShared<const FOperationLifecyclePlan>(
						TEXT("delegated:") + Request.GetRequestId(), Fingerprint,
						TArray<FString>{Scope},
						TMap<FString, FString>{{Scope,
							Request.GetCommandBindingHash()}},
						TArray<FString>{TEXT("delegated_terminal_evidence")});
				const TSharedRef<const FOperationImpactAuthorization> Authorization =
					MakeShared<const FOperationImpactAuthorization>(
						Fingerprint, TArray<FString>{Scope});
				const TSharedRef<FOperationLifecycleService> Service =
					MakeShared<FOperationLifecycleService>(
						StaticCastSharedRef<IOperationVersionFencePort>(State),
						StaticCastSharedRef<IOperationDomainMutationPort>(State),
						StaticCastSharedRef<IOperationStabilityPort>(State),
						StaticCastSharedRef<IOperationPersistencePort>(State),
						StaticCastSharedRef<IOperationIndependentVerificationPort>(State));
				return FDomainOperationInvocation::TryCreate(
					Plan, Authorization, Service,
					StaticCastSharedRef<IDomainOperationInvocationContext>(State),
					OutInvocation, OutError);
			}

		private:
			FString OperationId;
			FMechanicsResolver Resolver = nullptr;
			bool bPersists = false;
			bool bIndependentReadback = false;
		};
	}

	TSharedRef<const IDomainOperationInvocationFactory> CreateInvocationFactory(
		FString OperationId, const FMechanicsResolver Resolver,
		const bool bPersists, const bool bIndependentReadback)
	{
		return MakeShared<const FFactory>(MoveTemp(OperationId), Resolver,
			bPersists, bIndependentReadback);
	}
}

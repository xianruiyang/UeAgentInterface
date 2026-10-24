#include "CommandHandlers/PieRuntimeDebug/UeAgentPieRuntimeDebugCommandHandler.h"

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CommandEdge/UeAgentApplicationResultProjection.h"

#include "Dom/JsonValue.h"
#include "OperationOwners/PieRuntimeDebug/UeAgentPieRuntimeDebugOwner.h"

namespace UE::UAI::CommandHandlers::PieRuntimeDebug
{
	using namespace CommandEdge;
	using namespace Contract;
	using namespace OperationOwners::PieRuntimeDebug;

	namespace
	{
		const FString Owner(TEXT("pie-runtime-debug-command-owner"));

		FDiagnostic CommandFailure(const FString& Code, const FString& SubjectId, const FString& Reason)
		{
			FDiagnostic Diagnostic = FDiagnostic::Failure(Code, Reason, TEXT("command"));
			Diagnostic.Schema = TEXT("uai.runtime.diagnostic/1");
			Diagnostic.SourceKind = TEXT("native_result");
			Diagnostic.SubjectId = SubjectId;
			return Diagnostic;
		}

		

		class FHandler final : public IUeAgentExactCommandHandler
		{
		public:
			explicit FHandler(
				TSharedRef<IUeAgentOperationApplicationPort> InOperationPort,
				const FUeAgentPieRuntimeDebugRoute& InRoute)
				: Route(InRoute)
				, OperationPort(MoveTemp(InOperationPort))
			{
			}

			const FString& GetCommandId() const override
			{
				return Route.CommandId;
			}

			const FString& GetPublicOwner() const override
			{
				return Owner;
			}

			FUeAgentCommandResult Submit(
				const FUeAgentCommandRequest& Request,
				const FUeAgentCallerContext& Caller,
				const TSharedRef<IUeAgentCancellationSignal>& Cancellation) override
			{
				if (!Caller.IsValid()
					|| !Caller.GetAuthorityScopes().Contains(TEXT("uai:operate")))
				{
					auto Diagnostic = CommandFailure(TEXT("pie_runtime_debug_authority_required"),
						Request.GetCommandId(), !Caller.IsValid() ? TEXT("The caller context is invalid.")
							: TEXT("runtime.scenario.execute requires uai:operate"));
					Diagnostic.PropertyPath = !Caller.IsValid() ? TEXT("caller_valid") : TEXT("authority_scope_present");
					Diagnostic.ActualValue = MakeShared<FJsonValueBoolean>(false);
					Diagnostic.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
					return FUeAgentCommandResult::Failure(Request, TEXT("{}"), MoveTemp(Diagnostic));
				}
				TArray<FString> Scopes = Caller.GetAuthorityScopes();
				Scopes.AddUnique(TEXT("uai:operation"));
				Scopes.AddUnique(TEXT("uai:runtime"));
				const FUeAgentCallerContext EffectiveCaller(
					Caller.GetCallerId(),
					MoveTemp(Scopes),
					Caller.GetCorrelationId());
				const FUeAgentOperationResult Result = OperationPort->Submit(
					FUeAgentOperationRequest(
						Route.DescriptorIdentity,
						Request.GetRequestId(),
						TEXT("uai-command:") + Request.GetRequestId(),
						FUeAgentContractValue(Request.GetParamsJson())),
					EffectiveCaller,
					Cancellation);
				FApplicationResultProjectionPolicy Policy;
				Policy.InvalidResultCode = TEXT("pie_runtime_debug_result_invalid");
				Policy.InvalidPayloadCode = TEXT("pie_runtime_debug_payload_invalid");
				Policy.BoundaryFailureKind = EApplicationFailureKind::None;
				return ProjectOperationResult(Request, Route.DescriptorIdentity, Result, Policy);
			}

		private:
			FUeAgentPieRuntimeDebugRoute Route;
			TSharedRef<IUeAgentOperationApplicationPort> OperationPort;
		};
	}

	bool TryCreateHandler(
		TSharedRef<IUeAgentOperationApplicationPort> OperationPort,
		TSharedPtr<IUeAgentExactCommandHandler>& OutHandler,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutHandler.Reset();
		OutError.Reset();
		const FUeAgentPieRuntimeDebugRoute* Route = nullptr;
		if (!TryGetRoute(Route, OutError))
		{
			OutError = UE::UAI::Contract::FDiagnostic::WrapFailure(TEXT("pie_runtime_debug_route_invalid"), OutError, TEXT("handler_factory"));
			return false;
		}
		OutHandler = MakeShared<FHandler>(MoveTemp(OperationPort), *Route);
		return true;
	}
}

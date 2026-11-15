#include "CommandHandlers/LongRunning/UeAgentLongRunningLifecycleCommandHandlers.h"

#include "CommandEdge/UeAgentApplicationResultProjection.h"


namespace UE::UAI::CommandHandlers::LongRunning
{
	using namespace CommandEdge;
	using namespace Contract;
	using namespace OperationOwners::LongRunning;

	namespace
	{
		const FString Owner = TEXT("long-running-session-owner");

		FDiagnostic CommandFailure(const FString& Code, const FString& SubjectId, const FString& Reason)
		{
			FDiagnostic Diagnostic = FDiagnostic::Failure(Code, Reason, TEXT("command"));
			Diagnostic.Schema = TEXT("uai.runtime.diagnostic/1");
			Diagnostic.SubjectId = SubjectId;
			return Diagnostic;
		}

		

		class FHandler final : public IUeAgentExactCommandHandler
		{
		public:
			FHandler(
				FUeAgentLongRunningLifecycleRoute InRoute,
				TSharedRef<IUeAgentOperationApplicationPort> InOperationPort)
				: Route(MoveTemp(InRoute))
				, OperationPort(MoveTemp(InOperationPort))
			{
			}

			virtual const FString& GetCommandId() const override
			{
				return Route.CommandId;
			}

			virtual const FString& GetPublicOwner() const override
			{
				return Owner;
			}
			TUniquePtr<IUeAgentOperationReadScope> TryBeginConcurrentRead(const FUeAgentCommandRequest& Request) override
			{
				if (Route.CommandId != TEXT("operation.status") && Route.CommandId != TEXT("operation.receipt")) return {};
				return OperationPort->TryBeginConcurrentRead(FUeAgentOperationRequest(Route.DescriptorIdentity,
					Request.GetRequestId(), TEXT("uai-command:") + Request.GetRequestId(), FUeAgentContractValue(Request.GetParamsJson())));
			}

			virtual FUeAgentCommandResult Submit(
				const FUeAgentCommandRequest& Request,
				const FUeAgentCallerContext& CallerContext,
				const TSharedRef<IUeAgentCancellationSignal>& Cancellation)
				override
			{
				if (!CallerContext.IsValid()
					|| !CallerContext.GetAuthorityScopes().Contains(TEXT("uai:operate")))
				{
					return FUeAgentCommandResult::Failure(
						Request,
						TEXT("{}"),
						CommandFailure(
							TEXT("long_running_lifecycle_authority_required"),
							Request.GetCommandId(),
							TEXT("the public lifecycle route requires uai:operate")));
				}
				const FUeAgentOperationResult Result = OperationPort->Submit(
					FUeAgentOperationRequest(
						Route.DescriptorIdentity,
						Request.GetRequestId(),
						TEXT("uai-command:") + Request.GetRequestId(),
						FUeAgentContractValue(Request.GetParamsJson())),
					CallerContext,
					Cancellation);
				FApplicationResultProjectionPolicy Policy;
				Policy.InvalidResultCode = TEXT("long_running_lifecycle_result_invalid");
				Policy.InvalidPayloadCode = TEXT("long_running_lifecycle_payload_invalid");
				Policy.BoundaryFailureKind = EApplicationFailureKind::None;
				return ProjectOperationResult(Request, Route.DescriptorIdentity, Result, Policy);
			}

		private:
			FUeAgentLongRunningLifecycleRoute Route;
			TSharedRef<IUeAgentOperationApplicationPort> OperationPort;
		};
	}

	bool TryCreateLongRunningLifecycleCommandHandlers(
		TSharedRef<IUeAgentOperationApplicationPort> OperationPort,
		TArray<TSharedRef<IUeAgentExactCommandHandler>>& OutHandlers,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutHandlers.Reset();
		OutError.Reset();
		TArray<FUeAgentLongRunningLifecycleRoute> Routes;
		if (!TryGetLongRunningLifecycleRoutes(Routes, OutError)) return false;
		TSet<FString> CommandIds;
		for (const FUeAgentLongRunningLifecycleRoute& Route : Routes)
		{
			if (!Route.TryValidate(OutError))
			{
				OutHandlers.Reset();
				OutError = UE::UAI::Contract::FDiagnostic::WrapFailure(TEXT("long_running_lifecycle_route_invalid"), OutError, TEXT("handler_factory"));
				return false;
			}
			if (CommandIds.Contains(Route.CommandId))
			{
				OutHandlers.Reset();
				OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("long_running_lifecycle_route_invalid"), TEXT("The command route is registered more than once."), TEXT("handler_factory"), FString(), Route.CommandId);
				if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = Route.CommandId;
				return false;
			}
			CommandIds.Add(Route.CommandId);
			OutHandlers.Add(MakeShared<FHandler>(Route, OperationPort));
		}
		return true;
	}
}

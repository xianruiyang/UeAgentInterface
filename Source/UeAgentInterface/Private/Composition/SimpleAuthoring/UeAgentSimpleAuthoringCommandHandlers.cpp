#include "Composition/SimpleAuthoring/UeAgentSimpleAuthoringCommandHandlers.h"
#include "CommandEdge/UeAgentApplicationResultProjection.h"

#include "Dom/JsonValue.h"

namespace UE::UAI::Composition::SimpleAuthoring
{
	namespace
	{
		using namespace CommandEdge;
		using namespace Contract;

		



		class FSimpleAuthoringCommandHandler final
			: public IUeAgentCommandHandler
		{
		public:
			FSimpleAuthoringCommandHandler(
				FUeAgentSimpleAuthoringCommandRoute InRoute,
				TSharedRef<IUeAgentOperationApplicationPort>
					InOperationPort)
				: Route(MoveTemp(InRoute))
				, OperationPort(MoveTemp(InOperationPort))
			{
			}

			const FString& GetCommandId() const override
			{
				return Route.CommandId;
			}

			FUeAgentCommandResult Submit(
				const FUeAgentCommandRequest& Request,
				const FUeAgentCallerContext& CallerContext,
				const TSharedRef<IUeAgentCancellationSignal>&
					Cancellation) override
			{
				const FUeAgentOperationRequest OperationRequest(
					Route.DescriptorIdentity,
					Request.GetRequestId(),
					TEXT("uai-command:") + Request.GetRequestId(),
					FUeAgentContractValue(Request.GetParamsJson()));
				const FUeAgentOperationResult Result =
					OperationPort->Submit(
						OperationRequest,
						CallerContext,
						Cancellation);
				return ProjectOperationResult(Request, Route.DescriptorIdentity, Result);
			}

		private:
			const FUeAgentSimpleAuthoringCommandRoute Route;
			TSharedRef<IUeAgentOperationApplicationPort> OperationPort;
		};
	}

	bool TryCreateSimpleAuthoringCommandHandlers(
		const TArray<FUeAgentSimpleAuthoringCommandRoute>& Routes,
		TSharedRef<Contract::IUeAgentOperationApplicationPort>
			OperationPort,
		TArray<TSharedRef<CommandEdge::IUeAgentCommandHandler>>&
			OutHandlers,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutHandlers.Reset();
		OutError.Reset();
		if (Routes.IsEmpty())
		{
			OutError = Contract::FDiagnostic::Failure(TEXT("simple_authoring_command_routes_missing"),
				TEXT("At least one command route is required to register authoring handlers."), TEXT("registration"));
			OutError.SourceKind = TEXT("native_result");
			OutError.PropertyPath = TEXT("routes");
			OutError.ActualValue = MakeShared<FJsonValueNumber>(Routes.Num());
			OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("at least one route"));
			return false;
		}

		TSet<FString> CommandIds;
		TSet<FString> DescriptorIds;
		OutHandlers.Reserve(Routes.Num());
		for (const FUeAgentSimpleAuthoringCommandRoute& Route : Routes)
		{
			const FString DescriptorId =
				Route.DescriptorIdentity.GetDescriptorId();
			if (!Route.TryValidate(OutError))
			{
				OutHandlers.Reset();
				if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = Route.CommandId;
				return false;
			}
			if (CommandIds.Contains(Route.CommandId))
			{
				OutHandlers.Reset();
				OutError = Contract::FDiagnostic::Failure(TEXT("simple_authoring_command_handler_duplicate"),
					TEXT("More than one authoring route uses the same command ID."), TEXT("registration"));
				OutError.SourceKind = TEXT("native_result");
				OutError.PropertyPath = TEXT("command_id");
				OutError.ActualValue = MakeShared<FJsonValueString>(Route.CommandId);
				return false;
			}
			if (DescriptorIds.Contains(DescriptorId))
			{
				OutHandlers.Reset();
				OutError = Contract::FDiagnostic::Failure(TEXT("simple_authoring_descriptor_handler_duplicate"),
					TEXT("More than one authoring command route uses the same descriptor ID."), TEXT("registration"));
				OutError.SourceKind = TEXT("native_result");
				OutError.CandidateName = Route.CommandId;
				OutError.PropertyPath = TEXT("descriptor_id");
				OutError.ActualValue = MakeShared<FJsonValueString>(DescriptorId);
				return false;
			}
			CommandIds.Add(Route.CommandId);
			DescriptorIds.Add(DescriptorId);
			OutHandlers.Add(
				MakeShared<FSimpleAuthoringCommandHandler>(
					Route,
					OperationPort));
		}
		return true;
	}
}

#include "OperationOwners/AnimationCurveCommands/UeAgentAnimationCurveCommandOperationOwner.h"

#include "DomainAdapters/AnimationCurveCommands/UeAgentAnimationCurveCommandMechanicsBoundary.h"

namespace UE::UAI::OperationOwners::AnimationCurveCommands
{
	bool TryResolveMechanics(
		const FString& CommandId,
		TSharedPtr<CommandEdge::IUeAgentCommandHandler>& OutHandler,
		TArray<FString>& OutAuthorityScopes,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		return DomainAdapters::AnimationCurveCommands::TryResolveMechanics(
			CommandId, OutHandler, OutAuthorityScopes, OutError);
	}

	CommandEdge::FUeAgentCommandResult Submit(
		const TSharedRef<CommandEdge::IUeAgentCommandHandler>& Mechanics,
		const CommandEdge::FUeAgentCommandRequest& Request,
		const Contract::FUeAgentCallerContext& Caller,
		const TSharedRef<Contract::IUeAgentCancellationSignal>& Cancellation)
	{
		return DomainAdapters::AnimationCurveCommands::Submit(
			Mechanics, Request, Caller, Cancellation);
	}
}

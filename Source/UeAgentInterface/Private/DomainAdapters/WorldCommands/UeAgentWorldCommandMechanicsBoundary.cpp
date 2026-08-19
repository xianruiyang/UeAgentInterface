#include "DomainAdapters/WorldCommands/UeAgentWorldCommandMechanicsBoundary.h"

#include "Composition/SimpleAuthoring/OperationHost/UeAgentSimpleAuthoringOperationHost.h"
#include "DomainAdapters/WorldSimpleEffect/UeAgentWorldSimpleEffectCapabilities.h"
#include "QueryOwners/WorldCommands/UeAgentWorldCommandQueryOwner.h"

namespace UE::UAI::DomainAdapters::WorldCommands
{
	bool TryResolveMechanics(
		const FString& CommandId,
		TSharedPtr<CommandEdge::IUeAgentCommandHandler>& OutHandler,
		TArray<FString>& OutAuthorityScopes,
		FString& OutError)
	{
		OutHandler.Reset();
		OutAuthorityScopes.Reset();
		OutError.Reset();
		if (!QueryOwners::WorldCommands::IsCommand(CommandId))
		{
			OutError = TEXT("world_command_not_owned:") + CommandId;
			return false;
		}
		if (!Composition::SimpleAuthoring::OperationHost::TryResolveHandler(
				CommandId, OutHandler, OutError)
			|| !OutHandler.IsValid())
		{
			return false;
		}
		OutAuthorityScopes = DomainAdapters::WorldSimpleEffect::
			GetWorldSimpleEffectAuthorityScopes();
		return !OutAuthorityScopes.IsEmpty();
	}

	CommandEdge::FUeAgentCommandResult Submit(
		const TSharedRef<CommandEdge::IUeAgentCommandHandler>& Mechanics,
		const CommandEdge::FUeAgentCommandRequest& Request,
		const Contract::FUeAgentCallerContext& Caller,
		const TSharedRef<Contract::IUeAgentCancellationSignal>& Cancellation)
	{
		return Mechanics->Submit(Request, Caller, Cancellation);
	}
}

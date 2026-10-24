#include "DomainAdapters/WorldCommands/UeAgentWorldCommandMechanicsBoundary.h"

#include "Composition/SimpleAuthoring/OperationHost/UeAgentSimpleAuthoringOperationHost.h"
#include "DomainAdapters/WorldSimpleEffect/UeAgentWorldSimpleEffectCapabilities.h"
#include "Dom/JsonValue.h"
#include "QueryOwners/WorldCommands/UeAgentWorldCommandQueryOwner.h"

namespace UE::UAI::DomainAdapters::WorldCommands
{
	bool TryResolveMechanics(
		const FString& CommandId,
		TSharedPtr<CommandEdge::IUeAgentCommandHandler>& OutHandler,
		TArray<FString>& OutAuthorityScopes,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutHandler.Reset();
		OutAuthorityScopes.Reset();
		OutError.Reset();
		if (!QueryOwners::WorldCommands::IsCommand(CommandId))
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("world_command_not_owned"),
				TEXT("The selected command is not owned by the World domain."), TEXT("runtime_factory"), CommandId);
			OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
			OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
			OutError.PropertyPath = TEXT("world_command_owned");
			OutError.SourceKind = TEXT("native_result");
			return false;
		}
		if (!Composition::SimpleAuthoring::OperationHost::TryResolveHandler(
				CommandId, OutHandler, OutError)
			|| !OutHandler.IsValid())
		{
			if (OutError.IsEmpty()) {
				OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("world_command_mechanics_unavailable"),
					TEXT("The World handler resolver failed or returned no executable handler without reporting a diagnostic."), TEXT("runtime_factory"), CommandId);
				OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
				OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
				OutError.PropertyPath = TEXT("command_handler_ready");
				OutError.SourceKind = TEXT("native_result");
			}
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

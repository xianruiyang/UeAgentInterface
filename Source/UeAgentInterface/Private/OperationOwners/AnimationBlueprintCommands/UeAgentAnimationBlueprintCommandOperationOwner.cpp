#include "OperationOwners/AnimationBlueprintCommands/UeAgentAnimationBlueprintCommandOperationOwner.h"

#include "Composition/GraphCompound/UeAgentGraphCompoundAnimationBindings.h"
#include "Composition/GraphCompound/UeAgentGraphCompoundCommandHandlers.h"
#include "DomainAdapters/AnimationBlueprintCommands/UeAgentAnimationBlueprintCommandMechanicsBoundary.h"
#include "Misc/Paths.h"

namespace UE::UAI::OperationOwners::AnimationBlueprintCommands
{
	using namespace Composition::GraphCompound;

	bool TryCreateMechanicsHandlers(
		TMap<FString, TSharedRef<CommandEdge::IUeAgentCommandHandler>>& OutHandlers,
		FString& OutError)
	{
		OutHandlers.Reset();
		OutError.Reset();
		TArray<FUeAgentGraphCompoundActivationProfile> Profiles;
		if (!DomainAdapters::AnimationBlueprintCommands::TryGetActivationProfiles(
				Profiles, OutError))
		{
			return false;
		}
		FGraphCompoundRuntimeConfig RuntimeConfig;
		RuntimeConfig.MaxFrameBytes = 16 * 1024 * 1024;
		FGraphCompoundAnimationProductionConfig ProductionConfig;
		ProductionConfig.DurableJournalRoot = FPaths::ProjectSavedDir()
			/ TEXT("UeAgentInterface/GraphCompound/AnimationBlueprintCommands");
		ProductionConfig.OperationTimeoutSeconds = 120.0;
		TSharedPtr<FGraphCompoundRuntime> Runtime;
		FGraphCompoundRuntimeError RuntimeError;
		if (!TryCreateAnimationGraphProductionRuntime(
				RuntimeConfig, ProductionConfig, Runtime, RuntimeError)
			|| !Runtime.IsValid())
		{
			OutError = RuntimeError.Code + TEXT(":") + RuntimeError.Detail;
			return false;
		}
		TArray<TSharedRef<CommandEdge::IUeAgentCommandHandler>> Mechanics;
		if (!TryCreateGraphCompoundCommandHandlers(
				Profiles,
				Runtime->GetQueryPort(),
				Runtime->GetOperationPort(),
				Mechanics,
				OutError))
		{
			return false;
		}
		for (const TSharedRef<CommandEdge::IUeAgentCommandHandler>& Handler
			: Mechanics)
		{
			if (OutHandlers.Contains(Handler->GetCommandId()))
			{
				OutHandlers.Reset();
				OutError = TEXT("animation_blueprint_command_mechanics_handler_duplicate");
				return false;
			}
			OutHandlers.Add(Handler->GetCommandId(), Handler);
		}
		return OutHandlers.Num()
			== DomainAdapters::AnimationBlueprintCommands::GetCommandIds().Num();
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


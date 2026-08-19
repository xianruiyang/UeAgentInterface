#include "OperationOwners/AudioCommands/UeAgentAudioCommandOperationOwner.h"

#include "Composition/GraphCompound/UeAgentGraphCompoundAudioMediaBindings.h"
#include "Composition/GraphCompound/UeAgentGraphCompoundCommandHandlers.h"
#include "DomainAdapters/AudioCommands/UeAgentAudioCommandMechanicsBoundary.h"
#include "Misc/Paths.h"

namespace UE::UAI::OperationOwners::AudioCommands
{
	using namespace Composition::GraphCompound;
	bool TryCreateMechanicsHandlers(
		TMap<FString, TSharedRef<CommandEdge::IUeAgentCommandHandler>>& OutHandlers,
		FString& OutError)
	{
		OutHandlers.Reset();
		OutError.Reset();
		TArray<FUeAgentGraphCompoundActivationProfile> Profiles;
		if (!DomainAdapters::AudioCommands::TryGetActivationProfiles(Profiles, OutError))
		{
			return false;
		}
		FGraphCompoundRuntimeConfig RuntimeConfig;
		RuntimeConfig.MaxFrameBytes = 16 * 1024 * 1024;
		FGraphCompoundAudioMediaProductionConfig ProductionConfig;
		ProductionConfig.DurableJournalRoot = FPaths::ProjectSavedDir()
			/ TEXT("UeAgentInterface/GraphCompound/AudioCommands");
		ProductionConfig.OperationTimeoutSeconds = 120.0;
		TSharedPtr<FGraphCompoundRuntime> Runtime;
		FGraphCompoundRuntimeError RuntimeError;
		if (!TryCreateAudioMediaGraphProductionRuntime(
				RuntimeConfig, ProductionConfig, Runtime, RuntimeError)
			|| !Runtime.IsValid())
		{
			OutError = RuntimeError.Code + TEXT(":") + RuntimeError.Detail;
			return false;
		}
		TArray<TSharedRef<CommandEdge::IUeAgentCommandHandler>> Mechanics;
		if (!TryCreateGraphCompoundCommandHandlers(
				Profiles, Runtime->GetQueryPort(), Runtime->GetOperationPort(), Mechanics, OutError))
		{
			return false;
		}
		for (const auto& Handler : Mechanics)
		{
			if (OutHandlers.Contains(Handler->GetCommandId()))
			{
				OutHandlers.Reset();
				OutError = TEXT("audio_command_mechanics_handler_duplicate");
				return false;
			}
			OutHandlers.Add(Handler->GetCommandId(), Handler);
		}
		return OutHandlers.Num() == DomainAdapters::AudioCommands::GetCommandIds().Num();
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

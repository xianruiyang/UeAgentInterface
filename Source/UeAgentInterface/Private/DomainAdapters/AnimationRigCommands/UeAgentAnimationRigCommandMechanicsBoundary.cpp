#include "DomainAdapters/AnimationRigCommands/UeAgentAnimationRigCommandMechanicsBoundary.h"

#include "Composition/GraphCompound/UeAgentGraphCompoundAnimationBindings.h"
#include "Composition/GraphCompound/UeAgentGraphCompoundCommandHandlers.h"
#include "Composition/TimelineSpatial/UeAgentTimelineSpatialMechanicsHost.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "QueryOwners/AnimationRigCommands/UeAgentAnimationRigCommandQueryOwner.h"

namespace UE::UAI::DomainAdapters::AnimationRigCommands
{
	using namespace Composition::GraphCompound;

	namespace
	{
		struct FProcessHost final
		{
			FCriticalSection Mutex;
			TMap<FString, TSharedRef<CommandEdge::IUeAgentCommandHandler>> Handlers;
		};

		FProcessHost& ProcessHost()
		{
			static FProcessHost Host;
			return Host;
		}

		bool TryCreateGraphMechanics(
			TMap<FString, TSharedRef<CommandEdge::IUeAgentCommandHandler>>& OutHandlers,
			FString& OutError)
		{
			OutHandlers.Reset();
			TArray<FUeAgentGraphCompoundActivationProfile> Profiles;
			if (!QueryOwners::AnimationRigCommands::TryGetGraphProfiles(Profiles, OutError))
			{
				return false;
			}
			FGraphCompoundRuntimeConfig RuntimeConfig;
			RuntimeConfig.MaxFrameBytes = 16 * 1024 * 1024;
			FGraphCompoundAnimationProductionConfig ProductionConfig;
			ProductionConfig.DurableJournalRoot = FPaths::ProjectSavedDir()
				/ TEXT("UeAgentInterface/GraphCompound/AnimationRigCommands");
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
					Profiles, Runtime->GetQueryPort(), Runtime->GetOperationPort(),
					Mechanics, OutError))
			{
				return false;
			}
			for (const TSharedRef<CommandEdge::IUeAgentCommandHandler>& Handler : Mechanics)
			{
				if (OutHandlers.Contains(Handler->GetCommandId()))
				{
					OutHandlers.Reset();
					OutError = TEXT("animation_rig_graph_mechanics_duplicate:") + Handler->GetCommandId();
					return false;
				}
				OutHandlers.Add(Handler->GetCommandId(), Handler);
			}
			return OutHandlers.Num() == Profiles.Num();
		}
	}

	bool TryResolveMechanics(
		const FString& CommandId,
		TSharedPtr<CommandEdge::IUeAgentCommandHandler>& OutHandler,
		TArray<FString>& OutAuthorityScopes,
		FString& OutError)
	{
		if (CommandId == TEXT("animation.skeleton.slots.read"))
		{
			return Composition::TimelineSpatial::TryResolveTimelineSpatialMechanics(
				CommandId, OutHandler, OutAuthorityScopes, OutError);
		}
		OutHandler.Reset();
		OutAuthorityScopes.Reset();
		OutError.Reset();
		FProcessHost& Host = ProcessHost();
		FScopeLock Lock(&Host.Mutex);
		if (Host.Handlers.IsEmpty() && !TryCreateGraphMechanics(Host.Handlers, OutError))
		{
			return false;
		}
		const TSharedRef<CommandEdge::IUeAgentCommandHandler>* Found = Host.Handlers.Find(CommandId);
		if (!Found)
		{
			OutError = TEXT("animation_rig_graph_mechanics_missing:") + CommandId;
			return false;
		}
		OutHandler = *Found;
		OutAuthorityScopes.Add(TEXT("animation_rig_graph.write"));
		return true;
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

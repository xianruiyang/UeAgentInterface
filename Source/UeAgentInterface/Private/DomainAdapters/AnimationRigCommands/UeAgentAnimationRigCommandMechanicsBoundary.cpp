#include "DomainAdapters/AnimationRigCommands/UeAgentAnimationRigCommandMechanicsBoundary.h"
#include "Dom/JsonValue.h"

#include "Composition/GraphCompound/UeAgentGraphCompoundAnimationBindings.h"
#include "Composition/GraphCompound/UeAgentGraphCompoundCommandHandlers.h"
#include "Composition/TimelineSpatial/UeAgentTimelineSpatialMechanicsHost.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "QueryOwners/AnimationRigCommands/UeAgentAnimationRigCommandQueryOwner.h"

namespace UE::UAI::DomainAdapters::AnimationRigCommands
{
	using namespace Composition::GraphCompound;

	const TArray<FString>& GetCommandIds()
	{
		return QueryOwners::AnimationRigCommands::GetCommandIds();
	}

	const TArray<FString>& GetPublicCommandIds()
	{
		static const TArray<FString> Ids = []
		{
			TArray<FString> Result = GetCommandIds();
			Result.Remove(TEXT("animation.control_rig.hierarchy_graph.apply"));
			Result.Remove(TEXT("animation.deformer_graph.topology.apply"));
			Result.Remove(TEXT("animation.ik_rig.definition.apply"));
			Result.Remove(TEXT("animation.ik_retargeter.definition.apply"));
			Result.Remove(TEXT("animation.ik_retargeter.settings.apply"));
			Result.Remove(TEXT("animation.skeleton.definition.apply"));
			Result.Remove(TEXT("animation.skeleton.compatibility.apply"));
			Result.Remove(TEXT("animation.skeleton.virtual_bone.apply"));
			return Result;
		}();
		return Ids;
	}

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
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			OutHandlers.Reset();
			TArray<FUeAgentGraphCompoundActivationProfile> Profiles;
			if (!QueryOwners::AnimationRigCommands::TryGetGraphProfiles(Profiles, OutError))
			{
				return false;
			}
			FGraphCompoundRuntimeConfig RuntimeConfig;

			FGraphCompoundAnimationProductionConfig ProductionConfig;
			ProductionConfig.DurableJournalRoot = FPaths::ProjectSavedDir()
				/ TEXT("UeAgentInterface/GraphCompound/AnimationRigCommands");
			TSharedPtr<FGraphCompoundRuntime> Runtime;
			FGraphCompoundRuntimeError RuntimeError;
			if (!TryCreateAnimationGraphProductionRuntime(
				RuntimeConfig, ProductionConfig, Runtime, RuntimeError))
			{
				OutError = MoveTemp(RuntimeError);
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
					OutError = Contract::FDiagnostic::Failure(TEXT("animation_rig_graph_mechanics_duplicate"),
						TEXT("Two animation graph mechanics handlers are registered for the same command."), TEXT("runtime_factory"));
					OutError.SourceKind = TEXT("native_result");
					OutError.ActualValue = MakeShared<FJsonValueString>(Handler->GetCommandId());
					OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("unique command identifier"));
					return false;
				}
				OutHandlers.Add(Handler->GetCommandId(), Handler);
			}
			if (OutHandlers.Num() != Profiles.Num())
			{
				OutError = Contract::FDiagnostic::Failure(TEXT("animation_rig_graph_mechanics_count_mismatch"),
					TEXT("The number of created animation graph handlers does not match the registered profiles."), TEXT("runtime_factory"));
				OutError.SourceKind = TEXT("native_result");
				OutError.ActualValue = MakeShared<FJsonValueNumber>(OutHandlers.Num());
				OutError.ExpectedValue = MakeShared<FJsonValueNumber>(Profiles.Num());
				return false;
			}
			return true;
		}
	}

	bool TryResolveMechanics(
		const FString& CommandId,
		TSharedPtr<CommandEdge::IUeAgentCommandHandler>& OutHandler,
		TArray<FString>& OutAuthorityScopes,
		UE::UAI::Contract::FDiagnostic& OutError)
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
			OutError = Contract::FDiagnostic::Failure(TEXT("animation_rig_graph_mechanics_missing"),
				TEXT("No animation graph mechanics handler is registered for the requested command."), TEXT("runtime_factory"));
			OutError.SourceKind = TEXT("native_result");
			OutError.ActualValue = MakeShared<FJsonValueString>(CommandId);
			OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("registered animation graph command"));
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

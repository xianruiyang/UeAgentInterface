#include "DomainAdapters/AnimationCurveCommands/UeAgentAnimationCurveCommandMechanicsBoundary.h"

#include "Composition/GraphCompound/UeAgentGraphCompoundAnimationBindings.h"
#include "Composition/GraphCompound/UeAgentGraphCompoundCommandHandlers.h"
#include "Composition/TimelineSpatial/UeAgentTimelineSpatialMechanicsHost.h"
#include "Dom/JsonValue.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "QueryOwners/AnimationCurveCommands/UeAgentAnimationCurveCommandQueryOwner.h"

namespace UE::UAI::DomainAdapters::AnimationCurveCommands
{
	using namespace Composition::GraphCompound;

	namespace
	{
		struct FGraphMechanicsHost final
		{
			FCriticalSection Mutex;
			TMap<FString, TSharedRef<CommandEdge::IUeAgentCommandHandler>> Handlers;
		};

		FGraphMechanicsHost& GraphMechanicsHost()
		{
			static FGraphMechanicsHost Host;
			return Host;
		}

		bool TryCreateGraphMechanics(
			TMap<FString, TSharedRef<CommandEdge::IUeAgentCommandHandler>>& OutHandlers,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			OutHandlers.Reset();
			TArray<FUeAgentGraphCompoundActivationProfile> Profiles;
			if (!QueryOwners::AnimationCurveCommands::TryGetGraphProfiles(Profiles, OutError))
			{
				return false;
			}
			FGraphCompoundRuntimeConfig RuntimeConfig;

			FGraphCompoundAnimationProductionConfig ProductionConfig;
			ProductionConfig.DurableJournalRoot = FPaths::ProjectSavedDir()
				/ TEXT("UeAgentInterface/GraphCompound/AnimationCurveCommands");
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
					OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("animation_curve_graph_mechanics_duplicate"),
						TEXT("More than one graph mechanics handler was created for command ") + Handler->GetCommandId() + TEXT("."), TEXT("runtime_factory"));
					OutError.ExpectedValue = MakeShared<FJsonValueNumber>(1);
					OutError.ActualValue = MakeShared<FJsonValueNumber>(2);
					OutError.PropertyPath = TEXT("command_handler_registration_count");
					OutError.SourceKind = TEXT("native_result");
					return false;
				}
				OutHandlers.Add(Handler->GetCommandId(), Handler);
			}
			if (OutHandlers.Num() != Profiles.Num()) {
				OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("animation_curve_graph_mechanics_count_mismatch"),
					TEXT("The graph mechanics factory must provide one command handler for each animation activation profile."), TEXT("runtime_factory"));
				OutError.ExpectedValue = MakeShared<FJsonValueNumber>(Profiles.Num());
				OutError.ActualValue = MakeShared<FJsonValueNumber>(OutHandlers.Num());
				OutError.PropertyPath = TEXT("command_handler_count");
				OutError.SourceKind = TEXT("native_result");
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
		if (CommandId == TEXT("animation.blend_space.asset.create")
			|| CommandId == TEXT("animation.blend_space.definition.apply"))
		{
			OutHandler.Reset();
			OutAuthorityScopes.Reset();
			OutError.Reset();
			FGraphMechanicsHost& Host = GraphMechanicsHost();
			FScopeLock Lock(&Host.Mutex);
			if (Host.Handlers.IsEmpty()
				&& !TryCreateGraphMechanics(Host.Handlers, OutError))
			{
				return false;
			}
			const TSharedRef<CommandEdge::IUeAgentCommandHandler>* Found = Host.Handlers.Find(CommandId);
			if (!Found)
			{
				OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("animation_curve_graph_mechanics_missing"),
					TEXT("No graph mechanics handler is registered for the requested animation command."), TEXT("runtime_factory"));
				OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("command with a registered graph mechanics handler"));
				OutError.ActualValue = MakeShared<FJsonValueString>(CommandId);
				OutError.PropertyPath = TEXT("command_id");
				OutError.SourceKind = TEXT("native_result");
				return false;
			}
			OutHandler = *Found;
			OutAuthorityScopes.Add(TEXT("animation_rig_graph.write"));
			return true;
		}
		return Composition::TimelineSpatial::TryResolveTimelineSpatialMechanics(
			CommandId, OutHandler, OutAuthorityScopes, OutError);
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

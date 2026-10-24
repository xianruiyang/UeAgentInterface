#include "OperationOwners/AnimationBlueprintCommands/UeAgentAnimationBlueprintCommandOperationOwner.h"

#include "Composition/GraphCompound/UeAgentGraphCompoundAnimationBindings.h"
#include "Composition/GraphCompound/UeAgentGraphCompoundCommandHandlers.h"
#include "DomainAdapters/AnimationBlueprintCommands/UeAgentAnimationBlueprintCommandMechanicsBoundary.h"
#include "Dom/JsonValue.h"
#include "Misc/Paths.h"

namespace UE::UAI::OperationOwners::AnimationBlueprintCommands
{
	using namespace Composition::GraphCompound;

	bool TryCreateMechanicsHandlers(
		TMap<FString, TSharedRef<CommandEdge::IUeAgentCommandHandler>>& OutHandlers,
		UE::UAI::Contract::FDiagnostic& OutError)
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

		FGraphCompoundAnimationProductionConfig ProductionConfig;
		ProductionConfig.DurableJournalRoot = FPaths::ProjectSavedDir()
			/ TEXT("UeAgentInterface/GraphCompound/AnimationBlueprintCommands");
		TSharedPtr<FGraphCompoundRuntime> Runtime;
		FGraphCompoundRuntimeError RuntimeError;
		if (!TryCreateAnimationGraphProductionRuntime(
				RuntimeConfig, ProductionConfig, Runtime, RuntimeError)
			|| !Runtime.IsValid())
		{
			OutError = RuntimeError;
			if (OutError.IsEmpty()) {
				OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("animation_blueprint_command_mechanics_runtime_unavailable"),
					TEXT("The Animation Blueprint runtime factory failed or returned no executable runtime without reporting a diagnostic."), TEXT("runtime_factory"));
				OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
				OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
				OutError.PropertyPath = TEXT("runtime_ready");
				OutError.SourceKind = TEXT("native_result");
			}
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
				OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("animation_blueprint_command_mechanics_handler_duplicate"),
					TEXT("More than one mechanics handler was created for the same Animation Blueprint command."), TEXT("handler_factory"), Handler->GetCommandId());
				OutError.ExpectedValue = MakeShared<FJsonValueNumber>(1);
				OutError.ActualValue = MakeShared<FJsonValueNumber>(2);
				OutError.PropertyPath = TEXT("command_handler_registration_count");
				OutError.SourceKind = TEXT("native_result");
				if (DomainAdapters::AnimationBlueprintCommands::GetPublicCommandIds().Contains(Handler->GetCommandId()))
					OutError.PublicMessage += TEXT(" Command: ") + Handler->GetCommandId() + TEXT(".");
				return false;
			}
			OutHandlers.Add(Handler->GetCommandId(), Handler);
		}
		if (OutHandlers.Num() != DomainAdapters::AnimationBlueprintCommands::GetCommandIds().Num()) {
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("animation_blueprint_command_mechanics_handler_count_mismatch"),
				TEXT("The Animation Blueprint mechanics factory must provide one handler for each required command."), TEXT("handler_factory"));
			OutError.ExpectedValue = MakeShared<FJsonValueNumber>(DomainAdapters::AnimationBlueprintCommands::GetCommandIds().Num());
			OutError.ActualValue = MakeShared<FJsonValueNumber>(OutHandlers.Num());
			OutError.PropertyPath = TEXT("command_handler_count");
			OutError.SourceKind = TEXT("native_result");
			return false;
		}
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


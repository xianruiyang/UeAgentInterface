#include "OperationOwners/AiCommands/UeAgentAiCommandOperationOwner.h"

#include "Composition/GraphCompound/UeAgentGraphCompoundCommandHandlers.h"
#include "Composition/GraphCompound/UeAgentGraphCompoundInternalExecution.h"
#include "Composition/GraphCompound/UeAgentGraphCompoundPcgAiBindings.h"
#include "DomainAdapters/AiCommands/UeAgentAiCommandMechanicsBoundary.h"
#include "Dom/JsonValue.h"
#include "Misc/Paths.h"

namespace UE::UAI::OperationOwners::AiCommands
{
	using namespace Composition::GraphCompound;
	using namespace CommandContract;
	using namespace CommandEdge;

	bool TryCreateMechanicsHandlers(
		TMap<FString, TSharedRef<CommandEdge::IUeAgentCommandHandler>>& OutHandlers,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutHandlers.Reset();
		OutError.Reset();
		TArray<FUeAgentGraphCompoundActivationProfile> Profiles;
		if (!DomainAdapters::AiCommands::TryGetActivationProfiles(Profiles, OutError))
		{
			return false;
		}
		FGraphCompoundRuntimeConfig RuntimeConfig;

		FGraphCompoundPcgAiProductionConfig ProductionConfig;
		ProductionConfig.DurableJournalRoot = FPaths::ProjectSavedDir()
			/ TEXT("UeAgentInterface/GraphCompound/AiCommands");
		TSharedPtr<FGraphCompoundRuntime> Runtime;
		FGraphCompoundRuntimeError RuntimeError;
		if (!TryCreatePcgAiGraphProductionRuntime(
				RuntimeConfig, ProductionConfig, Runtime, RuntimeError)
			|| !Runtime.IsValid())
		{
			OutError = RuntimeError;
			if (OutError.IsEmpty()) {
				OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("ai_command_mechanics_runtime_unavailable"),
					TEXT("The AI authoring runtime factory failed or returned no executable runtime without reporting a diagnostic."), TEXT("runtime_factory"));
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
		for (const auto& Handler : Mechanics)
		{
			if (OutHandlers.Contains(Handler->GetCommandId()))
			{
				OutHandlers.Reset();
				OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("ai_command_mechanics_handler_duplicate"),
					TEXT("More than one mechanics handler was created for the same AI authoring command."), TEXT("internal_command"), Handler->GetCommandId());
				OutError.ExpectedValue = MakeShared<FJsonValueNumber>(1);
				OutError.ActualValue = MakeShared<FJsonValueNumber>(2);
				OutError.PropertyPath = TEXT("command_handler_registration_count");
				OutError.SourceKind = TEXT("native_result");
				if (DomainAdapters::AiCommands::GetPublicCommandIds().Contains(Handler->GetCommandId()))
					OutError.PublicMessage += TEXT(" Command: ") + Handler->GetCommandId() + TEXT(".");
				return false;
			}
			OutHandlers.Add(Handler->GetCommandId(), Handler);
		}
		if (OutHandlers.Num() != DomainAdapters::AiCommands::GetCommandIds().Num()) {
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("ai_command_mechanics_handler_count_mismatch"),
				TEXT("The AI authoring mechanics factory must provide one handler for each required command."), TEXT("internal_command"));
			OutError.ExpectedValue = MakeShared<FJsonValueNumber>(DomainAdapters::AiCommands::GetCommandIds().Num());
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

	bool TryExecuteInternal(
		const FString& CommandId,
		const FString& ParamsJson,
		const Contract::FUeAgentCallerContext& Caller,
		FString& OutPayloadJson,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutPayloadJson.Reset();
		OutError.Reset();

		TArray<FUeAgentGraphCompoundActivationProfile> Profiles;
		if (!DomainAdapters::AiCommands::TryGetActivationProfiles(
				Profiles, OutError))
		{
			return false;
		}
		TMap<FString, TSharedRef<IUeAgentCommandHandler>> Mechanics;
		if (!TryCreateMechanicsHandlers(Mechanics, OutError))
		{
			return false;
		}
		return TryExecuteGraphCompoundInternal(
			Profiles, Mechanics, CommandId, ParamsJson, Caller,
			TEXT("ai_command_"), OutPayloadJson, OutError);
	}
}

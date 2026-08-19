#include "OperationOwners/BlueprintCommands/UeAgentBlueprintCommandOperationOwner.h"

#include "Composition/GraphCompound/UeAgentGraphCompoundBlueprintMaterialBindings.h"
#include "Composition/GraphCompound/UeAgentGraphCompoundCommandHandlers.h"
#include "DomainAdapters/BlueprintCommands/UeAgentBlueprintCommandMechanicsBoundary.h"
#include "Dom/JsonObject.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace UE::UAI::OperationOwners::BlueprintCommands
{
	using namespace Composition::GraphCompound;
	namespace
	{
		CommandEdge::FUeAgentCommandResult NormalizeFailure(
			const CommandEdge::FUeAgentCommandRequest& Request,
			CommandEdge::FUeAgentCommandResult Result)
		{
			if (Result.IsOk())
			{
				return Result;
			}
			TSharedPtr<FJsonObject> Error;
			const TSharedRef<TJsonReader<>> Reader =
				TJsonReaderFactory<>::Create(Result.GetErrorJson());
			if (!FJsonSerializer::Deserialize(Reader, Error)
				|| !Error.IsValid())
			{
				return Result;
			}
			FString Code;
			if (!Error->TryGetStringField(TEXT("code"), Code)
				|| Code != TEXT("graph_compound_blueprint_parent_invalid"))
			{
				return Result;
			}
			Error->SetStringField(TEXT("detail"),
				TEXT("parent_class_ref must resolve to a Blueprint-capable class."));
			FString ErrorJson;
			const auto Writer = TJsonWriterFactory<TCHAR,
				TCondensedJsonPrintPolicy<TCHAR>>::Create(&ErrorJson);
			FJsonSerializer::Serialize(Error.ToSharedRef(), Writer);
			return CommandEdge::FUeAgentCommandResult::Failure(
				Request, Result.GetPayloadJson(), MoveTemp(ErrorJson));
		}
	}

	bool TryCreateMechanicsHandlers(
		TMap<FString, TSharedRef<CommandEdge::IUeAgentCommandHandler>>& OutHandlers,
		FString& OutError)
	{
		OutHandlers.Reset();
		OutError.Reset();
		TArray<FUeAgentGraphCompoundActivationProfile> Profiles;
		if (!DomainAdapters::BlueprintCommands::TryGetActivationProfiles(
				Profiles, OutError))
		{
			return false;
		}
		FGraphCompoundRuntimeConfig RuntimeConfig;
		RuntimeConfig.MaxFrameBytes = 16 * 1024 * 1024;
		FGraphCompoundBlueprintMaterialProductionConfig ProductionConfig;
		ProductionConfig.DurableJournalRoot = FPaths::ProjectSavedDir()
			/ TEXT("UeAgentInterface/GraphCompound/BlueprintCommands");
		ProductionConfig.OperationTimeoutSeconds = 120.0;
		TSharedPtr<FGraphCompoundRuntime> Runtime;
		FGraphCompoundRuntimeError RuntimeError;
		if (!TryCreateBlueprintMaterialGraphProductionRuntime(
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
				OutError = TEXT("blueprint_command_mechanics_handler_duplicate");
				return false;
			}
			OutHandlers.Add(Handler->GetCommandId(), Handler);
		}
		return OutHandlers.Num()
			== DomainAdapters::BlueprintCommands::GetCommandIds().Num();
	}

	CommandEdge::FUeAgentCommandResult Submit(
		const TSharedRef<CommandEdge::IUeAgentCommandHandler>& Mechanics,
		const CommandEdge::FUeAgentCommandRequest& Request,
		const Contract::FUeAgentCallerContext& Caller,
		const TSharedRef<Contract::IUeAgentCancellationSignal>& Cancellation)
	{
		return NormalizeFailure(
			Request, Mechanics->Submit(Request, Caller, Cancellation));
	}
}

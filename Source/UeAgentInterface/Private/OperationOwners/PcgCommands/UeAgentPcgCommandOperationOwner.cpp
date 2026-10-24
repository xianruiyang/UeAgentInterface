#include "OperationOwners/PcgCommands/UeAgentPcgCommandOperationOwner.h"

#include "Composition/GraphCompound/UeAgentGraphCompoundCommandHandlers.h"
#include "Composition/GraphCompound/UeAgentGraphCompoundPcgAiBindings.h"
#include "Core/Contract/UeAgentContractJson.h"
#include "Core/Contract/UeAgentInternalCommandExecution.h"
#include "DomainAdapters/PcgCommands/UeAgentPcgCommandMechanicsBoundary.h"
#include "Dom/JsonValue.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"

namespace UE::UAI::OperationOwners::PcgCommands
{
	using namespace CommandEdge;
	using namespace Composition::GraphCompound;
	using namespace ContextResource;

	namespace
	{
		const FString PcgGraphAssetCreateCommandId =
			TEXT("pcg.graph.asset.create");
		const FString PcgGraphApplyCommandId = TEXT("pcg.graph.apply");
		const FString PcgGraphParametersApplyCommandId =
			TEXT("pcg.graph_parameters.apply");

		struct FProcessMechanicsHost final
		{
			FCriticalSection Mutex;
			TMap<FString, TSharedRef<IUeAgentCommandHandler>> Handlers;
			TArray<FString> AuthorityScopes;
		};

		FProcessMechanicsHost& ProcessMechanicsHost()
		{
			static FProcessMechanicsHost Host;
			return Host;
		}

		bool AddMechanics(
			const TSharedRef<IUeAgentCommandHandler>& Handler,
			TMap<FString, TSharedRef<IUeAgentCommandHandler>>& OutHandlers,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			if (!DomainAdapters::PcgCommands::IsCommand(Handler->GetCommandId()))
			{
				return true;
			}
			if (OutHandlers.Contains(Handler->GetCommandId()))
			{
				OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("pcg_command_mechanics_handler_duplicate"),
					TEXT("More than one mechanics handler was created for the same PCG command."), TEXT("internal_command"), Handler->GetCommandId());
				OutError.ExpectedValue = MakeShared<FJsonValueNumber>(1);
				OutError.ActualValue = MakeShared<FJsonValueNumber>(2);
				OutError.PropertyPath = TEXT("command_handler_registration_count");
				OutError.SourceKind = TEXT("native_result");
				if (DomainAdapters::PcgCommands::GetPublicCommandIds().Contains(Handler->GetCommandId()))
					OutError.PublicMessage += TEXT(" Command: ") + Handler->GetCommandId() + TEXT(".");
				return false;
			}
			OutHandlers.Add(Handler->GetCommandId(), Handler);
			return true;
		}

		bool TrySubmitPcgMechanic(
			const FString& CommandId,
			const TSharedRef<FJsonObject>& Params,
			const FString& CorrelationId,
			FString& OutPayload,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			OutPayload.Reset();
			OutError.Reset();
			TSharedPtr<IUeAgentCommandHandler> Handler;
			TArray<FString> AuthorityScopes;
			if (!TryResolveMechanics(
					CommandId, Handler, AuthorityScopes, OutError)
				|| !Handler.IsValid())
			{
				if (OutError.IsEmpty()) {
					OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("pcg_command_mechanics_handler_unavailable"),
						TEXT("The PCG mechanics resolver failed or returned no executable handler without reporting a diagnostic."), TEXT("internal_command"), CommandId);
					OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
					OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
					OutError.PropertyPath = TEXT("command_handler_ready");
					OutError.SourceKind = TEXT("native_result");
					if (DomainAdapters::PcgCommands::GetPublicCommandIds().Contains(CommandId))
						OutError.PublicMessage += TEXT(" Command: ") + CommandId + TEXT(".");
				}
				return false;
			}

			FString ParamsJson;
			if (!Contract::Json::TryCanonicalize(Params, ParamsJson, OutError))
			{
				OutError = UE::UAI::Contract::FDiagnostic::WrapFailure(TEXT("pcg_internal_params_invalid"),
					OutError, TEXT("internal_command"), TEXT("The PCG command parameters could not be encoded."));
				return false;
			}
			TSharedPtr<const CommandContract::FUeAgentCommandContractRegistry>
				Contracts;
			TArray<FUeAgentGraphCompoundActivationProfile> Profiles;
			if (!DomainAdapters::PcgCommands::TryGetGraphProfiles(
					Profiles, OutError)
				|| !TryCreateGraphCompoundProductionCommandContractRegistry(
					Profiles, Contracts, OutError))
			{
				return false;
			}
			const CommandContract::FUeAgentCommandContract* ResolvedContract =
				Contracts.IsValid() ? Contracts->FindExact(CommandId) : nullptr;
			if (!ResolvedContract)
			{
				OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("pcg_internal_contract_missing"),
					Contracts.IsValid() ? TEXT("No command contract is registered for the required PCG operation.")
						: TEXT("The PCG contract factory returned no command registry."), TEXT("internal_command"), CommandId);
				OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
				OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
				OutError.PropertyPath = Contracts.IsValid() ? TEXT("command_contract_registered") : TEXT("command_registry_available");
				OutError.SourceKind = TEXT("native_result");
				if (DomainAdapters::PcgCommands::GetPublicCommandIds().Contains(CommandId))
					OutError.PublicMessage += TEXT(" Command: ") + CommandId + TEXT(".");
				return false;
			}
			AuthorityScopes.AddUnique(TEXT("uai:query"));
			AuthorityScopes.AddUnique(TEXT("uai:operate"));
			AuthorityScopes.AddUnique(TEXT("uai:authoring:hjson:import"));
			if (!CommandContract::TryExecuteInternalCommand(
					*ResolvedContract,
					Handler.ToSharedRef(),
					ParamsJson,
					Contract::FUeAgentCallerContext(
						TEXT("pcg-domain-provider"), AuthorityScopes,
						CorrelationId),
					TEXT("pcg_internal_"),
					OutPayload,
					OutError))
			{
				return false;
			}
			return true;
		}
	}

	bool TryCreateMechanicsHandlers(
		TMap<FString, TSharedRef<IUeAgentCommandHandler>>& OutHandlers,
		TArray<FString>& OutAuthorityScopes,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutHandlers.Reset();
		OutAuthorityScopes.Reset();
		OutError.Reset();

		TArray<FUeAgentGraphCompoundActivationProfile> GraphProfiles;
		if (!DomainAdapters::PcgCommands::TryGetGraphProfiles(
				GraphProfiles, OutError))
		{
			return false;
		}
		FGraphCompoundRuntimeConfig GraphConfig;

		FGraphCompoundPcgAiProductionConfig PcgConfig;
		PcgConfig.DurableJournalRoot = FPaths::ProjectSavedDir()
			/ TEXT("UeAgentInterface/GraphCompound/PcgCommands");
		TSharedPtr<FGraphCompoundRuntime> GraphRuntime;
		FGraphCompoundRuntimeError GraphError;
		if (!TryCreatePcgAiGraphProductionRuntime(
				GraphConfig, PcgConfig, GraphRuntime, GraphError)
			|| !GraphRuntime.IsValid())
		{
			OutError = GraphError;
			if (OutError.IsEmpty()) {
				OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("pcg_command_mechanics_runtime_unavailable"),
					TEXT("The PCG runtime factory failed or returned no executable runtime without reporting a diagnostic."), TEXT("runtime_factory"));
				OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
				OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
				OutError.PropertyPath = TEXT("runtime_ready");
				OutError.SourceKind = TEXT("native_result");
			}
			return false;
		}
		TArray<TSharedRef<IUeAgentCommandHandler>> GraphHandlers;
		if (!TryCreateGraphCompoundCommandHandlers(
				GraphProfiles,
				GraphRuntime->GetQueryPort(),
				GraphRuntime->GetOperationPort(),
				GraphHandlers,
				OutError))
		{
			return false;
		}
		for (const TSharedRef<IUeAgentCommandHandler>& Handler : GraphHandlers)
		{
			if (!AddMechanics(Handler, OutHandlers, OutError))
			{
				OutHandlers.Reset();
				return false;
			}
		}
		OutAuthorityScopes.Add(TEXT("pcg_ai_node_graph.write"));

		if (OutHandlers.Num() != DomainAdapters::PcgCommands::GetCommandIds().Num())
		{
			const int32 HandlerCount = OutHandlers.Num();
			OutHandlers.Reset();
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("pcg_command_mechanics_handler_count_mismatch"),
				TEXT("The PCG mechanics factory must provide one handler for each required command."), TEXT("internal_command"));
			OutError.ExpectedValue = MakeShared<FJsonValueNumber>(DomainAdapters::PcgCommands::GetCommandIds().Num());
			OutError.ActualValue = MakeShared<FJsonValueNumber>(HandlerCount);
			OutError.PropertyPath = TEXT("command_handler_count");
			OutError.SourceKind = TEXT("native_result");
			return false;
		}
		return true;
	}

	bool TryResolveMechanics(
		const FString& CommandId,
		TSharedPtr<IUeAgentCommandHandler>& OutHandler,
		TArray<FString>& OutAuthorityScopes,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutHandler.Reset();
		OutAuthorityScopes.Reset();
		OutError.Reset();
		FProcessMechanicsHost& Host = ProcessMechanicsHost();
		FScopeLock Lock(&Host.Mutex);
		if (Host.Handlers.IsEmpty()
			&& !TryCreateMechanicsHandlers(
				Host.Handlers, Host.AuthorityScopes, OutError))
		{
			return false;
		}
		const TSharedRef<IUeAgentCommandHandler>* Found =
			Host.Handlers.Find(CommandId);
		if (!Found)
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("pcg_command_mechanics_handler_missing"),
				TEXT("No mechanics handler is registered for the required PCG command."), TEXT("internal_command"), CommandId);
			OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
			OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
			OutError.PropertyPath = TEXT("command_handler_registered");
			OutError.SourceKind = TEXT("native_result");
			if (DomainAdapters::PcgCommands::GetPublicCommandIds().Contains(CommandId))
				OutError.PublicMessage += TEXT(" Command: ") + CommandId + TEXT(".");
			return false;
		}
		OutHandler = *Found;
		OutAuthorityScopes = Host.AuthorityScopes;
		return true;
	}

	FUeAgentCommandResult Submit(
		const TSharedRef<IUeAgentCommandHandler>& Mechanics,
		const FUeAgentCommandRequest& Request,
		const Contract::FUeAgentCallerContext& Caller,
		const TSharedRef<Contract::IUeAgentCancellationSignal>& Cancellation)
	{
		return Mechanics->Submit(Request, Caller, Cancellation);
	}

	bool TryCreatePcgGraphAsset(
		const TSharedRef<FJsonObject>& Params,
		const FString& CorrelationId,
		FString& OutPayload,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		return TrySubmitPcgMechanic(
			PcgGraphAssetCreateCommandId,
			Params,
			CorrelationId,
			OutPayload,
			OutError);
	}

	bool TryApplyPcgGraph(
		const TSharedRef<FJsonObject>& Params,
		const FString& CorrelationId,
		FString& OutPayload,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		return TrySubmitPcgMechanic(
			PcgGraphApplyCommandId,
			Params,
			CorrelationId,
			OutPayload,
			OutError);
	}

	bool TryApplyPcgGraphParameters(
		const TSharedRef<FJsonObject>& Params,
		const FString& CorrelationId,
		FString& OutPayload,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		return TrySubmitPcgMechanic(
			PcgGraphParametersApplyCommandId,
			Params,
			CorrelationId,
			OutPayload,
			OutError);
	}
}

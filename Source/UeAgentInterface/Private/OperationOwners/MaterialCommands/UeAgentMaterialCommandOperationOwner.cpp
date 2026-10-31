#include "OperationOwners/MaterialCommands/UeAgentMaterialCommandOperationOwner.h"

#include "Composition/GraphCompound/UeAgentGraphCompoundBlueprintMaterialBindings.h"
#include "Composition/GraphCompound/UeAgentGraphCompoundCommandHandlers.h"
#include "Composition/GraphCompound/UeAgentGraphCompoundInternalExecution.h"
#include "Core/DomainCapability/DelegatedCommandOperation/UeAgentDelegatedCommandOperation.h"
#include "DomainAdapters/MaterialCommands/UeAgentMaterialCommandMechanicsBoundary.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "Interfaces/IPluginManager.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace UE::UAI::OperationOwners::MaterialCommands
{
	using namespace BindingGateway::Operation;
	using namespace CommandEdge;
	using namespace Composition::GraphCompound;

	namespace
	{
		struct FProcessHost
		{
			FCriticalSection Mutex;
			TSharedPtr<FGraphCompoundRuntime> Runtime;
			TMap<FString, TSharedRef<IUeAgentCommandHandler>> Handlers;
		};

		FProcessHost& ProcessHost()
		{
			static FProcessHost Host;
			return Host;
		}

		bool TryInitializeLocked(FProcessHost& Host, UE::UAI::Contract::FDiagnostic& OutError)
		{
			OutError.Reset();
			if (Host.Runtime.IsValid()
				&& Host.Handlers.Num()
					== DomainAdapters::MaterialCommands::GetCommandIds().Num())
			{
				return true;
			}
			Host.Runtime.Reset();
			Host.Handlers.Reset();
			TArray<FUeAgentGraphCompoundActivationProfile> Profiles;
			if (!DomainAdapters::MaterialCommands::TryGetActivationProfiles(
					Profiles, OutError))
			{
				return false;
			}
			FGraphCompoundRuntimeConfig RuntimeConfig;

			FGraphCompoundBlueprintMaterialProductionConfig ProductionConfig;
			ProductionConfig.DurableJournalRoot = FPaths::ProjectSavedDir()
				/ TEXT("UeAgentInterface/GraphCompound/MaterialCommands");
			if (const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("UeAgentInterface")))
			{
				ProductionConfig.AdditionalDurableContentRoots.Add(Plugin->GetContentDir() / TEXT("Materials"));
			}
			FGraphCompoundRuntimeError RuntimeError;
			if (!TryCreateBlueprintMaterialGraphProductionRuntime(
					RuntimeConfig,
					ProductionConfig,
					Host.Runtime,
					RuntimeError)
				|| !Host.Runtime.IsValid())
			{
				Host.Runtime.Reset();
				OutError = RuntimeError;
				if (OutError.IsEmpty()) {
					OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("material_command_mechanics_runtime_unavailable"),
						TEXT("The material runtime factory failed or returned no executable runtime without reporting a diagnostic."), TEXT("runtime_factory"));
					OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
					OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
					OutError.PropertyPath = TEXT("runtime_ready");
					OutError.SourceKind = TEXT("native_result");
				}
				return false;
			}
			TArray<TSharedRef<IUeAgentCommandHandler>> Mechanics;
			if (!TryCreateGraphCompoundCommandHandlers(
					Profiles,
					Host.Runtime->GetQueryPort(),
					Host.Runtime->GetOperationPort(),
					Mechanics,
					OutError))
			{
				Host.Runtime.Reset();
				return false;
			}
			for (const TSharedRef<IUeAgentCommandHandler>& Handler : Mechanics)
			{
				if (Host.Handlers.Contains(Handler->GetCommandId()))
				{
					Host.Handlers.Reset();
					Host.Runtime.Reset();
					OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("material_command_mechanics_handler_duplicate"),
						TEXT("More than one mechanics handler was created for the same material command."), TEXT("handler_factory"), Handler->GetCommandId());
					OutError.ExpectedValue = MakeShared<FJsonValueNumber>(1);
					OutError.ActualValue = MakeShared<FJsonValueNumber>(2);
					OutError.PropertyPath = TEXT("command_handler_registration_count");
					OutError.SourceKind = TEXT("native_result");
					if (DomainAdapters::MaterialCommands::GetPublicCommandIds().Contains(Handler->GetCommandId()))
						OutError.PublicMessage += TEXT(" Command: ") + Handler->GetCommandId() + TEXT(".");
					return false;
				}
				Host.Handlers.Add(Handler->GetCommandId(), Handler);
			}
			if (Host.Handlers.Num()
				!= DomainAdapters::MaterialCommands::GetCommandIds().Num())
			{
				const int32 HandlerCount = Host.Handlers.Num();
				Host.Handlers.Reset();
				Host.Runtime.Reset();
				OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("material_command_mechanics_handler_count_mismatch"),
					TEXT("The material mechanics factory must provide one handler for each required command."), TEXT("internal_command"));
				OutError.ExpectedValue = MakeShared<FJsonValueNumber>(DomainAdapters::MaterialCommands::GetCommandIds().Num());
				OutError.ActualValue = MakeShared<FJsonValueNumber>(HandlerCount);
				OutError.PropertyPath = TEXT("command_handler_count");
				OutError.SourceKind = TEXT("native_result");
				return false;
			}
			return true;
		}

		UE::UAI::Contract::FDiagnostic CommandFailure(FString Code, const FString& Subject, FString Reason,
			const UE::UAI::Contract::FDiagnostic& Cause = UE::UAI::Contract::FDiagnostic())
		{
			auto Diagnostic = Cause.IsEmpty()
				? UE::UAI::Contract::FDiagnostic::Failure(MoveTemp(Code), MoveTemp(Reason), TEXT("command"))
				: UE::UAI::Contract::FDiagnostic::WrapFailure(MoveTemp(Code), Cause, TEXT("command"), MoveTemp(Reason));
			Diagnostic.SubjectId = Subject;
			if (Cause.IsEmpty()) Diagnostic.SourceKind = TEXT("native_result");
			if (DomainAdapters::MaterialCommands::GetPublicCommandIds().Contains(Subject))
				Diagnostic.PublicMessage += TEXT(" Command: ") + Subject + TEXT(".");
			Diagnostic.Schema = TEXT("uai.runtime.diagnostic/1");
			Diagnostic.TerminalStatus = TEXT("not_started");
			Diagnostic.bRetrySafe = true;
			Diagnostic.MutationStarted = false;
			Diagnostic.MutationMayHaveStarted = false;
			return Diagnostic;
		}

		UE::UAI::Contract::FDiagnostic InvocationFailure(const FString& Code, const FString& Subject, const UE::UAI::Contract::FDiagnostic& Cause)
		{
			auto Diagnostic = CommandFailure(Code, Subject, TEXT("The operation runtime could not be prepared."), Cause);
			if (Cause.IsEmpty()) {
				Diagnostic.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
				Diagnostic.ActualValue = MakeShared<FJsonValueBoolean>(false);
				Diagnostic.PropertyPath = TEXT("operation_runtime_prepared");
			}
			return Diagnostic;
		}

		UE::UAI::Contract::FDiagnostic ProjectionFailure(const FString& Code, const UE::UAI::Contract::FDiagnostic& Cause,
			const BindingGateway::Operation::FOperationLifecycleReceipt& Receipt)
		{
			auto Diagnostic = Cause.IsEmpty()
				? UE::UAI::Contract::FDiagnostic::Failure(Code, TEXT("The operation completed, but its result payload could not be produced."), TEXT("result_projection"))
				: UE::UAI::Contract::FDiagnostic::WrapFailure(Code, Cause, TEXT("result_projection"), TEXT("The operation completed, but its result payload could not be produced."));
			Diagnostic.Schema = TEXT("uai.runtime.diagnostic/1");
			if (Cause.IsEmpty()) {
				Diagnostic.SourceKind = TEXT("native_result");
				Diagnostic.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
				Diagnostic.ActualValue = MakeShared<FJsonValueBoolean>(false);
				Diagnostic.PropertyPath = TEXT("result_payload_produced");
			}
			Diagnostic.TerminalStatus = BindingGateway::Operation::LexToString(Receipt.GetStatus());
			Diagnostic.MutationStarted = Receipt.DidMutationStart();
			Diagnostic.MutationMayHaveStarted = Receipt.DidMutationStart();
			Diagnostic.bRetrySafe = Receipt.IsRetrySafe();
			return Diagnostic;
		}
	}

	bool TryCreateMechanicsHandlers(
		TMap<FString, TSharedRef<IUeAgentCommandHandler>>& OutHandlers,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutHandlers.Reset();
		FProcessHost& Host = ProcessHost();
		FScopeLock Lock(&Host.Mutex);
		if (!TryInitializeLocked(Host, OutError))
		{
			return false;
		}
		OutHandlers = Host.Handlers;
		return true;
	}

	bool TryResolveMechanicsHandler(
		const FString& CommandId,
		TSharedPtr<IUeAgentCommandHandler>& OutHandler,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutHandler.Reset();
		FProcessHost& Host = ProcessHost();
		FScopeLock Lock(&Host.Mutex);
		if (!TryInitializeLocked(Host, OutError))
		{
			return false;
		}
		const TSharedRef<IUeAgentCommandHandler>* Found =
			Host.Handlers.Find(CommandId);
		if (!Found)
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("material_command_mechanics_handler_missing"),
				TEXT("No mechanics handler is registered for the required material command."), TEXT("internal_command"), CommandId);
			OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
			OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
			OutError.PropertyPath = TEXT("command_handler_registered");
			OutError.SourceKind = TEXT("native_result");
			if (DomainAdapters::MaterialCommands::GetPublicCommandIds().Contains(CommandId))
				OutError.PublicMessage += TEXT(" Command: ") + CommandId + TEXT(".");
			return false;
		}
		OutHandler = *Found;
		return true;
	}

	bool TryResolveOperationMechanics(
		const FString& CommandId,
		TSharedPtr<IUeAgentCommandHandler>& OutHandler,
		TArray<FString>& OutAuthorityScopes,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutAuthorityScopes = {
			TEXT("material_niagara_graph.read"),
			TEXT("material_niagara_graph.write"),
		};
		return TryResolveMechanicsHandler(CommandId, OutHandler, OutError);
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
		if (!DomainAdapters::MaterialCommands::TryGetActivationProfiles(
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
			TEXT("material_command_"), OutPayloadJson,
			OutError);
	}

	TSharedPtr<const DomainCapability::IDomainOperationInvocationFactory>
	FindOperationInvocationFactory(const FString& OperationId)
	{
		if (!DomainAdapters::MaterialCommands::GetCommandIds().Contains(
				OperationId)
			|| DomainAdapters::MaterialCommands::IsQuery(OperationId))
		{
			return nullptr;
		}
		static const TMap<FString, TSharedPtr<const DomainCapability::
			IDomainOperationInvocationFactory>> Factories = []()
		{
			TMap<FString, TSharedPtr<const DomainCapability::
				IDomainOperationInvocationFactory>> Result;
			for (const FString& CommandId
				: DomainAdapters::MaterialCommands::GetCommandIds())
			{
				Result.Add(
					CommandId,
					DomainCapability::DelegatedCommandOperation::
						CreateInvocationFactory(
							CommandId,
							&TryResolveOperationMechanics,
							true,
							true));
			}
			return Result;
		}();
		return Factories.FindRef(OperationId);
	}

	FUeAgentCommandResult Submit(
		const FUeAgentCommandRequest& Request,
		const Contract::FUeAgentCallerContext& Caller,
		const TSharedRef<Contract::IUeAgentCancellationSignal>& Cancellation)
	{
		const TSharedPtr<const DomainCapability::
			IDomainOperationInvocationFactory> Factory =
				FindOperationInvocationFactory(Request.GetCommandId());
		if (!Factory.IsValid())
		{
			auto Diagnostic = CommandFailure(TEXT("material_command_operation_factory_missing"), Request.GetCommandId(),
				TEXT("The exact material operation has no lifecycle factory."));
			Diagnostic.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
			Diagnostic.ActualValue = MakeShared<FJsonValueBoolean>(false);
			Diagnostic.PropertyPath = TEXT("operation_factory_registered");
			return FUeAgentCommandResult::Failure(
				Request,
				TEXT("{}"),
				MoveTemp(Diagnostic));
		}
		DomainCapability::FDomainOperationInvocation Invocation;
		DomainCapability::FDomainOperationInvocationError InvocationError;
		if (!Factory->TryCreate(
				Request, Caller, Cancellation, Invocation, InvocationError))
		{
			return FUeAgentCommandResult::Failure(
				Request,
				TEXT("{}"),
				InvocationFailure(TEXT("material_command_runtime_unavailable"), Request.GetCommandId(), InvocationError));
		}
		FOperationLifecycleReceipt Receipt;
		if (!Invocation.GetService()->Run(
				*Invocation.GetPlan(), *Invocation.GetAuthorization(), Receipt))
		{
			auto Diagnostic = Receipt.GetDiagnostic();
			if (Diagnostic.SubjectId.IsEmpty()) Diagnostic.SubjectId = Request.GetCommandId();
			return FUeAgentCommandResult::Failure(Request, TEXT("{}"), MoveTemp(Diagnostic));
		}
		FString Payload;
		UE::UAI::Contract::FDiagnostic PayloadError;
		if (!Invocation.GetContext()->TryBuildSuccessPayload(
				Receipt, Payload, PayloadError))
		{
			return FUeAgentCommandResult::ProtocolError(Request.GetCorrelationId(), Request.GetRequestId(), Request.GetCommandId(), Request.GetCommandBindingHash(), ProjectionFailure(TEXT("material_command_operation_result_invalid"), PayloadError, Receipt));
		}
		return FUeAgentCommandResult::Success(Request, MoveTemp(Payload));
	}
}



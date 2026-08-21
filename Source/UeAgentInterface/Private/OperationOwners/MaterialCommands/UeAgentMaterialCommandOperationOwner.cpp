#include "OperationOwners/MaterialCommands/UeAgentMaterialCommandOperationOwner.h"

#include "Composition/GraphCompound/UeAgentGraphCompoundBlueprintMaterialBindings.h"
#include "Composition/GraphCompound/UeAgentGraphCompoundCommandHandlers.h"
#include "Core/DomainCapability/DelegatedCommandOperation/UeAgentDelegatedCommandOperation.h"
#include "DomainAdapters/MaterialCommands/UeAgentMaterialCommandMechanicsBoundary.h"
#include "Dom/JsonObject.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
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

		bool TryInitializeLocked(FProcessHost& Host, FString& OutError)
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
			RuntimeConfig.MaxFrameBytes = 16 * 1024 * 1024;
			FGraphCompoundBlueprintMaterialProductionConfig ProductionConfig;
			ProductionConfig.DurableJournalRoot = FPaths::ProjectSavedDir()
				/ TEXT("UeAgentInterface/GraphCompound/MaterialCommands");
			ProductionConfig.OperationTimeoutSeconds = 120.0;
			FGraphCompoundRuntimeError RuntimeError;
			if (!TryCreateBlueprintMaterialGraphProductionRuntime(
					RuntimeConfig,
					ProductionConfig,
					Host.Runtime,
					RuntimeError)
				|| !Host.Runtime.IsValid())
			{
				Host.Runtime.Reset();
				OutError = RuntimeError.Code + TEXT(":") + RuntimeError.Detail;
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
					OutError =
						TEXT("material_command_mechanics_handler_duplicate");
					return false;
				}
				Host.Handlers.Add(Handler->GetCommandId(), Handler);
			}
			if (Host.Handlers.Num()
				!= DomainAdapters::MaterialCommands::GetCommandIds().Num())
			{
				Host.Handlers.Reset();
				Host.Runtime.Reset();
				OutError = TEXT("material_command_mechanics_handler_count_mismatch");
				return false;
			}
			return true;
		}

		FString ErrorJson(
			const FString& Code,
			const FString& Subject,
			const FString& Detail,
			const FString& TerminalStatus = TEXT("not_started"),
			const bool bRetrySafe = true)
		{
			const TSharedRef<FJsonObject> Error = MakeShared<FJsonObject>();
			Error->SetStringField(TEXT("code"), Code);
			Error->SetStringField(TEXT("subject_id"), Subject);
			Error->SetStringField(TEXT("detail"), Detail);
			Error->SetStringField(TEXT("terminal_status"), TerminalStatus);
			Error->SetBoolField(TEXT("retry_safe"), bRetrySafe);
			FString Text;
			const TSharedRef<TJsonWriter<TCHAR,
				TCondensedJsonPrintPolicy<TCHAR>>> Writer =
					TJsonWriterFactory<TCHAR,
						TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
			FJsonSerializer::Serialize(Error, Writer);
			return Text;
		}
	}

	bool TryCreateMechanicsHandlers(
		TMap<FString, TSharedRef<IUeAgentCommandHandler>>& OutHandlers,
		FString& OutError)
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
		FString& OutError)
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
			OutError = TEXT("material_command_mechanics_handler_missing:")
				+ CommandId;
			return false;
		}
		OutHandler = *Found;
		return true;
	}

	bool TryResolveOperationMechanics(
		const FString& CommandId,
		TSharedPtr<IUeAgentCommandHandler>& OutHandler,
		TArray<FString>& OutAuthorityScopes,
		FString& OutError)
	{
		OutAuthorityScopes = {
			TEXT("material_niagara_graph.read"),
			TEXT("material_niagara_graph.write"),
		};
		return TryResolveMechanicsHandler(CommandId, OutHandler, OutError);
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
			return FUeAgentCommandResult::Failure(
				Request,
				TEXT("{}"),
				ErrorJson(
					TEXT("material_command_operation_factory_missing"),
					Request.GetCommandId(),
					TEXT("the exact material operation has no lifecycle factory")));
		}
		DomainCapability::FDomainOperationInvocation Invocation;
		DomainCapability::FDomainOperationInvocationError InvocationError;
		if (!Factory->TryCreate(
				Request, Caller, Cancellation, Invocation, InvocationError))
		{
			return FUeAgentCommandResult::Failure(
				Request,
				TEXT("{}"),
				ErrorJson(
					TEXT("material_command_runtime_unavailable"),
					Request.GetCommandId(),
					InvocationError.GetCode() + TEXT(":")
						+ InvocationError.GetDetail()));
		}
		FOperationLifecycleReceipt Receipt;
		if (!Invocation.GetService()->Run(
				*Invocation.GetPlan(), *Invocation.GetAuthorization(), Receipt))
		{
			FString Detail = Invocation.GetContext()->GetFailureDetail();
			if (Detail.IsEmpty())
			{
				Detail = TEXT("phase_history:")
					+ FString::Join(Receipt.GetPhaseHistory(), TEXT(","));
			}
			return FUeAgentCommandResult::Failure(
				Request,
				TEXT("{}"),
				ErrorJson(
					Receipt.GetErrorCode(),
					Request.GetCommandId(),
					Detail,
					LexToString(Receipt.GetStatus()),
					Receipt.IsRetrySafe()));
		}
		FString Payload;
		FString PayloadError;
		if (!Invocation.GetContext()->TryBuildSuccessPayload(
				Receipt, Payload, PayloadError))
		{
			return FUeAgentCommandResult::ProtocolError(
				Request.GetCorrelationId(),
				Request.GetRequestId(),
				Request.GetCommandId(),
				Request.GetCommandBindingHash(),
				TEXT("material_command_operation_result_invalid"),
				PayloadError);
		}
		return FUeAgentCommandResult::Success(Request, MoveTemp(Payload));
	}
}



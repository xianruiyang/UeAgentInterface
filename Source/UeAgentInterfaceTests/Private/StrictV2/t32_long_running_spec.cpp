#include "Misc/AutomationTest.h"

#include "CommandEdge/UeAgentCommandProtocol.h"
#include "Core/Contract/UeAgentContract.h"
#include "Editor.h"
#include "HAL/IConsoleManager.h"
#include "Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h"
#include "Module/CommandRegistration/LongRunning/UeAgentLongRunningLifecycleRegistration.h"
#include "OperationOwners/LongRunning/UeAgentLongRunningLifecycleOwner.h"

namespace UE::UAI::Tests::StrictV2::T32
{
	using namespace CommandRegistry;
	using namespace Module::CommandRegistration::Active;

	const TArray<FString>& ExpectedLifecycleCommands()
	{
		static const TArray<FString> Commands = {
			TEXT("operation.status"),
			TEXT("operation.receipt"),
			TEXT("operation.cancel"),
			TEXT("operation.resume"),
			TEXT("operation.reconcile"),
			TEXT("operation.release"),
		};
		return Commands;
	}

	class FNeverCancelled final : public Contract::IUeAgentCancellationSignal
	{
	public:
		virtual bool IsCancelled() const override { return false; }
	};

	class FRecordingOperationPort final
		: public Contract::IUeAgentOperationApplicationPort
	{
	public:
		virtual Contract::FUeAgentOperationResult Submit(
			const Contract::FUeAgentOperationRequest& Request,
			const Contract::FUeAgentCallerContext& CallerContext,
			const TSharedRef<Contract::IUeAgentCancellationSignal>& Cancellation)
			override
		{
			++Calls;
			Payloads.Add(Request.GetPayload().GetJsonValue());
			const FString ResultJson = bReturnUnknown
				? FString(TEXT("{\"operation_error\":{"))
					+ TEXT("\"code\":\"operation_not_found\",")
					+ TEXT("\"detail\":\"the operation is not retained\",")
					+ TEXT("\"subject_id\":\"operation:unknown\"}}")
				: FString(TEXT("{\"lifecycle_status\":\"running\"}"));
			return Contract::FUeAgentOperationResult(
				Request.GetDescriptorIdentity(),
				Request.GetRequestId(),
				Contract::FUeAgentContractValue(ResultJson));
		}

		int32 Calls = 0;
		bool bReturnUnknown = false;
		TArray<FString> Payloads;
	};

	bool DecodeRequest(
		const FString& CommandId,
		const FString& ContractHash,
		const FString& RequestId,
		const FString& ParamsJson,
		TOptional<CommandEdge::FUeAgentCommandRequest>& OutRequest)
	{
		const FString Envelope = FString::Printf(
			TEXT("{\"schema\":\"uai.command.v1\",\"version\":1,")
			TEXT("\"message_kind\":\"command_request\",")
			TEXT("\"correlation_id\":\"corr-t32\",")
			TEXT("\"request_id\":\"%s\",\"command_id\":\"%s\",")
			TEXT("\"command_binding_hash\":\"%s\",\"params\":%s}"),
			*RequestId,
			*CommandId,
			*ContractHash,
			*ParamsJson);
		FTCHARToUTF8 Encoded(*Envelope);
		TArray<uint8> Bytes;
		Bytes.Append(
			reinterpret_cast<const uint8*>(Encoded.Get()),
			Encoded.Length());
		CommandEdge::FUeAgentCommandProtocolDiagnostic Diagnostic;
		return CommandEdge::FUeAgentCommandProtocolCodec().DecodeRequest(
			Bytes,
			OutRequest,
			Diagnostic);
	}

	FString ParamsFor(const FString& CommandId)
	{
		const FString OperationRef =
			TEXT("{\"operation_id\":\"operation:fixture\",")
			TEXT("\"owner_lease_id\":\"lease:fixture\",")
			TEXT("\"server_session_id\":\"server-session:fixture\"}");
		if (CommandId == TEXT("operation.cancel"))
		{
			return TEXT("{\"operation_ref\":") + OperationRef
				+ TEXT(",\"reason\":\"user_cancelled\"}");
		}
		if (CommandId == TEXT("operation.resume")
			|| CommandId == TEXT("operation.reconcile"))
		{
			return TEXT("{\"expected_generation\":7,\"operation_ref\":")
				+ OperationRef + TEXT("}");
		}
		return TEXT("{\"operation_ref\":") + OperationRef + TEXT("}");
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T32InventoryAndActivationTest,
	"UAI.StrictV2.T32.LONG.RUNNING.InventoryAndActivation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T32InventoryAndActivationTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI;
	using namespace UE::UAI::Tests::StrictV2::T32;
	TSharedPtr<const FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
	FString Error;
	if (!TestTrue(
			TEXT("the process active composition freezes successfully"),
			TryComposeProcessActiveCommandComposition(Registry, Dispatch, Error))
		|| !Registry.IsValid())
	{
		AddError(Error);
		return false;
	}

	for (const FString& CommandId : ExpectedLifecycleCommands())
	{
		const CommandContract::FUeAgentFrozenCommandContract* Contract =
			Registry->FindExact(CommandId);
		TestNotNull(*FString::Printf(TEXT("%s is active"), *CommandId), Contract);
		if (Contract)
		{
			TestEqual(
				*FString::Printf(TEXT("%s has one T32 owner"), *CommandId),
				Contract->GetPublicOwner(),
				FString(TEXT("long-running-session-owner")));
		}
	}

	using namespace UE::UAI::Module::CommandRegistration::LongRunning;
	const TSharedRef<FRecordingOperationPort> RecordingPort =
		MakeShared<FRecordingOperationPort>();
	TSharedPtr<const FUeAgentImmutableCommandRegistry> IsolatedRegistry;
	TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> IsolatedDispatch;
	TestTrue(
		TEXT("the six public lifecycle routes compose around one operation port"),
		TryCreateLongRunningLifecycleRegistration(
			RecordingPort,
			IsolatedRegistry,
			IsolatedDispatch,
			Error));
	if (!IsolatedRegistry.IsValid() || !IsolatedDispatch.IsValid())
	{
		AddError(Error);
		return false;
	}

	int32 RequestIndex = 0;
	for (const FString& CommandId : ExpectedLifecycleCommands())
	{
		const CommandContract::FUeAgentFrozenCommandContract* Contract =
			IsolatedRegistry->FindExact(CommandId);
		if (!TestNotNull(TEXT("the isolated exact contract exists"), Contract))
		{
			continue;
		}
		const FString Params = ParamsFor(CommandId);
		TOptional<CommandEdge::FUeAgentCommandRequest> Request;
		TestTrue(
			TEXT("the public lifecycle request decodes exactly"),
			DecodeRequest(
				CommandId,
				Contract->GetContractHash(),
				FString::Printf(TEXT("req-t32-%d"), RequestIndex++),
				Params,
				Request));
		if (!Request.IsSet())
		{
			continue;
		}
		const CommandEdge::FUeAgentCommandResult Result =
			IsolatedDispatch->Submit(
				Request.GetValue(),
				Contract::FUeAgentCallerContext(
					TEXT("strict-v2-t32"),
					{TEXT("uai:operate")},
					TEXT("corr-t32")),
				MakeShared<FNeverCancelled>());
		TestTrue(TEXT("one exact lifecycle route delegates successfully"), Result.IsOk());
		TestEqual(
			TEXT("canonical params reach the single operation owner unchanged"),
			RecordingPort->Payloads.Last(),
			Params);
	}
	TestEqual(
		TEXT("six lifecycle requests produce exactly six owner calls"),
		RecordingPort->Calls,
		6);

	const CommandContract::FUeAgentFrozenCommandContract* StatusContract =
		IsolatedRegistry->FindExact(TEXT("operation.status"));
	TOptional<CommandEdge::FUeAgentCommandRequest> UnknownRequest;
	RecordingPort->bReturnUnknown = true;
	if (StatusContract)
	{
		DecodeRequest(
			TEXT("operation.status"),
			StatusContract->GetContractHash(),
			TEXT("req-t32-unknown"),
			ParamsFor(TEXT("operation.status")),
			UnknownRequest);
	}
	if (TestTrue(TEXT("the unknown-operation request decodes"), UnknownRequest.IsSet()))
	{
		const int32 CallsBeforeUnknown = RecordingPort->Calls;
		const CommandEdge::FUeAgentCommandResult Unknown =
			IsolatedDispatch->Submit(
				UnknownRequest.GetValue(),
				Contract::FUeAgentCallerContext(
					TEXT("strict-v2-t32"),
					{TEXT("uai:operate")},
					TEXT("corr-t32")),
				MakeShared<FNeverCancelled>());
		TestTrue(
			TEXT("unknown retained state remains a typed business failure"),
			!Unknown.IsOk() && !Unknown.IsProtocolError());
		TestTrue(
			TEXT("unknown retained state preserves its stable code"),
			Unknown.GetErrorJson().Contains(TEXT("operation_not_found")));
		TestEqual(
			TEXT("the handler never replays an unknown operation"),
			RecordingPort->Calls,
			CallsBeforeUnknown + 1);
	}

	using namespace UE::UAI::OperationOwners::LongRunning;
	TSharedPtr<Contract::IUeAgentOperationApplicationPort> FirstPort;
	TSharedPtr<Composition::LongRunningSession::FUeAgentLongRunningSessionRuntime>
		FirstRuntime;
	TestTrue(
		TEXT("the process-owned production runtime is available"),
		TryGetProductionLongRunningLifecyclePort(
			FirstPort,
			FirstRuntime,
			Error));
	IConsoleObject* const RestartObject =
		IConsoleManager::Get().FindConsoleObject(
			TEXT("UeAgentInterface.RestartServer"));
	IConsoleCommand* const RestartCommand = RestartObject
		? RestartObject->AsCommand()
		: nullptr;
	if (TestNotNull(
		TEXT("the real service reconstruction command is registered"),
		RestartCommand))
	{
		TestTrue(
			TEXT("CommandService and transport reconstruct successfully"),
			RestartCommand->Execute(
				{},
				GEditor ? GEditor->GetEditorWorldContext().World() : nullptr,
				*GLog));
	}
	TSharedPtr<Contract::IUeAgentOperationApplicationPort> ReattachedPort;
	TSharedPtr<Composition::LongRunningSession::FUeAgentLongRunningSessionRuntime>
		ReattachedRuntime;
	TestTrue(
		TEXT("the reconstructed public contribution reattaches to the same host"),
		TryGetProductionLongRunningLifecyclePort(
			ReattachedPort,
			ReattachedRuntime,
			Error));
	TestTrue(
		TEXT("the operation registry survives CommandService reconstruction"),
		FirstRuntime == ReattachedRuntime);
	TestTrue(
		TEXT("the lifecycle port survives CommandService reconstruction"),
		FirstPort == ReattachedPort);
	return !HasAnyErrors();
}

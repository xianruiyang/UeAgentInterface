#include "Misc/AutomationTest.h"

#include "CommandEdge/UeAgentCommandProtocol.h"
#include "Core/Contract/UeAgentContract.h"
#include "Editor.h"
#include "HAL/IConsoleManager.h"
#include "Module/CommandRegistration/LongRunning/UeAgentLongRunningLifecycleRegistration.h"
#include "OperationOwners/LongRunning/UeAgentLongRunningLifecycleOwner.h"

namespace
{
	using namespace UE::UAI;

	const TArray<FString>& ExpectedCommands()
	{
		static const TArray<FString> Commands = {
			TEXT("operation.status"), TEXT("operation.receipt"),
			TEXT("operation.cancel"), TEXT("operation.resume"),
			TEXT("operation.reconcile"), TEXT("operation.release"),
			TEXT("operation.discover"), TEXT("operation.recover"), TEXT("operation.takeover"),
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
			*RequestId, *CommandId, *ContractHash, *ParamsJson);
		FTCHARToUTF8 Encoded(*Envelope);
		TArray<uint8> Bytes;
		Bytes.Append(
			reinterpret_cast<const uint8*>(Encoded.Get()), Encoded.Length());
		CommandEdge::FUeAgentCommandProtocolDiagnostic Diagnostic;
		return CommandEdge::FUeAgentCommandProtocolCodec().DecodeRequest(
			Bytes, OutRequest, Diagnostic);
	}

	FString ParamsFor(const FString& CommandId)
	{
		if (CommandId == TEXT("operation.discover")) return TEXT("{}");
		if (CommandId == TEXT("operation.recover")) return TEXT("{\"command_id\":\"editor.session\",\"request_id\":\"req-original\"}");
		if (CommandId == TEXT("operation.takeover")) return TEXT("{\"operation_id\":\"operation:fixture\"}");
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
	FUeAgentStrictV2T32ExactDispatchTest,
	"UAI.StrictV2.T32.LONG.RUNNING.ExactDispatchAndTypedFailure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T32ExactDispatchTest::RunTest(const FString& Parameters)
{
	using namespace UE::UAI;
	using namespace UE::UAI::Module::CommandRegistration::LongRunning;
	const TSharedRef<FRecordingOperationPort> Port =
		MakeShared<FRecordingOperationPort>();
	TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
	FString Error;
	TestTrue(TEXT("the nine lifecycle routes compose in isolation"),
		TryCreateLongRunningLifecycleRegistration(
			Port, Registry, Dispatch, Error));
	if (!Registry.IsValid() || !Dispatch.IsValid())
	{
		AddError(Error);
		return false;
	}

	int32 RequestIndex = 0;
	for (const FString& CommandId : ExpectedCommands())
	{
		const auto* CommandContract = Registry->FindExact(CommandId);
		if (!TestNotNull(TEXT("the exact contract exists"), CommandContract))
		{
			continue;
		}
		const FString Params = ParamsFor(CommandId);
		TOptional<CommandEdge::FUeAgentCommandRequest> Request;
		TestTrue(TEXT("the exact wire request decodes"), DecodeRequest(
			CommandId,
			CommandContract->GetContractHash(),
			FString::Printf(TEXT("req-t32-%d"), RequestIndex++),
			Params,
			Request));
		if (!Request.IsSet())
		{
			continue;
		}
		const CommandEdge::FUeAgentCommandResult Result = Dispatch->Submit(
			Request.GetValue(),
			Contract::FUeAgentCallerContext(
				TEXT("strict-v2-t32"), {TEXT("uai:operate")}, TEXT("corr-t32")),
			MakeShared<FNeverCancelled>());
		TestTrue(TEXT("one exact route delegates successfully"), Result.IsOk());
		TestEqual(TEXT("canonical params are forwarded unchanged"),
			Port->Payloads.Last(), Params);
	}
	TestEqual(TEXT("each request produces exactly one owner call"), Port->Calls, ExpectedCommands().Num());

	const FString UnknownCommand = TEXT("operation.status");
	const auto* UnknownContract = Registry->FindExact(UnknownCommand);
	TOptional<CommandEdge::FUeAgentCommandRequest> UnknownRequest;
	Port->bReturnUnknown = true;
	if (UnknownContract)
	{
		DecodeRequest(UnknownCommand, UnknownContract->GetContractHash(),
			TEXT("req-t32-unknown"), ParamsFor(UnknownCommand), UnknownRequest);
	}
	if (TestTrue(TEXT("the unknown-operation request decodes"),
		UnknownRequest.IsSet()))
	{
		const int32 CallsBeforeUnknown = Port->Calls;
		const CommandEdge::FUeAgentCommandResult Unknown = Dispatch->Submit(
			UnknownRequest.GetValue(),
			Contract::FUeAgentCallerContext(
				TEXT("strict-v2-t32"), {TEXT("uai:operate")}, TEXT("corr-t32")),
			MakeShared<FNeverCancelled>());
		TestTrue(TEXT("unknown remains a typed command failure"),
			!Unknown.IsOk() && !Unknown.IsProtocolError());
		TestTrue(TEXT("unknown preserves the stable code"),
			Unknown.GetErrorJson().Contains(TEXT("operation_not_found")));
		TestEqual(TEXT("unknown is never replayed by the handler"),
			Port->Calls, CallsBeforeUnknown + 1);
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T32ProcessHostContinuityTest,
	"UAI.StrictV2.T32.LONG.RUNNING.ProcessHostContinuity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T32ProcessHostContinuityTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI;
	using namespace UE::UAI::OperationOwners::LongRunning;
	TSharedPtr<Contract::IUeAgentOperationApplicationPort> FirstPort;
	TSharedPtr<Composition::LongRunningSession::FUeAgentLongRunningSessionRuntime>
		FirstRuntime;
	FString Error;
	TestTrue(TEXT("the process-owned production runtime is available"),
		TryGetProductionLongRunningLifecyclePort(
			FirstPort, FirstRuntime, Error));
	IConsoleObject* const RestartObject = IConsoleManager::Get().FindConsoleObject(
		TEXT("UeAgentInterface.RestartServer"));
	IConsoleCommand* const RestartCommand = RestartObject
		? RestartObject->AsCommand()
		: nullptr;
	if (TestNotNull(TEXT("the real service restart command is registered"),
		RestartCommand))
	{
		TestTrue(TEXT("CommandService and transport reconstruct successfully"),
			RestartCommand->Execute({}, GEditor
				? GEditor->GetEditorWorldContext().World()
				: nullptr, *GLog));
	}
	TSharedPtr<Contract::IUeAgentOperationApplicationPort> ReattachedPort;
	TSharedPtr<Composition::LongRunningSession::FUeAgentLongRunningSessionRuntime>
		ReattachedRuntime;
	TestTrue(TEXT("a reconstructed contribution reattaches to the same host"),
		TryGetProductionLongRunningLifecyclePort(
			ReattachedPort, ReattachedRuntime, Error));
	TestTrue(TEXT("the operation registry survives reconstruction"),
		FirstRuntime == ReattachedRuntime);
	TestTrue(TEXT("the lifecycle port survives reconstruction"),
		FirstPort == ReattachedPort);
	return !HasAnyErrors();
}

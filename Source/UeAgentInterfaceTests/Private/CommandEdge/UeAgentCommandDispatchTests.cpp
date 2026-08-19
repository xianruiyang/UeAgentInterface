#include "Misc/AutomationTest.h"

#include "CommandEdge/UeAgentCommandDispatch.h"
#include "CommandEdge/UeAgentCommandProtocol.h"
#include "Core/Contract/UeAgentCommandContract.h"


namespace
{
	using namespace UE::UAI::CommandContract;
	using namespace UE::UAI::CommandEdge;
	using namespace UE::UAI::Contract;

	class FNeverCancelled final : public IUeAgentCancellationSignal
	{
	public:
		bool IsCancelled() const override
		{
			return false;
		}
	};

	class FRecordingHandler final : public IUeAgentCommandHandler
	{
	public:
		explicit FRecordingHandler(FString InCommandId)
			: CommandId(MoveTemp(InCommandId))
		{
		}

		const FString& GetCommandId() const override
		{
			return CommandId;
		}

		FUeAgentCommandResult Submit(
			const FUeAgentCommandRequest& Request,
			const FUeAgentCallerContext& CallerContext,
			const TSharedRef<IUeAgentCancellationSignal>&
				Cancellation) override
		{
			++Calls;
			return FUeAgentCommandResult::Success(
				Request,
				TEXT("{\"handled\":true}"));
		}

		int32 Calls = 0;

	private:
		FString CommandId;
	};

	TOptional<FUeAgentCommandRequest> RequestFor(
		const FString& CommandId,
		const FString& BindingHash,
		const FString& Params = TEXT("{}"))
	{
		const FString Body =
			TEXT("{\"command_binding_hash\":\"")
			+ BindingHash
			+ TEXT("\",\"command_id\":\"")
			+ CommandId
			+ TEXT("\",\"correlation_id\":\"corr-dispatch\",")
			  TEXT("\"message_kind\":\"command_request\",\"params\":")
			+ Params
			+ TEXT(",\"request_id\":\"req-dispatch\",")
			  TEXT("\"schema\":\"uai.command.v1\",\"version\":1}");
		const FTCHARToUTF8 Converted(*Body);
		TArray<uint8> Bytes;
		Bytes.Append(
			reinterpret_cast<const uint8*>(Converted.Get()),
			Converted.Length());
		FUeAgentCommandProtocolCodec Codec;
		FUeAgentCommandProtocolDiagnostic Diagnostic;
		TOptional<FUeAgentCommandRequest> Result;
		Codec.DecodeRequest(Bytes, Result, Diagnostic);
		return Result;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentCommandDispatchExactTest,
	"UeAgentInterface.CommandEdge.Dispatch.Exact",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentCommandDispatchExactTest::RunTest(
	const FString& Parameters)
{
	const TSharedRef<const FUeAgentCommandContractRegistry> Registry =
		GetDefaultCommandContractRegistry();
	const TSharedRef<FRecordingHandler> Query =
		MakeShared<FRecordingHandler>(
			TEXT("reference.catalog_search"));
	const TSharedRef<FRecordingHandler> Read =
		MakeShared<FRecordingHandler>(
			TEXT("reference.asset_read"));
	const TSharedRef<FRecordingHandler> Effect =
		MakeShared<FRecordingHandler>(
			TEXT("reference.asset_apply"));
	TSharedPtr<const FUeAgentCommandDispatch> Dispatch;
	FString Error;
	TestTrue(
		TEXT("one exact handler per active command composes"),
		FUeAgentCommandDispatch::TryCreate(
			Registry,
			{Query, Read, Effect},
			Dispatch,
			Error));
	if (!Dispatch.IsValid())
	{
		AddError(Error);
		return false;
	}
	const FUeAgentCommandContract* Contract =
		Registry->FindExact(TEXT("reference.catalog_search"));
	const TOptional<FUeAgentCommandRequest> Request =
		RequestFor(
			Contract->GetCommandId(),
			Contract->GetBindingHash(),
			TEXT(
				"{\"catalog_kind\":\"uai_reference\","
				"\"match_mode\":\"broad\","
				"\"materialization_policy\":\"index_only\","
				"\"search_text\":\"PivotOffset\"}"));
	TestTrue(TEXT("fixture request decodes"), Request.IsSet());
	if (!Request.IsSet())
	{
		return false;
	}
	const FUeAgentCallerContext Caller(
		TEXT("dispatch-agent"),
		{TEXT("uai:query")},
		Request->GetCorrelationId());
	const FUeAgentCommandResult Result = Dispatch->Submit(
		Request.GetValue(),
		Caller,
		MakeShared<FNeverCancelled>());
	TestTrue(TEXT("exact dispatch returns handler result"), Result.IsOk());
	TestEqual(TEXT("query handler called once"), Query->Calls, 1);
	TestEqual(TEXT("read handler not called"), Read->Calls, 0);
	TestEqual(TEXT("effect handler not called"), Effect->Calls, 0);

	TSharedPtr<const FUeAgentCommandDispatch> Rejected;
	TestFalse(
		TEXT("missing active handler is rejected at composition"),
		FUeAgentCommandDispatch::TryCreate(
			Registry,
			{Query},
			Rejected,
			Error));
	TestEqual(
		TEXT("missing handler failure is stable"),
		Error,
		FString(TEXT("command_dispatch_handler_missing")));
	TestFalse(
		TEXT("duplicate handler is rejected"),
		FUeAgentCommandDispatch::TryCreate(
			Registry,
			{Query, Query, Read, Effect},
			Rejected,
			Error));
	TestEqual(
		TEXT("duplicate handler failure is stable"),
		Error,
		FString(TEXT("command_dispatch_handler_duplicate")));
	return true;
}


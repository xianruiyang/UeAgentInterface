#include "Misc/AutomationTest.h"

#include "CommandEdge/UeAgentCommandProtocol.h"


namespace
{
	using namespace UE::UAI::CommandEdge;

	TArray<uint8> Utf8Bytes(const FString& Text)
	{
		const FTCHARToUTF8 Converted(*Text);
		TArray<uint8> Result;
		Result.Append(
			reinterpret_cast<const uint8*>(Converted.Get()),
			Converted.Length());
		return Result;
	}

	FString Utf8Text(const TArray<uint8>& Bytes)
	{
		const FUTF8ToTCHAR Converted(
			reinterpret_cast<const ANSICHAR*>(Bytes.GetData()),
			Bytes.Num());
		return FString(Converted.Length(), Converted.Get());
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentCommandProtocolCanonicalTest,
	"UeAgentInterface.CommandEdge.Protocol.Canonical",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentCommandProtocolCanonicalTest::RunTest(
	const FString& Parameters)
{
	const FString Canonical =
		TEXT("{\"command_binding_hash\":\"sha256:")
		TEXT("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa")
		TEXT("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",")
		TEXT("\"command_id\":\"fixture.echo_exact\",")
		TEXT("\"correlation_id\":\"corr-fixture-001\",")
		TEXT("\"message_kind\":\"command_request\",")
		TEXT("\"params\":{\"empty_array\":[],\"empty_object\":{},")
		TEXT("\"empty_string\":\"\",\"large_integer\":9007199254740993,")
		TEXT("\"nullable\":null,\"unicode\":\"协议/蓝图\"},")
		TEXT("\"request_id\":\"req-fixture-001\",")
		TEXT("\"schema\":\"uai.command.v1\",\"version\":1}");
	FUeAgentCommandProtocolCodec Codec;
	TOptional<FUeAgentCommandRequest> Request;
	FUeAgentCommandProtocolDiagnostic Diagnostic;
	TestTrue(
		TEXT("decode cross-language canonical request"),
		Codec.DecodeRequest(
			Utf8Bytes(Canonical),
			Request,
			Diagnostic));
	if (!Request.IsSet())
	{
		AddError(Diagnostic.Code + TEXT(":") + Diagnostic.Message);
		return false;
	}
	TestEqual(
		TEXT("large integer lexeme remains exact"),
		Request->GetParamsJson().Contains(TEXT("9007199254740993")),
		true);
	const TSharedPtr<FJsonValue> LargeInteger =
		Request->GetParams()->TryGetField(TEXT("large_integer"));
	int64 ExactInteger = 0;
	TestTrue(
		TEXT("large integer remains an int64"),
		LargeInteger.IsValid()
			&& LargeInteger->TryGetNumber(ExactInteger));
	TestEqual(
		TEXT("large integer value"),
		ExactInteger,
		static_cast<int64>(9007199254740993LL));
	TestTrue(
		TEXT("explicit null remains present"),
		Request->GetParams()->HasTypedField<EJson::Null>(
			TEXT("nullable")));
	TestFalse(
		TEXT("omitted parameter stays omitted"),
		Request->GetParams()->HasField(TEXT("omitted")));

	TArray<uint8> Encoded;
	TestTrue(
		TEXT("re-encode decoded request"),
		Codec.EncodeRequest(
			Request.GetValue(),
			Encoded,
			Diagnostic));
	TestEqual(
		TEXT("canonical request bytes round-trip"),
		Utf8Text(Encoded),
		Canonical);

	const FUeAgentCommandResult Success =
		FUeAgentCommandResult::Success(
			Request.GetValue(),
			TEXT(
				"{\"large_integer\":9007199254740993,"
				"\"unicode\":\"协议/蓝图\"}"));
	TestTrue(
		TEXT("success result encodes"),
		Codec.EncodeResult(Success, Encoded, Diagnostic));
	TestTrue(
		TEXT("result preserves large integer"),
		Utf8Text(Encoded).Contains(TEXT("9007199254740993")));
	const FUeAgentCommandResult Failure =
		FUeAgentCommandResult::Failure(
			Request.GetValue(),
			TEXT(
				"{\"observed\":{\"large_integer\":"
				"9007199254740993}}"),
			TEXT(
				"{\"code\":\"fixture_failed\","
				"\"detail\":\"协议失败\"}"));
	TestTrue(
		TEXT("failure result encodes"),
		Codec.EncodeResult(Failure, Encoded, Diagnostic));
	TOptional<FUeAgentCommandResult> DecodedFailure;
	TestTrue(
		TEXT("failure result decodes"),
		Codec.DecodeResult(
			Encoded,
			DecodedFailure,
			Diagnostic));
	TestTrue(
		TEXT("failure remains a server failure"),
		DecodedFailure.IsSet()
			&& !DecodedFailure->IsOk()
			&& !DecodedFailure->IsProtocolError());
	TestEqual(
		TEXT("failure payload remains lossless"),
		DecodedFailure->GetPayloadJson(),
		FString(
			TEXT(
				"{\"observed\":{\"large_integer\":"
				"9007199254740993}}")));
	TestEqual(
		TEXT("failure error remains structured"),
		DecodedFailure->GetErrorJson(),
		FString(
			TEXT(
				"{\"code\":\"fixture_failed\","
				"\"detail\":\"协议失败\"}")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentCommandProtocolNoDepthGateTest,
	"UeAgentInterface.CommandEdge.Protocol.NoDepthGate",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentCommandProtocolNoDepthGateTest::RunTest(
	const FString& Parameters)
{
	FString Nested(TEXT("{\"leaf\":\"保留\"}"));
	for (int32 Index = 0; Index < 64; ++Index)
	{
		Nested = TEXT("{\"child\":") + Nested + TEXT("}");
	}
	const FString Payload =
		TEXT("{\"command_binding_hash\":\"sha256:")
		TEXT("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa")
		TEXT("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",")
		TEXT("\"command_id\":\"fixture.echo_exact\",")
		TEXT("\"correlation_id\":\"corr-depth\",")
		TEXT("\"message_kind\":\"command_request\",")
		TEXT("\"params\":{\"nested\":")
		+ Nested
		+ TEXT("},\"request_id\":\"req-depth\",")
		  TEXT("\"schema\":\"uai.command.v1\",\"version\":1}");
	FUeAgentCommandProtocolCodec Codec;
	TOptional<FUeAgentCommandRequest> Request;
	FUeAgentCommandProtocolDiagnostic Diagnostic;
	TestTrue(
		TEXT("ordinary deep JSON has no policy depth gate"),
		Codec.DecodeRequest(
			Utf8Bytes(Payload),
			Request,
			Diagnostic));
	TestTrue(
		TEXT("deep request remains available"),
		Request.IsSet());
	return true;
}


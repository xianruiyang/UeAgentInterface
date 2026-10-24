#include "Misc/AutomationTest.h"

#include "CommandEdge/UeAgentCommandProtocol.h"
#include "Core/Contract/UeAgentContractJson.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"


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
			UE::UAI::Contract::FDiagnostic::Failure(TEXT("fixture_failed"), TEXT("协议失败")));
	TestTrue(
		TEXT("failure result encodes"),
		Codec.EncodeResult(Failure, Encoded, Diagnostic));
	TSharedPtr<FJsonObject> Public;
	if (!TestTrue(TEXT("failure response is JSON"),
		UE::UAI::Contract::Json::TryParseObject(Encoded, Public, Diagnostic))) return false;
	TestTrue(
		TEXT("failure remains a server failure"),
		!Public->GetBoolField(TEXT("ok"))
			&& Public->GetStringField(TEXT("message_kind")) == TEXT("command_result"));
	TestTrue(
		TEXT("failure payload remains lossless"),
		Utf8Text(Encoded).Contains(TEXT("{\"observed\":{\"large_integer\":9007199254740993}}")));
	TestEqual(
		TEXT("failure error remains structured"),
		Public->GetObjectField(TEXT("error"))->GetObjectField(TEXT("diagnostic"))->GetStringField(TEXT("message")),
		FString(TEXT("协议失败")));
	auto Rejection = UE::UAI::Contract::FDiagnostic::Failure(TEXT("command_binding_mismatch"), TEXT("The command binding differs."), TEXT("command_protocol"));
	Rejection.InternalContext = TEXT("internal-only-evidence");
	Rejection.Source.SourceUnit = TEXT("internal-unit");
	Rejection.PublicCause = MakeShared<UE::UAI::Contract::FDiagnostic>(Failure.GetDiagnostic());
	auto Business = MakeShared<FJsonObject>();
	Business->SetStringField(TEXT("identity"), TEXT("business identity"));
	Business->SetStringField(TEXT("hash"), TEXT("business hash"));
	Rejection.ActualValue = MakeShared<FJsonValueObject>(Business);
	auto Unchecked = MakeShared<FJsonObject>();
	Unchecked->SetStringField(TEXT("reason"), TEXT("request rejected"));
	Unchecked->SetStringField(TEXT("detail"), TEXT("handler was not invoked"));
	Rejection.Unchecked.Add(MakeShared<FJsonValueObject>(Unchecked));
	const auto ProtocolFailure = FUeAgentCommandResult::ProtocolError(Request->GetCorrelationId(), Request->GetRequestId(),
		Request->GetCommandId(), Request->GetCommandBindingHash(), Rejection);
	TestTrue(TEXT("protocol failure encodes"), Codec.EncodeResult(ProtocolFailure, Encoded, Diagnostic));
	if (!TestTrue(TEXT("protocol failure parses"), UE::UAI::Contract::Json::TryParseObject(Encoded, Public, Diagnostic))) return false;
	const auto Returned = Public->GetObjectField(TEXT("error"))->GetObjectField(TEXT("diagnostic"));
	TestEqual(TEXT("protocol diagnostic retains business identity"), Returned->GetObjectField(TEXT("actual_value"))->GetStringField(TEXT("identity")), FString(TEXT("business identity")));
	TestEqual(TEXT("protocol diagnostic retains business hash"), Returned->GetObjectField(TEXT("actual_value"))->GetStringField(TEXT("hash")), FString(TEXT("business hash")));
	TestEqual(TEXT("nested cause remains complete"), Returned->GetObjectField(TEXT("cause"))->GetStringField(TEXT("message")), FString(TEXT("协议失败")));
	TestEqual(TEXT("unchecked facts are not whitelisted again"), Returned->GetArrayField(TEXT("unchecked"))[0]->AsObject()->GetStringField(TEXT("detail")), FString(TEXT("handler was not invoked")));
	TestFalse(TEXT("internal context excluded by type boundary"), Utf8Text(Encoded).Contains(TEXT("internal-only-evidence")));
	TestFalse(TEXT("internal source excluded by type boundary"), Utf8Text(Encoded).Contains(TEXT("internal-unit")));
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


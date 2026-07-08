#include "Server/UeAgentHttpServer.h"

#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentHttpResponseOutletCompactFormattingTest,
	"UeAgentInterface.Json.HttpResponseOutlet.CompactFormatting",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentHttpResponseOutletCompactFormattingTest::RunTest(const FString& Parameters)
{
	FUeAgentRequestContext Ctx;
	Ctx.RequestId = TEXT("req-http");

	FUeAgentCommandOutput Output;
	Output.Data = MakeShared<FJsonObject>();
	Output.Data->SetNumberField(TEXT("deleted"), 2);
	Output.Data->SetStringField(TEXT("diagnostics"), TEXT("hidden"));

	const TSharedPtr<FJsonObject> Envelope = FUeAgentResponseBuilder::BuildResponseEnvelope(Ctx, true, FString(), Output);
	const FString Json = FUeAgentHttpServer::FormatResponseJson(Envelope.ToSharedRef(), Ctx.ResponseOptions);

	TestTrue(TEXT("formatted compact response has ok/data"), Json.Contains(TEXT("\"ok\": true")));
	TestTrue(TEXT("formatted compact response keeps visible data"), Json.Contains(TEXT("\"deleted\": 2")));
	TestFalse(TEXT("formatted compact response hides diagnostics"), Json.Contains(TEXT("diagnostics")));
	TestFalse(TEXT("formatted compact response hides request_id"), Json.Contains(TEXT("request_id")));
	TestFalse(TEXT("formatted compact response hides timestamp"), Json.Contains(TEXT("timestamp")));
	TestFalse(TEXT("formatted compact response hides empty artifacts"), Json.Contains(TEXT("artifacts")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentHttpResponseOutletDebugFormattingTest,
	"UeAgentInterface.Json.HttpResponseOutlet.DebugFormatting",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentHttpResponseOutletDebugFormattingTest::RunTest(const FString& Parameters)
{
	FUeAgentRequestContext Ctx;
	Ctx.RequestId = TEXT("req-http-debug");
	Ctx.ResponseOptions.View = TEXT("debug");

	FUeAgentCommandOutput Output;
	Output.Data = MakeShared<FJsonObject>();
	Output.Data->SetStringField(TEXT("compile_log"), TEXT("moved"));

	const TSharedPtr<FJsonObject> Envelope = FUeAgentResponseBuilder::BuildResponseEnvelope(Ctx, true, FString(), Output);
	const FString Json = FUeAgentHttpServer::FormatResponseJson(Envelope.ToSharedRef(), Ctx.ResponseOptions);

	TestTrue(TEXT("debug response contains request_id"), Json.Contains(TEXT("\"request_id\": \"req-http-debug\"")));
	TestTrue(TEXT("debug response contains debug object"), Json.Contains(TEXT("\"debug\"")));
	TestTrue(TEXT("debug response contains filtered_data"), Json.Contains(TEXT("filtered_data")));
	TestTrue(TEXT("debug response moves compile_log into debug"), Json.Contains(TEXT("compile_log")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentHttpResponseOutletStandardFormattingTest,
	"UeAgentInterface.Json.HttpResponseOutlet.StandardFormatting",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentHttpResponseOutletStandardFormattingTest::RunTest(const FString& Parameters)
{
	FUeAgentRequestContext Ctx;
	Ctx.RequestId = TEXT("req-http-standard");
	Ctx.ResponseOptions.View = TEXT("standard");

	FUeAgentCommandOutput Output;
	Output.Data = MakeShared<FJsonObject>();
	Output.Data->SetNumberField(TEXT("deleted"), 4);
	Output.Data->SetStringField(TEXT("diagnostics"), TEXT("hidden"));
	Output.Debug = MakeShared<FJsonObject>();
	Output.Debug->SetStringField(TEXT("trace"), TEXT("hidden"));

	const TSharedPtr<FJsonObject> Envelope = FUeAgentResponseBuilder::BuildResponseEnvelope(Ctx, true, FString(), Output);
	const FString Json = FUeAgentHttpServer::FormatResponseJson(Envelope.ToSharedRef(), Ctx.ResponseOptions);

	TestTrue(TEXT("standard response contains ok/data"), Json.Contains(TEXT("\"ok\": true")));
	TestTrue(TEXT("standard response keeps visible data"), Json.Contains(TEXT("\"deleted\": 4")));
	TestFalse(TEXT("standard response hides diagnostics"), Json.Contains(TEXT("diagnostics")));
	TestFalse(TEXT("standard response hides debug"), Json.Contains(TEXT("\"debug\"")));
	TestFalse(TEXT("standard response hides request_id"), Json.Contains(TEXT("request_id")));
	TestFalse(TEXT("standard response hides timestamp"), Json.Contains(TEXT("timestamp")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentHttpResponseOutletRawFormattingTest,
	"UeAgentInterface.Json.HttpResponseOutlet.RawFormatting",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentHttpResponseOutletRawFormattingTest::RunTest(const FString& Parameters)
{
	FUeAgentRequestContext Ctx;
	Ctx.RequestId = TEXT("req-http-raw");
	Ctx.ResponseOptions.View = TEXT("raw");

	FUeAgentCommandOutput Output;
	Output.Data = MakeShared<FJsonObject>();
	Output.Data->SetStringField(TEXT("diagnostics"), TEXT("kept"));

	const TSharedPtr<FJsonObject> Envelope = FUeAgentResponseBuilder::BuildResponseEnvelope(Ctx, true, FString(), Output);
	const FString Json = FUeAgentHttpServer::FormatResponseJson(Envelope.ToSharedRef(), Ctx.ResponseOptions);

	TestTrue(TEXT("raw response contains request_id"), Json.Contains(TEXT("\"request_id\": \"req-http-raw\"")));
	TestTrue(TEXT("raw response contains timestamp"), Json.Contains(TEXT("\"timestamp\"")));
	TestTrue(TEXT("raw response keeps diagnostics in data"), Json.Contains(TEXT("\"diagnostics\": \"kept\"")));
	TestTrue(TEXT("raw response keeps artifacts field"), Json.Contains(TEXT("\"artifacts\"")));
	TestTrue(TEXT("raw response keeps warnings field"), Json.Contains(TEXT("\"warnings\"")));
	return true;
}

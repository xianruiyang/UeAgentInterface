#include "Server/UeAgentHttpServer.h"

#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentResponseBuilderCompactSuccessTest,
	"UeAgentInterface.Json.ResponseBuilder.CompactSuccess",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentResponseBuilderCompactSuccessTest::RunTest(const FString& Parameters)
{
	FUeAgentRequestContext Ctx;
	Ctx.RequestId = TEXT("req-compact");

	FUeAgentCommandOutput Output;
	Output.Data = MakeShared<FJsonObject>();
	Output.Data->SetNumberField(TEXT("deleted"), 6);
	Output.Data->SetStringField(TEXT("diagnostics"), TEXT("hidden"));
	Output.Data->SetStringField(TEXT("raw_readback"), TEXT("hidden"));

	const TSharedPtr<FJsonObject> Envelope = FUeAgentResponseBuilder::BuildResponseEnvelope(Ctx, true, FString(), Output);
	const TSharedPtr<FJsonObject> Data = Envelope->GetObjectField(TEXT("data"));
	const TArray<TSharedPtr<FJsonValue>>* Artifacts = nullptr;
	const bool bHasArtifacts = Envelope->TryGetArrayField(TEXT("artifacts"), Artifacts);

	TestTrue(TEXT("ok is present"), Envelope->GetBoolField(TEXT("ok")));
	TestEqual(TEXT("visible data is preserved"), Data->GetIntegerField(TEXT("deleted")), 6);
	TestFalse(TEXT("request_id is hidden by default"), Envelope->HasField(TEXT("request_id")));
	TestFalse(TEXT("timestamp is hidden by default"), Envelope->HasField(TEXT("timestamp")));
	TestFalse(TEXT("empty error is omitted"), Envelope->HasField(TEXT("error")));
	TestFalse(TEXT("empty warnings are omitted"), Envelope->HasField(TEXT("warnings")));
	TestFalse(TEXT("debug is omitted by default"), Envelope->HasField(TEXT("debug")));
	TestEqual(TEXT("diagnostics stay in compact data"), Data->GetStringField(TEXT("diagnostics")), FString(TEXT("hidden")));
	TestEqual(TEXT("raw_readback stays in compact data"), Data->GetStringField(TEXT("raw_readback")), FString(TEXT("hidden")));
	TestFalse(TEXT("compact view does not externalize data fields"), bHasArtifacts);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentResponseBuilderDebugViewTest,
	"UeAgentInterface.Json.ResponseBuilder.DebugView",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentResponseBuilderDebugViewTest::RunTest(const FString& Parameters)
{
	FUeAgentRequestContext Ctx;
	Ctx.RequestId = TEXT("req-debug");
	Ctx.ResponseOptions.View = TEXT("debug");

	FUeAgentCommandOutput Output;
	Output.Data = MakeShared<FJsonObject>();
	Output.Data->SetBoolField(TEXT("saved"), true);
	Output.Data->SetStringField(TEXT("diagnostics"), TEXT("moved"));
	TSharedPtr<FJsonObject> CompileLog = MakeShared<FJsonObject>();
	CompileLog->SetNumberField(TEXT("errors"), 0);
	CompileLog->SetNumberField(TEXT("warnings"), 2);
	Output.Data->SetObjectField(TEXT("compile_log"), CompileLog);
	Output.Debug = MakeShared<FJsonObject>();
	Output.Debug->SetStringField(TEXT("query"), TEXT("asset"));

	const TSharedPtr<FJsonObject> Envelope = FUeAgentResponseBuilder::BuildResponseEnvelope(Ctx, true, FString(), Output);
	const TSharedPtr<FJsonObject> Data = Envelope->GetObjectField(TEXT("data"));
	const TSharedPtr<FJsonObject> Debug = Envelope->GetObjectField(TEXT("debug"));
	const TArray<TSharedPtr<FJsonValue>>* Artifacts = nullptr;
	const bool bHasArtifacts = Envelope->TryGetArrayField(TEXT("artifacts"), Artifacts);

	TestTrue(TEXT("debug view keeps ok"), Envelope->GetBoolField(TEXT("ok")));
	TestEqual(TEXT("debug view includes request_id"), Envelope->GetStringField(TEXT("request_id")), FString(TEXT("req-debug")));
	TestFalse(TEXT("debug view still omits timestamp"), Envelope->HasField(TEXT("timestamp")));
	TestTrue(TEXT("visible data remains in data"), Data->GetBoolField(TEXT("saved")));
	TestEqual(TEXT("debug view keeps diagnostics in data"), Data->GetStringField(TEXT("diagnostics")), FString(TEXT("moved")));
	TestEqual(TEXT("explicit debug is preserved"), Debug->GetStringField(TEXT("query")), FString(TEXT("asset")));
	TestTrue(TEXT("debug view keeps complete compile log in data"), Data->GetObjectField(TEXT("compile_log"))->GetIntegerField(TEXT("warnings")) == 2);
	TestFalse(TEXT("debug view does not externalize data fields"), bHasArtifacts);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentResponseBuilderStandardViewTest,
	"UeAgentInterface.Json.ResponseBuilder.StandardView",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentResponseBuilderStandardViewTest::RunTest(const FString& Parameters)
{
	FUeAgentRequestContext Ctx;
	Ctx.RequestId = TEXT("req-standard");
	Ctx.ResponseOptions.View = TEXT("standard");

	FUeAgentCommandOutput Output;
	Output.Data = MakeShared<FJsonObject>();
	Output.Data->SetNumberField(TEXT("deleted"), 3);
	Output.Data->SetStringField(TEXT("diagnostics"), TEXT("hidden"));
	Output.Data->SetStringField(TEXT("raw_readback"), TEXT("hidden"));
	Output.Debug = MakeShared<FJsonObject>();
	Output.Debug->SetStringField(TEXT("trace"), TEXT("hidden"));

	const TSharedPtr<FJsonObject> Envelope = FUeAgentResponseBuilder::BuildResponseEnvelope(Ctx, true, FString(), Output);
	const TSharedPtr<FJsonObject> Data = Envelope->GetObjectField(TEXT("data"));
	const TArray<TSharedPtr<FJsonValue>>* Artifacts = nullptr;
	const bool bHasArtifacts = Envelope->TryGetArrayField(TEXT("artifacts"), Artifacts);

	TestTrue(TEXT("standard view keeps ok"), Envelope->GetBoolField(TEXT("ok")));
	TestEqual(TEXT("standard view keeps visible data"), Data->GetIntegerField(TEXT("deleted")), 3);
	TestFalse(TEXT("standard view hides request_id"), Envelope->HasField(TEXT("request_id")));
	TestFalse(TEXT("standard view hides timestamp"), Envelope->HasField(TEXT("timestamp")));
	TestFalse(TEXT("standard view hides debug object"), Envelope->HasField(TEXT("debug")));
	TestEqual(TEXT("standard view keeps diagnostics in data"), Data->GetStringField(TEXT("diagnostics")), FString(TEXT("hidden")));
	TestEqual(TEXT("standard view keeps raw_readback in data"), Data->GetStringField(TEXT("raw_readback")), FString(TEXT("hidden")));
	TestFalse(TEXT("standard view does not externalize data fields"), bHasArtifacts);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentResponseBuilderSummaryViewTest,
	"UeAgentInterface.Json.ResponseBuilder.SummaryView",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentResponseBuilderSummaryViewTest::RunTest(const FString& Parameters)
{
	FUeAgentRequestContext Ctx;
	Ctx.RequestId = TEXT("req-summary");
	Ctx.ResponseOptions.View = TEXT("summary");

	FUeAgentCommandOutput Output;
	Output.Data = MakeShared<FJsonObject>();
	Output.Data->SetNumberField(TEXT("deleted"), 3);
	Output.Data->SetStringField(TEXT("diagnostics"), TEXT("hidden"));
	Output.Data->SetStringField(TEXT("raw_readback"), TEXT("hidden"));

	const TSharedPtr<FJsonObject> Envelope = FUeAgentResponseBuilder::BuildResponseEnvelope(Ctx, true, FString(), Output);
	const TSharedPtr<FJsonObject> Data = Envelope->GetObjectField(TEXT("data"));
	const TArray<TSharedPtr<FJsonValue>>* Artifacts = nullptr;
	const bool bHasArtifacts = Envelope->TryGetArrayField(TEXT("artifacts"), Artifacts);

	TestTrue(TEXT("summary view keeps ok"), Envelope->GetBoolField(TEXT("ok")));
	TestEqual(TEXT("summary view keeps visible data"), Data->GetIntegerField(TEXT("deleted")), 3);
	TestFalse(TEXT("summary view filters diagnostics from data"), Data->HasField(TEXT("diagnostics")));
	TestFalse(TEXT("summary view filters raw_readback from data"), Data->HasField(TEXT("raw_readback")));
	TestTrue(TEXT("summary view externalizes raw_readback"), bHasArtifacts && Artifacts && Artifacts->Num() == 1);
	if (Artifacts && Artifacts->Num() == 1)
	{
		const TSharedPtr<FJsonObject> Artifact = (*Artifacts)[0]->AsObject();
		TestEqual(TEXT("artifact field is tracked"), Artifact->GetStringField(TEXT("field")), FString(TEXT("raw_readback")));
		TestTrue(TEXT("artifact file exists"), IFileManager::Get().FileExists(*Artifact->GetStringField(TEXT("path"))));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentResponseBuilderDefaultLeakageTest,
	"UeAgentInterface.Json.ResponseBuilder.DefaultLeakage",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentResponseBuilderDefaultLeakageTest::RunTest(const FString& Parameters)
{
	FUeAgentRequestContext Ctx;
	Ctx.RequestId = TEXT("req-default-leakage");

	FUeAgentCommandOutput Output;
	Output.Data = MakeShared<FJsonObject>();
	Output.Data->SetStringField(TEXT("visible"), TEXT("kept"));
	Output.Data->SetStringField(TEXT("timestamp"), TEXT("hidden"));
	Output.Data->SetStringField(TEXT("diagnostics"), TEXT("hidden"));
	Output.Data->SetStringField(TEXT("coverage_report"), TEXT("hidden"));
	Output.Data->SetStringField(TEXT("validation_report"), TEXT("hidden"));
	Output.Data->SetStringField(TEXT("compile_log"), TEXT("hidden"));
	Output.Data->SetStringField(TEXT("raw"), TEXT("hidden"));
	Output.Data->SetStringField(TEXT("readback"), TEXT("hidden"));
	Output.Data->SetStringField(TEXT("raw_sample"), TEXT("hidden"));
	Output.Data->SetStringField(TEXT("debug_sample"), TEXT("hidden"));
	Output.Debug = MakeShared<FJsonObject>();
	Output.Debug->SetStringField(TEXT("trace"), TEXT("hidden"));

	const TSharedPtr<FJsonObject> Envelope = FUeAgentResponseBuilder::BuildResponseEnvelope(Ctx, true, FString(), Output);
	const TSharedPtr<FJsonObject> Data = Envelope->GetObjectField(TEXT("data"));

	TestTrue(TEXT("visible data is kept"), Data->GetStringField(TEXT("visible")).Equals(TEXT("kept")));
	TestFalse(TEXT("root timestamp is hidden by default"), Envelope->HasField(TEXT("timestamp")));
	TestFalse(TEXT("root debug is hidden by default"), Envelope->HasField(TEXT("debug")));
	TestTrue(TEXT("data timestamp is kept by default"), Data->HasField(TEXT("timestamp")));
	TestTrue(TEXT("diagnostics are kept by default"), Data->HasField(TEXT("diagnostics")));
	TestTrue(TEXT("coverage report is kept by default"), Data->HasField(TEXT("coverage_report")));
	TestTrue(TEXT("validation report is kept by default"), Data->HasField(TEXT("validation_report")));
	TestTrue(TEXT("compile log is kept by default"), Data->HasField(TEXT("compile_log")));
	TestTrue(TEXT("raw is kept by default"), Data->HasField(TEXT("raw")));
	TestTrue(TEXT("readback is kept by default"), Data->HasField(TEXT("readback")));
	TestTrue(TEXT("raw prefix fields are kept by default"), Data->HasField(TEXT("raw_sample")));
	TestTrue(TEXT("debug prefix fields are kept by default"), Data->HasField(TEXT("debug_sample")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentResponseBuilderRawViewTest,
	"UeAgentInterface.Json.ResponseBuilder.RawView",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentResponseBuilderRawViewTest::RunTest(const FString& Parameters)
{
	FUeAgentRequestContext Ctx;
	Ctx.RequestId = TEXT("req-raw");
	Ctx.ResponseOptions.View = TEXT("raw");

	FUeAgentCommandOutput Output;
	Output.Data = MakeShared<FJsonObject>();
	Output.Data->SetStringField(TEXT("diagnostics"), TEXT("kept"));
	TSharedPtr<FJsonObject> CompileLog = MakeShared<FJsonObject>();
	CompileLog->SetNumberField(TEXT("errors"), 1);
	Output.Data->SetObjectField(TEXT("compile_log"), CompileLog);
	Output.Debug = MakeShared<FJsonObject>();
	Output.Debug->SetStringField(TEXT("trace"), TEXT("kept"));

	const TSharedPtr<FJsonObject> Envelope = FUeAgentResponseBuilder::BuildResponseEnvelope(Ctx, true, FString(), Output);
	const TSharedPtr<FJsonObject> Data = Envelope->GetObjectField(TEXT("data"));

	TestTrue(TEXT("raw view keeps ok"), Envelope->GetBoolField(TEXT("ok")));
	TestEqual(TEXT("raw view includes request_id"), Envelope->GetStringField(TEXT("request_id")), FString(TEXT("req-raw")));
	TestTrue(TEXT("raw view includes timestamp"), Envelope->HasField(TEXT("timestamp")));
	TestTrue(TEXT("raw view includes empty error"), Envelope->HasField(TEXT("error")));
	TestTrue(TEXT("raw view includes artifacts field"), Envelope->HasField(TEXT("artifacts")));
	TestTrue(TEXT("raw view includes warnings field"), Envelope->HasField(TEXT("warnings")));
	TestEqual(TEXT("raw view does not filter data"), Data->GetStringField(TEXT("diagnostics")), FString(TEXT("kept")));
	TestEqual(TEXT("raw view preserves compile log in data"), Data->GetObjectField(TEXT("compile_log"))->GetIntegerField(TEXT("errors")), 1);
	TestEqual(TEXT("raw view preserves debug"), Envelope->GetObjectField(TEXT("debug"))->GetStringField(TEXT("trace")), FString(TEXT("kept")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentResponseBuilderCompactFailureTest,
	"UeAgentInterface.Json.ResponseBuilder.CompactFailure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentResponseBuilderCompactFailureTest::RunTest(const FString& Parameters)
{
	FUeAgentRequestContext Ctx;
	Ctx.RequestId = TEXT("req-fail");

	FUeAgentCommandOutput Output;
	Output.Data = MakeShared<FJsonObject>();
	Output.Data->SetStringField(TEXT("scope"), TEXT("UAI"));

	const TSharedPtr<FJsonObject> Envelope = FUeAgentResponseBuilder::BuildResponseEnvelope(Ctx, false, TEXT("no_deletable_candidates"), Output);
	const TSharedPtr<FJsonObject> Data = Envelope->GetObjectField(TEXT("data"));

	TestFalse(TEXT("failure ok is false"), Envelope->GetBoolField(TEXT("ok")));
	TestEqual(TEXT("failure error is stable"), Envelope->GetStringField(TEXT("error")), FString(TEXT("no_deletable_candidates")));
	TestEqual(TEXT("failure data is preserved"), Data->GetStringField(TEXT("scope")), FString(TEXT("UAI")));
	TestFalse(TEXT("failure hides request_id by default"), Envelope->HasField(TEXT("request_id")));
	TestFalse(TEXT("failure hides timestamp by default"), Envelope->HasField(TEXT("timestamp")));
	return true;
}

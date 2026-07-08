#include "Server/UeAgentHttpServer.h"

#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentCommandOutputLegacyBridgeTest,
	"UeAgentInterface.Json.CommandOutput.LegacyBridge",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentCommandOutputLegacyBridgeTest::RunTest(const FString& Parameters)
{
	TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), TEXT("/Game/Test"));
	TArray<TSharedPtr<FJsonValue>> Artifacts;
	Artifacts.Add(MakeShared<FJsonValueString>(TEXT("runtimeLogs/test.json")));

	const FUeAgentCommandOutput Output = FUeAgentCommandOutput::FromLegacy(Data, Artifacts);

	TestTrue(TEXT("legacy data is bridged"), Output.Data.IsValid());
	TestEqual(TEXT("legacy data field is preserved"), Output.Data->GetStringField(TEXT("asset_path")), FString(TEXT("/Game/Test")));
	TestEqual(TEXT("legacy artifacts are bridged"), Output.Artifacts.Num(), 1);
	TestFalse(TEXT("legacy bridge does not create debug"), Output.Debug.IsValid());
	TestEqual(TEXT("legacy bridge does not create warnings"), Output.Warnings.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentCommandOutputExplicitFieldsTest,
	"UeAgentInterface.Json.CommandOutput.ExplicitFields",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentCommandOutputExplicitFieldsTest::RunTest(const FString& Parameters)
{
	FUeAgentCommandOutput Output;
	Output.Data = MakeShared<FJsonObject>();
	Output.Data->SetBoolField(TEXT("ok"), true);
	Output.Debug = MakeShared<FJsonObject>();
	Output.Debug->SetStringField(TEXT("source"), TEXT("test"));
	Output.Artifacts.Add(MakeShared<FJsonValueString>(TEXT("a.json")));
	Output.Warnings.Add(MakeShared<FJsonValueString>(TEXT("warn")));

	TestTrue(TEXT("data can be explicit"), Output.Data.IsValid());
	TestTrue(TEXT("debug can be explicit"), Output.Debug.IsValid());
	TestEqual(TEXT("artifact can be explicit"), Output.Artifacts.Num(), 1);
	TestEqual(TEXT("warning can be explicit"), Output.Warnings.Num(), 1);

	Output.Reset();

	TestFalse(TEXT("reset clears data"), Output.Data.IsValid());
	TestFalse(TEXT("reset clears debug"), Output.Debug.IsValid());
	TestEqual(TEXT("reset clears artifacts"), Output.Artifacts.Num(), 0);
	TestEqual(TEXT("reset clears warnings"), Output.Warnings.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentCommandOutputUnknownCommandTest,
	"UeAgentInterface.Json.CommandOutput.UnknownCommand",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentCommandOutputUnknownCommandTest::RunTest(const FString& Parameters)
{
	FUeAgentHttpServer Server;
	FUeAgentRequestContext Ctx;
	Ctx.Command = TEXT("definitely_unknown_command_for_command_output_test");
	Ctx.Params = MakeShared<FJsonObject>();

	FUeAgentCommandOutput Output;
	FString Error;
	const bool bOk = Server.ExecuteCommandOnGameThread(Ctx, Output, Error);

	TestFalse(TEXT("unknown command fails"), bOk);
	TestEqual(TEXT("unknown command error"), Error, FString(TEXT("unknown_command")));
	TestTrue(TEXT("unknown command still returns data object"), Output.Data.IsValid());
	TestEqual(TEXT("unknown command artifacts empty"), Output.Artifacts.Num(), 0);
	return true;
}

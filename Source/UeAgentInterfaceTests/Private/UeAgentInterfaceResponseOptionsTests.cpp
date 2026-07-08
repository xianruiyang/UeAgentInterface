#include "Server/UeAgentHttpServer.h"

#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentResponseOptionsDefaultsTest,
	"UeAgentInterface.Json.ResponseOptions.Defaults",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentResponseOptionsDefaultsTest::RunTest(const FString& Parameters)
{
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	FUeAgentResponseOptions Options;
	FString Error;

	TestTrue(TEXT("missing response_options parses"), FUeAgentHttpServer::ParseResponseOptions(Root, Options, Error));
	TestEqual(TEXT("default view"), Options.View, FString(TEXT("llm_compact")));
	TestFalse(TEXT("default debug"), Options.bDebug);
	TestEqual(TEXT("default format"), Options.Format, FString(TEXT("structured_compact")));
	TestEqual(TEXT("default max_items"), Options.MaxItems, 50);
	TestTrue(TEXT("default parse has no error"), Error.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentResponseOptionsCustomTest,
	"UeAgentInterface.Json.ResponseOptions.Custom",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentResponseOptionsCustomTest::RunTest(const FString& Parameters)
{
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	TSharedRef<FJsonObject> ResponseOptions = MakeShared<FJsonObject>();
	ResponseOptions->SetStringField(TEXT("view"), TEXT("debug"));
	ResponseOptions->SetBoolField(TEXT("debug"), true);
	ResponseOptions->SetStringField(TEXT("format"), TEXT("pretty_debug"));
	ResponseOptions->SetNumberField(TEXT("max_items"), 12);
	Root->SetObjectField(TEXT("response_options"), ResponseOptions);

	FUeAgentResponseOptions Options;
	FString Error;

	TestTrue(TEXT("custom response_options parses"), FUeAgentHttpServer::ParseResponseOptions(Root, Options, Error));
	TestEqual(TEXT("custom view"), Options.View, FString(TEXT("debug")));
	TestTrue(TEXT("custom debug"), Options.bDebug);
	TestEqual(TEXT("custom format"), Options.Format, FString(TEXT("pretty_debug")));
	TestEqual(TEXT("custom max_items"), Options.MaxItems, 12);
	TestTrue(TEXT("custom parse has no error"), Error.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentResponseOptionsRootOnlyTest,
	"UeAgentInterface.Json.ResponseOptions.RootOnly",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentResponseOptionsRootOnlyTest::RunTest(const FString& Parameters)
{
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
	TSharedRef<FJsonObject> NestedOptions = MakeShared<FJsonObject>();
	NestedOptions->SetStringField(TEXT("view"), TEXT("raw"));
	NestedOptions->SetBoolField(TEXT("debug"), true);
	NestedOptions->SetStringField(TEXT("format"), TEXT("pretty_debug"));
	NestedOptions->SetNumberField(TEXT("max_items"), 5);
	Params->SetObjectField(TEXT("response_options"), NestedOptions);
	Root->SetObjectField(TEXT("params"), Params);

	FUeAgentResponseOptions Options;
	FString Error;

	TestTrue(TEXT("params.response_options is ignored"), FUeAgentHttpServer::ParseResponseOptions(Root, Options, Error));
	TestEqual(TEXT("view remains default"), Options.View, FString(TEXT("llm_compact")));
	TestFalse(TEXT("debug remains default"), Options.bDebug);
	TestEqual(TEXT("format remains default"), Options.Format, FString(TEXT("structured_compact")));
	TestEqual(TEXT("max_items remains default"), Options.MaxItems, 50);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentResponseOptionsInvalidMaxItemsTest,
	"UeAgentInterface.Json.ResponseOptions.InvalidMaxItems",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentResponseOptionsInvalidMaxItemsTest::RunTest(const FString& Parameters)
{
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	TSharedRef<FJsonObject> ResponseOptions = MakeShared<FJsonObject>();
	ResponseOptions->SetNumberField(TEXT("max_items"), 0);
	Root->SetObjectField(TEXT("response_options"), ResponseOptions);

	FUeAgentResponseOptions Options;
	FString Error;

	TestFalse(TEXT("non-positive max_items is rejected"), FUeAgentHttpServer::ParseResponseOptions(Root, Options, Error));
	TestEqual(TEXT("stable max_items error"), Error, FString(TEXT("invalid_response_options.max_items")));
	return true;
}

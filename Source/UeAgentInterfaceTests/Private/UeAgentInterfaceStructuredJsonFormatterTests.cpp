#include "Utilities/UeAgentStructuredJsonFormatter.h"

#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStructuredJsonFormatterBasicTest,
	"UeAgentInterface.Json.StructuredFormatter.Basic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStructuredJsonFormatterBasicTest::RunTest(const FString& Parameters)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	TSharedRef<FJsonObject> Asset = MakeShared<FJsonObject>();
	Asset->SetStringField(TEXT("path"), TEXT("/Game/A"));
	Asset->SetBoolField(TEXT("dirty"), false);
	Object->SetObjectField(TEXT("asset"), Asset);
	Object->SetNumberField(TEXT("deleted"), 6);
	Object->SetArrayField(TEXT("folders"), {
		MakeShared<FJsonValueString>(TEXT("A05")),
		MakeShared<FJsonValueString>(TEXT("A06")),
	});

	const FString Output = FUeAgentStructuredJsonFormatter::WriteObject(Object);

	TestTrue(TEXT("object with array property is expanded"), Output.Contains(TEXT("\n")));
	TestTrue(TEXT("nested short object is inline"), Output.Contains(TEXT("\"asset\": {\"path\": \"/Game/A\", \"dirty\": false}")));
	TestTrue(TEXT("scalar array is inline"), Output.Contains(TEXT("\"folders\": [\"A05\", \"A06\"]")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStructuredJsonFormatterArrayPropertyTest,
	"UeAgentInterface.Json.StructuredFormatter.ArrayProperty",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStructuredJsonFormatterArrayPropertyTest::RunTest(const FString& Parameters)
{
	TSharedRef<FJsonObject> Shape = MakeShared<FJsonObject>();
	Shape->SetStringField(TEXT("type"), TEXT("box"));
	Shape->SetArrayField(TEXT("center"), {
		MakeShared<FJsonValueNumber>(0),
		MakeShared<FJsonValueNumber>(0),
		MakeShared<FJsonValueNumber>(50),
	});
	Shape->SetArrayField(TEXT("extent"), {
		MakeShared<FJsonValueNumber>(100),
		MakeShared<FJsonValueNumber>(80),
		MakeShared<FJsonValueNumber>(50),
	});

	const FString Output = FUeAgentStructuredJsonFormatter::WriteObject(Shape);

	TestTrue(TEXT("shape object with array fields is expanded"), Output.Contains(TEXT("\n")));
	TestTrue(TEXT("center scalar array is inline on field line"), Output.Contains(TEXT("\"center\": [0, 0, 50]")));
	TestFalse(TEXT("center scalar array is not expanded"), Output.Contains(TEXT("\"center\": [\n")));
	TestFalse(TEXT("shape object is not fully inline"), Output.Contains(TEXT("{\"type\": \"box\", \"center\": [0, 0, 50]")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStructuredJsonFormatterSpatialArrayTest,
	"UeAgentInterface.Json.StructuredFormatter.SpatialArrays",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStructuredJsonFormatterSpatialArrayTest::RunTest(const FString& Parameters)
{
	TSharedRef<FJsonObject> Contract = MakeShared<FJsonObject>();
	Contract->SetArrayField(TEXT("up_world"), {
		MakeShared<FJsonValueNumber>(0),
		MakeShared<FJsonValueNumber>(0),
		MakeShared<FJsonValueNumber>(1),
	});

	TSharedRef<FJsonObject> Segment = MakeShared<FJsonObject>();
	Segment->SetArrayField(TEXT("a"), {
		MakeShared<FJsonValueNumber>(100),
		MakeShared<FJsonValueNumber>(200),
		MakeShared<FJsonValueNumber>(300),
	});
	Segment->SetArrayField(TEXT("b"), {
		MakeShared<FJsonValueNumber>(400),
		MakeShared<FJsonValueNumber>(500),
		MakeShared<FJsonValueNumber>(300),
	});
	Contract->SetObjectField(TEXT("line_segment_world"), Segment);

	const FString Output = FUeAgentStructuredJsonFormatter::WriteObject(Contract);

	TestTrue(TEXT("up_world scalar array is inline"), Output.Contains(TEXT("\"up_world\": [0, 0, 1]")));
	TestTrue(TEXT("line segment point a is inline"), Output.Contains(TEXT("\"a\": [100, 200, 300]")));
	TestTrue(TEXT("line segment point b is inline"), Output.Contains(TEXT("\"b\": [400, 500, 300]")));
	TestFalse(TEXT("up_world scalar array is not expanded"), Output.Contains(TEXT("\"up_world\": [\n")));
	TestFalse(TEXT("line segment point a is not expanded"), Output.Contains(TEXT("\"a\": [\n")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStructuredJsonFormatterThresholdTest,
	"UeAgentInterface.Json.StructuredFormatter.Thresholds",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStructuredJsonFormatterThresholdTest::RunTest(const FString& Parameters)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetNumberField(TEXT("a"), 1);
	Object->SetNumberField(TEXT("b"), 2);
	Object->SetNumberField(TEXT("c"), 3);
	Object->SetNumberField(TEXT("d"), 4);
	Object->SetNumberField(TEXT("e"), 5);
	Object->SetNumberField(TEXT("f"), 6);

	const FString Output = FUeAgentStructuredJsonFormatter::WriteObject(Object);

	TestFalse(TEXT("six exposed properties are inline"), Output.Contains(TEXT("\n")));
	TestTrue(TEXT("six exposed properties are written on one line"), Output.StartsWith(TEXT("{\"")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStructuredJsonFormatterAuthoringMetadataTest,
	"UeAgentInterface.Json.StructuredFormatter.AuthoringMetadata",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStructuredJsonFormatterAuthoringMetadataTest::RunTest(const FString& Parameters)
{
	TSharedRef<FJsonObject> Property = MakeShared<FJsonObject>();
	Property->SetStringField(TEXT("property_path"), TEXT("RelativeLocation"));

	TSharedRef<FJsonObject> Availability = MakeShared<FJsonObject>();
	Availability->SetBoolField(TEXT("enabled"), true);
	Availability->SetBoolField(TEXT("editable"), true);
	Property->SetObjectField(TEXT("availability"), Availability);

	TSharedRef<FJsonObject> Constraints = MakeShared<FJsonObject>();
	Constraints->SetStringField(TEXT("units"), TEXT("cm"));
	Property->SetObjectField(TEXT("constraints"), Constraints);

	TSharedRef<FJsonObject> SpaceContract = MakeShared<FJsonObject>();
	SpaceContract->SetStringField(TEXT("subject"), TEXT("RelativeLocation"));
	SpaceContract->SetStringField(TEXT("dimension"), TEXT("3d"));
	Property->SetObjectField(TEXT("space_contract"), SpaceContract);

	const FString Output = FUeAgentStructuredJsonFormatter::WriteObject(Property);

	TestTrue(TEXT("availability object is inline"), Output.Contains(TEXT("\"availability\": {\"enabled\": true, \"editable\": true}")));
	TestTrue(TEXT("constraints object is inline"), Output.Contains(TEXT("\"constraints\": {\"units\": \"cm\"}")));
	TestTrue(TEXT("space contract object is inline"), Output.Contains(TEXT("\"space_contract\": {\"subject\": \"RelativeLocation\", \"dimension\": \"3d\"}")));
	TestFalse(TEXT("availability object is not expanded"), Output.Contains(TEXT("\"availability\": {\n")));
	TestFalse(TEXT("constraints object is not expanded"), Output.Contains(TEXT("\"constraints\": {\n")));
	TestFalse(TEXT("space contract object is not expanded"), Output.Contains(TEXT("\"space_contract\": {\n")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStructuredJsonFormatterSaveFileTest,
	"UeAgentInterface.Json.StructuredFormatter.SaveFile",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStructuredJsonFormatterSaveFileTest::RunTest(const FString& Parameters)
{
	const FString OutputFile = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("Automation") / TEXT("UAI_StructuredFormatter") / TEXT("payload.json"));
	IFileManager::Get().Delete(*OutputFile, false, true, true);

	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	TSharedRef<FJsonObject> Transform = MakeShared<FJsonObject>();
	TSharedRef<FJsonObject> Location = MakeShared<FJsonObject>();
	Location->SetNumberField(TEXT("x"), 0);
	Location->SetNumberField(TEXT("y"), 0);
	Location->SetNumberField(TEXT("z"), 0);
	Transform->SetObjectField(TEXT("location"), Location);
	Object->SetObjectField(TEXT("transform"), Transform);

	FString Error;
	TestTrue(TEXT("save object to file"), FUeAgentStructuredJsonFormatter::SaveObjectToFile(OutputFile, Object, &Error));
	TestTrue(TEXT("file exists"), IFileManager::Get().FileExists(*OutputFile));

	FString Text;
	TestTrue(TEXT("file readable"), FFileHelper::LoadFileToString(Text, *OutputFile));
	TestTrue(TEXT("nested vector object is inline"), Text.Contains(TEXT("\"location\": {\"x\": 0, \"y\": 0, \"z\": 0}")));
	TestFalse(TEXT("nested vector object is not expanded"), Text.Contains(TEXT("\"location\":\n")));
	TestTrue(TEXT("saved json preserves threshold-based expansion"), Text.TrimEnd().Contains(TEXT("\n")));
	return true;
}

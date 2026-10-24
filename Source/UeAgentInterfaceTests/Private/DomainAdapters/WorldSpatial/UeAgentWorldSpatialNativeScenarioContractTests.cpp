#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
	FString FixturePath()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(
			FPaths::ProjectPluginsDir(),
			TEXT("UeAgentInterface/Source/UeAgentInterfaceTests/Private/"
				"DomainAdapters/WorldSpatial/Fixtures/"
				"world_spatial_native_scenario_cases.json")));
	}

	TSharedPtr<FJsonObject> LoadFixture(FAutomationTestBase* Test)
	{
		FString Text;
		if (!Test->TestTrue(
				TEXT("load WorldSpatial native scenario fixture"),
				FFileHelper::LoadFileToString(Text, *FixturePath())))
		{
			return nullptr;
		}
		TSharedPtr<FJsonObject> Object;
		const TSharedRef<TJsonReader<>> Reader =
			TJsonReaderFactory<>::Create(Text);
		Test->TestTrue(
			TEXT("parse WorldSpatial native scenario fixture"),
			FJsonSerializer::Deserialize(Reader, Object)
				&& Object.IsValid());
		return Object;
	}

	bool NonEmpty(
		FAutomationTestBase* Test,
		const TSharedPtr<FJsonObject>& Object,
		const TCHAR* Field,
		const FString& ScenarioId)
	{
		FString Value;
		return Test->TestTrue(
			FString::Printf(TEXT("%s is explicit: %s"), Field, *ScenarioId),
			Object.IsValid()
				&& Object->TryGetStringField(Field, Value)
				&& !Value.IsEmpty());
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentWorldSpatialNativeScenarioContractTest,
	"UeAgentInterface.DomainAdapters.WorldSpatial.E2.NativeScenarioContract",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentWorldSpatialNativeScenarioContractTest::RunTest(
	const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Fixture = LoadFixture(this);
	if (!Fixture.IsValid())
	{
		return false;
	}
	TestEqual(
		TEXT("fixture schema"),
		Fixture->GetStringField(TEXT("schema")),
		FString(TEXT(
			"uai.domain_adapter.world_spatial.native_scenario_fixture.v1")));
	const TArray<TSharedPtr<FJsonValue>>& Scenarios =
		Fixture->GetArrayField(TEXT("scenarios"));

	TSet<FString> ScenarioIds;
	TSet<FString> Capabilities;
	TSet<FString> TestIds;
	for (const TSharedPtr<FJsonValue>& Value : Scenarios)
	{
		const TSharedPtr<FJsonObject> Scenario = Value->AsObject();
		if (!TestTrue(TEXT("scenario is an object"), Scenario.IsValid()))
		{
			continue;
		}
		const FString ScenarioId =
			Scenario->GetStringField(TEXT("scenario_id"));
		TestFalse(
			TEXT("scenario identity is unique: ") + ScenarioId,
			ScenarioIds.Contains(ScenarioId));
		ScenarioIds.Add(ScenarioId);
		const FString TestId =
			Scenario->GetStringField(TEXT("test_id"));
		TestFalse(
			TEXT("executable test identity is unique: ") + TestId,
			TestIds.Contains(TestId));
		TestIds.Add(TestId);
		NonEmpty(this, Scenario, TEXT("family_id"), ScenarioId);
		const FString Lane = Scenario->GetStringField(TEXT("lane"));
		TestTrue(
			TEXT("scenario lane is explicit: ") + ScenarioId,
			Lane == TEXT("nullrhi") || Lane == TEXT("rhi"));
		NonEmpty(this, Scenario, TEXT("subject"), ScenarioId);
		NonEmpty(this, Scenario, TEXT("dimension"), ScenarioId);
		NonEmpty(this, Scenario, TEXT("semantic_frame"), ScenarioId);
		NonEmpty(this, Scenario, TEXT("write_frame"), ScenarioId);
		NonEmpty(this, Scenario, TEXT("read_frame"), ScenarioId);
		NonEmpty(this, Scenario, TEXT("verify_frame"), ScenarioId);
		NonEmpty(this, Scenario, TEXT("setup"), ScenarioId);
		NonEmpty(this, Scenario, TEXT("write"), ScenarioId);
		NonEmpty(this, Scenario, TEXT("readback"), ScenarioId);
		NonEmpty(this, Scenario, TEXT("cleanup"), ScenarioId);
		const TArray<TSharedPtr<FJsonValue>>& Conversion =
			Scenario->GetArrayField(TEXT("conversion_chain"));
		TestTrue(
			TEXT("conversion chain is explicit: ") + ScenarioId,
			!Conversion.IsEmpty());
		for (const TSharedPtr<FJsonValue>& Capability
			: Scenario->GetArrayField(TEXT("capabilities")))
		{
			const FString CapabilityId = Capability->AsString();
			TestFalse(
				TEXT("capability has one E2 owner: ") + CapabilityId,
				Capabilities.Contains(CapabilityId));
			Capabilities.Add(CapabilityId);
		}
	}
	TestTrue(
		TEXT("deprojection scenario names the dimension bridge"),
		Fixture->GetArrayField(TEXT("scenarios"))[5]
			->AsObject()
			->GetArrayField(TEXT("conversion_chain"))[1]
			->AsString()
			== TEXT("camera_deprojection_dimension_bridge"));
	return true;
}

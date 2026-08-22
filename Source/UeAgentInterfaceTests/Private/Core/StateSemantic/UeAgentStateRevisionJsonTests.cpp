#include "Misc/AutomationTest.h"

#include "Core/StateSemantic/UeAgentStateRevisionJson.h"

using namespace UE::UAI::StateSemantic;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStateRevisionJsonExactnessTest,
	"UeAgentInterface.Core.StateSemantic.StateRevisionJsonExactness",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentStateRevisionJsonExactnessTest::RunTest(
	const FString& Parameters)
{
	const FUeAgentStateRevision Expected(
		TEXT("uai://state/revision-fixture"),
		TEXT("fixture-generation"),
		FUeAgentStateRevisionJson::MaxExactJsonSequence);
	TOptional<FUeAgentStateRevision> Parsed;
	TestTrue(
		TEXT("maximum exact JSON revision round-trips"),
		FUeAgentStateRevisionJson::TryParse(
			FUeAgentStateRevisionJson::ToJson(Expected),
			Parsed));
	TestTrue(
		TEXT("round-tripped revision remains exact"),
		Parsed.IsSet() && Parsed.GetValue() == Expected);

	const TSharedPtr<FJsonObject> Fractional =
		FUeAgentStateRevisionJson::ToJson(Expected);
	Fractional->SetNumberField(TEXT("sequence"), 1.5);
	TestFalse(
		TEXT("fractional revision sequence is rejected"),
		FUeAgentStateRevisionJson::TryParse(Fractional, Parsed));

	const TSharedPtr<FJsonObject> Inexact =
		FUeAgentStateRevisionJson::ToJson(Expected);
	Inexact->SetNumberField(
		TEXT("sequence"),
		9007199254740992.0);
	TestFalse(
		TEXT("inexact JSON revision sequence is rejected"),
		FUeAgentStateRevisionJson::TryParse(Inexact, Parsed));
	return !HasAnyErrors();
}

#include "Misc/AutomationTest.h"

#include "Core/StateSemantic/UeAgentStateRevisionJson.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "Dom/JsonValue.h"

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
	const auto Resources = MakeShared<UE::UAI::ContextResource::FUeAgentResourceTracker, ESPMode::ThreadSafe>();
	for (const FString& Lexeme : {FString(TEXT("1.0000000000000000001")), FString(TEXT("1e0"))})
	{
		const bool bIntegral = Lexeme == TEXT("1e0");
		Fractional->SetField(TEXT("sequence"), MakeShared<FJsonValueNumberString>(Lexeme));
		TestEqual(TEXT("JSON sequence keeps exact integral semantics"),
			FUeAgentStateRevisionJson::TryParse(Fractional, Parsed), bIntegral);
		FUeAgentCanonicalValuePtr Canonical;
		FString Error;
		TestTrue(TEXT("canonical conversion"), CanonicalJson::TryFromJsonValue(
			MakeShared<FJsonValueObject>(Fractional), *Resources, Canonical, Error));
		TestEqual(TEXT("canonical sequence keeps exact integral semantics"),
			FUeAgentStateRevisionJson::TryReadCanonical(Canonical, Parsed), bIntegral);
	}
	TestTrue(TEXT("unbounded integer kind is not a native-width test"), IsIntegralNumberLexeme(TEXT("1e100")));
	TestFalse(TEXT("fraction must not be rounded into integer kind"), IsIntegralNumberLexeme(TEXT("7.0000000000000000001")));
	return !HasAnyErrors();
}

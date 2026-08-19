#include "Misc/AutomationTest.h"

#include "Core/ContextResource/UeAgentContextResource.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace
{
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::StateSemantic;

	FUeAgentResourceTracker MakeCanonicalJsonTracker()
	{
		return FUeAgentResourceTracker(FUeAgentResourceBudget({
			{EUeAgentResourceDimension::InputBytes, 4 * 1024 * 1024},
			{EUeAgentResourceDimension::OutputBytes, 4 * 1024 * 1024},
			{EUeAgentResourceDimension::StringBytes, 4 * 1024 * 1024},
			{EUeAgentResourceDimension::Nodes, 32768},
			{EUeAgentResourceDimension::Items, 32768},
			{EUeAgentResourceDimension::ReferenceEdges, 65536},
			{EUeAgentResourceDimension::WorkUnits, 262144},
			{EUeAgentResourceDimension::EstimatedMemoryBytes, 16 * 1024 * 1024},
		}));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentCanonicalValueJsonRoundTripTest,
	"UeAgentInterface.Core.StateSemantic.CanonicalJson.RoundTripWithoutFixedDepth",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentCanonicalValueJsonRoundTripTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FUeAgentResourceTracker Resources = MakeCanonicalJsonTracker();
	TSharedPtr<FJsonValue> Json = MakeShared<FJsonValueString>(TEXT("leaf"));
	for (int32 Index = 0; Index < 512; ++Index)
	{
		const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetField(TEXT("child"), Json);
		Json = MakeShared<FJsonValueObject>(Object);
	}

	FUeAgentCanonicalValuePtr Canonical;
	FString Error;
	TestTrue(
		TEXT("JSON converts to canonical value using resource bounds, not a fixed depth cap"),
		CanonicalJson::TryFromJsonValue(Json, Resources, Canonical, Error));
	TestTrue(TEXT("canonical root is valid"), Canonical.IsValid());

	TSharedPtr<FJsonValue> RoundTrip;
	TestTrue(
		TEXT("deep canonical value converts back to JSON"),
		Canonical.IsValid()
			&& CanonicalJson::TryToJsonValue(*Canonical, RoundTrip, Error));
	TestTrue(TEXT("round-trip root is valid"), RoundTrip.IsValid());
	return !HasAnyErrors();
}

#include "Misc/AutomationTest.h"

#include "Composition/HjsonRoundTrip/UeAgentHjsonRoundTripApplication.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentHjsonRoundTripApplicationModuleAvailableTest,
	"UeAgentInterface.Composition.HjsonRoundTrip.ModuleAvailable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentHjsonRoundTripApplicationModuleAvailableTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI::Composition::HjsonRoundTrip;

	TestEqual(
		TEXT(
			"HJSON Import is one composition capability rather than "
			"a second domain operation descriptor"),
		GetHjsonImportCapabilityId(),
		FString(TEXT("uai.hjson.import")));
	TestFalse(
		TEXT("the shared HJSON Import authority is not reference-only"),
		GetHjsonImportRequiredAuthorityScope().Contains(
			TEXT("reference"),
			ESearchCase::IgnoreCase));
	TestTrue(
		TEXT("directory source is accepted by the shared decoder"),
		IsSupportedHjsonImportSourceKind(TEXT("directory")));
	TestTrue(
		TEXT("single-file source is accepted by the shared decoder"),
		IsSupportedHjsonImportSourceKind(TEXT("single_file")));
	TestTrue(
		TEXT("inline source is accepted by the shared decoder"),
		IsSupportedHjsonImportSourceKind(TEXT("inline")));
	return !HasAnyErrors();
}

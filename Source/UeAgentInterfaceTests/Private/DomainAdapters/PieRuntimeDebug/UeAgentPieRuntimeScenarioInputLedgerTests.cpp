#include "Misc/AutomationTest.h"

#include "DomainAdapters/PieRuntimeDebug/UeAgentPieRuntimeScenarioProduction.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentPieRuntimeScenarioInputLedgerTest,
	"UeAgentInterface.DomainAdapters.PieRuntimeDebug.InputLedger",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentPieRuntimeScenarioInputLedgerTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace UE::UAI::DomainAdapters::PieRuntimeDebug;

	FUeAgentPieInjectedInputLedger Ledger;
	const FKey Digital = EKeys::One;
	const FKey ExplicitlyReleased = EKeys::Two;
	const FKey Axis = EKeys::Gamepad_LeftX;
	const FKey ExplicitlyZeroedAxis = EKeys::Gamepad_RightX;

	Ledger.RecordSuccessfulInjection(Digital, IE_Pressed, 1.0);
	Ledger.RecordSuccessfulInjection(Digital, IE_Repeat, 1.0);
	Ledger.RecordSuccessfulInjection(Digital, IE_Repeat, 1.0);
	Ledger.RecordSuccessfulInjection(ExplicitlyReleased, IE_Pressed, 1.0);
	Ledger.RecordSuccessfulInjection(ExplicitlyReleased, IE_Released, 0.0);
	Ledger.RecordSuccessfulInjection(Axis, IE_Axis, 0.25);
	Ledger.RecordSuccessfulInjection(Axis, IE_Axis, -0.75);
	Ledger.RecordSuccessfulInjection(ExplicitlyZeroedAxis, IE_Axis, 0.5);
	Ledger.RecordSuccessfulInjection(ExplicitlyZeroedAxis, IE_Axis, 0.0);

	const TArray<FUeAgentPieInjectedInputCleanup> Cleanup =
		Ledger.ConsumeCleanupActions();
	TestEqual(TEXT("one digital release and one axis zero"), Cleanup.Num(), 2);
	TestEqual(TEXT("digital cleanup key"), Cleanup[0].Key, Digital);
	TestEqual(TEXT("digital cleanup event"), Cleanup[0].Event, IE_Released);
	TestEqual(TEXT("digital cleanup amount"), Cleanup[0].Amount, 0.0f);
	TestEqual(TEXT("axis cleanup key"), Cleanup[1].Key, Axis);
	TestEqual(TEXT("axis cleanup event"), Cleanup[1].Event, IE_Axis);
	TestEqual(TEXT("axis cleanup amount"), Cleanup[1].Amount, 0.0f);
	TestTrue(TEXT("consume clears operation ledger"), Ledger.IsEmpty());
	TestTrue(TEXT("second cleanup is idempotent"),
		Ledger.ConsumeCleanupActions().IsEmpty());
	return !HasAnyErrors();
}

#include "Misc/AutomationTest.h"

#include "DomainAdapters/PieRuntimeDebug/UeAgentPieRuntimeScenarioProduction.h"

namespace
{
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::StateSemantic;

	FUeAgentResourceTracker MakeResources()
	{
		return FUeAgentResourceTracker();
	}

	FUeAgentCanonicalValueRef StringValue(const FString& Text, FUeAgentResourceTracker& Resources)
	{
		FUeAgentCanonicalValuePtr Value;
		FUeAgentStateSemanticError Error;
		check(FUeAgentCanonicalValue::TryString(Text, Resources, Value, Error));
		return Value.ToSharedRef();
	}

	FUeAgentCanonicalValueRef DecimalValue(const FString& Lexeme, FUeAgentResourceTracker& Resources)
	{
		FUeAgentCanonicalValuePtr Value;
		FUeAgentStateSemanticError Error;
		check(FUeAgentCanonicalValue::TryDecimal(Lexeme, Resources, Value, Error));
		return Value.ToSharedRef();
	}

	FUeAgentCanonicalValueRef BooleanValue(const bool bValue, FUeAgentResourceTracker& Resources)
	{
		FUeAgentCanonicalValuePtr Value;
		FUeAgentStateSemanticError Error;
		check(FUeAgentCanonicalValue::TryBoolean(bValue, Resources, Value, Error));
		return Value.ToSharedRef();
	}

	FUeAgentCanonicalValueRef ArrayValue(
		const TArray<FString>& Lexemes,
		FUeAgentResourceTracker& Resources)
	{
		TArray<FUeAgentCanonicalValueRef> Items;
		for (const FString& Lexeme : Lexemes) Items.Add(DecimalValue(Lexeme, Resources));
		FUeAgentCanonicalValuePtr Value;
		FUeAgentStateSemanticError Error;
		check(FUeAgentCanonicalValue::TryArray(Items, Resources, Value, Error));
		return Value.ToSharedRef();
	}

	FUeAgentCanonicalValueRef StepValue(
		const FString& ActionRef,
		const double AtSeconds,
		FUeAgentCanonicalValueRef Value,
		FUeAgentResourceTracker& Resources)
	{
		FUeAgentCanonicalValuePtr Object;
		FUeAgentStateSemanticError Error;
		check(FUeAgentCanonicalValue::TryObject({
			{TEXT("type"), StringValue(TEXT("input_action"), Resources)},
			{TEXT("at_seconds"), DecimalValue(LexToString(AtSeconds), Resources)},
			{TEXT("action_ref"), StringValue(ActionRef, Resources)},
			{TEXT("value"), Value}}, Resources, Object, Error));
		return Object.ToSharedRef();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentPieRuntimeScenarioInputActionTest,
	"UeAgentInterface.DomainAdapters.PieRuntimeDebug.InputActionStateMachine",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentPieRuntimeScenarioInputActionTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace UE::UAI::DomainAdapters::PieRuntimeDebug;
	auto Resources = MakeResources();
	const FString A = TEXT("/Game/Input/IA_Move.IA_Move");
	const FString B = TEXT("/Game/Input/IA_Look.IA_Look");
	FUeAgentPieInputActionSample Sample;
	UE::UAI::Contract::FDiagnostic Error;

	TestTrue(TEXT("bool sample parses"), TryParsePieInputActionSample(
		StepValue(A, 0.0, BooleanValue(true, Resources), Resources), {}, Sample, Error));
	TestEqual(TEXT("bool type"), Sample.ValueType, EInputActionValueType::Boolean);
	TestTrue(TEXT("bool nonzero"), Sample.IsNonZero());
	TestTrue(TEXT("Axis1D parses"), TryParsePieInputActionSample(
		StepValue(A, 0.1, DecimalValue(TEXT("0.25"), Resources), Resources), {}, Sample, Error));
	TestEqual(TEXT("Axis1D type"), Sample.ValueType, EInputActionValueType::Axis1D);
	TestTrue(TEXT("Axis2D parses"), TryParsePieInputActionSample(
		StepValue(A, 0.2, ArrayValue({TEXT("1"), TEXT("-1")}, Resources), Resources), {}, Sample, Error));
	TestEqual(TEXT("Axis2D type"), Sample.ValueType, EInputActionValueType::Axis2D);
	TestTrue(TEXT("Axis3D parses"), TryParsePieInputActionSample(
		StepValue(A, 0.3, ArrayValue({TEXT("1"), TEXT("2"), TEXT("3")}, Resources), Resources), {}, Sample, Error));
	TestEqual(TEXT("Axis3D type"), Sample.ValueType, EInputActionValueType::Axis3D);
	TestTrue(TEXT("plugin action object path is accepted by the parser"),
		TryParsePieInputActionSample(
			StepValue(TEXT("/InputPlugin/Actions/IA_Custom.IA_Custom"), 0.35,
				DecimalValue(TEXT("0.5"), Resources), Resources), {}, Sample, Error));
	TestFalse(TEXT("wrong array length rejected"), TryParsePieInputActionSample(
		StepValue(A, 0.4, ArrayValue({TEXT("1")}, Resources), Resources), {}, Sample, Error));
	TestTrue(TEXT("a small finite analog value is not silently treated as stop"),
		TryParsePieInputActionSample(
			StepValue(A, 0.45, DecimalValue(TEXT("0.000001"), Resources), Resources),
			{}, Sample, Error));
	TestTrue(TEXT("small analog value remains active"), Sample.IsNonZero());
	TestTrue(TEXT("zero sample parses"), TryParsePieInputActionSample(
		StepValue(A, 0.5, DecimalValue(TEXT("0"), Resources), Resources), {}, Sample, Error));
	TestFalse(TEXT("zero sample is zero"), Sample.IsNonZero());
	const FUeAgentPieInputActionSample Existing{A, 1.0, EInputActionValueType::Axis1D, FVector(1, 0, 0)};
	TestTrue(TEXT("same action and time accepted in declaration order"), TryParsePieInputActionSample(
		StepValue(A, 1.0, DecimalValue(TEXT("0.5"), Resources), Resources), {Existing}, Sample, Error));

	FUeAgentPieInputActionOperationLedger Ledger;
	Ledger.ApplySample(Existing);
	Ledger.ApplySample({A, 1.25, EInputActionValueType::Axis1D, FVector(0.5, 0, 0)});
	TestEqual(TEXT("later sample updates same action"),
		Ledger.GetActiveSamples().FindChecked(A).Value.X, 0.5);
	Ledger.ApplySample({B, 1.0, EInputActionValueType::Axis2D, FVector(1, 1, 0)});
	TestEqual(TEXT("two actions combine"), Ledger.GetActiveSamples().Num(), 2);
	Ledger.ApplySample({A, 1.5, EInputActionValueType::Axis1D, FVector(0, 0, 0)});
	TestEqual(TEXT("zero sample stops one action"), Ledger.GetActiveSamples().Num(), 1);
	Ledger.BeginRelease(10.0, 100);
	TestTrue(TEXT("release stops reinjection"), Ledger.IsReinjectionStopped());
	TestEqual(TEXT("release pending before zero observation"),
		Ledger.EvaluateRelease(10.1, 101), EUeAgentPieInputActionReleaseStatus::Pending);
	Ledger.RecordObservedValue(A, false);
	Ledger.RecordObservedValue(B, false);
	TestEqual(TEXT("release is verified even when triggers never produced a nonzero value"),
		Ledger.EvaluateRelease(10.2, 102), EUeAgentPieInputActionReleaseStatus::Verified);
	Ledger.BeginRelease(99.0, 999);
	TestTrue(TEXT("second cleanup is idempotent"), Ledger.IsReinjectionStopped());

	FUeAgentPieInputActionOperationLedger Unverified;
	Unverified.ApplySample(Existing);
	Unverified.BeginRelease(20.0, 200);
	TestEqual(TEXT("exact grace boundary remains pending"),
		Unverified.EvaluateRelease(22.0, 320), EUeAgentPieInputActionReleaseStatus::Pending);
	TestEqual(TEXT("wall grace overflow is unverified"),
		Unverified.EvaluateRelease(22.001, 320), EUeAgentPieInputActionReleaseStatus::Unverified);
	FUeAgentPieInputActionOperationLedger FrameUnverified;
	FrameUnverified.ApplySample(Existing);
	FrameUnverified.BeginRelease(30.0, 400);
	TestEqual(TEXT("frame grace overflow is unverified"),
		FrameUnverified.EvaluateRelease(30.1, 521), EUeAgentPieInputActionReleaseStatus::Unverified);

	FUeAgentCanonicalValuePtr NonFinite;
	FUeAgentStateSemanticError SemanticError;
	const bool bNonFiniteCanonical = FUeAgentCanonicalValue::TryDecimal(
		TEXT("nan"), Resources, NonFinite, SemanticError);
	if (bNonFiniteCanonical && NonFinite.IsValid())
	{
		TestFalse(TEXT("parser rejects non-finite canonical number"),
			TryParsePieInputActionSample(
				StepValue(A, 2.0, NonFinite.ToSharedRef(), Resources), {}, Sample, Error));
	}
	else
	{
		TestTrue(TEXT("canonical layer rejects non-finite number"), true);
	}
	return !HasAnyErrors();
}

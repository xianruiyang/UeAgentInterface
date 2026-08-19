#include "Core/ContextResource/UeAgentContextResource.h"

#include "Misc/AutomationTest.h"

namespace
{
	using namespace UE::UAI::ContextResource;

	static TMap<EUeAgentResourceDimension, int64>
	MakeResourceLimits()
	{
		return {
			{EUeAgentResourceDimension::InputBytes, 100},
			{EUeAgentResourceDimension::OutputBytes, 100},
			{EUeAgentResourceDimension::StringBytes, 100},
			{EUeAgentResourceDimension::Nodes, 10},
			{EUeAgentResourceDimension::Items, 10},
			{EUeAgentResourceDimension::Files, 10},
			{EUeAgentResourceDimension::ReferenceEdges, 10},
			{EUeAgentResourceDimension::WorkUnits, 10},
			{EUeAgentResourceDimension::EstimatedMemoryBytes, 1000},
		};
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentContextResourceBudgetContractTest,
	"UeAgentInterface.Core.ContextResource.ResourceBudget",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentContextResourceBudgetContractTest::RunTest(const FString& Parameters)
{
	using namespace UE::UAI::ContextResource;

	const TMap<EUeAgentResourceDimension, int64> Limits =
		MakeResourceLimits();
	TestEqual(TEXT("nine explicit resource dimensions"), Limits.Num(), 9);
	for (const TPair<EUeAgentResourceDimension, int64>& Pair : Limits)
	{
		const FString Name = LexToString(Pair.Key);
		TestFalse(TEXT("resource dimension is not depth"), Name.Contains(TEXT("depth")));
		TestNotEqual(TEXT("resource dimension is not output_chars"), Name, FString(TEXT("output_chars")));
	}

	FUeAgentResourceTracker ExactTracker(FUeAgentResourceBudget({
		{EUeAgentResourceDimension::Nodes, 2},
	}));
	FUeAgentResourceBudgetExceeded Error;
	TestTrue(
		TEXT("exact resource limit is allowed"),
		ExactTracker.TryCharge(EUeAgentResourceDimension::Nodes, 2, TEXT("/root"), Error));
	TestFalse(
		TEXT("N+1 is rejected before work"),
		ExactTracker.TryCharge(EUeAgentResourceDimension::Nodes, 1, TEXT("/root/child"), Error));
	TestEqual(TEXT("budget diagnostic code"), Error.Code, FString(TEXT("resource_budget_exceeded")));
	TestEqual(TEXT("budget diagnostic dimension"), Error.Dimension, EUeAgentResourceDimension::Nodes);
	TestEqual(TEXT("budget diagnostic limit"), Error.Limit, int64(2));
	TestEqual(TEXT("budget diagnostic observed"), Error.Observed, int64(3));
	TestEqual(TEXT("budget diagnostic path"), Error.Path, FString(TEXT("/root/child")));
	TestTrue(TEXT("budget stops early"), Error.bStoppedEarly);
	TestEqual(
		TEXT("rejected charge does not mutate usage"),
		ExactTracker.Snapshot().Consumed(EUeAgentResourceDimension::Nodes),
		int64(2));

	FUeAgentResourceTracker AtomicTracker(FUeAgentResourceBudget({
		{EUeAgentResourceDimension::Nodes, 2},
		{EUeAgentResourceDimension::Items, 1},
	}));
	const TMap<EUeAgentResourceDimension, int64> RejectedBatch = {
		{EUeAgentResourceDimension::Nodes, 1},
		{EUeAgentResourceDimension::Items, 2},
	};
	TestFalse(
		TEXT("multi-dimension rejection is reported"),
		AtomicTracker.TryChargeMany(RejectedBatch, TEXT("/batch"), Error));
	TestEqual(
		TEXT("atomic rejection keeps nodes at zero"),
		AtomicTracker.Snapshot().Consumed(EUeAgentResourceDimension::Nodes),
		int64(0));
	TestEqual(
		TEXT("atomic rejection keeps items at zero"),
		AtomicTracker.Snapshot().Consumed(EUeAgentResourceDimension::Items),
		int64(0));

	FUeAgentResourceTracker ScopedTracker{FUeAgentResourceBudget(Limits)};
	FUeAgentResourceScope First = ScopedTracker.Scope(TEXT("/source"));
	FUeAgentResourceScope Second = ScopedTracker.Scope(TEXT("/source/items"));
	TestTrue(
		TEXT("first scope charges shared tracker"),
		First.TryChargeMany({
			{EUeAgentResourceDimension::InputBytes, 12},
			{EUeAgentResourceDimension::Nodes, 2},
			{EUeAgentResourceDimension::WorkUnits, 3},
		}, Error));
	TestTrue(
		TEXT("second scope charges shared tracker"),
		Second.TryChargeMany({
			{EUeAgentResourceDimension::Items, 4},
			{EUeAgentResourceDimension::StringBytes, 6},
			{EUeAgentResourceDimension::WorkUnits, 2},
		}, Error));
	const FUeAgentResourceUsage Usage = ScopedTracker.Snapshot();
	TestEqual(TEXT("scoped input bytes"), Usage.Consumed(EUeAgentResourceDimension::InputBytes), int64(12));
	TestEqual(TEXT("scoped nodes"), Usage.Consumed(EUeAgentResourceDimension::Nodes), int64(2));
	TestEqual(TEXT("scoped items"), Usage.Consumed(EUeAgentResourceDimension::Items), int64(4));
	TestEqual(TEXT("scoped work units"), Usage.Consumed(EUeAgentResourceDimension::WorkUnits), int64(5));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentContextResourceTextParityTest,
	"UeAgentInterface.Core.ContextResource.TextParity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentContextResourceTextParityTest::RunTest(const FString& Parameters)
{
	using namespace UE::UAI::ContextResource;

	const FUeAgentTextUnits Ascii = MeasureText(TEXT("A"));
	TestEqual(TEXT("ASCII UTF-8 bytes"), Ascii.Utf8Bytes, int64(1));
	TestEqual(
		TEXT("ASCII UTF-16 code units"),
		Ascii.Utf16CodeUnits,
		int64(1));
	TestEqual(
		TEXT("ASCII Unicode scalars"),
		Ascii.UnicodeScalars,
		int64(1));

	const FUeAgentTextUnits Han = MeasureText(TEXT("\u6C49"));
	TestEqual(TEXT("Han UTF-8 bytes"), Han.Utf8Bytes, int64(3));
	TestEqual(
		TEXT("Han UTF-16 code units"),
		Han.Utf16CodeUnits,
		int64(1));
	TestEqual(
		TEXT("Han Unicode scalars"),
		Han.UnicodeScalars,
		int64(1));

	FString Supplementary;
	Supplementary.AppendChar(static_cast<TCHAR>(0xD83D));
	Supplementary.AppendChar(static_cast<TCHAR>(0xDE00));
	const FUeAgentTextUnits Emoji = MeasureText(Supplementary);
	TestEqual(TEXT("emoji UTF-8 bytes"), Emoji.Utf8Bytes, int64(4));
	TestEqual(
		TEXT("emoji UTF-16 code units"),
		Emoji.Utf16CodeUnits,
		int64(2));
	TestEqual(
		TEXT("emoji Unicode scalars"),
		Emoji.UnicodeScalars,
		int64(1));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentContextResourceDeadlineTest,
	"UeAgentInterface.Core.ContextResource.Deadline",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentContextResourceDeadlineTest::RunTest(const FString& Parameters)
{
	using namespace UE::UAI::ContextResource;

	double Now = 100.0;
	FUeAgentOperationDeadline Deadline = FUeAgentOperationDeadline::FromTimeout(
		5.0,
		[&Now]() { return Now; });
	TestEqual(TEXT("deadline started_at"), Deadline.GetStartedAt(), 100.0);
	TestEqual(TEXT("deadline is absolute"), Deadline.GetDeadlineAt(), 105.0);
	Now = 104.999;
	TestTrue(TEXT("before deadline can continue"), Deadline.CanContinue());
	TestTrue(TEXT("remaining time is positive"), Deadline.RemainingSeconds() > 0.0);

	Now = 105.0;
	FUeAgentOperationDeadlineExceeded Error;
	TestFalse(TEXT("exact deadline is expired"), Deadline.TryCheckpoint(TEXT("preflight"), Error));
	TestEqual(TEXT("deadline diagnostic code"), Error.Code, FString(TEXT("operation_deadline_exceeded")));
	TestEqual(TEXT("deadline diagnostic stage"), Error.Stage, FString(TEXT("preflight")));
	TestEqual(TEXT("deadline diagnostic deadline_at"), Error.DeadlineAt, 105.0);
	TestEqual(TEXT("deadline diagnostic observed_at"), Error.ObservedAt, 105.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentContextResourceCancellationTest,
	"UeAgentInterface.Core.ContextResource.Cancellation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentContextResourceCancellationTest::RunTest(const FString& Parameters)
{
	using namespace UE::UAI::ContextResource;

	double Now = 103.0;
	FUeAgentCancellationSource Source([&Now]() { return Now; });
	const FUeAgentCancellationToken Token = Source.GetToken();
	const FUeAgentCancellationRequest& First = Source.Request(TEXT("user_requested"));
	Now = 113.0;
	const FUeAgentCancellationRequest& Second = Source.Request(
		TEXT("disconnect_should_not_replace_explicit_request"));
	TestTrue(TEXT("first cancellation request remains stable"), &First == &Second);
	TestTrue(TEXT("token is cancelled"), Token.IsCancelled());
	TestEqual(TEXT("first cancellation reason remains"), Token.GetRequest()->Reason, FString(TEXT("user_requested")));
	TestEqual(TEXT("first cancellation time remains"), Token.GetRequest()->RequestedAt, 103.0);

	FUeAgentCancellationRequested Error;
	TestFalse(TEXT("cancelled token rejects checkpoint"), Token.TryCheckpoint(Error));
	TestEqual(TEXT("cancellation diagnostic code"), Error.Code, FString(TEXT("cancellation_requested")));
	TestEqual(TEXT("cancellation diagnostic reason"), Error.Reason, FString(TEXT("user_requested")));
	return true;
}

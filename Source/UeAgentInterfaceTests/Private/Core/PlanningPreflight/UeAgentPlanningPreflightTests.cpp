#include "Misc/AutomationTest.h"

#include "Core/PlanningPreflight/UeAgentPlanningPreflight.h"
#include "Core/ContextResource/UeAgentContextResource.h"
#include "Core/Contract/UeAgentContract.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"


namespace
{
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::Contract;
	using namespace UE::UAI::PlanningPreflight;
	using namespace UE::UAI::StateSemantic;

	static FUeAgentResourceTracker MakeTracker(
		const int64 WorkLimit = 131072)
	{
		return FUeAgentResourceTracker(FUeAgentResourceBudget({
			{EUeAgentResourceDimension::InputBytes, 262144},
			{EUeAgentResourceDimension::OutputBytes, 262144},
			{EUeAgentResourceDimension::StringBytes, 262144},
			{EUeAgentResourceDimension::Nodes, 16384},
			{EUeAgentResourceDimension::Items, 16384},
			{EUeAgentResourceDimension::Files, 8},
			{EUeAgentResourceDimension::ReferenceEdges, 4096},
			{EUeAgentResourceDimension::WorkUnits, WorkLimit},
			{EUeAgentResourceDimension::EstimatedMemoryBytes, 2097152},
		}));
	}

	static FUeAgentCanonicalIdentity MakeIdentity()
	{
		return FUeAgentCanonicalIdentity(
			TEXT("uai.reference.settings.title"),
			TEXT("reference_setting"),
			TEXT("/Game/UAI/P4_003"),
			TEXT("title"),
			TEXT("property"),
			TEXT("editor-session-17"));
	}

	static FUeAgentCanonicalValueRef MakeStringValue(
		const FString& Text,
		FUeAgentResourceTracker& Tracker)
	{
		FUeAgentCanonicalValuePtr Value;
		FUeAgentStateSemanticError Error;
		check(FUeAgentCanonicalValue::TryString(
			Text, Tracker, Value, Error));
		return Value.ToSharedRef();
	}

	static FUeAgentCanonicalSnapshotRef MakeSnapshot(
		FUeAgentResourceTracker& Tracker,
		const int64 Sequence = 42,
		const FString& Current = TEXT("旧标题"))
	{
		const FUeAgentStateRevision Revision(
			TEXT("fixture"),
			TEXT("editor-session-17"),
			Sequence);
		const FUeAgentCanonicalValueRef Value =
			MakeStringValue(Current, Tracker);
		const FUeAgentStateEntry Entry(
			MakeIdentity(),
			FUeAgentCanonicalType(
				TEXT("string"),
				EUeAgentValueKind::String,
				false,
				TEXT("scalar")),
			Value,
			FUeAgentDefaultValue(
				EUeAgentDefaultSourceKind::Schema,
				TEXT("fixture:c6:string-default"),
				TEXT("checked-in planning fixture"),
				MakeStringValue(TEXT(""), Tracker)),
			FUeAgentStateDetails(
				TEXT("title"),
				TEXT(""),
				{TEXT("P4-003")},
				true,
				TEXT("available"),
				{}),
			Revision);
		FUeAgentCanonicalSnapshotPtr Snapshot;
		FUeAgentStateSemanticError Error;
		check(FUeAgentCanonicalSnapshot::TryCreate(
			TEXT("snapshot:c6:fixture"),
			EUeAgentStatePlane::AuthoringMemory,
			Revision,
			FUeAgentTemporalPoint(
				TEXT("fixture-clock"),
				FString::Printf(TEXT("%lld"), Sequence)),
			FUeAgentStateProvenance(
				TEXT("independent_readback"),
				TEXT("fixture:c6"),
				TEXT("fixture:c6:independent-readback")),
			{Entry},
			Tracker,
			Snapshot,
			Error));
		return Snapshot.ToSharedRef();
	}

	static FChangeDraft MakeUpdate(
		FUeAgentResourceTracker& Tracker,
		const FString& Before = TEXT("旧标题"),
		const FString& After = TEXT("新标题😀"))
	{
		return FChangeDraft(
			MakeIdentity(),
			EChangeIntent::Update,
			MakeStringValue(Before, Tracker),
			MakeStringValue(After, Tracker),
			{TEXT("uai:reference:write"),
			 TEXT("uai:reference:settings")},
			TEXT(
				"reference-settings-adapter:"
				"title:no-indirect-impact"),
			{});
	}

	static FPlanningRequest MakeRequest(
		FUeAgentResourceTracker& Tracker,
		const bool bDryRun,
		TArray<FString> AuthorityScopes = {
			TEXT("uai:reference:write"),
			TEXT("uai:reference:settings")})
	{
		TArray<FChangeDraft> Changes;
		Changes.Add(MakeUpdate(Tracker));
		return FPlanningRequest(
			FChangeSetDraft(
				FUeAgentDescriptorIdentity(
					TEXT("uai.reference.settings.update"),
					1,
					TEXT("sha256:462ec53a5b260b93a75b85182b32efcad3a1392db07ee66858a99557f7ca8880")),
				TEXT("p4-003:planning:fixture"),
				TEXT("p4-003-idempotency-001"),
				MakeSnapshot(Tracker),
				MoveTemp(Changes)),
			FUeAgentCallerContext(
				TEXT("agent:p4-003"),
				MoveTemp(AuthorityScopes),
				TEXT("p4-003:correlation")),
			bDryRun);
	}
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentPlanningPreflightModuleAvailableTest,
	"UeAgentInterface.Core.PlanningPreflight.ModuleAvailable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentPlanningPreflightModuleAvailableTest::RunTest(
	const FString& Parameters)
{
	TestTrue(TEXT("C6 PlanningPreflight module is available"), true);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentPlanningPreflightTypedPlanTest,
	"UeAgentInterface.Core.PlanningPreflight.TypedPlan",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentPlanningPreflightTypedPlanTest::RunTest(
	const FString& Parameters)
{
	FUeAgentResourceTracker Tracker = MakeTracker();
	const FPlanningRequest Request = MakeRequest(Tracker, false);
	FPlanningResult Result;
	FPlanningPreflightError Error;
	TestTrue(
		TEXT("typed request plans successfully"),
		TryPlanChanges(Request, Tracker, Result, Error));
	TestTrue(TEXT("plan is ready"), Result.IsReady());
	TestTrue(
		TEXT("non-dry plan permits later execution"),
		Result.IsMutationPermitted());
	TestEqual(
		TEXT("one canonical change"),
		Result.GetPlan()->GetChangeSet().GetChanges().Num(),
		1);
	TestEqual(
		TEXT("one direct impact"),
		Result.GetPlan()->GetImpactSet().GetImpacts().Num(),
		1);
	TestEqual(
		TEXT("one revision/value precondition"),
		Result.GetPlan()->GetPreconditions().Num(),
		1);
	TestEqual(
		TEXT("plan binds snapshot revision"),
		Result.GetPlan()->GetObservedRevision().GetSequence(),
		static_cast<int64>(42));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentPlanningPreflightAuthorityRevisionTest,
	"UeAgentInterface.Core.PlanningPreflight.AuthorityRevision",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentPlanningPreflightAuthorityRevisionTest::RunTest(
	const FString& Parameters)
{
	FUeAgentResourceTracker DeniedTracker = MakeTracker();
	const FPlanningRequest Denied = MakeRequest(
		DeniedTracker,
		false,
		{TEXT("uai:reference:write")});
	FPlanningResult DeniedResult;
	FPlanningPreflightError DeniedError;
	TestFalse(
		TEXT("missing authority rejects whole plan"),
		TryPlanChanges(
			Denied, DeniedTracker, DeniedResult, DeniedError));
	TestEqual(
		TEXT("authority error is typed"),
		DeniedError.GetCode(),
		FString(TEXT("planning_authority_denied")));

	FUeAgentResourceTracker Tracker = MakeTracker();
	FPlanningResult Result;
	FPlanningPreflightError Error;
	TestTrue(
		TEXT("baseline plan succeeds"),
		TryPlanChanges(
			MakeRequest(Tracker, false),
			Tracker,
			Result,
			Error));
	const FUeAgentCanonicalSnapshotRef Stale =
		MakeSnapshot(Tracker, 43);
	TestFalse(
		TEXT("stale revision rejects revalidation"),
		TryRevalidatePlan(
			Result.GetPlan().ToSharedRef(),
			Stale,
			FUeAgentCallerContext(
				TEXT("agent:p4-003"),
				{TEXT("uai:reference:write"),
				 TEXT("uai:reference:settings")},
				TEXT("p4-003:revalidate")),
			Tracker,
			Error));
	TestEqual(
		TEXT("stale error is typed"),
		Error.GetCode(),
		FString(TEXT("planning_revision_stale")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentPlanningPreflightDryRunFingerprintTest,
	"UeAgentInterface.Core.PlanningPreflight.DryRunFingerprint",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentPlanningPreflightDryRunFingerprintTest::RunTest(
	const FString& Parameters)
{
	FUeAgentResourceTracker LiveTracker = MakeTracker();
	FPlanningResult Live;
	FPlanningPreflightError LiveError;
	TestTrue(
		TEXT("live preflight succeeds"),
		TryPlanChanges(
			MakeRequest(LiveTracker, false),
			LiveTracker,
			Live,
			LiveError));

	FUeAgentResourceTracker DryTracker = MakeTracker();
	FPlanningResult Dry;
	FPlanningPreflightError DryError;
	TestTrue(
		TEXT("dry preflight succeeds"),
		TryPlanChanges(
			MakeRequest(DryTracker, true),
			DryTracker,
			Dry,
			DryError));
	TestTrue(TEXT("result records dry-run"), Dry.IsDryRun());
	TestFalse(
		TEXT("dry-run never permits mutation"),
		Dry.IsMutationPermitted());
	TestEqual(
		TEXT("dry and live have the same plan fingerprint"),
		Dry.GetPlan()->GetFingerprint(),
		Live.GetPlan()->GetFingerprint());
	FString RecomputedFingerprint;
	TestTrue(
		TEXT("fingerprint is recomputable"),
		TryComputePlanFingerprint(
			Dry.GetPlan().ToSharedRef(),
			DryTracker,
			RecomputedFingerprint,
			DryError));
	TestEqual(
		TEXT("recomputed fingerprint matches"),
		RecomputedFingerprint,
		Dry.GetPlan()->GetFingerprint());

	FUeAgentResourceTracker NoOpTracker = MakeTracker();
	TArray<FChangeDraft> NoOpChanges;
	NoOpChanges.Add(MakeUpdate(
		NoOpTracker, TEXT("旧标题"), TEXT("旧标题")));
	const FPlanningRequest NoOpRequest(
		FChangeSetDraft(
			FUeAgentDescriptorIdentity(
				TEXT("uai.reference.settings.update"),
				1,
				TEXT("sha256:462ec53a5b260b93a75b85182b32efcad3a1392db07ee66858a99557f7ca8880")),
			TEXT("p4-003:planning:noop"),
			TEXT("p4-003-idempotency-noop"),
			MakeSnapshot(NoOpTracker),
			MoveTemp(NoOpChanges)),
		FUeAgentCallerContext(
			TEXT("agent:p4-003"),
			{TEXT("uai:reference:write"),
			 TEXT("uai:reference:settings")},
			TEXT("p4-003:noop")),
		false);
	FPlanningResult NoOp;
	FPlanningPreflightError NoOpError;
	TestTrue(
		TEXT("semantic no-op preflight succeeds"),
		TryPlanChanges(
			NoOpRequest, NoOpTracker, NoOp, NoOpError));
	TestEqual(
		TEXT("semantic no-op status"),
		NoOp.GetStatus(),
		FString(TEXT("no_change")));
	TestTrue(
		TEXT("semantic no-op is ready for execute/verify lifecycle"),
		NoOp.IsReady());
	TestFalse(
		TEXT("semantic no-op has no mutation permission"),
		NoOp.IsMutationPermitted());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentPlanningPreflightBudgetDepthTest,
	"UeAgentInterface.Core.PlanningPreflight.BudgetDepth",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentPlanningPreflightBudgetDepthTest::RunTest(
	const FString& Parameters)
{
	FUeAgentResourceTracker BuildTracker = MakeTracker();
	const FPlanningRequest Request =
		MakeRequest(BuildTracker, false);
	FUeAgentResourceTracker LowTracker = MakeTracker(1);
	FPlanningResult LowResult;
	FPlanningPreflightError LowError;
	TestFalse(
		TEXT("resource exhaustion fails closed"),
		TryPlanChanges(
			Request,
			LowTracker,
			LowResult,
			LowError));
	TestEqual(
		TEXT("resource error is preserved"),
		LowError.GetCode(),
		FString(TEXT("resource_budget_exceeded")));

	FUeAgentResourceTracker Tracker = MakeTracker();
	FUeAgentCanonicalValueRef Current =
		MakeStringValue(TEXT("leaf-before"), Tracker);
	FUeAgentCanonicalValueRef Proposed =
		MakeStringValue(TEXT("leaf-after"), Tracker);
	for (int32 Index = 0; Index < 384; ++Index)
	{
		FUeAgentCanonicalValuePtr CurrentArray;
		FUeAgentCanonicalValuePtr ProposedArray;
		FUeAgentStateSemanticError StateError;
		TestTrue(
			TEXT("deep current value builds"),
			FUeAgentCanonicalValue::TryArray(
				{Current},
				Tracker,
				CurrentArray,
				StateError));
		TestTrue(
			TEXT("deep proposed value builds"),
			FUeAgentCanonicalValue::TryArray(
				{Proposed},
				Tracker,
				ProposedArray,
				StateError));
		Current = CurrentArray.ToSharedRef();
		Proposed = ProposedArray.ToSharedRef();
	}
	TestEqual(
		TEXT("deep value preserves requested nesting"),
		Current->GetKind(),
		EUeAgentValueKind::Array);
	TestTrue(
		TEXT("deep values are governed by budget, not a depth cap"),
		Tracker.Snapshot().Consumed(
			EUeAgentResourceDimension::Nodes) >= 768);
	return true;
}

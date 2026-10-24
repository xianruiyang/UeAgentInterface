#include "Misc/AutomationTest.h"

#include "Core/Contract/Semantic/UeAgentStateValueContextPort.h"
#include "Core/ExecutionContext/UeAgentFrozenExecutionContext.h"

namespace
{
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::Contract;
	using namespace UE::UAI::ExecutionContext;
	using namespace UE::UAI::SemanticContract;
	using namespace UE::UAI::StateSemantic;

	static FUeAgentResourceTracker MakeResources()
	{
		return FUeAgentResourceTracker();
	}

	static FUeAgentExecutionContextPolicy ContextPolicy(
		const TArray<EUeAgentStatePlane>& StatePlanes)
	{
		return FUeAgentExecutionContextPolicy(
			StatePlanes,
			{
				EUeAgentExecutionContextFacet::World,
				EUeAgentExecutionContextFacet::Selection,
			},
			EUeAgentContextResolutionPolicy::ResolveCurrentThenFreeze,
			EUeAgentTransientContextPolicy::ReadOnly);
	}

	static TArray<FUeAgentExecutionContextFact> ContextFacts()
	{
		return {
			{
				EUeAgentExecutionContextFacet::World,
				TEXT("/Game/Maps/StrictV2.StrictV2:PersistentLevel"),
			},
			{
				EUeAgentExecutionContextFacet::Selection,
				TEXT("selection-revision-17"),
			},
		};
	}

	static bool FreezeContext(
		const EUeAgentStatePlane StatePlane,
		TSharedPtr<const FUeAgentFrozenExecutionContext>& OutContext,
		FUeAgentExecutionContextError& OutError)
	{
		return FUeAgentFrozenExecutionContext::TryFreeze(
			TEXT("request-t03"),
			TEXT("GptProjectTest"),
			ContextPolicy({
				EUeAgentStatePlane::AuthoringMemory,
				EUeAgentStatePlane::Persisted,
			}),
			StatePlane,
			ContextFacts(),
			FUeAgentTemporalPoint(
				TEXT("request_freshness_clock"),
				TEXT("41")),
			OutContext,
			OutError);
	}

	static FUeAgentStateEntry MakeEntry(
		FUeAgentResourceTracker& Resources,
		const EUeAgentAuthoredPresence Presence,
		const EUeAgentOverrideState OverrideState)
	{
		FUeAgentCanonicalValuePtr Value;
		FUeAgentStateSemanticError Error;
		FUeAgentCanonicalValue::TryString(
			TEXT("DefaultLabel"), Resources, Value, Error);
		return FUeAgentStateEntry(
			FUeAgentCanonicalIdentity(
				TEXT("field.display_label"),
				TEXT("property"),
				TEXT("asset:/Game/StrictV2/DA_Context"),
				TEXT("DisplayLabel"),
				TEXT("field"),
				TEXT("generation-3")),
			FUeAgentCanonicalType(
				TEXT("string"),
				EUeAgentValueKind::String,
				false,
				TEXT("scalar")),
			Value.ToSharedRef(),
			FUeAgentDefaultValue(
				EUeAgentDefaultSourceKind::ClassDefaultObject,
				TEXT("/Script/StrictV2.ContextObject:DisplayLabel"),
				TEXT("cdo-readback-3"),
				Value.ToSharedRef()),
			FUeAgentStateDetails(
				TEXT("Display Label"),
				TEXT("Stable display label."),
				{TEXT("StrictV2")},
				true,
				TEXT("available"),
				{}),
			FUeAgentStateRevision(
				TEXT("asset:/Game/StrictV2/DA_Context"),
				TEXT("generation-3"),
				7),
			Presence,
			OverrideState);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUaiStrictV2T03FrozenRequestContextTest,
	"UAI.StrictV2.T03.STATE.VALUE.CONTEXT.FrozenRequestContext",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUaiStrictV2T03FrozenRequestContextTest::RunTest(const FString& Parameters)
{
	TArray<FUeAgentExecutionContextFact> MutableFacts = ContextFacts();
	TSharedPtr<const FUeAgentFrozenExecutionContext> Context;
	FUeAgentExecutionContextError Error;
	TestTrue(
		TEXT("a fully resolved request context freezes once"),
		FUeAgentFrozenExecutionContext::TryFreeze(
			TEXT("request-t03"),
			TEXT("GptProjectTest"),
			ContextPolicy({EUeAgentStatePlane::Persisted}),
			EUeAgentStatePlane::Persisted,
			MutableFacts,
			FUeAgentTemporalPoint(
				TEXT("request_freshness_clock"), TEXT("41")),
			Context,
			Error));
	if (!Context.IsValid())
	{
		AddError(Error.Code);
		return false;
	}

	MutableFacts[0].StableIdentity = TEXT("mutated-after-freeze");
	TestEqual(
		TEXT("state plane is explicit and frozen"),
		Context->GetStatePlane(),
		EUeAgentStatePlane::Persisted);
	TestEqual(
		TEXT("resolved world is detached from mutable input"),
		Context->FindFacet(EUeAgentExecutionContextFacet::World),
		FString(TEXT("/Game/Maps/StrictV2.StrictV2:PersistentLevel")));
	TestEqual(
		TEXT("freshness observation remains separate request metadata"),
		Context->GetFreshnessObservedAt().GetClockId(),
		FString(TEXT("request_freshness_clock")));

	TArray<FUeAgentExecutionContextFact> MissingSelection = ContextFacts();
	MissingSelection.RemoveAt(1);
	TSharedPtr<const FUeAgentFrozenExecutionContext> Rejected;
	TestFalse(
		TEXT("a required current facet cannot be guessed later by a domain"),
		FUeAgentFrozenExecutionContext::TryFreeze(
			TEXT("request-t03-missing"),
			TEXT("GptProjectTest"),
			ContextPolicy({EUeAgentStatePlane::Persisted}),
			EUeAgentStatePlane::Persisted,
			MissingSelection,
			FUeAgentTemporalPoint(
				TEXT("request_freshness_clock"), TEXT("42")),
			Rejected,
			Error));
	TestEqual(
		TEXT("missing facets have a stable failure"),
		Error.Code,
		FString(TEXT("execution_context_required_facet_missing")));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUaiStrictV2T03PresenceDefaultStatePlaneTest,
	"UAI.StrictV2.T03.STATE.VALUE.CONTEXT.PresenceDefaultStatePlane",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUaiStrictV2T03PresenceDefaultStatePlaneTest::RunTest(const FString& Parameters)
{
	FUeAgentResourceTracker Resources = MakeResources();
	const FUeAgentStateEntry Inherited = MakeEntry(
		Resources,
		EUeAgentAuthoredPresence::Absent,
		EUeAgentOverrideState::Inherited);
	const FUeAgentStateEntry ExplicitDefault = MakeEntry(
		Resources,
		EUeAgentAuthoredPresence::Present,
		EUeAgentOverrideState::Overridden);
	TestTrue(TEXT("inherited entry is valid"), Inherited.IsValid());
	TestTrue(TEXT("explicit default entry is valid"), ExplicitDefault.IsValid());

	TSharedPtr<const FUeAgentFrozenExecutionContext> AuthoringContext;
	TSharedPtr<const FUeAgentFrozenExecutionContext> PersistedContext;
	FUeAgentExecutionContextError ContextError;
	TestTrue(
		TEXT("authoring context freezes"),
		FreezeContext(
			EUeAgentStatePlane::AuthoringMemory,
			AuthoringContext,
			ContextError));
	TestTrue(
		TEXT("persisted context freezes"),
		FreezeContext(
			EUeAgentStatePlane::Persisted,
			PersistedContext,
			ContextError));
	if (!AuthoringContext.IsValid() || !PersistedContext.IsValid())
	{
		return false;
	}

	FUeAgentStateValueContextPort Port;
	FUeAgentStateValueContextError Error;
	TestTrue(
		TEXT("the fixed port accepts inherited presence without rewriting it"),
		Port.ValidateEntry(*AuthoringContext, Inherited, Error));
	TestTrue(
		TEXT("the same port accepts an explicit default override"),
		Port.ValidateEntry(*AuthoringContext, ExplicitDefault, Error));
	TestEqual(
		TEXT("unset remains absent"),
		Inherited.GetAuthoredPresence(),
		EUeAgentAuthoredPresence::Absent);
	TestEqual(
		TEXT("explicit default remains present"),
		ExplicitDefault.GetAuthoredPresence(),
		EUeAgentAuthoredPresence::Present);

	const FUeAgentStateRevision Revision(
		TEXT("asset:/Game/StrictV2/DA_Context"),
		TEXT("generation-3"),
		7);
	const FUeAgentTemporalPoint CapturedAt(
		TEXT("asset_content_clock"), TEXT("7"));
	const FUeAgentStateProvenance Provenance(
		TEXT("independent_readback"),
		TEXT("asset:/Game/StrictV2/DA_Context"),
		TEXT("independent-readback-7"));
	FUeAgentCanonicalSnapshotPtr AuthoringSnapshot;
	FUeAgentCanonicalSnapshotPtr PersistedSnapshot;
	TestTrue(
		TEXT("authoring snapshot uses the context plane"),
		Port.TryCreateSnapshot(
			*AuthoringContext,
			TEXT("snapshot-authoring"),
			Revision,
			CapturedAt,
			Provenance,
			{Inherited},
			Resources,
			AuthoringSnapshot,
			Error));
	TestTrue(
		TEXT("persisted snapshot uses the context plane"),
		Port.TryCreateSnapshot(
			*PersistedContext,
			TEXT("snapshot-persisted"),
			Revision,
			CapturedAt,
			Provenance,
			{Inherited},
			Resources,
			PersistedSnapshot,
			Error));
	if (!AuthoringSnapshot.IsValid() || !PersistedSnapshot.IsValid())
	{
		AddError(Error.Code);
		return false;
	}
	TestEqual(
		TEXT("content time is not replaced with request freshness time"),
		PersistedSnapshot->GetCapturedAt().GetClockId(),
		FString(TEXT("asset_content_clock")));

	FUeAgentSemanticComparison Comparison;
	TestTrue(
		TEXT("the fixed port performs the semantic comparison"),
		Port.TryCompareSnapshots(
			AuthoringSnapshot.ToSharedRef(),
			PersistedSnapshot.ToSharedRef(),
			Resources,
			Comparison,
			Error));
	TestFalse(
		TEXT("authoring memory cannot substitute for persistence"),
		Comparison.IsEquivalent());
	TestTrue(
		TEXT("the mismatch names the state plane"),
		Comparison.GetDifferences().ContainsByPredicate(
			[](const FUeAgentSemanticDifference& Difference)
			{
				return Difference.GetKind()
					== EUeAgentDifferenceKind::StatePlaneMismatch;
			}));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUaiStrictV2T03ExplicitTimeSpaceConversionTest,
	"UAI.StrictV2.T03.STATE.VALUE.CONTEXT.ExplicitTimeSpaceConversion",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUaiStrictV2T03ExplicitTimeSpaceConversionTest::RunTest(
	const FString& Parameters)
{
	TSharedPtr<const FUeAgentFrozenExecutionContext> Context;
	FUeAgentExecutionContextError ContextError;
	TestTrue(
		TEXT("conversion context freezes"),
		FreezeContext(EUeAgentStatePlane::Persisted, Context, ContextError));
	if (!Context.IsValid())
	{
		return false;
	}

	FUeAgentResourceTracker Resources = MakeResources();
	FUeAgentStateValueContextPort Port;
	FUeAgentStateValueContextError Error;
	const FUeAgentTemporalPoint SourceTime(
		TEXT("sequence_tick"), TEXT("10"));
	const FUeAgentTemporalConversion TimeConversion(
		TEXT("sequence-to-display-v1"),
		TEXT("sequence_tick"),
		TEXT("display_tick"),
		FUeAgentTemporalRate(TEXT("1"), TEXT("1")),
		FUeAgentTemporalRate(TEXT("1"), TEXT("1")),
		TEXT("1"),
		TEXT("1"),
		TEXT("0"),
		TEXT("1"),
		TEXT("sequencer-rate-contract-v1"));
	TOptional<FUeAgentTemporalPoint> ConvertedTime;
	TestTrue(
		TEXT("time conversion consumes an explicit conversion contract"),
		Port.TryConvertTemporalValue(
			*Context,
			SourceTime,
			TimeConversion,
			EUeAgentTemporalRoundingPolicy::Exact,
			Resources,
			ConvertedTime,
			Error));
	TestTrue(TEXT("converted time exists"), ConvertedTime.IsSet());
	if (ConvertedTime.IsSet())
	{
		TestEqual(
			TEXT("time target clock comes from the conversion"),
			ConvertedTime->GetClockId(),
			FString(TEXT("display_tick")));
	}

	const FUeAgentTemporalConversion WrongTimeConversion(
		TEXT("other-to-display-v1"),
		TEXT("other_tick"),
		TEXT("display_tick"),
		FUeAgentTemporalRate(TEXT("1"), TEXT("1")),
		FUeAgentTemporalRate(TEXT("1"), TEXT("1")),
		TEXT("1"), TEXT("1"), TEXT("0"), TEXT("1"),
		TEXT("wrong-source-contract"));
	TestFalse(
		TEXT("a mismatched time conversion is rejected instead of guessed"),
		Port.TryConvertTemporalValue(
			*Context,
			SourceTime,
			WrongTimeConversion,
			EUeAgentTemporalRoundingPolicy::Exact,
			Resources,
			ConvertedTime,
			Error));

	const FUeAgentTemporalPoint SpatialTime(
		TEXT("world_transform_revision"), TEXT("3"));
	const TArray<TArray<FString>> IdentityBasis = {
		{TEXT("1"), TEXT("0"), TEXT("0")},
		{TEXT("0"), TEXT("1"), TEXT("0")},
		{TEXT("0"), TEXT("0"), TEXT("1")},
	};
	const FUeAgentSpatialFrame WorldFrame(
		TEXT("world"),
		TEXT("ue_world_basis"),
		3,
		IdentityBasis,
		{TEXT("0"), TEXT("0"), TEXT("0")},
		TEXT("cm"),
		TEXT("ue-world-frame-v1"),
		SpatialTime);
	const FUeAgentSpatialFrame ParentFrame(
		TEXT("parent:Actor-7"),
		TEXT("ue_world_basis"),
		3,
		IdentityBasis,
		{TEXT("100"), TEXT("0"), TEXT("0")},
		TEXT("cm"),
		TEXT("actor-parent-frame-v4"),
		SpatialTime);
	const FUeAgentSpatialValue WorldPoint(
		TEXT("component:Root"),
		EUeAgentSpatialQuantityKind::Point,
		WorldFrame,
		{TEXT("110"), TEXT("20"), TEXT("30")},
		SpatialTime);
	const FUeAgentSpatialConversion SpaceConversion(
		TEXT("world-to-parent-v4"),
		TEXT("world"),
		TEXT("parent:Actor-7"),
		3,
		3,
		IdentityBasis,
		{TEXT("-100"), TEXT("0"), TEXT("0")},
		SpatialTime,
		TEXT("actor-transform-readback-v4"));
	FUeAgentSpatialValuePtr ConvertedPoint;
	TestTrue(
		TEXT("space conversion consumes an explicit frame conversion"),
		Port.TryConvertSpatialValue(
			*Context,
			WorldPoint,
			ParentFrame,
			SpaceConversion,
			Resources,
			ConvertedPoint,
			Error));
	TestTrue(TEXT("converted point exists"), ConvertedPoint.IsValid());
	if (ConvertedPoint.IsValid())
	{
		TestEqual(
			TEXT("target frame comes from the explicit contract"),
			ConvertedPoint->GetFrame().GetFrameId(),
			FString(TEXT("parent:Actor-7")));
		TestTrue(
			TEXT("conversion evidence remains attached"),
			ConvertedPoint->GetConversionEvidence().Contains(
				TEXT("world-to-parent-v4")));
	}

	return !HasAnyErrors();
}

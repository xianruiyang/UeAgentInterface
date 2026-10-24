#include "Misc/AutomationTest.h"

#include "Core/ContextResource/UeAgentContextResource.h"
#include "Core/HjsonWorkspace/UeAgentSemanticSurfaceComposition.h"
#include "Core/StateSemantic/UeAgentSemanticSurface.h"

namespace
{
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::HjsonWorkspace;
	using namespace UE::UAI::StateSemantic;

	FUeAgentResourceTracker MakeResources()
	{
		return FUeAgentResourceTracker();
	}

	FUeAgentCanonicalValueRef RequireString(
		FAutomationTestBase* Test,
		FUeAgentResourceTracker& Resources,
		const FString& Text)
	{
		FUeAgentCanonicalValuePtr Value;
		FUeAgentStateSemanticError Error;
		Test->TestTrue(
			TEXT("create canonical string"),
			FUeAgentCanonicalValue::TryString(
				Text,
				Resources,
				Value,
				Error));
		return Value.ToSharedRef();
	}

	FUeAgentCanonicalValueRef RequireTemporal(
		FAutomationTestBase* Test,
		FUeAgentResourceTracker& Resources)
	{
		FUeAgentCanonicalValuePtr Value;
		FUeAgentStateSemanticError Error;
		Test->TestTrue(
			TEXT("create canonical temporal point"),
			FUeAgentCanonicalValue::TryTemporal(
				FUeAgentTemporalPoint(
					TEXT("fixture-timeline"),
					TEXT("24000"),
					TEXT("24000"),
					TEXT("1001"),
					TEXT("evaluated")),
				Resources,
				Value,
				Error));
		return Value.ToSharedRef();
	}

	FUeAgentCanonicalValueRef RequireSpatial(
		FAutomationTestBase* Test,
		FUeAgentResourceTracker& Resources)
	{
		const FUeAgentTemporalPoint ObservedAt(
			TEXT("fixture-spatial"),
			TEXT("1"));
		const FUeAgentSpatialFrame Frame(
			TEXT("fixture-local"),
			TEXT("fixture-world"),
			3,
			{
				{TEXT("1"), TEXT("0"), TEXT("0")},
				{TEXT("0"), TEXT("1"), TEXT("0")},
				{TEXT("0"), TEXT("0"), TEXT("1")},
			},
			{TEXT("0"), TEXT("0"), TEXT("0")},
			TEXT("centimeter"),
			TEXT("fixture:t64:declared-basis"),
			ObservedAt);
		FUeAgentCanonicalValuePtr Value;
		FUeAgentStateSemanticError Error;
		Test->TestTrue(
			TEXT("create canonical spatial value"),
			FUeAgentCanonicalValue::TrySpatial(
				FUeAgentSpatialValue(
					TEXT("fixture-component"),
					EUeAgentSpatialQuantityKind::Point,
					Frame,
					{TEXT("10"), TEXT("20"), TEXT("30")},
					ObservedAt,
					{TEXT("fixture:t64:direct-observation")}),
				Resources,
				Value,
				Error));
		return Value.ToSharedRef();
	}

	FUeAgentSemanticSurfaceItem MakeItem(
		FString Identity,
		FString SemanticType,
		FString OwnerIdentity,
		FString StableKey,
		FString Slot,
		int64 Order,
		TMap<FString, FUeAgentCanonicalValueRef> Values = {})
	{
		FUeAgentSemanticSurfaceItem Item;
		Item.Identity = MoveTemp(Identity);
		Item.SemanticType = MoveTemp(SemanticType);
		Item.OwnerIdentity = MoveTemp(OwnerIdentity);
		Item.StableKey = MoveTemp(StableKey);
		Item.Slot = MoveTemp(Slot);
		Item.Order = Order;
		Item.Values = MoveTemp(Values);
		return Item;
	}

	FUeAgentSemanticSurfaceRef BuildGraph(
		FAutomationTestBase* Test,
		FUeAgentResourceTracker& Resources,
		const FString& RightLabel)
	{
		FUeAgentSemanticSurfaceBuilder Builder(
			TEXT("surface:fixture:graph"),
			EUeAgentSemanticSurfaceKind::Graph,
			EUeAgentSemanticSurfaceIntent::FullState,
			EUeAgentSemanticSurfaceCompleteness::Complete,
			TEXT("fixture:t64:graph"));
		FString Error;
		Test->TestTrue(
			TEXT("add graph left item"),
			Builder.TryAddItem(
				MakeItem(
					TEXT("node:left"),
					TEXT("fixture.graph.node"),
					TEXT("surface:fixture:graph"),
					TEXT("left"),
					TEXT("nodes"),
					0,
					{{TEXT("label"), RequireString(Test, Resources, TEXT("Left"))}}),
				Error));
		Test->TestTrue(
			TEXT("add graph right item"),
			Builder.TryAddItem(
				MakeItem(
					TEXT("node:right"),
					TEXT("fixture.graph.node"),
					TEXT("surface:fixture:graph"),
					TEXT("right"),
					TEXT("nodes"),
					1,
					{{TEXT("label"), RequireString(Test, Resources, RightLabel)}}),
				Error));
		Test->TestTrue(
			TEXT("add graph output endpoint"),
			Builder.TryAddEndpoint({
				TEXT("endpoint:left:out"),
				TEXT("node:left"),
				TEXT("output"),
				0}, Error));
		Test->TestTrue(
			TEXT("add graph input endpoint"),
			Builder.TryAddEndpoint({
				TEXT("endpoint:right:in"),
				TEXT("node:right"),
				TEXT("input"),
				0}, Error));
		Test->TestTrue(
			TEXT("add graph relation"),
			Builder.TryAddRelation({
				TEXT("relation:left:right"),
				TEXT("endpoint:left:out"),
				TEXT("endpoint:right:in"),
				TEXT("data_flow"),
				0}, Error));

		FUeAgentSemanticSurfacePtr Surface;
		Test->TestTrue(
			TEXT("build typed graph surface"),
			Builder.TryBuild(Resources, Surface, Error));
		Test->TestTrue(TEXT("graph surface returned"), Surface.IsValid());
		return Surface.ToSharedRef();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentSemanticSurfaceFamilyContractTest,
	"UeAgentInterface.Core.SemanticSurface.FamilyContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentSemanticSurfaceFamilyContractTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI::StateSemantic;

	FUeAgentResourceTracker Resources = MakeResources();
	const TArray<EUeAgentSemanticSurfaceKind> Families{
		EUeAgentSemanticSurfaceKind::PropertyCollection,
		EUeAgentSemanticSurfaceKind::Graph,
		EUeAgentSemanticSurfaceKind::Tree,
		EUeAgentSemanticSurfaceKind::Timeline,
		EUeAgentSemanticSurfaceKind::KeyedTable,
		EUeAgentSemanticSurfaceKind::Spatial,
		EUeAgentSemanticSurfaceKind::ExternalProvenance,
	};
	for (const EUeAgentSemanticSurfaceKind Family : Families)
	{
		TestFalse(
			TEXT("every public surface family has a stable non-empty id"),
			LexToString(Family).IsEmpty());
	}

	FUeAgentSemanticSurfaceBuilder InvalidGraph(
		TEXT("surface:invalid:graph"),
		EUeAgentSemanticSurfaceKind::Graph,
		EUeAgentSemanticSurfaceIntent::LocalEdit,
		EUeAgentSemanticSurfaceCompleteness::Partial,
		TEXT("fixture:t64:invalid"));
	FString Error;
	TestTrue(
		TEXT("graph relation draft can be collected before validation"),
		InvalidGraph.TryAddRelation({
			TEXT("relation:missing"),
			TEXT("endpoint:missing:from"),
			TEXT("endpoint:missing:to"),
			TEXT("data_flow"),
			0}, Error));
	FUeAgentSemanticSurfacePtr InvalidSurface;
	TestFalse(
		TEXT("graph rejects unresolved endpoint identity"),
		InvalidGraph.TryBuild(Resources, InvalidSurface, Error));
	TestEqual(
		TEXT("graph reports the exact invalid contract"),
		Error,
		FString(TEXT("semantic_surface_relation_endpoint_missing")));

	FUeAgentSemanticSurfaceBuilder InvalidTree(
		TEXT("surface:invalid:tree"),
		EUeAgentSemanticSurfaceKind::Tree,
		EUeAgentSemanticSurfaceIntent::FullState,
		EUeAgentSemanticSurfaceCompleteness::Complete,
		TEXT("fixture:t64:invalid"));
	TestTrue(
		TEXT("tree item draft can be collected before validation"),
		InvalidTree.TryAddItem(
			MakeItem(
				TEXT("tree:child"),
				TEXT("fixture.tree.item"),
				TEXT("tree:missing-parent"),
				TEXT("child"),
				TEXT("children"),
				0),
			Error));
	TestFalse(
		TEXT("tree rejects a missing parent identity"),
		InvalidTree.TryBuild(Resources, InvalidSurface, Error));
	TestEqual(
		TEXT("tree reports the exact invalid contract"),
		Error,
		FString(TEXT("semantic_surface_tree_parent_missing")));

	FUeAgentSemanticSurfaceBuilder Timeline(
		TEXT("surface:fixture:timeline"),
		EUeAgentSemanticSurfaceKind::Timeline,
		EUeAgentSemanticSurfaceIntent::FullState,
		EUeAgentSemanticSurfaceCompleteness::Complete,
		TEXT("fixture:t64:timeline"));
	TestTrue(
		TEXT("timeline accepts an explicit temporal value"),
		Timeline.TryAddItem(
			MakeItem(
				TEXT("timeline:key:one"),
				TEXT("fixture.timeline.key"),
				TEXT("surface:fixture:timeline"),
				TEXT("key-one"),
				TEXT("keys"),
				0,
				{{TEXT("time"), RequireTemporal(this, Resources)}}),
			Error));
	FUeAgentSemanticSurfacePtr TimelineSurface;
	TestTrue(
		TEXT("timeline builds from typed time"),
		Timeline.TryBuild(Resources, TimelineSurface, Error));

	FUeAgentSemanticSurfaceBuilder Spatial(
		TEXT("surface:fixture:spatial"),
		EUeAgentSemanticSurfaceKind::Spatial,
		EUeAgentSemanticSurfaceIntent::LocalEdit,
		EUeAgentSemanticSurfaceCompleteness::Partial,
		TEXT("fixture:t64:spatial"));
	TestTrue(
		TEXT("spatial surface accepts an explicit framed value"),
		Spatial.TryAddItem(
			MakeItem(
				TEXT("spatial:component:one"),
				TEXT("fixture.spatial.component"),
				TEXT("surface:fixture:spatial"),
				TEXT("component-one"),
				TEXT("components"),
				0,
				{{TEXT("transform"), RequireSpatial(this, Resources)}}),
			Error));
	FUeAgentSemanticSurfacePtr SpatialSurface;
	TestTrue(
		TEXT("spatial surface builds from typed frame data"),
		Spatial.TryBuild(Resources, SpatialSurface, Error));
	if (SpatialSurface.IsValid())
	{
		TestTrue(
			TEXT("local edit intent remains explicit"),
			SpatialSurface->GetIntent()
				== EUeAgentSemanticSurfaceIntent::LocalEdit);
		TestTrue(
			TEXT("partial completeness remains explicit"),
			SpatialSurface->GetCompleteness()
				== EUeAgentSemanticSurfaceCompleteness::Partial);
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentSemanticSurfaceRoundTripTest,
	"UeAgentInterface.Core.SemanticSurface.RoundTripSnapshotAndDiff",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentSemanticSurfaceRoundTripTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI::Contract;
	using namespace UE::UAI::StateSemantic;

	FUeAgentResourceTracker Resources = MakeResources();
	const FUeAgentSemanticSurfaceRef Original =
		BuildGraph(this, Resources, TEXT("Right"));
	TArray<uint8> Bytes;
	FString Error;
	TestTrue(
		TEXT("serialize graph surface without a domain payload"),
		TrySerializeSemanticSurface(Original, Bytes, Error));
	FUeAgentSemanticSurfacePtr Parsed;
	TestTrue(
		TEXT("parse graph surface into the same typed contract"),
		TryParseSemanticSurface(Bytes, Resources, Parsed, Error));
	TestTrue(TEXT("parsed graph surface returned"), Parsed.IsValid());
	if (!Parsed.IsValid())
	{
		AddError(TEXT("semantic surface parse did not return a surface: ") + Error);
		return false;
	}

	FUeAgentSemanticComparison RoundTripComparison;
	FUeAgentStateSemanticError SemanticError;
	TestTrue(
		TEXT("compare original and parsed surface"),
		TryCompareSemanticSurfaces(
			Original,
			Parsed.ToSharedRef(),
			Resources,
			RoundTripComparison,
			SemanticError));
	TestTrue(
		TEXT("surface serialization is semantically lossless"),
		RoundTripComparison.IsEquivalent());

	FUeAgentCanonicalSnapshotPtr Snapshot;
	TestTrue(
		TEXT("one capture builder produces the canonical readback snapshot"),
		TryBuildSemanticSurfaceSnapshot(
			Original,
			TEXT("snapshot:t64:graph"),
			EUeAgentStatePlane::Persisted,
			FUeAgentStateRevision(TEXT("fixture"), TEXT("generation-1"), 1),
			FUeAgentTemporalPoint(TEXT("fixture-clock"), TEXT("1")),
			FUeAgentStateProvenance(
				TEXT("independent_readback"),
				TEXT("fixture:t64"),
				TEXT("evidence:t64:graph")),
			Resources,
			Snapshot,
			SemanticError));
	TestTrue(TEXT("canonical snapshot returned"), Snapshot.IsValid());
	if (!Snapshot.IsValid())
	{
		AddError(
			TEXT("semantic surface snapshot was not created: ")
			+ SemanticError.Code
			+ TEXT(" at ")
			+ SemanticError.Path);
		return false;
	}
	TestEqual(
		TEXT("snapshot covers surface, items, endpoints, and relation"),
		Snapshot->GetEntries().Num(),
		6);

	FAuthoringModelPtr Model;
	FWorkspaceError WorkspaceError;
	TestTrue(
		TEXT("the same surface composes the common HJSON authoring model"),
		TryBuildSemanticSurfaceAuthoringModel(
			Original,
			TEXT("fixture-t64-r1"),
			Resources,
			Model,
			WorkspaceError));
	TestTrue(TEXT("authoring model returned"), Model.IsValid());
	if (!Model.IsValid())
	{
		AddError(
			TEXT("semantic surface authoring model was not created: ")
			+ WorkspaceError.GetCode());
		return false;
	}

	const FUeAgentSemanticSurfaceRef Edited =
		BuildGraph(this, Resources, TEXT("Changed"));
	FUeAgentSemanticComparison EditComparison;
	TestTrue(
		TEXT("compare a local semantic edit"),
		TryCompareSemanticSurfaces(
			Original,
			Edited,
			Resources,
			EditComparison,
			SemanticError));
	TestFalse(TEXT("changed graph is not equivalent"), EditComparison.IsEquivalent());
	TestTrue(
		TEXT("local edit produces a bounded semantic difference"),
		EditComparison.GetDifferences().Num() > 0
			&& EditComparison.GetDifferences().Num() < 4);

	FUeAgentSemanticSurfaceBuilder UnrelatedBuilder(
		TEXT("surface:fixture:unrelated-properties"),
		EUeAgentSemanticSurfaceKind::PropertyCollection,
		EUeAgentSemanticSurfaceIntent::FullState,
		EUeAgentSemanticSurfaceCompleteness::Complete,
		TEXT("fixture:t64:unrelated"));
	TestTrue(
		TEXT("add an unrelated property surface"),
		UnrelatedBuilder.TryAddItem(
			MakeItem(
				TEXT("property:unrelated"),
				TEXT("fixture.property.string"),
				TEXT("surface:fixture:unrelated-properties"),
				TEXT("Unrelated"),
				TEXT("properties"),
				0,
				{{TEXT("value"), RequireString(this, Resources, TEXT("kept"))}}),
			Error));
	FUeAgentSemanticSurfacePtr Unrelated;
	TestTrue(
		TEXT("build the unrelated surface"),
		UnrelatedBuilder.TryBuild(Resources, Unrelated, Error));
	if (Unrelated.IsValid())
	{
		TArray<uint8> BeforeLocalEdit;
		TArray<uint8> AfterLocalEdit;
		TestTrue(
			TEXT("serialize unrelated surface before local graph edit"),
			TrySerializeSemanticSurface(
				Unrelated.ToSharedRef(),
				BeforeLocalEdit,
				Error));
		TestTrue(
			TEXT("serialize unrelated surface after local graph edit"),
			TrySerializeSemanticSurface(
				Unrelated.ToSharedRef(),
				AfterLocalEdit,
				Error));
		TestTrue(
			TEXT("a local graph edit leaves an unrelated surface unchanged"),
			AfterLocalEdit == BeforeLocalEdit);
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentSemanticSurfaceCalibrationTest,
	"UeAgentInterface.Core.SemanticSurface.CalibratedFamilies",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentSemanticSurfaceCalibrationTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI::StateSemantic;

	FUeAgentResourceTracker Resources = MakeResources();
	struct FCalibration
	{
		EUeAgentSemanticSurfaceKind Kind;
		const TCHAR* SurfaceId;
		const TCHAR* ItemId;
		const TCHAR* StableKey;
		const TCHAR* Provenance;
	};
	const TArray<FCalibration> Calibrations{
		{EUeAgentSemanticSurfaceKind::PropertyCollection,
			TEXT("surface:t13:uobject"), TEXT("property:health"),
			TEXT("Health"), TEXT("fixture:t13:canonical-snapshot")},
		{EUeAgentSemanticSurfaceKind::KeyedTable,
			TEXT("surface:t27:table"), TEXT("row:warrior"),
			TEXT("Warrior"), TEXT("fixture:t27:stable-row-key")},
		{EUeAgentSemanticSurfaceKind::ExternalProvenance,
			TEXT("surface:t30:external"), TEXT("source:texture"),
			TEXT("texture-source"), TEXT("fixture:t30:external-provenance")},
	};
	for (const FCalibration& Calibration : Calibrations)
	{
		FUeAgentSemanticSurfaceBuilder Builder(
			Calibration.SurfaceId,
			Calibration.Kind,
			EUeAgentSemanticSurfaceIntent::FullState,
			EUeAgentSemanticSurfaceCompleteness::Complete,
			Calibration.Provenance);
		FString Error;
		TestTrue(
			TEXT("add calibrated family item"),
			Builder.TryAddItem(
				MakeItem(
					Calibration.ItemId,
					TEXT("fixture.semantic.item"),
					Calibration.SurfaceId,
					Calibration.StableKey,
					TEXT("items"),
					0),
				Error));
		FUeAgentSemanticSurfacePtr Surface;
		TestTrue(
			TEXT("build calibrated family"),
			Builder.TryBuild(Resources, Surface, Error));
		TestTrue(TEXT("calibrated family surface returned"), Surface.IsValid());
		if (Surface.IsValid())
		{
			TestEqual(
				TEXT("stable key is retained without domain inference"),
				Surface->GetItems()[0].StableKey,
				FString(Calibration.StableKey));
			TestEqual(
				TEXT("source provenance is retained"),
				Surface->GetSourceProvenance(),
				FString(Calibration.Provenance));
		}
	}
	return !HasAnyErrors();
}

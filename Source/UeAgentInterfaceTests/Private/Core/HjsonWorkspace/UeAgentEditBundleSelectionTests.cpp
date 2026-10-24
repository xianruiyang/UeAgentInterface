#include "Misc/AutomationTest.h"

#include "Core/HjsonWorkspace/UeAgentEditBundleSelection.h"

#include "Core/Projection/UeAgentEditBundleProjection.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentEditBundleSelectionModuleContractTest,
	"UeAgentInterface.Core.HjsonWorkspace.EditBundleSelection.ModuleContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentEditBundleSelectionModuleContractTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI::HjsonWorkspace;
	using namespace UE::UAI::Projection;

	TestEqual(
		TEXT("edit bundle uses the versioned common workspace schema"),
		GetEditBundleWorkspaceSchemaId(),
		FString(TEXT("uai.authoring.edit_bundle/1")));
	TestEqual(
		TEXT("edit bundle uses one LLM-oriented profile"),
		GetEditBundleProfileId(),
		FString(TEXT("llm_edit_bundle")));

	const TArray<TPair<EWorkspaceSelectionKind, FString>> Kinds{
		{EWorkspaceSelectionKind::Property, TEXT("property")},
		{EWorkspaceSelectionKind::GraphItem, TEXT("graph_item")},
		{EWorkspaceSelectionKind::Connection, TEXT("connection")},
		{EWorkspaceSelectionKind::StructuralAdd, TEXT("structural_add")},
		{EWorkspaceSelectionKind::TreeItem, TEXT("tree_item")},
		{EWorkspaceSelectionKind::TimelineItem, TEXT("timeline_item")},
		{EWorkspaceSelectionKind::SpatialLayout, TEXT("spatial_layout")},
	};
	for (const TPair<EWorkspaceSelectionKind, FString>& Kind : Kinds)
	{
		TestEqual(
			TEXT("selection kind has one stable transport spelling"),
			LexToString(Kind.Key),
			Kind.Value);
	}

	const FWorkspaceSelection PropertySelection(
		TEXT("property-large"),
		EWorkspaceSelectionKind::Property,
		TArray<FString>{},
		TArray<FString>{TEXT("field.node.large_text")},
		TArray<FString>{},
		FString());
	TestTrue(
		TEXT("field selection is a valid common selection"),
		PropertySelection.IsValid());
	TestEqual(
		TEXT("field selection keeps the exact field identity"),
		PropertySelection.GetSelectedFieldIds(),
		TArray<FString>{TEXT("field.node.large_text")});

	const FWorkspaceSelection ItemSelection(
		TEXT("graph-item"),
		EWorkspaceSelectionKind::GraphItem,
		TArray<FString>{TEXT("item:graph:EventGraph:NodeA")},
		TArray<FString>{},
		TArray<FString>{},
		FString());
	TestTrue(
		TEXT("graph item selection is a valid common selection"),
		ItemSelection.IsValid());
	TestEqual(
		TEXT("item selection keeps the exact semantic unit identity"),
		ItemSelection.GetSelectedUnitIds(),
		TArray<FString>{TEXT("item:graph:EventGraph:NodeA")});

	const FWorkspaceSelection StructureSelection(
		TEXT("structural-add"),
		EWorkspaceSelectionKind::StructuralAdd,
		TArray<FString>{TEXT("structure:graph:EventGraph")},
		TArray<FString>{},
		TArray<FString>{},
		TEXT("binding:ue.blueprint.call.PrintString"));
	TestTrue(
		TEXT("structural selection is valid only with an exact candidate binding"),
		StructureSelection.IsValid());
	TestEqual(
		TEXT("structural selection keeps the exact candidate binding"),
		StructureSelection.GetCandidateBindingId(),
		FString(TEXT("binding:ue.blueprint.call.PrintString")));

	TestTrue(
		TEXT("selection closure is a native first-class contract"),
		TIsClass<FSelectionClosure>::Value);
	TestTrue(
		TEXT("scoped edit bundle is a native first-class contract"),
		TIsClass<FScopedEditBundle>::Value);
	TestTrue(
		TEXT("response projection is separate from the edit bundle"),
		TIsClass<FEditBundleProjection>::Value);

	const FString SelectionFingerprint =
		TEXT("sha256:1111111111111111111111111111111111111111111111111111111111111111");
	const FString ClosureFingerprint =
		TEXT("sha256:2222222222222222222222222222222222222222222222222222222222222222");
	const FString BundleFingerprint =
		TEXT("sha256:3333333333333333333333333333333333333333333333333333333333333333");
	const FSelectionClosure Closure(
		PropertySelection,
		TArray<FString>{
			TEXT("target:/Game/Fixture/BP_Selection"),
			TEXT("structure:graph:EventGraph"),
			TEXT("item:graph:EventGraph:NodeA"),
			TEXT("property:item:NodeA:node.advanced")},
		TArray<FString>{TEXT("field.node.large_text")},
		TArray<FString>{},
		SelectionFingerprint,
		ClosureFingerprint);
	TestTrue(
		TEXT("selection closure is valid and content addressed"),
		Closure.IsValid());
	TestEqual(
		TEXT("closure keeps the selected field"),
		Closure.GetFieldIds(),
		TArray<FString>{TEXT("field.node.large_text")});

	const FScopedEditBundle Bundle(
		GetEditBundleWorkspaceSchemaId(),
		GetEditBundleProfileId(),
		TEXT("uai.fixture.multi_surface.authoring"),
		2,
		TEXT("fixture-hx006-r1"),
		TEXT("sha256:4444444444444444444444444444444444444444444444444444444444444444"),
		SelectionFingerprint,
		ClosureFingerprint,
		TEXT("fixture:selection:snapshot"),
		TMap<FString, FSemanticUnit>{},
		TArray<FUeAgentStateEntry>{},
		TMap<FString, FExportFieldDescription>{},
		TMap<FString, FString>{},
		TOptional<FUeAgentCandidateBinding>(),
		BundleFingerprint);
	TestTrue(
		TEXT("edit bundle binds its base and closure without a second patch schema"),
		Bundle.IsValid());
	TestEqual(
		TEXT("bundle keeps the closure fingerprint"),
		Bundle.GetClosureFingerprint(),
		Closure.GetClosureFingerprint());

	const FEditBundleProjection Projection(
		BundleFingerprint,
		ClosureFingerprint,
		TArray<FString>{},
		TArray<FString>{},
		TOptional<FUeAgentArtifactDescriptor>(),
		FUeAgentPageInfo(0, 0));
	TestTrue(
		TEXT("inline response projection is valid without rewriting the bundle"),
		Projection.IsValid());
	TestFalse(
		TEXT("inline projection has no artifact"),
		Projection.GetArtifact().IsSet());
	TestEqual(
		TEXT("projection keeps the original bundle fingerprint"),
		Projection.GetBundleFingerprint(),
		Bundle.GetBundleFingerprint());
	return !HasAnyErrors();
}

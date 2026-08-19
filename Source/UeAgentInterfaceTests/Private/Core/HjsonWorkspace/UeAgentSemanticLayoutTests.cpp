#include "Misc/AutomationTest.h"

#include "Core/HjsonWorkspace/UeAgentSemanticLayout.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentSemanticLayoutModuleContractTest,
	"UeAgentInterface.Core.HjsonWorkspace.SemanticLayout.ModuleContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentSemanticLayoutModuleContractTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI::HjsonWorkspace;
	using namespace UE::UAI::ContextResource;

	const FString TargetHandle = StableLocalHandle(
		TEXT("target:/Game/Fixture/BP_HeavyNode"),
		ESemanticUnitKind::TargetState);
	const FString SameTargetHandle = StableLocalHandle(
		TEXT("target:/Game/Fixture/BP_HeavyNode"),
		ESemanticUnitKind::TargetState);
	const FString NodeHandle = StableLocalHandle(
		TEXT("item:blueprint:EventGraph:HeavyCall"),
		ESemanticUnitKind::ItemState);
	const FString PeerNodeHandle = StableLocalHandle(
		TEXT("item:blueprint:EventGraph:PeerCall"),
		ESemanticUnitKind::ItemState);

	TestEqual(
		TEXT("stable identity produces the same local handle"),
		TargetHandle,
		SameTargetHandle);
	TestNotEqual(
		TEXT("different semantic units do not share a local handle"),
		TargetHandle,
		NodeHandle);
	TestNotEqual(
		TEXT("different units of the same kind do not share a handle"),
		NodeHandle,
		PeerNodeHandle);
	TestFalse(
		TEXT("local handle is not a source path"),
		TargetHandle.Contains(TEXT("/"))
			|| TargetHandle.Contains(TEXT("\\"))
			|| TargetHandle.Contains(TEXT("..")));
	TestTrue(
		TEXT("workspace schema is the versioned common schema"),
		GetSemanticWorkspaceSchemaId()
			== TEXT("uai.authoring.workspace/2"));

	const FString TargetId =
		TEXT("target:/Game/Fixture/BP_HeavyNode");
	const FString StructureId =
		TEXT("structure:blueprint:EventGraph");
	const FString NodeId =
		TEXT("item:blueprint:EventGraph:HeavyCall");
	const FString PropertyId =
		TEXT("property:item:HeavyCall:node.advanced");
	const TMap<FString,
		UE::UAI::StateSemantic::FUeAgentCanonicalValueRef>
		EmptyValues;
	TArray<FSemanticUnit> Units;
	Units.Emplace(
		PropertyId,
		ESemanticUnitKind::PropertySet,
		NodeId,
		TEXT("uai.authoring.property_set/1"),
		TEXT("editable_properties"),
		TEXT("node.advanced"),
		EmptyValues,
		TArray<FString>{},
		TArray<FString>{},
		FString(),
		FString(),
		TArray<FString>{},
		TArray<FString>{TEXT("state"), TEXT("edit_bundle")},
		TEXT("fixture:hx004:native"));
	Units.Emplace(
		NodeId,
		ESemanticUnitKind::ItemState,
		StructureId,
		TEXT("ue.blueprint.node.call_function"),
		TEXT("call_function"),
		FString(),
		EmptyValues,
		TArray<FString>{},
		TArray<FString>{},
		FString(),
		FString(),
		TArray<FString>{},
		TArray<FString>{TEXT("state"), TEXT("edit_bundle")},
		TEXT("fixture:hx004:native"));
	Units.Emplace(
		StructureId,
		ESemanticUnitKind::StructureState,
		TargetId,
		TEXT("ue.structure.graph"),
		TEXT("event_graph"),
		FString(),
		EmptyValues,
		TArray<FString>{NodeId},
		TArray<FString>{},
		FString(),
		FString(),
		TArray<FString>{},
		TArray<FString>{TEXT("state"), TEXT("edit_bundle")},
		TEXT("fixture:hx004:native"));
	Units.Emplace(
		TargetId,
		ESemanticUnitKind::TargetState,
		FString(),
		TEXT("ue.blueprint.asset"),
		TEXT("authoring_target"),
		FString(),
		EmptyValues,
		TArray<FString>{StructureId},
		TArray<FString>{},
		FString(),
		FString(),
		TArray<FString>{},
		TArray<FString>{TEXT("state"), TEXT("edit_bundle")},
		TEXT("fixture:hx004:native"));

	const TMap<EUeAgentResourceDimension, int64> Limits{
		{EUeAgentResourceDimension::InputBytes, 4 * 1024 * 1024},
		{EUeAgentResourceDimension::OutputBytes, 8 * 1024 * 1024},
		{EUeAgentResourceDimension::StringBytes, 4 * 1024 * 1024},
		{EUeAgentResourceDimension::Nodes, 32768},
		{EUeAgentResourceDimension::Items, 32768},
		{EUeAgentResourceDimension::Files, 4096},
		{EUeAgentResourceDimension::ReferenceEdges, 32768},
		{EUeAgentResourceDimension::WorkUnits, 262144},
		{EUeAgentResourceDimension::EstimatedMemoryBytes, 32 * 1024 * 1024},
	};
	FUeAgentResourceTracker CompileResources{
		FUeAgentResourceBudget(Limits)};
	FAuthoringModelPtr Model;
	FWorkspaceError Error;
	TestTrue(
		TEXT("unordered semantic units compile into one common model"),
		TryCompileAuthoringModel(
			TEXT("uai.fixture.blueprint.authoring"),
			2,
			TEXT("state"),
			TEXT("fixture-hx004-r1"),
			MoveTemp(Units),
			TArray<FString>{TargetId},
			CompileResources,
			Model,
			Error));
	TestTrue(TEXT("compiled model is available"), Model.IsValid());
	if (!Model.IsValid())
	{
		return false;
	}
	TestEqual(
		TEXT("model keeps all semantic units"),
		Model->GetUnits().Num(),
		4);
	TestTrue(
		TEXT("semantic fingerprint is content addressed"),
		Model->GetSemanticFingerprint().StartsWith(TEXT("sha256:")));

	FUeAgentResourceTracker LowResources{
		FUeAgentResourceBudget(Limits)};
	FWorkspaceLayoutPlanPtr LowPlan;
	TestTrue(
		TEXT("directory layout plans without a token gate"),
		TryPlanWorkspaceLayout(
			Model.ToSharedRef(),
			1,
			EWorkspaceSourceMode::Directory,
			TMap<FString, int64>{
				{TEXT(" estimated_tokens "), 64},
				{TEXT("estimated_bytes"), 1024}},
			LowResources,
			LowPlan,
			Error));
	FUeAgentResourceTracker HighResources{
		FUeAgentResourceBudget(Limits)};
	FWorkspaceLayoutPlanPtr HighPlan;
	TestTrue(
		TEXT("large token telemetry remains non-blocking"),
		TryPlanWorkspaceLayout(
			Model.ToSharedRef(),
			1,
			EWorkspaceSourceMode::Directory,
			TMap<FString, int64>{
				{TEXT("estimated_tokens"), 1000000},
				{TEXT("estimated_bytes"), 8388608}},
			HighResources,
			HighPlan,
			Error));
	TestTrue(
		TEXT("both layout plans are available"),
		LowPlan.IsValid() && HighPlan.IsValid());
	if (!LowPlan.IsValid() || !HighPlan.IsValid())
	{
		return false;
	}
	TestTrue(
		TEXT("telemetry does not alter logical placements"),
		LowPlan->GetLogicalPlacements().OrderIndependentCompareEqual(
			HighPlan->GetLogicalPlacements()));
	TestTrue(
		TEXT("telemetry does not alter physical files"),
		LowPlan->GetPhysicalFiles().OrderIndependentCompareEqual(
			HighPlan->GetPhysicalFiles()));
	TestEqual(
		TEXT("manifest uses the same semantic fingerprint"),
		LowPlan->GetMachineManifest().SemanticFingerprint,
		Model->GetSemanticFingerprint());
	TestEqual(
		TEXT("manifest inventory covers every unit"),
		LowPlan->GetMachineManifest().UnitInventory.Num(),
		Model->GetUnits().Num());
	TestEqual(
		TEXT("LLM index is navigation-only"),
		LowPlan->GetLlmIndex().SurfaceUnitIds.Num(),
		3);
	TestTrue(
		TEXT("telemetry keys are normalized without corrupting lookup"),
		LowPlan->GetTelemetry().Contains(TEXT("estimated_tokens")));

	int32 NodeFileCount = 0;
	for (const TPair<FString, TArray<FString>>& File
		: LowPlan->GetPhysicalFiles())
	{
		NodeFileCount += File.Value.Contains(NodeId) ? 1 : 0;
	}
	TestEqual(
		TEXT("one semantic unit is not token-split across files"),
		NodeFileCount,
		1);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentSemanticLayoutCrossLanguageIdentityTest,
	"UeAgentInterface.Core.HjsonWorkspace.SemanticLayout.CrossLanguageIdentity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentSemanticLayoutCrossLanguageIdentityTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::HjsonWorkspace;

	const FString UnitId = TEXT("target:cross-language");
	const FSemanticUnit Unit(
		UnitId,
		ESemanticUnitKind::TargetState,
		FString(),
		TEXT("ue.test.asset"),
		TEXT("authoring_target"),
		FString(),
		{},
		{},
		{},
		FString(),
		FString(),
		{},
		{TEXT("state"), TEXT("edit_bundle")},
		TEXT("fixture:hx013:python-cpp"),
		TEXT(
			"test_asset|ue.test.asset|asset_path|"
			"asset_path=%2FGame%2FHX013"));
	TestTrue(TEXT("cross-language unit is valid"), Unit.IsValid());
	TestEqual(
		TEXT("Python and C++ use the same local handle vector"),
		StableLocalHandle(UnitId, ESemanticUnitKind::TargetState),
		FString(TEXT("target-cross-language-ecdf035ad4bce826")));
	TestEqual(
		TEXT("Python and C++ use the same physical unit content hash"),
		SemanticUnitContentHash(Unit),
		FString(
			TEXT("sha256:")
			TEXT("067138abf81e059cae87037304f5ae28")
			TEXT("37c5c49761dc17b2d492177dd7238187")));

	const TMap<EUeAgentResourceDimension, int64> Limits{
		{EUeAgentResourceDimension::InputBytes, 1024 * 1024},
		{EUeAgentResourceDimension::OutputBytes, 1024 * 1024},
		{EUeAgentResourceDimension::StringBytes, 1024 * 1024},
		{EUeAgentResourceDimension::Nodes, 1024},
		{EUeAgentResourceDimension::Items, 1024},
		{EUeAgentResourceDimension::Files, 1024},
		{EUeAgentResourceDimension::ReferenceEdges, 1024},
		{EUeAgentResourceDimension::WorkUnits, 8192},
		{EUeAgentResourceDimension::EstimatedMemoryBytes, 8 * 1024 * 1024},
	};
	FUeAgentResourceTracker Resources{FUeAgentResourceBudget(Limits)};
	FAuthoringModelPtr Model;
	FWorkspaceError Error;
	TestTrue(
		TEXT("cross-language identity vector compiles"),
		TryCompileAuthoringModel(
			TEXT("uai.authoring.composition/2"),
			2,
			TEXT("llm_authoring"),
			TEXT("hx013:parity:r1"),
			{Unit},
			{UnitId},
			Resources,
			Model,
			Error));
	TestTrue(TEXT("cross-language model is available"), Model.IsValid());
	if (Model.IsValid())
	{
		TestEqual(
			TEXT("Python and C++ use the same semantic fingerprint"),
			Model->GetSemanticFingerprint(),
			FString(
				TEXT("sha256:")
				TEXT("0b3b1d4a8034e008794c8de97bbcd38f")
				TEXT("e72a3e9f9a782a7df75e23d76774dd3f")));
	}
	return !HasAnyErrors();
}

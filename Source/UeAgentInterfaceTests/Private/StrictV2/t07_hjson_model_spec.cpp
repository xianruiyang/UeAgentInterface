#include "Misc/AutomationTest.h"

#include "Core/ContextResource/UeAgentContextResource.h"
#include "Core/HjsonWorkspace/UeAgentEditBundleSelection.h"
#include "Core/HjsonWorkspace/UeAgentExportFieldContract.h"
#include "Core/HjsonWorkspace/UeAgentHjsonWorkspace.h"
#include "Core/HjsonWorkspace/UeAgentSemanticLayout.h"

#if __has_include("Core/HjsonWorkspace/FieldContract/UeAgentUnifiedHjsonFieldContract.h") \
	&& __has_include("Core/HjsonWorkspace/Selection/UeAgentUnifiedHjsonSelection.h") \
	&& __has_include("Core/HjsonWorkspace/SemanticModel/UeAgentUnifiedHjsonSemanticModel.h") \
	&& __has_include("Core/HjsonWorkspace/Workspace/UeAgentUnifiedHjsonWorkspace.h")
#define UAI_T07_UNIFIED_HJSON_MODEL_AVAILABLE 1
#include "Core/HjsonWorkspace/FieldContract/UeAgentUnifiedHjsonFieldContract.h"
#include "Core/HjsonWorkspace/Selection/UeAgentUnifiedHjsonSelection.h"
#include "Core/HjsonWorkspace/SemanticModel/UeAgentUnifiedHjsonSemanticModel.h"
#include "Core/HjsonWorkspace/Workspace/UeAgentUnifiedHjsonWorkspace.h"
#else
#define UAI_T07_UNIFIED_HJSON_MODEL_AVAILABLE 0
#endif

#if UAI_T07_UNIFIED_HJSON_MODEL_AVAILABLE
namespace
{
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::HjsonWorkspace;
	using namespace UE::UAI::StateSemantic;

	static FUeAgentResourceTracker MakeResources(
		int64 Nodes = 32768,
		int64 WorkUnits = 262144)
	{
		return FUeAgentResourceTracker(FUeAgentResourceBudget({
			{EUeAgentResourceDimension::InputBytes, 8 * 1024 * 1024},
			{EUeAgentResourceDimension::OutputBytes, 8 * 1024 * 1024},
			{EUeAgentResourceDimension::StringBytes, 8 * 1024 * 1024},
			{EUeAgentResourceDimension::Nodes, Nodes},
			{EUeAgentResourceDimension::Items, 32768},
			{EUeAgentResourceDimension::Files, 4096},
			{EUeAgentResourceDimension::ReferenceEdges, 32768},
			{EUeAgentResourceDimension::WorkUnits, WorkUnits},
			{EUeAgentResourceDimension::EstimatedMemoryBytes,
				64 * 1024 * 1024},
		}));
	}

	static FSemanticUnit MakeUnit(
		FString UnitId,
		ESemanticUnitKind Kind,
		FString Owner,
		FString Role,
		FString FieldSet = FString(),
		TArray<FString> ItemRefs = {},
		TArray<FString> RelationRefs = {},
		FString OrderingRef = FString(),
		FString LayoutRef = FString())
	{
		return FSemanticUnit(
			MoveTemp(UnitId),
			Kind,
			MoveTemp(Owner),
			TEXT("uai.strict_v2.t07.semantic/1"),
			MoveTemp(Role),
			MoveTemp(FieldSet),
			{},
			MoveTemp(ItemRefs),
			MoveTemp(RelationRefs),
			MoveTemp(OrderingRef),
			MoveTemp(LayoutRef),
			{},
			{TEXT("state"), TEXT("minimal_create"), TEXT("local_edit")},
			TEXT("strict_v2:t07:current-design"));
	}

	struct FUnifiedFixture
	{
		FAuthoringModelPtr CoreModel;
		TMap<EUnifiedHjsonCompositionKind, TArray<FString>> Compositions;
	};

	static FUnifiedFixture MakeUnifiedFixture(
		FAutomationTestBase* Test)
	{
		const FString Target = TEXT("target");
		const FString Member = TEXT("member");
		const FString Structure = TEXT("structure");
		const FString Item = TEXT("item");
		const FString Endpoint = TEXT("endpoint");
		const FString Relation = TEXT("relation");
		const FString Order = TEXT("order");
		const FString Layout = TEXT("layout");
		const FString ValueProperty = TEXT("property.value");
		const FString ModeProperty = TEXT("property.mode");
		const FString Time = TEXT("temporal");
		const FString Space = TEXT("spatial");
		const FString External = TEXT("external");

		TArray<FSemanticUnit> Units;
		Units.Add(MakeUnit(
			Target,
			ESemanticUnitKind::TargetState,
			FString(),
			TEXT("target"),
			FString(),
			{Structure, Member}));
		Units.Add(MakeUnit(
			Member,
			ESemanticUnitKind::EntityState,
			Target,
			TEXT("member")));
		Units.Add(MakeUnit(
			Structure,
			ESemanticUnitKind::StructureState,
			Target,
			TEXT("structure"),
			FString(),
			{Item, Relation, Order, Layout}));
		Units.Add(MakeUnit(
			Item,
			ESemanticUnitKind::ItemState,
			Structure,
			TEXT("item"),
			FString(),
			{Endpoint},
			{Relation},
			Order,
			Layout));
		Units.Add(MakeUnit(
			Endpoint,
			ESemanticUnitKind::EntityState,
			Item,
			TEXT("endpoint")));
		Units.Add(MakeUnit(
			Relation,
			ESemanticUnitKind::RelationSet,
			Structure,
			TEXT("relation"),
			FString(),
			{Item, Endpoint}));
		Units.Add(MakeUnit(
			Order,
			ESemanticUnitKind::OrderingState,
			Structure,
			TEXT("order"),
			FString(),
			{Item, Endpoint}));
		Units.Add(MakeUnit(
			Layout,
			ESemanticUnitKind::LayoutState,
			Structure,
			TEXT("layout"),
			FString(),
			{Item, Endpoint}));
		Units.Add(MakeUnit(
			ValueProperty,
			ESemanticUnitKind::PropertySet,
			Item,
			TEXT("property_set"),
			TEXT("field.value")));
		Units.Add(MakeUnit(
			ModeProperty,
			ESemanticUnitKind::PropertySet,
			Item,
			TEXT("property_set"),
			TEXT("field.mode")));
		Units.Add(MakeUnit(
			Time,
			ESemanticUnitKind::PropertySet,
			Target,
			TEXT("temporal_contract"),
			TEXT("temporal")));
		Units.Add(MakeUnit(
			Space,
			ESemanticUnitKind::PropertySet,
			Target,
			TEXT("spatial_contract"),
			TEXT("spatial")));
		Units.Add(MakeUnit(
			External,
			ESemanticUnitKind::ExternalPayloadState,
			Target,
			TEXT("external_input")));

		FUeAgentResourceTracker Resources = MakeResources();
		FWorkspaceError Error;
		FAuthoringModelPtr CoreModel;
		Test->TestTrue(
			TEXT("canonical snapshot projection compiles one core semantic model"),
			TryCompileAuthoringModel(
				TEXT("uai.strict_v2.t07.hjson"),
				1,
				TEXT("unified"),
				TEXT("fixture-r1"),
				MoveTemp(Units),
				{Target},
				Resources,
				CoreModel,
				Error));

		TMap<EUnifiedHjsonCompositionKind, TArray<FString>> Compositions{
			{EUnifiedHjsonCompositionKind::Target, {Target}},
			{EUnifiedHjsonCompositionKind::PropertySet,
				{ValueProperty, ModeProperty}},
			{EUnifiedHjsonCompositionKind::MemberSet, {Member}},
			{EUnifiedHjsonCompositionKind::Structure, {Structure}},
			{EUnifiedHjsonCompositionKind::Item, {Item}},
			{EUnifiedHjsonCompositionKind::Endpoint, {Endpoint}},
			{EUnifiedHjsonCompositionKind::Relation, {Relation}},
			{EUnifiedHjsonCompositionKind::Order, {Order}},
			{EUnifiedHjsonCompositionKind::Layout, {Layout}},
			{EUnifiedHjsonCompositionKind::TemporalContract, {Time}},
			{EUnifiedHjsonCompositionKind::SpatialContract, {Space}},
			{EUnifiedHjsonCompositionKind::ExternalInput, {External}},
		};
		return {MoveTemp(CoreModel), MoveTemp(Compositions)};
	}

	static FExportFieldContract MakeExportField(
		FString FieldId,
		FString ExternalName,
		bool bDefaultReproducible,
		bool bPresenceSensitive)
	{
		return FExportFieldContract(
			TEXT("uai.strict_v2.t07.fields"),
			1,
			MoveTemp(FieldId),
			MoveTemp(ExternalName),
			TEXT("item"),
			TEXT("ue.int32.v1"),
			TEXT("editable"),
			TEXT("round_trip"),
			{TEXT("state"), TEXT("minimal_create"), TEXT("local_edit")},
			bPresenceSensitive,
			EExportFieldEmissionPolicy::OnSelection,
			EUeAgentDefaultSourceKind::ClassDefaultObject,
			bDefaultReproducible,
			TEXT("properties"),
			TEXT("strict_v2 current design"),
			TEXT("Value"),
			TEXT("Value"),
			TEXT("integer"));
	}
}
#endif

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T07CommonCompositionTest,
	"UAI.StrictV2.T07.HJSON.MODEL.CommonCompositionAndIntent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T07CommonCompositionTest::RunTest(
	const FString& Parameters)
{
#if !UAI_T07_UNIFIED_HJSON_MODEL_AVAILABLE
	TestTrue(TEXT("T07 unified HJSON model is implemented"), false);
	return false;
#else
	using namespace UE::UAI::HjsonWorkspace;
	using namespace UE::UAI::ContextResource;

	FUnifiedFixture Fixture = MakeUnifiedFixture(this);
	if (!Fixture.CoreModel.IsValid())
	{
		return false;
	}
	const TArray<EUnifiedHjsonCompositionKind> Kinds =
		GetAllUnifiedHjsonCompositionKinds();
	TestEqual(TEXT("the common model has exactly twelve compositions"), Kinds.Num(), 12);
	TSet<FString> Names;
	for (const EUnifiedHjsonCompositionKind Kind : Kinds)
	{
		Names.Add(LexToString(Kind));
	}
	TestEqual(TEXT("all composition names are unique"), Names.Num(), 12);

	TArray<FUnifiedHjsonSemanticModelPtr> Models;
	for (const EUnifiedHjsonAuthoringIntent Intent : {
		EUnifiedHjsonAuthoringIntent::FullState,
		EUnifiedHjsonAuthoringIntent::MinimalCreate,
		EUnifiedHjsonAuthoringIntent::LocalEdit})
	{
		FUeAgentResourceTracker Resources = MakeResources();
		FWorkspaceError Error;
		FUnifiedHjsonSemanticModelPtr Model;
		TestTrue(
			TEXT("each public authoring intent binds the same twelve-part model"),
			TryCreateUnifiedHjsonSemanticModel(
				Fixture.CoreModel.ToSharedRef(),
				Intent,
				Fixture.Compositions,
				Resources,
				Model,
				Error));
		TestTrue(TEXT("unified semantic model is available"), Model.IsValid());
		if (Model.IsValid())
		{
			Models.Add(MoveTemp(Model));
		}
	}
	TestEqual(TEXT("all three intents compile"), Models.Num(), 3);
	if (Models.Num() == 3)
	{
		TestEqual(
			TEXT("intent does not fork the semantic model identity"),
			Models[0]->GetModelFingerprint(),
			Models[1]->GetModelFingerprint());
		TestEqual(
			TEXT("local edit uses the same semantic model identity"),
			Models[0]->GetModelFingerprint(),
			Models[2]->GetModelFingerprint());
		TestEqual(
			TEXT("all source units have one composition owner"),
			Models[0]->GetBoundUnitCount(),
			Fixture.CoreModel->GetUnits().Num());
	}
	return true;
#endif
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T07FieldIntentTest,
	"UAI.StrictV2.T07.HJSON.MODEL.FieldOwnerAndExplicitIntent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T07FieldIntentTest::RunTest(
	const FString& Parameters)
{
#if !UAI_T07_UNIFIED_HJSON_MODEL_AVAILABLE
	TestTrue(TEXT("T07 unified field contract is implemented"), false);
	return false;
#else
	using namespace UE::UAI::HjsonWorkspace;

	const FExportFieldContract Value = MakeExportField(
		TEXT("field.value"), TEXT("Value"), true, false);
	FExportFieldRegistryPtr BaseRegistry;
	FWorkspaceError Error;
	TestTrue(
		TEXT("one underlying field owner compiles"),
		TryCompileExportFieldRegistry(
			{FExportFieldRegistration(
				Value,
				EExportFieldOwnerSource::Reflection,
				TEXT("reflection:fixture:Value"))},
			TEXT("uai.strict_v2.t07.fields"),
			1,
			BaseRegistry,
			Error));
	if (!BaseRegistry.IsValid())
	{
		return false;
	}

	FUnifiedHjsonFieldRegistryPtr UnifiedRegistry;
	TestTrue(
		TEXT("the unified contract declares every permitted edit intent"),
		TryCompileUnifiedHjsonFieldRegistry(
			BaseRegistry.ToSharedRef(),
			{FUnifiedHjsonFieldContract(
				TEXT("field.value"),
				false,
				{EUnifiedHjsonFieldIntent::Preserve,
					EUnifiedHjsonFieldIntent::SetValue,
					EUnifiedHjsonFieldIntent::RestoreDefault})},
			UnifiedRegistry,
			Error));
	TestTrue(TEXT("unified field registry is available"), UnifiedRegistry.IsValid());
	if (!UnifiedRegistry.IsValid())
	{
		return false;
	}

	EUnifiedHjsonFieldIntent Resolved = EUnifiedHjsonFieldIntent::Remove;
	TestTrue(
		TEXT("omission resolves to preserve and never to delete or clear"),
		TryResolveUnifiedHjsonFieldIntent(
			UnifiedRegistry.ToSharedRef(),
			TEXT("field.value"),
			TOptional<EUnifiedHjsonFieldIntent>(),
			Resolved,
			Error));
	TestEqual(
		TEXT("omission is a non-destructive explicit semantic"),
		Resolved,
		EUnifiedHjsonFieldIntent::Preserve);

	TestFalse(
		TEXT("null cannot be inferred for a non-nullable field"),
		TryResolveUnifiedHjsonFieldIntent(
			UnifiedRegistry.ToSharedRef(),
			TEXT("field.value"),
			EUnifiedHjsonFieldIntent::SetNull,
			Resolved,
			Error));
	TestEqual(
		TEXT("unsupported explicit intent has a typed failure"),
		Error.GetCode(),
		FString(TEXT("hjson_field_intent_not_allowed")));

	FExportFieldRegistryPtr ConflictRegistry;
	TestFalse(
		TEXT("two providers cannot own one field"),
		TryCompileExportFieldRegistry(
			{
				FExportFieldRegistration(
					Value,
					EExportFieldOwnerSource::Reflection,
					TEXT("reflection:fixture:Value")),
				FExportFieldRegistration(
					Value,
					EExportFieldOwnerSource::Handwritten,
					TEXT("handwritten:fixture:Value")),
			},
			TEXT("uai.strict_v2.t07.fields"),
			1,
			ConflictRegistry,
			Error));
	TestEqual(
		TEXT("owner conflict remains typed at the shared field authority"),
		Error.GetCode(),
		FString(TEXT("field_owner_conflict")));
	return true;
#endif
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T07CarrierSelectionTest,
	"UAI.StrictV2.T07.HJSON.MODEL.CarrierAndSelectionClosure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T07CarrierSelectionTest::RunTest(
	const FString& Parameters)
{
#if !UAI_T07_UNIFIED_HJSON_MODEL_AVAILABLE
	TestTrue(TEXT("T07 unified workspace and selection are implemented"), false);
	return false;
#else
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::HjsonWorkspace;

	FUnifiedFixture Fixture = MakeUnifiedFixture(this);
	if (!Fixture.CoreModel.IsValid())
	{
		return false;
	}
	FUeAgentResourceTracker ModelResources = MakeResources();
	FWorkspaceError Error;
	FUnifiedHjsonSemanticModelPtr Model;
	TestTrue(
		TEXT("local edit model compiles"),
		TryCreateUnifiedHjsonSemanticModel(
			Fixture.CoreModel.ToSharedRef(),
			EUnifiedHjsonAuthoringIntent::LocalEdit,
			Fixture.Compositions,
			ModelResources,
			Model,
			Error));
	if (!Model.IsValid())
	{
		return false;
	}

	TArray<FUnifiedHjsonWorkspacePtr> Workspaces;
	for (const EWorkspaceSourceMode SourceMode : {
		EWorkspaceSourceMode::Directory,
		EWorkspaceSourceMode::SingleFile,
		EWorkspaceSourceMode::Inline})
	{
		FUeAgentResourceTracker LayoutResources = MakeResources();
		FWorkspaceLayoutPlanPtr Layout;
		TestTrue(
			TEXT("each carrier receives a physical projection"),
			TryPlanWorkspaceLayout(
				Fixture.CoreModel.ToSharedRef(),
				1,
				SourceMode,
				{{TEXT("estimated_tokens"), 1000000}},
				LayoutResources,
				Layout,
				Error));
		FUeAgentResourceTracker WorkspaceResources = MakeResources();
		FUnifiedHjsonWorkspacePtr Workspace;
		if (Layout.IsValid())
		{
			TestTrue(
				TEXT("physical carrier preserves the unified semantic model"),
				TryCreateUnifiedHjsonWorkspace(
					Model.ToSharedRef(),
					Layout.ToSharedRef(),
					WorkspaceResources,
					Workspace,
					Error));
		}
		if (Workspace.IsValid())
		{
			Workspaces.Add(MoveTemp(Workspace));
		}
	}
	TestEqual(TEXT("directory, file and inline carriers compile"), Workspaces.Num(), 3);
	if (Workspaces.Num() == 3)
	{
		TestEqual(
			TEXT("single-file carrier keeps the same semantic fingerprint"),
			Workspaces[0]->GetSemanticFingerprint(),
			Workspaces[1]->GetSemanticFingerprint());
		TestEqual(
			TEXT("inline carrier keeps the same semantic fingerprint"),
			Workspaces[0]->GetSemanticFingerprint(),
			Workspaces[2]->GetSemanticFingerprint());
		TestEqual(
			TEXT("index covers every semantic unit"),
			Workspaces[0]->GetIndex().UnitIds.Num(),
			Fixture.CoreModel->GetUnits().Num());
	}

	const FWorkspaceSelection Selection(
		TEXT("selection:item:value"),
		EWorkspaceSelectionKind::Property,
		{TEXT("item")},
		{TEXT("field.value")},
		{},
		FString());
	const FSelectionPolicy Policy(
		TEXT("uai.strict_v2.t07.hjson"),
		1,
		TEXT("strict_v2.t07.selection"),
		{{TEXT("field.value"), {TEXT("field.mode")}}},
		{},
		{TEXT("endpoint")},
		{ESemanticUnitKind::LayoutState,
			ESemanticUnitKind::OrderingState});
	FUeAgentResourceTracker SelectionResources = MakeResources();
	FUnifiedHjsonSelectionPackagePtr Package;
	TestTrue(
		TEXT("selection delegates to the shared closure owner"),
		TryResolveUnifiedHjsonSelection(
			Model.ToSharedRef(),
			Selection,
			Policy,
			SelectionResources,
			Package,
			Error));
	TestTrue(TEXT("selection package is available"), Package.IsValid());
	if (Package.IsValid())
	{
		const FSelectionClosure& Closure = Package->GetClosure();
		TestTrue(TEXT("closure includes the selected item"), Closure.GetUnitIds().Contains(TEXT("item")));
		TestTrue(TEXT("closure includes the owner chain"), Closure.GetUnitIds().Contains(TEXT("structure")) && Closure.GetUnitIds().Contains(TEXT("target")));
		TestTrue(TEXT("closure includes the controlling field"), Closure.GetFieldIds().Contains(TEXT("field.mode")));
		TestTrue(TEXT("closure includes the direct relation"), Closure.GetRelationIds().Contains(TEXT("relation")));
		TestEqual(TEXT("selection package is tied to the same model"), Package->GetModelFingerprint(), Model->GetModelFingerprint());
	}
	return true;
#endif
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T07DepthResourceTest,
	"UAI.StrictV2.T07.HJSON.MODEL.DeepParsingAndResourceFailure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T07DepthResourceTest::RunTest(
	const FString& Parameters)
{
#if !UAI_T07_UNIFIED_HJSON_MODEL_AVAILABLE
	TestTrue(TEXT("T07 depth and resource semantics are implemented"), false);
	return false;
#else
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::HjsonWorkspace;

	const FString DeepDocument =
		FString::ChrN(512, TEXT('['))
		+ TEXT("0")
		+ FString::ChrN(512, TEXT(']'));
	FUeAgentResourceTracker FullResources = MakeResources(4096, 262144);
	FHjsonDocumentPtr Document;
	FWorkspaceError Error;
	TestTrue(
		TEXT("valid HJSON is not rejected by an artificial nesting limit"),
		TryParseHjsonDocument(
			DeepDocument,
			TEXT("strict_v2:t07:deep"),
			FullResources,
			Document,
			Error));
	TestTrue(TEXT("deep document is available"), Document.IsValid());

	FUeAgentResourceTracker BoundedResources = MakeResources(16, 64);
	FHjsonDocumentPtr Rejected;
	TestFalse(
		TEXT("the same document stops at an explicit request budget"),
		TryParseHjsonDocument(
			DeepDocument,
			TEXT("strict_v2:t07:bounded"),
			BoundedResources,
			Rejected,
			Error));
	TestTrue(TEXT("resource failure is typed"), !Error.GetCode().IsEmpty());
	TestTrue(TEXT("resource failure records early stop"), Error.IsStoppedEarly());
	TestTrue(TEXT("resource failure reports observed usage"), Error.GetObserved() > 0);
	return true;
#endif
}

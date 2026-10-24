#include "Misc/AutomationTest.h"

#include "Containers/StringConv.h"

#include "Core/HjsonWorkspace/UeAgentAuthoringComposition.h"
#include "Domains/Material/Authoring/UeAgentMaterialAuthoringDescriptors.h"
#include "Domains/Material/Authoring/UeAgentMaterialAuthoringState.h"
#include "Materials/MaterialInstanceConstant.h"

namespace
{
	using namespace UE::UAI::HjsonWorkspace;
	using namespace UE::UAI::ContextResource;

	FUeAgentResourceTracker MakeCompositionTracker()
	{
		using UE::UAI::ContextResource::EUeAgentResourceDimension;
		const TMap<EUeAgentResourceDimension, int64> Limits{
			{EUeAgentResourceDimension::InputBytes, 16 * 1024 * 1024},
			{EUeAgentResourceDimension::OutputBytes, 32 * 1024 * 1024},
			{EUeAgentResourceDimension::StringBytes, 16 * 1024 * 1024},
			{EUeAgentResourceDimension::Nodes, 131072},
			{EUeAgentResourceDimension::Items, 131072},
			{EUeAgentResourceDimension::Files, 16384},
			{EUeAgentResourceDimension::ReferenceEdges, 131072},
			{EUeAgentResourceDimension::WorkUnits, 1048576},
			{EUeAgentResourceDimension::EstimatedMemoryBytes, 128 * 1024 * 1024},
		};
		return FUeAgentResourceTracker();
	}

	UeAgentAuthoringState::FStateEntry MakeEntry(
		const FString& SemanticId,
		const FString& TargetKind,
		const FString& CanonicalKey,
		TMap<FString, FString> Selectors,
		const TSharedPtr<FJsonValue>& Value)
	{
		UeAgentAuthoringState::FStateEntry Entry;
		Entry.SemanticId = SemanticId;
		Entry.TargetIdentity.TargetKind = TargetKind;
		Entry.TargetIdentity.CanonicalKey = CanonicalKey;
		Entry.TargetIdentity.Selectors = MoveTemp(Selectors);
		Entry.Value = Value;
		Entry.bAvailable = true;
		Entry.bEditable = true;
		Entry.Provenance = TEXT("fixture:hx011:native");
		return Entry;
	}

	struct FExpectedComposition
	{
		const TCHAR* TargetKind;
		const TCHAR* const* SurfaceIds;
		int32 SurfaceCount;
	};

	bool VerifyComposition(
		FAutomationTestBase& Test,
		const FExpectedComposition& Expected)
	{
		FTargetCompositionContract Contract;
		if (!Test.TestTrue(
			FString::Printf(
				TEXT("%s has one registered composition contract"),
				Expected.TargetKind),
			FTargetCompositionRegistry::Get().FindContract(
				Expected.TargetKind,
				Contract)))
		{
			return false;
		}

		FString Error;
		Test.TestTrue(
			FString::Printf(
				TEXT("%s composition contract is valid"),
				Expected.TargetKind),
			ValidateTargetCompositionContract(Contract, Error));
		Test.TestEqual(
			FString::Printf(
				TEXT("%s uses the common composition schema"),
				Expected.TargetKind),
			Contract.SchemaId,
			FString(TEXT("uai.authoring.target_composition/1")));
		Test.TestEqual(
			FString::Printf(
				TEXT("%s binds the expected target kind"),
				Expected.TargetKind),
			Contract.TargetKind,
			FString(Expected.TargetKind));
		Test.TestTrue(
			FString::Printf(
				TEXT("%s has target properties"),
				Expected.TargetKind),
			Contract.Surfaces.ContainsByPredicate(
				[](const FCompositionSurfaceContract& Surface)
				{
					return Surface.SurfaceKind
						== EAuthoringSurfaceKind::Properties;
				}));
		Test.TestTrue(
			FString::Printf(
				TEXT("%s is not a top-level-property-only contract"),
				Expected.TargetKind),
			Contract.Surfaces.ContainsByPredicate(
				[](const FCompositionSurfaceContract& Surface)
				{
					return Surface.SurfaceKind
						!= EAuthoringSurfaceKind::Properties;
				}));
		Test.TestFalse(
			FString::Printf(
				TEXT("%s does not use a whole provider extension"),
				Expected.TargetKind),
			Contract.Surfaces.ContainsByPredicate(
				[](const FCompositionSurfaceContract& Surface)
				{
					return Surface.UnitKind
						== ESemanticUnitKind::OpaqueExtension;
				}));

		for (int32 Index = 0; Index < Expected.SurfaceCount; ++Index)
		{
			const FString SurfaceId(Expected.SurfaceIds[Index]);
			Test.TestTrue(
				FString::Printf(
					TEXT("%s exposes %s"),
					Expected.TargetKind,
					*SurfaceId),
				Contract.Surfaces.ContainsByPredicate(
					[&SurfaceId](
						const FCompositionSurfaceContract& Surface)
					{
						return Surface.SurfaceId == SurfaceId;
					}));
		}
		return !Test.HasAnyErrors();
	}

	FAuthoringModelPtr ComposeFixture(
		FAutomationTestBase& Test,
		const FString& TargetKind,
		TArray<UeAgentAuthoringState::FStateEntry> Entries)
	{
		FCompositionRequest Request;
		Request.TargetKind = TargetKind;
		Request.ProfileId = TEXT("llm_authoring");
		Request.BaseRevision = TEXT("fixture-hx012-r1");
		Request.SourceProvenance = TEXT("fixture:hx012:native");
		Request.Entries = MoveTemp(Entries);
		FUeAgentResourceTracker Resources = MakeCompositionTracker();
		FAuthoringModelPtr Model;
		FWorkspaceError Error;
		const bool bComposed = TryComposeAuthoringModel(
			Request,
			Resources,
			Model,
			Error);
		Test.TestTrue(
			FString::Printf(TEXT("%s fixture composes"), *TargetKind),
			bComposed);
		if (!bComposed)
		{
			Test.AddError(FString::Printf(
				TEXT("composition_error:%s:%s:%s"),
				*Error.GetCode(),
				*Error.GetSourceUnit(),
				*Error.GetJsonPointer()));
		}
		Test.TestTrue(
			FString::Printf(TEXT("%s fixture model is available"), *TargetKind),
			Model.IsValid());
		return Model;
	}

	bool HasPopulatedRole(
		const FAuthoringModelPtr& Model,
		const FString& Role,
		const ESemanticUnitKind UnitKind)
	{
		return Model.IsValid()
			&& Model->GetUnits().ContainsByPredicate(
				[&Role, UnitKind](const FSemanticUnit& Unit)
				{
					return Unit.GetRole() == Role
						&& Unit.GetUnitKind() == UnitKind
						&& !Unit.GetSemanticTypeRef().StartsWith(
							TEXT("uai.authoring.surface."));
				});
	}

	TSharedRef<FJsonObject> MakeTimelineDocument()
	{
		TSharedRef<FJsonObject> Key = MakeShared<FJsonObject>();
		Key->SetNumberField(TEXT("frame"), 24);
		Key->SetNumberField(TEXT("seconds"), 1.0);
		Key->SetNumberField(TEXT("value"), 0.75);
		TSharedRef<FJsonObject> Section = MakeShared<FJsonObject>();
		Section->SetStringField(TEXT("id"), TEXT("section-1"));
		Section->SetArrayField(
			TEXT("keys"),
			{MakeShared<FJsonValueObject>(Key)});
		TSharedRef<FJsonObject> Track = MakeShared<FJsonObject>();
		Track->SetStringField(TEXT("id"), TEXT("track-1"));
		Track->SetArrayField(
			TEXT("sections"),
			{MakeShared<FJsonValueObject>(Section)});
		TSharedRef<FJsonObject> MovieScene = MakeShared<FJsonObject>();
		MovieScene->SetNumberField(TEXT("display_rate"), 24);
		MovieScene->SetArrayField(
			TEXT("tracks"),
			{MakeShared<FJsonValueObject>(Track)});
		TSharedRef<FJsonObject> Document = MakeShared<FJsonObject>();
		Document->SetObjectField(TEXT("movie_scene"), MovieScene);
		return Document;
	}

	TSharedRef<FJsonObject> MakeRigDocument()
	{
		TSharedRef<FJsonObject> Bone = MakeShared<FJsonObject>();
		Bone->SetStringField(TEXT("kind"), TEXT("bone"));
		Bone->SetStringField(TEXT("name"), TEXT("root"));
		TSharedRef<FJsonObject> Elements = MakeShared<FJsonObject>();
		Elements->SetObjectField(TEXT("bone:root"), Bone);
		TSharedRef<FJsonObject> Hierarchy = MakeShared<FJsonObject>();
		Hierarchy->SetStringField(TEXT("transform_space"), TEXT("local"));
		Hierarchy->SetObjectField(TEXT("elements"), Elements);
		TSharedRef<FJsonObject> Position = MakeShared<FJsonObject>();
		Position->SetNumberField(TEXT("x"), 100.0);
		Position->SetNumberField(TEXT("y"), 50.0);
		TSharedRef<FJsonObject> Node = MakeShared<FJsonObject>();
		Node->SetStringField(TEXT("node_type"), TEXT("unit"));
		Node->SetObjectField(TEXT("position"), Position);
		TSharedRef<FJsonObject> Nodes = MakeShared<FJsonObject>();
		Nodes->SetObjectField(TEXT("BeginExecution"), Node);
		TSharedRef<FJsonObject> Graph = MakeShared<FJsonObject>();
		Graph->SetObjectField(TEXT("nodes"), Nodes);
		TSharedRef<FJsonObject> GraphItems = MakeShared<FJsonObject>();
		GraphItems->SetObjectField(TEXT("RigVMModel"), Graph);
		TSharedRef<FJsonObject> Graphs = MakeShared<FJsonObject>();
		Graphs->SetObjectField(TEXT("graphs"), GraphItems);
		TSharedRef<FJsonObject> Document = MakeShared<FJsonObject>();
		Document->SetObjectField(TEXT("hierarchy"), Hierarchy);
		Document->SetObjectField(TEXT("graphs"), Graphs);
		return Document;
	}

	TSharedRef<FJsonObject> MakeAudioDocument()
	{
		TSharedRef<FJsonObject> Position = MakeShared<FJsonObject>();
		Position->SetNumberField(TEXT("x"), 20.0);
		Position->SetNumberField(TEXT("y"), 40.0);
		TSharedRef<FJsonObject> Node = MakeShared<FJsonObject>();
		Node->SetStringField(TEXT("id"), TEXT("oscillator"));
		Node->SetStringField(TEXT("class_name"), TEXT("Sine"));
		Node->SetObjectField(TEXT("position"), Position);
		TSharedRef<FJsonObject> Edge = MakeShared<FJsonObject>();
		Edge->SetStringField(TEXT("from_node"), TEXT("oscillator"));
		Edge->SetStringField(TEXT("to_node"), TEXT("output"));
		TSharedRef<FJsonObject> Graph = MakeShared<FJsonObject>();
		Graph->SetArrayField(
			TEXT("nodes"),
			{MakeShared<FJsonValueObject>(Node)});
		Graph->SetArrayField(
			TEXT("edges"),
			{MakeShared<FJsonValueObject>(Edge)});
		TSharedRef<FJsonObject> Document = MakeShared<FJsonObject>();
		Document->SetObjectField(TEXT("graph"), Graph);
		return Document;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentGraphTreeAuthoringCompositionContractTest,
	"UeAgentInterface.Core.HjsonWorkspace.AuthoringComposition.GraphTreeContracts",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentGraphTreeAuthoringCompositionContractTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	FString ContractError;
	TestTrue(
		TEXT("graph/tree composition contracts register"),
		RegisterGraphTreeTargetCompositionContracts(ContractError));
	if (!ContractError.IsEmpty())
	{
		AddError(ContractError);
	}
	static const TCHAR* BlueprintSurfaces[] = {
		TEXT("target_settings"),
		TEXT("class_defaults"),
		TEXT("member_collection"),
		TEXT("component_tree"),
		TEXT("graph_collection"),
		TEXT("timeline_collection"),
		TEXT("node_properties"),
	};
	static const TCHAR* WidgetSurfaces[] = {
		TEXT("target_settings"),
		TEXT("member_collection"),
		TEXT("widget_tree"),
		TEXT("animation_collection"),
		TEXT("graph_collection"),
		TEXT("widget_properties"),
	};
	static const TCHAR* AnimBlueprintSurfaces[] = {
		TEXT("target_settings"),
		TEXT("member_collection"),
		TEXT("graph_collection"),
		TEXT("node_properties"),
	};
	static const TCHAR* MaterialSurfaces[] = {
		TEXT("target_settings"),
		TEXT("parameter_collection"),
		TEXT("graph_collection"),
		TEXT("expression_properties"),
	};
	static const TCHAR* PcgSurfaces[] = {
		TEXT("target_settings"),
		TEXT("graph_collection"),
		TEXT("node_properties"),
	};
	static const TCHAR* TypedGraphSurfaces[] = {
		TEXT("target_settings"),
		TEXT("parameter_collection"),
		TEXT("graph_collection"),
		TEXT("node_properties"),
	};
	const FExpectedComposition Expected[] = {
		{TEXT("ue.blueprint.asset"), BlueprintSurfaces, UE_ARRAY_COUNT(BlueprintSurfaces)},
		{TEXT("ue.widget_blueprint.asset"), WidgetSurfaces, UE_ARRAY_COUNT(WidgetSurfaces)},
		{TEXT("ue.anim_blueprint.asset"), AnimBlueprintSurfaces, UE_ARRAY_COUNT(AnimBlueprintSurfaces)},
		{TEXT("ue.material.asset"), MaterialSurfaces, UE_ARRAY_COUNT(MaterialSurfaces)},
		{TEXT("ue.material_function.asset"), MaterialSurfaces, UE_ARRAY_COUNT(MaterialSurfaces)},
		{TEXT("ue.material_instance.asset"), MaterialSurfaces, UE_ARRAY_COUNT(MaterialSurfaces)},
		{TEXT("ue.pcg_graph.asset"), PcgSurfaces, UE_ARRAY_COUNT(PcgSurfaces)},
		{TEXT("ue.texture_graph.asset"), TypedGraphSurfaces, UE_ARRAY_COUNT(TypedGraphSurfaces)},
		{TEXT("ue.deformer_graph.asset"), TypedGraphSurfaces, UE_ARRAY_COUNT(TypedGraphSurfaces)},
	};
	TestEqual(
		TEXT("composition schema is stable"),
		GetTargetCompositionContractSchemaId(),
		FString(TEXT("uai.authoring.target_composition/1")));
	for (const FExpectedComposition& Row : Expected)
	{
		VerifyComposition(*this, Row);
	}
	FCompositionSemanticRouteContract MaterialGraphRoute;
	TestTrue(
		TEXT("Material export graph resolves through the declared common Graph route"),
		TryResolveTargetCompositionSemanticRoute(
			TEXT("ue.material.asset"),
			UeAgentMaterialAuthoring::ExportGraphSemanticId,
			MaterialGraphRoute));
	TestEqual(
		TEXT("Material export graph uses the shared graph surface"),
		MaterialGraphRoute.SurfaceId,
		FString(TEXT("graph_collection")));
	TestEqual(
		TEXT("Material export graph is a structure unit"),
		MaterialGraphRoute.UnitKind,
		ESemanticUnitKind::StructureState);
	TestEqual(
		TEXT("Material export graph uses the common Graph role"),
		MaterialGraphRoute.Role,
		ECompositionSemanticRouteRole::Graph);
	TSharedRef<FJsonObject> Target = MakeShared<FJsonObject>();
	Target->SetStringField(TEXT("asset_path"), TEXT("/Game/Fixture/BP_Composition"));
	Target->SetStringField(TEXT("asset_kind"), TEXT("blueprint"));
	TSharedRef<FJsonObject> Graph = MakeShared<FJsonObject>();
	Graph->SetStringField(TEXT("graph_name"), TEXT("EventGraph"));
	Graph->SetStringField(TEXT("graph_kind"), TEXT("event_graph"));
	TSharedRef<FJsonObject> Node = MakeShared<FJsonObject>();
	Node->SetStringField(TEXT("node_class"), TEXT("/Script/BlueprintGraph.K2Node_CallFunction"));
	TSharedRef<FJsonObject> Position = MakeShared<FJsonObject>();
	Position->SetNumberField(TEXT("x"), 320.0);
	Position->SetNumberField(TEXT("y"), -96.0);
	Node->SetObjectField(TEXT("pos"), Position);
	TSharedRef<FJsonObject> Property = MakeShared<FJsonObject>();
	Property->SetStringField(TEXT("value"), TEXT("FixtureValue"));
	TSharedRef<FJsonObject> Edge = MakeShared<FJsonObject>();
	Edge->SetStringField(TEXT("from_node"), TEXT("builtin:event_begin_play"));
	Edge->SetStringField(TEXT("from_pin"), TEXT("then"));
	Edge->SetStringField(TEXT("to_node"), TEXT("node-guid-1"));
	Edge->SetStringField(TEXT("to_pin"), TEXT("execute"));
	TSharedRef<FJsonObject> SecondEdge = MakeShared<FJsonObject>();
	SecondEdge->SetStringField(TEXT("from_node"), TEXT("node-guid-1"));
	SecondEdge->SetStringField(TEXT("from_pin"), TEXT("then"));
	SecondEdge->SetStringField(TEXT("to_node"), TEXT("builtin:self"));
	SecondEdge->SetStringField(TEXT("to_pin"), TEXT("execute"));
	FCompositionRequest Request;
	Request.TargetKind = TEXT("ue.blueprint.asset");
	Request.ProfileId = TEXT("llm_authoring");
	Request.BaseRevision = TEXT("fixture-hx011-r1");
	Request.SourceProvenance = TEXT("fixture:hx011:native");
	Request.Entries = {
		MakeEntry(
			TEXT("uai.semantic.blueprint.export.target"),
			TEXT("blueprint_asset"),
			TEXT("blueprint:/Game/Fixture/BP_Composition"),
			{{TEXT("asset_path"), TEXT("/Game/Fixture/BP_Composition")}},
			MakeShared<FJsonValueObject>(Target)),
		MakeEntry(
			TEXT("uai.semantic.blueprint.graph"),
			TEXT("blueprint_graph"),
			TEXT("graph:event"),
			{
				{TEXT("asset_path"), TEXT("/Game/Fixture/BP_Composition")},
				{TEXT("graph_guid"), TEXT("graph-guid-1")},
				{TEXT("graph_name"), TEXT("EventGraph")},
			},
			MakeShared<FJsonValueObject>(Graph)),
		MakeEntry(
			TEXT("uai.semantic.blueprint.node"),
			TEXT("blueprint_node"),
			TEXT("node:node-guid-1"),
			{
				{TEXT("asset_path"), TEXT("/Game/Fixture/BP_Composition")},
				{TEXT("graph_guid"), TEXT("graph-guid-1")},
				{TEXT("node_guid"), TEXT("node-guid-1")},
			},
			MakeShared<FJsonValueObject>(Node)),
		MakeEntry(
			TEXT("uai.semantic.blueprint.node.property"),
			TEXT("blueprint_node_property"),
			TEXT("node-property:node-guid-1:fixture"),
			{
				{TEXT("asset_path"), TEXT("/Game/Fixture/BP_Composition")},
				{TEXT("graph_guid"), TEXT("graph-guid-1")},
				{TEXT("node_guid"), TEXT("node-guid-1")},
				{TEXT("property_path"), TEXT("FixtureProperty")},
			},
			MakeShared<FJsonValueObject>(Property)),
		MakeEntry(
			TEXT("uai.semantic.blueprint.edge"),
			TEXT("blueprint_edge"),
			TEXT("edge:fixture"),
			{
				{TEXT("asset_path"), TEXT("/Game/Fixture/BP_Composition")},
				{TEXT("graph_guid"), TEXT("graph-guid-1")},
				{TEXT("edge_key"), TEXT("fixture")},
			},
			MakeShared<FJsonValueObject>(Edge)),
		MakeEntry(
			TEXT("uai.semantic.blueprint.edge"),
			TEXT("blueprint_edge"),
			TEXT("edge:fixture:second"),
			{
				{TEXT("asset_path"), TEXT("/Game/Fixture/BP_Composition")},
				{TEXT("graph_guid"), TEXT("graph-guid-1")},
				{TEXT("edge_key"), TEXT("fixture:second")},
			},
			MakeShared<FJsonValueObject>(SecondEdge)),
	};

	FUeAgentResourceTracker Resources = MakeCompositionTracker();
	FAuthoringModelPtr Model;
	FWorkspaceError Error;
	TestTrue(
		TEXT("granular Blueprint state composes into the common authoring model"),
		TryComposeAuthoringModel(
			Request,
			Resources,
			Model,
			Error));
	TestTrue(TEXT("composed Blueprint model is available"), Model.IsValid());
	if (Model.IsValid())
	{
		const FSemanticUnit* NodeUnit = nullptr;
		bool bHasNodeProperty = false;
		bool bHasNodeLayout = false;
		bool bHasRelation = false;
		bool bHasStableBuiltin = false;
		int32 RelationSetCount = 0;
		int32 RelationRecordCount = 0;
		for (const FSemanticUnit& Unit : Model->GetUnits())
		{
			TestTrue(
				TEXT("composition never emits an opaque provider unit"),
				Unit.GetUnitKind() != ESemanticUnitKind::OpaqueExtension);
			if (Unit.GetUnitKind() == ESemanticUnitKind::ItemState
				&& Unit.GetValues().Contains(TEXT("node_class")))
			{
				NodeUnit = &Unit;
			}
			if (Unit.GetUnitKind() == ESemanticUnitKind::PropertySet
				&& Unit.GetRole() == TEXT("node_properties"))
			{
				bHasNodeProperty = true;
			}
			if (Unit.GetUnitKind() == ESemanticUnitKind::LayoutState)
			{
				bHasNodeLayout = true;
			}
			if (Unit.GetUnitKind() == ESemanticUnitKind::RelationSet)
			{
				bHasRelation = true;
				++RelationSetCount;
				if (const FUeAgentCanonicalValueRef* Relations =
					Unit.GetValues().Find(TEXT("relations")))
				{
					if ((*Relations)->GetKind()
						== UE::UAI::StateSemantic::EUeAgentValueKind::Object)
					{
						for (const TPair<
							FString,
							FUeAgentCanonicalValueRef>& Group
							: (*Relations)->GetObjectItems())
						{
							if (Group.Value->GetKind()
								== UE::UAI::StateSemantic::EUeAgentValueKind::Object)
							{
								RelationRecordCount +=
									Group.Value->GetObjectItems().Num();
							}
						}
					}
				}
			}
			if (Unit.GetUnitKind() == ESemanticUnitKind::ItemState
				&& Unit.GetValues().Contains(TEXT("builtin_id")))
			{
				bHasStableBuiltin = true;
			}
		}
		TestNotNull(TEXT("node state remains an item unit"), NodeUnit);
		if (NodeUnit)
		{
			TestFalse(
				TEXT("node core does not retain layout"),
				NodeUnit->GetValues().Contains(TEXT("pos")));
		}
		TestTrue(TEXT("node properties have a separate owner unit"), bHasNodeProperty);
		TestTrue(TEXT("node layout has a separate owner unit"), bHasNodeLayout);
		TestTrue(TEXT("graph links are relation units"), bHasRelation);
		TestEqual(
			TEXT("one graph owner emits one relation set"),
			RelationSetCount,
			1);
		TestEqual(
			TEXT("the relation set retains every captured relation"),
			RelationRecordCount,
			2);
		TestTrue(TEXT("referenced built-in nodes get stable item identities"), bHasStableBuiltin);

		FUeAgentResourceTracker PlanResources = MakeCompositionTracker();
		FExportWorkspacePlanPtr Plan;
		const bool bPlanBuilt = TestTrue(
			TEXT("large token telemetry does not block composition export planning"),
			TryBuildCompositionExportPlan(
				Model.ToSharedRef(),
				{
					{TEXT("estimated_tokens"), 1000000},
					{TEXT("estimated_bytes"), 8388608},
				},
				PlanResources,
				Plan,
				Error));
		if (!bPlanBuilt)
		{
			AddError(FString::Printf(
				TEXT("composition_export_error:%s:%s:%s"),
				*Error.GetCode(),
				*Error.GetSourceUnit(),
				*Error.GetJsonPointer()));
		}
		TestTrue(TEXT("composition export plan is available"), Plan.IsValid());
		if (Plan.IsValid())
		{
			TestTrue(
				TEXT("semantic workspace has the machine manifest"),
				Plan->GetFiles().ContainsByPredicate(
					[](const FExportFilePlan& File)
					{
						return File.GetRelativePath()
							== TEXT(".uai/workspace.hjson");
					}));
			TestFalse(
				TEXT("semantic workspace has no provider-extension path"),
				Plan->GetFiles().ContainsByPredicate(
					[](const FExportFilePlan& File)
					{
						return File.GetRelativePath().Contains(
							TEXT("provider_extension"));
					}));
		}
	}

	UMaterialInstanceConstant* MaterialInstance =
		NewObject<UMaterialInstanceConstant>(GetTransientPackage());
	TestNotNull(
		TEXT("transient Material Instance fixture is available"),
		MaterialInstance);
	if (MaterialInstance)
	{
		MaterialInstance->ScalarParameterValues.Add(
			FScalarParameterValue(
				FMaterialParameterInfo(TEXT("FixtureScalar")),
				0.625f));
		TArray<UeAgentAuthoringState::FReadRequest> InstanceRequests;
		FString InstanceError;
		TestTrue(
			TEXT("Material Instance export state is granular"),
			UeAgentMaterialAuthoring::
				BuildAssetExportSnapshotReadRequests(
					*MaterialInstance,
					InstanceRequests,
					InstanceError));
		TestTrue(
			TEXT("Material Instance state includes authored parameters"),
			InstanceRequests.ContainsByPredicate(
				[](const UeAgentAuthoringState::FReadRequest& Entry)
				{
					return Entry.SemanticId
						== UeAgentMaterialAuthoring::
							ExportInstanceParameterSemanticId;
				}));
		UeAgentAuthoringState::FCaptureResult InstanceCapture;
		TestTrue(
			TEXT("Material Instance granular state captures"),
			UeAgentAuthoringState::CaptureSnapshot(
				InstanceRequests,
				InstanceCapture));
		if (InstanceCapture.IsCaptured())
		{
			FCompositionRequest InstanceComposition;
			InstanceComposition.TargetKind =
				TEXT("ue.material_instance.asset");
			InstanceComposition.ProfileId = TEXT("llm_authoring");
			InstanceComposition.BaseRevision =
				TEXT("fixture-hx011-material-instance-r1");
			InstanceComposition.SourceProvenance =
				TEXT("fixture:hx011:material_instance");
			InstanceComposition.Entries =
				InstanceCapture.Snapshot.CopyEntries();
			FUeAgentResourceTracker InstanceResources =
				MakeCompositionTracker();
			FAuthoringModelPtr InstanceModel;
			TestTrue(
				TEXT("Material Instance parameters compose as common units"),
				TryComposeAuthoringModel(
					InstanceComposition,
					InstanceResources,
					InstanceModel,
					Error));
			TestTrue(
				TEXT("Material Instance parameter collection is populated"),
				InstanceModel.IsValid()
					&& InstanceModel->GetUnits().ContainsByPredicate(
						[](const FSemanticUnit& Unit)
						{
							return Unit.GetRole()
									== TEXT("parameter_collection")
								&& Unit.GetUnitKind()
									== ESemanticUnitKind::ItemState
								&& Unit.GetValues().Contains(
									TEXT("authored_override"));
						}));
		}
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentBlueprintMemberCompositionContractTest,
	"UeAgentInterface.Core.HjsonWorkspace.AuthoringComposition.BlueprintMembers",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentBlueprintMemberCompositionContractTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	FString ContractError;
	TestTrue(
		TEXT("graph/tree composition contracts register"),
		RegisterGraphTreeTargetCompositionContracts(ContractError));
	FTargetCompositionContract BlueprintContract;
	TestTrue(
		TEXT("Blueprint composition contract is available"),
		FTargetCompositionRegistry::Get().FindContract(
			TEXT("ue.blueprint.asset"), BlueprintContract));
	TestTrue(
		TEXT("Blueprint members use one readable structure-state surface"),
		BlueprintContract.Surfaces.ContainsByPredicate(
			[](const FCompositionSurfaceContract& Surface)
			{
				return Surface.SurfaceId == TEXT("member_collection")
					&& Surface.SurfaceKind == EAuthoringSurfaceKind::Structure
					&& Surface.UnitKind == ESemanticUnitKind::StructureState;
			}));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentAuthoringCompositionTargetIdentityBindingTest,
	"UeAgentInterface.Core.HjsonWorkspace.AuthoringComposition.TargetIdentityBinding",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentAuthoringCompositionTargetIdentityBindingTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	TSharedRef<FJsonObject> Target = MakeShared<FJsonObject>();
	Target->SetStringField(TEXT("asset_path"), TEXT("/Game/Fixture/HX013"));
	TSharedRef<FJsonObject> NodeA = MakeShared<FJsonObject>();
	NodeA->SetStringField(TEXT("title"), TEXT("A"));
	TSharedRef<FJsonObject> NodeB = MakeShared<FJsonObject>();
	NodeB->SetStringField(TEXT("title"), TEXT("B"));
	const FString NodeAKey(
		TEXT("blueprint_node|blueprint_node|node_guid|node_guid=a"));
	const FString NodeBKey(
		TEXT("blueprint_node|blueprint_node|node_guid|node_guid=b"));
	FAuthoringModelPtr Model = ComposeFixture(
		*this,
		TEXT("ue.blueprint.asset"),
		{
			MakeEntry(
				TEXT("uai.semantic.blueprint.asset.target"),
				TEXT("blueprint_asset"),
				TEXT("blueprint_asset|ue.blueprint.asset|asset_path|asset_path=fixture"),
				{{TEXT("asset_path"), TEXT("/Game/Fixture/HX013")}},
				MakeShared<FJsonValueObject>(Target)),
			MakeEntry(
				TEXT("uai.semantic.blueprint.node"),
				TEXT("blueprint_node"),
				NodeAKey,
				{
					{TEXT("asset_path"), TEXT("/Game/Fixture/HX013")},
					{TEXT("graph_guid"), TEXT("graph")},
					{TEXT("node_guid"), TEXT("a")},
				},
				MakeShared<FJsonValueObject>(NodeA)),
			MakeEntry(
				TEXT("uai.semantic.blueprint.node"),
				TEXT("blueprint_node"),
				NodeBKey,
				{
					{TEXT("asset_path"), TEXT("/Game/Fixture/HX013")},
					{TEXT("graph_guid"), TEXT("graph")},
					{TEXT("node_guid"), TEXT("b")},
				},
				MakeShared<FJsonValueObject>(NodeB)),
		});
	TSet<FString> BoundKeys;
	if (Model.IsValid())
	{
		for (const FSemanticUnit& Unit : Model->GetUnits())
		{
			if (Unit.GetSemanticTypeRef()
				== TEXT("uai.semantic.blueprint.node"))
			{
				BoundKeys.Add(Unit.GetTargetIdentityKey());
			}
		}
	}
	TestEqual(
		TEXT("same semantic type retains two exact Apply identities"),
		BoundKeys.Num(),
		2);
	TestTrue(TEXT("node A identity is preserved"), BoundKeys.Contains(NodeAKey));
	TestTrue(TEXT("node B identity is preserved"), BoundKeys.Contains(NodeBKey));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentOtherDomainAuthoringCompositionContractTest,
	"UeAgentInterface.Core.HjsonWorkspace.AuthoringComposition.OtherContracts",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentOtherDomainAuthoringCompositionContractTest::RunTest(
	const FString& Parameters)
{
	static const TCHAR* GenericSurfaces[] = {
		TEXT("target_settings"),
		TEXT("owned_collection"),
	};
	static const TCHAR* NiagaraSurfaces[] = {
		TEXT("target_settings"),
		TEXT("emitter_hierarchy"),
		TEXT("stack_collection"),
		TEXT("dynamic_input_tree"),
		TEXT("module_properties"),
	};
	static const TCHAR* TimelineSurfaces[] = {
		TEXT("target_settings"),
		TEXT("timeline_collection"),
	};
	static const TCHAR* LevelSurfaces[] = {
		TEXT("target_settings"),
		TEXT("actor_component_hierarchy"),
		TEXT("actor_component_properties"),
	};
	static const TCHAR* RigSurfaces[] = {
		TEXT("target_settings"),
		TEXT("rig_collection"),
		TEXT("rig_hierarchy"),
		TEXT("rigvm_graph"),
		TEXT("hierarchy_item_properties"),
	};
	static const TCHAR* BehaviorSurfaces[] = {
		TEXT("target_settings"),
		TEXT("behavior_tree"),
		TEXT("item_properties"),
	};
	static const TCHAR* AudioSurfaces[] = {
		TEXT("target_settings"),
		TEXT("audio_graph"),
		TEXT("item_properties"),
	};
	static const TCHAR* TableSurfaces[] = {
		TEXT("target_settings"),
		TEXT("table_collection"),
	};
	static const TCHAR* ExternalSurfaces[] = {
		TEXT("target_settings"),
		TEXT("source_payload"),
	};
	const FExpectedComposition Expected[] = {
		{TEXT("ue.generic.object"), GenericSurfaces, UE_ARRAY_COUNT(GenericSurfaces)},
		{TEXT("ue.niagara_system.asset"), NiagaraSurfaces, UE_ARRAY_COUNT(NiagaraSurfaces)},
		{TEXT("ue.niagara_emitter.asset"), NiagaraSurfaces, UE_ARRAY_COUNT(NiagaraSurfaces)},
		{TEXT("ue.niagara_script.asset"), NiagaraSurfaces, UE_ARRAY_COUNT(NiagaraSurfaces)},
		{TEXT("ue.level_sequence.asset"), TimelineSurfaces, UE_ARRAY_COUNT(TimelineSurfaces)},
		{TEXT("ue.animation_sequence.asset"), TimelineSurfaces, UE_ARRAY_COUNT(TimelineSurfaces)},
		{TEXT("ue.level.asset"), LevelSurfaces, UE_ARRAY_COUNT(LevelSurfaces)},
		{TEXT("ue.control_rig.asset"), RigSurfaces, UE_ARRAY_COUNT(RigSurfaces)},
		{TEXT("ue.ik_rig.asset"), RigSurfaces, UE_ARRAY_COUNT(RigSurfaces)},
		{TEXT("ue.skeleton.asset"), RigSurfaces, UE_ARRAY_COUNT(RigSurfaces)},
		{TEXT("ue.behavior_tree.asset"), BehaviorSurfaces, UE_ARRAY_COUNT(BehaviorSurfaces)},
		{TEXT("ue.metasound.asset"), AudioSurfaces, UE_ARRAY_COUNT(AudioSurfaces)},
		{TEXT("ue.sound_cue.asset"), AudioSurfaces, UE_ARRAY_COUNT(AudioSurfaces)},
		{TEXT("ue.data_table.asset"), TableSurfaces, UE_ARRAY_COUNT(TableSurfaces)},
		{TEXT("ue.imported_texture.asset"), ExternalSurfaces, UE_ARRAY_COUNT(ExternalSurfaces)},
		{TEXT("ue.imported_mesh.asset"), ExternalSurfaces, UE_ARRAY_COUNT(ExternalSurfaces)},
		{TEXT("ue.imported_audio.asset"), ExternalSurfaces, UE_ARRAY_COUNT(ExternalSurfaces)},
		{TEXT("ue.imported_csv.asset"), ExternalSurfaces, UE_ARRAY_COUNT(ExternalSurfaces)},
	};
	for (const FExpectedComposition& Row : Expected)
	{
		VerifyComposition(*this, Row);
	}

	TSharedRef<FJsonObject> Target = MakeShared<FJsonObject>();
	Target->SetStringField(TEXT("asset_path"), TEXT("/Game/Fixture/HX012"));

	TSharedRef<FJsonObject> Stack = MakeShared<FJsonObject>();
	Stack->SetStringField(TEXT("usage"), TEXT("ParticleUpdateScript"));
	TSharedRef<FJsonObject> Module = MakeShared<FJsonObject>();
	Module->SetStringField(TEXT("module_guid"), TEXT("module-1"));
	TSharedRef<FJsonObject> DynamicInput = MakeShared<FJsonObject>();
	DynamicInput->SetStringField(TEXT("input_name"), TEXT("Velocity"));
	TSharedRef<FJsonObject> Renderer = MakeShared<FJsonObject>();
	Renderer->SetStringField(TEXT("renderer_name"), TEXT("SpriteRenderer"));
	TSharedRef<FJsonObject> ModuleProperty = MakeShared<FJsonObject>();
	ModuleProperty->SetNumberField(TEXT("value"), 4.0);
	const TMap<FString, FString> NiagaraStageSelectors{
		{TEXT("asset_path"), TEXT("/Game/Fixture/HX012")},
		{TEXT("usage"), TEXT("ParticleUpdateScript")},
		{TEXT("usage_id"), TEXT("usage-1")},
	};
	TMap<FString, FString> NiagaraModuleSelectors = NiagaraStageSelectors;
	NiagaraModuleSelectors.Add(TEXT("module_guid"), TEXT("module-1"));
	TMap<FString, FString> NiagaraInputSelectors = NiagaraModuleSelectors;
	NiagaraInputSelectors.Add(TEXT("input_name"), TEXT("Velocity"));
	FAuthoringModelPtr NiagaraModel = ComposeFixture(
		*this,
		TEXT("ue.niagara_system.asset"),
		{
			MakeEntry(
				TEXT("uai.semantic.niagara.system"),
				TEXT("niagara_system"),
				TEXT("asset:/Game/Fixture/HX012"),
				{{TEXT("asset_path"), TEXT("/Game/Fixture/HX012")}},
				MakeShared<FJsonValueObject>(Target)),
			MakeEntry(
				TEXT("uai.semantic.niagara.stack_entry"),
				TEXT("niagara_stack_entry"),
				TEXT("stack:usage-1"),
				NiagaraStageSelectors,
				MakeShared<FJsonValueObject>(Stack)),
			MakeEntry(
				TEXT("uai.semantic.niagara.module"),
				TEXT("niagara_module"),
				TEXT("module:module-1"),
				NiagaraModuleSelectors,
				MakeShared<FJsonValueObject>(Module)),
			MakeEntry(
				TEXT("uai.semantic.niagara.dynamic_input"),
				TEXT("niagara_dynamic_input"),
				TEXT("input:Velocity"),
				NiagaraInputSelectors,
				MakeShared<FJsonValueObject>(DynamicInput)),
			MakeEntry(
				TEXT("uai.semantic.niagara.renderer"),
				TEXT("niagara_renderer"),
				TEXT("renderer:SpriteRenderer"),
				{
					{TEXT("asset_path"), TEXT("/Game/Fixture/HX012")},
					{TEXT("renderer_name"), TEXT("SpriteRenderer")},
				},
				MakeShared<FJsonValueObject>(Renderer)),
			MakeEntry(
				TEXT("uai.semantic.niagara.emitter_property"),
				TEXT("niagara_emitter_property"),
				TEXT("property:SpawnRate"),
				{
					{TEXT("asset_path"), TEXT("/Game/Fixture/HX012")},
					{TEXT("property_path"), TEXT("SpawnRate")},
				},
				MakeShared<FJsonValueObject>(ModuleProperty)),
		});
	TestTrue(
		TEXT("Niagara stages are common structure units"),
		NiagaraModel.IsValid()
			&& NiagaraModel->GetUnits().ContainsByPredicate(
				[](const FSemanticUnit& Unit)
				{
					return Unit.GetSemanticTypeRef()
							== TEXT("uai.semantic.niagara.stack_entry")
						&& Unit.GetRole() == TEXT("stack_collection")
						&& Unit.GetUnitKind()
							== ESemanticUnitKind::StructureState;
				}));
	TestTrue(
		TEXT("Niagara modules are common item units"),
		NiagaraModel.IsValid()
			&& NiagaraModel->GetUnits().ContainsByPredicate(
				[](const FSemanticUnit& Unit)
				{
					return Unit.GetSemanticTypeRef()
							== TEXT("uai.semantic.niagara.module")
						&& Unit.GetRole() == TEXT("stack_collection")
						&& Unit.GetUnitKind()
							== ESemanticUnitKind::ItemState;
				}));
	TestTrue(
		TEXT("Niagara dynamic inputs are independently addressable"),
		HasPopulatedRole(
			NiagaraModel,
			TEXT("dynamic_input_tree"),
			ESemanticUnitKind::ItemState));
	TestTrue(
		TEXT("Niagara module properties have a separate owner"),
		HasPopulatedRole(
			NiagaraModel,
			TEXT("module_properties"),
			ESemanticUnitKind::PropertySet));

	TSharedRef<FJsonObject> ActorTarget = MakeShared<FJsonObject>();
	ActorTarget->SetStringField(TEXT("actor_guid"), TEXT("actor-1"));
	ActorTarget->SetStringField(TEXT("actor_class"), TEXT("/Script/Engine.Actor"));
	ActorTarget->SetStringField(TEXT("actor_class"), TEXT("/Script/Engine.PointLight"));
	TSharedRef<FJsonObject> ActorState = MakeShared<FJsonObject>();
	ActorState->SetStringField(TEXT("label"), TEXT("FixtureActor"));
	TSharedRef<FJsonObject> ActorTransform = MakeShared<FJsonObject>();
	ActorTransform->SetArrayField(
		TEXT("location"),
		{
			MakeShared<FJsonValueNumber>(100.0),
			MakeShared<FJsonValueNumber>(200.0),
			MakeShared<FJsonValueNumber>(300.0),
		});
	ActorState->SetObjectField(TEXT("transform"), ActorTransform);
	TSharedRef<FJsonObject> ComponentTarget = MakeShared<FJsonObject>();
	ComponentTarget->SetStringField(TEXT("component_name"), TEXT("Root"));
	ComponentTarget->SetStringField(TEXT("component_class"), TEXT("/Script/Engine.SceneComponent"));
	TSharedRef<FJsonObject> ComponentProperty =
		MakeShared<FJsonObject>();
	ComponentProperty->SetBoolField(TEXT("value"), true);
	const TMap<FString, FString> ActorSelectors{
		{TEXT("asset_path"), TEXT("/Game/Fixture/Level")},
		{TEXT("actor_guid"), TEXT("actor-1")},
	};
	TSharedRef<FJsonObject> UnlabelledActorTarget = MakeShared<FJsonObject>();
	UnlabelledActorTarget->SetStringField(TEXT("actor_guid"), TEXT("actor-2"));
	UnlabelledActorTarget->SetStringField(
		TEXT("actor_class"), TEXT("/Script/Engine.PointLight"));
	TSharedRef<FJsonObject> UnlabelledActorState = MakeShared<FJsonObject>();
	const TMap<FString, FString> UnlabelledActorSelectors{
		{TEXT("asset_path"), TEXT("/Game/Fixture/Level")},
		{TEXT("actor_guid"), TEXT("actor-2")},
	};
	const TMap<FString, FString> ComponentSelectors{
		{TEXT("asset_path"), TEXT("/Game/Fixture/Level")},
		{TEXT("owner_actor_guid"), TEXT("actor-1")},
		{TEXT("component_name"), TEXT("Root")},
	};
	FAuthoringModelPtr LevelModel = ComposeFixture(
		*this,
		TEXT("ue.level.asset"),
		{
			MakeEntry(
				TEXT("uai.semantic.level.world.target"),
				TEXT("level_world"),
				TEXT("world:/Game/Fixture/Level"),
				{{TEXT("asset_path"), TEXT("/Game/Fixture/Level")}},
				MakeShared<FJsonValueObject>(Target)),
			MakeEntry(
				TEXT("uai.semantic.level.actor.target"),
				TEXT("level_actor"),
				TEXT("actor:actor-1"),
				ActorSelectors,
				MakeShared<FJsonValueObject>(ActorTarget)),
			MakeEntry(
				TEXT("uai.semantic.level.actor"),
				TEXT("level_actor"),
				TEXT("actor:actor-1"),
				ActorSelectors,
				MakeShared<FJsonValueObject>(ActorState)),
			MakeEntry(
				TEXT("uai.semantic.level.actor.target"),
				TEXT("level_actor"),
				TEXT("actor:actor-2"),
				UnlabelledActorSelectors,
				MakeShared<FJsonValueObject>(UnlabelledActorTarget)),
			MakeEntry(
				TEXT("uai.semantic.level.actor"),
				TEXT("level_actor"),
				TEXT("actor:actor-2"),
				UnlabelledActorSelectors,
				MakeShared<FJsonValueObject>(UnlabelledActorState)),
			MakeEntry(
				TEXT("uai.semantic.level.component.target"),
				TEXT("level_component"),
				TEXT("component:actor-1:Root"),
				ComponentSelectors,
				MakeShared<FJsonValueObject>(ComponentTarget)),
			MakeEntry(
				TEXT("uai.semantic.level.component.property"),
				TEXT("level_component_property"),
				TEXT("component_property:actor-1:Root:Visible"),
				{
					{TEXT("asset_path"), TEXT("/Game/Fixture/Level")},
					{TEXT("owner_actor_guid"), TEXT("actor-1")},
					{TEXT("component_name"), TEXT("Root")},
					{TEXT("property_path"), TEXT("Visible")},
				},
				MakeShared<FJsonValueObject>(ComponentProperty)),
		});
	TestTrue(
		TEXT("level actors/components are common hierarchy items"),
		HasPopulatedRole(
			LevelModel,
			TEXT("actor_component_hierarchy"),
			ESemanticUnitKind::ItemState));
	TestTrue(
		TEXT("level actor/component properties have separate units"),
		HasPopulatedRole(
			LevelModel,
			TEXT("actor_component_properties"),
			ESemanticUnitKind::PropertySet));
	TestTrue(
		TEXT("level state fallback uses a readable semantic field-set key"),
		LevelModel.IsValid()
			&& LevelModel->GetUnits().ContainsByPredicate(
				[](const FSemanticUnit& Unit)
				{
					return Unit.GetRole()
							== TEXT("actor_component_properties")
						&& Unit.GetSemanticTypeRef()
							== TEXT("uai.semantic.level.actor")
						&& Unit.GetFieldSetRef() == TEXT("level_actor");
				}));
	TestFalse(
		TEXT("level property field-set keys contain no binding or hash-shaped fallback"),
		LevelModel.IsValid()
			&& LevelModel->GetUnits().ContainsByPredicate(
				[](const FSemanticUnit& Unit)
				{
					return Unit.GetRole()
							== TEXT("actor_component_properties")
						&& (Unit.GetFieldSetRef().Contains(TEXT("binding:"))
							|| Unit.GetFieldSetRef().StartsWith(TEXT("sha")));
				}));
	TestTrue(
		TEXT("level spatial transform remains a canonical structured value"),
		LevelModel.IsValid()
			&& LevelModel->GetUnits().ContainsByPredicate(
				[](const FSemanticUnit& Unit)
				{
					return Unit.GetRole()
							== TEXT("actor_component_properties")
						&& Unit.GetValues().Contains(
							TEXT("transform"));
				}));
	TestTrue(
		TEXT("level hierarchy owns one keyed actors desired-state map"),
		LevelModel.IsValid()
			&& LevelModel->GetUnits().ContainsByPredicate(
				[](const FSemanticUnit& Unit)
				{
					return Unit.GetRole() == TEXT("actor_component_hierarchy")
						&& Unit.GetUnitKind() == ESemanticUnitKind::StructureState
						&& Unit.GetValues().Contains(TEXT("actors"));
				}));
	if (LevelModel.IsValid())
	{
		FUeAgentResourceTracker LevelPlanResources = MakeCompositionTracker();
		FExportWorkspacePlanPtr LevelPlan;
		FWorkspaceError LevelPlanError;
		TestTrue(TEXT("level keyed actors export plan builds"),
			TryBuildCompositionExportPlan(
				LevelModel.ToSharedRef(), {}, LevelPlanResources, LevelPlan, LevelPlanError));
		bool bFoundActorsDocument = false;
		bool bFoundVisibleHierarchyItem = false;
		bool bManifestCapturedActors = false;
		bool bManifestCapturedItemIdentity = false;
		if (LevelPlan.IsValid())
		{
			for (const FExportFilePlan& File : LevelPlan->GetFiles())
			{
				const FUTF8ToTCHAR Text(
					reinterpret_cast<const ANSICHAR*>(File.GetCanonicalUtf8().GetData()),
					File.GetCanonicalUtf8().Num());
				const FString Visible(Text.Length(), Text.Get());
				if (File.GetRelativePath() == TEXT(".uai/workspace.hjson"))
				{
					bManifestCapturedActors =
						Visible.Contains(TEXT("captured_values"))
						&& Visible.Contains(TEXT("actors"));
					bManifestCapturedItemIdentity =
						Visible.Contains(TEXT("actor_guid"))
						&& Visible.Contains(TEXT("component_name"));
					continue;
				}
				if (Visible.Contains(TEXT("actor_class"))
					|| Visible.Contains(TEXT("component_class")))
				{
					bFoundVisibleHierarchyItem = true;
					TestFalse(TEXT("visible hierarchy item omits unit id"),
						Visible.Contains(TEXT("unit_id")));
					TestFalse(TEXT("visible hierarchy item omits actor guid"),
						Visible.Contains(TEXT("actor_guid")));
					TestFalse(TEXT("visible hierarchy item omits object path"),
						Visible.Contains(TEXT("object_path")));
				}
				if (Visible.Contains(TEXT("actors"))
					&& Visible.Contains(TEXT("FixtureActor")))
				{
					bFoundActorsDocument = true;
					TestFalse(TEXT("visible actors omit captured values"),
						Visible.Contains(TEXT("captured_values")));
					TestFalse(TEXT("visible actors omit actor guid"),
						Visible.Contains(TEXT("actor_guid")));
					TestFalse(TEXT("visible actors omit operations"),
						Visible.Contains(TEXT("operation")));
				}
			}
		}
		TestTrue(TEXT("ordinary level file contains readable keyed actors"),
			bFoundActorsDocument);
		TestTrue(TEXT("level manifest captures keyed actor baseline"),
			bManifestCapturedActors);
		TestTrue(TEXT("ordinary level files contain readable hierarchy items"),
			bFoundVisibleHierarchyItem);
		TestTrue(TEXT("level manifest captures hierarchy item identity"),
			bManifestCapturedItemIdentity);
	}
	bool bComponentOwnedByActor = false;
	if (LevelModel.IsValid())
	{
		TSet<FString> ActorUnitIds;
		for (const FSemanticUnit& Unit : LevelModel->GetUnits())
		{
			if (Unit.GetSemanticTypeRef().EndsWith(
				TEXT(".actor.target")))
			{
				ActorUnitIds.Add(Unit.GetUnitId());
			}
		}
		bComponentOwnedByActor =
			!ActorUnitIds.IsEmpty()
			&& LevelModel->GetUnits().ContainsByPredicate(
				[&ActorUnitIds](const FSemanticUnit& Unit)
				{
					return Unit.GetSemanticTypeRef().EndsWith(
							TEXT(".component.target"))
						&& ActorUnitIds.Contains(Unit.GetOwnerRef());
				});
	}
	TestTrue(
		TEXT("level component owner points to its actor item"),
		bComponentOwnedByActor);

	FAuthoringModelPtr TimelineModel = ComposeFixture(
		*this,
		TEXT("ue.level_sequence.asset"),
		{
			MakeEntry(
				TEXT("uai.semantic.reflection.sequence.export.target"),
				TEXT("reflection.sequence"),
				TEXT("asset:/Game/Fixture/Sequence"),
				{{TEXT("asset_path"), TEXT("/Game/Fixture/Sequence")}},
				MakeShared<FJsonValueObject>(Target)),
			MakeEntry(
				TEXT("uai.semantic.reflection.sequence.profile_state"),
				TEXT("reflection.sequence"),
				TEXT("asset:/Game/Fixture/Sequence"),
				{{TEXT("asset_path"), TEXT("/Game/Fixture/Sequence")}},
				MakeShared<FJsonValueObject>(MakeTimelineDocument())),
		});
	TestTrue(
		TEXT("track/section/key document is decomposed into timeline items"),
		HasPopulatedRole(
			TimelineModel,
			TEXT("timeline_collection"),
			ESemanticUnitKind::ItemState));
	TestTrue(
		TEXT("timeline key keeps frame, seconds and value together"),
		TimelineModel.IsValid()
			&& TimelineModel->GetUnits().ContainsByPredicate(
				[](const FSemanticUnit& Unit)
				{
					return Unit.GetRole()
							== TEXT("timeline_collection")
						&& Unit.GetValues().Contains(TEXT("frame"))
						&& Unit.GetValues().Contains(TEXT("seconds"))
						&& Unit.GetValues().Contains(TEXT("value"));
				}));

	FAuthoringModelPtr RigModel = ComposeFixture(
		*this,
		TEXT("ue.control_rig.asset"),
		{
			MakeEntry(
				TEXT("uai.semantic.rigging.control_rig.export.target"),
				TEXT("rigging.control_rig"),
				TEXT("asset:/Game/Fixture/Rig"),
				{{TEXT("asset_path"), TEXT("/Game/Fixture/Rig")}},
				MakeShared<FJsonValueObject>(Target)),
			MakeEntry(
				TEXT("uai.semantic.rigging.control_rig.document"),
				TEXT("rigging.control_rig"),
				TEXT("asset:/Game/Fixture/Rig"),
				{{TEXT("asset_path"), TEXT("/Game/Fixture/Rig")}},
				MakeShared<FJsonValueObject>(MakeRigDocument())),
		});
	TestTrue(
		TEXT("rig hierarchy is independently addressable"),
		HasPopulatedRole(
			RigModel,
			TEXT("rig_hierarchy"),
			ESemanticUnitKind::ItemState));
	TestTrue(
		TEXT("RigVM graph is independently addressable"),
		HasPopulatedRole(
			RigModel,
			TEXT("rigvm_graph"),
			ESemanticUnitKind::ItemState));
	TestTrue(
		TEXT("RigVM node layout uses common layout units"),
		RigModel.IsValid()
			&& RigModel->GetUnits().ContainsByPredicate(
				[](const FSemanticUnit& Unit)
				{
					return Unit.GetUnitKind()
						== ESemanticUnitKind::LayoutState;
				}));

	FAuthoringModelPtr AudioModel = ComposeFixture(
		*this,
		TEXT("ue.metasound.asset"),
		{
			MakeEntry(
				TEXT("uai.semantic.audio.metasound.export.target"),
				TEXT("audio.metasound"),
				TEXT("asset:/Game/Fixture/MetaSound"),
				{{TEXT("asset_path"), TEXT("/Game/Fixture/MetaSound")}},
				MakeShared<FJsonValueObject>(Target)),
			MakeEntry(
				TEXT("uai.semantic.audio.metasound.document"),
				TEXT("audio.metasound"),
				TEXT("asset:/Game/Fixture/MetaSound"),
				{{TEXT("asset_path"), TEXT("/Game/Fixture/MetaSound")}},
				MakeShared<FJsonValueObject>(MakeAudioDocument())),
		});
	TestTrue(
		TEXT("audio graph nodes are common item units"),
		HasPopulatedRole(
			AudioModel,
			TEXT("audio_graph"),
			ESemanticUnitKind::ItemState));
	TestTrue(
		TEXT("audio graph edges are common relation units"),
		HasPopulatedRole(
			AudioModel,
			TEXT("audio_graph"),
			ESemanticUnitKind::RelationSet));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentBlueprintKeyedComponentEnvelopeTest,
	"UeAgentInterface.Core.HjsonWorkspace.AuthoringComposition.BlueprintKeyedComponentEnvelope",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentBlueprintKeyedComponentEnvelopeTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	FString ContractError;
	TestTrue(
		TEXT("Blueprint composition contracts register"),
		RegisterGraphTreeTargetCompositionContracts(ContractError));
	if (!ContractError.IsEmpty())
	{
		AddError(ContractError);
	}

	TSharedRef<FJsonObject> Target = MakeShared<FJsonObject>();
	Target->SetStringField(
		TEXT("asset_path"), TEXT("/Game/Fixture/BP_ComponentProjection"));
	Target->SetStringField(TEXT("asset_kind"), TEXT("blueprint"));
	TSharedRef<FJsonObject> Component = MakeShared<FJsonObject>();
	Component->SetStringField(TEXT("name"), TEXT("DefaultSceneRoot"));
	Component->SetStringField(
		TEXT("class"), TEXT("/Script/Engine.SceneComponent"));
	Component->SetStringField(TEXT("parent"), TEXT(""));
	Component->SetStringField(
		TEXT("component_guid"), TEXT("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee"));

	FCompositionRequest Request;
	Request.TargetKind = TEXT("ue.blueprint.asset");
	Request.ProfileId = TEXT("llm_authoring");
	Request.BaseRevision = TEXT("fixture-component-r1");
	Request.SourceProvenance = TEXT("fixture:component:native");
	Request.Entries = {
		MakeEntry(
			TEXT("uai.semantic.blueprint.export.target"),
			TEXT("blueprint_asset"),
			TEXT("blueprint:/Game/Fixture/BP_ComponentProjection"),
			{{TEXT("asset_path"), TEXT("/Game/Fixture/BP_ComponentProjection")}},
			MakeShared<FJsonValueObject>(Target)),
		MakeEntry(
			TEXT("uai.semantic.blueprint.component"),
			TEXT("blueprint_component"),
			TEXT("component:DefaultSceneRoot"),
			{
				{TEXT("asset_path"), TEXT("/Game/Fixture/BP_ComponentProjection")},
				{TEXT("component_guid"), TEXT("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee")},
				{TEXT("component_name"), TEXT("DefaultSceneRoot")},
			},
			MakeShared<FJsonValueObject>(Component)),
	};

	FUeAgentResourceTracker Resources = MakeCompositionTracker();
	FAuthoringModelPtr Model;
	FWorkspaceError Error;
	TestTrue(
		TEXT("minimal Blueprint component state composes"),
		TryComposeAuthoringModel(Request, Resources, Model, Error));
	if (!Model.IsValid())
	{
		AddError(Error.GetCode());
		return false;
	}

	FUeAgentResourceTracker PlanResources = MakeCompositionTracker();
	FExportWorkspacePlanPtr Plan;
	TestTrue(
		TEXT("minimal Blueprint component export plan builds"),
		TryBuildCompositionExportPlan(
			Model.ToSharedRef(), {}, PlanResources, Plan, Error));
	if (!Plan.IsValid())
	{
		AddError(Error.GetCode());
		return false;
	}

	bool bFoundVisibleComponents = false;
	bool bManifestCapturedComponents = false;
	for (const FExportFilePlan& File : Plan->GetFiles())
	{
		const FUTF8ToTCHAR Text(
			reinterpret_cast<const ANSICHAR*>(File.GetCanonicalUtf8().GetData()),
			File.GetCanonicalUtf8().Num());
		const FString Contents(Text.Length(), Text.Get());
		if (File.GetRelativePath() == TEXT(".uai/workspace.hjson"))
		{
			bManifestCapturedComponents =
				Contents.Contains(TEXT("captured_values"))
				&& Contents.Contains(TEXT("components"))
				&& Contents.Contains(TEXT("component_guid"));
			continue;
		}
		if (Contents.Contains(TEXT("components"))
			&& Contents.Contains(TEXT("DefaultSceneRoot")))
		{
			bFoundVisibleComponents =
				Contents.Contains(TEXT("Component1"))
				&& Contents.Contains(TEXT("name"))
				&& Contents.Contains(TEXT("class"))
				&& Contents.Contains(TEXT("parent"))
				&& !Contents.Contains(TEXT("captured_values"))
				&& !Contents.Contains(TEXT("operation"))
				&& !Contents.Contains(TEXT("component_guid"));
		}
	}
	TestTrue(TEXT("ordinary Blueprint file exposes readable components"),
		bFoundVisibleComponents);
	TestTrue(TEXT("machine manifest captures component baseline"),
		bManifestCapturedComponents);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentLevelKeyedActorEnvelopeTest,
	"UeAgentInterface.Core.HjsonWorkspace.AuthoringComposition.LevelKeyedActorEnvelope",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentLevelKeyedActorEnvelopeTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	FString ContractError;
	TestTrue(
		TEXT("level composition contracts register"),
		RegisterOtherTargetCompositionContracts(ContractError));
	TSharedRef<FJsonObject> LevelTarget = MakeShared<FJsonObject>();
	LevelTarget->SetStringField(TEXT("asset_path"), TEXT("/Game/Fixture/Level"));
	TSharedRef<FJsonObject> ActorTarget = MakeShared<FJsonObject>();
	ActorTarget->SetStringField(TEXT("actor_guid"), TEXT("actor-1"));
	TSharedRef<FJsonObject> ActorState = MakeShared<FJsonObject>();
	ActorState->SetStringField(TEXT("label"), TEXT("FixtureActor"));
	TSharedRef<FJsonObject> ActorTransform = MakeShared<FJsonObject>();
	ActorTransform->SetArrayField(
		TEXT("location"),
		{
			MakeShared<FJsonValueNumber>(100.0),
			MakeShared<FJsonValueNumber>(200.0),
			MakeShared<FJsonValueNumber>(300.0),
		});
	ActorState->SetObjectField(TEXT("transform"), ActorTransform);
	const TMap<FString, FString> ActorSelectors{
		{TEXT("asset_path"), TEXT("/Game/Fixture/Level")},
		{TEXT("actor_guid"), TEXT("actor-1")},
	};
	FAuthoringModelPtr Model = ComposeFixture(
		*this,
		TEXT("ue.level.asset"),
		{
			MakeEntry(
				TEXT("uai.semantic.level.world.target"),
				TEXT("level_world"),
				TEXT("world:/Game/Fixture/Level"),
				{{TEXT("asset_path"), TEXT("/Game/Fixture/Level")}},
				MakeShared<FJsonValueObject>(LevelTarget)),
			MakeEntry(
				TEXT("uai.semantic.level.actor.target"),
				TEXT("level_actor"),
				TEXT("actor:actor-1"),
				ActorSelectors,
				MakeShared<FJsonValueObject>(ActorTarget)),
			MakeEntry(
				TEXT("uai.semantic.level.actor"),
				TEXT("level_actor"),
				TEXT("actor:actor-1"),
				ActorSelectors,
				MakeShared<FJsonValueObject>(ActorState)),
		});
	if (!Model.IsValid())
	{
		return false;
	}

	FUeAgentResourceTracker PlanResources = MakeCompositionTracker();
	FExportWorkspacePlanPtr Plan;
	FWorkspaceError Error;
	TestTrue(
		TEXT("minimal level keyed actor export plan builds"),
		TryBuildCompositionExportPlan(
			Model.ToSharedRef(), {}, PlanResources, Plan, Error));
	if (!Plan.IsValid())
	{
		return false;
	}

	bool bManifestCapturedActors = false;
	bool bFoundVisibleActors = false;
	bool bFoundFallbackActor = false;
	bool bFoundReadableActorProperties = false;
	for (const FExportFilePlan& File : Plan->GetFiles())
	{
		const FUTF8ToTCHAR Text(
			reinterpret_cast<const ANSICHAR*>(File.GetCanonicalUtf8().GetData()),
			File.GetCanonicalUtf8().Num());
		const FString Contents(Text.Length(), Text.Get());
		if (File.GetRelativePath() == TEXT(".uai/workspace.hjson"))
		{
			bManifestCapturedActors =
				Contents.Contains(TEXT("captured_values"))
				&& Contents.Contains(TEXT("actors"));
			continue;
		}
		if (Contents.Contains(TEXT("actors"))
			&& (Contents.Contains(TEXT("FixtureActor: {"))
				|| Contents.Contains(TEXT("\"FixtureActor\": {"))))
		{
			bFoundVisibleActors = true;
			bFoundFallbackActor = Contents.Contains(TEXT("Actor: {"))
				|| Contents.Contains(TEXT("\"Actor\": {"));
			TestFalse(TEXT("visible actor structure omits captured values"),
				Contents.Contains(TEXT("captured_values")));
			TestFalse(TEXT("visible actor structure omits actor guid"),
				Contents.Contains(TEXT("actor_guid")));
			TestFalse(TEXT("visible actor structure omits operations"),
				Contents.Contains(TEXT("operation")));
		}
		if (File.GetRelativePath().EndsWith(TEXT("/properties.hjson"))
			&& Contents.Contains(TEXT("FixtureActor"))
			&& Contents.Contains(TEXT("transform")))
		{
			bFoundReadableActorProperties =
				Contents.Contains(TEXT("level_actor"));
			TestFalse(TEXT("visible actor property rows omit binding identities"),
				Contents.Contains(TEXT("binding:")));
			TestFalse(TEXT("visible actor property rows omit digest markers"),
				Contents.Contains(TEXT("sha256:")));
		}
	}
	TestTrue(TEXT("machine manifest captures actors baseline"),
		bManifestCapturedActors);
	TestTrue(TEXT("ordinary structure exposes readable actors"),
		bFoundVisibleActors);
	TestTrue(TEXT("unlabelled actor uses a stable readable fallback key"),
		bFoundFallbackActor);
	TestTrue(TEXT("ordinary properties expose a readable actor state row"),
		bFoundReadableActorProperties);
	return !HasAnyErrors();
}

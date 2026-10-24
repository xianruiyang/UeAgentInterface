#include "Misc/AutomationTest.h"

#include "DomainAdapters/PcgAiNodeGraph/UeAgentPcgAiNodeGraphContract.h"

#include "DomainAdapters/PcgAiNodeGraph/UeAgentPcgAiNodeGraphAdapter.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentPcgAiNodeGraphContractTest,
	"UeAgentInterface.DomainAdapters.PcgAiNodeGraph.E0.Contract."
		"IdentityTypeLinkCompileAndReadback",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentPcgAiNodeGraphContractTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI::DomainAdapters::PcgAiNodeGraph;
	TestEqual(TEXT("production contract version"),
		UAI_PCG_AI_NODE_GRAPH_CONTRACT_V1, 1);
	const auto& ProductionFamilies = GetPcgAiNodeGraphPortFamilyContracts();
	const auto& ProductionCapabilities =
		GetPcgAiNodeGraphCapabilityContracts();
	TSet<FString> ProductionFamilyIds;
	for (const auto& Family : ProductionFamilies)
	{
		const FString FamilyId = Family.GetFamilyId();
		TestFalse(TEXT("production family id is explicit"), FamilyId.IsEmpty());
		TestFalse(TEXT("production family id is unique: ") + FamilyId,
			ProductionFamilyIds.Contains(FamilyId));
		ProductionFamilyIds.Add(FamilyId);
		TestFalse(TEXT("production family identity facets are explicit: ")
			+ FamilyId, Family.GetRequiredIdentityFacets().IsEmpty());
		TestFalse(TEXT("production family lifecycle stages are explicit: ")
			+ FamilyId, Family.GetRequiredStages().IsEmpty());
		TestFalse(TEXT("production family barrier is explicit: ") + FamilyId,
			Family.GetBarrierKind().IsEmpty());
		TestFalse(TEXT("production family oracle is explicit: ") + FamilyId,
			Family.GetOracleKind().IsEmpty());
	}

	TSet<FString> ProductionCapabilityIds;
	for (const auto& Capability : ProductionCapabilities)
	{
		const FString CapabilityId = Capability.GetCapabilityId();
		TestFalse(TEXT("production capability id is explicit"),
			CapabilityId.IsEmpty());
		TestFalse(TEXT("production capability id is unique: ") + CapabilityId,
			ProductionCapabilityIds.Contains(CapabilityId));
		ProductionCapabilityIds.Add(CapabilityId);
		TestFalse(TEXT("production capability descriptor is explicit: ")
			+ CapabilityId, Capability.GetDescriptorId().IsEmpty());
		TestTrue(TEXT("production capability family exists: ") + CapabilityId,
			ProductionFamilyIds.Contains(Capability.GetPortFamilyId()));
		TestFalse(TEXT("production capability operation kind is explicit: ")
			+ CapabilityId, Capability.GetOperationKind().IsEmpty());
		TestFalse(TEXT("production capability effect class is explicit: ")
			+ CapabilityId, Capability.GetEffectClass().IsEmpty());
		TestTrue(TEXT("production capability version is valid: ")
			+ CapabilityId, Capability.GetVersion() > 0);
	}

	TestEqual(TEXT("production Adapter contract version"),
		UAI_PCG_AI_NODE_GRAPH_ADAPTER_V1, 1);
	FPcgAiNodeGraphAdapterError AdapterError;
	const FPcgAiNodeGraphTopology PcgTopology(
		TEXT("pcg_graph"),
		TEXT("/Game/UAI/GC006/PCG_GraphFixture"),
		TEXT("pcg_graph:root"),
		{
			FPcgAiNodeGraphItemIdentity(
				TEXT("pcg_builtin:input"), FString(),
				TEXT("pcg_graph_input"),
				TEXT("/Script/PCG.PCGNode"),
				TEXT("/Script/PCG.PCGGraphInputOutputSettings")),
			FPcgAiNodeGraphItemIdentity(
				TEXT("pcg_node:PCG_1"), TEXT("PCG_1"), FString(),
				TEXT("/Script/PCG.PCGNode"),
				TEXT("/Script/PCG.PCGCreatePointsGridSettings")),
		},
		{
			FPcgAiNodeGraphPinIdentity(
				TEXT("pin:input:output:In"),
				TEXT("pcg_builtin:input"), TEXT("In"),
				EPcgAiNodeGraphPinDirection::Output,
				TEXT("pcg.data:any")),
			FPcgAiNodeGraphPinIdentity(
				TEXT("pin:grid:input:In"),
				TEXT("pcg_node:PCG_1"), TEXT("In"),
				EPcgAiNodeGraphPinDirection::Input,
				TEXT("pcg.data:point")),
		},
		{
			FPcgAiNodeGraphRelationIdentity(
				TEXT("edge:input-to-grid"), TEXT("graph_link"),
				FString(),
				TEXT("pin:input:output:In"),
				TEXT("pin:grid:input:In"), INDEX_NONE),
		});
	TestTrue(TEXT("PCG native item/type/pin/link topology validates"),
		TryValidatePcgAiNodeGraphTopology(PcgTopology, AdapterError));
	TestTrue(TEXT("item preserves carrier and payload type separately"),
		PcgTopology.GetItems()[1].GetNativeCarrierTypeIdentity()
			== TEXT("/Script/PCG.PCGNode")
			&& PcgTopology.GetItems()[1].GetNativeTypeIdentity()
				== TEXT("/Script/PCG.PCGCreatePointsGridSettings"));

	const FPcgAiNodeGraphAssetIdentity AssetIdentity(
		TEXT("behavior_tree"),
		TEXT("/Game/UAI/GC006/BT_Fixture.BT_Fixture"),
		TEXT("/Script/AIModule.BehaviorTree"));
	TestTrue(TEXT("asset lifecycle identity validates"),
		TryValidatePcgAiNodeGraphAssetIdentity(
			AssetIdentity, AdapterError));

	const FPcgAiNodeGraphBlackboardEntryIdentity BlackboardIdentity(
		TEXT("/Game/UAI/GC006/BB_Fixture.BB_Fixture"),
		TEXT("TargetActor"),
		TEXT("/Script/AIModule.BlackboardKeyType_Object"),
		EPcgAiNodeGraphEntryOwnership::Local);
	TestTrue(TEXT("Blackboard owner/name/type/ownership validates"),
		TryValidatePcgAiNodeGraphBlackboardEntryIdentity(
			BlackboardIdentity, AdapterError));

	const FPcgAiNodeGraphParameterIdentity ParameterIdentity(
		TEXT("/Game/UAI/GC006/PCG_GraphFixture.PCG_GraphFixture"),
		FGuid(TEXT("11111111-2222-3333-4444-555555555555")),
		TEXT("Density"), TEXT("double"));
	TestTrue(TEXT("PCG property GUID/name/type identity validates"),
		TryValidatePcgAiNodeGraphParameterIdentity(
			ParameterIdentity, AdapterError));

	const FPcgAiNodeGraphComponentBindingIdentity ComponentIdentity(
		TEXT("/Game/UAI/GC006/GC006Actor"),
		TEXT("/Game/UAI/GC006/GC006Actor.PCGComponent"),
		TEXT("/Game/UAI/GC006/PCG_GraphFixture.PCG_GraphFixture"),
		TEXT("generate_on_demand"));
	TestTrue(TEXT("explicit actor/component/graph/policy validates"),
		TryValidatePcgAiNodeGraphComponentBindingIdentity(
			ComponentIdentity, AdapterError));

	const FPcgAiNodeGraphDataAssetIdentity DataAssetIdentity(
		TEXT("/Game/UAI/GC006/PCG_DataFixture.PCG_DataFixture"),
		TEXT("/Script/PCG.PCGParamData"),
		TEXT("/Game/Maps/WB_WhiteBox01"),
		TEXT("sha256:fixture-pcg-data-revision"));
	TestTrue(TEXT("PCG data/source/revision identity validates"),
		TryValidatePcgAiNodeGraphDataAssetIdentity(
			DataAssetIdentity, AdapterError));

	const FPcgAiNodeGraphTopology MissingType(
		TEXT("behavior_tree"), TEXT("/Game/UAI/GC006/BT_Fixture"),
		TEXT("behavior_tree:root"),
		{
			FPcgAiNodeGraphItemIdentity(
				TEXT("bt_node:root"),
				TEXT("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee"),
				FString(),
				TEXT("/Script/BehaviorTreeEditor.BehaviorTreeGraphNode_Root"),
				FString()),
		}, {}, {});
	TestFalse(TEXT("missing native type fails preflight"),
		TryValidatePcgAiNodeGraphTopology(MissingType, AdapterError));
	TestEqual(TEXT("missing type error is stable"), AdapterError.GetCode(),
		FString(TEXT("pcg_ai_node_graph_type_identity_missing")));

	const FPcgAiNodeGraphTopology DuplicateItems(
		TEXT("pcg_graph"), TEXT("pcg:duplicate"), TEXT("pcg:root"),
		{
			FPcgAiNodeGraphItemIdentity(
				TEXT("node:same"), TEXT("NativeA"), FString(),
				TEXT("/Script/PCG.PCGNode"),
				TEXT("/Script/PCG.PCGSettings")),
			FPcgAiNodeGraphItemIdentity(
				TEXT("node:same"), TEXT("NativeB"), FString(),
				TEXT("/Script/PCG.PCGNode"),
				TEXT("/Script/PCG.PCGSettings")),
		}, {}, {});
	TestFalse(TEXT("duplicate item identity fails closed"),
		TryValidatePcgAiNodeGraphTopology(DuplicateItems, AdapterError));
	TestEqual(TEXT("duplicate item error is stable"),
		AdapterError.GetCode(),
		FString(TEXT("pcg_ai_node_graph_item_identity_duplicate")));

	const FPcgAiNodeGraphTopology UnresolvedEndpoint(
		TEXT("pcg_graph"), TEXT("pcg:unresolved"), TEXT("pcg:root"),
		PcgTopology.GetItems(), PcgTopology.GetPins(),
		{FPcgAiNodeGraphRelationIdentity(
			TEXT("edge:missing"), TEXT("graph_link"), FString(),
			TEXT("pin:missing"), TEXT("pin:grid:input:In"), INDEX_NONE)});
	TestFalse(TEXT("unresolved graph endpoint fails closed"),
		TryValidatePcgAiNodeGraphTopology(
			UnresolvedEndpoint, AdapterError));
	TestEqual(TEXT("unresolved endpoint error is stable"),
		AdapterError.GetCode(),
		FString(TEXT("pcg_ai_node_graph_relation_endpoint_unresolved")));

	const FPcgAiNodeGraphTopology InvalidDirection(
		TEXT("pcg_graph"), TEXT("pcg:direction"), TEXT("pcg:root"),
		PcgTopology.GetItems(), PcgTopology.GetPins(),
		{FPcgAiNodeGraphRelationIdentity(
			TEXT("edge:reverse"), TEXT("graph_link"), FString(),
			TEXT("pin:grid:input:In"), TEXT("pin:input:output:In"),
			INDEX_NONE)});
	TestFalse(TEXT("invalid graph direction fails closed"),
		TryValidatePcgAiNodeGraphTopology(
			InvalidDirection, AdapterError));
	TestEqual(TEXT("invalid direction error is stable"),
		AdapterError.GetCode(),
		FString(TEXT("pcg_ai_node_graph_link_direction_invalid")));

	const TArray<FPcgAiNodeGraphItemIdentity> CycleItems = {
		FPcgAiNodeGraphItemIdentity(
			TEXT("state:a"), TEXT("guid:a"), FString(),
			TEXT("/Script/StateTreeEditorModule.StateTreeState"),
			TEXT("state_tree_state")),
		FPcgAiNodeGraphItemIdentity(
			TEXT("state:b"), TEXT("guid:b"), FString(),
			TEXT("/Script/StateTreeEditorModule.StateTreeState"),
			TEXT("state_tree_state")),
	};
	const FPcgAiNodeGraphTopology CycleTopology(
		TEXT("state_tree"), TEXT("state_tree:cycle"),
		TEXT("state_tree:root"), CycleItems, {}, {
			FPcgAiNodeGraphRelationIdentity(
				TEXT("child:a:b"), TEXT("hierarchy_child"), FString(),
				TEXT("state:a"), TEXT("state:b"), 0),
			FPcgAiNodeGraphRelationIdentity(
				TEXT("child:b:a"), TEXT("hierarchy_child"), FString(),
				TEXT("state:b"), TEXT("state:a"), 0),
		});
	TestFalse(TEXT("hierarchy cycle fails closed"),
		TryValidatePcgAiNodeGraphTopology(CycleTopology, AdapterError));
	TestEqual(TEXT("cycle error is stable"), AdapterError.GetCode(),
		FString(TEXT("pcg_ai_node_graph_hierarchy_cycle")));

	const FPcgAiNodeGraphLifecycleEvidence ValidLifecycle(
		TEXT("revision:40"),
		TEXT("fixture:gc-006:apply:41"),
		TEXT("fixture:gc-006:settle:41"),
		TEXT("revision:41"),
		TEXT("fixture:gc-006:independent-readback:41"),
		EPcgAiNodeGraphBarrierKind::Compile,
		true, 0, true, true, true);
	TestTrue(TEXT("compile/settle and independent capture validate"),
		TryValidatePcgAiNodeGraphLifecycleEvidence(
			ValidLifecycle, AdapterError));

	const FPcgAiNodeGraphLifecycleEvidence CompileIncomplete(
		TEXT("revision:40"),
		TEXT("fixture:gc-006:apply:41"),
		TEXT("fixture:gc-006:settle:41"),
		TEXT("revision:41"),
		TEXT("fixture:gc-006:independent-readback:41"),
		EPcgAiNodeGraphBarrierKind::Compile,
		false, 0, true, true, true);
	TestFalse(TEXT("required compile cannot remain incomplete"),
		TryValidatePcgAiNodeGraphLifecycleEvidence(
			CompileIncomplete, AdapterError));
	TestEqual(TEXT("compile incomplete error is stable"),
		AdapterError.GetCode(),
		FString(TEXT("pcg_ai_node_graph_compile_incomplete")));

	const FPcgAiNodeGraphLifecycleEvidence RefreshIncomplete(
		TEXT("revision:40"), TEXT("fixture:apply:41"),
		TEXT("fixture:settle:41"), TEXT("revision:41"),
		TEXT("fixture:readback:41"),
		EPcgAiNodeGraphBarrierKind::Refresh,
		false, 0, true, true, true);
	TestFalse(TEXT("required refresh cannot remain incomplete"),
		TryValidatePcgAiNodeGraphLifecycleEvidence(
			RefreshIncomplete, AdapterError));
	TestEqual(TEXT("refresh incomplete error is stable"),
		AdapterError.GetCode(),
		FString(TEXT("pcg_ai_node_graph_refresh_incomplete")));

	const FPcgAiNodeGraphLifecycleEvidence ValidateIncomplete(
		TEXT("revision:40"), TEXT("fixture:apply:41"),
		TEXT("fixture:settle:41"), TEXT("revision:41"),
		TEXT("fixture:readback:41"),
		EPcgAiNodeGraphBarrierKind::Validate,
		false, 0, true, true, true);
	TestFalse(TEXT("required validation cannot remain incomplete"),
		TryValidatePcgAiNodeGraphLifecycleEvidence(
			ValidateIncomplete, AdapterError));
	TestEqual(TEXT("validation incomplete error is stable"),
		AdapterError.GetCode(),
		FString(TEXT("pcg_ai_node_graph_validate_incomplete")));

	const FPcgAiNodeGraphLifecycleEvidence SettleIncomplete(
		TEXT("revision:40"), TEXT("fixture:apply:41"),
		TEXT("fixture:settle:41"), TEXT("revision:41"),
		TEXT("fixture:readback:41"),
		EPcgAiNodeGraphBarrierKind::Compile,
		true, 0, false, true, true);
	TestFalse(TEXT("settle cannot remain incomplete"),
		TryValidatePcgAiNodeGraphLifecycleEvidence(
			SettleIncomplete, AdapterError));
	TestEqual(TEXT("settle incomplete error is stable"),
		AdapterError.GetCode(),
		FString(TEXT("pcg_ai_node_graph_settle_incomplete")));

	const FPcgAiNodeGraphLifecycleEvidence CaptureFailed(
		TEXT("revision:40"), TEXT("fixture:apply:41"),
		TEXT("fixture:settle:41"), TEXT("revision:41"), FString(),
		EPcgAiNodeGraphBarrierKind::Compile,
		true, 0, true, false, true);
	TestFalse(TEXT("capture failure cannot become empty success"),
		TryValidatePcgAiNodeGraphLifecycleEvidence(
			CaptureFailed, AdapterError));
	TestEqual(TEXT("capture failure error is stable"),
		AdapterError.GetCode(),
		FString(TEXT("pcg_ai_node_graph_capture_failed")));

	const FPcgAiNodeGraphLifecycleEvidence EchoedReadback(
		TEXT("revision:40"),
		TEXT("fixture:gc-006:apply:41"),
		TEXT("fixture:gc-006:settle:41"),
		TEXT("revision:41"),
		TEXT("fixture:gc-006:apply:41"),
		EPcgAiNodeGraphBarrierKind::Compile,
		true, 0, true, true, false);
	TestFalse(TEXT("apply receipt cannot serve as independent readback"),
		TryValidatePcgAiNodeGraphLifecycleEvidence(
			EchoedReadback, AdapterError));
	TestEqual(TEXT("non-independent readback error is stable"),
		AdapterError.GetCode(),
		FString(TEXT("pcg_ai_node_graph_readback_not_independent")));

	return true;
}

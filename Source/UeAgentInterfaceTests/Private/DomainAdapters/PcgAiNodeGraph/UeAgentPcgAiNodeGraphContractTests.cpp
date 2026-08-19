#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#include "DomainAdapters/PcgAiNodeGraph/UeAgentPcgAiNodeGraphContract.h"

#include "DomainAdapters/PcgAiNodeGraph/UeAgentPcgAiNodeGraphAdapter.h"

namespace
{
	FString FixturePath()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(
			FPaths::ProjectPluginsDir(),
			TEXT(
				"UeAgentInterface/Source/UeAgentInterfaceTests/Private/"
				"DomainAdapters/PcgAiNodeGraph/Fixtures/"
				"pcg_ai_node_graph_contract_cases.json")));
	}

	TSharedPtr<FJsonObject> LoadFixture(FAutomationTestBase* Test)
	{
		FString Text;
		if (!Test->TestTrue(
				TEXT("load PcgAiNodeGraph contract fixture"),
				FFileHelper::LoadFileToString(Text, *FixturePath())))
		{
			return nullptr;
		}
		TSharedPtr<FJsonObject> Fixture;
		const TSharedRef<TJsonReader<>> Reader =
			TJsonReaderFactory<>::Create(Text);
		if (!Test->TestTrue(
				TEXT("parse PcgAiNodeGraph contract fixture"),
				FJsonSerializer::Deserialize(Reader, Fixture)
					&& Fixture.IsValid()))
		{
			return nullptr;
		}
		return Fixture;
	}

	TArray<FString> ReadStrings(
		const TSharedPtr<FJsonObject>& Object,
		const TCHAR* Field)
	{
		TArray<FString> Result;
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Object.IsValid()
			|| !Object->TryGetArrayField(Field, Values)
			|| Values == nullptr)
		{
			return Result;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			Result.Add(Value->AsString());
		}
		return Result;
	}

	bool HasExactStrings(TArray<FString> Left, TArray<FString> Right)
	{
		Left.Sort();
		Right.Sort();
		return Left == Right;
	}

	bool HasExactSet(
		const TSet<FString>& Actual,
		const TSet<FString>& Expected)
	{
		if (Actual.Num() != Expected.Num())
		{
			return false;
		}
		for (const FString& Value : Expected)
		{
			if (!Actual.Contains(Value))
			{
				return false;
			}
		}
		return true;
	}

	TSharedPtr<FJsonObject> FindByStringField(
		const TArray<TSharedPtr<FJsonValue>>& Values,
		const TCHAR* Field,
		const FString& Expected)
	{
		for (const TSharedPtr<FJsonValue>& Value : Values)
		{
			const TSharedPtr<FJsonObject> Object = Value->AsObject();
			if (Object.IsValid()
				&& Object->GetStringField(Field) == Expected)
			{
				return Object;
			}
		}
		return nullptr;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentPcgAiNodeGraphContractTest,
	"UeAgentInterface.DomainAdapters.PcgAiNodeGraph.E0.Contract."
		"IdentityTypeLinkCompileReadbackAndLegacyBoundary",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentPcgAiNodeGraphContractTest::RunTest(
	const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Fixture = LoadFixture(this);
	if (!Fixture.IsValid())
	{
		return false;
	}

	TestEqual(TEXT("fixture schema"),
		Fixture->GetStringField(TEXT("schema")),
		FString(TEXT("uai.domain_adapter.pcg_ai_node_graph.test_fixture.v1")));
	TestEqual(TEXT("all domains consume shared graph compound value"),
		Fixture->GetStringField(TEXT("canonical_value_schema")),
		FString(TEXT("uai.graph_compound.canonical.v1")));
	TestEqual(TEXT("Adapter owner is exact"),
		Fixture->GetStringField(TEXT("adapter_id")),
		FString(TEXT("D/PcgAiNodeGraph")));

	const TArray<TSharedPtr<FJsonValue>>& Families =
		Fixture->GetArrayField(TEXT("port_families"));
	const TArray<TSharedPtr<FJsonValue>>& Capabilities =
		Fixture->GetArrayField(TEXT("capabilities"));
	const TArray<TSharedPtr<FJsonValue>>& Mappings =
		Fixture->GetArrayField(TEXT("legacy_mappings"));
	TestEqual(TEXT("ten native port families"), Families.Num(),
		static_cast<int32>(Fixture->GetNumberField(
			TEXT("port_family_count"))));
	TestEqual(TEXT("seventeen exact capabilities"), Capabilities.Num(),
		static_cast<int32>(Fixture->GetNumberField(
			TEXT("capability_count"))));
	TestEqual(TEXT("all seventeen legacy inputs are adjudicated"),
		Mappings.Num(),
		static_cast<int32>(Fixture->GetNumberField(
			TEXT("legacy_candidate_count"))));

	const TSet<FString> ExpectedFamilies = {
		TEXT("asset_lifecycle"),
		TEXT("behavior_tree_graph_mutation"),
		TEXT("blackboard_schema_mutation"),
		TEXT("eqs_graph_mutation"),
		TEXT("smart_object_definition_mutation"),
		TEXT("state_tree_hierarchy_mutation"),
		TEXT("pcg_graph_topology_mutation"),
		TEXT("pcg_parameter_binding_mutation"),
		TEXT("pcg_component_binding_mutation"),
		TEXT("pcg_data_asset_mutation"),
	};
	TSet<FString> FamilyIds;
	for (const TSharedPtr<FJsonValue>& Value : Families)
	{
		const TSharedPtr<FJsonObject> Family = Value->AsObject();
		const FString FamilyId = Family->GetStringField(TEXT("family_id"));
		TestFalse(TEXT("family identity is unique: ") + FamilyId,
			FamilyIds.Contains(FamilyId));
		FamilyIds.Add(FamilyId);
		TestFalse(TEXT("family identity facets are explicit: ") + FamilyId,
			Family->GetArrayField(TEXT("required_identity_facets")).IsEmpty());
		TestFalse(TEXT("family lifecycle stages are explicit: ") + FamilyId,
			Family->GetArrayField(TEXT("required_stages")).IsEmpty());
		const FString Barrier = Family->GetStringField(TEXT("barrier_kind"));
		TestTrue(TEXT("family barrier is exact: ") + FamilyId,
			Barrier == TEXT("compile")
				|| Barrier == TEXT("refresh")
				|| Barrier == TEXT("validate"));
		TestFalse(TEXT("family native oracle is explicit: ") + FamilyId,
			Family->GetStringField(TEXT("oracle_kind")).IsEmpty());
	}
	TestTrue(TEXT("port family set is exact"),
		HasExactSet(FamilyIds, ExpectedFamilies));

	TSet<FString> CapabilityIds;
	for (const TSharedPtr<FJsonValue>& Value : Capabilities)
	{
		const TSharedPtr<FJsonObject> Capability = Value->AsObject();
		const FString CapabilityId =
			Capability->GetStringField(TEXT("capability_id"));
		const FString FamilyId =
			Capability->GetStringField(TEXT("port_family_id"));
		TestFalse(TEXT("capability identity is unique: ") + CapabilityId,
			CapabilityIds.Contains(CapabilityId));
		CapabilityIds.Add(CapabilityId);
		TestTrue(TEXT("capability binds an existing family: ") + CapabilityId,
			FamilyIds.Contains(FamilyId));
		TestEqual(TEXT("capability version: ") + CapabilityId,
			static_cast<int32>(Capability->GetNumberField(TEXT("version"))), 1);
	}
	for (const TSharedPtr<FJsonValue>& Value : Mappings)
	{
		const TSharedPtr<FJsonObject> Mapping = Value->AsObject();
		const FString LegacyId = Mapping->GetStringField(TEXT("legacy_id"));
		const TArray<FString> Targets =
			ReadStrings(Mapping, TEXT("target_capability_ids"));
		TestEqual(TEXT("legacy input has one exact target: ") + LegacyId,
			Targets.Num(), 1);
		if (Targets.Num() == 1)
		{
			TestTrue(TEXT("legacy target exists: ") + LegacyId,
				CapabilityIds.Contains(Targets[0]));
		}
		TestEqual(TEXT("legacy asset is scenario-only: ") + LegacyId,
			Mapping->GetStringField(TEXT("legacy_asset_disposition")),
			FString(TEXT("scenario_only")));
	}

	const TArray<TSharedPtr<FJsonValue>>& IdentityCases =
		Fixture->GetArrayField(TEXT("identity_cases"));
	const TSharedPtr<FJsonObject> PcgCase = FindByStringField(
		IdentityCases, TEXT("case_id"),
		TEXT("pcg_native_topology_and_parameter"));
	const TSharedPtr<FJsonObject> BtCase = FindByStringField(
		IdentityCases, TEXT("case_id"),
		TEXT("behavior_tree_and_blackboard_identity"));
	const TSharedPtr<FJsonObject> StructuredAiCase = FindByStringField(
		IdentityCases, TEXT("case_id"),
		TEXT("state_tree_eqs_smart_object_identity"));
	const TSharedPtr<FJsonObject> AssetCase = FindByStringField(
		IdentityCases, TEXT("case_id"),
		TEXT("asset_lifecycle_identity"));
	const TSharedPtr<FJsonObject> ComponentCase = FindByStringField(
		IdentityCases, TEXT("case_id"),
		TEXT("pcg_component_binding_identity"));
	const TSharedPtr<FJsonObject> DataAssetCase = FindByStringField(
		IdentityCases, TEXT("case_id"),
		TEXT("pcg_data_asset_identity"));
	TestTrue(TEXT("PCG topology identity case exists"), PcgCase.IsValid());
	TestTrue(TEXT("BehaviorTree and Blackboard identity case exists"),
		BtCase.IsValid());
	TestTrue(TEXT("StateTree/EQS/SmartObject identity case exists"),
		StructuredAiCase.IsValid());
	TestTrue(TEXT("asset lifecycle identity case exists"),
		AssetCase.IsValid());
	TestTrue(TEXT("PCG component binding identity case exists"),
		ComponentCase.IsValid());
	TestTrue(TEXT("PCG data asset identity case exists"),
		DataAssetCase.IsValid());

	if (PcgCase.IsValid())
	{
		TSet<FString> ItemKeys;
		for (const TSharedPtr<FJsonValue>& Value :
			PcgCase->GetArrayField(TEXT("items")))
		{
			const TSharedPtr<FJsonObject> Item = Value->AsObject();
			const FString ItemKey = Item->GetStringField(TEXT("item_key"));
			ItemKeys.Add(ItemKey);
			TestFalse(TEXT("PCG item keeps native type: ") + ItemKey,
				Item->GetStringField(TEXT("native_type_identity")).IsEmpty());
			TestFalse(TEXT("PCG item keeps native carrier type: ") + ItemKey,
				Item->GetStringField(
					TEXT("native_carrier_type_identity")).IsEmpty());
			TestTrue(TEXT("PCG item has native identity or built-in role: ")
				+ ItemKey,
				!Item->GetStringField(TEXT("native_identity")).IsEmpty()
					|| !Item->GetStringField(
						TEXT("stable_builtin_role")).IsEmpty());
		}
		TMap<FString, FString> PinDirections;
		for (const TSharedPtr<FJsonValue>& Value :
			PcgCase->GetArrayField(TEXT("pins")))
		{
			const TSharedPtr<FJsonObject> Pin = Value->AsObject();
			const FString PinKey = Pin->GetStringField(TEXT("pin_key"));
			TestTrue(TEXT("PCG pin owner resolves: ") + PinKey,
				ItemKeys.Contains(Pin->GetStringField(TEXT("owner_item_key"))));
			TestFalse(TEXT("PCG pin label is native: ") + PinKey,
				Pin->GetStringField(TEXT("native_label")).IsEmpty());
			TestFalse(TEXT("PCG pin type is native: ") + PinKey,
				Pin->GetStringField(TEXT("native_type_identity")).IsEmpty());
			PinDirections.Add(PinKey,
				Pin->GetStringField(TEXT("direction")));
		}
		for (const TSharedPtr<FJsonValue>& Value :
			PcgCase->GetArrayField(TEXT("relations")))
		{
			const TSharedPtr<FJsonObject> Relation = Value->AsObject();
			const FString From =
				Relation->GetStringField(TEXT("from_identity"));
			const FString To = Relation->GetStringField(TEXT("to_identity"));
			TestEqual(TEXT("graph link source is output"),
				PinDirections.FindRef(From), FString(TEXT("output")));
			TestEqual(TEXT("graph link target is input"),
				PinDirections.FindRef(To), FString(TEXT("input")));
		}
		const TSharedPtr<FJsonObject> Parameter =
			PcgCase->GetObjectField(TEXT("parameter_identity"));
		TestFalse(TEXT("property bag identity keeps GUID"),
			Parameter->GetStringField(TEXT("property_guid")).IsEmpty());
		TestFalse(TEXT("property bag identity keeps name"),
			Parameter->GetStringField(TEXT("property_name")).IsEmpty());
		TestFalse(TEXT("property bag identity keeps type"),
			Parameter->GetStringField(TEXT("property_type")).IsEmpty());
	}

	if (BtCase.IsValid())
	{
		for (const TSharedPtr<FJsonValue>& Value :
			BtCase->GetArrayField(TEXT("behavior_tree_items")))
		{
			const TSharedPtr<FJsonObject> Item = Value->AsObject();
			TestFalse(TEXT("BehaviorTree item keeps graph node GUID"),
				Item->GetStringField(TEXT("native_identity")).IsEmpty());
			TestFalse(TEXT("BehaviorTree item keeps runtime type"),
				Item->GetStringField(TEXT("native_type_identity")).IsEmpty());
			TestFalse(TEXT("BehaviorTree item keeps graph node class"),
				Item->GetStringField(
					TEXT("native_carrier_type_identity")).IsEmpty());
		}
		const TSharedPtr<FJsonObject> Blackboard =
			BtCase->GetObjectField(TEXT("blackboard_entry_identity"));
		TestFalse(TEXT("Blackboard entry owner is exact"),
			Blackboard->GetStringField(TEXT("owner_identity")).IsEmpty());
		TestFalse(TEXT("Blackboard key class is exact"),
			Blackboard->GetStringField(TEXT("key_type_identity")).IsEmpty());
		TestEqual(TEXT("Blackboard local/inherited ownership is explicit"),
			Blackboard->GetStringField(TEXT("entry_ownership")),
			FString(TEXT("local")));
	}

	if (StructuredAiCase.IsValid())
	{
		const TSharedPtr<FJsonObject> State =
			StructuredAiCase->GetObjectField(TEXT("state_identity"));
		const TSharedPtr<FJsonObject> Transition =
			StructuredAiCase->GetObjectField(TEXT("transition_identity"));
		const TSharedPtr<FJsonObject> Eqs =
			StructuredAiCase->GetObjectField(TEXT("eqs_identity"));
		const TSharedPtr<FJsonObject> SmartObject =
			StructuredAiCase->GetObjectField(TEXT("smart_object_identity"));
		TestFalse(TEXT("StateTree state keeps native GUID"),
			State->GetStringField(TEXT("state_guid")).IsEmpty());
		TestFalse(TEXT("StateTree transition keeps native GUID"),
			Transition->GetStringField(TEXT("transition_guid")).IsEmpty());
		TestFalse(TEXT("StateTree transition target is exact"),
			Transition->GetStringField(TEXT("target_state_guid")).IsEmpty());
		TestFalse(TEXT("EQS node keeps graph GUID"),
			Eqs->GetStringField(TEXT("graph_node_guid")).IsEmpty());
		TestFalse(TEXT("EQS generator/test type is exact"),
			Eqs->GetStringField(TEXT("native_type_identity")).IsEmpty());
		TestFalse(TEXT("SmartObject slot keeps GUID"),
			SmartObject->GetStringField(TEXT("slot_guid")).IsEmpty());
		TestFalse(TEXT("SmartObject behavior/data type is exact"),
			SmartObject->GetStringField(TEXT("native_type_identity")).IsEmpty());
		TestFalse(TEXT("StateTree carrier type is exact"),
			State->GetStringField(
				TEXT("native_carrier_type_identity")).IsEmpty());
		TestFalse(TEXT("EQS carrier type is exact"),
			Eqs->GetStringField(
				TEXT("native_carrier_type_identity")).IsEmpty());
		TestEqual(TEXT("EQS test order is explicit"),
			static_cast<int32>(Eqs->GetNumberField(TEXT("order_index"))), 0);
		TestFalse(TEXT("SmartObject slot carrier type is exact"),
			SmartObject->GetStringField(
				TEXT("native_carrier_type_identity")).IsEmpty());
	}

	const TSharedPtr<FJsonObject> DeepHierarchy =
		Fixture->GetObjectField(TEXT("deep_hierarchy_case"));
	TestEqual(TEXT("deep hierarchy fixture depth"),
		static_cast<int32>(DeepHierarchy->GetNumberField(TEXT("depth"))), 512);
	TestEqual(TEXT("deep acyclic hierarchy must succeed"),
		DeepHierarchy->GetStringField(TEXT("expected_result")),
		FString(TEXT("success")));
	TestEqual(TEXT("hierarchy has no fixed depth ceiling"),
		DeepHierarchy->GetStringField(TEXT("nesting_policy")),
		FString(TEXT("no_fixed_depth_limit_shared_resource_budget")));

	const TSet<FString> RequiredNegativeCodes = {
		TEXT("pcg_ai_node_graph_item_identity_duplicate"),
		TEXT("pcg_ai_node_graph_type_identity_missing"),
		TEXT("pcg_ai_node_graph_relation_endpoint_unresolved"),
		TEXT("pcg_ai_node_graph_link_direction_invalid"),
		TEXT("pcg_ai_node_graph_hierarchy_cycle"),
		TEXT("pcg_ai_node_graph_compile_incomplete"),
		TEXT("pcg_ai_node_graph_refresh_incomplete"),
		TEXT("pcg_ai_node_graph_validate_incomplete"),
		TEXT("pcg_ai_node_graph_settle_incomplete"),
		TEXT("pcg_ai_node_graph_capture_failed"),
		TEXT("pcg_ai_node_graph_readback_not_independent"),
	};
	TSet<FString> NegativeCodes;
	for (const TSharedPtr<FJsonValue>& Value :
		Fixture->GetArrayField(TEXT("negative_cases")))
	{
		const TSharedPtr<FJsonObject> Negative = Value->AsObject();
		NegativeCodes.Add(Negative->GetStringField(TEXT("error_code")));
		TestTrue(TEXT("negative case must fail"),
			Negative->GetBoolField(TEXT("must_return_false")));
		TestFalse(TEXT("negative case cannot become empty success"),
			Negative->GetBoolField(TEXT("allow_empty_success")));
	}
	TestTrue(TEXT("negative failure semantics are exact"),
		HasExactSet(NegativeCodes, RequiredNegativeCodes));

	const TSharedPtr<FJsonObject> Lifecycle =
		Fixture->GetObjectField(TEXT("lifecycle_evidence"));
	const TArray<FString> EvidenceFields =
		ReadStrings(Lifecycle, TEXT("required_distinct_evidence"));
	TSet<FString> EvidenceValues;
	for (const FString& Field : EvidenceFields)
	{
		EvidenceValues.Add(Lifecycle->GetStringField(Field));
	}
	TestEqual(TEXT("apply, settle and readback evidence are distinct"),
		EvidenceValues.Num(), EvidenceFields.Num());
	TestNotEqual(TEXT("independent readback advances revision"),
		Lifecycle->GetStringField(TEXT("before_revision")),
		Lifecycle->GetStringField(TEXT("readback_revision")));

	const TSet<FString> RequiredForbiddenOwners = {
		TEXT("public_command_dispatch"),
		TEXT("prefix_dispatch"),
		TEXT("json_or_hjson_parser"),
		TEXT("hjson_workspace_or_layout"),
		TEXT("domain_descriptor_registry"),
		TEXT("domain_state_owner"),
		TEXT("resource_budget_policy"),
		TEXT("transaction_or_save"),
		TEXT("verification_or_outcome"),
		TEXT("response_projection"),
		TEXT("world_actor_lifecycle_or_selection"),
	};
	TestTrue(TEXT("Adapter forbidden owner set is exact"),
		HasExactStrings(
			ReadStrings(Fixture, TEXT("forbidden_owners")),
			RequiredForbiddenOwners.Array()));
	const TArray<FString> ForbiddenOracles =
		ReadStrings(Fixture, TEXT("forbidden_oracles"));
	TestTrue(TEXT("legacy ApplyProvider is never an oracle"),
		ForbiddenOracles.Contains(
			TEXT("legacy_folder_or_json_apply_provider")));
	TestTrue(TEXT("display name or array index is never native identity"),
		ForbiddenOracles.Contains(
			TEXT("synthetic_display_name_or_array_index_identity")));
	TestTrue(TEXT("compile receipt is never a readback oracle"),
		ForbiddenOracles.Contains(TEXT("mutation_or_compile_receipt")));

	using namespace UE::UAI::DomainAdapters::PcgAiNodeGraph;
	TestEqual(TEXT("production contract version"),
		UAI_PCG_AI_NODE_GRAPH_CONTRACT_V1, 1);
	const auto& ProductionFamilies = GetPcgAiNodeGraphPortFamilyContracts();
	const auto& ProductionCapabilities =
		GetPcgAiNodeGraphCapabilityContracts();
	TestEqual(TEXT("production family count"),
		ProductionFamilies.Num(), Families.Num());
	TestEqual(TEXT("production capability count"),
		ProductionCapabilities.Num(), Capabilities.Num());
	for (const auto& Capability : ProductionCapabilities)
	{
		const TSharedPtr<FJsonObject> Expected = FindByStringField(
			Capabilities, TEXT("capability_id"),
			Capability.GetCapabilityId());
		TestTrue(TEXT("production capability exists in fixture: ")
			+ Capability.GetCapabilityId(), Expected.IsValid());
		if (Expected.IsValid())
		{
			TestEqual(TEXT("production capability descriptor: ")
				+ Capability.GetCapabilityId(),
				Capability.GetDescriptorId(),
				Expected->GetStringField(TEXT("descriptor_id")));
			TestEqual(TEXT("production capability family: ")
				+ Capability.GetCapabilityId(),
				Capability.GetPortFamilyId(),
				Expected->GetStringField(TEXT("port_family_id")));
			TestEqual(TEXT("production capability operation kind: ")
				+ Capability.GetCapabilityId(),
				Capability.GetOperationKind(),
				Expected->GetStringField(TEXT("operation_kind")));
			TestEqual(TEXT("production capability effect class: ")
				+ Capability.GetCapabilityId(),
				Capability.GetEffectClass(),
				Expected->GetStringField(TEXT("effect_class")));
			TestEqual(TEXT("production capability version: ")
				+ Capability.GetCapabilityId(),
				Capability.GetVersion(),
				static_cast<int32>(Expected->GetNumberField(TEXT("version"))));
		}
	}
	for (const auto& Family : ProductionFamilies)
	{
		const TSharedPtr<FJsonObject> Expected = FindByStringField(
			Families, TEXT("family_id"), Family.GetFamilyId());
		TestTrue(TEXT("production family exists in fixture: ")
			+ Family.GetFamilyId(), Expected.IsValid());
		if (Expected.IsValid())
		{
			TestTrue(TEXT("production family identity facets: ")
				+ Family.GetFamilyId(),
				HasExactStrings(
					Family.GetRequiredIdentityFacets(),
					ReadStrings(Expected,
						TEXT("required_identity_facets"))));
			TestTrue(TEXT("production family lifecycle stages: ")
				+ Family.GetFamilyId(),
				HasExactStrings(
					Family.GetRequiredStages(),
					ReadStrings(Expected, TEXT("required_stages"))));
			TestEqual(TEXT("production family barrier: ")
				+ Family.GetFamilyId(),
				Family.GetBarrierKind(),
				Expected->GetStringField(TEXT("barrier_kind")));
			TestEqual(TEXT("production family oracle: ")
				+ Family.GetFamilyId(),
				Family.GetOracleKind(),
				Expected->GetStringField(TEXT("oracle_kind")));
		}
	}

	using namespace UE::UAI::DomainAdapters::PcgAiNodeGraph;
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

	TArray<FPcgAiNodeGraphItemIdentity> DeepItems;
	TArray<FPcgAiNodeGraphRelationIdentity> DeepRelations;
	for (int32 Index = 0; Index < 512; ++Index)
	{
		const FString ItemKey = FString::Printf(TEXT("state:%d"), Index);
		DeepItems.Emplace(
			ItemKey,
			FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower),
			FString(),
			TEXT("/Script/StateTreeEditorModule.StateTreeState"),
			TEXT("state_tree_state"));
		if (Index > 0)
		{
			DeepRelations.Emplace(
				FString::Printf(TEXT("child:%d"), Index),
				TEXT("hierarchy_child"),
				FString(),
				FString::Printf(TEXT("state:%d"), Index - 1),
				ItemKey,
				Index - 1);
		}
	}
	const FPcgAiNodeGraphTopology DeepHierarchyTopology(
		TEXT("state_tree"), TEXT("/Game/UAI/GC006/ST_Deep"),
		TEXT("state_tree:root"),
		MoveTemp(DeepItems), {}, MoveTemp(DeepRelations));
	TestTrue(TEXT("512-level acyclic hierarchy has no fixed depth cap"),
		TryValidatePcgAiNodeGraphTopology(
			DeepHierarchyTopology, AdapterError));

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

#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#include "DomainAdapters/AnimationRigGraph/UeAgentAnimationRigGraphContract.h"

#include "DomainAdapters/AnimationRigGraph/UeAgentAnimationRigGraphAdapter.h"

static_assert(UAI_ANIMATION_RIG_GRAPH_CONTRACT_V1 == 1,
	"AnimationRigGraph production contract version must be explicit");
static_assert(UAI_ANIMATION_RIG_GRAPH_ADAPTER_V1 == 1,
	"AnimationRigGraph production adapter version must be explicit");

namespace
{
	FString FixturePath()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(
			FPaths::ProjectPluginsDir(),
			TEXT("UeAgentInterface/Source/UeAgentInterfaceTests/Private/")
			TEXT("DomainAdapters/AnimationRigGraph/Fixtures/")
			TEXT("animation_rig_graph_contract_cases.json")));
	}

	TSharedPtr<FJsonObject> LoadFixture(FAutomationTestBase* Test)
	{
		FString Text;
		if (!Test->TestTrue(
				TEXT("load AnimationRigGraph contract fixture"),
				FFileHelper::LoadFileToString(Text, *FixturePath())))
		{
			return nullptr;
		}
		TSharedPtr<FJsonObject> Fixture;
		const TSharedRef<TJsonReader<>> Reader =
			TJsonReaderFactory<>::Create(Text);
		if (!Test->TestTrue(
				TEXT("parse AnimationRigGraph contract fixture"),
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
	FUeAgentAnimationRigGraphContractTest,
	"UeAgentInterface.DomainAdapters.AnimationRigGraph.E0.Contract."
		"IdentityHierarchyPinCompileReadbackAndLegacyBoundary",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentAnimationRigGraphContractTest::RunTest(
	const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Fixture = LoadFixture(this);
	if (!Fixture.IsValid())
	{
		return false;
	}

	TestEqual(TEXT("fixture schema"),
		Fixture->GetStringField(TEXT("schema")),
		FString(TEXT(
			"uai.domain_adapter.animation_rig_graph.test_fixture.v1")));
	TestEqual(TEXT("shared graph compound canonical value"),
		Fixture->GetStringField(TEXT("canonical_value_schema")),
		FString(TEXT("uai.graph_compound.canonical.v1")));
	TestEqual(TEXT("Adapter owner is exact"),
		Fixture->GetStringField(TEXT("adapter_id")),
		FString(TEXT("D/AnimationRigGraph")));

	const TArray<TSharedPtr<FJsonValue>>& Families =
		Fixture->GetArrayField(TEXT("port_families"));
	const TArray<TSharedPtr<FJsonValue>>& Capabilities =
		Fixture->GetArrayField(TEXT("capabilities"));
	const TArray<TSharedPtr<FJsonValue>>& Mappings =
		Fixture->GetArrayField(TEXT("legacy_mappings"));
	TestEqual(TEXT("twelve native port families"), Families.Num(),
		static_cast<int32>(Fixture->GetNumberField(
			TEXT("port_family_count"))));
	TestEqual(TEXT("twenty-six exact capabilities"), Capabilities.Num(),
		static_cast<int32>(Fixture->GetNumberField(
			TEXT("capability_count"))));
	TestEqual(TEXT("all twenty-six legacy inputs adjudicated"),
		Mappings.Num(),
		static_cast<int32>(Fixture->GetNumberField(
			TEXT("legacy_candidate_count"))));

	const TSet<FString> ExpectedFamilies = {
		TEXT("asset_lifecycle"),
		TEXT("anim_blueprint_graph_mutation"),
		TEXT("blend_space_definition_mutation"),
		TEXT("skeleton_definition_mutation"),
		TEXT("control_rig_hierarchy_graph_mutation"),
		TEXT("control_rig_shape_library_mutation"),
		TEXT("deformer_graph_topology_mutation"),
		TEXT("deformer_source_binding_mutation"),
		TEXT("mesh_deformer_collection_mutation"),
		TEXT("ml_deformer_configuration_mutation"),
		TEXT("ik_rig_definition_mutation"),
		TEXT("ik_retargeter_definition_mutation"),
	};
	TSet<FString> FamilyIds;
	for (const TSharedPtr<FJsonValue>& Value : Families)
	{
		const TSharedPtr<FJsonObject> Family = Value->AsObject();
		const FString FamilyId =
			Family->GetStringField(TEXT("family_id"));
		TestFalse(TEXT("family identity unique: ") + FamilyId,
			FamilyIds.Contains(FamilyId));
		FamilyIds.Add(FamilyId);
		TestFalse(TEXT("family identity facets explicit: ") + FamilyId,
			Family->GetArrayField(
				TEXT("required_identity_facets")).IsEmpty());
		TestFalse(TEXT("family lifecycle stages explicit: ") + FamilyId,
			Family->GetArrayField(TEXT("required_stages")).IsEmpty());
		const FString Barrier =
			Family->GetStringField(TEXT("barrier_kind"));
		TestTrue(TEXT("family barrier exact: ") + FamilyId,
			Barrier == TEXT("compile")
				|| Barrier == TEXT("refresh")
				|| Barrier == TEXT("validate"));
		TestFalse(TEXT("native oracle explicit: ") + FamilyId,
			Family->GetStringField(TEXT("oracle_kind")).IsEmpty());
		const FString Availability =
			Family->GetStringField(TEXT("availability_policy"));
		TestTrue(TEXT("availability policy exact: ") + FamilyId,
			Availability == TEXT("required")
				|| Availability
					== TEXT("conditional_concrete_native_class"));
	}
	TestTrue(TEXT("port family set exact"),
		HasExactSet(FamilyIds, ExpectedFamilies));

	TSet<FString> CapabilityIds;
	for (const TSharedPtr<FJsonValue>& Value : Capabilities)
	{
		const TSharedPtr<FJsonObject> Capability = Value->AsObject();
		const FString CapabilityId =
			Capability->GetStringField(TEXT("capability_id"));
		const FString DescriptorId =
			Capability->GetStringField(TEXT("descriptor_id"));
		const FString FamilyId =
			Capability->GetStringField(TEXT("port_family_id"));
		TestFalse(TEXT("capability identity unique: ") + CapabilityId,
			CapabilityIds.Contains(CapabilityId));
		CapabilityIds.Add(CapabilityId);
		TestTrue(TEXT("capability family exists: ") + CapabilityId,
			FamilyIds.Contains(FamilyId));
		TestEqual(TEXT("descriptor identity exact: ") + CapabilityId,
			DescriptorId,
			FString(TEXT("uai.operation.")) + CapabilityId);
		TestEqual(TEXT("operation kind exact: ") + CapabilityId,
			Capability->GetStringField(TEXT("operation_kind")),
			FString(TEXT("operation")));
		TestEqual(TEXT("effect class exact: ") + CapabilityId,
			Capability->GetStringField(TEXT("effect_class")),
			FString(TEXT("write")));
		TestEqual(TEXT("capability version: ") + CapabilityId,
			static_cast<int32>(Capability->GetNumberField(TEXT("version"))),
			1);
	}
	for (const TSharedPtr<FJsonValue>& Value : Mappings)
	{
		const TSharedPtr<FJsonObject> Mapping = Value->AsObject();
		const FString LegacyId =
			Mapping->GetStringField(TEXT("legacy_id"));
		const TArray<FString> Targets =
			ReadStrings(Mapping, TEXT("target_capability_ids"));
		TestEqual(TEXT("legacy input has one target: ") + LegacyId,
			Targets.Num(), 1);
		if (Targets.Num() == 1)
		{
			TestTrue(TEXT("legacy target exists: ") + LegacyId,
				CapabilityIds.Contains(Targets[0]));
		}
		TestEqual(TEXT("legacy input is scenario-only: ") + LegacyId,
			Mapping->GetStringField(TEXT("legacy_asset_disposition")),
			FString(TEXT("scenario_only")));
	}

	const TSet<FString> AllowedIdentitySources = {
		TEXT("native_guid"),
		TEXT("native_object_path"),
		TEXT("native_key"),
		TEXT("native_composite"),
		TEXT("stable_builtin_role"),
	};
	const TArray<TSharedPtr<FJsonValue>>& IdentityCases =
		Fixture->GetArrayField(TEXT("identity_cases"));
	const TSet<FString> ExpectedIdentityCases = {
		TEXT("anim_blueprint_and_control_rig_graph"),
		TEXT("blend_space_and_skeleton_definition"),
		TEXT("deformer_graph_collection_and_typed_configuration"),
		TEXT("ik_rig_and_retargeter_definition"),
	};
	TSet<FString> IdentityCaseIds;
	for (const TSharedPtr<FJsonValue>& CaseValue : IdentityCases)
	{
		const TSharedPtr<FJsonObject> Case = CaseValue->AsObject();
		const FString CaseId = Case->GetStringField(TEXT("case_id"));
		IdentityCaseIds.Add(CaseId);
		TSet<FString> Endpoints;
		for (const TSharedPtr<FJsonValue>& ItemValue :
			Case->GetArrayField(TEXT("items")))
		{
			const TSharedPtr<FJsonObject> Item = ItemValue->AsObject();
			const FString ItemKey =
				Item->GetStringField(TEXT("item_key"));
			const FString IdentitySource =
				Item->GetStringField(TEXT("identity_source"));
			TestFalse(TEXT("item identity unique: ") + ItemKey,
				Endpoints.Contains(ItemKey));
			Endpoints.Add(ItemKey);
			TestTrue(TEXT("item identity source native: ") + ItemKey,
				AllowedIdentitySources.Contains(IdentitySource));
			const FString NativeIdentity =
				Item->GetStringField(TEXT("native_identity"));
			const FString BuiltinRole =
				Item->GetStringField(TEXT("stable_builtin_role"));
			TestTrue(TEXT("item has native identity or built-in role: ")
					+ ItemKey,
				!NativeIdentity.IsEmpty() || !BuiltinRole.IsEmpty());
			TestFalse(TEXT("carrier type explicit: ") + ItemKey,
				Item->GetStringField(
					TEXT("native_carrier_type_identity")).IsEmpty());
			TestFalse(TEXT("payload type explicit: ") + ItemKey,
				Item->GetStringField(
					TEXT("native_type_identity")).IsEmpty());
			TestFalse(TEXT("semantic role explicit: ") + ItemKey,
				Item->GetStringField(TEXT("semantic_role")).IsEmpty());
		}
		for (const TSharedPtr<FJsonValue>& PinValue :
			Case->GetArrayField(TEXT("pins")))
		{
			const TSharedPtr<FJsonObject> Pin = PinValue->AsObject();
			const FString PinKey = Pin->GetStringField(TEXT("pin_key"));
			const FString Owner =
				Pin->GetStringField(TEXT("owner_item_key"));
			TestTrue(TEXT("pin owner resolves: ") + PinKey,
				Endpoints.Contains(Owner));
			TestFalse(TEXT("pin identity unique: ") + PinKey,
				Endpoints.Contains(PinKey));
			Endpoints.Add(PinKey);
			const FString Direction =
				Pin->GetStringField(TEXT("direction"));
			TestTrue(TEXT("pin direction exact: ") + PinKey,
				Direction == TEXT("input") || Direction == TEXT("output"));
			TestFalse(TEXT("pin native type explicit: ") + PinKey,
				Pin->GetStringField(
					TEXT("native_type_identity")).IsEmpty());
		}
		TSet<FString> OrderedSlots;
		for (const TSharedPtr<FJsonValue>& RelationValue :
			Case->GetArrayField(TEXT("relations")))
		{
			const TSharedPtr<FJsonObject> Relation =
				RelationValue->AsObject();
			const FString RelationKey =
				Relation->GetStringField(TEXT("relation_key"));
			const FString From =
				Relation->GetStringField(TEXT("from_identity"));
			const FString To =
				Relation->GetStringField(TEXT("to_identity"));
			TestTrue(TEXT("relation source resolves: ") + RelationKey,
				Endpoints.Contains(From));
			TestTrue(TEXT("relation target resolves: ") + RelationKey,
				Endpoints.Contains(To));
			TestFalse(TEXT("relation kind explicit: ") + RelationKey,
				Relation->GetStringField(TEXT("relation_kind")).IsEmpty());
			const int32 Order = static_cast<int32>(
				Relation->GetNumberField(TEXT("order_index")));
			if (Order >= 0)
			{
				const FString Slot = FString::Printf(TEXT("%s|%s|%d"),
					*From,
					*Relation->GetStringField(TEXT("relation_kind")),
					Order);
				TestFalse(TEXT("relation order unique: ") + RelationKey,
					OrderedSlots.Contains(Slot));
				OrderedSlots.Add(Slot);
			}
		}
	}
	TestTrue(TEXT("identity case set exact"),
		HasExactSet(IdentityCaseIds, ExpectedIdentityCases));

	const TSharedPtr<FJsonObject> DeepCase =
		Fixture->GetObjectField(TEXT("deep_hierarchy_case"));
	TestEqual(TEXT("deep hierarchy exercises 512 levels"),
		static_cast<int32>(DeepCase->GetNumberField(TEXT("depth"))), 512);
	TestEqual(TEXT("deep hierarchy must succeed"),
		DeepCase->GetStringField(TEXT("expected_result")),
		FString(TEXT("success")));
	TestEqual(TEXT("no fixed depth cap"),
		DeepCase->GetStringField(TEXT("nesting_policy")),
		FString(TEXT("no_fixed_depth_limit_shared_resource_budget")));

	const TSet<FString> ExpectedNegativeCodes = {
		TEXT("animation_rig_graph_item_identity_duplicate"),
		TEXT("animation_rig_graph_identity_source_missing"),
		TEXT("animation_rig_graph_type_identity_missing"),
		TEXT("animation_rig_graph_relation_endpoint_unresolved"),
		TEXT("animation_rig_graph_pin_direction_invalid"),
		TEXT("animation_rig_graph_relation_order_duplicate"),
		TEXT("animation_rig_graph_hierarchy_cycle"),
		TEXT("animation_rig_graph_compile_incomplete"),
		TEXT("animation_rig_graph_refresh_incomplete"),
		TEXT("animation_rig_graph_validate_incomplete"),
		TEXT("animation_rig_graph_settle_incomplete"),
		TEXT("animation_rig_graph_capture_failed"),
		TEXT("animation_rig_graph_readback_not_independent"),
		TEXT("animation_rig_graph_concrete_type_required"),
		TEXT("animation_rig_graph_external_value_evidence_missing"),
	};
	TSet<FString> NegativeCodes;
	for (const TSharedPtr<FJsonValue>& Value :
		Fixture->GetArrayField(TEXT("negative_cases")))
	{
		const TSharedPtr<FJsonObject> Negative = Value->AsObject();
		NegativeCodes.Add(Negative->GetStringField(TEXT("error_code")));
		TestTrue(TEXT("negative must return false"),
			Negative->GetBoolField(TEXT("must_return_false")));
		TestFalse(TEXT("negative cannot publish empty success"),
			Negative->GetBoolField(TEXT("allow_empty_success")));
	}
	TestTrue(TEXT("negative code set exact"),
		HasExactSet(NegativeCodes, ExpectedNegativeCodes));

	const TSharedPtr<FJsonObject> Lifecycle =
		Fixture->GetObjectField(TEXT("lifecycle_evidence"));
	TSet<FString> EvidenceValues;
	for (const FString& Field :
		ReadStrings(Lifecycle, TEXT("required_distinct_evidence")))
	{
		const FString Value = Lifecycle->GetStringField(Field);
		TestFalse(TEXT("lifecycle evidence non-empty: ") + Field,
			Value.IsEmpty());
		TestFalse(TEXT("lifecycle evidence distinct: ") + Field,
			EvidenceValues.Contains(Value));
		EvidenceValues.Add(Value);
	}
	TestNotEqual(TEXT("readback revision advances"),
		Lifecycle->GetStringField(TEXT("before_revision")),
		Lifecycle->GetStringField(TEXT("readback_revision")));

	const TSet<FString> ForbiddenOwners(
		ReadStrings(Fixture, TEXT("forbidden_owners")));
	TestTrue(TEXT("Adapter cannot own spatial conversion"),
		ForbiddenOwners.Contains(TEXT("space_or_time_conversion")));
	TestTrue(TEXT("Adapter cannot own training/session"),
		ForbiddenOwners.Contains(TEXT("training_or_session_owner")));
	TestTrue(TEXT("Adapter cannot own HJSON"),
		ForbiddenOwners.Contains(TEXT("json_or_hjson_parser")));
	const TSet<FString> ForbiddenOracles(
		ReadStrings(Fixture, TEXT("forbidden_oracles")));
	TestTrue(TEXT("opaque reflection is forbidden oracle"),
		ForbiddenOracles.Contains(TEXT(
			"arbitrary_reflection_property_bag")));
	TestTrue(TEXT("legacy provider is forbidden oracle"),
		ForbiddenOracles.Contains(TEXT(
			"legacy_folder_or_json_apply_provider")));

	using namespace UE::UAI::DomainAdapters::AnimationRigGraph;
	const TArray<FAnimationRigGraphPortFamilyContract>&
		ProductionFamilies = GetAnimationRigGraphPortFamilyContracts();
	const TArray<FAnimationRigGraphCapabilityContract>&
		ProductionCapabilities = GetAnimationRigGraphCapabilityContracts();
	TestEqual(TEXT("production family count"), ProductionFamilies.Num(),
		Families.Num());
	TestEqual(TEXT("production capability count"),
		ProductionCapabilities.Num(), Capabilities.Num());
	for (const TSharedPtr<FJsonValue>& Value : Families)
	{
		const TSharedPtr<FJsonObject> Expected = Value->AsObject();
		const FString FamilyId =
			Expected->GetStringField(TEXT("family_id"));
		const FAnimationRigGraphPortFamilyContract* Actual =
			ProductionFamilies.FindByPredicate(
				[&](const FAnimationRigGraphPortFamilyContract& Candidate)
				{
					return Candidate.GetFamilyId() == FamilyId;
				});
		if (!TestNotNull(TEXT("production family exists: ") + FamilyId,
			Actual))
		{
			continue;
		}
		TArray<FString> ActualFacets = Actual->GetRequiredIdentityFacets();
		TArray<FString> ExpectedFacets =
			ReadStrings(Expected, TEXT("required_identity_facets"));
		ActualFacets.Sort();
		ExpectedFacets.Sort();
		TestEqual(TEXT("family facets exact: ") + FamilyId,
			ActualFacets, ExpectedFacets);
		TArray<FString> ActualStages = Actual->GetRequiredStages();
		TArray<FString> ExpectedStages =
			ReadStrings(Expected, TEXT("required_stages"));
		ActualStages.Sort();
		ExpectedStages.Sort();
		TestEqual(TEXT("family stages exact: ") + FamilyId,
			ActualStages, ExpectedStages);
		TestEqual(TEXT("family barrier exact: ") + FamilyId,
			Actual->GetBarrierKind(),
			Expected->GetStringField(TEXT("barrier_kind")));
		TestEqual(TEXT("family oracle exact: ") + FamilyId,
			Actual->GetOracleKind(),
			Expected->GetStringField(TEXT("oracle_kind")));
		TestEqual(TEXT("family availability exact: ") + FamilyId,
			Actual->GetAvailabilityPolicy(),
			Expected->GetStringField(TEXT("availability_policy")));
	}
	for (const TSharedPtr<FJsonValue>& Value : Capabilities)
	{
		const TSharedPtr<FJsonObject> Expected = Value->AsObject();
		const FString CapabilityId =
			Expected->GetStringField(TEXT("capability_id"));
		const FAnimationRigGraphCapabilityContract* Actual =
			ProductionCapabilities.FindByPredicate(
				[&](const FAnimationRigGraphCapabilityContract& Candidate)
				{
					return Candidate.GetCapabilityId() == CapabilityId;
				});
		if (!TestNotNull(TEXT("production capability exists: ")
				+ CapabilityId, Actual))
		{
			continue;
		}
		TestEqual(TEXT("capability descriptor exact: ") + CapabilityId,
			Actual->GetDescriptorId(),
			Expected->GetStringField(TEXT("descriptor_id")));
		TestEqual(TEXT("capability family exact: ") + CapabilityId,
			Actual->GetPortFamilyId(),
			Expected->GetStringField(TEXT("port_family_id")));
		TestEqual(TEXT("capability operation exact: ") + CapabilityId,
			Actual->GetOperationKind(),
			Expected->GetStringField(TEXT("operation_kind")));
		TestEqual(TEXT("capability effect exact: ") + CapabilityId,
			Actual->GetEffectClass(),
			Expected->GetStringField(TEXT("effect_class")));
		TestEqual(TEXT("capability version exact: ") + CapabilityId,
			Actual->GetVersion(),
			static_cast<int32>(Expected->GetNumberField(TEXT("version"))));
	}

	using namespace UE::UAI::DomainAdapters::AnimationRigGraph;
	TArray<FAnimationRigGraphItemIdentity> DeepItems;
	TArray<FAnimationRigGraphRelationIdentity> DeepRelations;
	DeepItems.Reserve(512);
	DeepRelations.Reserve(511);
	for (int32 Index = 0; Index < 512; ++Index)
	{
		const FString Key = FString::Printf(TEXT("rig_element:%d"), Index);
		DeepItems.Emplace(Key, TEXT("native_key"),
			FString::Printf(TEXT("Control:GC008_%d"), Index), FString(),
			TEXT("/Script/ControlRig.RigHierarchy"),
			TEXT("/Script/ControlRig.RigControlElement"), TEXT("control"));
		if (Index > 0)
		{
			DeepRelations.Emplace(
				FString::Printf(TEXT("parent:%d"), Index),
				TEXT("hierarchy_child"), FString(),
				FString::Printf(TEXT("rig_element:%d"), Index - 1),
				Key, 0);
		}
	}
	FAnimationRigGraphTopology DeepTopology(
		DeepItems, {}, DeepRelations);
	FAnimationRigGraphAdapterError AdapterError;
	TestTrue(TEXT("512-level hierarchy validates without fixed cap"),
		TryValidateAnimationRigGraphTopology(DeepTopology, AdapterError));
	TArray<FAnimationRigGraphRelationIdentity> CyclicRelations =
		DeepRelations;
	CyclicRelations.Emplace(TEXT("cycle"), TEXT("hierarchy_child"),
		FString(), TEXT("rig_element:511"), TEXT("rig_element:0"), 0);
	FAnimationRigGraphTopology CyclicTopology(
		DeepItems, {}, CyclicRelations);
	TestFalse(TEXT("hierarchy cycle rejected structurally"),
		TryValidateAnimationRigGraphTopology(CyclicTopology, AdapterError));
	TestEqual(TEXT("hierarchy cycle exact error"), AdapterError.GetCode(),
		FString(TEXT("animation_rig_graph_hierarchy_cycle")));

	FAnimationRigGraphAssetIdentity AssetIdentity(
		TEXT("anim_blueprint"),
		TEXT("/Game/UAI/GC008/ABP_GC008.ABP_GC008"),
		TEXT("/Script/Engine.AnimBlueprint"));
	TestTrue(TEXT("typed asset identity validates"),
		TryValidateAnimationRigGraphAssetIdentity(
			AssetIdentity, AdapterError));
	FAnimationRigGraphLifecycleEvidence LifecycleEvidence(
		EAnimationRigGraphBarrierKind::Compile,
		true, 0,
		TEXT("revision:80"), TEXT("fixture:gc-008:apply:81"),
		TEXT("fixture:gc-008:barrier:81"),
		TEXT("fixture:gc-008:settle:81"), true,
		TEXT("revision:81"),
		TEXT("fixture:gc-008:independent-readback:81"));
	TestTrue(TEXT("lifecycle evidence validates"),
		TryValidateAnimationRigGraphLifecycleEvidence(
			LifecycleEvidence, AdapterError));

	return !HasAnyErrors();
}

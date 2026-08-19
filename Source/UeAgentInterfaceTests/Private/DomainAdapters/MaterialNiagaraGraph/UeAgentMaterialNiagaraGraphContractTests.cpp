#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

// Header presence is part of this test contract; keep this source rebuilt when
#include "DomainAdapters/MaterialNiagaraGraph/UeAgentMaterialNiagaraGraphContract.h"

#include "DomainAdapters/MaterialNiagaraGraph/UeAgentMaterialNiagaraGraphAdapter.h"

namespace
{
	FString FixturePath()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(
			FPaths::ProjectPluginsDir(),
			TEXT(
				"UeAgentInterface/Source/UeAgentInterfaceTests/Private/"
				"DomainAdapters/MaterialNiagaraGraph/Fixtures/"
				"material_niagara_graph_contract_cases.json")));
	}

	TSharedPtr<FJsonObject> LoadFixture(FAutomationTestBase* Test)
	{
		FString Text;
		if (!Test->TestTrue(
				TEXT("load MaterialNiagaraGraph contract fixture"),
				FFileHelper::LoadFileToString(Text, *FixturePath())))
		{
			return nullptr;
		}

		TSharedPtr<FJsonObject> Fixture;
		const TSharedRef<TJsonReader<>> Reader =
			TJsonReaderFactory<>::Create(Text);
		if (!Test->TestTrue(
				TEXT("parse MaterialNiagaraGraph contract fixture"),
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

	const TSharedPtr<FJsonObject> FindByStringField(
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
	FUeAgentMaterialNiagaraGraphContractTest,
	"UeAgentInterface.DomainAdapters.MaterialNiagaraGraph.E0.Contract."
		"IdentityStackCompoundCaptureAndLegacyBoundary",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentMaterialNiagaraGraphContractTest::RunTest(
	const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Fixture = LoadFixture(this);
	if (!Fixture.IsValid())
	{
		return false;
	}

	TestEqual(
		TEXT("fixture schema"),
		Fixture->GetStringField(TEXT("schema")),
		FString(TEXT(
			"uai.domain_adapter.material_niagara_graph.test_fixture.v1")));
	TestEqual(
		TEXT("Material and Niagara consume the shared graph value"),
		Fixture->GetStringField(TEXT("canonical_value_schema")),
		FString(TEXT("uai.graph_compound.canonical.v1")));
	TestEqual(
		TEXT("Adapter owner is exact"),
		Fixture->GetStringField(TEXT("adapter_id")),
		FString(TEXT("D/MaterialNiagaraGraph")));

	const TArray<TSharedPtr<FJsonValue>>& Families =
		Fixture->GetArrayField(TEXT("port_families"));
	const TArray<TSharedPtr<FJsonValue>>& Capabilities =
		Fixture->GetArrayField(TEXT("capabilities"));
	const TArray<TSharedPtr<FJsonValue>>& Mappings =
		Fixture->GetArrayField(TEXT("legacy_mappings"));
	TestEqual(
		TEXT("seven native port families"),
		Families.Num(),
		static_cast<int32>(Fixture->GetNumberField(
			TEXT("port_family_count"))));
	TestEqual(
		TEXT("thirteen exact capabilities"),
		Capabilities.Num(),
		static_cast<int32>(Fixture->GetNumberField(
			TEXT("capability_count"))));
	TestEqual(
		TEXT("thirteen retained legacy inputs are adjudicated"),
		Mappings.Num(),
		static_cast<int32>(Fixture->GetNumberField(
			TEXT("legacy_candidate_count"))));

	const TSet<FString> ExpectedFamilies = {
		TEXT("asset_lifecycle"),
		TEXT("material_graph_mutation"),
		TEXT("material_instance_parameter_mutation"),
		TEXT("niagara_stack_mutation"),
		TEXT("niagara_compound_input"),
		TEXT("niagara_renderer_event_parameter_mutation"),
		TEXT("compile_refresh_settle"),
	};
	TSet<FString> FamilyIds;
	for (const TSharedPtr<FJsonValue>& Value : Families)
	{
		const TSharedPtr<FJsonObject> Family = Value->AsObject();
		const FString FamilyId = Family->GetStringField(TEXT("family_id"));
		TestFalse(
			TEXT("family identity is unique: ") + FamilyId,
			FamilyIds.Contains(FamilyId));
		FamilyIds.Add(FamilyId);
		TestFalse(
			TEXT("family has stable identity facets: ") + FamilyId,
			Family->GetArrayField(TEXT("required_identity_facets")).IsEmpty());
		TestFalse(
			TEXT("family has explicit lifecycle stages: ") + FamilyId,
			Family->GetArrayField(TEXT("required_stages")).IsEmpty());
		TestFalse(
			TEXT("family has an independent native oracle: ") + FamilyId,
			Family->GetStringField(TEXT("oracle_kind")).IsEmpty());
	}
	bool bHasExactFamilies = FamilyIds.Num() == ExpectedFamilies.Num();
	for (const FString& FamilyId : ExpectedFamilies)
	{
		bHasExactFamilies = FamilyIds.Contains(FamilyId)
			&& bHasExactFamilies;
	}
	TestTrue(TEXT("port family set is exact"), bHasExactFamilies);

	TSet<FString> CapabilityIds;
	for (const TSharedPtr<FJsonValue>& Value : Capabilities)
	{
		const TSharedPtr<FJsonObject> Capability = Value->AsObject();
		const FString CapabilityId =
			Capability->GetStringField(TEXT("capability_id"));
		const FString FamilyId =
			Capability->GetStringField(TEXT("port_family_id"));
		TestFalse(
			TEXT("capability identity is unique: ") + CapabilityId,
			CapabilityIds.Contains(CapabilityId));
		CapabilityIds.Add(CapabilityId);
		TestTrue(
			TEXT("capability binds an existing family: ") + CapabilityId,
			FamilyIds.Contains(FamilyId));
		TestEqual(
			TEXT("capability version: ") + CapabilityId,
			static_cast<int32>(Capability->GetNumberField(TEXT("version"))),
			1);
	}
	for (const TSharedPtr<FJsonValue>& Value : Mappings)
	{
		const TSharedPtr<FJsonObject> Mapping = Value->AsObject();
		const TArray<FString> Targets =
			ReadStrings(Mapping, TEXT("target_capability_ids"));
		TestFalse(
			TEXT("legacy mapping has a target: ")
				+ Mapping->GetStringField(TEXT("legacy_id")),
			Targets.IsEmpty());
		for (const FString& Target : Targets)
		{
			TestTrue(
				TEXT("legacy mapping resolves exact capability: ")
					+ Mapping->GetStringField(TEXT("legacy_id")),
				CapabilityIds.Contains(Target));
		}
	}

	const TArray<TSharedPtr<FJsonValue>>& IdentityCases =
		Fixture->GetArrayField(TEXT("identity_cases"));
	const TSharedPtr<FJsonObject> MaterialCase = FindByStringField(
		IdentityCases,
		TEXT("case_id"),
		TEXT("material_expression_to_root_property"));
	const TSharedPtr<FJsonObject> ParameterCase = FindByStringField(
		IdentityCases,
		TEXT("case_id"),
		TEXT("material_instance_parameter_info"));
	const TSharedPtr<FJsonObject> NiagaraCase = FindByStringField(
		IdentityCases,
		TEXT("case_id"),
		TEXT("niagara_stage_stack_and_compound"));
	TestTrue(TEXT("Material graph identity case exists"),
		MaterialCase.IsValid());
	TestTrue(TEXT("Material instance parameter identity case exists"),
		ParameterCase.IsValid());
	TestTrue(TEXT("Niagara stack and compound identity case exists"),
		NiagaraCase.IsValid());
	if (MaterialCase.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>& Items =
			MaterialCase->GetArrayField(TEXT("items"));
		const TArray<TSharedPtr<FJsonValue>>& Pins =
			MaterialCase->GetArrayField(TEXT("pins"));
		const TArray<TSharedPtr<FJsonValue>>& Links =
			MaterialCase->GetArrayField(TEXT("links"));
		TestEqual(TEXT("Material fixture has expression and root"),
			Items.Num(), 2);
		const TSharedPtr<FJsonObject> Root = Items[1]->AsObject();
		TestTrue(TEXT("Material root deliberately lacks a GUID"),
			Root->GetStringField(TEXT("native_guid")).IsEmpty());
		TestEqual(TEXT("Material root has a stable built-in role"),
			Root->GetStringField(TEXT("stable_builtin_role")),
			FString(TEXT("material_root")));
		TSet<FString> ItemKeys;
		for (const TSharedPtr<FJsonValue>& ItemValue : Items)
		{
			ItemKeys.Add(ItemValue->AsObject()->GetStringField(
				TEXT("item_key")));
		}
		TSet<FString> PinKeys;
		for (const TSharedPtr<FJsonValue>& PinValue : Pins)
		{
			const TSharedPtr<FJsonObject> Pin = PinValue->AsObject();
			PinKeys.Add(Pin->GetStringField(TEXT("pin_key")));
			TestTrue(TEXT("Material pin resolves an exact owner"),
				ItemKeys.Contains(Pin->GetStringField(
					TEXT("owner_item_key"))));
		}
		const TSharedPtr<FJsonObject> Link = Links[0]->AsObject();
		TestTrue(TEXT("Material link source is an exact output identity"),
			PinKeys.Contains(Link->GetStringField(
				TEXT("from_pin_identity"))));
		TestTrue(TEXT("Material root property is an exact input identity"),
			PinKeys.Contains(Link->GetStringField(
				TEXT("to_pin_identity"))));
	}
	if (ParameterCase.IsValid())
	{
		const TSharedPtr<FJsonObject> Identity =
			ParameterCase->GetObjectField(TEXT("parameter_identity"));
		TestFalse(TEXT("parameter name participates in identity"),
			Identity->GetStringField(TEXT("parameter_name")).IsEmpty());
		TestFalse(TEXT("parameter association participates in identity"),
			Identity->GetStringField(TEXT("parameter_association")).IsEmpty());
		TestTrue(TEXT("parameter index participates in identity"),
			Identity->HasField(TEXT("parameter_index")));
	}
	if (NiagaraCase.IsValid())
	{
		const TSharedPtr<FJsonObject> Stage =
			NiagaraCase->GetObjectField(TEXT("stage_identity"));
		TestFalse(TEXT("Niagara stage has a stable key"),
			Stage->GetStringField(TEXT("stage_key")).IsEmpty());
		TestFalse(TEXT("Niagara system emitter identity uses handle ID"),
			Stage->GetStringField(TEXT("emitter_handle_id")).IsEmpty());
		const TArray<TSharedPtr<FJsonValue>>& StackItems =
			NiagaraCase->GetArrayField(TEXT("stack_items"));
		TSet<FString> StackItemKeys;
		for (const TSharedPtr<FJsonValue>& ItemValue : StackItems)
		{
			const TSharedPtr<FJsonObject> Item = ItemValue->AsObject();
			StackItemKeys.Add(Item->GetStringField(TEXT("item_key")));
			TestFalse(TEXT("Niagara stack item uses native node GUID"),
				Item->GetStringField(TEXT("module_node_guid")).IsEmpty());
		}
		for (const TSharedPtr<FJsonValue>& OrderValue :
			NiagaraCase->GetArrayField(TEXT("ordering")))
		{
			const TSharedPtr<FJsonObject> Order = OrderValue->AsObject();
			TestTrue(TEXT("ordering refers to an exact stack item"),
				StackItemKeys.Contains(Order->GetStringField(
					TEXT("item_key"))));
		}
		const TSharedPtr<FJsonObject> Delegate =
			NiagaraCase->GetObjectField(TEXT("compound_delegate"));
		TestEqual(TEXT("compound validation has one existing owner"),
			Delegate->GetStringField(TEXT("adapter_id")),
			FString(TEXT("D/NiagaraCompound")));
		TestEqual(TEXT("compound failures are propagated, not hidden"),
			Delegate->GetStringField(TEXT("failure_policy")),
			FString(TEXT("propagate_exact_failure")));
		TestEqual(TEXT("compound nesting has no fixed depth ceiling"),
			Delegate->GetStringField(TEXT("nesting_policy")),
			FString(TEXT("no_fixed_depth_limit_shared_resource_budget")));
		TestTrue(TEXT("compound selector facets are exact"),
			HasExactStrings(
				ReadStrings(Delegate, TEXT("selector_facets")),
				{
					TEXT("stage_key"),
					TEXT("emitter_handle_id"),
					TEXT("module_node_guid"),
					TEXT("input_name"),
				}));
	}

	const TSet<FString> RequiredNegativeCodes = {
		TEXT("material_niagara_graph_item_identity_duplicate"),
		TEXT("material_niagara_graph_link_endpoint_unresolved"),
		TEXT("material_niagara_graph_stack_item_identity_missing"),
		TEXT("material_niagara_graph_stack_capture_failed"),
		TEXT("material_niagara_graph_compound_capture_failed"),
		TEXT("niagara_compound_branch_conflict"),
		TEXT("material_niagara_graph_compile_incomplete"),
		TEXT("material_niagara_graph_stack_issue"),
		TEXT("material_niagara_graph_readback_not_independent"),
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
	bool bHasExactNegativeCodes =
		NegativeCodes.Num() == RequiredNegativeCodes.Num();
	for (const FString& Code : RequiredNegativeCodes)
	{
		bHasExactNegativeCodes = NegativeCodes.Contains(Code)
			&& bHasExactNegativeCodes;
	}
	TestTrue(TEXT("negative failure semantics are exact"),
		bHasExactNegativeCodes);

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
		TEXT("resource_budget_policy"),
		TEXT("transaction_or_save"),
		TEXT("verification_or_outcome"),
		TEXT("response_projection"),
	};
	TestTrue(TEXT("Adapter forbidden owner set is exact"),
		HasExactStrings(
			ReadStrings(Fixture, TEXT("forbidden_owners")),
			RequiredForbiddenOwners.Array()));
	TestTrue(TEXT("old folder provider is never an oracle"),
		ReadStrings(Fixture, TEXT("forbidden_oracles")).Contains(
			TEXT("legacy_folder_apply_provider")));
	TestTrue(TEXT("compile receipt is never a readback oracle"),
		ReadStrings(Fixture, TEXT("forbidden_oracles")).Contains(
			TEXT("mutation_or_compile_receipt")));

	using namespace UE::UAI::DomainAdapters::MaterialNiagaraGraph;
	TestEqual(TEXT("production contract version"),
		UAI_MATERIAL_NIAGARA_GRAPH_CONTRACT_V1, 1);
	const auto& ProductionFamilies =
		GetMaterialNiagaraGraphPortFamilyContracts();
	const auto& ProductionCapabilities =
		GetMaterialNiagaraGraphCapabilityContracts();
	TestEqual(TEXT("production family count"),
		ProductionFamilies.Num(), Families.Num());
	TestEqual(TEXT("production capability count"),
		ProductionCapabilities.Num(), Capabilities.Num());
	TestEqual(TEXT("compound delegate remains D/NiagaraCompound"),
		GetMaterialNiagaraGraphCompoundDelegateAdapterId(),
		FString(TEXT("D/NiagaraCompound")));
	TestEqual(TEXT("compound failure policy remains exact propagation"),
		GetMaterialNiagaraGraphCompoundFailurePolicy(),
		FString(TEXT("propagate_exact_failure")));
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
			TestEqual(TEXT("production family oracle: ")
				+ Family.GetFamilyId(),
				Family.GetOracleKind(),
				Expected->GetStringField(TEXT("oracle_kind")));
		}
	}

	using namespace UE::UAI::DomainAdapters::MaterialNiagaraGraph;
	TestEqual(TEXT("production Adapter contract version"),
		UAI_MATERIAL_NIAGARA_GRAPH_ADAPTER_V1, 1);

	const FMaterialNiagaraGraphTopology MaterialTopology(
		TEXT("material"),
		TEXT("/Game/UAI/GC004/M_GraphFixture"),
		TEXT("material_graph:root"),
		{
			FMaterialNiagaraGraphItemIdentity(
				TEXT("expression:constant3"),
				TEXT("11111111-2222-3333-4444-555555555555"),
				FString()),
			FMaterialNiagaraGraphItemIdentity(
				TEXT("material:root"), FString(), TEXT("material_root")),
		},
		{
			FMaterialNiagaraGraphPinIdentity(
				TEXT("expression:constant3:output:0"),
				TEXT("expression:constant3"), FString(), 0,
				EMaterialNiagaraGraphPinDirection::Output),
			FMaterialNiagaraGraphPinIdentity(
				TEXT("material:root:base_color"),
				TEXT("material:root"), TEXT("base_color"), INDEX_NONE,
				EMaterialNiagaraGraphPinDirection::Input),
		},
		{
			FMaterialNiagaraGraphLinkIdentity(
				TEXT("link:constant3:base_color"),
				TEXT("expression:constant3:output:0"),
				TEXT("material:root:base_color")),
		},
		{});
	FMaterialNiagaraGraphAdapterError AdapterError;
	TestTrue(TEXT("Material GUID/root/pin/link topology validates"),
		TryValidateMaterialNiagaraGraphTopology(
			MaterialTopology, AdapterError));

	const FMaterialNiagaraGraphTopology UnresolvedRootLink(
		TEXT("material"),
		MaterialTopology.GetTargetIdentity(),
		MaterialTopology.GetStructureIdentity(),
		MaterialTopology.GetItems(),
		MaterialTopology.GetPins(),
		{
			FMaterialNiagaraGraphLinkIdentity(
				TEXT("link:bad"),
				TEXT("expression:constant3:output:0"),
				TEXT("material:root:missing")),
		},
		{});
	TestFalse(TEXT("unresolved Material root endpoint fails preflight"),
		TryValidateMaterialNiagaraGraphTopology(
			UnresolvedRootLink, AdapterError));
	TestEqual(TEXT("unresolved Material endpoint error is stable"),
		AdapterError.GetCode(),
		FString(TEXT("material_niagara_graph_link_endpoint_unresolved")));

	const FMaterialNiagaraGraphTopology NiagaraStack(
		TEXT("niagara_stack"),
		TEXT("/Game/UAI/GC004/NS_StackFixture"),
		TEXT("particle_spawn:00000000-0000-0000-0000-000000000000"),
		{
			FMaterialNiagaraGraphItemIdentity(
				TEXT("module:initialize_particle"),
				TEXT("22222222-3333-4444-5555-666666666666"),
				FString()),
			FMaterialNiagaraGraphItemIdentity(
				TEXT("module:solve_forces"),
				TEXT("33333333-4444-5555-6666-777777777777"),
				FString()),
		},
		{},
		{},
		{
			FMaterialNiagaraGraphOrderingIdentity(
				TEXT("order:particle_spawn:0"),
				TEXT("module:initialize_particle"), FString()),
			FMaterialNiagaraGraphOrderingIdentity(
				TEXT("order:particle_spawn:1"),
				TEXT("module:solve_forces"),
				TEXT("module:initialize_particle")),
		});
	TestTrue(TEXT("Niagara stage/module/order topology validates"),
		TryValidateMaterialNiagaraGraphTopology(
			NiagaraStack, AdapterError));

	const FMaterialNiagaraGraphTopology MissingStackItem(
		TEXT("niagara_stack"),
		NiagaraStack.GetTargetIdentity(),
		NiagaraStack.GetStructureIdentity(),
		NiagaraStack.GetItems(), {}, {},
		{
			FMaterialNiagaraGraphOrderingIdentity(
				TEXT("order:bad"), TEXT("module:missing"), FString()),
		});
	TestFalse(TEXT("Niagara ordering cannot ignore a missing stack item"),
		TryValidateMaterialNiagaraGraphTopology(
			MissingStackItem, AdapterError));
	TestEqual(TEXT("missing stack item error is stable"),
		AdapterError.GetCode(),
		FString(TEXT("material_niagara_graph_stack_item_identity_missing")));

	const FMaterialNiagaraGraphLifecycleEvidence ValidLifecycle(
		TEXT("revision:81"),
		TEXT("fixture:gc-004:apply:82"),
		TEXT("fixture:gc-004:settle:82"),
		TEXT("revision:82"),
		TEXT("fixture:gc-004:independent-readback:82"),
		true,
		0,
		0,
		true,
		true);
	TestTrue(TEXT("complete settle and independent captures validate"),
		TryValidateMaterialNiagaraGraphLifecycleEvidence(
			ValidLifecycle, AdapterError));

	const FMaterialNiagaraGraphLifecycleEvidence StackCaptureFailed(
		TEXT("revision:81"),
		TEXT("fixture:gc-004:apply:82"),
		TEXT("fixture:gc-004:settle:82"),
		TEXT("revision:82"),
		TEXT("fixture:gc-004:independent-readback:82"),
		true, 0, 0, false, true);
	TestFalse(TEXT("stack capture failure cannot become success"),
		TryValidateMaterialNiagaraGraphLifecycleEvidence(
			StackCaptureFailed, AdapterError));
	TestEqual(TEXT("stack capture failure code is stable"),
		AdapterError.GetCode(),
		FString(TEXT("material_niagara_graph_stack_capture_failed")));

	const FMaterialNiagaraGraphLifecycleEvidence CompoundCaptureFailed(
		TEXT("revision:81"),
		TEXT("fixture:gc-004:apply:82"),
		TEXT("fixture:gc-004:settle:82"),
		TEXT("revision:82"),
		TEXT("fixture:gc-004:independent-readback:82"),
		true, 0, 0, true, false);
	TestFalse(TEXT("compound capture failure cannot become success"),
		TryValidateMaterialNiagaraGraphLifecycleEvidence(
			CompoundCaptureFailed, AdapterError));
	TestEqual(TEXT("compound capture failure code is stable"),
		AdapterError.GetCode(),
		FString(TEXT("material_niagara_graph_compound_capture_failed")));

	TestFalse(TEXT("dynamic-input validation failure cannot become success"),
		TryValidateMaterialNiagaraGraphCompoundCapture(
			false,
			TEXT("niagara_compound_branch_conflict"),
			TEXT("literal and dynamic branches are both active"),
			AdapterError));
	TestEqual(TEXT("D/NiagaraCompound failure code is propagated exactly"),
		AdapterError.GetCode(),
		FString(TEXT("niagara_compound_branch_conflict")));

	const FMaterialNiagaraGraphLifecycleEvidence CompileIncomplete(
		TEXT("revision:81"),
		TEXT("fixture:gc-004:apply:82"),
		TEXT("fixture:gc-004:settle:82"),
		TEXT("revision:82"),
		TEXT("fixture:gc-004:independent-readback:82"),
		false, 0, 0, true, true);
	TestFalse(TEXT("incomplete compile cannot be hidden by settle"),
		TryValidateMaterialNiagaraGraphLifecycleEvidence(
			CompileIncomplete, AdapterError));
	TestEqual(TEXT("compile incomplete error is stable"),
		AdapterError.GetCode(),
		FString(TEXT("material_niagara_graph_compile_incomplete")));

	const FMaterialNiagaraGraphLifecycleEvidence StackIssue(
		TEXT("revision:81"),
		TEXT("fixture:gc-004:apply:82"),
		TEXT("fixture:gc-004:settle:82"),
		TEXT("revision:82"),
		TEXT("fixture:gc-004:independent-readback:82"),
		true, 0, 1, true, true);
	TestFalse(TEXT("Niagara stack issue cannot be hidden by settle"),
		TryValidateMaterialNiagaraGraphLifecycleEvidence(
			StackIssue, AdapterError));
	TestEqual(TEXT("stack issue error is stable"),
		AdapterError.GetCode(),
		FString(TEXT("material_niagara_graph_stack_issue")));

	const FMaterialNiagaraGraphLifecycleEvidence EchoedReadback(
		TEXT("revision:81"),
		TEXT("fixture:gc-004:apply:82"),
		TEXT("fixture:gc-004:settle:82"),
		TEXT("revision:82"),
		TEXT("fixture:gc-004:apply:82"),
		true, 0, 0, true, true);
	TestFalse(TEXT("apply receipt cannot serve as graph/stack readback"),
		TryValidateMaterialNiagaraGraphLifecycleEvidence(
			EchoedReadback, AdapterError));
	TestEqual(TEXT("non-independent readback error is stable"),
		AdapterError.GetCode(),
		FString(TEXT("material_niagara_graph_readback_not_independent")));

	return true;
}

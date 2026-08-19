#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#include "DomainAdapters/BlueprintUmgGraph/UeAgentBlueprintUmgGraphContract.h"

#include "DomainAdapters/BlueprintUmgGraph/UeAgentBlueprintUmgGraphAdapter.h"

namespace
{
	FString FixturePath()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(
			FPaths::ProjectPluginsDir(),
			TEXT(
				"UeAgentInterface/Source/UeAgentInterfaceTests/Private/"
				"DomainAdapters/BlueprintUmgGraph/Fixtures/"
				"blueprint_umg_graph_contract_cases.json")));
	}

	TSharedPtr<FJsonObject> LoadFixture(FAutomationTestBase* Test)
	{
		FString Text;
		if (!Test->TestTrue(
				TEXT("load BlueprintUmgGraph contract fixture"),
				FFileHelper::LoadFileToString(Text, *FixturePath())))
		{
			return nullptr;
		}

		TSharedPtr<FJsonObject> Fixture;
		const TSharedRef<TJsonReader<>> Reader =
			TJsonReaderFactory<>::Create(Text);
		if (!Test->TestTrue(
				TEXT("parse BlueprintUmgGraph contract fixture"),
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
	FUeAgentBlueprintUmgGraphContractTest,
	"UeAgentInterface.DomainAdapters.BlueprintUmgGraph.E0.Contract."
		"IdentityTopologyLifecycleAndLegacyBoundary",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentBlueprintUmgGraphContractTest::RunTest(
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
			"uai.domain_adapter.blueprint_umg_graph.test_fixture.v1")));
	TestEqual(
		TEXT("canonical graph value has one shared schema"),
		Fixture->GetStringField(TEXT("canonical_value_schema")),
		FString(TEXT("uai.graph_compound.canonical.v1")));
	TestEqual(
		TEXT("adapter owner is exact"),
		Fixture->GetStringField(TEXT("adapter_id")),
		FString(TEXT("D/BlueprintUmgGraph")));

	const TArray<TSharedPtr<FJsonValue>>& Families =
		Fixture->GetArrayField(TEXT("port_families"));
	const TArray<TSharedPtr<FJsonValue>>& Capabilities =
		Fixture->GetArrayField(TEXT("capabilities"));
	const TArray<TSharedPtr<FJsonValue>>& Mappings =
		Fixture->GetArrayField(TEXT("legacy_mappings"));
	TestEqual(
		TEXT("six native port families"),
		Families.Num(),
		static_cast<int32>(Fixture->GetNumberField(
			TEXT("port_family_count"))));
	TestEqual(
		TEXT("six exact capabilities"),
		Capabilities.Num(),
		static_cast<int32>(Fixture->GetNumberField(
			TEXT("capability_count"))));
	TestEqual(
		TEXT("all six legacy inputs adjudicated"),
		Mappings.Num(),
		static_cast<int32>(Fixture->GetNumberField(
			TEXT("legacy_candidate_count"))));

	const TSet<FString> ExpectedFamilies = {
		TEXT("asset_lifecycle"),
		TEXT("graph_mutation"),
		TEXT("hierarchy_mutation"),
		TEXT("binding_mutation"),
		TEXT("event_discovery"),
		TEXT("compile_settle"),
	};
	TSet<FString> FamilyIds;
	for (const TSharedPtr<FJsonValue>& Value : Families)
	{
		const TSharedPtr<FJsonObject> Family = Value->AsObject();
		const FString FamilyId =
			Family->GetStringField(TEXT("family_id"));
		TestFalse(
			TEXT("family identity is unique: ") + FamilyId,
			FamilyIds.Contains(FamilyId));
		FamilyIds.Add(FamilyId);
		TestTrue(
			TEXT("family has explicit identity facets: ") + FamilyId,
			!Family->GetArrayField(
				TEXT("required_identity_facets")).IsEmpty());
		TestTrue(
			TEXT("family has explicit lifecycle stages: ") + FamilyId,
			!Family->GetArrayField(
				TEXT("required_stages")).IsEmpty());
		TestFalse(
			TEXT("family has independent oracle: ") + FamilyId,
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
			TEXT("capability binds an existing family: ")
				+ CapabilityId,
			FamilyIds.Contains(FamilyId));
		TestEqual(
			TEXT("capability version: ") + CapabilityId,
			static_cast<int32>(
				Capability->GetNumberField(TEXT("version"))),
			1);
	}
	for (const TSharedPtr<FJsonValue>& Value : Mappings)
	{
		const TSharedPtr<FJsonObject> Mapping = Value->AsObject();
		for (const FString& Target :
			ReadStrings(Mapping, TEXT("target_capability_ids")))
		{
			TestTrue(
				TEXT("legacy mapping resolves exact capability: ")
					+ Mapping->GetStringField(TEXT("legacy_id")),
				CapabilityIds.Contains(Target));
		}
	}

	const TArray<TSharedPtr<FJsonValue>>& IdentityCases =
		Fixture->GetArrayField(TEXT("identity_cases"));
	const TSharedPtr<FJsonObject> BlueprintCase = FindByStringField(
		IdentityCases,
		TEXT("case_id"),
		TEXT("blueprint_builtin_and_guid_nodes"));
	const TSharedPtr<FJsonObject> UmgCase = FindByStringField(
		IdentityCases,
		TEXT("case_id"),
		TEXT("umg_tree_slot_and_binding"));
	TestTrue(TEXT("Blueprint graph identity case exists"),
		BlueprintCase.IsValid());
	TestTrue(TEXT("UMG hierarchy/binding case exists"),
		UmgCase.IsValid());
	if (BlueprintCase.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>& Items =
			BlueprintCase->GetArrayField(TEXT("items"));
		const TArray<TSharedPtr<FJsonValue>>& Pins =
			BlueprintCase->GetArrayField(TEXT("pins"));
		const TArray<TSharedPtr<FJsonValue>>& Links =
			BlueprintCase->GetArrayField(TEXT("links"));
		TestEqual(TEXT("Blueprint fixture has two nodes"), Items.Num(), 2);
		TestEqual(TEXT("Blueprint fixture has two endpoint pins"),
			Pins.Num(), 2);
		TestEqual(TEXT("Blueprint fixture has one exact link"),
			Links.Num(), 1);
		const TSharedPtr<FJsonObject> Builtin = Items[0]->AsObject();
		TestTrue(TEXT("built-in node deliberately lacks a GUID"),
			Builtin->GetStringField(TEXT("native_guid")).IsEmpty());
		TestFalse(TEXT("built-in node instead has stable role identity"),
			Builtin->GetStringField(TEXT("stable_builtin_role")).IsEmpty());
		TSet<FString> PinIds;
		for (const TSharedPtr<FJsonValue>& PinValue : Pins)
		{
			PinIds.Add(PinValue->AsObject()->GetStringField(
				TEXT("pin_identity")));
		}
		const TSharedPtr<FJsonObject> Link = Links[0]->AsObject();
		TestTrue(TEXT("link source resolves by pin identity"),
			PinIds.Contains(Link->GetStringField(
				TEXT("from_pin_identity"))));
		TestTrue(TEXT("link target resolves by pin identity"),
			PinIds.Contains(Link->GetStringField(
				TEXT("to_pin_identity"))));
	}
	if (UmgCase.IsValid())
	{
		TestEqual(TEXT("UMG fixture has explicit hierarchy relation"),
			UmgCase->GetArrayField(TEXT("hierarchy")).Num(), 1);
		TestEqual(TEXT("UMG fixture has explicit binding identity"),
			UmgCase->GetArrayField(TEXT("bindings")).Num(), 1);
	}

	const TArray<TSharedPtr<FJsonValue>>& NegativeCases =
		Fixture->GetArrayField(TEXT("negative_cases"));
	const TSet<FString> RequiredNegativeCodes = {
		TEXT("blueprint_umg_graph_item_identity_duplicate"),
		TEXT("blueprint_umg_graph_link_endpoint_unresolved"),
		TEXT("blueprint_umg_graph_hierarchy_cycle"),
		TEXT("blueprint_umg_graph_builtin_identity_missing"),
		TEXT("blueprint_umg_graph_compile_incomplete"),
		TEXT("blueprint_umg_graph_readback_not_independent"),
	};
	TSet<FString> NegativeCodes;
	for (const TSharedPtr<FJsonValue>& Value : NegativeCases)
	{
		NegativeCodes.Add(Value->AsObject()->GetStringField(
			TEXT("error_code")));
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

	using namespace UE::UAI::DomainAdapters::BlueprintUmgGraph;
	TestEqual(TEXT("production contract version"),
		UAI_BLUEPRINT_UMG_GRAPH_CONTRACT_V1, 1);
	const auto& ProductionFamilies =
		GetBlueprintUmgGraphPortFamilyContracts();
	const auto& ProductionCapabilities =
		GetBlueprintUmgGraphCapabilityContracts();
	TestEqual(TEXT("production family count"),
		ProductionFamilies.Num(), Families.Num());
	TestEqual(TEXT("production capability count"),
		ProductionCapabilities.Num(), Capabilities.Num());
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
					ReadStrings(Expected,
						TEXT("required_stages"))));
			TestEqual(TEXT("production family oracle: ")
				+ Family.GetFamilyId(),
				Family.GetOracleKind(),
				Expected->GetStringField(TEXT("oracle_kind")));
		}
	}

	using namespace UE::UAI::DomainAdapters::BlueprintUmgGraph;
	TestEqual(TEXT("production Adapter contract version"),
		UAI_BLUEPRINT_UMG_GRAPH_ADAPTER_V1, 1);

	const FBlueprintUmgGraphTopology ValidTopology(
		TEXT("blueprint"),
		TEXT("/Game/UAI/GC002/BP_GraphFixture"),
		TEXT("graph:EventGraph"),
		{
			FBlueprintUmgGraphItemIdentity(
				TEXT("node:builtin:event_entry"), FString(),
				TEXT("event_entry:ReceiveBeginPlay")),
			FBlueprintUmgGraphItemIdentity(
				TEXT("node:call:print_string"),
				TEXT("11111111-2222-3333-4444-555555555555"),
				FString()),
		},
		{
			FBlueprintUmgGraphPinIdentity(
				TEXT("pin:entry:Then"),
				TEXT("node:builtin:event_entry"),
				TEXT("Then"),
				EBlueprintUmgGraphPinDirection::Output),
			FBlueprintUmgGraphPinIdentity(
				TEXT("pin:print:execute"),
				TEXT("node:call:print_string"),
				TEXT("execute"),
				EBlueprintUmgGraphPinDirection::Input),
		},
		{
			FBlueprintUmgGraphLinkIdentity(
				TEXT("link:begin_play_to_print"),
				TEXT("pin:entry:Then"),
				TEXT("pin:print:execute")),
		},
		{},
		{});
	FBlueprintUmgGraphAdapterError AdapterError;
	TestTrue(TEXT("stable built-in role and exact pin endpoints validate"),
		TryValidateBlueprintUmgGraphTopology(
			ValidTopology, AdapterError));

	const FBlueprintUmgGraphTopology UnresolvedLink(
		TEXT("blueprint"),
		TEXT("/Game/UAI/GC002/BP_GraphFixture"),
		TEXT("graph:EventGraph"),
		ValidTopology.GetItems(),
		ValidTopology.GetPins(),
		{
			FBlueprintUmgGraphLinkIdentity(
				TEXT("link:bad"), TEXT("pin:entry:Then"),
				TEXT("pin:missing")),
		},
		{},
		{});
	TestFalse(TEXT("unresolved link fails before mutation"),
		TryValidateBlueprintUmgGraphTopology(
			UnresolvedLink, AdapterError));
	TestEqual(TEXT("unresolved link error is stable"),
		AdapterError.GetCode(),
		FString(TEXT("blueprint_umg_graph_link_endpoint_unresolved")));

	TArray<FBlueprintUmgGraphItemIdentity> DeepItems;
	TArray<FBlueprintUmgGraphHierarchyRelation> DeepHierarchy;
	DeepItems.Reserve(513);
	DeepHierarchy.Reserve(512);
	DeepItems.Emplace(TEXT("widget:0"), FString(), TEXT("root_widget"));
	for (int32 Index = 1; Index <= 512; ++Index)
	{
		const FString Item = FString::Printf(TEXT("widget:%d"), Index);
		const FString Parent = FString::Printf(TEXT("widget:%d"), Index - 1);
		DeepItems.Emplace(Item, FString::Printf(TEXT("guid:%d"), Index), FString());
		DeepHierarchy.Emplace(
			FString::Printf(TEXT("parent:%d"), Index),
			Item, Parent,
			FString::Printf(TEXT("order:%d:0"), Index - 1));
	}
	const FBlueprintUmgGraphTopology DeepTree(
		TEXT("widget_blueprint"),
		TEXT("/Game/UAI/GC002/WBP_DeepFixture"),
		TEXT("tree:WidgetTree"),
		MoveTemp(DeepItems), {}, {}, MoveTemp(DeepHierarchy), {});
	TestTrue(TEXT("acyclic hierarchy has no nesting depth ceiling"),
		TryValidateBlueprintUmgGraphTopology(DeepTree, AdapterError));

	const FBlueprintUmgGraphLifecycleEvidence LifecycleEvidence(
		TEXT("revision:41"),
		TEXT("fixture:gc-002:apply:42"),
		TEXT("fixture:gc-002:settle:42"),
		TEXT("revision:42"),
		TEXT("fixture:gc-002:independent-readback:42"),
		true,
		0);
	TestTrue(TEXT("compile/settle/readback evidence validates separately"),
		TryValidateBlueprintUmgGraphLifecycleEvidence(
			LifecycleEvidence, AdapterError));
	const FBlueprintUmgGraphLifecycleEvidence EchoedReadback(
		TEXT("revision:41"),
		TEXT("fixture:gc-002:apply:42"),
		TEXT("fixture:gc-002:settle:42"),
		TEXT("revision:42"),
		TEXT("fixture:gc-002:apply:42"),
		true,
		0);
	TestFalse(TEXT("apply receipt cannot serve as readback"),
		TryValidateBlueprintUmgGraphLifecycleEvidence(
			EchoedReadback, AdapterError));
	TestEqual(TEXT("non-independent readback error is stable"),
		AdapterError.GetCode(),
		FString(TEXT("blueprint_umg_graph_readback_not_independent")));

	return true;
}

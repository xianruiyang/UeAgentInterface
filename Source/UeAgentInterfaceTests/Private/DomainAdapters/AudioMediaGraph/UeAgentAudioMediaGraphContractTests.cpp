#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

// GC-011 rebuilds this source after introducing the production boundary.
#include "DomainAdapters/AudioMediaGraph/UeAgentAudioMediaGraphContract.h"

#include "DomainAdapters/AudioMediaGraph/UeAgentAudioMediaGraphAdapter.h"

namespace
{
	FString FixturePath()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(
			FPaths::ProjectPluginsDir(),
			TEXT("UeAgentInterface/Source/UeAgentInterfaceTests/Private/")
			TEXT("DomainAdapters/AudioMediaGraph/Fixtures/")
			TEXT("audio_media_graph_contract_cases.json")));
	}

	TSharedPtr<FJsonObject> LoadFixture(FAutomationTestBase* Test)
	{
		FString Text;
		if (!Test->TestTrue(
			TEXT("load AudioMediaGraph contract fixture"),
			FFileHelper::LoadFileToString(Text, *FixturePath())))
		{
			return nullptr;
		}
		TSharedPtr<FJsonObject> Fixture;
		const TSharedRef<TJsonReader<>> Reader =
			TJsonReaderFactory<>::Create(Text);
		if (!Test->TestTrue(
			TEXT("parse AudioMediaGraph contract fixture"),
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

	TSet<FString> ToSet(const TArray<FString>& Values)
	{
		TSet<FString> Result;
		for (const FString& Value : Values)
		{
			Result.Add(Value);
		}
		return Result;
	}

	bool HasSameStrings(
		const TSet<FString>& Left,
		const TSet<FString>& Right)
	{
		if (Left.Num() != Right.Num())
		{
			return false;
		}
		for (const FString& Value : Left)
		{
			if (!Right.Contains(Value))
			{
				return false;
			}
		}
		return true;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentAudioMediaGraphContractTest,
	"UeAgentInterface.DomainAdapters.AudioMediaGraph.E0.Contract."
		"NativeIdentityLifecycleArtifactAndLegacyBoundary",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentAudioMediaGraphContractTest::RunTest(
	const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Fixture = LoadFixture(this);
	if (!Fixture.IsValid())
	{
		return false;
	}

	TestEqual(TEXT("fixture schema"),
		Fixture->GetStringField(TEXT("schema")),
		FString(TEXT("uai.domain_adapter.audio_media_graph.test_fixture.v1")));
	TestEqual(TEXT("shared canonical graph value"),
		Fixture->GetStringField(TEXT("canonical_value_schema")),
		FString(TEXT("uai.graph_compound.canonical.v1")));
	TestEqual(TEXT("exact Adapter owner"),
		Fixture->GetStringField(TEXT("adapter_id")),
		FString(TEXT("D/AudioMediaGraph")));

	const TArray<TSharedPtr<FJsonValue>>& Families =
		Fixture->GetArrayField(TEXT("port_families"));
	const TArray<TSharedPtr<FJsonValue>>& Capabilities =
		Fixture->GetArrayField(TEXT("capabilities"));
	const TArray<TSharedPtr<FJsonValue>>& Mappings =
		Fixture->GetArrayField(TEXT("legacy_mappings"));
	TestEqual(TEXT("five native port families"), Families.Num(), 5);
	TestEqual(TEXT("fifteen exact capabilities"), Capabilities.Num(), 15);
	TestEqual(TEXT("seven legacy inputs adjudicated"), Mappings.Num(), 7);

	const TSet<FString> ExpectedFamilies = {
		TEXT("asset_lifecycle"),
		TEXT("metasound_document_graph_mutation"),
		TEXT("sound_cue_node_graph_mutation"),
		TEXT("texture_graph_topology_mutation"),
		TEXT("texture_graph_artifact_state"),
	};
	TSet<FString> FamilyIds;
	for (const TSharedPtr<FJsonValue>& Value : Families)
	{
		const TSharedPtr<FJsonObject> Family = Value->AsObject();
		const FString FamilyId = Family->GetStringField(TEXT("family_id"));
		TestFalse(TEXT("unique family: ") + FamilyId,
			FamilyIds.Contains(FamilyId));
		FamilyIds.Add(FamilyId);
		TestFalse(TEXT("native identity facets: ") + FamilyId,
			Family->GetArrayField(TEXT("required_identity_facets")).IsEmpty());
		TestFalse(TEXT("lifecycle stages: ") + FamilyId,
			Family->GetArrayField(TEXT("required_stages")).IsEmpty());
		TestFalse(TEXT("independent native oracle: ") + FamilyId,
			Family->GetStringField(TEXT("oracle_kind")).IsEmpty());
	}
	TestTrue(TEXT("family set is exact"),
		HasSameStrings(FamilyIds, ExpectedFamilies));

	TSet<FString> CapabilityIds;
	int32 QueryCount = 0;
	int32 EffectCount = 0;
	for (const TSharedPtr<FJsonValue>& Value : Capabilities)
	{
		const TSharedPtr<FJsonObject> Capability = Value->AsObject();
		const FString CapabilityId =
			Capability->GetStringField(TEXT("capability_id"));
		TestFalse(TEXT("unique capability: ") + CapabilityId,
			CapabilityIds.Contains(CapabilityId));
		CapabilityIds.Add(CapabilityId);
		TestTrue(TEXT("capability family exists: ") + CapabilityId,
			FamilyIds.Contains(Capability->GetStringField(
				TEXT("port_family_id"))));
		const bool bArtifactQuery = CapabilityId
			== TEXT("texture_graph.artifact.capture");
		if (bArtifactQuery)
		{
			++QueryCount;
			TestEqual(TEXT("artifact observation is a read-only query"),
				Capability->GetStringField(TEXT("operation_kind")),
				FString(TEXT("query")));
			TestEqual(TEXT("artifact observation has no effect ownership"),
				Capability->GetStringField(TEXT("effect_class")),
				FString(TEXT("compound_query")));
		}
		else
		{
			++EffectCount;
			TestEqual(TEXT("mutation capability is a compound effect: ")
				+ CapabilityId,
				Capability->GetStringField(TEXT("effect_class")),
				FString(TEXT("compound_effect")));
		}
		TestEqual(TEXT("capability version: ") + CapabilityId,
			static_cast<int32>(Capability->GetNumberField(TEXT("version"))),
			1);
	}
	TestEqual(TEXT("one capability is a query"), QueryCount, 1);
	TestEqual(TEXT("fourteen capabilities are effects"), EffectCount, 14);

	const TSet<FString> ExpectedLegacy = {
		TEXT("metasound_apply_folder"), TEXT("metasound_create"),
		TEXT("sound_cue_apply_folder"), TEXT("sound_cue_create"),
		TEXT("texture_graph_apply_folder"),
		TEXT("texture_graph_bake_outputs"), TEXT("texture_graph_create"),
	};
	TSet<FString> LegacyIds;
	const TSet<FString> ExternalSuccessorIds = ToSet(
		ReadStrings(Fixture, TEXT("external_successor_capability_ids")));
	TestTrue(TEXT("artifact bake is owned outside the graph adapter contract"),
		ExternalSuccessorIds.Num() == 1
			&& ExternalSuccessorIds.Contains(
				TEXT("texture_graph.artifact.bake")));
	for (const TSharedPtr<FJsonValue>& Value : Mappings)
	{
		const TSharedPtr<FJsonObject> Mapping = Value->AsObject();
		const FString LegacyId = Mapping->GetStringField(TEXT("legacy_id"));
		LegacyIds.Add(LegacyId);
		TestEqual(TEXT("legacy is scenario evidence only: ") + LegacyId,
			Mapping->GetStringField(TEXT("source_role")),
			FString(TEXT("scenario_only")));
		const TArray<FString> Targets =
			ReadStrings(Mapping, TEXT("target_capability_ids"));
		TestFalse(TEXT("legacy input maps to target capabilities: ") + LegacyId,
			Targets.IsEmpty());
		for (const FString& Target : Targets)
		{
			TestTrue(TEXT("mapped successor is explicitly owned: ") + Target,
				CapabilityIds.Contains(Target)
					|| ExternalSuccessorIds.Contains(Target));
		}
		TestFalse(TEXT("legacy oracle is explicitly rejected: ") + LegacyId,
			Mapping->GetStringField(TEXT("prohibited_oracle")).IsEmpty());
	}
	TestTrue(TEXT("legacy set is exact"),
		HasSameStrings(LegacyIds, ExpectedLegacy));

	const TArray<TSharedPtr<FJsonValue>>& IdentityCases =
		Fixture->GetArrayField(TEXT("identity_cases"));
	TSet<FString> IdentitySources;
	for (const TSharedPtr<FJsonValue>& Value : IdentityCases)
	{
		const TSharedPtr<FJsonObject> Identity = Value->AsObject();
		const FString CaseId = Identity->GetStringField(TEXT("case_id"));
		const FString Source =
			Identity->GetStringField(TEXT("identity_source"));
		IdentitySources.Add(Source);
		TestTrue(TEXT("identity source is native: ") + CaseId,
			Source == TEXT("native_guid")
				|| Source == TEXT("uobject_path")
				|| Source == TEXT("native_composite"));
		TestFalse(TEXT("native key is present: ") + CaseId,
			Identity->GetStringField(TEXT("native_key")).IsEmpty());
		TestFalse(TEXT("type key is present: ") + CaseId,
			Identity->GetStringField(TEXT("type_key")).IsEmpty());
	}
	TestTrue(TEXT("MetaSound uses native GUID identity"),
		IdentitySources.Contains(TEXT("native_guid")));
	TestTrue(TEXT("SoundCue uses UObject path identity"),
		IdentitySources.Contains(TEXT("uobject_path")));
	TestTrue(TEXT("ordered/native compound identity is preserved"),
		IdentitySources.Contains(TEXT("native_composite")));

	const TSharedPtr<FJsonObject> DeepCase =
		Fixture->GetObjectField(TEXT("deep_acyclic_case"));
	TestEqual(TEXT("deep valid graph is not a fixed limit"),
		DeepCase->GetStringField(TEXT("validation")),
		FString(TEXT("iterative_no_fixed_depth_limit")));
	TestTrue(TEXT("deep case is non-trivial"),
		DeepCase->GetNumberField(TEXT("item_count")) >= 512.0);
	TestTrue(TEXT("resource protection is an operation budget"),
		DeepCase->GetNumberField(TEXT("operation_budget"))
			> DeepCase->GetNumberField(TEXT("item_count")));

	TSet<FString> NegativeCodes;
	for (const TSharedPtr<FJsonValue>& Value :
		Fixture->GetArrayField(TEXT("negative_cases")))
	{
		NegativeCodes.Add(Value->AsObject()->GetStringField(TEXT("error_code")));
	}
	for (const FString& RequiredCode : {
		FString(TEXT("audio_media_graph_duplicate_identity")),
		FString(TEXT("audio_media_graph_endpoint_missing")),
		FString(TEXT("audio_media_graph_cycle")),
		FString(TEXT("audio_media_graph_compile_incomplete")),
		FString(TEXT("audio_media_graph_evaluate_incomplete")),
		FString(TEXT("audio_media_graph_settle_incomplete")),
		FString(TEXT("audio_media_graph_capture_failed")),
		FString(TEXT("audio_media_graph_artifact_before_settle")),
		FString(TEXT("audio_media_graph_echo_readback_forbidden"))})
	{
		TestTrue(TEXT("fail-closed code exists: ") + RequiredCode,
			NegativeCodes.Contains(RequiredCode));
	}

	const TSharedPtr<FJsonObject> Lifecycle =
		Fixture->GetObjectField(TEXT("lifecycle_evidence"));
	TSet<FString> EvidenceValues;
	for (const FString& Field :
		ReadStrings(Lifecycle, TEXT("required_distinct_evidence")))
	{
		const FString Value = Lifecycle->GetStringField(Field);
		TestFalse(TEXT("lifecycle evidence exists: ") + Field,
			Value.IsEmpty());
		TestFalse(TEXT("lifecycle evidence is distinct: ") + Field,
			EvidenceValues.Contains(Value));
		EvidenceValues.Add(Value);
	}
	TestFalse(TEXT("independent capture advances revision"),
		Lifecycle->GetStringField(TEXT("before_revision"))
			== Lifecycle->GetStringField(TEXT("readback_revision")));

	const TSet<FString> ForbiddenOwners =
		ToSet(ReadStrings(Fixture, TEXT("forbidden_owners")));
	for (const FString& Owner : {
		FString(TEXT("json_or_hjson_parser")),
		FString(TEXT("transaction_or_save")),
		FString(TEXT("artifact_publisher")),
		FString(TEXT("verification_or_outcome")),
		FString(TEXT("network_transport"))})
	{
		TestTrue(TEXT("Adapter does not own: ") + Owner,
			ForbiddenOwners.Contains(Owner));
	}
	const TSet<FString> ForbiddenOracles =
		ToSet(ReadStrings(Fixture, TEXT("forbidden_oracles")));
	for (const FString& Oracle : {
		FString(TEXT("legacy_folder_or_reflection_apply_provider")),
		FString(TEXT("empty_nodes_edges_outputs_arrays")),
		FString(TEXT("outputs_num_nonnegative")),
		FString(TEXT("created_texture_echo")),
		FString(TEXT("compile_receipt_as_readback"))})
	{
		TestTrue(TEXT("invalid oracle rejected: ") + Oracle,
			ForbiddenOracles.Contains(Oracle));
	}

	using namespace UE::UAI::DomainAdapters::AudioMediaGraph;
	static_assert(UAI_AUDIO_MEDIA_GRAPH_CONTRACT_V1 == 1,
		"AudioMediaGraph production contract version must be explicit");
	TestEqual(TEXT("production family count"),
		GetAudioMediaGraphPortFamilyContracts().Num(), Families.Num());
	TestEqual(TEXT("production capability count"),
		GetAudioMediaGraphCapabilityContracts().Num(), Capabilities.Num());

	using namespace UE::UAI::DomainAdapters::AudioMediaGraph;
	FAudioMediaGraphError Error;
	FAudioMediaGraphStableIdentity ValidIdentity(
		TEXT("native_guid"), TEXT("node_guid:gc010"),
		TEXT("MetasoundFrontendNode"));
	TestTrue(TEXT("typed native identity validates"),
		TryValidateAudioMediaGraphStableIdentity(ValidIdentity, Error));

	TArray<FAudioMediaGraphStableIdentity> DeepItems;
	TArray<FAudioMediaGraphRelation> DeepRelations;
	const int32 DeepCount = static_cast<int32>(
		DeepCase->GetNumberField(TEXT("item_count")));
	for (int32 Index = 0; Index < DeepCount; ++Index)
	{
		DeepItems.Emplace(TEXT("native_composite"),
			FString::Printf(TEXT("sound_node:%d"), Index),
			TEXT("SoundNode"));
		if (Index > 0)
		{
			DeepRelations.Emplace(
				DeepItems[Index - 1].GetNativeKey(),
				DeepItems[Index].GetNativeKey(),
				TEXT("ordered_child"), 0);
		}
	}
	TestTrue(TEXT("512-deep acyclic graph validates iteratively"),
		TryValidateAudioMediaGraphAcyclicRelations(
			DeepItems, DeepRelations,
			static_cast<int32>(DeepCase->GetNumberField(
				TEXT("operation_budget"))), Error));
	DeepRelations.Emplace(
		DeepItems.Last().GetNativeKey(),
		DeepItems[0].GetNativeKey(), TEXT("ordered_child"), 0);
	TestFalse(TEXT("real cycle fails closed"),
		TryValidateAudioMediaGraphAcyclicRelations(
			DeepItems, DeepRelations,
			static_cast<int32>(DeepCase->GetNumberField(
				TEXT("operation_budget"))), Error));
	TestEqual(TEXT("cycle error is exact"), Error.GetCode(),
		FString(TEXT("audio_media_graph_cycle")));

	FAudioMediaGraphLifecycleEvidence LifecycleEvidence(
		EAudioMediaGraphBarrierKind::Compile, true, true, true,
		TEXT("revision:100"), TEXT("fixture:gc-010:apply:101"),
		TEXT("fixture:gc-010:barrier:101"),
		TEXT("fixture:gc-010:settle:101"), TEXT("revision:101"),
		TEXT("fixture:gc-010:independent-readback:101"));
	TestTrue(TEXT("separated lifecycle evidence validates"),
		TryValidateAudioMediaGraphLifecycleEvidence(
			LifecycleEvidence, Error));

	return !HasAnyErrors();
}

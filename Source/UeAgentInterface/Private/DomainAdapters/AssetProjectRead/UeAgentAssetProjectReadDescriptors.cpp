#include "DomainAdapters/AssetProjectRead/UeAgentAssetProjectReadDescriptors.h"

#include "Core/Contract/UeAgentContractJson.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace UE::UAI::DomainAdapters::AssetProjectRead
{
	using namespace UE::UAI::Contract;

	namespace
	{
		FString CanonicalSchema(const TCHAR* Schema)
		{
			TSharedPtr<FJsonObject> Object;
			const TSharedRef<TJsonReader<>> Reader =
				TJsonReaderFactory<>::Create(Schema);
			if (!FJsonSerializer::Deserialize(Reader, Object)
				|| !Object.IsValid())
			{
				return Schema;
			}
			FString Canonical;
			UE::UAI::Contract::FDiagnostic Error;
			return Json::TryCanonicalize(Object, Canonical, Error)
				? Canonical
				: FString(Schema);
		}

		void Add(
			TArray<FUeAgentOperationDescriptor>& OutDescriptors,
			const TCHAR* DescriptorId,
			const TCHAR* CapabilityId,
			const TCHAR* ProviderBinding,
			EUeAgentQueryKind QueryKind,
			TArray<EUeAgentStatePlane> StatePlanes,
			const FString& InputSchema,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			if (!OutError.IsEmpty())
			{
				return;
			}
			auto BuildDescriptor = [=](const FString& DescriptorHash)
			{
				return FUeAgentOperationDescriptor(
					FUeAgentDescriptorIdentity(
						DescriptorId,
						1,
						DescriptorHash),
					CapabilityId,
					TEXT("asset_project"),
					EUeAgentDescriptorOperationKind::Query,
					FUeAgentExecutionContextPolicy(
						StatePlanes,
						{},
						EUeAgentContextResolutionPolicy::
							ResolveCurrentThenFreeze,
						EUeAgentTransientContextPolicy::ReadOnly),
					FUeAgentContractValue(CanonicalSchema(*InputSchema)),
					FUeAgentContractValue(TEXT("{\"type\":\"object\"}")),
					FUeAgentQueryBinding(
						ProviderBinding,
						QueryKind,
						EUeAgentQueryMaterializationPolicy::
							RuntimeObserve,
						EUeAgentQuerySideEffectPolicy::
							NoPersistentMutation));
			};
			const FUeAgentOperationDescriptor Draft =
				BuildDescriptor(FString());
			FString DescriptorHash;
			if (!TryComputeOperationDescriptorHash(
					Draft,
					DescriptorHash,
					OutError))
			{
				return;
			}
			OutDescriptors.Add(BuildDescriptor(DescriptorHash));
		}

		TArray<EUeAgentStatePlane> AssetPlanes()
		{
			return {
				EUeAgentStatePlane::AuthoringMemory,
				EUeAgentStatePlane::Persisted,
				EUeAgentStatePlane::CompiledGenerated,
			};
		}

		TArray<EUeAgentStatePlane> AuthoringPersisted()
		{
			return {
				EUeAgentStatePlane::AuthoringMemory,
				EUeAgentStatePlane::Persisted,
			};
		}

		void AddAssetDescriptors(
			TArray<FUeAgentOperationDescriptor>& Descriptors,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			Add(Descriptors, TEXT("uai.query.asset.owned_candidates"), TEXT("asset.owned_candidates.read"),
				TEXT("asset_project.read.asset_owned_candidates"), EUeAgentQueryKind::State,
				{EUeAgentStatePlane::AuthoringMemory},
				TEXT(R"json({"type":"object","properties":{"property_path":{"type":"string","minLength":1},"query":{"type":"string"}},"required":["property_path"],"additionalProperties":false})json"), OutError);
			Add(
				Descriptors,
				TEXT("uai.query.asset.summary"),
				TEXT("asset.summary.read"),
				TEXT("asset_project.read.asset_summary"),
				EUeAgentQueryKind::State,
				AssetPlanes(),
				TEXT(R"json({"type":"object","properties":{"include_class_lineage":{"type":"boolean"},"include_package_state":{"type":"boolean"}},"additionalProperties":false})json"),
				OutError);
			Add(
				Descriptors,
				TEXT("uai.query.asset.properties"),
				TEXT("asset.properties.read"),
				TEXT("asset_project.read.asset_properties"),
				EUeAgentQueryKind::State,
				AssetPlanes(),
				TEXT(R"json({"type":"object","properties":{"field_ids":{"type":"array","items":{"type":"string","minLength":1},"uniqueItems":true},"include_defaults":{"type":"boolean"},"include_presence":{"type":"boolean"}},"additionalProperties":false})json"),
				OutError);
			Add(
				Descriptors,
				TEXT("uai.query.asset.relations"),
				TEXT("asset.relations.read"),
				TEXT("asset_project.read.asset_relations"),
				EUeAgentQueryKind::State,
				{
					EUeAgentStatePlane::AuthoringMemory,
					EUeAgentStatePlane::Persisted,
					EUeAgentStatePlane::RegistryIndex,
				},
				TEXT(R"json({"type":"object","properties":{"direction":{"type":"string","enum":["dependencies","referencers","both"]},"relation_kinds":{"type":"array","items":{"type":"string","enum":["hard","soft","manage","searchable_name"]},"uniqueItems":true},"package_scope":{"type":"string","enum":["all","game","editor_only","build"]}},"required":["direction"],"additionalProperties":false})json"),
				OutError);
			Add(
				Descriptors,
				TEXT("uai.query.asset.validation"),
				TEXT("asset.validation.read"),
				TEXT("asset_project.read.asset_validation"),
				EUeAgentQueryKind::RuntimeProbe,
				AssetPlanes(),
				TEXT(R"json({"type":"object","properties":{"rule_ids":{"type":"array","items":{"type":"string","minLength":1},"uniqueItems":true},"include_passed":{"type":"boolean"}},"additionalProperties":false})json"),
				OutError);
			Add(
				Descriptors,
				TEXT("uai.query.asset.collection"),
				TEXT("asset.collection.read"),
				TEXT("asset_project.read.asset_collection"),
				EUeAgentQueryKind::State,
				AuthoringPersisted(),
				TEXT(R"json({"type":"object","properties":{"collection_kind":{"type":"string","enum":["rows","items","entries"]},"keys":{"type":"array","items":{"type":"string","minLength":1},"uniqueItems":true},"key_prefix":{"type":"string"},"field_ids":{"type":"array","items":{"type":"string","minLength":1},"uniqueItems":true}},"required":["collection_kind"],"additionalProperties":false})json"),
				OutError);
			Add(
				Descriptors,
				TEXT("uai.query.asset.sample"),
				TEXT("asset.sample.read"),
				TEXT("asset_project.read.asset_sample"),
				EUeAgentQueryKind::RuntimeProbe,
				{
					EUeAgentStatePlane::AuthoringMemory,
					EUeAgentStatePlane::Persisted,
					EUeAgentStatePlane::CompiledGenerated,
					EUeAgentStatePlane::RenderedObservation,
				},
				TEXT(R"json({"type":"object","properties":{"sample_kind":{"type":"string","enum":["bounds","vertex"]},"index":{"type":"integer","minimum":0}},"required":["sample_kind"],"additionalProperties":false})json"),
				OutError);
		}

		void AddRegistryEditorDescriptors(
			TArray<FUeAgentOperationDescriptor>& Descriptors,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			Add(
				Descriptors,
				TEXT("uai.query.reference.origin.resolve"),
				TEXT("reference.origin.resolve"),
				TEXT("asset_project.read.reference_origin"),
				EUeAgentQueryKind::Reference,
				AssetPlanes(),
				TEXT(R"json({"type":"object","properties":{"target":{"type":"object","properties":{"source":{"type":"string","enum":["catalog_candidate","json_reference","ue_object","graph_node","name_reference"]},"candidate_id":{"type":"string","minLength":1},"json_file":{"type":"string","minLength":1},"json_pointer":{"type":"string","minLength":1},"kind":{"type":"string","minLength":1},"name":{"type":"string","minLength":1},"full_name":{"type":"string","minLength":1},"asset_path":{"type":"string","minLength":1},"graph_name":{"type":"string","minLength":1},"node_guid":{"type":"string","minLength":1}},"required":["source"],"additionalProperties":false}},"required":["target"],"additionalProperties":false})json"),
				OutError);
			Add(
				Descriptors,
				TEXT("uai.query.reference.read"),
				TEXT("reference.asset.read"),
				TEXT("asset_project.read.reference_asset"),
				EUeAgentQueryKind::State,
				AssetPlanes(),
				TEXT(R"json({"type":"object","properties":{"field_ids":{"type":"array","items":{"type":"string","minLength":1},"uniqueItems":true}},"additionalProperties":false})json"),
				OutError);
			Add(
				Descriptors,
				TEXT("uai.query.asset.registry_record"),
				TEXT("asset.registry_record.read"),
				TEXT("asset_project.read.registry_record"),
				EUeAgentQueryKind::State,
				{EUeAgentStatePlane::RegistryIndex},
				TEXT(R"json({"type":"object","properties":{"include_tags":{"type":"boolean"},"include_chunk_ids":{"type":"boolean"}},"additionalProperties":false})json"),
				OutError);
			Add(
				Descriptors,
				TEXT("uai.query.asset.registry_relations"),
				TEXT("asset.registry_relations.read"),
				TEXT("asset_project.read.registry_relations"),
				EUeAgentQueryKind::State,
				{EUeAgentStatePlane::RegistryIndex},
				TEXT(R"json({"type":"object","properties":{"direction":{"type":"string","enum":["dependencies","referencers","both"]},"relation_kinds":{"type":"array","items":{"type":"string","enum":["hard","soft","manage","searchable_name"]},"uniqueItems":true}},"required":["direction"],"additionalProperties":false})json"),
				OutError);
			Add(
				Descriptors,
				TEXT("uai.query.editor.open_assets"),
				TEXT("editor.open_assets.read"),
				TEXT("asset_project.read.editor_open_assets"),
				EUeAgentQueryKind::RuntimeProbe,
				{EUeAgentStatePlane::AuthoringMemory},
				TEXT(R"json({"type":"object","properties":{"include_editor_identity":{"type":"boolean"}},"additionalProperties":false})json"),
				OutError);
			Add(
				Descriptors,
				TEXT("uai.query.editor.dirty_resources"),
				TEXT("editor.dirty_resources.read"),
				TEXT("asset_project.read.editor_dirty_resources"),
				EUeAgentQueryKind::RuntimeProbe,
				{EUeAgentStatePlane::AuthoringMemory},
				TEXT(R"json({"type":"object","properties":{"resource_kinds":{"type":"array","items":{"type":"string","enum":["asset","map","config"]},"uniqueItems":true}},"additionalProperties":false})json"),
				OutError);
		}

		FString ProjectConfigQuerySchema(const TCHAR* CanonicalInput, const TCHAR* Actions)
		{
			// Canonical reads and capability reads are distinct public shapes.
			// The latter forwards only parameters to its action owner; canonical
			// required fields must never be imposed on that nested request.
			return FString::Printf(
				TEXT(R"json({"oneOf":[%s,{"type":"object","properties":{"editor_project_capability":{"type":"string","enum":%s},"parameters":{"type":"object"}},"required":["editor_project_capability"],"additionalProperties":false}]})json"),
				CanonicalInput, Actions);
		}

		void AddProjectConfigDescriptors(
			TArray<FUeAgentOperationDescriptor>& Descriptors,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			Add(
				Descriptors,
				TEXT("uai.query.project.summary"),
				TEXT("project.summary.read"),
				TEXT("asset_project.read.project_summary"),
				EUeAgentQueryKind::State,
				AuthoringPersisted(),
				TEXT(R"json({"type":"object","properties":{},"additionalProperties":false})json"),
				OutError);
			Add(
				Descriptors,
				TEXT("uai.query.project.settings"),
				TEXT("project.settings.read"),
				TEXT("asset_project.read.project_settings"),
				EUeAgentQueryKind::State,
				AuthoringPersisted(),
				ProjectConfigQuerySchema(
					TEXT(R"json({"type":"object","properties":{"section_id":{"type":"string","minLength":1},"field_ids":{"type":"array","items":{"type":"string","minLength":1},"uniqueItems":true},"include_defaults":{"type":"boolean"}},"required":["section_id"],"additionalProperties":false})json"),
					TEXT(R"json(["project_settings.index","project_settings.page","target.read"])json")),
				OutError);
			Add(
				Descriptors,
				TEXT("uai.query.project.platform"),
				TEXT("project.platform.read"),
				TEXT("asset_project.read.project_platform"),
				EUeAgentQueryKind::State,
				AuthoringPersisted(),
				ProjectConfigQuerySchema(
					TEXT(R"json({"type":"object","properties":{"platform_ids":{"type":"array","items":{"type":"string","minLength":1},"uniqueItems":true},"field_ids":{"type":"array","items":{"type":"string","minLength":1},"uniqueItems":true},"resolve_inheritance":{"type":"boolean"}},"additionalProperties":false})json"),
					TEXT(R"json(["platform_config.export","target_platform.query","target.read"])json")),
				OutError);
			Add(
				Descriptors,
				TEXT("uai.query.project.validation"),
				TEXT("project.validation.read"),
				TEXT("asset_project.read.project_validation"),
				EUeAgentQueryKind::RuntimeProbe,
				AuthoringPersisted(),
				ProjectConfigQuerySchema(
					TEXT(R"json({"type":"object","properties":{"section_ids":{"type":"array","items":{"type":"string","minLength":1},"uniqueItems":true},"rule_ids":{"type":"array","items":{"type":"string","minLength":1},"uniqueItems":true},"include_passed":{"type":"boolean"}},"additionalProperties":false})json"),
					TEXT(R"json(["device_profile.diff","device_profile.validate","platform_config.validate","platform_profile.validate_packaging","project_settings.diff","project_settings.validate","scalability.validate","target.read"])json")),
				OutError);
			Add(
				Descriptors,
				TEXT("uai.query.config.section"),
				TEXT("config.section.read"),
				TEXT("asset_project.read.config_section"),
				EUeAgentQueryKind::State,
				AuthoringPersisted(),
				ProjectConfigQuerySchema(
					TEXT(R"json({"type":"object","properties":{"config_scope":{"type":"string","enum":["engine","game","editor","input","device_profiles","scalability","platform"]},"section_id":{"type":"string","minLength":1},"keys":{"type":"array","items":{"type":"string","minLength":1},"uniqueItems":true}},"required":["config_scope","section_id"],"additionalProperties":false})json"),
					TEXT(R"json(["device_profile.export","device_profile.query","scalability.export","target.read"])json")),
				OutError);
			Add(
				Descriptors,
				TEXT("uai.query.config.resolved_value"),
				TEXT("config.resolved_value.read"),
				TEXT("asset_project.read.config_resolved_value"),
				EUeAgentQueryKind::RuntimeProbe,
				AuthoringPersisted(),
				ProjectConfigQuerySchema(
					TEXT(R"json({"type":"object","properties":{"config_scope":{"type":"string","enum":["engine","game","editor","input","device_profiles","scalability","platform"]},"section_id":{"type":"string","minLength":1},"key":{"type":"string","minLength":1},"platform_id":{"type":"string","minLength":1},"device_profile_id":{"type":"string","minLength":1}},"required":["config_scope","section_id","key"],"additionalProperties":false})json"),
					TEXT(R"json(["device_profile.resolve_cvars","target.read"])json")),
				OutError);
		}
	}

	bool TryCreateAssetProjectReadDescriptorRegistry(
		TSharedPtr<const FUeAgentDescriptorRegistry>& OutRegistry,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		TArray<FUeAgentOperationDescriptor> Descriptors;
		Descriptors.Reserve(18);
		AddAssetDescriptors(Descriptors, OutError);
		AddRegistryEditorDescriptors(Descriptors, OutError);
		AddProjectConfigDescriptors(Descriptors, OutError);
		if (!OutError.IsEmpty())
		{
			OutRegistry.Reset();
			return false;
		}
		return FUeAgentDescriptorRegistry::TryCreate(
			MoveTemp(Descriptors),
			OutRegistry,
			OutError);
	}
}

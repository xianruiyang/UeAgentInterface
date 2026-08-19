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
			FString Error;
			return Json::TryCanonicalize(Object, Canonical, Error)
				? Canonical
				: FString(Schema);
		}

		void Add(
			TArray<FUeAgentOperationDescriptor>& OutDescriptors,
			const TCHAR* DescriptorId,
			const TCHAR* DescriptorHash,
			const TCHAR* CapabilityId,
			const TCHAR* ProviderBinding,
			EUeAgentQueryKind QueryKind,
			TArray<EUeAgentStatePlane> StatePlanes,
			const TCHAR* InputSchema)
		{
			OutDescriptors.Emplace(
				FUeAgentDescriptorIdentity(
					DescriptorId,
					1,
					DescriptorHash),
				CapabilityId,
				TEXT("asset_project"),
				EUeAgentDescriptorOperationKind::Query,
				FUeAgentExecutionContextPolicy(
					MoveTemp(StatePlanes),
					{},
					EUeAgentContextResolutionPolicy::
						ResolveCurrentThenFreeze,
					EUeAgentTransientContextPolicy::ReadOnly),
				FUeAgentContractValue(CanonicalSchema(InputSchema)),
				FUeAgentContractValue(TEXT("{\"type\":\"object\"}")),
				FUeAgentQueryBinding(
					ProviderBinding,
					QueryKind,
					EUeAgentQueryMaterializationPolicy::
						RuntimeObserve,
					EUeAgentQuerySideEffectPolicy::
						NoPersistentMutation));
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
			TArray<FUeAgentOperationDescriptor>& Descriptors)
		{
			Add(
				Descriptors,
				TEXT("uai.query.asset.summary"),
				TEXT("sha256:72a69072c32e4e0e096549473f5607ad3974a0987d557bfd5eea883f2a70e421"),
				TEXT("asset.summary.read"),
				TEXT("asset_project.read.asset_summary"),
				EUeAgentQueryKind::State,
				AssetPlanes(),
				TEXT(R"json({"type":"object","properties":{"include_class_lineage":{"type":"boolean"},"include_package_state":{"type":"boolean"}},"additionalProperties":false})json"));
			Add(
				Descriptors,
				TEXT("uai.query.asset.properties"),
				TEXT("sha256:72338ed09b27f39027ca1ff2850c3a1c22b06cfb5c01e823bb6d0b689ddfc71d"),
				TEXT("asset.properties.read"),
				TEXT("asset_project.read.asset_properties"),
				EUeAgentQueryKind::State,
				AssetPlanes(),
				TEXT(R"json({"type":"object","properties":{"field_ids":{"type":"array","items":{"type":"string","minLength":1},"uniqueItems":true},"include_defaults":{"type":"boolean"},"include_presence":{"type":"boolean"}},"additionalProperties":false})json"));
			Add(
				Descriptors,
				TEXT("uai.query.asset.relations"),
				TEXT("sha256:4b2a31dd853f8eeac4d960b82ddf0abf3caeb1b02596daabb0212821d5880e6f"),
				TEXT("asset.relations.read"),
				TEXT("asset_project.read.asset_relations"),
				EUeAgentQueryKind::State,
				{
					EUeAgentStatePlane::AuthoringMemory,
					EUeAgentStatePlane::Persisted,
					EUeAgentStatePlane::RegistryIndex,
				},
				TEXT(R"json({"type":"object","properties":{"direction":{"type":"string","enum":["dependencies","referencers","both"]},"relation_kinds":{"type":"array","items":{"type":"string","enum":["hard","soft","manage","searchable_name"]},"uniqueItems":true}},"required":["direction"],"additionalProperties":false})json"));
			Add(
				Descriptors,
				TEXT("uai.query.asset.validation"),
				TEXT("sha256:c90ef3c9048489bd53d93336c5582fcf6499bc7809a76a8ec3cc456ad75a483f"),
				TEXT("asset.validation.read"),
				TEXT("asset_project.read.asset_validation"),
				EUeAgentQueryKind::RuntimeProbe,
				AssetPlanes(),
				TEXT(R"json({"type":"object","properties":{"rule_ids":{"type":"array","items":{"type":"string","minLength":1},"uniqueItems":true},"include_passed":{"type":"boolean"}},"additionalProperties":false})json"));
			Add(
				Descriptors,
				TEXT("uai.query.asset.collection"),
				TEXT("sha256:0a3259ac7aa3d9d1e654108ad4b72becb6119d0dd9f82f11cbee50818796f113"),
				TEXT("asset.collection.read"),
				TEXT("asset_project.read.asset_collection"),
				EUeAgentQueryKind::State,
				AuthoringPersisted(),
				TEXT(R"json({"type":"object","properties":{"collection_kind":{"type":"string","enum":["rows","items","entries"]},"keys":{"type":"array","items":{"type":"string","minLength":1},"uniqueItems":true},"key_prefix":{"type":"string"},"field_ids":{"type":"array","items":{"type":"string","minLength":1},"uniqueItems":true}},"required":["collection_kind"],"additionalProperties":false})json"));
			Add(
				Descriptors,
				TEXT("uai.query.asset.sample"),
				TEXT("sha256:a21954ee8e211316c8a728505d203333e173bf63d8cfb2e68eabe67c5f355f79"),
				TEXT("asset.sample.read"),
				TEXT("asset_project.read.asset_sample"),
				EUeAgentQueryKind::RuntimeProbe,
				{
					EUeAgentStatePlane::AuthoringMemory,
					EUeAgentStatePlane::Persisted,
					EUeAgentStatePlane::CompiledGenerated,
					EUeAgentStatePlane::RenderedObservation,
				},
				TEXT(R"json({"type":"object","properties":{"sample_kind":{"type":"string","enum":["pixel","uv","vertex","bounds","preview_state"]},"coordinates":{"type":"array","items":{"type":"number"}},"index":{"type":"integer","minimum":0},"channel":{"type":"string","minLength":1}},"required":["sample_kind"],"additionalProperties":false})json"));
		}

		void AddRegistryEditorDescriptors(
			TArray<FUeAgentOperationDescriptor>& Descriptors)
		{
			Add(
				Descriptors,
				TEXT("uai.query.reference.origin.resolve"),
				TEXT("sha256:c03a818f38e2405c6b73af0b5e5ab2eb8aea0671a4cc051fd2dee07e71d36296"),
				TEXT("reference.origin.resolve"),
				TEXT("asset_project.read.reference_origin"),
				EUeAgentQueryKind::Reference,
				AssetPlanes(),
				TEXT(R"json({"type":"object","properties":{"target":{"type":"object","properties":{"source":{"type":"string","enum":["catalog_candidate","json_reference","ue_object","graph_node","name_reference"]},"candidate_id":{"type":"string","minLength":1},"json_file":{"type":"string","minLength":1},"json_pointer":{"type":"string","minLength":1},"kind":{"type":"string","minLength":1},"name":{"type":"string","minLength":1},"full_name":{"type":"string","minLength":1},"asset_path":{"type":"string","minLength":1},"graph_name":{"type":"string","minLength":1},"node_guid":{"type":"string","minLength":1}},"required":["source"],"additionalProperties":false}},"required":["target"],"additionalProperties":false})json"));
			Add(
				Descriptors,
				TEXT("uai.query.asset.registry_record"),
				TEXT("sha256:1395062a9c39e7bfecf28142732c13c126aaff4d49568c5aefe6df02dddf659f"),
				TEXT("asset.registry_record.read"),
				TEXT("asset_project.read.registry_record"),
				EUeAgentQueryKind::State,
				{EUeAgentStatePlane::RegistryIndex},
				TEXT(R"json({"type":"object","properties":{"include_tags":{"type":"boolean"},"include_chunk_ids":{"type":"boolean"}},"additionalProperties":false})json"));
			Add(
				Descriptors,
				TEXT("uai.query.asset.registry_relations"),
				TEXT("sha256:5615497f2b41bcb8a3d0d2185e07030c65b66607b9c3e3f471ebabb88ba00737"),
				TEXT("asset.registry_relations.read"),
				TEXT("asset_project.read.registry_relations"),
				EUeAgentQueryKind::State,
				{EUeAgentStatePlane::RegistryIndex},
				TEXT(R"json({"type":"object","properties":{"direction":{"type":"string","enum":["dependencies","referencers","both"]},"relation_kinds":{"type":"array","items":{"type":"string","enum":["hard","soft","manage","searchable_name"]},"uniqueItems":true}},"required":["direction"],"additionalProperties":false})json"));
			Add(
				Descriptors,
				TEXT("uai.query.editor.open_assets"),
				TEXT("sha256:793d8c0fdffc117fc18cfb2f42e6abd4649ec07e67e6610c2216616ea4eb3096"),
				TEXT("editor.open_assets.read"),
				TEXT("asset_project.read.editor_open_assets"),
				EUeAgentQueryKind::RuntimeProbe,
				{EUeAgentStatePlane::AuthoringMemory},
				TEXT(R"json({"type":"object","properties":{"include_editor_identity":{"type":"boolean"}},"additionalProperties":false})json"));
			Add(
				Descriptors,
				TEXT("uai.query.editor.dirty_resources"),
				TEXT("sha256:d600c586edc61834d732ba941f51a01f9f35e5b559b224758e0e0b47afdaf8ce"),
				TEXT("editor.dirty_resources.read"),
				TEXT("asset_project.read.editor_dirty_resources"),
				EUeAgentQueryKind::RuntimeProbe,
				{EUeAgentStatePlane::AuthoringMemory},
				TEXT(R"json({"type":"object","properties":{"resource_kinds":{"type":"array","items":{"type":"string","enum":["asset","map","config"]},"uniqueItems":true}},"additionalProperties":false})json"));
		}

		void AddProjectConfigDescriptors(
			TArray<FUeAgentOperationDescriptor>& Descriptors)
		{
			Add(
				Descriptors,
				TEXT("uai.query.project.summary"),
				TEXT("sha256:2946f37f18ce8b1ba614fc9cfed3ca821b07685d51e4aecfc7cf2eda2b508825"),
				TEXT("project.summary.read"),
				TEXT("asset_project.read.project_summary"),
				EUeAgentQueryKind::State,
				AuthoringPersisted(),
				TEXT(R"json({"type":"object","properties":{},"additionalProperties":false})json"));
			Add(
				Descriptors,
				TEXT("uai.query.project.settings"),
				TEXT("sha256:2c0ac63d822ba9d7c8edcaa7380e5b1e15a8617821fc389e10bec1c338f74b32"),
				TEXT("project.settings.read"),
				TEXT("asset_project.read.project_settings"),
				EUeAgentQueryKind::State,
				AuthoringPersisted(),
				TEXT(R"json({"type":"object","properties":{"section_id":{"type":"string","minLength":1},"field_ids":{"type":"array","items":{"type":"string","minLength":1},"uniqueItems":true},"include_defaults":{"type":"boolean"},"editor_project_capability":{"type":"string","enum":["project_settings.index","project_settings.page","target.read"]},"parameters":{"type":"object"}},"required":["section_id"],"additionalProperties":false})json"));
			Add(
				Descriptors,
				TEXT("uai.query.project.platform"),
				TEXT("sha256:76f2a14fe537aad20734c81d097865d374d169256f2102d24412f92f185b5d85"),
				TEXT("project.platform.read"),
				TEXT("asset_project.read.project_platform"),
				EUeAgentQueryKind::State,
				AuthoringPersisted(),
				TEXT(R"json({"type":"object","properties":{"platform_ids":{"type":"array","items":{"type":"string","minLength":1},"uniqueItems":true},"field_ids":{"type":"array","items":{"type":"string","minLength":1},"uniqueItems":true},"resolve_inheritance":{"type":"boolean"},"editor_project_capability":{"type":"string","enum":["platform_config.export","target_platform.query","target.read"]},"parameters":{"type":"object"}},"additionalProperties":false})json"));
			Add(
				Descriptors,
				TEXT("uai.query.project.validation"),
				TEXT("sha256:432978314fe69e5bf01391989a7ee11b93851e91f25bf8268f29a19951062734"),
				TEXT("project.validation.read"),
				TEXT("asset_project.read.project_validation"),
				EUeAgentQueryKind::RuntimeProbe,
				AuthoringPersisted(),
				TEXT(R"json({"type":"object","properties":{"section_ids":{"type":"array","items":{"type":"string","minLength":1},"uniqueItems":true},"rule_ids":{"type":"array","items":{"type":"string","minLength":1},"uniqueItems":true},"include_passed":{"type":"boolean"},"editor_project_capability":{"type":"string","enum":["device_profile.diff","device_profile.validate","platform_config.validate","platform_profile.validate_packaging","project_settings.diff","project_settings.validate","scalability.validate","target.read"]},"parameters":{"type":"object"}},"additionalProperties":false})json"));
			Add(
				Descriptors,
				TEXT("uai.query.config.section"),
				TEXT("sha256:17b868e7323148560ebdd2900b2fe8ecb085c5bf37fb0b81015e90de8a60bc00"),
				TEXT("config.section.read"),
				TEXT("asset_project.read.config_section"),
				EUeAgentQueryKind::State,
				AuthoringPersisted(),
				TEXT(R"json({"type":"object","properties":{"config_scope":{"type":"string","enum":["engine","game","editor","input","device_profiles","scalability","platform"]},"section_id":{"type":"string","minLength":1},"keys":{"type":"array","items":{"type":"string","minLength":1},"uniqueItems":true},"editor_project_capability":{"type":"string","enum":["device_profile.export","device_profile.query","scalability.export","target.read"]},"parameters":{"type":"object"}},"required":["config_scope","section_id"],"additionalProperties":false})json"));
			Add(
				Descriptors,
				TEXT("uai.query.config.resolved_value"),
				TEXT("sha256:38cd6d7c152d23ca33d3b19f47e12d8c73570d91c4f9cbbd353dff7bf81de388"),
				TEXT("config.resolved_value.read"),
				TEXT("asset_project.read.config_resolved_value"),
				EUeAgentQueryKind::RuntimeProbe,
				AuthoringPersisted(),
				TEXT(R"json({"type":"object","properties":{"config_scope":{"type":"string","enum":["engine","game","editor","input","device_profiles","scalability","platform"]},"section_id":{"type":"string","minLength":1},"key":{"type":"string","minLength":1},"platform_id":{"type":"string","minLength":1},"device_profile_id":{"type":"string","minLength":1},"editor_project_capability":{"type":"string","enum":["device_profile.resolve_cvars","target.read"]},"parameters":{"type":"object"}},"required":["config_scope","section_id","key"],"additionalProperties":false})json"));
		}
	}

	bool TryCreateAssetProjectReadDescriptorRegistry(
		TSharedPtr<const FUeAgentDescriptorRegistry>& OutRegistry,
		FString& OutError)
	{
		TArray<FUeAgentOperationDescriptor> Descriptors;
		Descriptors.Reserve(17);
		AddAssetDescriptors(Descriptors);
		AddRegistryEditorDescriptors(Descriptors);
		AddProjectConfigDescriptors(Descriptors);
		if (Descriptors.Num() != 17)
		{
			OutRegistry.Reset();
			OutError =
				TEXT("asset_project_read_descriptor_count_invalid");
			return false;
		}
		return FUeAgentDescriptorRegistry::TryCreate(
			MoveTemp(Descriptors),
			OutRegistry,
			OutError);
	}
}

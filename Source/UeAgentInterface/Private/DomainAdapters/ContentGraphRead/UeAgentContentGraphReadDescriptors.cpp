#include "DomainAdapters/ContentGraphRead/UeAgentContentGraphReadDescriptors.h"

#include "Core/Contract/UeAgentContractJson.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace UE::UAI::DomainAdapters::ContentGraphRead
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

		TArray<EUeAgentStatePlane> GraphPlanes()
		{
			return {
				EUeAgentStatePlane::AuthoringMemory,
				EUeAgentStatePlane::Persisted,
				EUeAgentStatePlane::CompiledGenerated,
			};
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
				TEXT("content_graph"),
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
	}

	bool TryCreateContentGraphReadDescriptorRegistry(
		TSharedPtr<const FUeAgentDescriptorRegistry>& OutRegistry,
		FString& OutError)
	{
		TArray<FUeAgentOperationDescriptor> Descriptors;
		Descriptors.Reserve(7);
		Add(
			Descriptors,
			TEXT("uai.query.content_graph.summary"),
			TEXT("sha256:fcc5a16effcca7382ddbc71c03d1b0c6e700a7e5b2883ca882fb5efd80cce9f4"),
			TEXT("content_graph.summary.read"),
			TEXT("content_graph.read.summary"),
			EUeAgentQueryKind::State,
			GraphPlanes(),
			TEXT(R"json({"type":"object","properties":{"include_counts":{"type":"boolean"},"include_revision":{"type":"boolean"}},"additionalProperties":false})json"));
		Add(
			Descriptors,
			TEXT("uai.query.content_graph.structure"),
			TEXT("sha256:ba7bb17241770e90dec4b09e08239334cd95c055e7afe19b9f03ff13a933708c"),
			TEXT("content_graph.structure.read"),
			TEXT("content_graph.read.structure"),
			EUeAgentQueryKind::State,
			GraphPlanes(),
			TEXT(R"json({"type":"object","properties":{"container_ids":{"type":"array","items":{"type":"string","minLength":1},"uniqueItems":true},"include_hierarchy":{"type":"boolean"},"include_order":{"type":"boolean"}},"additionalProperties":false})json"));
		Add(
			Descriptors,
			TEXT("uai.query.content_graph.items"),
			TEXT("sha256:6145ef33779b3e0889ae2757c1e6856b730855c4d2bef7ab01a9b3d6bd49de4a"),
			TEXT("content_graph.items.read"),
			TEXT("content_graph.read.items"),
			EUeAgentQueryKind::State,
			GraphPlanes(),
			TEXT(R"json({"type":"object","properties":{"item_ids":{"type":"array","items":{"type":"string","minLength":1},"uniqueItems":true},"item_kinds":{"type":"array","items":{"type":"string","minLength":1},"uniqueItems":true},"field_ids":{"type":"array","items":{"type":"string","minLength":1},"uniqueItems":true}},"additionalProperties":false})json"));
		Add(
			Descriptors,
			TEXT("uai.query.content_graph.relations"),
			TEXT("sha256:86eaa07f9bd79db91a86a209105b55cf77931403ede9ac684f488b07acfcad00"),
			TEXT("content_graph.relations.read"),
			TEXT("content_graph.read.relations"),
			EUeAgentQueryKind::State,
			GraphPlanes(),
			TEXT(R"json({"type":"object","properties":{"relation_ids":{"type":"array","items":{"type":"string","minLength":1},"uniqueItems":true},"endpoint_ids":{"type":"array","items":{"type":"string","minLength":1},"uniqueItems":true},"relation_kinds":{"type":"array","items":{"type":"string","enum":["connection","parent","membership","order","reference","transition"]},"uniqueItems":true}},"additionalProperties":false})json"));
		Add(
			Descriptors,
			TEXT("uai.query.content_graph.properties"),
			TEXT("sha256:58aae781d0bec72a562d3803dfb7f5a5ef1b729324ede7fa04e7fda3ca7a36b9"),
			TEXT("content_graph.properties.read"),
			TEXT("content_graph.read.properties"),
			EUeAgentQueryKind::State,
			GraphPlanes(),
			TEXT(R"json({"type":"object","properties":{"owner_ids":{"type":"array","items":{"type":"string","minLength":1},"uniqueItems":true},"field_ids":{"type":"array","items":{"type":"string","minLength":1},"uniqueItems":true},"include_defaults":{"type":"boolean"},"include_presence":{"type":"boolean"}},"required":["owner_ids"],"additionalProperties":false})json"));
		Add(
			Descriptors,
			TEXT("uai.query.content_graph.diagnostics"),
			TEXT("sha256:ad9f63b7aa89735eac4ddff4816e8b389980d7ab1c53e79faeabc0db4db4cf06"),
			TEXT("content_graph.diagnostics.read"),
			TEXT("content_graph.read.diagnostics"),
			EUeAgentQueryKind::RuntimeProbe,
			{
				EUeAgentStatePlane::AuthoringMemory,
				EUeAgentStatePlane::CompiledGenerated,
			},
			TEXT(R"json({"type":"object","properties":{"severity":{"type":"array","items":{"type":"string","enum":["info","warning","error"]},"uniqueItems":true},"include_resolved":{"type":"boolean"}},"additionalProperties":false})json"));
		Add(
			Descriptors,
			TEXT("uai.query.content_graph.compound"),
			TEXT("sha256:18e5a1357c045a15637fce2f530a7b1d337a5b6cd8d2aac08c958e106d810065"),
			TEXT("content_graph.compound.read"),
			TEXT("content_graph.read.compound"),
			EUeAgentQueryKind::State,
			GraphPlanes(),
			TEXT(R"json({"type":"object","properties":{"sections":{"type":"array","items":{"type":"string","enum":["summary","structure","items","relations","properties","parameters","diagnostics"]},"uniqueItems":true},"root_ids":{"type":"array","items":{"type":"string","minLength":1},"uniqueItems":true},"include_closure":{"type":"boolean"},"localization_capability":{"type":"string","enum":["localization.diff","localization.target.read","localization.culture.read","localization.query_targets","localization.report","localization.validate","string_table.diff","string_table.validate"]},"parameters":{"type":"object"}},"additionalProperties":false})json"));

		if (Descriptors.Num() != 7)
		{
			OutRegistry.Reset();
			OutError =
				TEXT("content_graph_read_descriptor_count_invalid");
			return false;
		}
		return FUeAgentDescriptorRegistry::TryCreate(
			MoveTemp(Descriptors),
			OutRegistry,
			OutError);
	}
}

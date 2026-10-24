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
			UE::UAI::Contract::FDiagnostic Error;
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
			const TCHAR* CapabilityId,
			const TCHAR* ProviderBinding,
			EUeAgentQueryKind QueryKind,
			TArray<EUeAgentStatePlane> StatePlanes,
			const TCHAR* InputSchema,
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
				TEXT("content_graph"),
				EUeAgentDescriptorOperationKind::Query,
				FUeAgentExecutionContextPolicy(
					StatePlanes,
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
	}

	bool TryCreateContentGraphReadDescriptorRegistry(
		TSharedPtr<const FUeAgentDescriptorRegistry>& OutRegistry,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		TArray<FUeAgentOperationDescriptor> Descriptors;
		Descriptors.Reserve(7);
		Add(
			Descriptors,
			TEXT("uai.query.content_graph.summary"),
			TEXT("content_graph.summary.read"),
			TEXT("content_graph.read.summary"),
			EUeAgentQueryKind::State,
			GraphPlanes(),
			TEXT(R"json({"type":"object","properties":{"include_counts":{"type":"boolean"},"include_revision":{"type":"boolean"}},"additionalProperties":false})json"),
			OutError);
		Add(
			Descriptors,
			TEXT("uai.query.content_graph.structure"),
			TEXT("content_graph.structure.read"),
			TEXT("content_graph.read.structure"),
			EUeAgentQueryKind::State,
			GraphPlanes(),
			TEXT(R"json({"type":"object","properties":{"containers":{"type":"array","items":{"type":"string","minLength":1},"uniqueItems":true},"include_hierarchy":{"type":"boolean"},"include_order":{"type":"boolean"}},"additionalProperties":false})json"),
			OutError);
		Add(
			Descriptors,
			TEXT("uai.query.content_graph.items"),
			TEXT("content_graph.items.read"),
			TEXT("content_graph.read.items"),
			EUeAgentQueryKind::State,
			GraphPlanes(),
			TEXT(R"json({"type":"object","properties":{"containers":{"type":"array","items":{"type":"string","minLength":1},"uniqueItems":true},"items":{"type":"array","items":{"type":"string","minLength":1},"uniqueItems":true},"item_kinds":{"type":"array","items":{"type":"string","minLength":1},"uniqueItems":true},"fields":{"type":"array","items":{"type":"string","minLength":1},"uniqueItems":true}},"additionalProperties":false})json"),
			OutError);
		Add(
			Descriptors,
			TEXT("uai.query.content_graph.relations"),
			TEXT("content_graph.relations.read"),
			TEXT("content_graph.read.relations"),
			EUeAgentQueryKind::State,
			GraphPlanes(),
			TEXT(R"json({"type":"object","properties":{"containers":{"type":"array","items":{"type":"string","minLength":1},"uniqueItems":true},"endpoints":{"type":"array","items":{"type":"string","minLength":1},"uniqueItems":true},"relation_kinds":{"type":"array","items":{"type":"string","enum":["connection","parent","membership","order","reference","transition"]},"uniqueItems":true}},"additionalProperties":false})json"),
			OutError);
		Add(
			Descriptors,
			TEXT("uai.query.content_graph.properties"),
			TEXT("content_graph.properties.read"),
			TEXT("content_graph.read.properties"),
			EUeAgentQueryKind::State,
			GraphPlanes(),
			TEXT(R"json({"type":"object","properties":{"containers":{"type":"array","items":{"type":"string","minLength":1},"uniqueItems":true},"owners":{"type":"array","items":{"type":"string","minLength":1},"uniqueItems":true},"fields":{"type":"array","items":{"type":"string","minLength":1},"uniqueItems":true},"include_defaults":{"type":"boolean"},"include_presence":{"type":"boolean"}},"required":["owners"],"additionalProperties":false})json"),
			OutError);
		Add(
			Descriptors,
			TEXT("uai.query.content_graph.diagnostics"),
			TEXT("content_graph.diagnostics.read"),
			TEXT("content_graph.read.diagnostics"),
			EUeAgentQueryKind::RuntimeProbe,
			{
				EUeAgentStatePlane::AuthoringMemory,
				EUeAgentStatePlane::CompiledGenerated,
			},
			TEXT(R"json({"type":"object","properties":{"severity":{"type":"array","items":{"type":"string","enum":["info","warning","error"]},"uniqueItems":true},"include_resolved":{"type":"boolean"}},"additionalProperties":false})json"),
			OutError);
		Add(
			Descriptors,
			TEXT("uai.query.content_graph.compound"),
			TEXT("content_graph.compound.read"),
			TEXT("content_graph.read.compound"),
			EUeAgentQueryKind::State,
			GraphPlanes(),
			TEXT(R"json({"type":"object","properties":{"sections":{"type":"array","items":{"type":"string","enum":["summary","structure","items","relations","properties","parameters","diagnostics"]},"uniqueItems":true},"roots":{"type":"array","items":{"type":"string","minLength":1},"uniqueItems":true},"include_closure":{"type":"boolean"},"localization_capability":{"type":"string","enum":["localization.diff","localization.target.read","localization.culture.read","localization.query_targets","localization.report","localization.validate","string_table.diff","string_table.validate"]},"parameters":{"type":"object"}},"additionalProperties":false})json"),
			OutError);

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

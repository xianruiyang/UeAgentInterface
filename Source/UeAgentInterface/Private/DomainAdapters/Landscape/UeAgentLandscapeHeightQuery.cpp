#include "DomainAdapters/Landscape/UeAgentLandscapeHeightQuery.h"

#include "Core/Contract/UeAgentContractJson.h"
#include "DomainAdapters/WorldRead/UeAgentWorldReadNative.h"
#include "DomainAdapters/WorldRead/UeAgentWorldReadProductionEnvironment.h"
#include "DomainAdapters/WorldRead/UeAgentWorldReadProvider.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace UE::UAI::DomainAdapters::Landscape
{
	using namespace BindingGateway::Query;
	using namespace Contract;

	namespace
	{
		const FString ProviderBinding(TEXT("landscape.read.height"));

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
	}

	const FString& GetLandscapeHeightReadProviderBinding()
	{
		return ProviderBinding;
	}

	bool TryCreateLandscapeHeightReadDescriptorRegistry(
		TSharedPtr<const FUeAgentDescriptorRegistry>& OutRegistry,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		const FString InputSchema = CanonicalSchema(
			TEXT(R"json({"type":"object","properties":{"region":{"type":"object","properties":{"frame_id":{"const":"ue.landscape.local_sample_grid"},"min_inclusive":{"type":"array","items":{"type":"integer"},"minItems":2,"maxItems":2},"max_inclusive":{"type":"array","items":{"type":"integer"},"minItems":2,"maxItems":2}},"required":["frame_id","min_inclusive","max_inclusive"],"additionalProperties":false},"space_contract":{"type":"object","properties":{"frame_id":{"const":"ue.landscape.local_sample_grid"},"unit":{"const":"sample"},"basis_evidence":{"type":"string","minLength":1}},"required":["frame_id","unit","basis_evidence"],"additionalProperties":false}},"required":["region","space_contract"],"additionalProperties":false})json"));
		auto BuildDescriptor = [&InputSchema](const FString& DescriptorHash)
		{
			return FUeAgentOperationDescriptor(
				FUeAgentDescriptorIdentity(
					TEXT("uai.query.landscape.height.read"),
					1,
					DescriptorHash),
				TEXT("landscape.height.read"),
				TEXT("landscape"),
				EUeAgentDescriptorOperationKind::Query,
				FUeAgentExecutionContextPolicy(
					{EUeAgentStatePlane::AuthoringMemory},
					{EUeAgentExecutionContextFacet::World},
					EUeAgentContextResolutionPolicy::
						ResolveCurrentThenFreeze,
					EUeAgentTransientContextPolicy::ReadOnly),
				FUeAgentContractValue(InputSchema),
				FUeAgentContractValue(TEXT("{\"type\":\"object\"}")),
				FUeAgentQueryBinding(
					ProviderBinding,
					EUeAgentQueryKind::State,
					EUeAgentQueryMaterializationPolicy::RuntimeObserve,
					EUeAgentQuerySideEffectPolicy::NoPersistentMutation));
		};
		const FUeAgentOperationDescriptor Draft = BuildDescriptor(FString());
		FString DescriptorHash;
		if (!TryComputeOperationDescriptorHash(
				Draft,
				DescriptorHash,
				OutError))
		{
			return false;
		}
		TArray<FUeAgentOperationDescriptor> Descriptors;
		Descriptors.Add(BuildDescriptor(DescriptorHash));
		return FUeAgentDescriptorRegistry::TryCreate(
			MoveTemp(Descriptors),
			OutRegistry,
			OutError);
	}

	bool TryCreateLandscapeHeightReadProviderRegistrations(
		TArray<FUeAgentDomainQueryProviderRegistration>&
			OutRegistrations,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutRegistrations.Reset();
		OutError.Reset();
		const TSharedRef<
			WorldRead::FUeAgentWorldReadProductionEnvironment>
			Environment = MakeShared<
				WorldRead::FUeAgentWorldReadProductionEnvironment>();
		TSharedPtr<WorldRead::IUeAgentWorldReadHook> Hook;
		WorldRead::FUeAgentWorldReadError HookError;
		if (!WorldRead::FUeAgentWorldReadNativeHookFactory::TryCreate(
				ProviderBinding,
				Environment,
				Hook,
				HookError)
			|| !Hook.IsValid())
		{
			OutError = HookError;
			return false;
		}
		OutRegistrations.Emplace(
			ProviderBinding,
			MakeShared<WorldRead::FUeAgentWorldReadProvider>(
				ProviderBinding,
				Hook.ToSharedRef()));
		return true;
	}
}

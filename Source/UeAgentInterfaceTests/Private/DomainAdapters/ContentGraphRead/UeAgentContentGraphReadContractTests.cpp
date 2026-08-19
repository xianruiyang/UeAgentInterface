#include "Misc/AutomationTest.h"

#include "Core/Contract/UeAgentContractJson.h"
#include "Core/Contract/UeAgentContractSchema.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#include "DomainAdapters/ContentGraphRead/UeAgentContentGraphReadDescriptors.h"

namespace
{
	using namespace UE::UAI::Contract;

	FString FixturePath()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(
			FPaths::ProjectPluginsDir(),
			TEXT("UeAgentInterface/Source/UeAgentInterfaceTests/Private/"
				"DomainAdapters/ContentGraphRead/Fixtures/"
				"content_graph_read_contract_cases.json")));
	}

	TSharedPtr<FJsonObject> LoadFixture(FAutomationTestBase* Test)
	{
		FString Text;
		if (!Test->TestTrue(
				TEXT("load ContentGraphRead contract fixture"),
				FFileHelper::LoadFileToString(Text, *FixturePath())))
		{
			return nullptr;
		}
		TSharedPtr<FJsonObject> Fixture;
		const TSharedRef<TJsonReader<>> Reader =
			TJsonReaderFactory<>::Create(Text);
		if (!Test->TestTrue(
				TEXT("parse ContentGraphRead contract fixture"),
				FJsonSerializer::Deserialize(Reader, Fixture)
					&& Fixture.IsValid()))
		{
			return nullptr;
		}
		return Fixture;
	}

	bool IsForbiddenInputField(const FString& Field)
	{
		static const TSet<FString> Forbidden = {
			TEXT("command"), TEXT("command_id"), TEXT("route"),
			TEXT("provider_binding"), TEXT("selector"), TEXT("http"),
			TEXT("output_file"), TEXT("output_folder"),
			TEXT("input_file"), TEXT("input_folder"),
			TEXT("token_budget"), TEXT("limit"), TEXT("max_items"),
			TEXT("max_hits"), TEXT("max_samples"), TEXT("expected"),
			TEXT("desired"), TEXT("json"), TEXT("json_file"),
		};
		return Forbidden.Contains(Field.ToLower());
	}

	bool ValidateInputOwnership(
		FAutomationTestBase* Test,
		const TSharedPtr<FJsonObject>& Schema,
		const FString& DescriptorId,
		const FString& Path)
	{
		if (!Schema.IsValid())
		{
			Test->AddError(FString::Printf(
				TEXT("content_graph_read_schema_missing:%s:%s"),
				*DescriptorId,
				*Path));
			return false;
		}
		bool bValid = true;
		const TSharedPtr<FJsonObject>* Properties = nullptr;
		if (Schema->TryGetObjectField(TEXT("properties"), Properties)
			&& Properties
			&& Properties->IsValid())
		{
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair
				: (*Properties)->Values)
			{
				if (IsForbiddenInputField(Pair.Key))
				{
					Test->AddError(FString::Printf(
						TEXT("content_graph_read_cross_layer_input:"
							"%s:%s/%s"),
						*DescriptorId,
						*Path,
						*Pair.Key));
					bValid = false;
				}
				if (Pair.Value.IsValid()
					&& Pair.Value->Type == EJson::Object)
				{
					bValid = ValidateInputOwnership(
						Test,
						Pair.Value->AsObject(),
						DescriptorId,
						Path + TEXT("/") + Pair.Key)
						&& bValid;
				}
			}
		}
		const TSharedPtr<FJsonObject>* Items = nullptr;
		if (Schema->TryGetObjectField(TEXT("items"), Items)
			&& Items
			&& Items->IsValid())
		{
			bValid = ValidateInputOwnership(
				Test,
				*Items,
				DescriptorId,
				Path + TEXT("/items"))
				&& bValid;
		}
		return bValid;
	}

	bool CompileSchema(
		FAutomationTestBase* Test,
		const TSharedPtr<FJsonObject>& Schema,
		const FString& DescriptorId,
		const TCHAR* Kind)
	{
		FString Canonical;
		FString JsonError;
		if (!Test->TestTrue(
				*FString::Printf(
					TEXT("%s %s canonicalizes"),
					*DescriptorId,
					Kind),
				Json::TryCanonicalize(Schema, Canonical, JsonError)))
		{
			return false;
		}
		TSharedPtr<const FUeAgentContractSchema> Compiled;
		FUeAgentContractSchemaError SchemaError;
		return Test->TestTrue(
			*FString::Printf(
				TEXT("%s %s compiles"),
				*DescriptorId,
				Kind),
			FUeAgentContractSchema::TryCompile(
				FUeAgentContractValue(Canonical),
				Compiled,
				SchemaError))
			&& Compiled.IsValid();
	}

	TArray<TSharedPtr<FJsonValue>> SortedValues(
		const TSharedPtr<FJsonObject>& Row,
		const TCHAR* Field)
	{
		TArray<FString> Strings;
		for (const TSharedPtr<FJsonValue>& Value
			: Row->GetArrayField(Field))
		{
			Strings.Add(Value->AsString());
		}
		Strings.Sort();
		TArray<TSharedPtr<FJsonValue>> Values;
		for (const FString& String : Strings)
		{
			Values.Add(MakeShared<FJsonValueString>(String));
		}
		return Values;
	}

	FString ComputeHash(
		FAutomationTestBase* Test,
		const TSharedPtr<FJsonObject>& Row)
	{
		const TSharedPtr<FJsonObject> Policy =
			MakeShared<FJsonObject>();
		Policy->SetArrayField(
			TEXT("allowed_state_planes"),
			SortedValues(Row, TEXT("allowed_state_planes")));
		Policy->SetArrayField(
			TEXT("required_facets"),
			SortedValues(Row, TEXT("required_context_facets")));
		Policy->SetStringField(
			TEXT("resolution_policy"),
			TEXT("resolve_current_then_freeze"));
		Policy->SetStringField(
			TEXT("transient_access_policy"),
			TEXT("read_only"));

		const TSharedPtr<FJsonObject> Query =
			MakeShared<FJsonObject>();
		Query->SetStringField(
			TEXT("materialization_policy"),
			TEXT("runtime_observe"));
		Query->SetStringField(
			TEXT("provider_binding"),
			Row->GetStringField(TEXT("provider_binding")));
		Query->SetStringField(
			TEXT("query_kind"),
			Row->GetStringField(TEXT("query_kind")));
		Query->SetStringField(
			TEXT("side_effect_policy"),
			TEXT("no_persistent_mutation"));

		const TSharedPtr<FJsonObject> Descriptor =
			MakeShared<FJsonObject>();
		Descriptor->SetStringField(
			TEXT("capability_id"),
			Row->GetStringField(TEXT("capability_id")));
		Descriptor->SetStringField(
			TEXT("descriptor_id"),
			Row->GetStringField(TEXT("descriptor_id")));
		Descriptor->SetStringField(TEXT("domain_id"), TEXT("content_graph"));
		Descriptor->SetObjectField(TEXT("execution_context_policy"), Policy);
		Descriptor->SetObjectField(
			TEXT("input_schema"),
			Row->GetObjectField(TEXT("input_schema")));
		Descriptor->SetStringField(TEXT("operation_kind"), TEXT("query"));
		Descriptor->SetObjectField(TEXT("query"), Query);
		Descriptor->SetObjectField(
			TEXT("result_schema"),
			Row->GetObjectField(TEXT("result_schema")));
		Descriptor->SetNumberField(
			TEXT("version"),
			Row->GetNumberField(TEXT("version")));

		FString Canonical;
		FString Error;
		if (!Test->TestTrue(
				TEXT("canonical descriptor"),
				Json::TryCanonicalize(Descriptor, Canonical, Error)))
		{
			return FString();
		}
		return Json::Sha256Utf8(Canonical);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentContentGraphReadContractFixtureTest,
	"UeAgentInterface.DomainAdapters.ContentGraphRead.E0.Contract."
		"DescriptorAndScenarioBoundary",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentContentGraphReadContractFixtureTest::RunTest(
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
			"uai.domain_adapter.content_graph_read.test_fixture.v1")));

	const TArray<TSharedPtr<FJsonValue>>* RequiredValues = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* DescriptorValues = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* ProviderScenarioValues = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* DomainScenarioValues = nullptr;
	if (!Fixture->TryGetArrayField(
			TEXT("required_domains"),
			RequiredValues)
		|| !RequiredValues
		|| !Fixture->TryGetArrayField(
			TEXT("descriptors"),
			DescriptorValues)
		|| !DescriptorValues
		|| !Fixture->TryGetArrayField(
			TEXT("e2_provider_scenarios"),
			ProviderScenarioValues)
		|| !ProviderScenarioValues
		|| !Fixture->TryGetArrayField(
			TEXT("e2_domain_scenarios"),
			DomainScenarioValues)
		|| !DomainScenarioValues)
	{
		AddError(TEXT("content_graph_read_fixture_arrays_missing"));
		return false;
	}

	TSet<FString> RequiredDomains;
	for (const TSharedPtr<FJsonValue>& Value : *RequiredValues)
	{
		RequiredDomains.Add(Value->AsString());
	}
	TestEqual(TEXT("six explicit graph domains"), RequiredDomains.Num(), 6);
	TestEqual(
		TEXT("seven shared semantic descriptors"),
		DescriptorValues->Num(),
		7);
	TestEqual(
		TEXT("declared descriptor count"),
		static_cast<int32>(
			Fixture->GetNumberField(TEXT("descriptor_count"))),
		7);

	TSet<FString> DescriptorIds;
	TSet<FString> ProviderBindings;
	TSet<FString> DescriptorHashes;
	bool bValid = true;
	for (const TSharedPtr<FJsonValue>& Value : *DescriptorValues)
	{
		const TSharedPtr<FJsonObject> Row = Value->AsObject();
		const FString DescriptorId =
			Row->GetStringField(TEXT("descriptor_id"));
		const FString ProviderBinding =
			Row->GetStringField(TEXT("provider_binding"));
		const FString Hash =
			Row->GetStringField(TEXT("descriptor_hash"));
		TestTrue(
			*DescriptorId,
			DescriptorId.StartsWith(TEXT("uai.query.content_graph.")));
		TestTrue(
			*ProviderBinding,
			ProviderBinding.StartsWith(TEXT("content_graph.read.")));
		TestFalse(
			TEXT("descriptor does not expose legacy identity"),
			Row->HasField(TEXT("legacy_command_id"))
				|| Row->HasField(TEXT("migration_id")));
		TestEqual(TEXT("version one"), Row->GetNumberField(TEXT("version")), 1.0);
		TestEqual(TEXT("frozen canonical hash"), Hash, ComputeHash(this, Row));
		bValid = !DescriptorIds.Contains(DescriptorId)
			&& !ProviderBindings.Contains(ProviderBinding)
			&& !DescriptorHashes.Contains(Hash)
			&& bValid;
		DescriptorIds.Add(DescriptorId);
		ProviderBindings.Add(ProviderBinding);
		DescriptorHashes.Add(Hash);
		const TSharedPtr<FJsonObject> Input =
			Row->GetObjectField(TEXT("input_schema"));
		bValid = CompileSchema(
			this,
			Input,
			DescriptorId,
			TEXT("input_schema"))
			&& CompileSchema(
				this,
				Row->GetObjectField(TEXT("result_schema")),
				DescriptorId,
				TEXT("result_schema"))
			&& ValidateInputOwnership(
				this,
				Input,
				DescriptorId,
				TEXT("input"))
			&& bValid;
	}
	TestTrue(TEXT("descriptor identities are unique"), bValid);
	TSet<FString> ScenarioProviders;
	for (const TSharedPtr<FJsonValue>& Value : *ProviderScenarioValues)
	{
		const TSharedPtr<FJsonObject> Row = Value->AsObject();
		const FString ProviderBinding =
			Row->GetStringField(TEXT("provider_binding"));
		TestTrue(
			TEXT("provider scenario uses a frozen provider"),
			ProviderBindings.Contains(ProviderBinding));
		const TArray<TSharedPtr<FJsonValue>>* Prefixes = nullptr;
		TestTrue(
			TEXT("provider scenario has typed semantic oracles"),
			Row->TryGetArrayField(
				TEXT("expected_semantic_prefixes"),
				Prefixes)
				&& Prefixes
				&& !Prefixes->IsEmpty());
		ScenarioProviders.Add(ProviderBinding);
	}
	TestEqual(
		TEXT("one E2 oracle per provider"),
		ProviderScenarioValues->Num(),
		7);
	TestEqual(
		TEXT("E2 covers every provider"),
		ScenarioProviders.Num(),
		ProviderBindings.Num());
	for (const FString& ProviderBinding : ProviderBindings)
	{
		TestTrue(
			*FString::Printf(
				TEXT("E2 covers %s"),
				*ProviderBinding),
			ScenarioProviders.Contains(ProviderBinding));
	}

	TSet<FString> ScenarioDomains;
	for (const TSharedPtr<FJsonValue>& Value : *DomainScenarioValues)
	{
		const TSharedPtr<FJsonObject> Row = Value->AsObject();
		TestTrue(
			TEXT("scenario uses a frozen provider"),
			ProviderBindings.Contains(
				Row->GetStringField(TEXT("provider_binding"))));
		TestFalse(
			TEXT("scenario owns an independent oracle"),
			Row->GetStringField(TEXT("oracle")).IsEmpty());
		ScenarioDomains.Add(Row->GetStringField(TEXT("domain")));
	}
	TestEqual(
		TEXT("one compound E2 oracle per domain"),
		DomainScenarioValues->Num(),
		6);
	TestEqual(TEXT("E2 covers every domain"), ScenarioDomains.Num(), 6);
	for (const FString& Domain : RequiredDomains)
	{
		TestTrue(
			*FString::Printf(TEXT("E2 owns %s"), *Domain),
			ScenarioDomains.Contains(Domain));
	}

	TSharedPtr<const FUeAgentDescriptorRegistry> ProductionRegistry;
	FString RegistryError;
	TestTrue(
		TEXT("create production ContentGraphRead descriptor registry"),
		UE::UAI::DomainAdapters::ContentGraphRead::
			TryCreateContentGraphReadDescriptorRegistry(
				ProductionRegistry,
				RegistryError));
	if (!ProductionRegistry.IsValid())
	{
		AddError(TEXT("content_graph_read_registry_failed:")
			+ RegistryError);
		return false;
	}
	TestEqual(
		TEXT("production registry owns all frozen descriptors"),
		ProductionRegistry->GetDescriptors().Num(),
		7);
	for (const TSharedPtr<FJsonValue>& Value : *DescriptorValues)
	{
		const TSharedPtr<FJsonObject> Row = Value->AsObject();
		const FUeAgentOperationDescriptor* Descriptor =
			ProductionRegistry->FindByDescriptorId(
				Row->GetStringField(TEXT("descriptor_id")));
		TestTrue(
			*FString::Printf(
				TEXT("%s is registered with exact hash and provider"),
				*Row->GetStringField(TEXT("descriptor_id"))),
			Descriptor
				&& Descriptor->GetIdentity().GetDescriptorHash()
					== Row->GetStringField(TEXT("descriptor_hash"))
				&& Descriptor->GetQueryBinding().IsSet()
				&& Descriptor->GetQueryBinding()
					->GetProviderBinding()
					== Row->GetStringField(
						TEXT("provider_binding")));
	}
	return true;
}


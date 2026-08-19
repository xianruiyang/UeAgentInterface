#include "Misc/AutomationTest.h"

#include "Core/Contract/UeAgentContractJson.h"
#include "Core/Contract/UeAgentContractSchema.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#include "DomainAdapters/AssetProjectRead/UeAgentAssetProjectReadDescriptors.h"

namespace
{
	using namespace UE::UAI::Contract;

	FString FixturePath()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(
			FPaths::ProjectPluginsDir(),
			TEXT("UeAgentInterface/Source/UeAgentInterfaceTests/Private/"
				"DomainAdapters/AssetProjectRead/Fixtures/"
				"asset_project_read_contract_cases.json")));
	}

	TSharedPtr<FJsonObject> LoadFixture(FAutomationTestBase* Test)
	{
		FString Text;
		if (!Test->TestTrue(
				TEXT("load AssetProjectRead contract fixture"),
				FFileHelper::LoadFileToString(Text, *FixturePath())))
		{
			return nullptr;
		}
		TSharedPtr<FJsonObject> Fixture;
		const TSharedRef<TJsonReader<>> Reader =
			TJsonReaderFactory<>::Create(Text);
		if (!Test->TestTrue(
				TEXT("parse AssetProjectRead contract fixture"),
				FJsonSerializer::Deserialize(Reader, Fixture)
					&& Fixture.IsValid()))
		{
			return nullptr;
		}
		return Fixture;
	}

	bool IsForbiddenInputField(
		const FString& Field,
		const FString& DescriptorId,
		const FString& Path)
	{
		if (DescriptorId == TEXT("uai.query.reference.origin.resolve")
			&& Path.EndsWith(TEXT("/target"))
			&& Field.Equals(TEXT("json_file"), ESearchCase::IgnoreCase))
		{
			return false;
		}
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
				TEXT("asset_project_read_schema_missing:%s:%s"),
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
				if (IsForbiddenInputField(Pair.Key, DescriptorId, Path))
				{
					Test->AddError(FString::Printf(
						TEXT("asset_project_read_cross_layer_input:"
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
		Descriptor->SetStringField(TEXT("domain_id"), TEXT("asset_project"));
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
	FUeAgentAssetProjectReadContractFixtureTest,
	"UeAgentInterface.DomainAdapters.AssetProjectRead.E0.Contract."
		"DescriptorAndScenarioBoundary",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentAssetProjectReadContractFixtureTest::RunTest(
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
			"uai.domain_adapter.asset_project_read.test_fixture.v1")));

	const TArray<TSharedPtr<FJsonValue>>* RequiredValues = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* DescriptorValues = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* ScenarioValues = nullptr;
	if (!Fixture->TryGetArrayField(
			TEXT("required_families"),
			RequiredValues)
		|| !RequiredValues
		|| !Fixture->TryGetArrayField(
			TEXT("descriptors"),
			DescriptorValues)
		|| !DescriptorValues
		|| !Fixture->TryGetArrayField(
			TEXT("e2_scenarios"),
			ScenarioValues)
		|| !ScenarioValues)
	{
		AddError(TEXT("asset_project_read_fixture_arrays_missing"));
		return false;
	}

	TSet<FString> RequiredFamilies;
	for (const TSharedPtr<FJsonValue>& Value : *RequiredValues)
	{
		RequiredFamilies.Add(Value->AsString());
	}
	TestEqual(TEXT("six semantic families"), RequiredFamilies.Num(), 6);
	TestEqual(
		TEXT("seventeen descriptors"),
		DescriptorValues->Num(),
		17);
	TestEqual(
		TEXT("declared descriptor count"),
		static_cast<int32>(
			Fixture->GetNumberField(TEXT("descriptor_count"))),
		17);

	TSet<FString> DescriptorIds;
	TSet<FString> ProviderBindings;
	TSet<FString> DescriptorHashes;
	TSet<FString> Families;
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
		const FString Family =
			Row->GetStringField(TEXT("family"));
		TestTrue(
			*DescriptorId,
			DescriptorId.StartsWith(TEXT("uai.query.")));
		TestTrue(
			*ProviderBinding,
			ProviderBinding.StartsWith(TEXT("asset_project.read.")));
		TestTrue(*Family, RequiredFamilies.Contains(Family));
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
		Families.Add(Family);
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
	TestEqual(
		TEXT("all semantic families own descriptors"),
		Families.Num(),
		RequiredFamilies.Num());

	TSet<FString> ScenarioFamilies;
	for (const TSharedPtr<FJsonValue>& Value : *ScenarioValues)
	{
		const TSharedPtr<FJsonObject> Row = Value->AsObject();
		TestTrue(
			TEXT("scenario uses a frozen provider"),
			ProviderBindings.Contains(
				Row->GetStringField(TEXT("provider_binding"))));
		TestFalse(
			TEXT("scenario owns an independent oracle"),
			Row->GetStringField(TEXT("oracle")).IsEmpty());
		ScenarioFamilies.Add(Row->GetStringField(TEXT("family")));
	}
	TestEqual(TEXT("one E2 oracle per family"), ScenarioValues->Num(), 6);
	TestEqual(TEXT("E2 covers every family"), ScenarioFamilies.Num(), 6);

	TSharedPtr<const FUeAgentDescriptorRegistry> ProductionRegistry;
	FString RegistryError;
	TestTrue(
		TEXT("create production AssetProjectRead descriptor registry"),
		UE::UAI::DomainAdapters::AssetProjectRead::
			TryCreateAssetProjectReadDescriptorRegistry(
				ProductionRegistry,
				RegistryError));
	if (!ProductionRegistry.IsValid())
	{
		AddError(TEXT("asset_project_read_registry_failed:")
			+ RegistryError);
		return false;
	}
	TestEqual(
		TEXT("production registry owns all frozen descriptors"),
		ProductionRegistry->GetDescriptors().Num(),
		17);
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

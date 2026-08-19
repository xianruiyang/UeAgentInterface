#include "Misc/AutomationTest.h"

#include "Core/Contract/UeAgentContractJson.h"
#include "Core/Contract/UeAgentContractSchema.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#include "DomainAdapters/WorldRead/UeAgentWorldReadDescriptors.h"

namespace
{
	using namespace UE::UAI::Contract;

	FString WorldReadFixturePath()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(
			FPaths::ProjectPluginsDir(),
			TEXT("UeAgentInterface/Source/UeAgentInterfaceTests/Private/"
				"DomainAdapters/WorldRead/Fixtures/"
				"world_read_contract_cases.json")));
	}

	TSharedPtr<FJsonObject> LoadWorldReadFixture(
		FAutomationTestBase* Test)
	{
		FString Text;
		if (!Test->TestTrue(
				TEXT("load WorldRead contract fixture"),
				FFileHelper::LoadFileToString(
					Text,
					*WorldReadFixturePath())))
		{
			return nullptr;
		}

		TSharedPtr<FJsonObject> Fixture;
		const TSharedRef<TJsonReader<>> Reader =
			TJsonReaderFactory<>::Create(Text);
		if (!Test->TestTrue(
				TEXT("parse WorldRead contract fixture"),
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
			TEXT("command"),
			TEXT("command_id"),
			TEXT("route"),
			TEXT("provider_binding"),
			TEXT("selector"),
			TEXT("http"),
			TEXT("output_file"),
			TEXT("output_folder"),
			TEXT("input_file"),
			TEXT("input_folder"),
			TEXT("token_budget"),
			TEXT("limit"),
			TEXT("max_items"),
			TEXT("max_hits"),
			TEXT("max_samples"),
			TEXT("expected"),
			TEXT("desired"),
			TEXT("json"),
			TEXT("json_file"),
		};
		return Forbidden.Contains(Field.ToLower());
	}

	bool ValidateSchemaFieldOwnership(
		FAutomationTestBase* Test,
		const TSharedPtr<FJsonObject>& Schema,
		const FString& DescriptorId,
		const FString& Path)
	{
		if (!Schema.IsValid())
		{
			Test->AddError(FString::Printf(
				TEXT("world_read_schema_object_missing:%s:%s"),
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
						TEXT(
							"world_read_cross_layer_input_forbidden:"
							"%s:%s/%s"),
						*DescriptorId,
						*Path,
						*Pair.Key));
					bValid = false;
				}
				if (Pair.Value.IsValid()
					&& Pair.Value->Type == EJson::Object)
				{
					bValid =
						ValidateSchemaFieldOwnership(
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
			bValid =
				ValidateSchemaFieldOwnership(
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
		const TSharedPtr<FJsonObject>& SchemaObject,
		const FString& DescriptorId,
		const TCHAR* SchemaKind)
	{
		FString Canonical;
		FString JsonError;
		if (!Test->TestTrue(
				*FString::Printf(
					TEXT("%s %s canonicalizes"),
					*DescriptorId,
					SchemaKind),
				Json::TryCanonicalize(
					SchemaObject,
					Canonical,
					JsonError)))
		{
			Test->AddError(FString::Printf(
				TEXT("world_read_schema_canonicalize_failed:%s:%s:%s"),
				*DescriptorId,
				SchemaKind,
				*JsonError));
			return false;
		}

		TSharedPtr<const FUeAgentContractSchema> Compiled;
		FUeAgentContractSchemaError SchemaError;
		if (!Test->TestTrue(
				*FString::Printf(
					TEXT("%s %s compiles as C0 ContractSchema v1"),
					*DescriptorId,
					SchemaKind),
				FUeAgentContractSchema::TryCompile(
					FUeAgentContractValue(Canonical),
					Compiled,
					SchemaError)))
		{
			Test->AddError(FString::Printf(
				TEXT(
					"world_read_schema_compile_failed:%s:%s:%s:%s"),
				*DescriptorId,
				SchemaKind,
				*SchemaError.Code,
				*SchemaError.Detail));
			return false;
		}
		return Compiled.IsValid();
	}

	TArray<TSharedPtr<FJsonValue>> SortedStringValues(
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
		Values.Reserve(Strings.Num());
		for (const FString& String : Strings)
		{
			Values.Add(MakeShared<FJsonValueString>(String));
		}
		return Values;
	}

	FString ComputeDescriptorHash(
		FAutomationTestBase* Test,
		const TSharedPtr<FJsonObject>& Row)
	{
		const TSharedPtr<FJsonObject> Policy =
			MakeShared<FJsonObject>();
		Policy->SetArrayField(
			TEXT("allowed_state_planes"),
			SortedStringValues(
				Row,
				TEXT("allowed_state_planes")));
		Policy->SetArrayField(
			TEXT("required_facets"),
			SortedStringValues(
				Row,
				TEXT("required_context_facets")));
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
		Descriptor->SetStringField(
			TEXT("domain_id"),
			TEXT("world"));
		Descriptor->SetObjectField(
			TEXT("execution_context_policy"),
			Policy);
		Descriptor->SetObjectField(
			TEXT("input_schema"),
			Row->GetObjectField(TEXT("input_schema")));
		Descriptor->SetStringField(
			TEXT("operation_kind"),
			TEXT("query"));
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
				*FString::Printf(
					TEXT("%s descriptor canonicalizes"),
					*Row->GetStringField(TEXT("descriptor_id"))),
				Json::TryCanonicalize(
					Descriptor,
					Canonical,
					Error)))
		{
			return FString();
		}
		return Json::Sha256Utf8(Canonical);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentWorldReadContractFixtureTest,
	"UeAgentInterface.DomainAdapters.WorldRead.E0.Contract."
		"DescriptorAndScenarioBoundary",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentWorldReadContractFixtureTest::RunTest(
	const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Fixture =
		LoadWorldReadFixture(this);
	if (!Fixture.IsValid())
	{
		return false;
	}

	TestEqual(
		TEXT("fixture schema"),
		Fixture->GetStringField(TEXT("schema")),
		FString(TEXT(
			"uai.domain_adapter.world_read.test_fixture.v1")));

	const TArray<TSharedPtr<FJsonValue>>* RequiredFamilyValues = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* DescriptorValues = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* ScenarioValues = nullptr;
	if (!TestTrue(
			TEXT("required families exist"),
			Fixture->TryGetArrayField(
				TEXT("required_families"),
				RequiredFamilyValues)
				&& RequiredFamilyValues)
		|| !TestTrue(
			TEXT("descriptors exist"),
			Fixture->TryGetArrayField(
				TEXT("descriptors"),
				DescriptorValues)
				&& DescriptorValues)
		|| !TestTrue(
			TEXT("E2 scenarios exist"),
			Fixture->TryGetArrayField(
				TEXT("e2_scenarios"),
				ScenarioValues)
				&& ScenarioValues))
	{
		return false;
	}

	TSet<FString> RequiredFamilies;
	for (const TSharedPtr<FJsonValue>& Value : *RequiredFamilyValues)
	{
		RequiredFamilies.Add(Value->AsString());
	}
	TestEqual(
		TEXT("seven required semantic families"),
		RequiredFamilies.Num(),
		7);

	const int32 DeclaredDescriptorCount = static_cast<int32>(
		Fixture->GetNumberField(TEXT("descriptor_count")));
	TestEqual(
		TEXT("fixture freezes 23 descriptors"),
		DeclaredDescriptorCount,
		23);
	TestEqual(
		TEXT("declared descriptor count matches rows"),
		DescriptorValues->Num(),
		DeclaredDescriptorCount);

	TSet<FString> DescriptorIds;
	TSet<FString> ProviderBindings;
	TSet<FString> DescriptorHashes;
	TSet<FString> DescriptorFamilies;
	bool bAllDescriptorsValid = true;
	for (const TSharedPtr<FJsonValue>& Value : *DescriptorValues)
	{
		const TSharedPtr<FJsonObject> Row = Value->AsObject();
		if (!Row.IsValid())
		{
			AddError(TEXT("world_read_descriptor_row_invalid"));
			bAllDescriptorsValid = false;
			continue;
		}

		const FString DescriptorId =
			Row->GetStringField(TEXT("descriptor_id"));
		const FString ProviderBinding =
			Row->GetStringField(TEXT("provider_binding"));
		const FString DescriptorHash =
			Row->GetStringField(TEXT("descriptor_hash"));
		const FString Family =
			Row->GetStringField(TEXT("family"));
		TestTrue(
			*FString::Printf(
				TEXT("%s has public query identity"),
				*DescriptorId),
			DescriptorId.StartsWith(TEXT("uai.query.world.")));
		TestTrue(
			*FString::Printf(
				TEXT("%s has server-owned provider binding"),
				*DescriptorId),
			ProviderBinding.StartsWith(TEXT("world.read.")));
		TestEqual(
			*FString::Printf(
				TEXT("%s starts at descriptor version one"),
				*DescriptorId),
			static_cast<int32>(
				Row->GetNumberField(TEXT("version"))),
			1);
		TestTrue(
			*FString::Printf(
				TEXT("%s uses a required family"),
				*DescriptorId),
			RequiredFamilies.Contains(Family));
		TestFalse(
			*FString::Printf(
				TEXT("%s does not expose a legacy command id"),
				*DescriptorId),
			Row->HasField(TEXT("legacy_command_id"))
				|| Row->HasField(TEXT("migration_id")));
		TestEqual(
			*FString::Printf(
				TEXT("%s freezes its canonical C0 hash"),
				*DescriptorId),
			DescriptorHash,
			ComputeDescriptorHash(this, Row));

		if (DescriptorIds.Contains(DescriptorId))
		{
			AddError(FString::Printf(
				TEXT("world_read_descriptor_duplicate:%s"),
				*DescriptorId));
			bAllDescriptorsValid = false;
		}
		if (ProviderBindings.Contains(ProviderBinding))
		{
			AddError(FString::Printf(
				TEXT("world_read_provider_binding_duplicate:%s"),
				*ProviderBinding));
			bAllDescriptorsValid = false;
		}
		if (DescriptorHashes.Contains(DescriptorHash))
		{
			AddError(FString::Printf(
				TEXT("world_read_descriptor_hash_duplicate:%s"),
				*DescriptorHash));
			bAllDescriptorsValid = false;
		}
		DescriptorIds.Add(DescriptorId);
		ProviderBindings.Add(ProviderBinding);
		DescriptorHashes.Add(DescriptorHash);
		DescriptorFamilies.Add(Family);

		const TSharedPtr<FJsonObject> InputSchema =
			Row->GetObjectField(TEXT("input_schema"));
		const TSharedPtr<FJsonObject> ResultSchema =
			Row->GetObjectField(TEXT("result_schema"));
		bAllDescriptorsValid =
			CompileSchema(
				this,
				InputSchema,
				DescriptorId,
				TEXT("input_schema"))
			&& bAllDescriptorsValid;
		bAllDescriptorsValid =
			CompileSchema(
				this,
				ResultSchema,
				DescriptorId,
				TEXT("result_schema"))
			&& bAllDescriptorsValid;
		bAllDescriptorsValid =
			ValidateSchemaFieldOwnership(
				this,
				InputSchema,
				DescriptorId,
				TEXT("input"))
			&& bAllDescriptorsValid;

		const bool bSpatialFamily =
			Family == TEXT("spatial")
			|| Family == TEXT("mesh")
			|| Family == TEXT("navigation");
		const TSharedPtr<FJsonObject>* SpaceContract = nullptr;
		TestEqual(
			*FString::Printf(
				TEXT("%s space contract presence follows family"),
				*DescriptorId),
			Row->TryGetObjectField(
				TEXT("space_contract"),
				SpaceContract)
				&& SpaceContract
				&& SpaceContract->IsValid(),
			bSpatialFamily);
		if (bSpatialFamily
			&& SpaceContract
			&& SpaceContract->IsValid())
		{
			for (const TCHAR* Field : {
				TEXT("subject"),
				TEXT("dimension"),
				TEXT("semantic_frame"),
				TEXT("write_frame"),
				TEXT("read_frame"),
				TEXT("verify_frame"),
				TEXT("conversion_chain"),
			})
			{
				TestTrue(
					*FString::Printf(
						TEXT("%s space contract owns %s"),
						*DescriptorId,
						Field),
					(*SpaceContract)->HasField(Field));
			}
			TestEqual(
				*FString::Printf(
					TEXT("%s is a read-only spatial contract"),
					*DescriptorId),
				(*SpaceContract)->GetStringField(
					TEXT("write_frame")),
				FString(TEXT("not_applicable")));
			TestEqual(
				*FString::Printf(
					TEXT("%s is explicitly three-dimensional"),
					*DescriptorId),
				static_cast<int32>(
					(*SpaceContract)->GetNumberField(
						TEXT("dimension"))),
				3);
		}
	}

	TestTrue(
		TEXT("all descriptor rows satisfy the frozen boundary"),
		bAllDescriptorsValid);
	TestEqual(
		TEXT("all required families own descriptors"),
		DescriptorFamilies.Num(),
		RequiredFamilies.Num());

	TSet<FString> ScenarioIds;
	TSet<FString> ScenarioFamilies;
	for (const TSharedPtr<FJsonValue>& Value : *ScenarioValues)
	{
		const TSharedPtr<FJsonObject> Row = Value->AsObject();
		const FString CaseId =
			Row->GetStringField(TEXT("case_id"));
		const FString Family =
			Row->GetStringField(TEXT("family"));
		const FString ProviderBinding =
			Row->GetStringField(TEXT("provider_binding"));
		TestFalse(
			*FString::Printf(
				TEXT("E2 case id is unique: %s"),
				*CaseId),
			ScenarioIds.Contains(CaseId));
		TestTrue(
			*FString::Printf(
				TEXT("%s targets a frozen provider"),
				*CaseId),
			ProviderBindings.Contains(ProviderBinding));
		TestFalse(
			*FString::Printf(
				TEXT("%s owns an independent oracle"),
				*CaseId),
			Row->GetStringField(TEXT("oracle")).IsEmpty());
		ScenarioIds.Add(CaseId);
		ScenarioFamilies.Add(Family);
	}
	TestEqual(
		TEXT("one independent E2 oracle per semantic family"),
		ScenarioValues->Num(),
		RequiredFamilies.Num());
	bool bAllFamiliesCovered =
		ScenarioFamilies.Num() == RequiredFamilies.Num();
	for (const FString& RequiredFamily : RequiredFamilies)
	{
		bAllFamiliesCovered =
			ScenarioFamilies.Contains(RequiredFamily)
			&& bAllFamiliesCovered;
	}
	TestTrue(
		TEXT("E2 scenarios cover every required family"),
		bAllFamiliesCovered);

	TSharedPtr<const FUeAgentDescriptorRegistry> ProductionRegistry;
	FString RegistryError;
	const bool bProductionRegistryCreated = TestTrue(
		TEXT("create production WorldRead descriptor registry"),
		UE::UAI::DomainAdapters::WorldRead::
			TryCreateWorldReadDescriptorRegistry(
				ProductionRegistry,
				RegistryError));
	if (!bProductionRegistryCreated)
	{
		AddError(TEXT("world_read_registry_failed:") + RegistryError);
	}
	if (ProductionRegistry.IsValid())
	{
		TestEqual(
			TEXT("production registry owns all frozen descriptors"),
			ProductionRegistry->GetDescriptors().Num(),
			DeclaredDescriptorCount);
		for (const TSharedPtr<FJsonValue>& Value : *DescriptorValues)
		{
			const TSharedPtr<FJsonObject> Row = Value->AsObject();
			const FUeAgentOperationDescriptor* Descriptor =
				ProductionRegistry->FindByDescriptorId(
					Row->GetStringField(TEXT("descriptor_id")));
			TestTrue(
				*FString::Printf(
					TEXT("%s is registered in production"),
					*Row->GetStringField(TEXT("descriptor_id"))),
				Descriptor
					&& Descriptor->GetIdentity().GetDescriptorHash()
						== Row->GetStringField(
							TEXT("descriptor_hash"))
					&& Descriptor->GetQueryBinding().IsSet()
					&& Descriptor->GetQueryBinding()
						->GetProviderBinding()
						== Row->GetStringField(
							TEXT("provider_binding")));
		}
	}
	return true;
}

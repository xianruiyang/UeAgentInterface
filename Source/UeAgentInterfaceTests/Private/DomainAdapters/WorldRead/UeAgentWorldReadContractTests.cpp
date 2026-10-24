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

	TArray<FString> RetiredSpatialInputFields(
		const FString& DescriptorId)
	{
		if (DescriptorId == TEXT("uai.query.world.overlap"))
		{
			return {
				TEXT("trace_complex"),
				TEXT("ignore_target_refs"),
			};
		}
		if (DescriptorId == TEXT("uai.query.world.capsule_sweep"))
		{
			return {
				TEXT("return_penetration_depth"),
				TEXT("ignore_target_refs"),
			};
		}
		if (DescriptorId == TEXT("uai.query.world.capsule_path"))
		{
			return {
				TEXT("trace_complex"),
				TEXT("require_floor"),
				TEXT("snap_to_floor"),
				TEXT("floor_trace_channel"),
				TEXT("floor_trace_up_cm"),
				TEXT("floor_trace_down_cm"),
				TEXT("floor_clearance_cm"),
				TEXT("max_walkable_slope_deg"),
			};
		}
		if (DescriptorId == TEXT("uai.query.world.nearby_shapes"))
		{
			return {
				TEXT("component_classes"),
				TEXT("include_visual_fallback"),
				TEXT("detail_level"),
			};
		}
		return {};
	}

	const TCHAR* OptionalSpatialCollectionField(
		const FString& DescriptorId)
	{
		if (DescriptorId == TEXT("uai.query.world.overlap"))
		{
			return TEXT("include_targets");
		}
		if (DescriptorId == TEXT("uai.query.world.capsule_sweep"))
		{
			return TEXT("include_hits");
		}
		if (DescriptorId == TEXT("uai.query.world.nearby_shapes"))
		{
			return TEXT("include_shapes");
		}
		return nullptr;
	}

	TArray<FString> RetiredTopologyInputFields(
		const FString& DescriptorId)
	{
		if (DescriptorId == TEXT("uai.query.world.hlod_assignments")
			|| DescriptorId == TEXT("uai.query.world.streaming_levels"))
		{
			return {TEXT("scope")};
		}
		return {};
	}

	bool SchemaRequiresField(
		const TSharedPtr<FJsonObject>& Schema,
		const FString& Field)
	{
		const TArray<TSharedPtr<FJsonValue>>* Required = nullptr;
		if (!Schema->TryGetArrayField(TEXT("required"), Required)
			|| !Required)
		{
			return false;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Required)
		{
			if (Value.IsValid() && Value->AsString() == Field)
			{
				return true;
			}
		}
		return false;
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
	TSet<FString> DescriptorIds;
	TSet<FString> ProviderBindings;
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
				|| Row->HasField(TEXT("migration_id"))
				|| Row->HasField(TEXT("descriptor_hash")));

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
		DescriptorIds.Add(DescriptorId);
		ProviderBindings.Add(ProviderBinding);
		DescriptorFamilies.Add(Family);

		const TSharedPtr<FJsonObject> InputSchema =
			Row->GetObjectField(TEXT("input_schema"));
		const TSharedPtr<FJsonObject> ResultSchema =
			Row->GetObjectField(TEXT("result_schema"));
		if (DescriptorId == TEXT("uai.query.world.actor_components"))
		{
			TestEqual(
				TEXT("actor components exposes no unowned input fields"),
				InputSchema->GetObjectField(TEXT("properties"))->Values.Num(),
				0);
		}
		if (DescriptorId == TEXT("uai.query.world.mesh_closest_vertex")
			|| DescriptorId == TEXT("uai.query.world.mesh_vertex_position"))
		{
			const TArray<TSharedPtr<FJsonValue>> TargetKinds =
				Row->GetArrayField(TEXT("target_kinds"));
			TestEqual(
				*FString::Printf(
					TEXT("%s exposes only its native mesh target kind"),
					*DescriptorId),
				TargetKinds.Num(),
				1);
			if (TargetKinds.Num() == 1)
			{
				TestEqual(
					*FString::Printf(
						TEXT("%s targets static mesh components"),
						*DescriptorId),
					TargetKinds[0]->AsString(),
					FString(TEXT("static_mesh_component")));
			}
		}
		if (DescriptorId == TEXT("uai.query.world.nav_find_path"))
		{
			const TSharedPtr<FJsonObject> InputProperties =
				InputSchema->GetObjectField(TEXT("properties"));
			const TSharedPtr<FJsonObject> IncludePathPoints =
				InputProperties->GetObjectField(TEXT("include_path_points"));
			TestEqual(
				TEXT("nav find path exposes optional path-point selection"),
				IncludePathPoints->GetStringField(TEXT("type")),
				FString(TEXT("boolean")));
			const TArray<TSharedPtr<FJsonValue>> Required =
				InputSchema->GetArrayField(TEXT("required"));
			bool bRequiresIncludePathPoints = false;
			for (const TSharedPtr<FJsonValue>& RequiredValue : Required)
			{
				bRequiresIncludePathPoints =
					bRequiresIncludePathPoints
					|| RequiredValue->AsString()
						== TEXT("include_path_points");
			}
			TestFalse(
				TEXT("nav find path defaults to omitting path points"),
				bRequiresIncludePathPoints);
		}
		const TArray<FString> RetiredFields =
			RetiredSpatialInputFields(DescriptorId);
		if (!RetiredFields.IsEmpty())
		{
			const TSharedPtr<FJsonObject> InputProperties =
				InputSchema->GetObjectField(TEXT("properties"));
			TestFalse(
				*FString::Printf(
					TEXT("%s rejects undeclared input fields"),
					*DescriptorId),
				InputSchema->GetBoolField(TEXT("additionalProperties")));
			for (const FString& RetiredField : RetiredFields)
			{
				TestFalse(
					*FString::Printf(
						TEXT("%s no longer exposes unconsumed %s"),
						*DescriptorId,
						*RetiredField),
					InputProperties->HasField(RetiredField));
			}
		}
		const TCHAR* OptionalCollectionField =
			OptionalSpatialCollectionField(DescriptorId);
		if (OptionalCollectionField)
		{
			const TSharedPtr<FJsonObject> InputProperties =
				InputSchema->GetObjectField(TEXT("properties"));
			const TSharedPtr<FJsonObject> FieldSchema =
				InputProperties->GetObjectField(OptionalCollectionField);
			TestEqual(
				*FString::Printf(
					TEXT("%s %s is boolean"),
					*DescriptorId,
					OptionalCollectionField),
				FieldSchema->GetStringField(TEXT("type")),
				FString(TEXT("boolean")));
			bool bRequired = false;
			for (const TSharedPtr<FJsonValue>& Required
				: InputSchema->GetArrayField(TEXT("required")))
			{
				bRequired = Required->AsString() == OptionalCollectionField
					|| bRequired;
			}
			TestFalse(
				*FString::Printf(
					TEXT("%s %s remains optional"),
					*DescriptorId,
					OptionalCollectionField),
				bRequired);
		}
		const TArray<FString> RetiredTopologyFields =
			RetiredTopologyInputFields(DescriptorId);
		if (!RetiredTopologyFields.IsEmpty())
		{
			const TSharedPtr<FJsonObject> InputProperties =
				InputSchema->GetObjectField(TEXT("properties"));
			for (const FString& RetiredField : RetiredTopologyFields)
			{
				TestFalse(
					*FString::Printf(
						TEXT("%s no longer exposes unconsumed %s"),
						*DescriptorId,
						*RetiredField),
					InputProperties->HasField(RetiredField));
			}
		}
		if (DescriptorId == TEXT("uai.query.world.streaming_levels"))
		{
			const TSharedPtr<FJsonObject> LevelPackages =
				InputSchema->GetObjectField(TEXT("properties"))
					->GetObjectField(TEXT("level_packages"));
			TestEqual(
				TEXT("streaming level package filter remains an array"),
				LevelPackages->GetStringField(TEXT("type")),
				FString(TEXT("array")));
			TestEqual(
				TEXT("streaming level package filter items remain strings"),
				LevelPackages->GetObjectField(TEXT("items"))
					->GetStringField(TEXT("type")),
				FString(TEXT("string")));
			TestFalse(
				TEXT("streaming level package filter remains optional"),
				SchemaRequiresField(InputSchema, TEXT("level_packages")));
		}
		if (DescriptorId == TEXT("uai.query.world.partition"))
		{
			const TSharedPtr<FJsonObject> IncludeLoadedRegions =
				InputSchema->GetObjectField(TEXT("properties"))
					->GetObjectField(TEXT("include_loaded_regions"));
			TestEqual(
				TEXT("partition loaded-region selector remains boolean"),
				IncludeLoadedRegions->GetStringField(TEXT("type")),
				FString(TEXT("boolean")));
			TestFalse(
				TEXT("partition loaded-region selector remains optional"),
				SchemaRequiresField(
					InputSchema,
					TEXT("include_loaded_regions")));
		}
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
	for (const FString& RequiredFamily : RequiredFamilies)
	{
		TestTrue(
			*FString::Printf(TEXT("descriptor family covers %s"), *RequiredFamily),
			DescriptorFamilies.Contains(RequiredFamily));
	}

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
	bool bAllFamiliesCovered = true;
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
		TSet<FString> ProductionDescriptorHashes;
		for (const TSharedPtr<FJsonValue>& Value : *DescriptorValues)
		{
			const TSharedPtr<FJsonObject> Row = Value->AsObject();
			FString InputSchema;
			FString ResultSchema;
			FString SchemaError;
			const bool bSchemasCanonical =
				Json::TryCanonicalize(
					Row->GetObjectField(TEXT("input_schema")),
					InputSchema,
					SchemaError)
				&& Json::TryCanonicalize(
					Row->GetObjectField(TEXT("result_schema")),
					ResultSchema,
					SchemaError);
			TestTrue(
				TEXT("fixture schemas canonicalize for registry comparison"),
				bSchemasCanonical);
			const FUeAgentOperationDescriptor* Descriptor =
				ProductionRegistry->FindByDescriptorId(
					Row->GetStringField(TEXT("descriptor_id")));
			const bool bUniqueMachineIdentity = Descriptor
				&& !ProductionDescriptorHashes.Contains(
					Descriptor->GetIdentity().GetDescriptorHash());
			TestTrue(
				*FString::Printf(
					TEXT("%s is registered in production"),
					*Row->GetStringField(TEXT("descriptor_id"))),
				Descriptor
					&& !Descriptor->GetIdentity().GetDescriptorHash().IsEmpty()
					&& bUniqueMachineIdentity
					&& Descriptor->GetCapabilityId()
						== Row->GetStringField(TEXT("capability_id"))
					&& Descriptor->GetInputSchema().GetJsonValue()
						== InputSchema
					&& Descriptor->GetResultSchema().GetJsonValue()
						== ResultSchema
					&& Descriptor->GetQueryBinding().IsSet()
					&& Descriptor->GetQueryBinding()
						->GetProviderBinding()
						== Row->GetStringField(
							TEXT("provider_binding")));
			if (Descriptor)
			{
				ProductionDescriptorHashes.Add(
					Descriptor->GetIdentity().GetDescriptorHash());
			}
		}
	}
	return true;
}

#pragma once

#include "Core/Contract/UeAgentContractJson.h"
#include "Core/Contract/UeAgentContractSchema.h"
#include "Core/Contract/UeAgentDescriptorRegistry.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace UE::UAI::Tests::AssetDataSimpleEffect
{
	using namespace Contract;

	inline FString FixturePath()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(
			FPaths::ProjectPluginsDir(),
			TEXT(
				"UeAgentInterface/Source/UeAgentInterfaceTests/"
				"Private/DomainAdapters/AssetDataSimpleEffect/"
				"Fixtures/asset_data_simple_effect_contract_cases.json")));
	}

	inline TSharedPtr<FJsonObject> LoadFixture(
		FAutomationTestBase* Test)
	{
		FString Text;
		if (!Test->TestTrue(
				TEXT("load AssetDataSimpleEffect fixture"),
				FFileHelper::LoadFileToString(Text, *FixturePath())))
		{
			return nullptr;
		}
		TSharedPtr<FJsonObject> Fixture;
		const TSharedRef<TJsonReader<>> Reader =
			TJsonReaderFactory<>::Create(Text);
		if (!Test->TestTrue(
				TEXT("parse AssetDataSimpleEffect fixture"),
				FJsonSerializer::Deserialize(Reader, Fixture)
					&& Fixture.IsValid()))
		{
			return nullptr;
		}
		return Fixture;
	}

	inline TSharedPtr<FJsonObject> StringSchema()
	{
		const TSharedPtr<FJsonObject> Schema =
			MakeShared<FJsonObject>();
		Schema->SetStringField(TEXT("type"), TEXT("string"));
		Schema->SetNumberField(TEXT("minLength"), 1);
		return Schema;
	}

	inline TSharedPtr<FJsonObject> EnumSchema(
		const TArray<FString>& Values)
	{
		const TSharedPtr<FJsonObject> Schema = StringSchema();
		TArray<TSharedPtr<FJsonValue>> EnumValues;
		for (const FString& Value : Values)
		{
			EnumValues.Add(
				MakeShared<FJsonValueString>(Value));
		}
		Schema->SetArrayField(TEXT("enum"), MoveTemp(EnumValues));
		return Schema;
	}

	inline TSharedPtr<FJsonObject> ConstStringSchema(
		const FString& Value)
	{
		const TSharedPtr<FJsonObject> Schema =
			MakeShared<FJsonObject>();
		Schema->SetStringField(TEXT("const"), Value);
		return Schema;
	}

	inline TSharedPtr<FJsonObject> ObjectSchema(
		const TSharedPtr<FJsonObject>& Properties,
		const TArray<FString>& Required)
	{
		const TSharedPtr<FJsonObject> Schema =
			MakeShared<FJsonObject>();
		Schema->SetStringField(TEXT("type"), TEXT("object"));
		Schema->SetObjectField(TEXT("properties"), Properties);
		Schema->SetBoolField(TEXT("additionalProperties"), false);
		TArray<TSharedPtr<FJsonValue>> RequiredValues;
		for (const FString& Name : Required)
		{
			RequiredValues.Add(
				MakeShared<FJsonValueString>(Name));
		}
		Schema->SetArrayField(
			TEXT("required"),
			MoveTemp(RequiredValues));
		return Schema;
	}

	inline TArray<FString> StringArray(
		const TSharedPtr<FJsonObject>& Object,
		const TCHAR* Field)
	{
		TArray<FString> Result;
		for (const TSharedPtr<FJsonValue>& Value
			: Object->GetArrayField(Field))
		{
			Result.Add(Value->AsString());
		}
		return Result;
	}

	inline TSharedPtr<FJsonValue> CloneJsonValue(
		const TSharedPtr<FJsonValue>& Value)
	{
		if (!Value.IsValid())
		{
			return MakeShared<FJsonValueNull>();
		}
		switch (Value->Type)
		{
		case EJson::Null:
			return MakeShared<FJsonValueNull>();
		case EJson::Boolean:
			return MakeShared<FJsonValueBoolean>(Value->AsBool());
		case EJson::String:
			return MakeShared<FJsonValueString>(Value->AsString());
		case EJson::Number:
			return MakeShared<FJsonValueNumber>(Value->AsNumber());
		case EJson::Array:
			{
				TArray<TSharedPtr<FJsonValue>> Items;
				for (const TSharedPtr<FJsonValue>& Item
					: Value->AsArray())
				{
					Items.Add(CloneJsonValue(Item));
				}
				return MakeShared<FJsonValueArray>(MoveTemp(Items));
			}
		case EJson::Object:
			{
				const TSharedPtr<FJsonObject> Object =
					MakeShared<FJsonObject>();
				for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair
					: Value->AsObject()->Values)
				{
					Object->SetField(
						Pair.Key,
						CloneJsonValue(Pair.Value));
				}
				return MakeShared<FJsonValueObject>(Object);
			}
		default:
			return MakeShared<FJsonValueNull>();
		}
	}

	inline TArray<TSharedPtr<FJsonObject>> FieldContracts(
		const TSharedPtr<FJsonObject>& Fixture,
		const TSharedPtr<FJsonObject>& Row)
	{
		TArray<TSharedPtr<FJsonObject>> Result;
		const FString ProfileId =
			Row->GetStringField(TEXT("profile_id"));
		const TSet<FString> FieldIds(
			StringArray(Row, TEXT("field_ids")));
		for (const TSharedPtr<FJsonValue>& Value
			: Fixture->GetArrayField(TEXT("field_contracts")))
		{
			const TSharedPtr<FJsonObject> Field = Value->AsObject();
			if (Field->GetStringField(TEXT("profile_id")) == ProfileId
				&& FieldIds.Contains(
					Field->GetStringField(TEXT("field_id"))))
			{
				Result.Add(Field);
			}
		}
		return Result;
	}

	inline TSharedPtr<FJsonObject> FindFieldContract(
		const TSharedPtr<FJsonObject>& Fixture,
		const TSharedPtr<FJsonObject>& Row,
		const FString& FieldId)
	{
		for (const TSharedPtr<FJsonObject>& Field
			: FieldContracts(Fixture, Row))
		{
			if (Field->GetStringField(TEXT("field_id")) == FieldId)
			{
				return Field;
			}
		}
		return nullptr;
	}

	inline TSharedPtr<FJsonObject> FindFactoryContract(
		const TSharedPtr<FJsonObject>& Fixture,
		const TSharedPtr<FJsonObject>& Row)
	{
		if (Row->GetStringField(TEXT("native_operation_type"))
			!= TEXT("asset.object.create"))
		{
			return nullptr;
		}
		const FString ProfileId =
			Row->GetStringField(TEXT("profile_id"));
		for (const TSharedPtr<FJsonValue>& Value
			: Fixture->GetArrayField(TEXT("factory_contracts")))
		{
			const TSharedPtr<FJsonObject> Contract =
				Value->AsObject();
			if (Contract->GetStringField(TEXT("profile_id"))
				== ProfileId)
			{
				return Contract;
			}
		}
		return nullptr;
	}

	inline TSharedPtr<FJsonObject> FactoryRefSchema()
	{
		const TSharedPtr<FJsonObject> RevisionProperties =
			MakeShared<FJsonObject>();
		RevisionProperties->SetObjectField(
			TEXT("source_id"),
			StringSchema());
		RevisionProperties->SetObjectField(
			TEXT("generation"),
			StringSchema());
		const TSharedPtr<FJsonObject> SequenceSchema =
			MakeShared<FJsonObject>();
		SequenceSchema->SetStringField(TEXT("type"), TEXT("integer"));
		SequenceSchema->SetNumberField(TEXT("minimum"), 0);
		RevisionProperties->SetObjectField(
			TEXT("sequence"),
			SequenceSchema);

		const TSharedPtr<FJsonObject> Properties =
			MakeShared<FJsonObject>();
		Properties->SetObjectField(
			TEXT("candidate_ref"),
			StringSchema());
		Properties->SetObjectField(
			TEXT("catalog_revision"),
			ObjectSchema(
				RevisionProperties,
				{
					TEXT("source_id"),
					TEXT("generation"),
					TEXT("sequence"),
				}));
		return ObjectSchema(
			Properties,
			{TEXT("candidate_ref"), TEXT("catalog_revision")});
	}

	inline TSharedPtr<FJsonObject> ValueKindSchema(
		const FString& ValueKind)
	{
		const TSharedPtr<FJsonObject> Schema =
			MakeShared<FJsonObject>();
		Schema->SetStringField(
			TEXT("type"),
			ValueKind == TEXT("number")
				? TEXT("number")
				: ValueKind);
		if (ValueKind == TEXT("string"))
		{
			Schema->SetNumberField(TEXT("minLength"), 1);
		}
		return Schema;
	}

	inline bool IsCreate(
		const TSharedPtr<FJsonObject>& Row)
	{
		return Row->GetStringField(TEXT("native_operation_type"))
			== TEXT("asset.object.create");
	}

	inline bool IsClone(
		const TSharedPtr<FJsonObject>& Row)
	{
		return Row->GetStringField(TEXT("native_operation_type"))
			== TEXT("asset.object.clone");
	}

	inline TSharedPtr<FJsonObject> ChangeValueSchema(
		const TSharedPtr<FJsonObject>& Field)
	{
		const TSharedPtr<FJsonObject> Properties =
			MakeShared<FJsonObject>();
		Properties->SetObjectField(
			TEXT("type_id"),
			ConstStringSchema(
				Field->GetStringField(TEXT("type_id"))));
		Properties->SetObjectField(
			TEXT("canonical_value"),
			ValueKindSchema(
				Field->GetStringField(TEXT("value_kind"))));
		return ObjectSchema(
			Properties,
			{TEXT("type_id"), TEXT("canonical_value")});
	}

	inline TSharedPtr<FJsonObject> ChangeUnitSchema(
		const TSharedPtr<FJsonObject>& Field)
	{
		TArray<FString> ValuePresences;
		TArray<FString> NoValuePresences;
		for (const FString& Presence
			: StringArray(Field, TEXT("allowed_presences")))
		{
			if (Presence == TEXT("set")
				|| Presence == TEXT("replace")
				|| Presence == TEXT("remove"))
			{
				ValuePresences.Add(Presence);
			}
			else
			{
				NoValuePresences.Add(Presence);
			}
		}
		TArray<TSharedPtr<FJsonValue>> Alternatives;
		if (!ValuePresences.IsEmpty())
		{
			const TSharedPtr<FJsonObject> Properties =
				MakeShared<FJsonObject>();
			Properties->SetObjectField(
				TEXT("field_id"),
				ConstStringSchema(
					Field->GetStringField(TEXT("field_id"))));
			Properties->SetObjectField(
				TEXT("presence"),
				EnumSchema(ValuePresences));
			Properties->SetObjectField(
				TEXT("value"),
				ChangeValueSchema(Field));
			Alternatives.Add(MakeShared<FJsonValueObject>(
				ObjectSchema(
					Properties,
					{
						TEXT("field_id"),
						TEXT("presence"),
						TEXT("value"),
					})));
		}
		if (!NoValuePresences.IsEmpty())
		{
			const TSharedPtr<FJsonObject> Properties =
				MakeShared<FJsonObject>();
			Properties->SetObjectField(
				TEXT("field_id"),
				ConstStringSchema(
					Field->GetStringField(TEXT("field_id"))));
			Properties->SetObjectField(
				TEXT("presence"),
				EnumSchema(NoValuePresences));
			Alternatives.Add(MakeShared<FJsonValueObject>(
				ObjectSchema(
					Properties,
					{TEXT("field_id"), TEXT("presence")})));
		}
		check(!Alternatives.IsEmpty());
		if (Alternatives.Num() == 1)
		{
			return Alternatives[0]->AsObject();
		}
		const TSharedPtr<FJsonObject> Schema =
			MakeShared<FJsonObject>();
		Schema->SetArrayField(TEXT("oneOf"), MoveTemp(Alternatives));
		return Schema;
	}

	inline TSharedPtr<FJsonObject> BuildInputSchema(
		const TSharedPtr<FJsonObject>& Fixture,
		const TSharedPtr<FJsonObject>& Row)
	{
		const TSharedPtr<FJsonObject> Properties =
			MakeShared<FJsonObject>();
		Properties->SetObjectField(TEXT("target_ref"), StringSchema());
		TArray<FString> Required{TEXT("target_ref")};

		if (IsCreate(Row)
			&& FindFactoryContract(Fixture, Row).IsValid())
		{
			Properties->SetObjectField(
				TEXT("factory_ref"),
				FactoryRefSchema());
			Required.Add(TEXT("factory_ref"));
		}
		else if (IsClone(Row))
		{
			Properties->SetObjectField(
				TEXT("source_ref"),
				StringSchema());
			Properties->SetObjectField(
				TEXT("expected_source_revision"),
				StringSchema());
			Properties->SetObjectField(
				TEXT("destructive_authorization_id"),
				StringSchema());
			Required.Add(TEXT("source_ref"));
			Required.Add(TEXT("expected_source_revision"));
		}
		else if (!IsCreate(Row))
		{
			Properties->SetObjectField(
				TEXT("expected_revision"),
				StringSchema());
			const TSharedPtr<FJsonObject> Changes =
				MakeShared<FJsonObject>();
			Changes->SetStringField(TEXT("type"), TEXT("array"));
			TArray<TSharedPtr<FJsonValue>> FieldAlternatives;
			for (const TSharedPtr<FJsonObject>& Field
				: FieldContracts(Fixture, Row))
			{
				FieldAlternatives.Add(
					MakeShared<FJsonValueObject>(
						ChangeUnitSchema(Field)));
			}
			check(!FieldAlternatives.IsEmpty());
			const TSharedPtr<FJsonObject> ItemSchema =
				MakeShared<FJsonObject>();
			ItemSchema->SetArrayField(
				TEXT("oneOf"),
				MoveTemp(FieldAlternatives));
			Changes->SetObjectField(TEXT("items"), ItemSchema);
			Changes->SetNumberField(TEXT("minItems"), 1);
			Changes->SetBoolField(TEXT("uniqueItems"), true);
			Properties->SetObjectField(TEXT("changes"), Changes);
			Required.Add(TEXT("expected_revision"));
			Required.Add(TEXT("changes"));
		}
		return ObjectSchema(Properties, Required);
	}

	inline TSharedPtr<FJsonObject> BuildResultSchema()
	{
		const TSharedPtr<FJsonObject> StringArraySchema =
			MakeShared<FJsonObject>();
		StringArraySchema->SetStringField(
			TEXT("type"),
			TEXT("array"));
		StringArraySchema->SetObjectField(
			TEXT("items"),
			StringSchema());
		StringArraySchema->SetBoolField(
			TEXT("uniqueItems"),
			true);

		const TSharedPtr<FJsonObject> Properties =
			MakeShared<FJsonObject>();
		Properties->SetObjectField(
			TEXT("operation_id"),
			StringSchema());
		Properties->SetObjectField(
			TEXT("outcome"),
			EnumSchema({
				TEXT("complete"),
				TEXT("partial"),
				TEXT("rolled_back"),
				TEXT("state_unknown"),
			}));
		Properties->SetObjectField(
			TEXT("target_ref"),
			StringSchema());
		Properties->SetObjectField(
			TEXT("asset_revision"),
			StringSchema());
		Properties->SetObjectField(
			TEXT("evidence_refs"),
			StringArraySchema);
		return ObjectSchema(
			Properties,
			{
				TEXT("operation_id"),
				TEXT("outcome"),
				TEXT("target_ref"),
				TEXT("asset_revision"),
			});
	}

	inline TSharedPtr<FJsonObject> BuildValidInput(
		const TSharedPtr<FJsonObject>& Fixture,
		const TSharedPtr<FJsonObject>& Row)
	{
		const TSharedPtr<FJsonObject> Input =
			MakeShared<FJsonObject>();
		Input->SetStringField(
			TEXT("target_ref"),
			TEXT("uai://asset/fixture-target"));
		if (IsCreate(Row))
		{
			const TSharedPtr<FJsonObject> Factory =
				FindFactoryContract(Fixture, Row);
			if (Factory.IsValid())
			{
				const TSharedPtr<FJsonObject> Revision =
					MakeShared<FJsonObject>();
				Revision->SetStringField(
					TEXT("source_id"),
					Factory->GetStringField(
						TEXT("catalog_source_id")));
				Revision->SetStringField(
					TEXT("generation"),
					Factory->GetStringField(
						TEXT("catalog_generation")));
				Revision->SetNumberField(
					TEXT("sequence"),
					Factory->GetNumberField(
						TEXT("catalog_sequence")));
				const TSharedPtr<FJsonObject> FactoryRef =
					MakeShared<FJsonObject>();
				FactoryRef->SetStringField(
					TEXT("candidate_ref"),
					Factory->GetStringField(
						TEXT("candidate_ref")));
				FactoryRef->SetObjectField(
					TEXT("catalog_revision"),
					Revision);
				Input->SetObjectField(
					TEXT("factory_ref"),
					FactoryRef);
			}
		}
		else if (IsClone(Row))
		{
			Input->SetStringField(
				TEXT("source_ref"),
				TEXT("uai://asset/fixture-source"));
			Input->SetStringField(
				TEXT("expected_source_revision"),
				TEXT("rev:source:1"));
			Input->SetStringField(
				TEXT("destructive_authorization_id"),
				TEXT("destructive-authorization:fixture"));
		}
		else if (!IsCreate(Row))
		{
			Input->SetStringField(
				TEXT("expected_revision"),
				TEXT("rev:target:1"));
			const TArray<TSharedPtr<FJsonObject>> Fields =
				FieldContracts(Fixture, Row);
			check(!Fields.IsEmpty());
			const TSharedPtr<FJsonObject>& Field = Fields[0];
			const TArray<FString> Presences =
				StringArray(Field, TEXT("allowed_presences"));
			check(!Presences.IsEmpty());
			const TSharedPtr<FJsonObject> Unit =
				MakeShared<FJsonObject>();
			Unit->SetStringField(
				TEXT("field_id"),
				Field->GetStringField(TEXT("field_id")));
			Unit->SetStringField(
				TEXT("presence"),
				Presences[0]);
			const TSharedPtr<FJsonObject> TypedValue =
				MakeShared<FJsonObject>();
			TypedValue->SetStringField(
				TEXT("type_id"),
				Field->GetStringField(TEXT("type_id")));
			TypedValue->SetField(
				TEXT("canonical_value"),
				CloneJsonValue(
					Field->TryGetField(TEXT("example_value"))));
			Unit->SetObjectField(TEXT("value"), TypedValue);
			TArray<TSharedPtr<FJsonValue>> Changes;
			Changes.Add(
				MakeShared<FJsonValueObject>(Unit));
			Input->SetArrayField(
				TEXT("changes"),
				MoveTemp(Changes));
		}
		return Input;
	}

	inline FUeAgentContractValue ContractValue(
		FAutomationTestBase* Test,
		const TSharedPtr<FJsonObject>& Value,
		const FString& Label)
	{
		FString Canonical;
		FString Error;
		Test->TestTrue(
			*Label,
			Json::TryCanonicalize(Value, Canonical, Error));
		if (Canonical.IsEmpty())
		{
			Test->AddError(Label + TEXT(":") + Error);
		}
		return FUeAgentContractValue(MoveTemp(Canonical));
	}

	inline bool TryCompileSchema(
		FAutomationTestBase* Test,
		const TSharedPtr<FJsonObject>& Schema,
		const FString& Label,
		TSharedPtr<const FUeAgentContractSchema>& OutSchema)
	{
		OutSchema.Reset();
		FUeAgentContractSchemaError Error;
		const bool bCompiled =
			FUeAgentContractSchema::TryCompile(
				ContractValue(
					Test,
					Schema,
					Label + TEXT(" canonicalizes")),
				OutSchema,
				Error);
		Test->TestTrue(*(Label + TEXT(" compiles")), bCompiled);
		if (!bCompiled)
		{
			Test->AddError(
				Label + TEXT(":") + Error.Code
				+ TEXT(":") + Error.Detail);
		}
		return bCompiled && OutSchema.IsValid();
	}

	inline FString ComputeDescriptorHash(
		FAutomationTestBase* Test,
		const TSharedPtr<FJsonObject>& Row,
		const TSharedPtr<FJsonObject>& InputSchema,
		const TSharedPtr<FJsonObject>& ResultSchema)
	{
		const TSharedPtr<FJsonObject> Policy =
			MakeShared<FJsonObject>();
		Policy->SetArrayField(
			TEXT("allowed_state_planes"),
			{
				MakeShared<FJsonValueString>(
					TEXT("authoring_memory")),
			});
		Policy->SetArrayField(TEXT("required_facets"), {});
		Policy->SetStringField(
			TEXT("resolution_policy"),
			TEXT("explicit_only"));
		Policy->SetStringField(
			TEXT("transient_access_policy"),
			TEXT("temporary_mutation_allowed"));

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
			TEXT("asset_data"));
		Descriptor->SetObjectField(
			TEXT("execution_context_policy"),
			Policy);
		Descriptor->SetObjectField(
			TEXT("input_schema"),
			InputSchema);
		Descriptor->SetStringField(
			TEXT("operation_kind"),
			TEXT("operation"));
		Descriptor->SetObjectField(
			TEXT("result_schema"),
			ResultSchema);
		Descriptor->SetNumberField(TEXT("version"), 1);

		FString Canonical;
		FString Error;
		Test->TestTrue(
			TEXT("asset descriptor canonicalizes"),
			Json::TryCanonicalize(
				Descriptor,
				Canonical,
				Error));
		return Json::Sha256Utf8(Canonical);
	}

	inline bool ContainsForbiddenField(
		const TSharedPtr<FJsonObject>& Object,
		const TSet<FString>& Forbidden,
		FString& OutField)
	{
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair
			: Object->Values)
		{
			if (Forbidden.Contains(Pair.Key))
			{
				OutField = Pair.Key;
				return true;
			}
			if (Pair.Value->Type == EJson::Object
				&& ContainsForbiddenField(
					Pair.Value->AsObject(),
					Forbidden,
					OutField))
			{
				return true;
			}
			if (Pair.Value->Type == EJson::Array)
			{
				for (const TSharedPtr<FJsonValue>& Item
					: Pair.Value->AsArray())
				{
					if (Item->Type == EJson::Object
						&& ContainsForbiddenField(
							Item->AsObject(),
							Forbidden,
							OutField))
					{
						return true;
					}
				}
			}
		}
		return false;
	}
}

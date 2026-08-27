#include "Core/Contract/Discovery/UeAgentContractExample.h"

#include "Core/Contract/UeAgentContractJson.h"
#include "Core/Contract/UeAgentContractSchema.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace UE::UAI::Contract
{
	namespace
	{
		constexpr int32 MaxGenerationWork = 4096;

		bool Fail(FString& OutError, const FString& Code)
		{
			OutError = Code;
			return false;
		}

		bool TryType(
			const TSharedRef<const FJsonObject>& Schema,
			FString& OutType)
		{
			if (Schema->TryGetStringField(TEXT("type"), OutType))
			{
				return true;
			}
			const TArray<TSharedPtr<FJsonValue>>* Types = nullptr;
			if (Schema->TryGetArrayField(TEXT("type"), Types)
				&& Types
				&& !Types->IsEmpty())
			{
				for (const TSharedPtr<FJsonValue>& Type : *Types)
				{
					if (Type.IsValid() && Type->TryGetString(OutType))
					{
						return true;
					}
				}
			}
			return false;
		}

		bool TryMinimalValue(
			const TSharedRef<const FJsonObject>& Schema,
			TSharedPtr<FJsonValue>& OutValue,
			int32& Work,
			FString& OutError)
		{
			if (++Work > MaxGenerationWork)
			{
				return Fail(OutError, TEXT("contract_example_generation_limit"));
			}
			if (Schema->HasField(TEXT("allOf"))
				|| Schema->HasField(TEXT("anyOf"))
				|| Schema->HasField(TEXT("oneOf")))
			{
				return Fail(
					OutError,
					TEXT("contract_example_composition_unsupported"));
			}
			if (const TSharedPtr<FJsonValue>* Constant =
				Schema->Values.Find(TEXT("const")))
			{
				OutValue = *Constant;
				return OutValue.IsValid();
			}
			const TArray<TSharedPtr<FJsonValue>>* Enum = nullptr;
			if (Schema->TryGetArrayField(TEXT("enum"), Enum)
				&& Enum
				&& !Enum->IsEmpty())
			{
				OutValue = (*Enum)[0];
				return OutValue.IsValid();
			}

			FString Type;
			if (!TryType(Schema, Type))
			{
				return Fail(OutError, TEXT("contract_example_type_missing"));
			}
			if (Type == TEXT("object"))
			{
				const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
				const TArray<TSharedPtr<FJsonValue>>* Required = nullptr;
				const TSharedPtr<FJsonObject>* Properties = nullptr;
				if (Schema->TryGetArrayField(TEXT("required"), Required)
					&& Required
					&& !Required->IsEmpty())
				{
					if (!Schema->TryGetObjectField(TEXT("properties"), Properties)
						|| !Properties
						|| !Properties->IsValid())
					{
						return Fail(
							OutError,
							TEXT("contract_example_required_schema_missing"));
					}
					for (const TSharedPtr<FJsonValue>& RequiredValue : *Required)
					{
						FString Name;
						if (!RequiredValue.IsValid()
							|| !RequiredValue->TryGetString(Name))
						{
							return Fail(
								OutError,
								TEXT("contract_example_required_invalid"));
						}
						const TSharedPtr<FJsonObject>* PropertySchema = nullptr;
						if (!(*Properties)->TryGetObjectField(Name, PropertySchema)
							|| !PropertySchema
							|| !PropertySchema->IsValid())
						{
							return Fail(
								OutError,
								TEXT("contract_example_required_schema_missing:")
									+ Name);
						}
						TSharedPtr<FJsonValue> PropertyValue;
						if (!TryMinimalValue(
								PropertySchema->ToSharedRef(),
								PropertyValue,
								Work,
								OutError))
						{
							return false;
						}
						Object->SetField(Name, PropertyValue);
					}
				}
				OutValue = MakeShared<FJsonValueObject>(Object);
				return true;
			}
			if (Type == TEXT("array"))
			{
				double Minimum = 0.0;
				Schema->TryGetNumberField(TEXT("minItems"), Minimum);
				const int32 Count = FMath::Max(0, FMath::RoundToInt(Minimum));
				TArray<TSharedPtr<FJsonValue>> Values;
				if (Count > 0)
				{
					const TSharedPtr<FJsonObject>* ItemSchema = nullptr;
					if (!Schema->TryGetObjectField(TEXT("items"), ItemSchema)
						|| !ItemSchema
						|| !ItemSchema->IsValid())
					{
						return Fail(
							OutError,
							TEXT("contract_example_items_schema_missing"));
					}
					for (int32 Index = 0; Index < Count; ++Index)
					{
						TSharedPtr<FJsonValue> Item;
						if (!TryMinimalValue(
								ItemSchema->ToSharedRef(),
								Item,
								Work,
								OutError))
						{
							return false;
						}
						Values.Add(Item);
					}
				}
				OutValue = MakeShared<FJsonValueArray>(MoveTemp(Values));
				return true;
			}
			if (Type == TEXT("string"))
			{
				double Minimum = 0.0;
				Schema->TryGetNumberField(TEXT("minLength"), Minimum);
				OutValue = MakeShared<FJsonValueString>(
					FString::ChrN(FMath::Max(1, FMath::RoundToInt(Minimum)), TEXT('x')));
				return true;
			}
			if (Type == TEXT("integer") || Type == TEXT("number"))
			{
				double Value = 0.0;
				double Minimum = 0.0;
				double Maximum = 0.0;
				double ExclusiveMinimum = 0.0;
				double ExclusiveMaximum = 0.0;
				const bool bHasMinimum =
					Schema->TryGetNumberField(TEXT("minimum"), Minimum);
				const bool bHasMaximum =
					Schema->TryGetNumberField(TEXT("maximum"), Maximum);
				const bool bHasExclusiveMinimum = Schema->TryGetNumberField(
					TEXT("exclusiveMinimum"),
					ExclusiveMinimum);
				const bool bHasExclusiveMaximum = Schema->TryGetNumberField(
					TEXT("exclusiveMaximum"),
					ExclusiveMaximum);
				if (Type == TEXT("integer"))
				{
					if (bHasMinimum)
					{
						Value = FMath::Max(Value, FMath::CeilToDouble(Minimum));
					}
					if (bHasExclusiveMinimum)
					{
						Value = FMath::Max(
							Value,
							FMath::FloorToDouble(ExclusiveMinimum) + 1.0);
					}
					if (bHasMaximum)
					{
						Value = FMath::Min(Value, FMath::FloorToDouble(Maximum));
					}
					if (bHasExclusiveMaximum)
					{
						Value = FMath::Min(
							Value,
							FMath::CeilToDouble(ExclusiveMaximum) - 1.0);
					}
				}
				else
				{
					if (bHasMinimum)
					{
						Value = FMath::Max(Value, Minimum);
					}
					if (bHasExclusiveMinimum && Value <= ExclusiveMinimum)
					{
						const bool bHasUpper =
							bHasMaximum || bHasExclusiveMaximum;
						const double Upper = bHasMaximum && bHasExclusiveMaximum
							? FMath::Min(Maximum, ExclusiveMaximum)
							: bHasMaximum ? Maximum : ExclusiveMaximum;
						Value = bHasUpper
							? ExclusiveMinimum / 2.0 + Upper / 2.0
							: ExclusiveMinimum
								+ FMath::Max(1.0, FMath::Abs(ExclusiveMinimum) * 0.01);
					}
					if (bHasMaximum)
					{
						Value = FMath::Min(Value, Maximum);
					}
					if (bHasExclusiveMaximum && Value >= ExclusiveMaximum)
					{
						const bool bHasLower =
							bHasMinimum || bHasExclusiveMinimum;
						const double Lower = bHasMinimum && bHasExclusiveMinimum
							? FMath::Max(Minimum, ExclusiveMinimum)
							: bHasMinimum ? Minimum : ExclusiveMinimum;
						Value = bHasLower
							? Lower / 2.0 + ExclusiveMaximum / 2.0
							: ExclusiveMaximum
								- FMath::Max(1.0, FMath::Abs(ExclusiveMaximum) * 0.01);
					}
				}
				OutValue = MakeShared<FJsonValueNumber>(Value);
				return true;
			}
			if (Type == TEXT("boolean"))
			{
				OutValue = MakeShared<FJsonValueBoolean>(false);
				return true;
			}
			if (Type == TEXT("null"))
			{
				OutValue = MakeShared<FJsonValueNull>();
				return true;
			}
			return Fail(OutError, TEXT("contract_example_type_unsupported:") + Type);
		}

		bool TryPythonLiteral(
			const TSharedPtr<FJsonValue>& Value,
			FString& OutLiteral,
			FString& OutError)
		{
			if (!Value.IsValid())
			{
				return Fail(OutError, TEXT("contract_example_value_invalid"));
			}
			switch (Value->Type)
			{
			case EJson::Null:
				OutLiteral = TEXT("None");
				return true;
			case EJson::String:
				return Json::TryCanonicalize(Value, OutLiteral, OutError);
			case EJson::Boolean:
				OutLiteral = Value->AsBool() ? TEXT("True") : TEXT("False");
				return true;
			case EJson::Number:
				OutLiteral = FString::SanitizeFloat(Value->AsNumber());
				return true;
			case EJson::Array:
			{
				TArray<FString> Items;
				for (const TSharedPtr<FJsonValue>& Item : Value->AsArray())
				{
					FString Literal;
					if (!TryPythonLiteral(Item, Literal, OutError))
					{
						return false;
					}
					Items.Add(MoveTemp(Literal));
				}
				OutLiteral = TEXT("[") + FString::Join(Items, TEXT(",")) + TEXT("]");
				return true;
			}
			case EJson::Object:
			{
				TArray<FString> Keys;
				Value->AsObject()->Values.GetKeys(Keys);
				Keys.Sort();
				TArray<FString> Fields;
				for (const FString& Key : Keys)
				{
					FString KeyLiteral;
					FString Literal;
					if (!Json::TryCanonicalize(
							MakeShared<FJsonValueString>(Key),
							KeyLiteral,
							OutError)
						|| !TryPythonLiteral(
							Value->AsObject()->Values[Key],
							Literal,
							OutError))
					{
						return false;
					}
					Fields.Add(KeyLiteral + TEXT(":") + Literal);
				}
				OutLiteral = TEXT("{") + FString::Join(Fields, TEXT(",")) + TEXT("}");
				return true;
			}
			default:
				return Fail(OutError, TEXT("contract_example_json_type_unsupported"));
			}
		}
	}

	bool TryBuildMinimalContractExample(
		const FUeAgentContractSchema& Schema,
		FUeAgentContractValue& OutCanonicalInstance,
		FString& OutPythonLiteral,
		FString& OutError)
	{
		OutCanonicalInstance = FUeAgentContractValue(FString());
		OutPythonLiteral.Reset();
		OutError.Reset();
		TSharedPtr<FJsonObject> Root;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(
			Schema.GetCanonicalSchema().GetJsonValue());
		if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
		{
			return Fail(OutError, TEXT("contract_example_schema_parse_failed"));
		}
		int32 Work = 0;
		TSharedPtr<FJsonValue> Candidate;
		if (!TryMinimalValue(Root.ToSharedRef(), Candidate, Work, OutError))
		{
			return false;
		}
		FString CandidateJson;
		if (!Json::TryCanonicalize(Candidate, CandidateJson, OutError))
		{
			OutError = TEXT("contract_example_serialization_failed:") + OutError;
			return false;
		}
		TOptional<FUeAgentContractValue> Canonical;
		FUeAgentContractSchemaError ValidationError;
		if (!Schema.TryValidateAndCanonicalize(
				FUeAgentContractValue(MoveTemp(CandidateJson)),
				[&Work](
					const int64 WorkUnits,
					const FString& InstancePath,
					FUeAgentContractSchemaError& OutCheckpointError)
				{
					Work += static_cast<int32>(WorkUnits);
					if (Work <= MaxGenerationWork)
					{
						return true;
					}
					OutCheckpointError.Code =
						TEXT("contract_example_generation_limit");
					OutCheckpointError.InstancePath = InstancePath;
					return false;
				},
				Canonical,
				ValidationError)
			|| !Canonical.IsSet())
		{
			OutError = TEXT("contract_example_validation_failed:")
				+ ValidationError.Code + TEXT(":")
				+ ValidationError.InstancePath;
			return false;
		}
		TSharedPtr<FJsonValue> CanonicalValue;
		const TSharedRef<TJsonReader<>> CanonicalReader =
			TJsonReaderFactory<>::Create(Canonical->GetJsonValue());
		if (!FJsonSerializer::Deserialize(CanonicalReader, CanonicalValue)
			|| !TryPythonLiteral(CanonicalValue, OutPythonLiteral, OutError))
		{
			return false;
		}
		OutCanonicalInstance = Canonical.GetValue();
		return true;
	}
}

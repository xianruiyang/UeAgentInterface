#include "Core/Contract/Discovery/UeAgentContractExample.h"

#include "Core/Contract/UeAgentDiagnostic.h"

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
		FString ChildPath(const FString& Path, const FString& Field)
		{
			return Path + TEXT("/") + Field.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"));
		}

		bool Fail(FDiagnostic& OutError, const FString& Code, const FString& Reason, const FString& SchemaPath,
			const TSharedPtr<FJsonValue>& Actual = nullptr)
		{
			OutError = FDiagnostic::Failure(Code, Reason, TEXT("contract_example"));
			OutError.SchemaPath = SchemaPath;
			OutError.ActualValue = Actual;
			OutError.ActualValuePresent = Actual.IsValid();
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
			UE::UAI::Contract::FDiagnostic& OutError,
			const FString& SchemaPath = FString())
		{
			for (const TCHAR* Composition : {TEXT("oneOf"), TEXT("anyOf")})
			{
				const TArray<TSharedPtr<FJsonValue>>* Branches = nullptr;
				if (!Schema->TryGetArrayField(Composition, Branches) || !Branches)
				{
					continue;
				}
				FString SchemaJson;
				const TSharedRef<FJsonObject> SchemaObject = MakeShared<FJsonObject>();
				SchemaObject->Values = Schema->Values;
				TSharedPtr<const FUeAgentContractSchema> Compiled;
				FUeAgentContractSchemaError SchemaError;
				if (!Json::TryCanonicalize(SchemaObject, SchemaJson, OutError))
				{
					OutError = FDiagnostic::WrapFailure(TEXT("contract_example_composition_invalid"), OutError,
						TEXT("contract_example"), TEXT("The composed schema could not be encoded for example generation."));
					OutError.SchemaPath = SchemaPath;
					return false;
				}
				if (!FUeAgentContractSchema::TryCompile(FUeAgentContractValue(MoveTemp(SchemaJson)), Compiled, SchemaError))
				{
					OutError = FDiagnostic::WrapFailure(TEXT("contract_example_composition_invalid"), SchemaError,
						TEXT("contract_example"), TEXT("The composed schema is invalid; an example cannot be generated."));
					OutError.SchemaPath = SchemaPath;
					return false;
				}
				TArray<FDiagnostic> BranchErrors;
				for (int32 Index = 0; Index < Branches->Num(); ++Index)
				{
					const TSharedPtr<FJsonValue>& Branch = (*Branches)[Index];
					const FString BranchPath = ChildPath(ChildPath(SchemaPath, Composition), FString::FromInt(Index));
					const TSharedPtr<FJsonObject>* BranchObject = nullptr;
					TSharedPtr<FJsonValue> Candidate;
					FString CandidateJson;
					UE::UAI::Contract::FDiagnostic CandidateError;
					TOptional<FUeAgentContractValue> Canonical;
					FUeAgentContractSchemaError ValidationError;
					if (Branch.IsValid() && Branch->TryGetObject(BranchObject)
						&& BranchObject && BranchObject->IsValid()
						&& TryMinimalValue(BranchObject->ToSharedRef(), Candidate, CandidateError, BranchPath)
						&& Json::TryCanonicalize(Candidate, CandidateJson, CandidateError)
						&& Compiled->TryValidateAndCanonicalize(
							FUeAgentContractValue(MoveTemp(CandidateJson)),
							[](int64, const FString&, FUeAgentContractSchemaError&) { return true; },
							Canonical, ValidationError))
					{
						OutValue = MoveTemp(Candidate);
						OutError.Reset();
						return true;
					}
					if (!ValidationError.IsEmpty()) CandidateError = ValidationError;
					if (CandidateError.IsEmpty())
					{
						Fail(CandidateError, TEXT("contract_example_branch_invalid"),
							TEXT("Each composition branch must be a schema object."), BranchPath, Branch);
					}
					if (CandidateError.SchemaPath.IsEmpty()) CandidateError.SchemaPath = BranchPath;
					BranchErrors.Add(MoveTemp(CandidateError));
				}
				Fail(OutError, TEXT("contract_example_composition_no_valid_candidate"),
					TEXT("No generated branch example satisfies the composed schema."), ChildPath(SchemaPath, Composition));
				OutError.Diagnostics = MoveTemp(BranchErrors);
				return false;
			}
			if (Schema->HasField(TEXT("allOf")))
			{
				return Fail(
					OutError,
					TEXT("contract_example_composition_unsupported"),
					TEXT("Automatic example generation does not support allOf composition."), ChildPath(SchemaPath, TEXT("allOf")));
			}
			if (const TSharedPtr<FJsonValue>* Constant =
				Schema->Values.Find(TEXT("const")))
			{
				OutValue = *Constant;
				return OutValue.IsValid() || Fail(OutError, TEXT("contract_example_value_invalid"),
					TEXT("The schema constant has no JSON value."), ChildPath(SchemaPath, TEXT("const")));
			}
			const TArray<TSharedPtr<FJsonValue>>* Enum = nullptr;
			if (Schema->TryGetArrayField(TEXT("enum"), Enum)
				&& Enum
				&& !Enum->IsEmpty())
			{
				OutValue = (*Enum)[0];
				return OutValue.IsValid() || Fail(OutError, TEXT("contract_example_value_invalid"),
					TEXT("The first enum member has no JSON value."), ChildPath(SchemaPath, TEXT("enum")) + TEXT("/0"));
			}

			FString Type;
			if (!TryType(Schema, Type))
			{
				return Fail(OutError, TEXT("contract_example_type_missing"),
					TEXT("Example generation requires a string type or a non-empty array of string types."),
					ChildPath(SchemaPath, TEXT("type")), Schema->TryGetField(TEXT("type")));
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
						TEXT("contract_example_required_schema_missing"), TEXT("Required fields need a properties schema object."),
						ChildPath(SchemaPath, TEXT("properties")), Schema->TryGetField(TEXT("properties")));
					}
					for (int32 RequiredIndex = 0; RequiredIndex < Required->Num(); ++RequiredIndex)
					{
						const TSharedPtr<FJsonValue>& RequiredValue = (*Required)[RequiredIndex];
						FString Name;
						if (!RequiredValue.IsValid()
							|| !RequiredValue->TryGetString(Name))
						{
							return Fail(
								OutError,
								TEXT("contract_example_required_invalid"), TEXT("A required field name must be a string."),
								ChildPath(SchemaPath, TEXT("required")) + TEXT("/") + FString::FromInt(RequiredIndex), RequiredValue);
						}
						const TSharedPtr<FJsonObject>* PropertySchema = nullptr;
						if (!(*Properties)->TryGetObjectField(Name, PropertySchema)
							|| !PropertySchema
							|| !PropertySchema->IsValid())
						{
							return Fail(
								OutError,
								TEXT("contract_example_required_schema_missing:")
									+ Name, TEXT("The required field has no schema object."),
								ChildPath(ChildPath(SchemaPath, TEXT("properties")), Name), (*Properties)->TryGetField(Name));
						}
						TSharedPtr<FJsonValue> PropertyValue;
						if (!TryMinimalValue(
								PropertySchema->ToSharedRef(),
								PropertyValue,
								OutError, ChildPath(ChildPath(SchemaPath, TEXT("properties")), Name)))
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
							TEXT("contract_example_items_schema_missing"), TEXT("A non-empty array example requires an items schema object."),
							ChildPath(SchemaPath, TEXT("items")), Schema->TryGetField(TEXT("items")));
					}
					for (int32 Index = 0; Index < Count; ++Index)
					{
						TSharedPtr<FJsonValue> Item;
						if (!TryMinimalValue(
								ItemSchema->ToSharedRef(),
								Item,
								OutError, ChildPath(SchemaPath, TEXT("items"))))
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
			return Fail(OutError, TEXT("contract_example_type_unsupported:") + Type,
				TEXT("Example generation supports object, array, string, integer, number, boolean and null types."),
				ChildPath(SchemaPath, TEXT("type")), MakeShared<FJsonValueString>(Type));
		}

		bool TryPythonLiteral(
			const TSharedPtr<FJsonValue>& Value,
			FString& OutLiteral,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			if (!Value.IsValid())
			{
				return Fail(OutError, TEXT("contract_example_value_invalid"), TEXT("The generated example has no JSON value."), FString());
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
				return Fail(OutError, TEXT("contract_example_json_type_unsupported"),
					TEXT("The generated example contains a JSON type that cannot be represented as a Python literal."), FString(),
					MakeShared<FJsonValueNumber>(static_cast<int32>(Value->Type)));
			}
		}
	}

	bool TryBuildMinimalContractExample(
		const FUeAgentContractSchema& Schema,
		FUeAgentContractValue& OutCanonicalInstance,
		FString& OutPythonLiteral,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutCanonicalInstance = FUeAgentContractValue(FString());
		OutPythonLiteral.Reset();
		OutError.Reset();
		TSharedPtr<FJsonObject> Root;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(
			Schema.GetCanonicalSchema().GetJsonValue());
		if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
		{
			return Fail(OutError, TEXT("contract_example_schema_parse_failed"),
				TEXT("The canonical schema could not be decoded as a JSON object: ") + Reader->GetErrorMessage(), TEXT("/"));
		}
		TSharedPtr<FJsonValue> Candidate;
		if (!TryMinimalValue(Root.ToSharedRef(), Candidate, OutError))
		{
			return false;
		}
		FString CandidateJson;
		if (!Json::TryCanonicalize(Candidate, CandidateJson, OutError))
		{
			if (OutError.Stage.IsEmpty()) OutError.Stage = TEXT("contract_example");
			return false;
		}
		TOptional<FUeAgentContractValue> Canonical;
		FUeAgentContractSchemaError ValidationError;
		if (!Schema.TryValidateAndCanonicalize(
				FUeAgentContractValue(MoveTemp(CandidateJson)),
				[](
					const int64,
					const FString&,
					FUeAgentContractSchemaError&)
				{
					return true;
				},
				Canonical,
				ValidationError)
			|| !Canonical.IsSet())
		{
			OutError = FDiagnostic::WrapFailure(TEXT("contract_example_validation_failed"), ValidationError,
				TEXT("contract_example"), TEXT("The generated example does not satisfy its command schema."));
			return false;
		}
		TSharedPtr<FJsonValue> CanonicalValue;
		const TSharedRef<TJsonReader<>> CanonicalReader =
			TJsonReaderFactory<>::Create(Canonical->GetJsonValue());
		if (!FJsonSerializer::Deserialize(CanonicalReader, CanonicalValue))
		{
			return Fail(OutError, TEXT("contract_example_value_parse_failed"),
				TEXT("The validated example could not be decoded: ") + CanonicalReader->GetErrorMessage(), FString());
		}
		if (!TryPythonLiteral(CanonicalValue, OutPythonLiteral, OutError))
		{
			return false;
		}
		OutCanonicalInstance = Canonical.GetValue();
		return true;
	}
}

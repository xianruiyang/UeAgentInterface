#include "CommandRegistry/UeAgentImmutableCommandRegistry.h"

#include "Core/Contract/UeAgentDiagnostic.h"

#include "Core/Contract/UeAgentCommandJson.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace UE::UAI::CommandRegistry
{
	using namespace CommandContract;

	namespace
	{
		UE::UAI::Contract::FDiagnostic RegistrationFailure(const FString& Code, const FString& Reason,
			const FString& Command, const FString& Property, const TSharedPtr<FJsonValue>& Actual = nullptr)
		{
			auto Error = UE::UAI::Contract::FDiagnostic::Failure(Code, Reason, TEXT("command_registration"));
			Error.CandidateName = Command;
			Error.PropertyPath = Property;
			Error.SourceKind = TEXT("command_declaration");
			Error.ActualValue = Actual;
			return Error;
		}

		FString Clean(FString Value)
		{
			Value.TrimStartAndEndInline();
			return Value;
		}

		bool IsIdentifier(const FString& Value)
		{
			if (Value.IsEmpty()
				|| !(FChar::IsAlpha(Value[0]) || Value[0] == TEXT('_')))
			{
				return false;
			}
			for (const TCHAR Character : Value)
			{
				if (!(FChar::IsAlnum(Character)
					|| Character == TEXT('_')))
				{
					return false;
				}
			}
			return true;
		}

		bool IsCommandId(const FString& Value)
		{
			TArray<FString> Segments;
			Value.ParseIntoArray(Segments, TEXT("."), false);
			if (Segments.Num() < 2)
			{
				return false;
			}
			for (const FString& Segment : Segments)
			{
				if (!IsIdentifier(Segment))
				{
					return false;
				}
			}
			return FString::Join(Segments, TEXT(".")) == Value;
		}

		bool IsVariantId(const FString& Value)
		{
			if (Value.IsEmpty() || !FChar::IsAlnum(Value[0]))
			{
				return false;
			}
			for (const TCHAR Character : Value)
			{
				if (!(FChar::IsAlnum(Character)
					|| Character == TEXT('_')
					|| Character == TEXT('-')
					|| Character == TEXT('.')
					|| Character == TEXT(':')
					|| Character == TEXT('/')))
				{
					return false;
				}
			}
			return true;
		}

		bool IsWireType(const FString& Value)
		{
			static const TSet<FString> Types = {
				TEXT("boolean"),
				TEXT("integer"),
				TEXT("number"),
				TEXT("string"),
				TEXT("object"),
				TEXT("array"),
				TEXT("any"),
			};
			return Types.Contains(Value);
		}

		bool ValidateStrings(const TArray<FString>& Values, const FString& Code, const FString& Command,
			const FString& Property, UE::UAI::Contract::FDiagnostic& OutError)
		{
			if (Values.IsEmpty())
			{
				OutError = RegistrationFailure(Code, TEXT("The contract list must contain at least one entry."), Command, Property, MakeShared<FJsonValueNumber>(0));
				return false;
			}
			TSet<FString> Seen;
			for (int32 Index = 0; Index < Values.Num(); ++Index)
			{
				const FString& Value = Values[Index];
				if (Value.IsEmpty() || Seen.Contains(Value))
				{
					OutError = RegistrationFailure(Code, Value.IsEmpty() ? TEXT("The contract list contains an empty entry.") : TEXT("The contract list contains a duplicate entry."),
						Command, FString::Printf(TEXT("%s[%d]"), *Property, Index), MakeShared<FJsonValueString>(Value));
					return false;
				}
				Seen.Add(Value);
			}
			return true;
		}

		bool IsTutorialReference(const FString& Value)
		{
			for (const TCHAR Character : Value)
			{
				if (FChar::IsWhitespace(Character))
				{
					return false;
				}
			}
			TArray<FString> Sections;
			Value.ParseIntoArray(Sections, TEXT("#"), false);
			if (Sections.Num() != 2
				|| Sections[0].IsEmpty()
				|| Sections[1].IsEmpty())
			{
				return false;
			}
			const FString& Owner = Sections[0];
			return (Owner.StartsWith(TEXT("uai-docs:"))
					&& Owner.Len() > FCString::Strlen(TEXT("uai-docs:")))
				|| (Owner.StartsWith(TEXT("uai-skill:"))
					&& Owner.Len() > FCString::Strlen(TEXT("uai-skill:")));
		}

		bool ValidateTutorialReferences(const TArray<FString>& Values, const FString& Command, UE::UAI::Contract::FDiagnostic& OutError)
		{
			if (!ValidateStrings(Values, TEXT("command_tutorial_reference_invalid"), Command, TEXT("tutorial_references"), OutError))
			{
				return false;
			}
			for (int32 Index = 0; Index < Values.Num(); ++Index)
			{
				const FString& Value = Values[Index];
				if (!IsTutorialReference(Value))
				{
					OutError = RegistrationFailure(TEXT("command_tutorial_reference_invalid"), TEXT("A tutorial reference must be uai-docs:<owner>#<anchor> or uai-skill:<owner>#<anchor>, without whitespace."),
						Command, FString::Printf(TEXT("tutorial_references[%d]"), Index), MakeShared<FJsonValueString>(Value));
					return false;
				}
			}
			return true;
		}

		bool ValidatePurpose(const FUeAgentCommandPurposeDeclaration& Purpose, const FString& Code,
			const FString& Command, const FString& Property, UE::UAI::Contract::FDiagnostic& OutError)
		{
			const auto Required = [&](const FString& Value, const TCHAR* Field)
			{
				if (!Value.IsEmpty()) return true;
				OutError = RegistrationFailure(Code, TEXT("A required command purpose field is empty."), Command, Property + TEXT(".") + Field, MakeShared<FJsonValueString>(Value));
				return false;
			};
			return Required(Purpose.Summary, TEXT("summary"))
				&& Required(Purpose.Target, TEXT("target"))
				&& ValidateStrings(Purpose.Preconditions, Code, Command, Property + TEXT(".preconditions"), OutError)
				&& Required(Purpose.ParameterGuidance, TEXT("parameter_guidance"))
				&& Required(Purpose.ResultGuidance, TEXT("result_guidance"))
				&& Required(Purpose.FailureGuidance, TEXT("failure_guidance"))
				&& Required(Purpose.SideEffectGuidance, TEXT("side_effect_guidance"))
				&& Required(Purpose.MinimumExample, TEXT("minimum_example"))
				&& ValidateStrings(Purpose.RelatedFlows, Code, Command, Property + TEXT(".related_flows"), OutError)
				&& ValidateStrings(Purpose.Keywords, Code, Command, Property + TEXT(".keywords"), OutError);
		}

		void NormalizeStrings(TArray<FString>& Values)
		{
			for (FString& Value : Values)
			{
				Value = Clean(MoveTemp(Value));
			}
		}

		void NormalizeSchema(FString& SchemaJson)
		{
			SchemaJson = Clean(MoveTemp(SchemaJson));
			if (SchemaJson.IsEmpty())
			{
				return;
			}
			TSharedPtr<FJsonObject> Schema;
			UE::UAI::Contract::FDiagnostic Error;
			FString Canonical;
			if (Json::TryParseObject(SchemaJson, Schema, Error)
				&& Schema.IsValid()
				&& Json::TryCanonicalize(Schema, Canonical, Error))
			{
				SchemaJson = MoveTemp(Canonical);
			}
		}

		void NormalizePurpose(FUeAgentCommandPurposeDeclaration& Purpose)
		{
			Purpose.Summary = Clean(MoveTemp(Purpose.Summary));
			Purpose.Target = Clean(MoveTemp(Purpose.Target));
			Purpose.ParameterGuidance =
				Clean(MoveTemp(Purpose.ParameterGuidance));
			Purpose.ResultGuidance =
				Clean(MoveTemp(Purpose.ResultGuidance));
			Purpose.FailureGuidance =
				Clean(MoveTemp(Purpose.FailureGuidance));
			Purpose.SideEffectGuidance =
				Clean(MoveTemp(Purpose.SideEffectGuidance));
			Purpose.MinimumExample =
				Clean(MoveTemp(Purpose.MinimumExample));
			NormalizeStrings(Purpose.Preconditions);
			NormalizeStrings(Purpose.RelatedFlows);
			NormalizeStrings(Purpose.Keywords);
			NormalizeStrings(Purpose.Synonyms);
		}

		void Normalize(FUeAgentCommandDeclaration& Declaration)
		{
			Declaration.CommandId =
				Clean(MoveTemp(Declaration.CommandId));
			Declaration.PythonNamespace =
				Clean(MoveTemp(Declaration.PythonNamespace));
			Declaration.PythonFunction =
				Clean(MoveTemp(Declaration.PythonFunction));
			Declaration.PublicOwner =
				Clean(MoveTemp(Declaration.PublicOwner));
			NormalizeStrings(Declaration.StateLayers);
			for (FUeAgentCommandFieldDeclaration& Field :
				Declaration.InputFields)
			{
				Field.Name = Clean(MoveTemp(Field.Name));
				Field.WireType = Clean(MoveTemp(Field.WireType));
				Field.Description = Clean(MoveTemp(Field.Description));
				NormalizeSchema(Field.SchemaJson);
			}
			for (FUeAgentCommandFieldDeclaration& Field :
				Declaration.ResultFields)
			{
				Field.Name = Clean(MoveTemp(Field.Name));
				Field.WireType = Clean(MoveTemp(Field.WireType));
				Field.Description = Clean(MoveTemp(Field.Description));
				NormalizeSchema(Field.SchemaJson);
			}
			for (FUeAgentModelResultFieldDeclaration& Field :
				Declaration.ModelResultFields)
			{
				Field.Name = Clean(MoveTemp(Field.Name));
				NormalizeStrings(Field.SourcePath);
			}
			for (FUeAgentCommandErrorDeclaration& Error :
				Declaration.Errors)
			{
				Error.Code = Clean(MoveTemp(Error.Code));
				Error.Description = Clean(MoveTemp(Error.Description));
			}
			NormalizePurpose(Declaration.Purpose);
			for (FUeAgentCommandVariantDeclaration& Variant :
				Declaration.Variants)
			{
				Variant.VariantId = Clean(MoveTemp(Variant.VariantId));
				NormalizeSchema(Variant.RequestSchemaJson);
				NormalizePurpose(Variant.Purpose);
			}
			Declaration.Variants.Sort(
				[](const FUeAgentCommandVariantDeclaration& Left,
					const FUeAgentCommandVariantDeclaration& Right)
				{
					return Left.VariantId < Right.VariantId;
				});
			NormalizeStrings(Declaration.TutorialReferences);
		}

		bool ValidateFields(
			const TArray<FUeAgentCommandFieldDeclaration>& Fields,
			const FString& Command, const FString& Role,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			TSet<FString> Names;
			for (int32 Index = 0; Index < Fields.Num(); ++Index)
			{
				const auto& Field = Fields[Index];
				const FString Path = FString::Printf(TEXT("%s[%d]"), *Role, Index);
				if (!IsIdentifier(Field.Name)
					|| !IsWireType(Field.WireType)
					|| Field.Description.IsEmpty())
				{
					const bool bNameInvalid = !IsIdentifier(Field.Name);
					const bool bTypeInvalid = !IsWireType(Field.WireType);
					OutError = RegistrationFailure(TEXT("command_contract_incomplete"),
						bNameInvalid ? TEXT("The command field name is not a valid identifier.") : bTypeInvalid ? TEXT("The command field wire type is not supported.") : TEXT("The command field description is empty."),
						Command, Path + (bNameInvalid ? TEXT(".name") : bTypeInvalid ? TEXT(".wire_type") : TEXT(".description")),
						MakeShared<FJsonValueString>(bNameInvalid ? Field.Name : bTypeInvalid ? Field.WireType : Field.Description));
					return false;
				}
				if (!Field.SchemaJson.IsEmpty())
				{
					TSharedPtr<FJsonObject> Schema;
					UE::UAI::Contract::FDiagnostic SchemaError;
					if (!Json::TryParseObject(
							Field.SchemaJson,
							Schema,
							SchemaError)
						|| !Schema.IsValid())
					{
						OutError = UE::UAI::Contract::FDiagnostic::WrapFailure(TEXT("command_contract_field_schema_invalid"), SchemaError,
							TEXT("command_registration"), TEXT("The command field schema is not a valid JSON object."));
						OutError.CandidateName = Command;
						OutError.PropertyPath = Path + TEXT(".schema");
						return false;
					}
				}
				if (Names.Contains(Field.Name))
				{
					OutError = RegistrationFailure(TEXT("command_contract_field_duplicate"), TEXT("The command declares the same field name more than once."),
						Command, Path + TEXT(".name"), MakeShared<FJsonValueString>(Field.Name));
					return false;
				}
				Names.Add(Field.Name);
			}
			return true;
		}

		bool ValidateVariants(
			const TArray<FUeAgentCommandVariantDeclaration>& Variants,
			const FString& Command,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			TSet<FString> Identities;
			for (int32 Index = 0; Index < Variants.Num(); ++Index)
			{
				const auto& Variant = Variants[Index];
				const FString Path = FString::Printf(TEXT("variants[%d]"), Index);
				if (!IsVariantId(Variant.VariantId)
					|| Variant.RequestSchemaJson.IsEmpty())
				{
					OutError = RegistrationFailure(TEXT("command_variant_contract_incomplete"),
						!IsVariantId(Variant.VariantId) ? TEXT("The command variant ID is invalid.") : Variant.RequestSchemaJson.IsEmpty() ? TEXT("The command variant request schema is empty.") : TEXT("The command variant purpose is incomplete."),
						Command, Path + (!IsVariantId(Variant.VariantId) ? TEXT(".id") : Variant.RequestSchemaJson.IsEmpty() ? TEXT(".request_schema") : TEXT(".purpose")), MakeShared<FJsonValueString>(Variant.VariantId));
					return false;
				}
				if (!ValidatePurpose(Variant.Purpose, TEXT("command_variant_contract_incomplete"), Command, Path + TEXT(".purpose"), OutError)) return false;
				if (Identities.Contains(Variant.VariantId))
				{
					OutError = RegistrationFailure(TEXT("command_variant_contract_duplicate"), TEXT("The command declares the same variant ID more than once."),
						Command, Path + TEXT(".id"), MakeShared<FJsonValueString>(Variant.VariantId));
					return false;
				}
				TSharedPtr<FJsonObject> Schema;
				UE::UAI::Contract::FDiagnostic SchemaError;
				FString SchemaType;
				bool bAdditionalProperties = true;
				if (!Json::TryParseObject(
						Variant.RequestSchemaJson,
						Schema,
						SchemaError)
					|| !Schema.IsValid()
					|| !Schema->TryGetStringField(TEXT("type"), SchemaType)
					|| SchemaType != TEXT("object")
					|| !Schema->TryGetBoolField(
						TEXT("additionalProperties"), bAdditionalProperties)
					|| bAdditionalProperties)
				{
					OutError = UE::UAI::Contract::FDiagnostic::WrapFailure(TEXT("command_variant_request_schema_invalid"), SchemaError,
						TEXT("command_registration"), TEXT("A command variant schema must be a JSON object with type=object and additionalProperties=false."));
					OutError.CandidateName = Command;
					OutError.PropertyPath = Path + TEXT(".request_schema");
					if (Schema.IsValid())
					{
						const bool bTypeInvalid = SchemaType != TEXT("object");
						OutError.PropertyPath += bTypeInvalid ? TEXT(".type") : TEXT(".additionalProperties");
						OutError.ActualValue = Schema->TryGetField(bTypeInvalid ? TEXT("type") : TEXT("additionalProperties"));
						OutError.ActualValuePresent = OutError.ActualValue.IsValid();
						OutError.ExpectedValue = bTypeInvalid ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(TEXT("object"))) : MakeShared<FJsonValueBoolean>(false);
					}
					return false;
				}
				Identities.Add(Variant.VariantId);
			}
			return true;
		}

		bool Validate(
			const FUeAgentCommandDeclaration& Declaration,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			if (!IsCommandId(Declaration.CommandId)
				|| !IsIdentifier(Declaration.PythonNamespace)
				|| !IsIdentifier(Declaration.PythonFunction))
			{
				const bool bCommandInvalid = !IsCommandId(Declaration.CommandId);
				const bool bNamespaceInvalid = !IsIdentifier(Declaration.PythonNamespace);
				OutError = RegistrationFailure(TEXT("command_contract_identity_invalid"),
					bCommandInvalid ? TEXT("The command name must contain at least two valid dot-separated identifiers.") : TEXT("The Python namespace and function must be valid identifiers."),
					Declaration.CommandId, bCommandInvalid ? TEXT("command_id") : bNamespaceInvalid ? TEXT("python_namespace") : TEXT("python_function"),
					MakeShared<FJsonValueString>(bCommandInvalid ? Declaration.CommandId : bNamespaceInvalid ? Declaration.PythonNamespace : Declaration.PythonFunction));
				return false;
			}
			if (Declaration.Availability
					!= EUeAgentCommandAvailability::Enabled
				&& Declaration.Availability
					!= EUeAgentCommandAvailability::Dormant
				&& Declaration.Availability
					!= EUeAgentCommandAvailability::Disabled)
			{
				OutError = RegistrationFailure(TEXT("command_contract_availability_invalid"), TEXT("Command availability must be enabled, dormant or disabled."),
					Declaration.CommandId, TEXT("availability"), MakeShared<FJsonValueNumber>(static_cast<int32>(Declaration.Availability)));
				return false;
			}
			switch (Declaration.SideEffect)
			{
			case EUeAgentCommandSideEffect::None:
			case EUeAgentCommandSideEffect::EditorState:
			case EUeAgentCommandSideEffect::AssetState:
			case EUeAgentCommandSideEffect::WorldState:
			case EUeAgentCommandSideEffect::ExternalState:
				break;
			default:
				OutError = RegistrationFailure(TEXT("command_contract_side_effect_invalid"), TEXT("Command side effect must be none, editor_state, asset_state, world_state or external_state."),
					Declaration.CommandId, TEXT("side_effect"), MakeShared<FJsonValueNumber>(static_cast<int32>(Declaration.SideEffect)));
				return false;
			}
			if (Declaration.ContractVersion < 1
				|| Declaration.PublicOwner.IsEmpty()
				|| Declaration.ResultFields.IsEmpty()
				|| Declaration.Errors.IsEmpty()
				|| Declaration.StateLayers.IsEmpty())
			{
				const FString Field = Declaration.ContractVersion < 1 ? TEXT("contract_version") : Declaration.PublicOwner.IsEmpty() ? TEXT("public_owner") :
					Declaration.ResultFields.IsEmpty() ? TEXT("result_fields") : Declaration.Errors.IsEmpty() ? TEXT("errors") : Declaration.StateLayers.IsEmpty() ? TEXT("state_layers") : TEXT("purpose");
				OutError = RegistrationFailure(TEXT("command_contract_incomplete"), TEXT("The command declaration has an invalid version or a missing required contract section."), Declaration.CommandId, Field);
				return false;
			}
			if (!ValidatePurpose(Declaration.Purpose, TEXT("command_contract_incomplete"), Declaration.CommandId, TEXT("purpose"), OutError)
				|| !ValidateStrings(Declaration.StateLayers, TEXT("command_contract_state_layer_invalid"), Declaration.CommandId, TEXT("state_layers"), OutError)
				|| !ValidateTutorialReferences(Declaration.TutorialReferences, Declaration.CommandId, OutError)) return false;
			if (!ValidateFields(Declaration.InputFields, Declaration.CommandId, TEXT("input_fields"), OutError)
				|| !ValidateFields(Declaration.ResultFields, Declaration.CommandId, TEXT("result_fields"), OutError)
				|| !ValidateVariants(Declaration.Variants, Declaration.CommandId, OutError))
			{
				return false;
			}
			TSet<FString> ModelResultNames;
			for (const FUeAgentModelResultFieldDeclaration& Field :
				Declaration.ModelResultFields)
			{
				if (!IsIdentifier(Field.Name)
					|| Field.SourcePath.IsEmpty()
					|| ModelResultNames.Contains(Field.Name))
				{
					OutError = RegistrationFailure(TEXT("command_model_result_field_invalid"),
						!IsIdentifier(Field.Name) ? TEXT("The model result field name is invalid.") : Field.SourcePath.IsEmpty() ? TEXT("The model result field source path is empty.") : TEXT("The model result field name is duplicated."),
						Declaration.CommandId, TEXT("model_result_fields"), MakeShared<FJsonValueString>(Field.Name));
					return false;
				}
				for (const FString& Segment : Field.SourcePath)
				{
					if (Segment.IsEmpty())
					{
						OutError = RegistrationFailure(TEXT("command_model_result_path_invalid"), TEXT("The model result source path contains an empty segment."),
							Declaration.CommandId, TEXT("model_result_fields.") + Field.Name + TEXT(".source_path"), MakeShared<FJsonValueString>(FString::Join(Field.SourcePath, TEXT("/"))));
						return false;
					}
				}
				ModelResultNames.Add(Field.Name);
			}
			TSet<FString> ErrorCodes;
			for (const FUeAgentCommandErrorDeclaration& Error :
				Declaration.Errors)
			{
				if (!IsIdentifier(Error.Code)
					|| Error.Description.IsEmpty())
				{
					OutError = RegistrationFailure(TEXT("command_contract_incomplete"), !IsIdentifier(Error.Code) ? TEXT("The declared error code is not a valid identifier.") : TEXT("The declared error has no description."),
						Declaration.CommandId, TEXT("errors"), MakeShared<FJsonValueString>(Error.Code));
					return false;
				}
				if (ErrorCodes.Contains(Error.Code))
				{
					OutError = RegistrationFailure(TEXT("command_contract_error_duplicate"), TEXT("The command declares the same error code more than once."),
						Declaration.CommandId, TEXT("errors"), MakeShared<FJsonValueString>(Error.Code));
					return false;
				}
				ErrorCodes.Add(Error.Code);
			}
			return true;
		}

		const TCHAR* AvailabilityText(
			const EUeAgentCommandAvailability Value)
		{
			switch (Value)
			{
			case EUeAgentCommandAvailability::Enabled:
				return TEXT("enabled");
			case EUeAgentCommandAvailability::Dormant:
				return TEXT("dormant");
			case EUeAgentCommandAvailability::Disabled:
				return TEXT("disabled");
			default:
				return TEXT("");
			}
		}

		const TCHAR* SideEffectText(
			const EUeAgentCommandSideEffect Value)
		{
			switch (Value)
			{
			case EUeAgentCommandSideEffect::None:
				return TEXT("none");
			case EUeAgentCommandSideEffect::EditorState:
				return TEXT("editor_state");
			case EUeAgentCommandSideEffect::AssetState:
				return TEXT("asset_state");
			case EUeAgentCommandSideEffect::WorldState:
				return TEXT("world_state");
			case EUeAgentCommandSideEffect::ExternalState:
				return TEXT("external_state");
			default:
				return TEXT("");
			}
		}

		TArray<TSharedPtr<FJsonValue>> StringArray(
			const TArray<FString>& Values)
		{
			TArray<TSharedPtr<FJsonValue>> Result;
			Result.Reserve(Values.Num());
			for (const FString& Value : Values)
			{
				Result.Add(MakeShared<FJsonValueString>(Value));
			}
			return Result;
		}

		TSharedPtr<FJsonObject> FieldObject(
			const FUeAgentCommandFieldDeclaration& Field)
		{
			const TSharedPtr<FJsonObject> Result =
				MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("name"), Field.Name);
			Result->SetStringField(TEXT("wire_type"), Field.WireType);
			Result->SetBoolField(TEXT("required"), Field.bRequired);
			Result->SetBoolField(TEXT("nullable"), Field.bNullable);
			Result->SetStringField(
				TEXT("description"),
				Field.Description);
			if (!Field.SchemaJson.IsEmpty())
			{
				TSharedPtr<FJsonObject> Schema;
				UE::UAI::Contract::FDiagnostic Error;
				if (Json::TryParseObject(
						Field.SchemaJson,
						Schema,
						Error)
					&& Schema.IsValid())
				{
					Result->SetObjectField(TEXT("schema"), Schema);
				}
			}
			return Result;
		}

		TArray<TSharedPtr<FJsonValue>> FieldArray(
			const TArray<FUeAgentCommandFieldDeclaration>& Fields)
		{
			TArray<TSharedPtr<FJsonValue>> Result;
			Result.Reserve(Fields.Num());
			for (const FUeAgentCommandFieldDeclaration& Field : Fields)
			{
				Result.Add(MakeShared<FJsonValueObject>(
					FieldObject(Field)));
			}
			return Result;
		}

		TArray<TSharedPtr<FJsonValue>> ModelResultFieldArray(
			const TArray<FUeAgentModelResultFieldDeclaration>& Fields)
		{
			TArray<TSharedPtr<FJsonValue>> Result;
			Result.Reserve(Fields.Num());
			for (const FUeAgentModelResultFieldDeclaration& Field : Fields)
			{
				const TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
				Object->SetStringField(TEXT("name"), Field.Name);
				Object->SetArrayField(TEXT("source_path"), StringArray(Field.SourcePath));
				Result.Add(MakeShared<FJsonValueObject>(Object));
			}
			return Result;
		}

		TArray<TSharedPtr<FJsonValue>> ErrorArray(
			const TArray<FUeAgentCommandErrorDeclaration>& Errors)
		{
			TArray<TSharedPtr<FJsonValue>> Result;
			Result.Reserve(Errors.Num());
			for (const FUeAgentCommandErrorDeclaration& Error : Errors)
			{
				const TSharedPtr<FJsonObject> Object =
					MakeShared<FJsonObject>();
				Object->SetStringField(TEXT("code"), Error.Code);
				Object->SetStringField(
					TEXT("description"),
					Error.Description);
				Result.Add(MakeShared<FJsonValueObject>(Object));
			}
			return Result;
		}

		TSharedPtr<FJsonObject> PurposeObject(
			const FUeAgentCommandPurposeDeclaration& Purpose)
		{
			const TSharedPtr<FJsonObject> Result =
				MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("summary"), Purpose.Summary);
			Result->SetStringField(TEXT("target"), Purpose.Target);
			Result->SetArrayField(
				TEXT("preconditions"),
				StringArray(Purpose.Preconditions));
			Result->SetStringField(
				TEXT("parameter_guidance"),
				Purpose.ParameterGuidance);
			Result->SetStringField(
				TEXT("result_guidance"),
				Purpose.ResultGuidance);
			Result->SetStringField(
				TEXT("failure_guidance"),
				Purpose.FailureGuidance);
			Result->SetStringField(
				TEXT("side_effect_guidance"),
				Purpose.SideEffectGuidance);
			Result->SetStringField(
				TEXT("minimum_example"),
				Purpose.MinimumExample);
			Result->SetArrayField(
				TEXT("related_flows"),
				StringArray(Purpose.RelatedFlows));
			Result->SetArrayField(
				TEXT("keywords"),
				StringArray(Purpose.Keywords));
			Result->SetArrayField(
				TEXT("synonyms"),
				StringArray(Purpose.Synonyms));
			return Result;
		}

		TArray<TSharedPtr<FJsonValue>> VariantArray(
			const TArray<FUeAgentCommandVariantDeclaration>& Variants,
			const bool bIncludePurpose)
		{
			TArray<TSharedPtr<FJsonValue>> Result;
			Result.Reserve(Variants.Num());
			for (const FUeAgentCommandVariantDeclaration& Variant : Variants)
			{
				const TSharedPtr<FJsonObject> Object =
					MakeShared<FJsonObject>();
				Object->SetStringField(TEXT("variant_id"), Variant.VariantId);
				TSharedPtr<FJsonObject> RequestSchema;
				UE::UAI::Contract::FDiagnostic Error;
				if (Json::TryParseObject(
						Variant.RequestSchemaJson,
						RequestSchema,
						Error)
					&& RequestSchema.IsValid())
				{
					Object->SetObjectField(
						TEXT("request_schema"), RequestSchema);
				}
				if (bIncludePurpose)
				{
					Object->SetObjectField(
						TEXT("purpose"), PurposeObject(Variant.Purpose));
				}
				Result.Add(MakeShared<FJsonValueObject>(Object));
			}
			return Result;
		}

		TSharedPtr<FJsonObject> ContractObject(
			const FUeAgentCommandDeclaration& Declaration,
			const bool bIncludePurpose,
			const FString& ContractHash = FString())
		{
			const TSharedPtr<FJsonObject> Result =
				MakeShared<FJsonObject>();
			Result->SetStringField(
				TEXT("command_id"),
				Declaration.CommandId);
			Result->SetStringField(
				TEXT("python_namespace"),
				Declaration.PythonNamespace);
			Result->SetStringField(
				TEXT("python_function"),
				Declaration.PythonFunction);
			Result->SetNumberField(
				TEXT("contract_version"),
				Declaration.ContractVersion);
			if (!ContractHash.IsEmpty())
			{
				Result->SetStringField(
					TEXT("contract_hash"),
					ContractHash);
			}
			Result->SetStringField(
				TEXT("public_owner"),
				Declaration.PublicOwner);
			Result->SetStringField(
				TEXT("availability"),
				AvailabilityText(Declaration.Availability));
			Result->SetStringField(
				TEXT("side_effect"),
				SideEffectText(Declaration.SideEffect));
			Result->SetArrayField(
				TEXT("state_layers"),
				StringArray(Declaration.StateLayers));
			Result->SetArrayField(
				TEXT("input_fields"),
				FieldArray(Declaration.InputFields));
			Result->SetArrayField(
				TEXT("result_fields"),
				FieldArray(Declaration.ResultFields));
			if (!Declaration.ModelResultFields.IsEmpty())
			{
				Result->SetArrayField(
					TEXT("model_result_fields"),
					ModelResultFieldArray(Declaration.ModelResultFields));
			}
			Result->SetArrayField(
				TEXT("errors"),
				ErrorArray(Declaration.Errors));
			if (bIncludePurpose)
			{
				Result->SetObjectField(
					TEXT("purpose"),
					PurposeObject(Declaration.Purpose));
			}
			Result->SetArrayField(
				TEXT("variants"),
				VariantArray(
					Declaration.Variants,
					bIncludePurpose));
			return Result;
		}

		TSharedPtr<FJsonObject> RegistryCommandObject(
			const FUeAgentCommandDeclaration& Declaration,
			const FString& ContractHash)
		{
			const TSharedPtr<FJsonObject> Result =
				ContractObject(Declaration, true, ContractHash);
			Result->SetArrayField(
				TEXT("tutorial_references"),
				StringArray(Declaration.TutorialReferences));
			return Result;
		}

		FString ContractHash(
			const FUeAgentCommandDeclaration& Declaration, UE::UAI::Contract::FDiagnostic& Error)
		{
			FString Canonical;
			if (!Json::TryCanonicalize(
				ContractObject(Declaration, false),
				Canonical,
				Error))
			{
				return {};
			}
			return Json::Sha256Utf8(Canonical);
		}

		bool IsSha256(const FString& Value)
		{
			return Value.Len() == 71
				&& Value.StartsWith(TEXT("sha256:"));
		}
	}

	FUeAgentImmutableCommandRegistry::FUeAgentImmutableCommandRegistry(
		TArray<FUeAgentFrozenCommandContract> InContracts,
		FString InRegistryHash)
		: Contracts(MoveTemp(InContracts))
		, RegistryHash(MoveTemp(InRegistryHash))
	{
		for (int32 Index = 0; Index < Contracts.Num(); ++Index)
		{
			IndexByCommandId.Add(Contracts[Index].GetCommandId(), Index);
		}
	}

	bool FUeAgentImmutableCommandRegistry::TryFreeze(
		TArray<FUeAgentCommandDeclaration> Declarations,
		TSharedPtr<const FUeAgentImmutableCommandRegistry>& OutRegistry,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutRegistry.Reset();
		OutError.Reset();
		if (Declarations.IsEmpty())
		{
			OutError = RegistrationFailure(TEXT("command_contract_registry_empty"), TEXT("The command registry contains no declarations."), FString(), TEXT("commands"), MakeShared<FJsonValueNumber>(0));
			return false;
		}
		for (FUeAgentCommandDeclaration& Declaration : Declarations)
		{
			Normalize(Declaration);
			if (!Validate(Declaration, OutError))
			{
				return false;
			}
		}
		Declarations.Sort(
			[](const FUeAgentCommandDeclaration& Left,
			   const FUeAgentCommandDeclaration& Right)
			{
				return Left.CommandId < Right.CommandId;
			});

		TSet<FString> PythonBindings;
		for (int32 Index = 0; Index < Declarations.Num(); ++Index)
		{
			const FUeAgentCommandDeclaration& Declaration =
				Declarations[Index];
			if (Index > 0
				&& Declarations[Index - 1].CommandId
					== Declaration.CommandId)
			{
				OutError = RegistrationFailure(TEXT("command_contract_id_duplicate"), TEXT("The command name is registered more than once."), Declaration.CommandId, TEXT("command_id"), MakeShared<FJsonValueString>(Declaration.CommandId));
				return false;
			}
			const FString PythonBinding =
				Declaration.PythonNamespace
				+ TEXT(".")
				+ Declaration.PythonFunction;
			if (PythonBindings.Contains(PythonBinding))
			{
				OutError = RegistrationFailure(TEXT("command_python_binding_duplicate"), TEXT("The Python binding is registered by more than one command."), Declaration.CommandId, TEXT("python_binding"), MakeShared<FJsonValueString>(PythonBinding));
				return false;
			}
			PythonBindings.Add(PythonBinding);
		}

		TArray<FUeAgentFrozenCommandContract> Frozen;
		Frozen.Reserve(Declarations.Num());
		for (FUeAgentCommandDeclaration& Declaration : Declarations)
		{
			UE::UAI::Contract::FDiagnostic HashError;
			const FString Hash = ContractHash(Declaration, HashError);
			if (!IsSha256(Hash))
			{
				OutError = RegistrationFailure(TEXT("command_contract_hash_failed"), TEXT("The command contract hash could not be computed."), Declaration.CommandId, TEXT("contract"));
				if (!HashError.IsEmpty())
				{
					OutError.CauseCode = HashError.Code;
					OutError.PublicCause = MakeShared<UE::UAI::Contract::FDiagnostic>(MoveTemp(HashError));
				}
				return false;
			}
			Frozen.Add(FUeAgentFrozenCommandContract(
				MoveTemp(Declaration),
				Hash));
		}

		TArray<TSharedPtr<FJsonValue>> CommandValues;
		CommandValues.Reserve(Frozen.Num());
		for (const FUeAgentFrozenCommandContract& Contract : Frozen)
		{
			FUeAgentCommandDeclaration Projection;
			Projection.CommandId = Contract.GetCommandId();
			Projection.PythonNamespace = Contract.GetPythonNamespace();
			Projection.PythonFunction = Contract.GetPythonFunction();
			Projection.ContractVersion = Contract.GetContractVersion();
			Projection.PublicOwner = Contract.GetPublicOwner();
			Projection.Availability = Contract.GetAvailability();
			Projection.SideEffect = Contract.GetSideEffect();
			Projection.StateLayers = Contract.GetStateLayers();
			Projection.InputFields = Contract.GetInputFields();
			Projection.ResultFields = Contract.GetResultFields();
			Projection.ModelResultFields = Contract.GetModelResultFields();
			Projection.Errors = Contract.GetErrors();
			Projection.Purpose = Contract.GetPurpose();
			Projection.Variants = Contract.GetVariants();
			Projection.TutorialReferences =
				Contract.GetTutorialReferences();
			CommandValues.Add(MakeShared<FJsonValueObject>(
				RegistryCommandObject(
					Projection,
					Contract.GetContractHash())));
		}
		FString Canonical;
		if (!Json::TryCanonicalize(
			MakeShared<FJsonValueArray>(CommandValues),
			Canonical,
			OutError))
		{
			return false;
		}
		const FString RegistryHash = Json::Sha256Utf8(Canonical);
		if (!IsSha256(RegistryHash))
		{
			OutError = RegistrationFailure(TEXT("command_registry_hash_failed"), TEXT("The frozen command registry hash could not be computed."), FString(), TEXT("commands"));
			return false;
		}
		OutRegistry = MakeShareable(
			new FUeAgentImmutableCommandRegistry(
				MoveTemp(Frozen),
				RegistryHash));
		return true;
	}

	const TArray<FUeAgentFrozenCommandContract>&
	FUeAgentImmutableCommandRegistry::GetContracts() const
	{
		return Contracts;
	}

	const FUeAgentFrozenCommandContract*
	FUeAgentImmutableCommandRegistry::FindExact(
		const FString& CommandId) const
	{
		const int32* Index = IndexByCommandId.Find(CommandId);
		return Index ? &Contracts[*Index] : nullptr;
	}

	const FString&
	FUeAgentImmutableCommandRegistry::GetRegistryHash() const
	{
		return RegistryHash;
	}

	bool FUeAgentImmutableCommandRegistry::BuildProjection(
		FString& OutProjection,
		UE::UAI::Contract::FDiagnostic& OutError) const
	{
		OutProjection.Reset();
		OutError.Reset();
		TArray<TSharedPtr<FJsonValue>> Commands;
		Commands.Reserve(Contracts.Num());
		for (const FUeAgentFrozenCommandContract& Contract : Contracts)
		{
			FUeAgentCommandDeclaration Declaration;
			Declaration.CommandId = Contract.GetCommandId();
			Declaration.PythonNamespace = Contract.GetPythonNamespace();
			Declaration.PythonFunction = Contract.GetPythonFunction();
			Declaration.ContractVersion = Contract.GetContractVersion();
			Declaration.PublicOwner = Contract.GetPublicOwner();
			Declaration.Availability = Contract.GetAvailability();
			Declaration.SideEffect = Contract.GetSideEffect();
			Declaration.StateLayers = Contract.GetStateLayers();
			Declaration.InputFields = Contract.GetInputFields();
			Declaration.ResultFields = Contract.GetResultFields();
			Declaration.ModelResultFields = Contract.GetModelResultFields();
			Declaration.Errors = Contract.GetErrors();
			Declaration.Purpose = Contract.GetPurpose();
			Declaration.Variants = Contract.GetVariants();
			Declaration.TutorialReferences =
				Contract.GetTutorialReferences();
			Commands.Add(MakeShared<FJsonValueObject>(
				RegistryCommandObject(
					Declaration,
					Contract.GetContractHash())));
		}
		const TSharedPtr<FJsonObject> Source = MakeShared<FJsonObject>();
		Source->SetStringField(
			TEXT("kind"),
			TEXT("cpp_immutable_command_registry"));
		const TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(
			TEXT("schema"),
			TEXT("uai.immutable_command_registry.v1"));
		Root->SetObjectField(TEXT("source"), Source);
		Root->SetStringField(TEXT("registry_hash"), RegistryHash);
		Root->SetArrayField(TEXT("commands"), Commands);
		return Json::TryCanonicalize(Root, OutProjection, OutError);
	}
}

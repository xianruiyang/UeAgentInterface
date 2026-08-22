#include "CommandRegistry/UeAgentImmutableCommandRegistry.h"

#include "Core/Contract/UeAgentCommandJson.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace UE::UAI::CommandRegistry
{
	using namespace CommandContract;

	namespace
	{
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

		bool HasNonEmptyUniqueStrings(const TArray<FString>& Values)
		{
			if (Values.IsEmpty())
			{
				return false;
			}
			TSet<FString> Seen;
			for (const FString& Value : Values)
			{
				if (Value.IsEmpty() || Seen.Contains(Value))
				{
					return false;
				}
				Seen.Add(Value);
			}
			return true;
		}

		bool PurposeComplete(
			const FUeAgentCommandPurposeDeclaration& Purpose)
		{
			return !Purpose.Summary.IsEmpty()
				&& !Purpose.Target.IsEmpty()
				&& HasNonEmptyUniqueStrings(Purpose.Preconditions)
				&& !Purpose.ParameterGuidance.IsEmpty()
				&& !Purpose.ResultGuidance.IsEmpty()
				&& !Purpose.FailureGuidance.IsEmpty()
				&& !Purpose.SideEffectGuidance.IsEmpty()
				&& !Purpose.MinimumExample.IsEmpty()
				&& HasNonEmptyUniqueStrings(Purpose.RelatedFlows)
				&& HasNonEmptyUniqueStrings(Purpose.Keywords);
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
			FString Error;
			FString Canonical;
			if (Json::TryParseObject(SchemaJson, Schema, Error)
				&& Schema.IsValid()
				&& Json::TryCanonicalize(Schema, Canonical, Error))
			{
				SchemaJson = MoveTemp(Canonical);
			}
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
			for (FUeAgentCommandErrorDeclaration& Error :
				Declaration.Errors)
			{
				Error.Code = Clean(MoveTemp(Error.Code));
				Error.Description = Clean(MoveTemp(Error.Description));
			}
			FUeAgentCommandPurposeDeclaration& Purpose =
				Declaration.Purpose;
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

		bool ValidateFields(
			const TArray<FUeAgentCommandFieldDeclaration>& Fields,
			FString& OutError)
		{
			TSet<FString> Names;
			for (const FUeAgentCommandFieldDeclaration& Field : Fields)
			{
				if (!IsIdentifier(Field.Name)
					|| !IsWireType(Field.WireType)
					|| Field.Description.IsEmpty())
				{
					OutError = TEXT("command_contract_incomplete");
					return false;
				}
				if (!Field.SchemaJson.IsEmpty())
				{
					TSharedPtr<FJsonObject> Schema;
					FString SchemaError;
					if (!Json::TryParseObject(
							Field.SchemaJson,
							Schema,
							SchemaError)
						|| !Schema.IsValid())
					{
						OutError = TEXT("command_contract_field_schema_invalid");
						return false;
					}
				}
				if (Names.Contains(Field.Name))
				{
					OutError = TEXT("command_contract_field_duplicate");
					return false;
				}
				Names.Add(Field.Name);
			}
			return true;
		}

		bool Validate(
			const FUeAgentCommandDeclaration& Declaration,
			FString& OutError)
		{
			if (!IsCommandId(Declaration.CommandId)
				|| !IsIdentifier(Declaration.PythonNamespace)
				|| !IsIdentifier(Declaration.PythonFunction))
			{
				OutError = TEXT("command_contract_identity_invalid");
				return false;
			}
			if (Declaration.Availability
					!= EUeAgentCommandAvailability::Enabled
				&& Declaration.Availability
					!= EUeAgentCommandAvailability::Disabled)
			{
				OutError = TEXT("command_contract_availability_invalid");
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
				OutError = TEXT("command_contract_side_effect_invalid");
				return false;
			}
			if (Declaration.ContractVersion < 1
				|| Declaration.PublicOwner.IsEmpty()
				|| Declaration.ResultFields.IsEmpty()
				|| Declaration.Errors.IsEmpty()
				|| Declaration.StateLayers.IsEmpty()
				|| !PurposeComplete(Declaration.Purpose))
			{
				OutError = TEXT("command_contract_incomplete");
				return false;
			}
			if (!HasNonEmptyUniqueStrings(Declaration.StateLayers))
			{
				OutError = TEXT("command_contract_state_layer_invalid");
				return false;
			}
			if (!ValidateFields(Declaration.InputFields, OutError)
				|| !ValidateFields(Declaration.ResultFields, OutError))
			{
				return false;
			}
			TSet<FString> ErrorCodes;
			for (const FUeAgentCommandErrorDeclaration& Error :
				Declaration.Errors)
			{
				if (!IsIdentifier(Error.Code)
					|| Error.Description.IsEmpty())
				{
					OutError = TEXT("command_contract_incomplete");
					return false;
				}
				if (ErrorCodes.Contains(Error.Code))
				{
					OutError = TEXT("command_contract_error_duplicate");
					return false;
				}
				ErrorCodes.Add(Error.Code);
			}
			return true;
		}

		const TCHAR* AvailabilityText(
			const EUeAgentCommandAvailability Value)
		{
			return Value == EUeAgentCommandAvailability::Enabled
				? TEXT("enabled")
				: TEXT("disabled");
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
				FString Error;
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

		TSharedPtr<FJsonObject> ContractObject(
			const FUeAgentCommandDeclaration& Declaration,
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
			Result->SetArrayField(
				TEXT("errors"),
				ErrorArray(Declaration.Errors));
			Result->SetObjectField(
				TEXT("purpose"),
				PurposeObject(Declaration.Purpose));
			return Result;
		}

		FString ContractHash(
			const FUeAgentCommandDeclaration& Declaration)
		{
			FString Canonical;
			FString Error;
			if (!Json::TryCanonicalize(
				ContractObject(Declaration),
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
		FString& OutError)
	{
		OutRegistry.Reset();
		OutError.Reset();
		if (Declarations.IsEmpty())
		{
			OutError = TEXT("command_contract_registry_empty");
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
				OutError = TEXT("command_contract_id_duplicate");
				return false;
			}
			const FString PythonBinding =
				Declaration.PythonNamespace
				+ TEXT(".")
				+ Declaration.PythonFunction;
			if (PythonBindings.Contains(PythonBinding))
			{
				OutError = TEXT("command_python_binding_duplicate");
				return false;
			}
			PythonBindings.Add(PythonBinding);
		}

		TArray<FUeAgentFrozenCommandContract> Frozen;
		Frozen.Reserve(Declarations.Num());
		for (FUeAgentCommandDeclaration& Declaration : Declarations)
		{
			const FString Hash = ContractHash(Declaration);
			if (!IsSha256(Hash))
			{
				OutError = TEXT("command_contract_hash_failed");
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
			Projection.Errors = Contract.GetErrors();
			Projection.Purpose = Contract.GetPurpose();
			CommandValues.Add(MakeShared<FJsonValueObject>(
				ContractObject(Projection, Contract.GetContractHash())));
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
			OutError = TEXT("command_registry_hash_failed");
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
		FString& OutError) const
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
			Declaration.Errors = Contract.GetErrors();
			Declaration.Purpose = Contract.GetPurpose();
			Commands.Add(MakeShared<FJsonValueObject>(
				ContractObject(Declaration, Contract.GetContractHash())));
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

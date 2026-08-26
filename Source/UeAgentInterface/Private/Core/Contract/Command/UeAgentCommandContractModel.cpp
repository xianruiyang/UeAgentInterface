#include "Core/Contract/Command/UeAgentCommandContractModel.h"

#include "Core/Contract/UeAgentCommandJson.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace UE::UAI::CommandContract
{
	bool TryBuildCommandVariantRequestSchema(
		const TArray<FUeAgentCommandVariantRequestFieldDeclaration>& Fields,
		FString& OutSchemaJson,
		FString& OutError)
	{
		OutSchemaJson.Reset();
		OutError.Reset();
		if (Fields.IsEmpty())
		{
			OutError = TEXT("command_variant_request_fields_empty");
			return false;
		}
		const TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
		const TSharedRef<FJsonObject> Definitions = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> Required;
		TSet<FString> Names;
		for (const FUeAgentCommandVariantRequestFieldDeclaration& Field : Fields)
		{
			if (Field.Name.IsEmpty() || Names.Contains(Field.Name))
			{
				OutError = TEXT("command_variant_request_field_invalid:")
					+ Field.Name;
				return false;
			}
			TSharedPtr<FJsonObject> FieldSchema;
			if (!Json::TryParseObject(
					Field.SchemaJson,
					FieldSchema,
					OutError)
				|| !FieldSchema.IsValid())
			{
				OutError = TEXT("command_variant_request_field_schema_invalid:")
					+ Field.Name + TEXT(":") + OutError;
				return false;
			}
			const TSharedPtr<FJsonObject>* FieldDefinitions = nullptr;
			if (FieldSchema->TryGetObjectField(
					TEXT("$defs"), FieldDefinitions)
				&& FieldDefinitions
				&& FieldDefinitions->IsValid())
			{
				for (const TPair<FString, TSharedPtr<FJsonValue>>& Definition :
					(*FieldDefinitions)->Values)
				{
					if (Definitions->HasField(Definition.Key))
					{
						OutError = TEXT(
							"command_variant_request_definition_duplicate:")
							+ Definition.Key;
						return false;
					}
					Definitions->SetField(Definition.Key, Definition.Value);
				}
				FieldSchema->RemoveField(TEXT("$defs"));
			}
			Properties->SetObjectField(Field.Name, FieldSchema);
			if (Field.bRequired)
			{
				Required.Add(MakeShared<FJsonValueString>(Field.Name));
			}
			Names.Add(Field.Name);
		}
		const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("type"), TEXT("object"));
		Root->SetBoolField(TEXT("additionalProperties"), false);
		Root->SetObjectField(TEXT("properties"), Properties);
		Root->SetArrayField(TEXT("required"), MoveTemp(Required));
		if (!Definitions->Values.IsEmpty())
		{
			Root->SetObjectField(TEXT("$defs"), Definitions);
		}
		return Json::TryCanonicalize(Root, OutSchemaJson, OutError);
	}

	FUeAgentFrozenCommandContract::FUeAgentFrozenCommandContract(
		FUeAgentCommandDeclaration InDeclaration,
		FString InContractHash)
		: Declaration(MoveTemp(InDeclaration))
		, ContractHash(MoveTemp(InContractHash))
	{
	}

	const FString& FUeAgentFrozenCommandContract::GetCommandId() const
	{
		return Declaration.CommandId;
	}

	const FString&
	FUeAgentFrozenCommandContract::GetPythonNamespace() const
	{
		return Declaration.PythonNamespace;
	}

	const FString& FUeAgentFrozenCommandContract::GetPythonFunction() const
	{
		return Declaration.PythonFunction;
	}

	int32 FUeAgentFrozenCommandContract::GetContractVersion() const
	{
		return Declaration.ContractVersion;
	}

	const FString& FUeAgentFrozenCommandContract::GetContractHash() const
	{
		return ContractHash;
	}

	const FString& FUeAgentFrozenCommandContract::GetPublicOwner() const
	{
		return Declaration.PublicOwner;
	}

	EUeAgentCommandAvailability
	FUeAgentFrozenCommandContract::GetAvailability() const
	{
		return Declaration.Availability;
	}

	EUeAgentCommandSideEffect
	FUeAgentFrozenCommandContract::GetSideEffect() const
	{
		return Declaration.SideEffect;
	}

	const TArray<FString>&
	FUeAgentFrozenCommandContract::GetStateLayers() const
	{
		return Declaration.StateLayers;
	}

	const TArray<FUeAgentCommandFieldDeclaration>&
	FUeAgentFrozenCommandContract::GetInputFields() const
	{
		return Declaration.InputFields;
	}

	const TArray<FUeAgentCommandFieldDeclaration>&
	FUeAgentFrozenCommandContract::GetResultFields() const
	{
		return Declaration.ResultFields;
	}

	const TArray<FUeAgentCommandErrorDeclaration>&
	FUeAgentFrozenCommandContract::GetErrors() const
	{
		return Declaration.Errors;
	}

	const FUeAgentCommandPurposeDeclaration&
	FUeAgentFrozenCommandContract::GetPurpose() const
	{
		return Declaration.Purpose;
	}

	const TArray<FUeAgentCommandVariantDeclaration>&
	FUeAgentFrozenCommandContract::GetVariants() const
	{
		return Declaration.Variants;
	}

	const TArray<FString>&
	FUeAgentFrozenCommandContract::GetTutorialReferences() const
	{
		return Declaration.TutorialReferences;
	}
}

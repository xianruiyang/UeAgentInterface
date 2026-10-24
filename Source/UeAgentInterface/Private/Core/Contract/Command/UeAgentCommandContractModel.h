#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"

namespace UE::UAI::CommandRegistry
{
	class FUeAgentImmutableCommandRegistry;
}

namespace UE::UAI::CommandContract
{
	enum class EUeAgentCommandAvailability : uint8
	{
		Enabled,
		Dormant,
		Disabled,
	};

	enum class EUeAgentCommandSideEffect : uint8
	{
		None,
		EditorState,
		AssetState,
		WorldState,
		ExternalState,
	};

	struct UEAGENTINTERFACE_API FUeAgentCommandFieldDeclaration final
	{
		FString Name;
		FString WireType;
		bool bRequired = false;
		bool bNullable = false;
		FString Description;
		FString SchemaJson;
	};

	struct UEAGENTINTERFACE_API FUeAgentCommandErrorDeclaration final
	{
		FString Code;
		FString Description;
	};

	/**
	 * One short model-facing field selected from the lossless command payload.
	 * SourcePath is an exact sequence of object keys, not a dotted string, so
	 * neither the manifest consumer nor the model has to parse identities.
	 */
	struct UEAGENTINTERFACE_API FUeAgentModelResultFieldDeclaration final
	{
		FString Name;
		TArray<FString> SourcePath;
	};

	struct UEAGENTINTERFACE_API FUeAgentCommandPurposeDeclaration final
	{
		FString Summary;
		FString Target;
		TArray<FString> Preconditions;
		FString ParameterGuidance;
		FString ResultGuidance;
		FString FailureGuidance;
		FString SideEffectGuidance;
		FString MinimumExample;
		TArray<FString> RelatedFlows;
		TArray<FString> Keywords;
		TArray<FString> Synonyms;
	};

	/**
	 * One directly callable behavior selected within a public command.
	 * RequestSchemaJson covers the complete command params object, including
	 * any fixed family/policy selectors, so discovery consumers never need a
	 * second selector-to-parameters mapping.
	 */
	struct UEAGENTINTERFACE_API FUeAgentCommandVariantDeclaration final
	{
		FString VariantId;
		FString RequestSchemaJson;
		FUeAgentCommandPurposeDeclaration Purpose;
	};

	struct UEAGENTINTERFACE_API
	FUeAgentCommandVariantRequestFieldDeclaration final
	{
		FString Name;
		FString SchemaJson;
		bool bRequired = true;
	};

	UEAGENTINTERFACE_API bool TryBuildCommandVariantRequestSchema(
		const TArray<FUeAgentCommandVariantRequestFieldDeclaration>& Fields,
		FString& OutSchemaJson,
		UE::UAI::Contract::FDiagnostic& OutError);

	struct UEAGENTINTERFACE_API FUeAgentCommandDeclaration final
	{
		FString CommandId;
		FString PythonNamespace;
		FString PythonFunction;
		int32 ContractVersion = 0;
		FString PublicOwner;
		EUeAgentCommandAvailability Availability =
			EUeAgentCommandAvailability::Disabled;
		EUeAgentCommandSideEffect SideEffect =
			EUeAgentCommandSideEffect::None;
		TArray<FString> StateLayers;
		TArray<FUeAgentCommandFieldDeclaration> InputFields;
		TArray<FUeAgentCommandFieldDeclaration> ResultFields;
		TArray<FUeAgentModelResultFieldDeclaration> ModelResultFields;
		TArray<FUeAgentCommandErrorDeclaration> Errors;
		FUeAgentCommandPurposeDeclaration Purpose;
		TArray<FUeAgentCommandVariantDeclaration> Variants;
		TArray<FString> TutorialReferences = {
			TEXT("uai-docs:python-sdk-and-cli#command-discovery"),
			TEXT("uai-skill:public-sdk-playbook#command-discovery"),
		};
	};

	class UEAGENTINTERFACE_API FUeAgentFrozenCommandContract final
	{
	public:
		FUeAgentFrozenCommandContract(
			const FUeAgentFrozenCommandContract&) = default;
		FUeAgentFrozenCommandContract(
			FUeAgentFrozenCommandContract&&) = default;
		FUeAgentFrozenCommandContract& operator=(
			const FUeAgentFrozenCommandContract&) = default;
		FUeAgentFrozenCommandContract& operator=(
			FUeAgentFrozenCommandContract&&) = default;

		const FString& GetCommandId() const;
		const FString& GetPythonNamespace() const;
		const FString& GetPythonFunction() const;
		int32 GetContractVersion() const;
		const FString& GetContractHash() const;
		const FString& GetPublicOwner() const;
		EUeAgentCommandAvailability GetAvailability() const;
		EUeAgentCommandSideEffect GetSideEffect() const;
		const TArray<FString>& GetStateLayers() const;
		const TArray<FUeAgentCommandFieldDeclaration>&
		GetInputFields() const;
		const TArray<FUeAgentCommandFieldDeclaration>&
		GetResultFields() const;
		const TArray<FUeAgentModelResultFieldDeclaration>&
		GetModelResultFields() const;
		const TArray<FUeAgentCommandErrorDeclaration>& GetErrors() const;
		const FUeAgentCommandPurposeDeclaration& GetPurpose() const;
		const TArray<FUeAgentCommandVariantDeclaration>& GetVariants() const;
		const TArray<FString>& GetTutorialReferences() const;

	private:
		friend class UE::UAI::CommandRegistry::
			FUeAgentImmutableCommandRegistry;

		FUeAgentFrozenCommandContract(
			FUeAgentCommandDeclaration InDeclaration,
			FString InContractHash);

		FUeAgentCommandDeclaration Declaration;
		FString ContractHash;
	};
}

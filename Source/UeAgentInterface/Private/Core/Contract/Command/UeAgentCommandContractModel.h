#pragma once

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
	};

	struct UEAGENTINTERFACE_API FUeAgentCommandErrorDeclaration final
	{
		FString Code;
		FString Description;
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
		TArray<FUeAgentCommandErrorDeclaration> Errors;
		FUeAgentCommandPurposeDeclaration Purpose;
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
		const TArray<FUeAgentCommandErrorDeclaration>& GetErrors() const;
		const FUeAgentCommandPurposeDeclaration& GetPurpose() const;

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

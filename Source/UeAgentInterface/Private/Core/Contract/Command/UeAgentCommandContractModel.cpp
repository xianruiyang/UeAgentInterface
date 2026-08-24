#include "Core/Contract/Command/UeAgentCommandContractModel.h"

namespace UE::UAI::CommandContract
{
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

	const TArray<FString>&
	FUeAgentFrozenCommandContract::GetTutorialReferences() const
	{
		return Declaration.TutorialReferences;
	}
}

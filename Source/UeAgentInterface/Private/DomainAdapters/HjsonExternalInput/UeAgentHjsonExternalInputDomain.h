#pragma once

#include "CoreMinimal.h"
#include "Core/HjsonWorkspace/UeAgentHjsonWorkspace.h"

namespace UE::UAI::DomainAdapters::HjsonExternalInput
{
	enum class EHjsonExternalInputFamily : uint8
	{
		Factory,
		SkinWeightProfile,
		Localization,
	};

	struct UEAGENTINTERFACE_API FHjsonExternalInputFamilyContract final
	{
		EHjsonExternalInputFamily Family = EHjsonExternalInputFamily::Factory;
		FString RootTypeIdentity;
		FString InitializationContractId;
		TArray<FString> ImporterCommandIds;

		bool IsValid() const;
	};

	struct UEAGENTINTERFACE_API FHjsonExternalInputDelegation final
	{
		FString CommandId;
		FString CanonicalParamsJson;

		bool IsValid() const;
	};

	UEAGENTINTERFACE_API const TArray<FHjsonExternalInputFamilyContract>&
	GetHjsonExternalInputFamilyContracts();

	UEAGENTINTERFACE_API const FHjsonExternalInputFamilyContract*
	FindHjsonExternalInputFamilyContract(const FString& RootTypeIdentity);

	UEAGENTINTERFACE_API bool TryBuildHjsonExternalInputDelegation(
		const FString& CanonicalContentGraphParamsJson,
		const FHjsonExternalInputFamilyContract& Contract,
		FHjsonExternalInputDelegation& OutDelegation,
		HjsonWorkspace::FWorkspaceError& OutError);
}

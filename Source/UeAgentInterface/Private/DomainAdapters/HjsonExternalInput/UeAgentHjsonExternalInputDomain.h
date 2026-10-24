#pragma once

#include "CoreMinimal.h"
#include "Core/HjsonWorkspace/UeAgentHjsonWorkspace.h"

namespace UE::UAI::DomainAdapters::HjsonExternalInput
{
	struct FResolvedExternalInputDocument;
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
		bool TryValidate(UE::UAI::Contract::FDiagnostic& OutError) const;
	};

	struct UEAGENTINTERFACE_API FHjsonExternalInputDelegation final
	{
		FString CommandId;
		FString CanonicalParamsJson;
		TSharedPtr<const FResolvedExternalInputDocument> SourceDocument;

		bool IsValid() const;
		void LocateFailure(HjsonWorkspace::FWorkspaceError& Error) const;
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

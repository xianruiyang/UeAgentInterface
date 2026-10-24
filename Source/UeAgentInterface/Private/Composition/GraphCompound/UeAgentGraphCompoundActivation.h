#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"
#include "Core/Contract/UeAgentCommandContract.h"
#include "Core/Contract/UeAgentDescriptorRegistry.h"
#include "Templates/SharedPointer.h"

#define UAI_GRAPH_COMPOUND_ACTIVATION_V1 1

namespace UE::UAI::Composition::GraphCompound
{
	class UEAGENTINTERFACE_API FUeAgentGraphCompoundActivationProfile final
	{
	public:
		FUeAgentGraphCompoundActivationProfile(
			FString InCapabilityId,
			FString InNamespace,
			FString InMethod,
			Contract::FUeAgentDescriptorIdentity InDescriptorIdentity,
			FString InOperationKind);

		const FString& GetCapabilityId() const;
		const FString& GetCommandId() const;
		const FString& GetNamespace() const;
		const FString& GetMethod() const;
		const Contract::FUeAgentDescriptorIdentity&
			GetDescriptorIdentity() const;
		const FString& GetOperationKind() const;
		bool IsValid() const;
		const Contract::FDiagnostic& GetValidationDiagnostic() const;
		bool TryMatchDefinition(const FUeAgentGraphCompoundActivationProfile& Expected,
			Contract::FDiagnostic& OutError, const FString& Stage) const;

	private:
		FString CapabilityId;
		FString Namespace;
		FString Method;
		Contract::FUeAgentDescriptorIdentity DescriptorIdentity;
		FString OperationKind;
		Contract::FDiagnostic ValidationDiagnostic;
	};

	UEAGENTINTERFACE_API bool TryGetGraphCompoundActivationProfiles(
		const TArray<FUeAgentGraphCompoundActivationProfile>*& OutProfiles,
		Contract::FDiagnostic& OutError);

	UEAGENTINTERFACE_API bool TryCreateGraphCompoundDescriptorRegistry(
		TSharedPtr<const Contract::FUeAgentDescriptorRegistry>& OutRegistry,
		UE::UAI::Contract::FDiagnostic& OutError);

	/** Builds the outermost production registry from base and Graph contributions. */
	UEAGENTINTERFACE_API bool
	TryCreateGraphCompoundProductionCommandContractRegistry(
		TSharedPtr<const CommandContract::FUeAgentCommandContractRegistry>&
			OutRegistry,
		UE::UAI::Contract::FDiagnostic& OutError);

	/** Builds the production registry for an explicit canonical Graph subset. */
	UEAGENTINTERFACE_API bool
	TryCreateGraphCompoundProductionCommandContractRegistry(
		const TArray<FUeAgentGraphCompoundActivationProfile>& Profiles,
		TSharedPtr<const CommandContract::FUeAgentCommandContractRegistry>&
			OutRegistry,
		UE::UAI::Contract::FDiagnostic& OutError);
}

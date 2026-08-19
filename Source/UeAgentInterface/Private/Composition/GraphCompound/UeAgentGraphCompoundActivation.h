#pragma once

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

	private:
		FString CapabilityId;
		FString Namespace;
		FString Method;
		Contract::FUeAgentDescriptorIdentity DescriptorIdentity;
		FString OperationKind;
		bool bValid = false;
	};

	UEAGENTINTERFACE_API const TArray<FUeAgentGraphCompoundActivationProfile>&
	GetGraphCompoundActivationProfiles();

	UEAGENTINTERFACE_API bool TryCreateGraphCompoundDescriptorRegistry(
		TSharedPtr<const Contract::FUeAgentDescriptorRegistry>& OutRegistry,
		FString& OutError);

	/** Builds the outermost production registry: 185 base + 77 Graph commands. */
	UEAGENTINTERFACE_API bool
	TryCreateGraphCompoundProductionCommandContractRegistry(
		TSharedPtr<const CommandContract::FUeAgentCommandContractRegistry>&
			OutRegistry,
		FString& OutError);

	/** Builds the production registry for an explicit canonical Graph subset. */
	UEAGENTINTERFACE_API bool
	TryCreateGraphCompoundProductionCommandContractRegistry(
		const TArray<FUeAgentGraphCompoundActivationProfile>& Profiles,
		TSharedPtr<const CommandContract::FUeAgentCommandContractRegistry>&
			OutRegistry,
		FString& OutError);
}

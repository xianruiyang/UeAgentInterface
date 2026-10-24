#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"
#include "Core/Contract/UeAgentCommandContract.h"
#include "Core/Contract/UeAgentDescriptorRegistry.h"
#include "Templates/SharedPointer.h"

#define UAI_SIMPLE_AUTHORING_COMPOSITION_V1 1

namespace UE::UAI::Composition::SimpleAuthoring
{
	struct UEAGENTINTERFACE_API
		FUeAgentSimpleAuthoringCommandRoute final
	{
		FUeAgentSimpleAuthoringCommandRoute(
			FString InCommandId,
			FString InCapabilityId,
			FString InNamespace,
			FString InMethod,
			FString InFamilyId,
			Contract::FUeAgentDescriptorIdentity
				InDescriptorIdentity);

		FString CommandId;
		FString CapabilityId;
		FString Namespace;
		FString Method;
		FString FamilyId;
		Contract::FUeAgentDescriptorIdentity DescriptorIdentity;

		bool IsValid() const;
		bool TryValidate(UE::UAI::Contract::FDiagnostic& OutError) const;
	};

	UEAGENTINTERFACE_API const TArray<
		FUeAgentSimpleAuthoringCommandRoute>&
	GetSimpleAuthoringCommandRoutes();

	UEAGENTINTERFACE_API bool
	TryCreateSimpleAuthoringDescriptorRegistry(
		TSharedPtr<const Contract::FUeAgentDescriptorRegistry>&
			OutRegistry,
		UE::UAI::Contract::FDiagnostic& OutError);

	UEAGENTINTERFACE_API bool
	TryCreateProductionCommandContractRegistry(
		TSharedPtr<const CommandContract::
			FUeAgentCommandContractRegistry>& OutRegistry,
		UE::UAI::Contract::FDiagnostic& OutError);
}

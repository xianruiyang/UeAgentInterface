#pragma once

#include "CoreMinimal.h"
#include "Core/Contract/UeAgentCommandContract.h"
#include "Core/Contract/UeAgentDescriptorRegistry.h"
#include "Templates/SharedPointer.h"

#define UAI_TIMELINE_SPATIAL_ACTIVATION_V1 1

namespace UE::UAI::Composition::TimelineSpatial
{
	struct UEAGENTINTERFACE_API FUeAgentTimelineSpatialActivationProfile final
	{
		explicit FUeAgentTimelineSpatialActivationProfile(
			Contract::FUeAgentDescriptorIdentity InDescriptorIdentity)
			: DescriptorIdentity(MoveTemp(InDescriptorIdentity))
		{
		}

		FString AdapterId;
		FString CapabilityId;
		FString CommandId;
		FString Namespace;
		FString Method;
		Contract::FUeAgentDescriptorIdentity DescriptorIdentity;
		FString OperationKind;
		FString EffectClass;
		FString FamilyId;
		FString TypedOperationId;
		FString ProviderBinding;
		FString ContextSource;
		FString OracleKind;
		bool bRequiresTarget = false;
		bool bRequiresInput = false;
		bool bSupportsProjection = false;

		bool IsValid() const;
	};

	UEAGENTINTERFACE_API const TArray<
		FUeAgentTimelineSpatialActivationProfile>&
	GetTimelineSpatialActivationProfiles();

	UEAGENTINTERFACE_API bool TryCreateTimelineSpatialDescriptorRegistry(
		TSharedPtr<const Contract::FUeAgentDescriptorRegistry>& OutRegistry,
		FString& OutError);

	UEAGENTINTERFACE_API bool
	TryCreateTimelineSpatialProductionCommandContractRegistry(
		TSharedPtr<const CommandContract::FUeAgentCommandContractRegistry>&
			OutRegistry,
		FString& OutError);
}

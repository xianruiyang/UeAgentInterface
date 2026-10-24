#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"
#include "Core/Contract/UeAgentCommandContract.h"
#include "Core/Contract/Command/UeAgentCommandContractModel.h"
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
		TArray<CommandContract::FUeAgentModelResultFieldDeclaration>
			ModelResultFields;
		bool bRequiresTarget = false;
		bool bRequiresInput = false;
		bool bSupportsProjection = false;

		bool IsValid() const;
		bool TryValidate(Contract::FDiagnostic& OutError) const;
	};

	UEAGENTINTERFACE_API bool TryGetTimelineSpatialActivationProfiles(
		const TArray<FUeAgentTimelineSpatialActivationProfile>*& OutProfiles,
		Contract::FDiagnostic& OutError);

	UEAGENTINTERFACE_API bool TryCreateTimelineSpatialDescriptorRegistry(
		TSharedPtr<const Contract::FUeAgentDescriptorRegistry>& OutRegistry,
		UE::UAI::Contract::FDiagnostic& OutError);

	UEAGENTINTERFACE_API bool
	TryCreateTimelineSpatialProductionCommandContractRegistry(
		TSharedPtr<const CommandContract::FUeAgentCommandContractRegistry>&
			OutRegistry,
		UE::UAI::Contract::FDiagnostic& OutError);

	bool TryBuildTimelineSpatialPublicParameterSchema(
		const FUeAgentTimelineSpatialActivationProfile& Profile,
		const Contract::FUeAgentOperationDescriptor& Descriptor,
		const FString& ParameterName,
		FString& OutSchemaJson,
		UE::UAI::Contract::FDiagnostic& OutError);
}

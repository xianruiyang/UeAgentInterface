#pragma once

#include "CoreMinimal.h"

#define UAI_WORLD_SPATIAL_CONTRACT_V1 1

namespace UE::UAI::DomainAdapters::WorldSpatial
{
	class UEAGENTINTERFACE_API FUeAgentWorldSpatialPortFamilyContract final
	{
	public:
		FUeAgentWorldSpatialPortFamilyContract(
			FString InFamilyId,
			TArray<int32> InDimensions,
			FString InSubjectPolicy,
			FString InSemanticFramePolicy,
			FString InWriteFramePolicy,
			FString InReadFramePolicy,
			FString InVerifyFramePolicy,
			FString InDimensionBridge,
			FString InConversionChain,
			TArray<FString> InRequiredContextFacets);

		const FString& GetFamilyId() const;
		const FString& GetSpaceContractId() const;
		const TArray<int32>& GetDimensions() const;
		const FString& GetSubjectPolicy() const;
		const FString& GetSemanticFramePolicy() const;
		const FString& GetWriteFramePolicy() const;
		const FString& GetReadFramePolicy() const;
		const FString& GetVerifyFramePolicy() const;
		const FString& GetDimensionBridge() const;
		const FString& GetConversionChain() const;
		const TArray<FString>& GetRequiredContextFacets() const;

	private:
		FString FamilyId;
		FString SpaceContractId = TEXT("uai.space_contract.v1");
		TArray<int32> Dimensions;
		FString SubjectPolicy;
		FString SemanticFramePolicy;
		FString WriteFramePolicy;
		FString ReadFramePolicy;
		FString VerifyFramePolicy;
		FString DimensionBridge;
		FString ConversionChain;
		TArray<FString> RequiredContextFacets;
	};

	class UEAGENTINTERFACE_API FUeAgentWorldSpatialCapabilityContract final
	{
	public:
		FUeAgentWorldSpatialCapabilityContract(
			FString InCapabilityId,
			FString InDescriptorId,
			int32 InVersion,
			FString InOperationKind,
			FString InEffectClass,
			FString InPortFamilyId);

		const FString& GetCapabilityId() const;
		const FString& GetDescriptorId() const;
		int32 GetVersion() const;
		const FString& GetOperationKind() const;
		const FString& GetEffectClass() const;
		const FString& GetPortFamilyId() const;
		const FString& GetSpaceContractId() const;

	private:
		FString CapabilityId;
		FString DescriptorId;
		int32 Version = 0;
		FString OperationKind;
		FString EffectClass;
		FString PortFamilyId;
		FString SpaceContractId = TEXT("uai.space_contract.v1");
	};

	UEAGENTINTERFACE_API const TArray<FUeAgentWorldSpatialPortFamilyContract>&
	GetWorldSpatialPortFamilyContracts();

	UEAGENTINTERFACE_API const TArray<FUeAgentWorldSpatialCapabilityContract>&
	GetWorldSpatialCapabilityContracts();
}

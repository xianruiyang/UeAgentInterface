#pragma once

#include "CoreMinimal.h"

#define UAI_RIG_SIMULATION_SPATIAL_CONTRACT_V1 1

namespace UE::UAI::DomainAdapters::RigSimulationSpatial
{
	class UEAGENTINTERFACE_API
	FUeAgentRigSimulationSpatialPortFamilyContract final
	{
	public:
		FUeAgentRigSimulationSpatialPortFamilyContract(
			FString InFamilyId,
			TArray<int32> InDimensions,
			TArray<FString> InAcceptedTimeStates,
			FString InSubjectPolicy,
			FString InSemanticFramePolicy,
			FString InWriteFramePolicy,
			FString InReadFramePolicy,
			FString InVerifyFramePolicy,
			FString InConversionChain,
			TArray<FString> InRequiredContextFacets,
			TArray<FString> InForbiddenImplicitFacets,
			FString InOracleKind);

		const FString& GetFamilyId() const;
		const FString& GetTemporalContractId() const;
		const FString& GetSpaceContractId() const;
		const TArray<int32>& GetDimensions() const;
		const TArray<FString>& GetAcceptedTimeStates() const;
		const FString& GetSubjectPolicy() const;
		const FString& GetSemanticFramePolicy() const;
		const FString& GetWriteFramePolicy() const;
		const FString& GetReadFramePolicy() const;
		const FString& GetVerifyFramePolicy() const;
		const FString& GetConversionChain() const;
		const TArray<FString>& GetRequiredContextFacets() const;
		const TArray<FString>& GetForbiddenImplicitFacets() const;
		const FString& GetOracleKind() const;

	private:
		FString FamilyId;
		FString TemporalContractId = TEXT("uai.temporal_contract.v1");
		FString SpaceContractId = TEXT("uai.space_contract.v1");
		TArray<int32> Dimensions;
		TArray<FString> AcceptedTimeStates;
		FString SubjectPolicy;
		FString SemanticFramePolicy;
		FString WriteFramePolicy;
		FString ReadFramePolicy;
		FString VerifyFramePolicy;
		FString ConversionChain;
		TArray<FString> RequiredContextFacets;
		TArray<FString> ForbiddenImplicitFacets;
		FString OracleKind;
	};

	class UEAGENTINTERFACE_API
	FUeAgentRigSimulationSpatialCapabilityContract final
	{
	public:
		FUeAgentRigSimulationSpatialCapabilityContract(
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
		const FString& GetTemporalContractId() const;
		const FString& GetSpaceContractId() const;

	private:
		FString CapabilityId;
		FString DescriptorId;
		int32 Version = 0;
		FString OperationKind;
		FString EffectClass;
		FString PortFamilyId;
		FString TemporalContractId = TEXT("uai.temporal_contract.v1");
		FString SpaceContractId = TEXT("uai.space_contract.v1");
	};

	UEAGENTINTERFACE_API const TArray<
		FUeAgentRigSimulationSpatialPortFamilyContract>&
	GetRigSimulationSpatialPortFamilyContracts();

	UEAGENTINTERFACE_API const TArray<
		FUeAgentRigSimulationSpatialCapabilityContract>&
	GetRigSimulationSpatialCapabilityContracts();
}

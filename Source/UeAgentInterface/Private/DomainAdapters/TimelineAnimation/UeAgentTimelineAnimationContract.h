#pragma once

#include "CoreMinimal.h"

#define UAI_TIMELINE_ANIMATION_CONTRACT_V1 1

namespace UE::UAI::DomainAdapters::TimelineAnimation
{
	class UEAGENTINTERFACE_API FUeAgentTimelineAnimationPortFamilyContract final
	{
	public:
		FUeAgentTimelineAnimationPortFamilyContract(
			FString InFamilyId,
			TArray<FString> InRequiredContextFacets,
			TArray<FString> InAcceptedTimeStates,
			FString InClockPolicy,
			FString InRangePolicy,
			FString InSpatialPolicy,
			FString InOracleKind);

		const FString& GetFamilyId() const;
		const FString& GetTemporalContractId() const;
		const FString& GetSpaceContractId() const;
		const TArray<FString>& GetRequiredContextFacets() const;
		const TArray<FString>& GetAcceptedTimeStates() const;
		const FString& GetClockPolicy() const;
		const FString& GetRangePolicy() const;
		const FString& GetSpatialPolicy() const;
		const FString& GetOracleKind() const;

	private:
		FString FamilyId;
		FString TemporalContractId = TEXT("uai.temporal_contract.v1");
		FString SpaceContractId = TEXT("uai.space_contract.v1");
		TArray<FString> RequiredContextFacets;
		TArray<FString> AcceptedTimeStates;
		FString ClockPolicy;
		FString RangePolicy;
		FString SpatialPolicy;
		FString OracleKind;
	};

	class UEAGENTINTERFACE_API FUeAgentTimelineAnimationCapabilityContract final
	{
	public:
		FUeAgentTimelineAnimationCapabilityContract(
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
		FUeAgentTimelineAnimationPortFamilyContract>&
	GetTimelineAnimationPortFamilyContracts();

	UEAGENTINTERFACE_API const TArray<
		FUeAgentTimelineAnimationCapabilityContract>&
	GetTimelineAnimationCapabilityContracts();
}

#pragma once

#include "CoreMinimal.h"

#define UAI_ANIMATION_RIG_GRAPH_CONTRACT_V1 1

namespace UE::UAI::DomainAdapters::AnimationRigGraph
{
	class FAnimationRigGraphPortFamilyContract final
	{
	public:
		FAnimationRigGraphPortFamilyContract(
			FString InFamilyId,
			TArray<FString> InRequiredIdentityFacets,
			TArray<FString> InRequiredStages,
			FString InBarrierKind,
			FString InOracleKind,
			FString InAvailabilityPolicy);

		const FString& GetFamilyId() const { return FamilyId; }
		const TArray<FString>& GetRequiredIdentityFacets() const { return RequiredIdentityFacets; }
		const TArray<FString>& GetRequiredStages() const { return RequiredStages; }
		const FString& GetBarrierKind() const { return BarrierKind; }
		const FString& GetOracleKind() const { return OracleKind; }
		const FString& GetAvailabilityPolicy() const { return AvailabilityPolicy; }

	private:
		FString FamilyId;
		TArray<FString> RequiredIdentityFacets;
		TArray<FString> RequiredStages;
		FString BarrierKind;
		FString OracleKind;
		FString AvailabilityPolicy;
	};

	class FAnimationRigGraphCapabilityContract final
	{
	public:
		FAnimationRigGraphCapabilityContract(
			FString InCapabilityId,
			FString InDescriptorId,
			FString InPortFamilyId,
			FString InOperationKind,
			FString InEffectClass,
			int32 InVersion);

		const FString& GetCapabilityId() const { return CapabilityId; }
		const FString& GetDescriptorId() const { return DescriptorId; }
		const FString& GetPortFamilyId() const { return PortFamilyId; }
		const FString& GetOperationKind() const { return OperationKind; }
		const FString& GetEffectClass() const { return EffectClass; }
		int32 GetVersion() const { return Version; }

	private:
		FString CapabilityId;
		FString DescriptorId;
		FString PortFamilyId;
		FString OperationKind;
		FString EffectClass;
		int32 Version = 1;
	};

	UEAGENTINTERFACE_API const TArray<FAnimationRigGraphPortFamilyContract>&
	GetAnimationRigGraphPortFamilyContracts();

	UEAGENTINTERFACE_API const TArray<FAnimationRigGraphCapabilityContract>&
	GetAnimationRigGraphCapabilityContracts();
}

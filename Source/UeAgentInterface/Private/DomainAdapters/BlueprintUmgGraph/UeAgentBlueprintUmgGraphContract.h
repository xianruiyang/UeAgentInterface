#pragma once

#include "CoreMinimal.h"

#define UAI_BLUEPRINT_UMG_GRAPH_CONTRACT_V1 1

namespace UE::UAI::DomainAdapters::BlueprintUmgGraph
{
	class FBlueprintUmgGraphPortFamilyContract final
	{
	public:
		FBlueprintUmgGraphPortFamilyContract(
			FString InFamilyId,
			TArray<FString> InRequiredIdentityFacets,
			TArray<FString> InRequiredStages,
			FString InOracleKind);

		const FString& GetFamilyId() const { return FamilyId; }
		const TArray<FString>& GetRequiredIdentityFacets() const
		{
			return RequiredIdentityFacets;
		}
		const TArray<FString>& GetRequiredStages() const
		{
			return RequiredStages;
		}
		const FString& GetOracleKind() const { return OracleKind; }

	private:
		FString FamilyId;
		TArray<FString> RequiredIdentityFacets;
		TArray<FString> RequiredStages;
		FString OracleKind;
	};

	class FBlueprintUmgGraphCapabilityContract final
	{
	public:
		FBlueprintUmgGraphCapabilityContract(
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

	UEAGENTINTERFACE_API const TArray<FBlueprintUmgGraphPortFamilyContract>&
	GetBlueprintUmgGraphPortFamilyContracts();

	UEAGENTINTERFACE_API const TArray<FBlueprintUmgGraphCapabilityContract>&
	GetBlueprintUmgGraphCapabilityContracts();
}

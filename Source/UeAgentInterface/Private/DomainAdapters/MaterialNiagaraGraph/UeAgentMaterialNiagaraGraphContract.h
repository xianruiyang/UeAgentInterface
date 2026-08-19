#pragma once

#include "CoreMinimal.h"

#define UAI_MATERIAL_NIAGARA_GRAPH_CONTRACT_V1 1

namespace UE::UAI::DomainAdapters::MaterialNiagaraGraph
{
	class FMaterialNiagaraGraphPortFamilyContract final
	{
	public:
		FMaterialNiagaraGraphPortFamilyContract(
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

	class FMaterialNiagaraGraphCapabilityContract final
	{
	public:
		FMaterialNiagaraGraphCapabilityContract(
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

	UEAGENTINTERFACE_API const TArray<
		FMaterialNiagaraGraphPortFamilyContract>&
	GetMaterialNiagaraGraphPortFamilyContracts();

	UEAGENTINTERFACE_API const TArray<
		FMaterialNiagaraGraphCapabilityContract>&
	GetMaterialNiagaraGraphCapabilityContracts();

	UEAGENTINTERFACE_API FString
	GetMaterialNiagaraGraphCompoundDelegateAdapterId();

	UEAGENTINTERFACE_API FString
	GetMaterialNiagaraGraphCompoundFailurePolicy();
}


#pragma once

#include "CoreMinimal.h"

#define UAI_COMPILE_BUILD_JOB_CONTRACT_V1 1

namespace UE::UAI::DomainAdapters::CompileBuildJob
{
	enum class EUeAgentCompileBuildJobFamily : uint8
	{
		AssetCompile,
		WorldBuild,
		ExternalProcess,
		WorldPartitionResave,
		Packaging,
	};

	class UEAGENTINTERFACE_API
	FUeAgentCompileBuildJobFamilyContract final
	{
	public:
		FUeAgentCompileBuildJobFamilyContract(
			EUeAgentCompileBuildJobFamily InFamily,
			FString InFamilyId,
			FString InSubjectKind,
			FString InPolicyKind,
			FString InSignalTypeId);

		EUeAgentCompileBuildJobFamily GetFamily() const;
		const FString& GetFamilyId() const;
		const FString& GetSubjectKind() const;
		const FString& GetPolicyKind() const;
		const FString& GetSignalTypeId() const;

	private:
		EUeAgentCompileBuildJobFamily Family;
		FString FamilyId;
		FString SubjectKind;
		FString PolicyKind;
		FString SignalTypeId;
	};

	UEAGENTINTERFACE_API const TArray<
		FUeAgentCompileBuildJobFamilyContract>&
	GetCompileBuildJobFamilyContracts();

	UEAGENTINTERFACE_API const FUeAgentCompileBuildJobFamilyContract*
	FindCompileBuildJobFamilyContract(
		EUeAgentCompileBuildJobFamily Family);
}

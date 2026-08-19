#include "DomainAdapters/CompileBuildJob/UeAgentCompileBuildJobContract.h"

namespace UE::UAI::DomainAdapters::CompileBuildJob
{
	FUeAgentCompileBuildJobFamilyContract::
	FUeAgentCompileBuildJobFamilyContract(
		EUeAgentCompileBuildJobFamily InFamily,
		FString InFamilyId,
		FString InSubjectKind,
		FString InPolicyKind,
		FString InSignalTypeId)
		: Family(InFamily)
		, FamilyId(MoveTemp(InFamilyId))
		, SubjectKind(MoveTemp(InSubjectKind))
		, PolicyKind(MoveTemp(InPolicyKind))
		, SignalTypeId(MoveTemp(InSignalTypeId))
	{
	}

	EUeAgentCompileBuildJobFamily
	FUeAgentCompileBuildJobFamilyContract::GetFamily() const
	{
		return Family;
	}

	const FString&
	FUeAgentCompileBuildJobFamilyContract::GetFamilyId() const
	{
		return FamilyId;
	}

	const FString&
	FUeAgentCompileBuildJobFamilyContract::GetSubjectKind() const
	{
		return SubjectKind;
	}

	const FString&
	FUeAgentCompileBuildJobFamilyContract::GetPolicyKind() const
	{
		return PolicyKind;
	}

	const FString&
	FUeAgentCompileBuildJobFamilyContract::GetSignalTypeId() const
	{
		return SignalTypeId;
	}

	const TArray<FUeAgentCompileBuildJobFamilyContract>&
	GetCompileBuildJobFamilyContracts()
	{
		static const TArray<FUeAgentCompileBuildJobFamilyContract>
			Contracts = {
				{
					EUeAgentCompileBuildJobFamily::AssetCompile,
					TEXT("asset_compile"),
					TEXT("asset_ref"),
					TEXT("asset_compile_policy_ref"),
					TEXT("uai.compile_build.asset_compile.progress/v1"),
				},
				{
					EUeAgentCompileBuildJobFamily::WorldBuild,
					TEXT("world_build"),
					TEXT("world_ref"),
					TEXT("world_build_policy_ref"),
					TEXT("uai.compile_build.world_build.progress/v1"),
				},
				{
					EUeAgentCompileBuildJobFamily::ExternalProcess,
					TEXT("external_process"),
					TEXT("project_or_asset_ref"),
					TEXT("owned_tool_policy_ref"),
					TEXT("uai.compile_build.external_process.report_ref/v1"),
				},
				{
					EUeAgentCompileBuildJobFamily::WorldPartitionResave,
					TEXT("world_partition_resave"),
					TEXT("world_ref"),
					TEXT("resave_policy_ref"),
					TEXT("uai.compile_build.world_partition_resave.progress/v1"),
				},
				{
					EUeAgentCompileBuildJobFamily::Packaging,
					TEXT("packaging"),
					TEXT("project_ref"),
					TEXT("packaging_policy_ref"),
					TEXT("uai.compile_build.packaging.report_ref/v1"),
				},
			};
		return Contracts;
	}

	const FUeAgentCompileBuildJobFamilyContract*
	FindCompileBuildJobFamilyContract(
		const EUeAgentCompileBuildJobFamily Family)
	{
		return GetCompileBuildJobFamilyContracts().FindByPredicate(
			[Family](
				const FUeAgentCompileBuildJobFamilyContract& Contract)
			{
				return Contract.GetFamily() == Family;
			});
	}
}

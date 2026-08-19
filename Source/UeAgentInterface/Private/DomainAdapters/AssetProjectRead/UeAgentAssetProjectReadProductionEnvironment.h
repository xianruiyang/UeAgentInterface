#pragma once

#include "CoreMinimal.h"
#include "DomainAdapters/AssetProjectRead/UeAgentAssetProjectReadNative.h"

namespace UE::UAI::DomainAdapters::AssetProjectRead
{
	class UEAGENTINTERFACE_API
		FUeAgentAssetProjectReadProductionEnvironment final
		: public IUeAgentAssetProjectReadEnvironment
	{
	public:
		virtual bool TryResolveAsset(
			const UE::UAI::Discovery::FUeAgentCandidateBinding& Target,
			UObject*& OutAsset,
			FUeAgentAssetProjectReadError& OutError) override;

		virtual bool TryResolveRegistryRecord(
			const UE::UAI::Discovery::FUeAgentCandidateBinding& Target,
			FAssetData& OutRecord,
			FUeAgentAssetProjectReadError& OutError) override;

		virtual bool TryResolveRegistryRelations(
			const FAssetData& Record,
			const FString& Direction,
			const TArray<FString>& RelationKinds,
			TArray<FString>& OutDependencies,
			TArray<FString>& OutReferencers,
			FUeAgentAssetProjectReadError& OutError) override;

		virtual bool TryResolveEditorResources(
			const UE::UAI::ContextResource::
				FUeAgentExecutionContextRef& Context,
			TArray<UPackage*>& OutPackages,
			FUeAgentAssetProjectReadError& OutError) override;

		virtual bool TryResolveProjectObject(
			const UE::UAI::ContextResource::
				FUeAgentExecutionContextRef& Context,
			const FString& SourceId,
			UObject*& OutObject,
			FUeAgentAssetProjectReadError& OutError) override;

		virtual bool TryResolveConfig(
			const UE::UAI::ContextResource::
				FUeAgentExecutionContextRef& Context,
			const FString& ConfigScope,
			const FConfigFile*& OutConfig,
			FUeAgentAssetProjectReadError& OutError) override;
	};
}

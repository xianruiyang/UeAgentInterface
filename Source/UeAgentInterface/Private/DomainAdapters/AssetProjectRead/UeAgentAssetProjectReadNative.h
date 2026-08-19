#pragma once

#include "CoreMinimal.h"
#include "DomainAdapters/AssetProjectRead/UeAgentAssetProjectReadProvider.h"
#include "Templates/SharedPointer.h"

class FConfigFile;
class UObject;
class UPackage;
struct FAssetData;

namespace UE::UAI::DomainAdapters::AssetProjectRead
{
	class UEAGENTINTERFACE_API
		IUeAgentAssetProjectReadEnvironment
	{
	public:
		virtual ~IUeAgentAssetProjectReadEnvironment() = default;

		virtual bool TryResolveAsset(
			const UE::UAI::Discovery::
				FUeAgentCandidateBinding& Target,
			UObject*& OutAsset,
			FUeAgentAssetProjectReadError& OutError) = 0;

		virtual bool TryResolveRegistryRecord(
			const UE::UAI::Discovery::
				FUeAgentCandidateBinding& Target,
			FAssetData& OutRecord,
			FUeAgentAssetProjectReadError& OutError) = 0;

		virtual bool TryResolveRegistryRelations(
			const FAssetData& Record,
			const FString& Direction,
			const TArray<FString>& RelationKinds,
			TArray<FString>& OutDependencies,
			TArray<FString>& OutReferencers,
			FUeAgentAssetProjectReadError& OutError) = 0;

		virtual bool TryResolveEditorResources(
			const UE::UAI::ContextResource::
				FUeAgentExecutionContextRef& Context,
			TArray<UPackage*>& OutPackages,
			FUeAgentAssetProjectReadError& OutError) = 0;

		virtual bool TryResolveProjectObject(
			const UE::UAI::ContextResource::
				FUeAgentExecutionContextRef& Context,
			const FString& SourceId,
			UObject*& OutObject,
			FUeAgentAssetProjectReadError& OutError) = 0;

		virtual bool TryResolveConfig(
			const UE::UAI::ContextResource::
				FUeAgentExecutionContextRef& Context,
			const FString& ConfigScope,
			const FConfigFile*& OutConfig,
			FUeAgentAssetProjectReadError& OutError) = 0;
	};

	class UEAGENTINTERFACE_API
		FUeAgentAssetProjectReadNativeHookFactory final
	{
	public:
		static bool TryCreate(
			const FString& ProviderBinding,
			TSharedRef<IUeAgentAssetProjectReadEnvironment>
				Environment,
			TSharedPtr<IUeAgentAssetProjectReadHook>& OutHook,
			FUeAgentAssetProjectReadError& OutError);
	};
}

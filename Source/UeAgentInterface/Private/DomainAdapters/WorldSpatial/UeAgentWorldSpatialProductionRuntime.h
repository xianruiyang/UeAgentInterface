#pragma once

#include "CoreMinimal.h"
#include "DomainAdapters/WorldSpatial/UeAgentWorldSpatialExecutionChains.h"
#include "DomainAdapters/WorldSpatial/UeAgentWorldSpatialProductionDependencies.h"
#include "Templates/SharedPointer.h"

class UWorld;

namespace UE::UAI::BindingGateway::Query
{
	class FUeAgentDomainQueryProviderRegistry;
}

namespace UE::UAI::Contract
{
	class FUeAgentDescriptorRegistry;
}

namespace UE::UAI::DomainAdapters::WorldSpatial
{
	struct UEAGENTINTERFACE_API FUeAgentWorldSpatialProductionRuntimeConfig
	{
		FString ProjectInstanceId;
		FString EditorInstanceId;
		FString ServerSessionId;
		int64 RegistryEpoch = -1;
		TArray<FUeAgentWorldSpatialViewportBinding> ViewportBindings;
		TArray<FUeAgentWorldSpatialMeshSelectionBinding> MeshSelectionBindings;

		bool TryValidate(FUeAgentWorldSpatialExecutionError& OutError) const;
		bool IsValid() const;
	};

	/**
	 * Owns the concrete main-Editor dependencies and exact C0/C3 runtime for all
	 * WorldSpatial capabilities. Creation is scoped to one explicit world; it
	 * never resolves a current world, viewport, graph, or selection implicitly.
	 */
	class UEAGENTINTERFACE_API FUeAgentWorldSpatialProductionRuntime final
	{
	public:
		static bool TryCreate(
			const FUeAgentWorldSpatialProductionRuntimeConfig& Config,
			UWorld& ExplicitWorld,
			TSharedPtr<FUeAgentWorldSpatialProductionRuntime>& OutRuntime,
			FUeAgentWorldSpatialExecutionError& OutError);

		const TSharedPtr<const UE::UAI::Contract::FUeAgentDescriptorRegistry>&
			GetDescriptorRegistry() const;
		const TSharedPtr<const UE::UAI::BindingGateway::Query::
			FUeAgentDomainQueryProviderRegistry>& GetQueryProviderRegistry() const;
		const TArray<TSharedRef<FUeAgentWorldSpatialOperationChain>>&
			GetOperationChains() const;
		bool TryReadRevision(
			const FString& ExplicitRef,
			FWorldSpatialRevision& OutRevision,
			FUeAgentWorldSpatialExecutionError& OutError) const;
		void GetExplicitViewportIdentities(
			TArray<FString>& OutViewportIdentities) const;

	private:
		FUeAgentWorldSpatialProductionRuntime(
			TSharedPtr<const UE::UAI::Contract::FUeAgentDescriptorRegistry>
				InDescriptorRegistry,
			TSharedPtr<const UE::UAI::BindingGateway::Query::
				FUeAgentDomainQueryProviderRegistry> InQueryProviderRegistry,
			TArray<TSharedRef<FUeAgentWorldSpatialOperationChain>>
				InOperationChains,
			FUeAgentWorldSpatialNativeDependencies InNativeDependencies);

		TSharedPtr<const UE::UAI::Contract::FUeAgentDescriptorRegistry>
			DescriptorRegistry;
		TSharedPtr<const UE::UAI::BindingGateway::Query::
			FUeAgentDomainQueryProviderRegistry> QueryProviderRegistry;
		TArray<TSharedRef<FUeAgentWorldSpatialOperationChain>> OperationChains;
		FUeAgentWorldSpatialNativeDependencies NativeDependencies;
	};
}

#pragma once

#include "Composition/GraphCompound/UeAgentGraphCompoundBlueprintMaterialRoutes.h"
#include "Composition/GraphCompound/UeAgentGraphCompoundRuntime.h"
#include "Core/PlanningPreflight/UeAgentPlanningPreflight.h"

namespace UE::UAI::AssetDurable
{
	class FPackageAssetDurableBackend;
}

namespace UE::UAI::DomainAdapters::MaterialNiagaraGraph
{
	class FMaterialNiagaraGraphNativeHook;
}

namespace UE::UAI::Composition::GraphCompound
{
	/**
	 * Niagara-specific UE mechanics composed by the exact GraphCompound binding.
	 * Public JSON, canonical planning and Outcome remain owned by the shared route,
	 * C4/C6/C8D/C9 modules; this service only joins those contracts to native
	 * Niagara asset lifecycle and compile-settle behavior.
	 */
	class FGraphCompoundNiagaraLifecycleProduction final
	{
	public:
		FGraphCompoundNiagaraLifecycleProduction(
			FString InJournalRoot,
			DomainAdapters::MaterialNiagaraGraph::
				FMaterialNiagaraGraphNativeHook& InNativeHook,
			AssetDurable::FPackageAssetDurableBackend& InDurableBackend,
			int64& InLastFencingToken);

		bool TryCaptureAssetCreateSnapshot(
			const FGraphCompoundDomainRouteProfile& Profile,
			const FGraphCompoundAssetCreateRequest& Request,
			ContextResource::FUeAgentResourceTracker& Resources,
			StateSemantic::FUeAgentCanonicalSnapshotPtr& OutSnapshot,
			FGraphCompoundRuntimeError& OutError) const;

		bool TryExecuteAssetCreate(
			const FGraphCompoundDomainRouteProfile& Profile,
			const FGraphCompoundAssetCreateRequest& Request,
			const PlanningPreflight::FPlanningPreflightPlanRef& Plan,
			const Contract::FUeAgentCallerContext& CallerContext,
			ContextResource::FUeAgentResourceTracker& Resources,
			const TSharedRef<Contract::IUeAgentCancellationSignal>&
				Cancellation,
			Contract::FUeAgentContractValue& OutResult,
			FGraphCompoundRuntimeError& OutError);

		bool TryCaptureCompileSnapshot(
			const FGraphCompoundDomainRouteProfile& Profile,
			const FString& TargetRef,
			ContextResource::FUeAgentResourceTracker& Resources,
			StateSemantic::FUeAgentCanonicalSnapshotPtr& OutSnapshot,
			FGraphCompoundRuntimeError& OutError) const;

		bool TryExecuteCompile(
			const FGraphCompoundDomainRouteProfile& Profile,
			const FString& TargetRef,
			const FGraphCompoundCanonicalPayload& Payload,
			const PlanningPreflight::FPlanningPreflightPlanRef& Plan,
			const Contract::FUeAgentCallerContext& CallerContext,
			ContextResource::FUeAgentResourceTracker& Resources,
			const TSharedRef<Contract::IUeAgentCancellationSignal>&
				Cancellation,
			Contract::FUeAgentContractValue& OutResult,
			FGraphCompoundRuntimeError& OutError);

	private:
		FString JournalRoot;
		DomainAdapters::MaterialNiagaraGraph::
			FMaterialNiagaraGraphNativeHook& NativeHook;
		AssetDurable::FPackageAssetDurableBackend& DurableBackend;
		int64& LastFencingToken;
	};
}

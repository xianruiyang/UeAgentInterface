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
	 * One production owner for the four exact Niagara typed mutations. It keeps
	 * selector-based C4 capture, native application, durable save and independent
	 * C9 reload readback on the same stable identity contract.
	 */
	class FGraphCompoundNiagaraTypedMutationProduction final
	{
	public:
		FGraphCompoundNiagaraTypedMutationProduction(
			FString InJournalRoot,
			DomainAdapters::MaterialNiagaraGraph::
				FMaterialNiagaraGraphNativeHook& InNativeHook,
			AssetDurable::FPackageAssetDurableBackend& InDurableBackend,
			int64& InLastFencingToken);

		bool TryCaptureSnapshot(
			const FGraphCompoundDomainRouteProfile& Profile,
			const FGraphCompoundNiagaraMutationSelector& Selector,
			ContextResource::FUeAgentResourceTracker& Resources,
			StateSemantic::FUeAgentCanonicalSnapshotPtr& OutSnapshot,
			FGraphCompoundRuntimeError& OutError) const;

		bool TryExecute(
			const FGraphCompoundDomainRouteProfile& Profile,
			const FGraphCompoundNiagaraMutationSelector& Selector,
			const FGraphCompoundCanonicalPayload& Payload,
			const PlanningPreflight::FPlanningPreflightPlanRef& Plan,
			const Contract::FUeAgentCallerContext& CallerContext,
			TSharedRef<ContextResource::FUeAgentResourceTracker,
				ESPMode::ThreadSafe> Resources,
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

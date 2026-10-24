#pragma once

#include "Composition/GraphCompound/UeAgentGraphCompoundBlueprintMaterialRoutes.h"
#include "Composition/GraphCompound/UeAgentGraphCompoundRuntime.h"

#define UAI_GRAPH_COMPOUND_BLUEPRINT_MATERIAL_BINDINGS_V1 1

namespace UE::UAI::Composition::GraphCompound
{
	/**
	 * Native production seam used by exact bindings. Route selection is already
	 * complete before this seam is called: query and typed operation are
	 * physically separate methods and cannot fall back to each other.
	 */
	class UEAGENTINTERFACE_API
	IGraphCompoundBlueprintMaterialEnvironment
	{
	public:
		virtual ~IGraphCompoundBlueprintMaterialEnvironment() = default;

		virtual bool TryQuery(
			const FGraphCompoundDomainRouteProfile& Profile,
			const FGraphCompoundEventQueryRequest& Request,
			const Contract::FUeAgentCallerContext& CallerContext,
			TSharedRef<ContextResource::FUeAgentResourceTracker,
				ESPMode::ThreadSafe> Resources,
			const TSharedRef<Contract::IUeAgentCancellationSignal>&
				Cancellation,
			Contract::FUeAgentContractValue& OutResult,
			FGraphCompoundRuntimeError& OutError) = 0;

		virtual bool TryCaptureAssetCreateSnapshot(
			const FGraphCompoundDomainRouteProfile& Profile,
			const FGraphCompoundAssetCreateRequest& Request,
			TSharedRef<ContextResource::FUeAgentResourceTracker,
				ESPMode::ThreadSafe> Resources,
			const TSharedRef<Contract::IUeAgentCancellationSignal>&
				Cancellation,
			StateSemantic::FUeAgentCanonicalSnapshotPtr& OutSnapshot,
			FGraphCompoundRuntimeError& OutError) = 0;

		virtual bool TryExecuteAssetCreate(
			const FGraphCompoundDomainRouteProfile& Profile,
			const FGraphCompoundAssetCreateRequest& Request,
			const PlanningPreflight::FPlanningPreflightPlanRef& Plan,
			const Contract::FUeAgentCallerContext& CallerContext,
			TSharedRef<ContextResource::FUeAgentResourceTracker,
				ESPMode::ThreadSafe> Resources,
			const TSharedRef<Contract::IUeAgentCancellationSignal>&
				Cancellation,
			Contract::FUeAgentContractValue& OutResult,
			FGraphCompoundRuntimeError& OutError) = 0;

		virtual bool TryCaptureTypedOperationSnapshot(
			const FGraphCompoundDomainRouteProfile& Profile,
			const FString& TargetRef,
			const TOptional<FGraphCompoundNiagaraMutationSelector>&
				NiagaraSelector,
			TSharedRef<ContextResource::FUeAgentResourceTracker,
				ESPMode::ThreadSafe> Resources,
			const TSharedRef<Contract::IUeAgentCancellationSignal>&
				Cancellation,
			StateSemantic::FUeAgentCanonicalSnapshotPtr& OutSnapshot,
			FGraphCompoundRuntimeError& OutError) = 0;

		virtual bool TryExecuteTypedOperation(
			const FGraphCompoundDomainRouteProfile& Profile,
			const FString& TargetRef,
			const TOptional<FGraphCompoundNiagaraMutationSelector>&
				NiagaraSelector,
			const FGraphCompoundCanonicalPayload& Payload,
			const PlanningPreflight::FPlanningPreflightPlanRef& Plan,
			const Contract::FUeAgentCallerContext& CallerContext,
			TSharedRef<ContextResource::FUeAgentResourceTracker,
				ESPMode::ThreadSafe> Resources,
			const TSharedRef<Contract::IUeAgentCancellationSignal>&
				Cancellation,
			Contract::FUeAgentContractValue& OutResult,
			FGraphCompoundRuntimeError& OutError) = 0;

	};

	struct UEAGENTINTERFACE_API
	FGraphCompoundBlueprintMaterialProductionConfig
	{
		/** Empty selects Saved/UeAgentInterface/GraphCompoundDurable. */
		FString DurableJournalRoot;
	};

	/** Creates the complete exact query and operation owner set. */
	UEAGENTINTERFACE_API bool TryCreateBlueprintMaterialGraphBindings(
		TSharedRef<IGraphCompoundBlueprintMaterialEnvironment> Environment,
		TArray<TSharedRef<IGraphCompoundQueryBinding>>& OutQueryBindings,
		TArray<TSharedRef<IGraphCompoundOperationBinding>>&
			OutOperationBindings,
		FGraphCompoundRuntimeError& OutError);

	/** Creates the real UE/C5 environment used by production activation. */
	UEAGENTINTERFACE_API bool
	TryCreateBlueprintMaterialGraphProductionEnvironment(
		const FGraphCompoundBlueprintMaterialProductionConfig& Config,
		TSharedPtr<IGraphCompoundBlueprintMaterialEnvironment>&
			OutEnvironment,
		FGraphCompoundRuntimeError& OutError);

	/** Domain-scoped runtime; the 77-descriptor outer assembly remains RT-007. */
	UEAGENTINTERFACE_API bool
	TryCreateBlueprintMaterialGraphProductionRuntime(
		const FGraphCompoundRuntimeConfig& RuntimeConfig,
		const FGraphCompoundBlueprintMaterialProductionConfig&
			ProductionConfig,
		TSharedPtr<FGraphCompoundRuntime>& OutRuntime,
		FGraphCompoundRuntimeError& OutError);
}

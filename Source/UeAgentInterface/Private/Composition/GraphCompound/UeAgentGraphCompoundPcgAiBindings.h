#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "Composition/GraphCompound/UeAgentGraphCompoundPcgAnimationRoutes.h"
#include "Composition/GraphCompound/UeAgentGraphCompoundRuntime.h"

#define UAI_GRAPH_COMPOUND_PCG_AI_BINDINGS_V1 1

namespace UE::UAI::Composition::GraphCompound
{
	/**
	 * Pre-C4 typed target. It contains no raw public request and no canonical
	 * payload, because canonical identities require the real C4 snapshot.
	 */
	class UEAGENTINTERFACE_API FGraphCompoundPcgAiTargetRequest final
	{
	public:
		FGraphCompoundPcgAiTargetRequest(
			FString InTargetRef,
			FString InTargetKind);

		const FString& GetTargetRef() const;
		const FString& GetTargetKind() const;
		bool IsValid() const;
		const Contract::FDiagnostic& GetValidationDiagnostic() const;

	private:
		FString TargetRef;
		FString TargetKind;
		Contract::FDiagnostic ValidationDiagnostic;
	};

	/** Typed post-C4 request passed to native execution. */
	class UEAGENTINTERFACE_API FGraphCompoundPcgAiOperationRequest final
	{
	public:
		FGraphCompoundPcgAiOperationRequest(
			FGraphCompoundPcgAiTargetRequest InTarget,
			FGraphCompoundCanonicalPayload InPayload);

		const FGraphCompoundPcgAiTargetRequest& GetTarget() const;
		const FString& GetTargetRef() const;
		const FString& GetTargetKind() const;
		const FGraphCompoundCanonicalPayload& GetPayload() const;
		bool IsValid() const;
		const Contract::FDiagnostic& GetValidationDiagnostic() const;

	private:
		FGraphCompoundPcgAiTargetRequest Target;
		FGraphCompoundCanonicalPayload Payload;
		Contract::FDiagnostic ValidationDiagnostic;
	};

	/**
	 * Native PCG/AI production seam. Exact descriptor selection, public schema
	 * validation, typed decoding and C6 planning are complete before execute.
	 */
	class UEAGENTINTERFACE_API IGraphCompoundPcgAiEnvironment
	{
	public:
		virtual ~IGraphCompoundPcgAiEnvironment() = default;

		virtual bool TryCaptureSnapshot(
			const FPcgAnimationGraphRouteProfile& Profile,
			const FGraphCompoundPcgAiTargetRequest& Request,
			TSharedRef<ContextResource::FUeAgentResourceTracker,
				ESPMode::ThreadSafe> Resources,
			const TSharedRef<Contract::IUeAgentCancellationSignal>&
				Cancellation,
			StateSemantic::FUeAgentCanonicalSnapshotPtr& OutSnapshot,
			FGraphCompoundRuntimeError& OutError) = 0;

		virtual bool TryExecute(
			const FPcgAnimationGraphRouteProfile& Profile,
			const FGraphCompoundPcgAiOperationRequest& Request,
			const PlanningPreflight::FPlanningPreflightPlanRef& Plan,
			const Contract::FUeAgentCallerContext& CallerContext,
			TSharedRef<ContextResource::FUeAgentResourceTracker,
				ESPMode::ThreadSafe> Resources,
			const TSharedRef<Contract::IUeAgentCancellationSignal>&
				Cancellation,
			Contract::FUeAgentContractValue& OutResult,
			FGraphCompoundRuntimeError& OutError) = 0;
	};

	struct UEAGENTINTERFACE_API FGraphCompoundPcgAiProductionConfig
	{
		/** Empty selects Saved/UeAgentInterface/GraphCompoundDurable. */
		FString DurableJournalRoot;
	};

	/** Projects the exact 17 PCG/AI descriptors from the combined route truth. */
	UEAGENTINTERFACE_API bool TryCreatePcgAiGraphDescriptorRegistry(
		TSharedPtr<const Contract::FUeAgentDescriptorRegistry>& OutRegistry,
		UE::UAI::Contract::FDiagnostic& OutError);

	/** Creates one exact operation owner for each PCG/AI descriptor. */
	UEAGENTINTERFACE_API bool TryCreatePcgAiGraphBindings(
		TSharedRef<IGraphCompoundPcgAiEnvironment> Environment,
		TArray<TSharedRef<IGraphCompoundOperationBinding>>& OutBindings,
		FGraphCompoundRuntimeError& OutError);

	UEAGENTINTERFACE_API bool TryCreatePcgAiGraphProductionEnvironment(
		const FGraphCompoundPcgAiProductionConfig& Config,
		TSharedPtr<IGraphCompoundPcgAiEnvironment>& OutEnvironment,
		FGraphCompoundRuntimeError& OutError);

	/** Domain-scoped runtime; outer 77-descriptor assembly remains RT-007. */
	UEAGENTINTERFACE_API bool TryCreatePcgAiGraphProductionRuntime(
		const FGraphCompoundRuntimeConfig& RuntimeConfig,
		const FGraphCompoundPcgAiProductionConfig& ProductionConfig,
		TSharedPtr<FGraphCompoundRuntime>& OutRuntime,
		FGraphCompoundRuntimeError& OutError);
}

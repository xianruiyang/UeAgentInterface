#pragma once

#include "Composition/GraphCompound/UeAgentGraphCompoundPcgAnimationRoutes.h"
#include "Composition/GraphCompound/UeAgentGraphCompoundRuntime.h"

#define UAI_GRAPH_COMPOUND_ANIMATION_BINDINGS_V1 1

namespace UE::UAI::Composition::GraphCompound
{
	class UEAGENTINTERFACE_API FGraphCompoundAnimationTargetRequest final
	{
	public:
		FGraphCompoundAnimationTargetRequest(
			FString InTargetRef,
			FString InTargetKind);

		const FString& GetTargetRef() const;
		const FString& GetTargetKind() const;
		bool IsValid() const;

	private:
		FString TargetRef;
		FString TargetKind;
		bool bValid = false;
	};

	class UEAGENTINTERFACE_API FGraphCompoundAnimationOperationRequest final
	{
	public:
		FGraphCompoundAnimationOperationRequest(
			FGraphCompoundAnimationTargetRequest InTarget,
			FGraphCompoundCanonicalPayload InPayload);

		const FGraphCompoundAnimationTargetRequest& GetTarget() const;
		const FString& GetTargetRef() const;
		const FString& GetTargetKind() const;
		const FGraphCompoundCanonicalPayload& GetPayload() const;
		bool IsValid() const;

	private:
		FGraphCompoundAnimationTargetRequest Target;
		FGraphCompoundCanonicalPayload Payload;
		bool bValid = false;
	};

	class UEAGENTINTERFACE_API IGraphCompoundAnimationEnvironment
	{
	public:
		virtual ~IGraphCompoundAnimationEnvironment() = default;

		virtual bool TryCaptureSnapshot(
			const FPcgAnimationGraphRouteProfile& Profile,
			const FGraphCompoundAnimationTargetRequest& Request,
			TSharedRef<ContextResource::FUeAgentResourceTracker,
				ESPMode::ThreadSafe> Resources,
			const TSharedRef<Contract::IUeAgentCancellationSignal>& Cancellation,
			StateSemantic::FUeAgentCanonicalSnapshotPtr& OutSnapshot,
			FGraphCompoundRuntimeError& OutError) = 0;

		virtual bool TryExecute(
			const FPcgAnimationGraphRouteProfile& Profile,
			const FGraphCompoundAnimationOperationRequest& Request,
			const PlanningPreflight::FPlanningPreflightPlanRef& Plan,
			const Contract::FUeAgentCallerContext& CallerContext,
			TSharedRef<ContextResource::FUeAgentResourceTracker,
				ESPMode::ThreadSafe> Resources,
			const TSharedRef<Contract::IUeAgentCancellationSignal>& Cancellation,
			Contract::FUeAgentContractValue& OutResult,
			FGraphCompoundRuntimeError& OutError) = 0;
	};

	struct UEAGENTINTERFACE_API FGraphCompoundAnimationProductionConfig
	{
		FString DurableJournalRoot;
		double OperationTimeoutSeconds = 120.0;

		bool IsValid() const;
	};

	UEAGENTINTERFACE_API bool TryCreateAnimationGraphDescriptorRegistry(
		TSharedPtr<const Contract::FUeAgentDescriptorRegistry>& OutRegistry,
		FString& OutError);

	UEAGENTINTERFACE_API bool TryCreateAnimationGraphBindings(
		TSharedRef<IGraphCompoundAnimationEnvironment> Environment,
		TArray<TSharedRef<IGraphCompoundOperationBinding>>& OutBindings,
		FGraphCompoundRuntimeError& OutError);

	UEAGENTINTERFACE_API bool TryCreateAnimationGraphProductionEnvironment(
		const FGraphCompoundAnimationProductionConfig& Config,
		TSharedPtr<IGraphCompoundAnimationEnvironment>& OutEnvironment,
		FGraphCompoundRuntimeError& OutError);

	UEAGENTINTERFACE_API bool TryCreateAnimationGraphProductionRuntime(
		const FGraphCompoundRuntimeConfig& RuntimeConfig,
		const FGraphCompoundAnimationProductionConfig& ProductionConfig,
		TSharedPtr<FGraphCompoundRuntime>& OutRuntime,
		FGraphCompoundRuntimeError& OutError);
}

#pragma once

#include "CommandEdge/UeAgentCommandDispatch.h"
#include "Composition/GraphCompound/UeAgentGraphCompoundAudioMediaRoutes.h"
#include "Composition/GraphCompound/UeAgentGraphCompoundRuntime.h"
#include "Core/BindingGateway/Operation/UeAgentOperationLifecycleService.h"

#define UAI_GRAPH_COMPOUND_AUDIO_MEDIA_BINDINGS_V1 1

namespace UE::UAI::Composition::GraphCompound
{
	class UEAGENTINTERFACE_API IGraphCompoundAudioMediaLifecycleContext
	{
	public:
		virtual ~IGraphCompoundAudioMediaLifecycleContext() = default;

		virtual bool TryBuildResult(
			const BindingGateway::Operation::FOperationLifecycleReceipt& Receipt,
			Contract::FUeAgentContractValue& OutResult,
			FGraphCompoundRuntimeError& OutError) const = 0;
	};

	class UEAGENTINTERFACE_API FGraphCompoundAudioMediaLifecycleInvocation final
	{
	public:
		FGraphCompoundAudioMediaLifecycleInvocation() = default;

		static bool TryCreate(
			TSharedRef<const BindingGateway::Operation::FOperationLifecyclePlan>
				Plan,
			TSharedRef<const BindingGateway::Operation::
				FOperationImpactAuthorization> Authorization,
			TSharedRef<BindingGateway::Operation::FOperationLifecycleService>
				Service,
			TSharedRef<IGraphCompoundAudioMediaLifecycleContext> Context,
			FGraphCompoundAudioMediaLifecycleInvocation& OutInvocation,
			FGraphCompoundRuntimeError& OutError);

		bool IsValid() const;
		TSharedRef<const BindingGateway::Operation::FOperationLifecyclePlan>
			GetPlan() const;
		TSharedRef<const BindingGateway::Operation::
			FOperationImpactAuthorization> GetAuthorization() const;
		TSharedRef<BindingGateway::Operation::FOperationLifecycleService>
			GetService() const;
		TSharedRef<IGraphCompoundAudioMediaLifecycleContext> GetContext() const;

	private:
		TSharedPtr<const BindingGateway::Operation::FOperationLifecyclePlan> Plan;
		TSharedPtr<const BindingGateway::Operation::
			FOperationImpactAuthorization> Authorization;
		TSharedPtr<BindingGateway::Operation::FOperationLifecycleService> Service;
		TSharedPtr<IGraphCompoundAudioMediaLifecycleContext> Context;
	};

	class UEAGENTINTERFACE_API FGraphCompoundAudioMediaTargetRequest final
	{
	public:
		FGraphCompoundAudioMediaTargetRequest(
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

	class UEAGENTINTERFACE_API FGraphCompoundAudioMediaOperationRequest final
	{
	public:
		FGraphCompoundAudioMediaOperationRequest(
			FGraphCompoundAudioMediaTargetRequest InTarget,
			FGraphCompoundCanonicalPayload InPayload);

		const FGraphCompoundAudioMediaTargetRequest& GetTarget() const;
		const FString& GetTargetRef() const;
		const FString& GetTargetKind() const;
		const FGraphCompoundCanonicalPayload& GetPayload() const;
		bool IsValid() const;

	private:
		FGraphCompoundAudioMediaTargetRequest Target;
		FGraphCompoundCanonicalPayload Payload;
		bool bValid = false;
	};

	class UEAGENTINTERFACE_API IGraphCompoundAudioMediaEnvironment
	{
	public:
		virtual ~IGraphCompoundAudioMediaEnvironment() = default;

		virtual bool TryCaptureSnapshot(
			const FAudioMediaGraphRouteProfile& Profile,
			const FGraphCompoundAudioMediaTargetRequest& Request,
			TSharedRef<ContextResource::FUeAgentResourceTracker,
				ESPMode::ThreadSafe> Resources,
			const TSharedRef<Contract::IUeAgentCancellationSignal>& Cancellation,
			StateSemantic::FUeAgentCanonicalSnapshotPtr& OutSnapshot,
			FGraphCompoundRuntimeError& OutError) = 0;

		virtual bool TryExecuteOperation(
			const FAudioMediaGraphRouteProfile& Profile,
			const FGraphCompoundAudioMediaOperationRequest& Request,
			const PlanningPreflight::FPlanningPreflightPlanRef& Plan,
			const Contract::FUeAgentCallerContext& CallerContext,
			TSharedRef<ContextResource::FUeAgentResourceTracker,
				ESPMode::ThreadSafe> Resources,
			const TSharedRef<Contract::IUeAgentCancellationSignal>& Cancellation,
			Contract::FUeAgentContractValue& OutResult,
			FGraphCompoundRuntimeError& OutError) = 0;

		virtual bool TryCreateOperationLifecycleInvocation(
			const FAudioMediaGraphRouteProfile& Profile,
			const FGraphCompoundAudioMediaOperationRequest& Request,
			const PlanningPreflight::FPlanningPreflightPlanRef& Plan,
			const Contract::FUeAgentCallerContext& CallerContext,
			TSharedRef<ContextResource::FUeAgentResourceTracker,
				ESPMode::ThreadSafe> Resources,
			const TSharedRef<Contract::IUeAgentCancellationSignal>& Cancellation,
			FGraphCompoundAudioMediaLifecycleInvocation& OutInvocation,
			FGraphCompoundRuntimeError& OutError) = 0;

		virtual bool TryExecuteQuery(
			const FAudioMediaGraphRouteProfile& Profile,
			const FAudioMediaGraphArtifactQueryRequest& Request,
			const Contract::FUeAgentCallerContext& CallerContext,
			TSharedRef<ContextResource::FUeAgentResourceTracker,
				ESPMode::ThreadSafe> Resources,
			const TSharedRef<Contract::IUeAgentCancellationSignal>& Cancellation,
			Contract::FUeAgentContractValue& OutResult,
			FGraphCompoundRuntimeError& OutError) = 0;
	};

	struct UEAGENTINTERFACE_API FGraphCompoundAudioMediaProductionConfig
	{
		FString DurableJournalRoot;
		double OperationTimeoutSeconds = 120.0;

		bool IsValid() const;
	};

	UEAGENTINTERFACE_API bool TryCreateAudioMediaGraphBindings(
		TSharedRef<IGraphCompoundAudioMediaEnvironment> Environment,
		TArray<TSharedRef<IGraphCompoundQueryBinding>>& OutQueryBindings,
		TArray<TSharedRef<IGraphCompoundOperationBinding>>& OutOperationBindings,
		FGraphCompoundRuntimeError& OutError);

	UEAGENTINTERFACE_API bool TryCreateAudioMediaGraphProductionEnvironment(
		const FGraphCompoundAudioMediaProductionConfig& Config,
		TSharedPtr<IGraphCompoundAudioMediaEnvironment>& OutEnvironment,
		FGraphCompoundRuntimeError& OutError);

	UEAGENTINTERFACE_API bool TryCreateAudioMediaGraphProductionRuntime(
		const FGraphCompoundRuntimeConfig& RuntimeConfig,
		const FGraphCompoundAudioMediaProductionConfig& ProductionConfig,
		TSharedPtr<FGraphCompoundRuntime>& OutRuntime,
		FGraphCompoundRuntimeError& OutError);

	UEAGENTINTERFACE_API bool
	TryCreateAudioMediaGraphOperationLifecycleInvocation(
		const FString& CommandId,
		const CommandEdge::FUeAgentCommandRequest& Request,
		const Contract::FUeAgentCallerContext& CallerContext,
		const TSharedRef<Contract::IUeAgentCancellationSignal>& Cancellation,
		const FGraphCompoundAudioMediaProductionConfig& ProductionConfig,
		FGraphCompoundAudioMediaLifecycleInvocation& OutInvocation,
		FGraphCompoundRuntimeError& OutError);
}

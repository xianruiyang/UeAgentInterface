#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

class FJsonValue;
namespace UE::UAI::StateSemantic { class FUeAgentCanonicalSnapshot; }

#include "CommandEdge/UeAgentCommandDispatch.h"
#include "Composition/GraphCompound/UeAgentGraphCompoundAudioMediaRoutes.h"
#include "Composition/GraphCompound/UeAgentGraphCompoundRuntime.h"
#include "Core/BindingGateway/Operation/UeAgentOperationLifecycleService.h"
#include "DomainAdapters/AudioMediaGraph/UeAgentAudioMediaGraphTextureGraphNativeHook.h"

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
		const Contract::FDiagnostic& GetValidationDiagnostic() const;

	private:
		FString TargetRef;
		FString TargetKind;
		Contract::FDiagnostic ValidationDiagnostic;
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
		const Contract::FDiagnostic& GetValidationDiagnostic() const;

	private:
		FGraphCompoundAudioMediaTargetRequest Target;
		FGraphCompoundCanonicalPayload Payload;
		Contract::FDiagnostic ValidationDiagnostic;
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
	};

	struct UEAGENTINTERFACE_API FTextureGraphSemanticCompositeRequest final
	{
		FString TargetRef;
		DomainAdapters::AudioMediaGraph::FAudioMediaGraphTextureGraphNodeBindings NodeBindings;
		TArray<DomainAdapters::AudioMediaGraph::
			FAudioMediaGraphTextureGraphNodeMutation> Nodes;
		TArray<DomainAdapters::AudioMediaGraph::
			FAudioMediaGraphTextureGraphParameterMutation> Parameters;
		TArray<DomainAdapters::AudioMediaGraph::
			FAudioMediaGraphTextureGraphLinkMutation> Links;

		// Reflected TG_Setting descriptors keyed by the captured native node identity.
		TMap<int32, TArray<TSharedPtr<FJsonValue>>> Settings;
		// Inline node properties have no captured index until native creation completes.
		TMap<FString, TArray<TSharedPtr<FJsonValue>>> ElementSettings;
		// The operation's observed pure-value scope, borrowed through write-before
		// validation. No native object bindings or complete authoring model.
		TSharedPtr<const StateSemantic::FUeAgentCanonicalSnapshot> BeforeSnapshot;

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

	UEAGENTINTERFACE_API bool TryValidateTextureGraphSemanticComposite(
		const FTextureGraphSemanticCompositeRequest& Request,
		const PlanningPreflight::FPlanningPreflightPlanRef& Plan,
		const TSharedRef<Contract::IUeAgentCancellationSignal>& Cancellation,
		FGraphCompoundRuntimeError& OutError);
	UEAGENTINTERFACE_API bool TryApplyTextureGraphSemanticCompositeNative(
		UObject& Asset, const FTextureGraphSemanticCompositeRequest& Request,
		FGraphCompoundRuntimeError& OutError,
		DomainAdapters::AudioMediaGraph::FAudioMediaGraphTextureGraphNodeBindings& Bindings);
	UEAGENTINTERFACE_API bool TryVerifyTextureGraphSemanticCompositeNative(
		UObject& Asset, const FTextureGraphSemanticCompositeRequest& Request,
		UE::UAI::Contract::FDiagnostic& OutError,
		const DomainAdapters::AudioMediaGraph::FAudioMediaGraphTextureGraphNodeBindings& Bindings);
}

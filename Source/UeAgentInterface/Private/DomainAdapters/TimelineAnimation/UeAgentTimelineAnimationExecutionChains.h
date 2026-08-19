#pragma once

#include "CoreMinimal.h"
#include "Core/BindingGateway/Operation/UeAgentOperationBinding.h"
#include "Core/BindingGateway/Query/UeAgentDomainQuery.h"
#include "Core/Contract/UeAgentContract.h"
#include "Core/PlanningPreflight/UeAgentPlanningPreflight.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "Templates/SharedPointer.h"

#define UAI_TIMELINE_ANIMATION_EXECUTION_CHAINS_V1 1

namespace UE::UAI::DomainAdapters::TimelineAnimation
{
	class UEAGENTINTERFACE_API FUeAgentTimelineAnimationExecutionChainContract final
	{
	public:
		FUeAgentTimelineAnimationExecutionChainContract(
			FString InCapabilityId,
			FString InDescriptorId,
			FString InOperationKind,
			FString InC3Lane,
			FString InFamilyId,
			FString InBindingId,
			FString InTypedRequestId,
			FString InTypedResultId,
			FString InC6ChangeKind,
			FString InNativeActionId,
			FString InReadbackId,
			FString InReadbackOwner,
			FString InSettleBarrier,
			FString InOracleKind,
			bool bInRequiresNativeMutation,
			bool bInReadsBackAfterMutation);

		const FString& GetCapabilityId() const;
		const FString& GetDescriptorId() const;
		const FString& GetOperationKind() const;
		const FString& GetC3Lane() const;
		const FString& GetFamilyId() const;
		const FString& GetBindingId() const;
		const FString& GetTypedOperationId() const;
		const FString& GetTemporalContractId() const;
		const FString& GetTypedRequestId() const;
		const FString& GetTypedResultId() const;
		const FString& GetC6ChangeKind() const;
		const FString& GetNativeActionId() const;
		const FString& GetReadbackId() const;
		const FString& GetReadbackOwner() const;
		const FString& GetSettleBarrier() const;
		const FString& GetOracleKind() const;
		bool RequiresNativeMutation() const;
		bool ReadsBackAfterMutation() const;

	private:
		FString CapabilityId;
		FString DescriptorId;
		FString OperationKind;
		FString C3Lane;
		FString FamilyId;
		FString BindingId;
		FString TemporalContractId = TEXT("uai.temporal_contract.v1");
		FString TypedRequestId;
		FString TypedResultId;
		FString C6ChangeKind;
		FString NativeActionId;
		FString ReadbackId;
		FString ReadbackOwner;
		FString SettleBarrier;
		FString OracleKind;
		bool bRequiresNativeMutation = false;
		bool bReadsBackAfterMutation = false;
	};

	UEAGENTINTERFACE_API const TArray<FUeAgentTimelineAnimationExecutionChainContract>&
	GetTimelineAnimationExecutionChainContracts();

	struct UEAGENTINTERFACE_API FUeAgentTimelineAnimationExecutionError
	{
		FString Code;
		FString SubjectId;
		FString Detail;

		bool IsValid() const { return !Code.IsEmpty(); }
	};

	class UEAGENTINTERFACE_API IUeAgentTimelineAnimationTypedRequest
	{
	public:
		virtual ~IUeAgentTimelineAnimationTypedRequest() = default;
		virtual const FString& GetTypeId() const = 0;
		virtual const FString& GetFamilyId() const = 0;
		virtual const FString& GetRequestKindId() const = 0;
		virtual bool TryBuildChangeDraft(
			const UE::UAI::StateSemantic::FUeAgentCanonicalSnapshot& Snapshot,
			const UE::UAI::ContextResource::FUeAgentOperationContext& Context,
			TOptional<UE::UAI::PlanningPreflight::FChangeDraft>& OutDraft,
			FUeAgentTimelineAnimationExecutionError& OutError) const = 0;
	};

	using FUeAgentTimelineAnimationTypedRequestPtr =
		TSharedPtr<const IUeAgentTimelineAnimationTypedRequest>;
	using FUeAgentTimelineAnimationTypedRequestRef =
		TSharedRef<const IUeAgentTimelineAnimationTypedRequest>;

	class UEAGENTINTERFACE_API IUeAgentTimelineAnimationRequestDecoder
	{
	public:
		virtual ~IUeAgentTimelineAnimationRequestDecoder() = default;
		virtual bool TryDecode(
			const FUeAgentTimelineAnimationExecutionChainContract& Contract,
			const UE::UAI::Contract::FUeAgentContractValue& CanonicalInput,
			UE::UAI::ContextResource::FUeAgentResourceTracker& Resources,
			FUeAgentTimelineAnimationTypedRequestPtr& OutRequest,
			FUeAgentTimelineAnimationExecutionError& OutError) = 0;
	};

	enum class EUeAgentTimelineAnimationReadPhase : uint8
	{
		Query,
		Preflight,
		Verification,
	};

	class UEAGENTINTERFACE_API IUeAgentTimelineAnimationReadbackPort
	{
	public:
		virtual ~IUeAgentTimelineAnimationReadbackPort() = default;
		virtual bool TryRead(
			const FUeAgentTimelineAnimationExecutionChainContract& Contract,
			const FUeAgentTimelineAnimationTypedRequestRef& Request,
			const TOptional<UE::UAI::Discovery::FUeAgentCandidateBinding>& Target,
			EUeAgentTimelineAnimationReadPhase Phase,
			const UE::UAI::ContextResource::FUeAgentOperationContext& Context,
			TSharedPtr<const UE::UAI::StateSemantic::FUeAgentCanonicalSnapshot>& OutSnapshot,
			FUeAgentTimelineAnimationExecutionError& OutError) = 0;
	};

	class UEAGENTINTERFACE_API FUeAgentTimelineAnimationMutationResult final
	{
	public:
		FUeAgentTimelineAnimationMutationResult() = default;
		FUeAgentTimelineAnimationMutationResult(
			FString InOperationId,
			FString InMutationEvidenceId);

		const FString& GetOperationId() const;
		const FString& GetMutationEvidenceId() const;
		bool IsValid() const;

	private:
		FString OperationId;
		FString MutationEvidenceId;
		bool bValid = false;
	};

	class UEAGENTINTERFACE_API IUeAgentTimelineAnimationMutationPort
	{
	public:
		virtual ~IUeAgentTimelineAnimationMutationPort() = default;
		virtual bool TryMutate(
			const FUeAgentTimelineAnimationExecutionChainContract& Contract,
			const FUeAgentTimelineAnimationTypedRequestRef& Request,
			const TArray<UE::UAI::BindingGateway::Operation::FUeAgentOperationTargetBinding>& Targets,
			const UE::UAI::ContextResource::FUeAgentOperationContext& Context,
			FUeAgentTimelineAnimationMutationResult& OutResult,
			FUeAgentTimelineAnimationExecutionError& OutError) = 0;
	};

	class UEAGENTINTERFACE_API IUeAgentTimelineAnimationSettlePort
	{
	public:
		virtual ~IUeAgentTimelineAnimationSettlePort() = default;
		virtual bool TrySettle(
			const FUeAgentTimelineAnimationExecutionChainContract& Contract,
			const FUeAgentTimelineAnimationMutationResult& Mutation,
			const UE::UAI::ContextResource::FUeAgentOperationContext& Context,
			FUeAgentTimelineAnimationExecutionError& OutError) = 0;
	};

	class UEAGENTINTERFACE_API IUeAgentTimelineAnimationResultComparator
	{
	public:
		virtual ~IUeAgentTimelineAnimationResultComparator() = default;
		virtual bool TryCompare(
			const FUeAgentTimelineAnimationExecutionChainContract& Contract,
			const FUeAgentTimelineAnimationTypedRequestRef& Request,
			const UE::UAI::StateSemantic::FUeAgentCanonicalSnapshot& Before,
			const UE::UAI::StateSemantic::FUeAgentCanonicalSnapshot& After,
			const UE::UAI::ContextResource::FUeAgentOperationContext& Context,
			UE::UAI::Verification::FVerificationOutcome& OutOutcome,
			FUeAgentTimelineAnimationExecutionError& OutError) = 0;
	};

	class UEAGENTINTERFACE_API FUeAgentTimelineAnimationOperationChain final
	{
	public:
		class FState;

		const FUeAgentTimelineAnimationExecutionChainContract& GetContract() const;
		bool TryBuildPlanningRequest(
			const UE::UAI::Contract::FUeAgentContractValue& CanonicalInput,
			const TArray<UE::UAI::BindingGateway::Operation::FUeAgentOperationTargetBinding>& Targets,
			const UE::UAI::ContextResource::FUeAgentOperationContext& Context,
			TOptional<UE::UAI::PlanningPreflight::FPlanningRequest>& OutRequest,
			UE::UAI::BindingGateway::Operation::FUeAgentOperationBindingError& OutError);
		UE::UAI::BindingGateway::Operation::IUeAgentOperationPlanner& GetPlanner() const;
		UE::UAI::BindingGateway::Operation::IUeAgentOperationExecutor& GetExecutor() const;
		UE::UAI::BindingGateway::Operation::IUeAgentOperationVerifier& GetVerifier() const;

	private:
		FUeAgentTimelineAnimationOperationChain(
			FUeAgentTimelineAnimationExecutionChainContract InContract,
			TSharedRef<IUeAgentTimelineAnimationRequestDecoder> InDecoder,
			TSharedRef<IUeAgentTimelineAnimationReadbackPort> InReadback,
			TSharedRef<FState, ESPMode::ThreadSafe> InState,
			TSharedRef<UE::UAI::BindingGateway::Operation::IUeAgentOperationPlanner> InPlanner,
			TSharedRef<UE::UAI::BindingGateway::Operation::IUeAgentOperationExecutor> InExecutor,
			TSharedRef<UE::UAI::BindingGateway::Operation::IUeAgentOperationVerifier> InVerifier);

		FUeAgentTimelineAnimationExecutionChainContract Contract;
		TSharedRef<IUeAgentTimelineAnimationRequestDecoder> Decoder;
		TSharedRef<IUeAgentTimelineAnimationReadbackPort> Readback;
		TSharedRef<FState, ESPMode::ThreadSafe> State;
		TSharedRef<UE::UAI::BindingGateway::Operation::IUeAgentOperationPlanner> Planner;
		TSharedRef<UE::UAI::BindingGateway::Operation::IUeAgentOperationExecutor> Executor;
		TSharedRef<UE::UAI::BindingGateway::Operation::IUeAgentOperationVerifier> Verifier;

		friend UEAGENTINTERFACE_API bool TryCreateTimelineAnimationOperationChains(
			TSharedRef<IUeAgentTimelineAnimationRequestDecoder>,
			TSharedRef<IUeAgentTimelineAnimationReadbackPort>,
			TSharedRef<IUeAgentTimelineAnimationMutationPort>,
			TSharedRef<IUeAgentTimelineAnimationSettlePort>,
			TSharedRef<IUeAgentTimelineAnimationResultComparator>,
			TArray<TSharedRef<FUeAgentTimelineAnimationOperationChain>>&,
			FUeAgentTimelineAnimationExecutionError&);
	};

	UEAGENTINTERFACE_API bool TryCreateTimelineAnimationQueryProviderRegistry(
		TSharedRef<IUeAgentTimelineAnimationRequestDecoder> Decoder,
		TSharedRef<IUeAgentTimelineAnimationReadbackPort> Readback,
		TSharedPtr<const UE::UAI::BindingGateway::Query::FUeAgentDomainQueryProviderRegistry>& OutRegistry,
		FUeAgentTimelineAnimationExecutionError& OutError);

	UEAGENTINTERFACE_API bool TryCreateTimelineAnimationOperationChains(
		TSharedRef<IUeAgentTimelineAnimationRequestDecoder> Decoder,
		TSharedRef<IUeAgentTimelineAnimationReadbackPort> Readback,
		TSharedRef<IUeAgentTimelineAnimationMutationPort> Mutation,
		TSharedRef<IUeAgentTimelineAnimationSettlePort> Settle,
		TSharedRef<IUeAgentTimelineAnimationResultComparator> Comparator,
		TArray<TSharedRef<FUeAgentTimelineAnimationOperationChain>>& OutChains,
		FUeAgentTimelineAnimationExecutionError& OutError);
}



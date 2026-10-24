#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"
#include "Core/BindingGateway/Operation/UeAgentOperationBinding.h"
#include "Core/BindingGateway/Query/UeAgentDomainQuery.h"
#include "Core/Contract/UeAgentContract.h"
#include "Core/ExecutionMutation/UeAgentExecutionMutation.h"
#include "Core/PlanningPreflight/UeAgentPlanningPreflight.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "Templates/SharedPointer.h"

#define UAI_WORLD_SPATIAL_EXECUTION_CHAINS_V1 1

namespace UE::UAI::DomainAdapters::WorldSpatial
{
	class UEAGENTINTERFACE_API FUeAgentWorldSpatialExecutionChainContract final
	{
	public:
		FUeAgentWorldSpatialExecutionChainContract(
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

	UEAGENTINTERFACE_API const TArray<FUeAgentWorldSpatialExecutionChainContract>&
	GetWorldSpatialExecutionChainContracts();

	using FUeAgentWorldSpatialExecutionError = UE::UAI::Contract::FDiagnostic;

	struct UEAGENTINTERFACE_API FUeAgentWorldSpatialFenceEntry
	{
		FString TargetRef;
		UE::UAI::StateSemantic::FUeAgentStateRevision Revision;
	};

	struct UEAGENTINTERFACE_API FUeAgentWorldSpatialPreflightFence
	{
		TArray<FUeAgentWorldSpatialFenceEntry> Entries;
		bool TryValidate(FUeAgentWorldSpatialExecutionError& OutError) const;
		bool IsValid() const;
	};

	class UEAGENTINTERFACE_API IUeAgentWorldSpatialTypedRequest
	{
	public:
		virtual ~IUeAgentWorldSpatialTypedRequest() = default;
		virtual const FString& GetTypeId() const = 0;
		virtual const FString& GetFamilyId() const = 0;
		virtual const FString& GetRequestKindId() const = 0;
		virtual bool TryBuildChangeDraft(
			const UE::UAI::StateSemantic::FUeAgentCanonicalSnapshot& Snapshot,
			const UE::UAI::ContextResource::FUeAgentOperationContext& Context,
			TOptional<UE::UAI::PlanningPreflight::FChangeDraft>& OutDraft,
			FUeAgentWorldSpatialExecutionError& OutError) const = 0;
	};

	using FUeAgentWorldSpatialTypedRequestPtr =
		TSharedPtr<const IUeAgentWorldSpatialTypedRequest>;
	using FUeAgentWorldSpatialTypedRequestRef =
		TSharedRef<const IUeAgentWorldSpatialTypedRequest>;

	class UEAGENTINTERFACE_API IUeAgentWorldSpatialRequestDecoder
	{
	public:
		virtual ~IUeAgentWorldSpatialRequestDecoder() = default;
		virtual bool TryDecode(
			const FUeAgentWorldSpatialExecutionChainContract& Contract,
			const UE::UAI::Contract::FUeAgentContractValue& CanonicalInput,
			UE::UAI::ContextResource::FUeAgentResourceTracker& Resources,
			FUeAgentWorldSpatialTypedRequestPtr& OutRequest,
			FUeAgentWorldSpatialExecutionError& OutError,
			const FString& InputSubject = FString()) = 0;
	};

	enum class EUeAgentWorldSpatialReadPhase : uint8
	{
		Query,
		Preflight,
		Verification,
	};

	class UEAGENTINTERFACE_API IUeAgentWorldSpatialReadbackPort
	{
	public:
		virtual ~IUeAgentWorldSpatialReadbackPort() = default;
		virtual bool TryRead(
			const FUeAgentWorldSpatialExecutionChainContract& Contract,
			const FUeAgentWorldSpatialTypedRequestRef& Request,
			const TOptional<UE::UAI::Discovery::FUeAgentCandidateBinding>& Target,
			EUeAgentWorldSpatialReadPhase Phase,
			const UE::UAI::ContextResource::FUeAgentOperationContext& Context,
			TSharedPtr<const UE::UAI::StateSemantic::FUeAgentCanonicalSnapshot>& OutSnapshot,
			FUeAgentWorldSpatialExecutionError& OutError) = 0;
	};

	class UEAGENTINTERFACE_API FUeAgentWorldSpatialMutationResult final
	{
	public:
		FUeAgentWorldSpatialMutationResult();
		// Failure evidence only: default/invalid results still mean unknown.
		static FUeAgentWorldSpatialMutationResult NoMutation();
		bool IsNoMutationConfirmed() const;
		FUeAgentWorldSpatialMutationResult(
			FString InOperationId,
			FString InMutationEvidenceId);

		const FString& GetOperationId() const;
		const FString& GetMutationEvidenceId() const;
		bool TryValidate(FUeAgentWorldSpatialExecutionError& OutError) const;
		bool IsValid() const;

	private:
		FString OperationId;
		FString MutationEvidenceId;
		UE::UAI::ExecutionMutation::FMutationFacts FailureFacts;
		FUeAgentWorldSpatialExecutionError ValidationDiagnostic;
	};

	class UEAGENTINTERFACE_API IUeAgentWorldSpatialMutationPort
	{
	public:
		virtual ~IUeAgentWorldSpatialMutationPort() = default;
		virtual bool TryMutate(
			const FUeAgentWorldSpatialExecutionChainContract& Contract,
			const FUeAgentWorldSpatialTypedRequestRef& Request,
			const TArray<UE::UAI::BindingGateway::Operation::FUeAgentOperationTargetBinding>& Targets,
			const FUeAgentWorldSpatialPreflightFence& Fence,
			const UE::UAI::ContextResource::FUeAgentOperationContext& Context,
			FUeAgentWorldSpatialMutationResult& OutResult,
			FUeAgentWorldSpatialExecutionError& OutError) = 0;
		virtual bool TryCaptureFence(
			const FUeAgentWorldSpatialExecutionChainContract& Contract,
			const FUeAgentWorldSpatialTypedRequestRef& Request,
			const TArray<UE::UAI::BindingGateway::Operation::FUeAgentOperationTargetBinding>& Targets,
			const UE::UAI::ContextResource::FUeAgentOperationContext& Context,
			FUeAgentWorldSpatialPreflightFence& OutFence,
			FUeAgentWorldSpatialExecutionError& OutError) = 0;
	};

	class UEAGENTINTERFACE_API IUeAgentWorldSpatialSettlePort
	{
	public:
		virtual ~IUeAgentWorldSpatialSettlePort() = default;
		virtual bool TrySettle(
			const FUeAgentWorldSpatialExecutionChainContract& Contract,
			const FUeAgentWorldSpatialMutationResult& Mutation,
			const UE::UAI::ContextResource::FUeAgentOperationContext& Context,
			FUeAgentWorldSpatialExecutionError& OutError) = 0;
	};

	class UEAGENTINTERFACE_API IUeAgentWorldSpatialResultComparator
	{
	public:
		virtual ~IUeAgentWorldSpatialResultComparator() = default;
		virtual bool TryCompare(
			const FUeAgentWorldSpatialExecutionChainContract& Contract,
			const FUeAgentWorldSpatialTypedRequestRef& Request,
			const UE::UAI::StateSemantic::FUeAgentCanonicalSnapshot& Before,
			const UE::UAI::StateSemantic::FUeAgentCanonicalSnapshot& After,
			const UE::UAI::ContextResource::FUeAgentOperationContext& Context,
			UE::UAI::Verification::FVerificationOutcome& OutOutcome,
			FUeAgentWorldSpatialExecutionError& OutError) = 0;
	};

	class UEAGENTINTERFACE_API FUeAgentWorldSpatialOperationChain final
	{
	public:
		class FState;

		const FUeAgentWorldSpatialExecutionChainContract& GetContract() const;
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
		FUeAgentWorldSpatialOperationChain(
			FUeAgentWorldSpatialExecutionChainContract InContract,
			TSharedRef<IUeAgentWorldSpatialRequestDecoder> InDecoder,
			TSharedRef<IUeAgentWorldSpatialReadbackPort> InReadback,
			TSharedRef<FState, ESPMode::ThreadSafe> InState,
			TSharedRef<UE::UAI::BindingGateway::Operation::IUeAgentOperationPlanner> InPlanner,
			TSharedRef<UE::UAI::BindingGateway::Operation::IUeAgentOperationExecutor> InExecutor,
			TSharedRef<UE::UAI::BindingGateway::Operation::IUeAgentOperationVerifier> InVerifier);

		FUeAgentWorldSpatialExecutionChainContract Contract;
		TSharedRef<IUeAgentWorldSpatialRequestDecoder> Decoder;
		TSharedRef<IUeAgentWorldSpatialReadbackPort> Readback;
		TSharedRef<FState, ESPMode::ThreadSafe> State;
		TSharedRef<UE::UAI::BindingGateway::Operation::IUeAgentOperationPlanner> Planner;
		TSharedRef<UE::UAI::BindingGateway::Operation::IUeAgentOperationExecutor> Executor;
		TSharedRef<UE::UAI::BindingGateway::Operation::IUeAgentOperationVerifier> Verifier;

		friend UEAGENTINTERFACE_API bool TryCreateWorldSpatialOperationChains(
			TSharedRef<IUeAgentWorldSpatialRequestDecoder>,
			TSharedRef<IUeAgentWorldSpatialReadbackPort>,
			TSharedRef<IUeAgentWorldSpatialMutationPort>,
			TSharedRef<IUeAgentWorldSpatialSettlePort>,
			TSharedRef<IUeAgentWorldSpatialResultComparator>,
			TArray<TSharedRef<FUeAgentWorldSpatialOperationChain>>&,
			FUeAgentWorldSpatialExecutionError&);
	};

	UEAGENTINTERFACE_API bool TryCreateWorldSpatialQueryProviderRegistry(
		TSharedRef<IUeAgentWorldSpatialRequestDecoder> Decoder,
		TSharedRef<IUeAgentWorldSpatialReadbackPort> Readback,
		TSharedPtr<const UE::UAI::BindingGateway::Query::FUeAgentDomainQueryProviderRegistry>& OutRegistry,
		FUeAgentWorldSpatialExecutionError& OutError);

	UEAGENTINTERFACE_API bool TryCreateWorldSpatialOperationChains(
		TSharedRef<IUeAgentWorldSpatialRequestDecoder> Decoder,
		TSharedRef<IUeAgentWorldSpatialReadbackPort> Readback,
		TSharedRef<IUeAgentWorldSpatialMutationPort> Mutation,
		TSharedRef<IUeAgentWorldSpatialSettlePort> Settle,
		TSharedRef<IUeAgentWorldSpatialResultComparator> Comparator,
		TArray<TSharedRef<FUeAgentWorldSpatialOperationChain>>& OutChains,
		FUeAgentWorldSpatialExecutionError& OutError);
}

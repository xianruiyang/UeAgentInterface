#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"
#include "Core/BindingGateway/Operation/UeAgentOperationBinding.h"
#include "Core/BindingGateway/Query/UeAgentDomainQuery.h"
#include "Core/Contract/UeAgentContract.h"
#include "Core/Contract/Command/UeAgentCommandContractModel.h"
#include "Core/PlanningPreflight/UeAgentPlanningPreflight.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "DomainAdapters/RigSimulationSpatial/UeAgentRigSimulationSpatialPort.h"
#include "Templates/SharedPointer.h"

#define UAI_RIG_SIMULATION_SPATIAL_EXECUTION_CHAINS_V1 1

namespace UE::UAI::DomainAdapters::RigSimulationSpatial
{
	class UEAGENTINTERFACE_API FUeAgentRigSimulationSpatialExecutionChainContract final
	{
	public:
		FUeAgentRigSimulationSpatialExecutionChainContract(
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
			bool bInReadsBackAfterMutation,
			TArray<UE::UAI::CommandContract::FUeAgentModelResultFieldDeclaration>
				InModelResultFields = {});

		const FString& GetCapabilityId() const;
		const FString& GetDescriptorId() const;
		const FString& GetOperationKind() const;
		const FString& GetC3Lane() const;
		const FString& GetFamilyId() const;
		const FString& GetBindingId() const;
		const FString& GetTypedOperationId() const;
		const FString& GetTemporalContractId() const;
		const FString& GetSpaceContractId() const;
		const FString& GetTypedPortId() const;
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
		const TArray<UE::UAI::CommandContract::FUeAgentModelResultFieldDeclaration>&
		GetModelResultFields() const;

	private:
		FString CapabilityId;
		FString DescriptorId;
		FString OperationKind;
		FString C3Lane;
		FString FamilyId;
		FString BindingId;
		FString TemporalContractId = TEXT("uai.temporal_contract.v1");
		FString SpaceContractId = TEXT("uai.space_contract.v1");
		FString TypedPortId = TEXT("uai.rig_simulation_spatial.typed_port.v1");
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
		TArray<UE::UAI::CommandContract::FUeAgentModelResultFieldDeclaration>
			ModelResultFields;
	};

	UEAGENTINTERFACE_API const TArray<FUeAgentRigSimulationSpatialExecutionChainContract>&
	GetRigSimulationSpatialExecutionChainContracts();

	using FUeAgentRigSimulationSpatialExecutionError = UE::UAI::Contract::FDiagnostic;

	class UEAGENTINTERFACE_API IUeAgentRigSimulationSpatialTypedRequest
	{
	public:
		virtual ~IUeAgentRigSimulationSpatialTypedRequest() = default;
		virtual const FString& GetTypeId() const = 0;
		virtual const FString& GetFamilyId() const = 0;
		virtual const FString& GetRequestKindId() const = 0;
		virtual const FUeAgentRigSimulationSpatialPortRequest&
		GetFormalPortRequest() const = 0;
		virtual bool TryBuildChangeDraft(
			const UE::UAI::StateSemantic::FUeAgentCanonicalSnapshot& Snapshot,
			const UE::UAI::ContextResource::FUeAgentOperationContext& Context,
			TOptional<UE::UAI::PlanningPreflight::FChangeDraft>& OutDraft,
			FUeAgentRigSimulationSpatialExecutionError& OutError) const = 0;
	};

	using FUeAgentRigSimulationSpatialTypedRequestPtr =
		TSharedPtr<const IUeAgentRigSimulationSpatialTypedRequest>;
	using FUeAgentRigSimulationSpatialTypedRequestRef =
		TSharedRef<const IUeAgentRigSimulationSpatialTypedRequest>;

	class UEAGENTINTERFACE_API IUeAgentRigSimulationSpatialRequestDecoder
	{
	public:
		virtual ~IUeAgentRigSimulationSpatialRequestDecoder() = default;
		virtual bool TryDecode(
			const FUeAgentRigSimulationSpatialExecutionChainContract& Contract,
			const UE::UAI::Contract::FUeAgentContractValue& CanonicalInput,
			UE::UAI::ContextResource::FUeAgentResourceTracker& Resources,
			FUeAgentRigSimulationSpatialTypedRequestPtr& OutRequest,
			FUeAgentRigSimulationSpatialExecutionError& OutError) = 0;
	};

	enum class EUeAgentRigSimulationSpatialReadPhase : uint8
	{
		Query,
		Preflight,
		Verification,
	};

	struct UEAGENTINTERFACE_API FUeAgentRigSimulationSpatialReadbackResult
	{
		FUeAgentRigSimulationSpatialReadbackResult(
			TSharedRef<const UE::UAI::StateSemantic::FUeAgentCanonicalSnapshot> InSnapshot,
			UE::UAI::Contract::FDiagnostic InDifference = {})
			: Snapshot(MoveTemp(InSnapshot)), Difference(MoveTemp(InDifference)) {}

		TSharedRef<const UE::UAI::StateSemantic::FUeAgentCanonicalSnapshot> Snapshot;
		// A successful observation can differ from the requested state without being a readback failure.
		UE::UAI::Contract::FDiagnostic Difference;
	};

	class UEAGENTINTERFACE_API IUeAgentRigSimulationSpatialReadbackPort
	{
	public:
		virtual ~IUeAgentRigSimulationSpatialReadbackPort() = default;
		virtual bool TryRead(
			const FUeAgentRigSimulationSpatialExecutionChainContract& Contract,
			const FUeAgentRigSimulationSpatialTypedRequestRef& Request,
			const TOptional<UE::UAI::Discovery::FUeAgentCandidateBinding>& Target,
			EUeAgentRigSimulationSpatialReadPhase Phase,
			const UE::UAI::ContextResource::FUeAgentOperationContext& Context,
			TOptional<FUeAgentRigSimulationSpatialReadbackResult>& OutReadback,
			FUeAgentRigSimulationSpatialExecutionError& OutError) = 0;
	};

	class UEAGENTINTERFACE_API FUeAgentRigSimulationSpatialMutationResult final
	{
	public:
		FUeAgentRigSimulationSpatialMutationResult() = default;
		FUeAgentRigSimulationSpatialMutationResult(
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

	class UEAGENTINTERFACE_API IUeAgentRigSimulationSpatialMutationPort
	{
	public:
		virtual ~IUeAgentRigSimulationSpatialMutationPort() = default;
		virtual bool TryMutate(
			const FUeAgentRigSimulationSpatialExecutionChainContract& Contract,
			const FUeAgentRigSimulationSpatialTypedRequestRef& Request,
			const TArray<UE::UAI::BindingGateway::Operation::FUeAgentOperationTargetBinding>& Targets,
			const UE::UAI::ContextResource::FUeAgentOperationContext& Context,
			FUeAgentRigSimulationSpatialMutationResult& OutResult,
			FUeAgentRigSimulationSpatialExecutionError& OutError) = 0;
	};

	class UEAGENTINTERFACE_API IUeAgentRigSimulationSpatialSettlePort
	{
	public:
		virtual ~IUeAgentRigSimulationSpatialSettlePort() = default;
		virtual bool TrySettle(
			const FUeAgentRigSimulationSpatialExecutionChainContract& Contract,
			const FUeAgentRigSimulationSpatialMutationResult& Mutation,
			const UE::UAI::ContextResource::FUeAgentOperationContext& Context,
			FUeAgentRigSimulationSpatialExecutionError& OutError) = 0;
	};

	class UEAGENTINTERFACE_API IUeAgentRigSimulationSpatialResultComparator
	{
	public:
		virtual ~IUeAgentRigSimulationSpatialResultComparator() = default;
		virtual bool TryCompare(
			const FUeAgentRigSimulationSpatialExecutionChainContract& Contract,
			const FUeAgentRigSimulationSpatialTypedRequestRef& Request,
			const UE::UAI::StateSemantic::FUeAgentCanonicalSnapshot& Before,
			const FUeAgentRigSimulationSpatialReadbackResult& After,
			const UE::UAI::ContextResource::FUeAgentOperationContext& Context,
			UE::UAI::Verification::FVerificationOutcome& OutOutcome,
			FUeAgentRigSimulationSpatialExecutionError& OutError) = 0;
	};

	class UEAGENTINTERFACE_API FUeAgentRigSimulationSpatialOperationChain final
	{
	public:
		class FState;

		const FUeAgentRigSimulationSpatialExecutionChainContract& GetContract() const;
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
		FUeAgentRigSimulationSpatialOperationChain(
			FUeAgentRigSimulationSpatialExecutionChainContract InContract,
			TSharedRef<IUeAgentRigSimulationSpatialRequestDecoder> InDecoder,
			TSharedRef<IUeAgentRigSimulationSpatialReadbackPort> InReadback,
			TSharedRef<FState, ESPMode::ThreadSafe> InState,
			TSharedRef<UE::UAI::BindingGateway::Operation::IUeAgentOperationPlanner> InPlanner,
			TSharedRef<UE::UAI::BindingGateway::Operation::IUeAgentOperationExecutor> InExecutor,
			TSharedRef<UE::UAI::BindingGateway::Operation::IUeAgentOperationVerifier> InVerifier);

		FUeAgentRigSimulationSpatialExecutionChainContract Contract;
		TSharedRef<IUeAgentRigSimulationSpatialRequestDecoder> Decoder;
		TSharedRef<IUeAgentRigSimulationSpatialReadbackPort> Readback;
		TSharedRef<FState, ESPMode::ThreadSafe> State;
		TSharedRef<UE::UAI::BindingGateway::Operation::IUeAgentOperationPlanner> Planner;
		TSharedRef<UE::UAI::BindingGateway::Operation::IUeAgentOperationExecutor> Executor;
		TSharedRef<UE::UAI::BindingGateway::Operation::IUeAgentOperationVerifier> Verifier;

		friend UEAGENTINTERFACE_API bool TryCreateRigSimulationSpatialOperationChains(
			TSharedRef<IUeAgentRigSimulationSpatialRequestDecoder>,
			TSharedRef<IUeAgentRigSimulationSpatialReadbackPort>,
			TSharedRef<IUeAgentRigSimulationSpatialMutationPort>,
			TSharedRef<IUeAgentRigSimulationSpatialSettlePort>,
			TSharedRef<IUeAgentRigSimulationSpatialResultComparator>,
			TArray<TSharedRef<FUeAgentRigSimulationSpatialOperationChain>>&,
			FUeAgentRigSimulationSpatialExecutionError&);
	};

	UEAGENTINTERFACE_API bool TryCreateRigSimulationSpatialQueryProviderRegistry(
		TSharedRef<IUeAgentRigSimulationSpatialRequestDecoder> Decoder,
		TSharedRef<IUeAgentRigSimulationSpatialReadbackPort> Readback,
		TSharedPtr<const UE::UAI::BindingGateway::Query::FUeAgentDomainQueryProviderRegistry>& OutRegistry,
		FUeAgentRigSimulationSpatialExecutionError& OutError);

	UEAGENTINTERFACE_API bool TryCreateRigSimulationSpatialOperationChains(
		TSharedRef<IUeAgentRigSimulationSpatialRequestDecoder> Decoder,
		TSharedRef<IUeAgentRigSimulationSpatialReadbackPort> Readback,
		TSharedRef<IUeAgentRigSimulationSpatialMutationPort> Mutation,
		TSharedRef<IUeAgentRigSimulationSpatialSettlePort> Settle,
		TSharedRef<IUeAgentRigSimulationSpatialResultComparator> Comparator,
		TArray<TSharedRef<FUeAgentRigSimulationSpatialOperationChain>>& OutChains,
		FUeAgentRigSimulationSpatialExecutionError& OutError);
}



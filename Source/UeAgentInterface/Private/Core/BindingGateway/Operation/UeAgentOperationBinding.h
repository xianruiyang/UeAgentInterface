#pragma once

#include "CoreMinimal.h"
#include "Core/BindingGateway/Ticket/UeAgentBindingTicket.h"
#include "Core/ContextResource/UeAgentContextResource.h"
#include "Core/ContextResource/UeAgentOperationContext.h"
#include "Core/Contract/UeAgentContract.h"
#include "Core/Contract/UeAgentDescriptorRegistry.h"
#include "Core/Discovery/UeAgentDiscovery.h"
#include "Core/ExecutionMutation/UeAgentExecutionMutation.h"
#include "Core/PlanningPreflight/UeAgentPlanningPreflight.h"
#include "Core/Projection/UeAgentProjection.h"
#include "Core/Verification/UeAgentVerification.h"

#define UAI_OPERATION_BINDING_EXPLICIT_CONTEXT_V1 1
#define UAI_OPERATION_BINDING_PREPARATION_V1 1

namespace UE::UAI::BindingGateway::Operation
{
	using UE::UAI::BindingGateway::Ticket::
		FUeAgentOperationSessionIdentity;
	using UE::UAI::BindingGateway::Ticket::FUeAgentOperationTicket;
	using UE::UAI::BindingGateway::Ticket::
		FUeAgentOperationTicketService;
	using UE::UAI::ContextResource::EUeAgentResourceDimension;
	using UE::UAI::ContextResource::EUeAgentRequestKind;
	using UE::UAI::ContextResource::FUeAgentCancellationToken;
	using UE::UAI::ContextResource::FUeAgentExecutionContextError;
	using UE::UAI::ContextResource::FUeAgentExecutionContextInput;
	using UE::UAI::ContextResource::FUeAgentExecutionContextRef;
	using UE::UAI::ContextResource::FUeAgentExecutionContextRefPtr;
	using UE::UAI::ContextResource::
		IUeAgentExecutionContextValidator;
	using UE::UAI::ContextResource::FUeAgentOperationContext;
	using UE::UAI::ContextResource::FUeAgentOperationContextError;
	using UE::UAI::ContextResource::FUeAgentOperationDeadline;
	using UE::UAI::ContextResource::FUeAgentResourceTracker;
	using UE::UAI::Contract::FUeAgentCallerContext;
	using UE::UAI::Contract::FUeAgentContractValue;
	using UE::UAI::Contract::FUeAgentDescriptorIdentity;
	using UE::UAI::Discovery::FUeAgentCandidateBinding;
	using UE::UAI::ExecutionMutation::FExecutionReceipt;
	using UE::UAI::PlanningPreflight::FPlanningRequest;
	using UE::UAI::PlanningPreflight::FPlanningResult;
	using UE::UAI::Projection::FUeAgentProjectedResponse;
	using UE::UAI::Projection::FUeAgentProjectionRequest;
	using UE::UAI::Verification::FVerificationOutcome;

	struct UEAGENTINTERFACE_API FUeAgentOperationBindingError
	{
		FString Code;
		FString SubjectId;
		FString Detail;
		EUeAgentResourceDimension ResourceDimension =
			EUeAgentResourceDimension::WorkUnits;
		int64 ResourceLimit = 0;
		int64 ResourceObserved = 0;

		bool IsValid() const
		{
			return !Code.IsEmpty();
		}
	};

	class UEAGENTINTERFACE_API FUeAgentOperationTargetBinding final
	{
	public:
		FUeAgentOperationTargetBinding(
			FUeAgentOperationTicket InTicket,
			FUeAgentCandidateBinding InBinding);

		const FUeAgentOperationTicket& GetTicket() const;
		const FUeAgentCandidateBinding& GetBinding() const;
		bool IsValid() const;

	private:
		FUeAgentOperationTicket Ticket;
		FUeAgentCandidateBinding Binding;
		bool bValid = false;
	};

	class UEAGENTINTERFACE_API IUeAgentOperationRequestPreparer
	{
	public:
		virtual ~IUeAgentOperationRequestPreparer() = default;

		virtual bool TryPrepare(
			const FUeAgentContractValue& CanonicalInput,
			const TArray<FUeAgentOperationTargetBinding>&
				RequestedTargets,
			const FUeAgentOperationContext& Context,
			TOptional<FPlanningRequest>& OutPlanning,
			FUeAgentOperationBindingError& OutError) = 0;
	};

	class UEAGENTINTERFACE_API FUeAgentOperationBindingPayload final
	{
	public:
		FUeAgentOperationBindingPayload(
			FUeAgentOperationSessionIdentity InSession,
			TArray<FUeAgentOperationTargetBinding> InTargets,
			FUeAgentExecutionContextInput InExecutionContext,
			FPlanningRequest InPlanning,
			FUeAgentProjectionRequest InProjection);

		FUeAgentOperationBindingPayload(
			FUeAgentOperationSessionIdentity InSession,
			TArray<FUeAgentOperationTargetBinding> InTargets,
			FUeAgentExecutionContextInput InExecutionContext,
			FUeAgentContractValue InInput,
			TSharedRef<IUeAgentOperationRequestPreparer>
				InPreparer,
			FUeAgentProjectionRequest InProjection);

		const FUeAgentOperationSessionIdentity& GetSession() const;
		const TArray<FUeAgentOperationTargetBinding>&
			GetTargets() const;
		const FUeAgentExecutionContextInput&
			GetExecutionContext() const;
		const FPlanningRequest& GetPlanning() const;
		bool RequiresPreparation() const;
		const FUeAgentContractValue& GetInput() const;
		const TSharedPtr<IUeAgentOperationRequestPreparer>&
			GetPreparer() const;
		const FUeAgentProjectionRequest& GetProjection() const;
		bool IsValid() const;

	private:
		FUeAgentOperationSessionIdentity Session;
		TArray<FUeAgentOperationTargetBinding> Targets;
		FUeAgentExecutionContextInput ExecutionContext;
		TOptional<FPlanningRequest> Planning;
		TOptional<FUeAgentContractValue> Input;
		TSharedPtr<IUeAgentOperationRequestPreparer> Preparer;
		FUeAgentProjectionRequest Projection;
		bool bValid = false;
	};

	using FUeAgentOperationBindingRequest =
		UE::UAI::Contract::TUeAgentOperationRequest<
			FUeAgentOperationBindingPayload>;

	class UEAGENTINTERFACE_API FUeAgentOperationBindingResponse final
	{
	public:
		FUeAgentOperationBindingResponse(
			FString InOperationId,
			FString InPlanFingerprint,
			TSharedRef<const FUeAgentProjectedResponse>
				InProjection);

		const FString& GetOperationId() const;
		const FString& GetPlanFingerprint() const;
		const TSharedPtr<const FUeAgentProjectedResponse>&
			GetProjection() const;
		bool IsValid() const;

	private:
		FString OperationId;
		FString PlanFingerprint;
		TSharedPtr<const FUeAgentProjectedResponse> Projection;
		bool bValid = false;
	};

	using FUeAgentOperationBindingResult =
		UE::UAI::Contract::TUeAgentOperationResult<
			FUeAgentOperationBindingResponse>;

	class UEAGENTINTERFACE_API IUeAgentOperationSessionSource
	{
	public:
		virtual ~IUeAgentOperationSessionSource() = default;
		virtual FUeAgentOperationSessionIdentity GetCurrent() const = 0;
	};

	class UEAGENTINTERFACE_API IUeAgentOperationPlanner
	{
	public:
		virtual ~IUeAgentOperationPlanner() = default;

		virtual bool TryPlan(
			const FPlanningRequest& Request,
			const FUeAgentOperationContext& Context,
			FPlanningResult& OutResult,
			FUeAgentOperationBindingError& OutError) = 0;
	};

	class UEAGENTINTERFACE_API IUeAgentOperationExecutor
	{
	public:
		virtual ~IUeAgentOperationExecutor() = default;

		virtual bool TryExecute(
			const FPlanningResult& Planning,
			const TArray<FUeAgentOperationTargetBinding>& Targets,
			const FUeAgentOperationContext& Context,
			FExecutionReceipt& OutReceipt,
			FUeAgentOperationBindingError& OutError) = 0;
	};

	class UEAGENTINTERFACE_API IUeAgentOperationVerifier
	{
	public:
		virtual ~IUeAgentOperationVerifier() = default;

		virtual bool TryVerify(
			const FExecutionReceipt& Receipt,
			const TArray<FUeAgentOperationTargetBinding>& Targets,
			const FUeAgentOperationContext& Context,
			FVerificationOutcome& OutOutcome,
			FUeAgentOperationBindingError& OutError) = 0;
	};

	class UEAGENTINTERFACE_API FUeAgentOperationBinding final
	{
	public:
		static bool TryCreate(
			TSharedRef<
				const UE::UAI::Contract::
					FUeAgentDescriptorRegistry>
				DescriptorRegistry,
			FUeAgentDescriptorIdentity DescriptorIdentity,
			TSharedRef<
				const IUeAgentExecutionContextValidator>
				ContextValidator,
			TSharedRef<IUeAgentOperationPlanner> Planner,
			TSharedRef<IUeAgentOperationExecutor> Executor,
			TSharedRef<IUeAgentOperationVerifier> Verifier,
			TSharedRef<
				const UE::UAI::Projection::
					FUeAgentResponseProjector> Projector,
			TSharedRef<IUeAgentOperationSessionSource>
				SessionSource,
			TSharedRef<const FUeAgentOperationTicketService>
				TicketService,
			TSharedPtr<const FUeAgentOperationBinding>& OutBinding,
			FUeAgentOperationBindingError& OutError);

		bool TrySubmit(
			const FUeAgentOperationBindingRequest& Request,
			const FUeAgentCallerContext& CallerContext,
			TSharedRef<
				FUeAgentResourceTracker,
				ESPMode::ThreadSafe> Resources,
			const FUeAgentOperationDeadline& Deadline,
			const FUeAgentCancellationToken& Cancellation,
			TSharedPtr<const FUeAgentOperationBindingResult>&
				OutResult,
			FUeAgentOperationBindingError& OutError) const;

	private:
		FUeAgentOperationBinding(
			TSharedRef<
				const UE::UAI::Contract::
					FUeAgentDescriptorRegistry>
				InDescriptorRegistry,
			FUeAgentDescriptorIdentity InDescriptorIdentity,
			TSharedRef<
				const IUeAgentExecutionContextValidator>
				InContextValidator,
			TSharedRef<IUeAgentOperationPlanner> InPlanner,
			TSharedRef<IUeAgentOperationExecutor> InExecutor,
			TSharedRef<IUeAgentOperationVerifier> InVerifier,
			TSharedRef<
				const UE::UAI::Projection::
					FUeAgentResponseProjector> InProjector,
			TSharedRef<IUeAgentOperationSessionSource>
				InSessionSource,
			TSharedRef<const FUeAgentOperationTicketService>
				InTicketService);

		TSharedRef<
			const UE::UAI::Contract::FUeAgentDescriptorRegistry>
			DescriptorRegistry;
		FUeAgentDescriptorIdentity DescriptorIdentity;
		TSharedRef<const IUeAgentExecutionContextValidator>
			ContextValidator;
		TSharedRef<IUeAgentOperationPlanner> Planner;
		TSharedRef<IUeAgentOperationExecutor> Executor;
		TSharedRef<IUeAgentOperationVerifier> Verifier;
		TSharedRef<
			const UE::UAI::Projection::FUeAgentResponseProjector>
			Projector;
		TSharedRef<IUeAgentOperationSessionSource> SessionSource;
		TSharedRef<const FUeAgentOperationTicketService>
			TicketService;
	};
}

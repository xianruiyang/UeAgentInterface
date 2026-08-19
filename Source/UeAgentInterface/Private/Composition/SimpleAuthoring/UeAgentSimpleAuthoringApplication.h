#pragma once

#include "Composition/SimpleAuthoring/UeAgentSimpleAuthoringComposition.h"
#include "Core/BindingGateway/Operation/UeAgentOperationBinding.h"
#include "Core/BindingGateway/Ticket/UeAgentBindingTicket.h"
#include "Core/ContextResource/UeAgentOperationContext.h"
#include "Core/Contract/UeAgentContract.h"
#include "Core/Contract/UeAgentDescriptorRegistry.h"
#include "Core/Discovery/UeAgentDiscovery.h"
#include "Core/PlanningPreflight/UeAgentPlanningPreflight.h"
#include "Core/Projection/UeAgentProjection.h"
#include "Templates/SharedPointer.h"

#define UAI_SIMPLE_AUTHORING_APPLICATION_V1 1

namespace UE::UAI::Composition::SimpleAuthoring
{
	struct UEAGENTINTERFACE_API
		FUeAgentSimpleAuthoringApplicationError
	{
		FString Code;
		FString SubjectId;
		FString Detail;

		bool IsValid() const
		{
			return !Code.IsEmpty();
		}
	};

	class UEAGENTINTERFACE_API
		FUeAgentSimpleAuthoringPreparedOperation final
	{
	public:
		FUeAgentSimpleAuthoringPreparedOperation(
			TArray<Discovery::FUeAgentCandidateBinding> InTargets,
			PlanningPreflight::FPlanningRequest InPlanning,
			Projection::FUeAgentProjectionRequest InProjection);

		const TArray<Discovery::FUeAgentCandidateBinding>&
			GetTargets() const;
		const PlanningPreflight::FPlanningRequest&
			GetPlanning() const;
		const Projection::FUeAgentProjectionRequest&
			GetProjection() const;
		bool IsValid() const;

	private:
		TArray<Discovery::FUeAgentCandidateBinding> Targets;
		PlanningPreflight::FPlanningRequest Planning;
		Projection::FUeAgentProjectionRequest Projection;
		bool bValid = false;
	};

	class UEAGENTINTERFACE_API
		IUeAgentSimpleAuthoringCapabilityProvider
	{
	public:
		virtual ~IUeAgentSimpleAuthoringCapabilityProvider() = default;

		virtual const FString& GetFamilyId() const = 0;
		virtual TSharedRef<
			BindingGateway::Operation::IUeAgentOperationPlanner>
			GetPlanner() = 0;
		virtual TSharedRef<
			BindingGateway::Operation::IUeAgentOperationExecutor>
			GetExecutor() = 0;
		virtual TSharedRef<
			BindingGateway::Operation::IUeAgentOperationVerifier>
			GetVerifier() = 0;

		virtual bool TryPrepare(
			const Contract::FUeAgentOperationRequest&
				CanonicalRequest,
			const ContextResource::FUeAgentExecutionContextRef&
				Context,
			const Contract::FUeAgentCallerContext& CallerContext,
			ContextResource::FUeAgentResourceTracker& Resources,
			const ContextResource::FUeAgentOperationDeadline&
				Deadline,
			const ContextResource::FUeAgentCancellationToken&
				Cancellation,
			TOptional<
				FUeAgentSimpleAuthoringPreparedOperation>&
				OutPrepared,
			FUeAgentSimpleAuthoringApplicationError& OutError) = 0;
	};

	struct UEAGENTINTERFACE_API
		FUeAgentSimpleAuthoringApplicationConfig
	{
		TSharedPtr<const Contract::FUeAgentDescriptorRegistry>
			DescriptorRegistry;
		ContextResource::FUeAgentExecutionContextInput
			ExecutionContext;
		FString ServerSessionId;
		int64 RegistryEpoch = 0;
		double TimeoutSeconds = 0.0;
		int64 MaxFrameBytes = 0;
	};

	class UEAGENTINTERFACE_API
		FUeAgentSimpleAuthoringApplication final
		: public Contract::IUeAgentOperationApplicationPort
	{
	public:
		~FUeAgentSimpleAuthoringApplication() override;

		static bool TryCreate(
			const FUeAgentSimpleAuthoringApplicationConfig& Config,
			TArray<FUeAgentSimpleAuthoringCommandRoute> Routes,
			TArray<TSharedRef<
				IUeAgentSimpleAuthoringCapabilityProvider>>
				Providers,
			TSharedRef<const
				BindingGateway::Ticket::
					FUeAgentOperationTicketService> TicketService,
			TSharedRef<const
				Projection::FUeAgentResponseProjector> Projector,
			TSharedPtr<FUeAgentSimpleAuthoringApplication>&
				OutApplication,
			FUeAgentSimpleAuthoringApplicationError& OutError);

		Contract::FUeAgentOperationResult Submit(
			const Contract::FUeAgentOperationRequest& Request,
			const Contract::FUeAgentCallerContext& CallerContext,
			const TSharedRef<
				Contract::IUeAgentCancellationSignal>&
				Cancellation) override;

		int32 GetBindingCount() const;

	private:
		class FState;

		explicit FUeAgentSimpleAuthoringApplication(
			TSharedRef<FState, ESPMode::ThreadSafe> InState);

		Contract::FUeAgentOperationResult SubmitOnGameThread(
			const Contract::FUeAgentOperationRequest& Request,
			const Contract::FUeAgentCallerContext& CallerContext,
			const TSharedRef<
				Contract::IUeAgentCancellationSignal>&
				Cancellation);

		TSharedRef<FState, ESPMode::ThreadSafe> State;
	};
}


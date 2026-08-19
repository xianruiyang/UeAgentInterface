#pragma once

#include "Composition/HjsonImport/UeAgentHjsonImportTargetRegistry.h"
#include "Core/BindingGateway/Operation/UeAgentOperationBinding.h"
#include "Core/BindingGateway/Ticket/UeAgentBindingTicket.h"
#include "Core/Projection/UeAgentProjection.h"

namespace UE::UAI::Composition::HjsonImport
{
	struct UEAGENTINTERFACE_API FUeAgentHjsonImportOperationRuntimeConfig
	{
		FString ProjectInstanceId;
		FString EditorInstanceId;
		FString ServerSessionId;
		int64 RegistryEpoch = -1;

		bool IsValid() const;
	};

	struct UEAGENTINTERFACE_API FUeAgentHjsonImportC3OResult
	{
		FUeAgentHjsonTargetImportReceipt ImportReceipt;
		FString OperationId;
		FString PlanFingerprint;
		FString VerificationStatus;

		bool IsValid() const;
	};

	class UEAGENTINTERFACE_API FUeAgentHjsonImportOperationRuntime final
	{
	public:
		class FState;

		static bool TryCreate(
			const FUeAgentHjsonImportOperationRuntimeConfig& Config,
			TSharedRef<const BindingGateway::Ticket::
				FUeAgentOperationTicketService> TicketService,
			TSharedRef<const Projection::FUeAgentResponseProjector> Projector,
			TSharedPtr<FUeAgentHjsonImportOperationRuntime>& OutRuntime,
			FUeAgentHjsonImportTargetError& OutError);

		bool TrySubmit(
			FUeAgentHjsonTargetImportRequest Request,
			const FString& RequestId,
			const Contract::FUeAgentCallerContext& CallerContext,
			TSharedRef<ContextResource::FUeAgentResourceTracker,
				ESPMode::ThreadSafe> Resources,
			const ContextResource::FUeAgentOperationDeadline& Deadline,
			const ContextResource::FUeAgentCancellationToken& Cancellation,
			FUeAgentHjsonImportC3OResult& OutResult,
			FUeAgentHjsonImportTargetError& OutError) const;

		const TSharedRef<const Contract::FUeAgentDescriptorRegistry>&
			GetDescriptorRegistry() const;

	private:
		explicit FUeAgentHjsonImportOperationRuntime(
			TSharedRef<FState, ESPMode::ThreadSafe> InState);

		TSharedRef<FState, ESPMode::ThreadSafe> State;
	};
}

#pragma once

#include "Composition/LongRunningSession/UeAgentLongRunningSessionActivation.h"
#include "CommandEdge/UeAgentCommandDispatch.h"
#include "Core/Contract/UeAgentContract.h"
#include "Core/DomainCapability/UeAgentDomainCapability.h"

namespace UE::UAI::OperationOwners::CaptureMedia
{
	struct UEAGENTINTERFACE_API FUeAgentCaptureMediaRoute final
	{
		FString CommandId;
		FString PythonFunction;
		Contract::FUeAgentDescriptorIdentity DescriptorIdentity;
		bool IsValid() const;
	};

	UEAGENTINTERFACE_API const TArray<FUeAgentCaptureMediaRoute>& GetRoutes();
	UEAGENTINTERFACE_API bool TryGetProductionOperationPort(
		TSharedPtr<Contract::IUeAgentOperationApplicationPort>& OutPort,
		FString& OutError);
	UEAGENTINTERFACE_API TSharedRef<const DomainCapability::
		IDomainOperationInvocationFactory> FindOperationInvocationFactory(
			const FString& CommandId);
	UEAGENTINTERFACE_API CommandEdge::FUeAgentCommandResult Submit(
		const CommandEdge::FUeAgentCommandRequest& Request,
		const Contract::FUeAgentCallerContext& Caller,
		const TSharedRef<Contract::IUeAgentCancellationSignal>& Cancellation);
}

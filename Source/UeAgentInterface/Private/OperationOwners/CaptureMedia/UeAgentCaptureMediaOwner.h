#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

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

	UEAGENTINTERFACE_API bool TryGetRoutes(
		const TArray<FUeAgentCaptureMediaRoute>*& OutRoutes,
		UE::UAI::Contract::FDiagnostic& OutError);
	UEAGENTINTERFACE_API bool TryGetProductionOperationPort(
		TSharedPtr<Contract::IUeAgentOperationApplicationPort>& OutPort,
		UE::UAI::Contract::FDiagnostic& OutError);
	UEAGENTINTERFACE_API bool TryFindOperationInvocationFactory(
		const FString& CommandId,
		TSharedPtr<const DomainCapability::IDomainOperationInvocationFactory>& OutFactory,
		UE::UAI::Contract::FDiagnostic& OutError);
	UEAGENTINTERFACE_API CommandEdge::FUeAgentCommandResult Submit(
		const CommandEdge::FUeAgentCommandRequest& Request,
		const Contract::FUeAgentCallerContext& Caller,
		const TSharedRef<Contract::IUeAgentCancellationSignal>& Cancellation);
}

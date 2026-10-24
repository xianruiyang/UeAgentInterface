#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CommandEdge/UeAgentCommandDispatch.h"
#include "Composition/GraphCompound/UeAgentGraphCompoundAudioMediaBindings.h"
#include "CoreMinimal.h"
#include "Core/DomainCapability/UeAgentDomainCapability.h"
#include "Utilities/AuthoringOperationApply/UeAgentAuthoringOperationCoordinator.h"

namespace UE::UAI::OperationOwners::TextureGraph
{
	UEAGENTINTERFACE_API bool TryApplySemanticTextureGraphComposite(
		const Composition::GraphCompound::
			FTextureGraphSemanticCompositeRequest& Request,
		const PlanningPreflight::FPlanningPreflightPlanRef& Plan,
		const TSharedRef<Contract::IUeAgentCancellationSignal>& Cancellation,
		UeAgentAuthoringOperationApply::FApplyWorkspaceResult& OutResult,
		UE::UAI::Contract::FDiagnostic& OutError,
		const UeAgentAuthoringOperationApply::FBackendOptions& Options = {});

	UEAGENTINTERFACE_API TSharedPtr<const DomainCapability::
		IDomainOperationInvocationFactory>
	FindTextureGraphOperationInvocationFactory(const FString& OperationId);

	UEAGENTINTERFACE_API bool TryCreateMechanicsHandlers(
		TSharedRef<const DomainCapability::IDomainOperationInvocationFactory>
			BakeFactory,
		TMap<FString, TSharedRef<CommandEdge::IUeAgentCommandHandler>>& OutHandlers,
		UE::UAI::Contract::FDiagnostic& OutError);

	UEAGENTINTERFACE_API CommandEdge::FUeAgentCommandResult Submit(
		const TSharedRef<CommandEdge::IUeAgentCommandHandler>& Mechanics,
		const CommandEdge::FUeAgentCommandRequest& Request,
		const Contract::FUeAgentCallerContext& Caller,
		const TSharedRef<Contract::IUeAgentCancellationSignal>& Cancellation);
}

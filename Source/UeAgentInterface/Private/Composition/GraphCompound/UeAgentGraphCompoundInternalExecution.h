#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CommandEdge/UeAgentCommandDispatch.h"
#include "Composition/GraphCompound/UeAgentGraphCompoundActivation.h"

namespace UE::UAI::Composition::GraphCompound
{
	/**
	 * Executes an already-created GraphCompound mechanics handler through the
	 * same production contract and request codec used by the public ingress.
	 * The registry, envelope and binding are machine-side details only.
	 */
	UEAGENTINTERFACE_API bool TryExecuteGraphCompoundInternal(
		const TArray<FUeAgentGraphCompoundActivationProfile>& Profiles,
		const TMap<FString, TSharedRef<CommandEdge::IUeAgentCommandHandler>>& Mechanics,
		const FString& CommandId,
		const FString& ParamsJson,
		const Contract::FUeAgentCallerContext& Caller,
		const FString& ErrorPrefix,
		FString& OutPayloadJson,
		UE::UAI::Contract::FDiagnostic& OutError);
}

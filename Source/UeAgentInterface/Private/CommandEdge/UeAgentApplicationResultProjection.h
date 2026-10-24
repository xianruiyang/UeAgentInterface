#pragma once

#include "CommandEdge/UeAgentCommandProtocol.h"

namespace UE::UAI::CommandEdge
{
	// Only existing wire-contract and execution differences belong here.
	// Validation and diagnostic propagation always use the same implementation.
	struct FApplicationResultProjectionPolicy
	{
		// Empty keeps the source diagnostic code; mismatches use the common code.
		FString InvalidResultCode;
		FString InvalidPayloadCode;
		EApplicationFailureKind BoundaryFailureKind = EApplicationFailureKind::Operation;
		EApplicationFailureKind ApplicationFailureKind = EApplicationFailureKind::Operation;
		bool bValidateObjectPayload = true;
		bool bInvalidResultMayHaveMutated = false;
	};

	FUeAgentCommandResult ProjectOperationResult(
		const FUeAgentCommandRequest& Request,
		const Contract::FUeAgentDescriptorIdentity& ExpectedDescriptor,
		const Contract::FUeAgentOperationResult& Result,
		const FApplicationResultProjectionPolicy& Policy = {});

	FUeAgentCommandResult ProjectQueryResult(
		const FUeAgentCommandRequest& Request,
		const Contract::FUeAgentDescriptorIdentity& ExpectedDescriptor,
		const Contract::FUeAgentQueryResult& Result);
}

#pragma once

#include "CoreMinimal.h"
#include "Core/ExecutionMutation/UeAgentExecutionMutation.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"

class UObject;

namespace UE::UAI::DomainAdapters::ModelingGeometryEffect
{
	UEAGENTINTERFACE_API bool IsModelingGeometryCarrier(
		const UObject& Object);

	UEAGENTINTERFACE_API bool TryReadModelingGeometryCarrierRevision(
		const FString& SourceId,
		UObject& Carrier,
		TOptional<StateSemantic::FUeAgentStateRevision>& OutRevision,
		ExecutionMutation::FExecutionMutationError& OutError);
}

#pragma once

#include "CoreMinimal.h"
#include "DomainAdapters/RigSimulationSpatial/UeAgentRigSimulationSpatialPhysicsNative.h"

namespace UE::UAI::DomainAdapters::RigSimulationSpatial
{
	UEAGENTINTERFACE_API bool TryCreateProductionPhysicsSessionPort(
		TSharedPtr<IUeAgentRigSimulationSpatialPhysicsSessionPort>& OutPort,
		FUeAgentRigSimulationSpatialExecutionError& OutError);
}

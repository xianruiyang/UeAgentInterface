#pragma once

#include "CoreMinimal.h"
#include "Templates/SharedPointer.h"

#define UAI_RIG_SIMULATION_SPATIAL_EXACT_DESCRIPTORS_V1 1

namespace UE::UAI::Contract
{
	class FUeAgentDescriptorRegistry;
}

namespace UE::UAI::DomainAdapters::RigSimulationSpatial
{
	/** Builds the immutable exact C0 descriptor registry for all 18 RigSimulationSpatial capabilities. */
	UEAGENTINTERFACE_API bool TryCreateRigSimulationSpatialDescriptorRegistry(
		TSharedPtr<const UE::UAI::Contract::FUeAgentDescriptorRegistry>&
			OutRegistry,
		FString& OutError);
}



#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"
#include "Templates/SharedPointer.h"

#define UAI_WORLD_SPATIAL_EXACT_DESCRIPTORS_V1 1

namespace UE::UAI::Contract
{
	class FUeAgentDescriptorRegistry;
}

namespace UE::UAI::DomainAdapters::WorldSpatial
{
	/** Builds the immutable exact C0 descriptor registry for all 22 WorldSpatial capabilities. */
	UEAGENTINTERFACE_API bool TryCreateWorldSpatialDescriptorRegistry(
		TSharedPtr<const UE::UAI::Contract::FUeAgentDescriptorRegistry>&
			OutRegistry,
		UE::UAI::Contract::FDiagnostic& OutError);
}

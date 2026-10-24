#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"
#include "Templates/SharedPointer.h"

#define UAI_TIMELINE_ANIMATION_EXACT_DESCRIPTORS_V1 1

namespace UE::UAI::Contract
{
	class FUeAgentDescriptorRegistry;
}

namespace UE::UAI::DomainAdapters::TimelineAnimation
{
	/** Builds the immutable exact C0 descriptor registry for all 20 TimelineAnimation capabilities. */
	UEAGENTINTERFACE_API bool TryCreateTimelineAnimationDescriptorRegistry(
		TSharedPtr<const UE::UAI::Contract::FUeAgentDescriptorRegistry>&
			OutRegistry,
		UE::UAI::Contract::FDiagnostic& OutError);
}

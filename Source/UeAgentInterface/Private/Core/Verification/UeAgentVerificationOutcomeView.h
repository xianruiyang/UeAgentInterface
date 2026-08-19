#pragma once

#include "CoreMinimal.h"
#include "Core/Verification/UeAgentVerification.h"

namespace UE::UAI::Verification
{
	UEAGENTINTERFACE_API bool TryCanonicalizeVerificationOutcome(
		const FVerificationOutcome& Outcome,
		FUeAgentResourceTracker& Resources,
		UE::UAI::StateSemantic::FUeAgentCanonicalValuePtr& OutView,
		FVerificationError& OutError);
}

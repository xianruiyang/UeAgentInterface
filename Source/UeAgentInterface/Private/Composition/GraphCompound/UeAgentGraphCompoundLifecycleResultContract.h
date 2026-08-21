#pragma once

#include "CoreMinimal.h"

namespace UE::UAI::Composition::GraphCompound::LifecycleResultContract
{
	FString BuildMutationLifecycleResultSchema(
		bool bRequireIndependentReadbackEvidence);
}

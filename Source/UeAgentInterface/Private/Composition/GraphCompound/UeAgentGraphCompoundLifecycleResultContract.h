#pragma once

#include "CoreMinimal.h"

namespace UE::UAI::Contract { struct FDiagnostic; }

namespace UE::UAI::Composition::GraphCompound::LifecycleResultContract
{
	FString BuildMutationLifecycleResultSchema(
		bool bRequireIndependentReadbackEvidence, Contract::FDiagnostic& OutError);
}

#pragma once

#include "CoreMinimal.h"

namespace UE::UAI::QueryOwners::Reference
{
	UEAGENTINTERFACE_API const TArray<FString>& GetStateLayers();
	UEAGENTINTERFACE_API const TArray<FString>& GetIndependentReadbackFlows();
}

#pragma once

#include "CoreMinimal.h"

namespace UE::UAI::QueryOwners::Landscape
{
	UEAGENTINTERFACE_API const FString& GetPersistedStateLayer();
	UEAGENTINTERFACE_API const TArray<FString>& GetIndependentReadbackFlows();
}

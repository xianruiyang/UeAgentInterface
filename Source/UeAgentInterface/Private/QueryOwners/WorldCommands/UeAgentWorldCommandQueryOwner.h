#pragma once

#include "CoreMinimal.h"

namespace UE::UAI::QueryOwners::WorldCommands
{
	UEAGENTINTERFACE_API const TArray<FString>& GetCommandIds();
	UEAGENTINTERFACE_API bool IsCommand(const FString& CommandId);
	UEAGENTINTERFACE_API const TArray<FString>& GetIndependentReadbackFlows();
}

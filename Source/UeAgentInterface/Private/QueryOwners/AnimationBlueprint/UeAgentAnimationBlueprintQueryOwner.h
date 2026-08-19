#pragma once

#include "CoreMinimal.h"

namespace UE::UAI::QueryOwners::AnimationBlueprint
{
	UEAGENTINTERFACE_API bool IsReadOnlyContentGraphCommand(
		const FString& CommandId);
}

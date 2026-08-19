#pragma once

#include "CoreMinimal.h"

namespace UE::UAI::QueryOwners::Sequence
{
	UEAGENTINTERFACE_API bool IsReadOnlyContentGraphCommand(
		const FString& CommandId);
}

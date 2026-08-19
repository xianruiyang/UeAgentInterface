#pragma once

#include "CoreMinimal.h"

class FJsonObject;
namespace UeAgentAuthoringFieldPolicy
{
	UEAGENTINTERFACE_API void NormalizeAuthoringObjectTree(const TSharedPtr<FJsonObject>& Object);
}

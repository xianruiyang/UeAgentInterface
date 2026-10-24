#pragma once

#include "CoreMinimal.h"

class UK2Node;

namespace UeAgentBlueprintInputBindings
{
	// Captures native input identity using stable public semantic fields.
	// Used for both template candidate selection and actual-node observation.
	void Capture(const UK2Node& Node, TMap<FString, FString>& OutFields);
}

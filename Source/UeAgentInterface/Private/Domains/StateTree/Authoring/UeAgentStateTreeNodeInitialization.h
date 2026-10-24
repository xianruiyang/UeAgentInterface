#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"

struct FStateTreeEditorNode;

namespace UeAgentStateTreeAuthoring
{
	// Initializes the native node and its instance from a resolved struct or Blueprint class.
	bool InitializeNode(FStateTreeEditorNode& Node, UObject& Outer, const UStruct* Type, UE::UAI::Contract::FDiagnostic& Error);
}

#pragma once

#include "CoreMinimal.h"

class USmartObjectDefinition;

namespace UE::UAI::QueryOwners::AiBehavior
{
	/** Selectable DefinitionData leaves in the root or slot's native data picker. */
	void VisitSmartObjectDataMenu(const USmartObjectDefinition& Definition, bool bSlot,
		TFunctionRef<void(UScriptStruct&)> Visit);

	/** Resolve a known name without loading every derived Blueprint. Selection is rechecked on the actual element. */
	void VisitSmartObjectBehaviorTypes(USmartObjectDefinition& Definition, bool bSlot,
		TFunctionRef<void(const FString&, const FTopLevelAssetPath&)> Visit);
	bool CanAssignSmartObjectBehavior(USmartObjectDefinition& Definition, int32 Slot, int32 Index, const UClass& Class);
}

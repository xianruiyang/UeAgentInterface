#pragma once

#include "CoreMinimal.h"

class UStateTreeEditorData;
class UScriptStruct;

namespace UE::UAI::QueryOwners::AiBehavior
{
	// An unset owner enumerates the union of actual Details binding targets.
	bool VisitStateTreeFunctionMenu(UStateTreeEditorData& Data, FGuid Owner, const FString& TargetPath,
		TFunctionRef<void(const UScriptStruct&, const FString&)> Visit, FString& Failure);
}

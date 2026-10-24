#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleInterface.h"

class UBlueprint;
class UFunction;

// Loaded only after the host has loaded ModelViewViewModelEditor. This module
// owns the optional native callback's read-only rename effect inspection.
class IUeAgentInterfaceMVVMModule : public IModuleInterface
{
public:
	virtual bool HasExternalRenameEffect(const UBlueprint& Source, const UBlueprint& Dependent,
		FName OldName, FName NewName) const = 0;
	virtual bool HasFunctionReference(const UBlueprint& Dependent,
		TFunctionRef<bool(const UFunction*)> MatchesTarget) const = 0;
};

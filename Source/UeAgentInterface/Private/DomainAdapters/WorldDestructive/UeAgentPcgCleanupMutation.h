#pragma once

#include "DomainAdapters/WorldDestructive/UeAgentWorldDestructiveOwner.h"

class UPCGComponent;

namespace UE::UAI::DomainAdapters::WorldDestructive
{
bool TryCreatePcgCleanupMutation(UPCGComponent& Component,
    TSharedPtr<IUeAgentWorldDestructiveDurableMutation>& OutMutation,
    Contract::FDiagnostic& OutError);
}

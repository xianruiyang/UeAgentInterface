#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"
#include "DomainAdapters/WorldDestructive/UeAgentWorldDestructiveOwner.h"
#include "Templates/SharedPointer.h"

namespace UE::UAI::OperationOwners::DestructiveContent
{
	UEAGENTINTERFACE_API bool TryCreateWorldDestructiveDurableRunner(
		FString JournalRoot,
		FString StableJournalOwnerId,
		TSharedPtr<
			DomainAdapters::WorldDestructive::
				IUeAgentWorldDestructiveDurableRunner>& OutRunner,
		UE::UAI::Contract::FDiagnostic& OutError);
}

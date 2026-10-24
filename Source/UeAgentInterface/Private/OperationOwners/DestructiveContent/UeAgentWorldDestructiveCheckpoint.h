#pragma once

#include "DomainAdapters/WorldDestructive/UeAgentWorldDestructiveOwner.h"
#include "Core/PlanningPreflight/UeAgentPlanningPreflight.h"

namespace UE::UAI::OperationOwners::DestructiveContent
{
DomainAdapters::WorldDestructive::FUeAgentWorldDestructiveResult ExecuteCheckpointedWorldMutation(
    UWorld& World, const DomainAdapters::WorldDestructive::FUeAgentWorldDestructiveRequest& Request,
    const TSharedRef<DomainAdapters::WorldDestructive::IUeAgentWorldDestructiveDurableMutation>& Mutation,
    const PlanningPreflight::FPlanningPreflightPlanRef& Plan, const FString& JournalRoot,
    const TSharedRef<Contract::IUeAgentCancellationSignal>& Cancellation);
}

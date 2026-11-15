#pragma once

#include "Core/HjsonWorkspace/UeAgentSemanticLayout.h"

class FJsonObject;
class FJsonValue;

namespace UE::UAI::HjsonWorkspace
{
    struct FCompositionUnitSet;
    struct FCommittedUnitChange
    {
        FString UnitId;
        TOptional<FSemanticUnit> Before, After;
    };
    // Original delta owner's pure typed output. Persistence still uses the
    // established delta/1 row grammar; a live consumer need not decode it back.
    struct FCommittedWorkspaceDelta
    {
        FString ContractId, ProfileId, AfterRevision;
        int32 ContractVersion = 0;
        TArray<FString> BeforeTargets, AfterTargets;
        TArray<FCommittedUnitChange> Changes;
        // Selected actual observations, including a setter's unchanged result.
        // These are not changes and never participate in cumulative folding.
        TMap<FString, FSemanticUnit> AcceptedActualUnits;
    };
    /** Exact observed transaction difference, independent of an author's edit
     * list. Persistence contains changed units only, never native bindings. */
    bool TryBuildCommittedWorkspaceDelta(const FAuthoringModel& Before,
        const FAuthoringModel& After, ContextResource::FUeAgentResourceTracker& Resources,
        TSharedPtr<FJsonObject>& OutDelta, FWorkspaceError& Error,
        TSharedPtr<const FCommittedWorkspaceDelta>* OutTyped = nullptr);
    bool TryBuildCommittedWorkspaceDelta(const FCompositionUnitSet& Before,
        const FCompositionUnitSet& After, ContextResource::FUeAgentResourceTracker& Resources,
        TSharedPtr<FJsonObject>& OutDelta, FWorkspaceError& Error,
        TSharedPtr<const FCommittedWorkspaceDelta>* OutTyped = nullptr);

    /** Complete native observations proved stable membership and identified all
     * changed scalar properties. These two models contain only their actual
     * owner/definition closure. Unset requires the full semantic fallback. */
    TOptional<bool> TryBuildCommittedPropertyDelta(const FAuthoringModel& Baseline,
        const FAuthoringModel& Before, const FAuthoringModel& After,
        const TArray<FString>& ChangedUnits, const FString& ActualAfterRevision,
        ContextResource::FUeAgentResourceTracker& Resources,
        TSharedPtr<FJsonObject>& OutDelta, FWorkspaceError& Error,
        TSharedPtr<const FCommittedWorkspaceDelta>* OutTyped = nullptr);

    /** Fold selected ordered actual records relative to this destination's
     * publication. Output is earliest published before/final after, without
     * intermediate Models or owning an old generation. */
    bool TryFoldCommittedWorkspaceDeltas(const FAuthoringModel& Published,
        TConstArrayView<TSharedPtr<const FCommittedWorkspaceDelta>> Records,
        ContextResource::FUeAgentResourceTracker& Resources, FCommittedWorkspaceDelta& Out,
        FWorkspaceError& Error);
    bool TryReadCommittedWorkspaceDelta(const FJsonObject& Record,
        ContextResource::FUeAgentResourceTracker& Resources, FCommittedWorkspaceDelta& Out,
        FWorkspaceError& Error);
    // The same lossless row codec used by actual deltas and accepted intent.
    bool TryEncodeCommittedWorkspaceUnit(const FSemanticUnit& Unit,
        ContextResource::FUeAgentResourceTracker& Resources, TSharedPtr<FJsonValue>& Out,
        FWorkspaceError& Error);
    bool TryDecodeCommittedWorkspaceUnit(const FJsonValue& Value,
        ContextResource::FUeAgentResourceTracker& Resources, TOptional<FSemanticUnit>& Out,
        FWorkspaceError& Error);
}

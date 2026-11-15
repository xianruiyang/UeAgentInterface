#pragma once

#include "Core/HjsonWorkspace/UeAgentSemanticLayout.h"

namespace UE::UAI::HjsonWorkspace
{
    /** The shared declaration meaning: native property/document field paths
     * are not semantic dependency identities. No directory lookup is needed. */
    bool HasSemanticFieldSetReference(ESemanticUnitKind Kind, const FString& SemanticType, const FString& FieldSet);

    /** Semantic declaration fields shared by import and committed deltas.
     * Presentation/provenance policy belongs to the consuming projection. */
    bool AreSemanticUnitDeclarationsEquivalent(const FSemanticUnit& A, const FSemanticUnit& B,
        bool bCompareItems = true);
    /** Semantic values from the admitted typed model. Preserves exact value
     * kinds, number precision, string/key case, array order and absence.
     * Does not compare source bytes, presentation or publication identities. */
    bool TryAreSemanticUnitValuesEquivalent(const FSemanticUnit& A, const FSemanticUnit& B,
        FUeAgentResourceTracker& Resources, bool& OutEquivalent, FWorkspaceError& Error);

    /** The semantic row contract (the fields written by SemanticUnitJson),
     * using typed value equality rather than its serialized content hash.
     * Callers with presentation/provenance requirements retain those checks.
     * A complete current native model may additionally supply its declared
     * property types for the existing native-value comparison (for example,
     * old float32 spelling versus its shortest round-trip output). Without
     * that model, decimal values retain exact source-semantic comparison. */
    bool TryAreSemanticUnitsEquivalent(const FSemanticUnit& A, const FSemanticUnit& B,
        FUeAgentResourceTracker& Resources, bool& OutEquivalent, FWorkspaceError& Error,
        const FAuthoringModel* DeclaredModel = nullptr);
}

#include "Core/HjsonWorkspace/UeAgentSemanticUnitComparison.h"
#include "Core/HjsonWorkspace/UeAgentAuthoringComposition.h"

namespace UE::UAI::HjsonWorkspace
{
    bool HasSemanticFieldSetReference(ESemanticUnitKind Kind, const FString& SemanticType, const FString& FieldSet)
    {
        return !FieldSet.IsEmpty() && !(Kind == ESemanticUnitKind::PropertySet
            && (SemanticType.EndsWith(TEXT(".property")) || SemanticType.Contains(TEXT("#"))
                || SemanticType == TEXT("uai.authoring.surface.target_settings")));
    }

    bool AreSemanticUnitDeclarationsEquivalent(const FSemanticUnit& A, const FSemanticUnit& B, bool bCompareItems)
    {
        const auto Text = [](const FString& X, const FString& Y) { return X.Equals(Y, ESearchCase::CaseSensitive); };
        const auto Refs = [&](const TArray<FString>& X, const TArray<FString>& Y) {
            if (X.Num() != Y.Num()) return false;
            for (int32 I = 0; I < X.Num(); ++I) if (!Text(X[I], Y[I])) return false;
            return true;
        };
        return A.GetUnitKind() == B.GetUnitKind()
            && Text(A.GetUnitId(), B.GetUnitId()) && Text(A.GetOwnerRef(), B.GetOwnerRef())
            && Text(A.GetSemanticTypeRef(), B.GetSemanticTypeRef()) && Text(A.GetRole(), B.GetRole())
            && Text(A.GetFieldSetRef(), B.GetFieldSetRef()) && Text(A.GetOrderingRef(), B.GetOrderingRef())
            && Text(A.GetLayoutRef(), B.GetLayoutRef()) && Text(A.GetTargetIdentityKey(), B.GetTargetIdentityKey())
            && (!bCompareItems || Refs(A.GetItemRefs(), B.GetItemRefs())) && Refs(A.GetRelationRefs(), B.GetRelationRefs())
            && Refs(A.GetDependencyRefs(), B.GetDependencyRefs()) && Refs(A.GetProfileVisibility(), B.GetProfileVisibility());
    }

    bool TryAreSemanticUnitValuesEquivalent(const FSemanticUnit& A, const FSemanticUnit& B,
        FUeAgentResourceTracker& Resources, bool& OutEquivalent, FWorkspaceError& Error)
    {
        OutEquivalent = A.SharesStorageWith(B);
        if (OutEquivalent) return true;
        if (A.GetValueCount() != B.GetValueCount()) return true;
        bool bEquivalent = true, bSucceeded = true;
        A.VisitValues([&](const FString& Key, const auto& Value) {
            const FString* MatchedKey = nullptr;
            const auto* Other = B.FindValue(Key, &MatchedKey);
            if (!Other || !Key.Equals(*MatchedKey, ESearchCase::CaseSensitive)) { bEquivalent = false; return false; }
            if (&Value.Get() == &Other->Get()) return true;
            StateSemantic::FUeAgentSemanticComparison Comparison;
            StateSemantic::FUeAgentStateSemanticError SemanticError;
            if (!StateSemantic::TryCompareValues(Value, *Other,
                TEXT("/values/") + EscapeJsonPointerSegment(Key), Resources, Comparison, SemanticError)) {
                Error.SetFailure(MoveTemp(SemanticError), A.GetUnitId()); bSucceeded = bEquivalent = false; return false;
            }
            if (!Comparison.IsEquivalent()) { bEquivalent = false; return false; }
            return true;
        });
        OutEquivalent = bEquivalent;
        return bSucceeded;
    }

    bool TryAreSemanticUnitsEquivalent(const FSemanticUnit& A, const FSemanticUnit& B,
        FUeAgentResourceTracker& Resources, bool& OutEquivalent, FWorkspaceError& Error,
        const FAuthoringModel* DeclaredModel)
    {
        OutEquivalent = A.SharesStorageWith(B);
        if (OutEquivalent) return true;
        if (!AreSemanticUnitDeclarationsEquivalent(A, B)) return true;
        if (!TryAreSemanticUnitValuesEquivalent(A, B, Resources, OutEquivalent, Error)) return false;
        if (OutEquivalent || !DeclaredModel || A.GetUnitKind() != ESemanticUnitKind::PropertySet) return true;
        // A current native-state proof can use its complete declared model.
        // Reuse the existing typed native comparison; never round source/SHA or
        // infer numeric meaning from strings without that declaration.
        if (A.GetValueCount() != B.GetValueCount()) return true;
        if (!A.VisitValues([&](const FString& Key, const auto&) {
            const FString* MatchedKey = nullptr;
            return B.FindValue(Key, &MatchedKey) && Key.Equals(*MatchedKey, ESearchCase::CaseSensitive);
        })) return true;
        TSet<FString> ChangedFields;
        if (!TryCollectChangedUnitValueFields(&A, B, Resources, ChangedFields, Error, DeclaredModel)) return false;
        OutEquivalent = ChangedFields.IsEmpty();
        return true;
    }
}

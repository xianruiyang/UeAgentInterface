// Copyright Epic Games, Inc. All Rights Reserved.
#include "Domains/Blueprint/Authoring/UeAgentBlueprintNativeNormalization.h"
#include "Core/Contract/UeAgentDiagnostic.h"

#include "Domains/Blueprint/Authoring/UeAgentBlueprintAssetLifecycle.h"
#include "Utilities/AuthoringCore/UeAgentNativeNormalizationIntent.h"
#include "Utilities/AuthoringObjectApply/UeAgentAuthoringObjectApply.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "Engine/Blueprint.h"
#include "K2Node_MathExpression.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectGlobals.h"

namespace UeAgentBlueprintAuthoring
{
bool IsNativeGeneratedGraph(const UEdGraph& Graph)
{
    const auto* Expression = Cast<UK2Node_MathExpression>(Graph.GetOuter());
    return Expression && Expression->BoundGraph == &Graph;
}

bool NormalizeNativeNodeProperties(UObject& Object,
    TArray<TSharedPtr<FJsonValue>>& Descriptors,
    const UeAgentAuthoringObjectApply::FOptions& Options,
    UeAgentAuthoringObjectApply::FFailure& OutFailure)
{
    using namespace UeAgentAuthoringObjectApply;
    const auto* Node = Cast<UK2Node_MathExpression>(&Object);
    if (!Node || !Descriptors.ContainsByPredicate([](const auto& Value) {
        FString Path;
        return Value && Value->Type == EJson::Object
            && Value->AsObject()->TryGetStringField(TEXT("property_path"), Path)
            && Path == GET_MEMBER_NAME_STRING_CHECKED(UK2Node_MathExpression, Expression);
    })) return true;

    // A node-only duplicate still points at the real Blueprint's symbols/graph.
    // Reuse the lifecycle clone: preserve member/node identities while isolating
    // native graph generation, reconstruction and MarkBlueprintAsModified.
    UBlueprint* Source = FBlueprintEditorUtils::FindBlueprintForNode(Node);
    TStrongObjectPtr<UBlueprint> Preview(Source
        ? UeAgentBlueprintAssetLifecycle::DuplicateBlueprint(Source, GetTransientPackage(),
            MakeUniqueObjectName(GetTransientPackage(), Source->GetClass(), TEXT("NativeNodePreview")), true, &OutFailure.Diagnostic)
        : nullptr);
    if (!Preview.IsValid()) {
        if (!OutFailure.Diagnostic.IsEmpty()) return false;
        OutFailure.Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(TEXT("blueprint_native_normalization_context_missing"),
            !Source ? TEXT("The math-expression node has no owning Blueprint.") : TEXT("The editor could not duplicate the owning Blueprint for native expression normalization."),
            TEXT("native_normalization"));
        OutFailure.Diagnostic.SourceKind = TEXT("native_result");
        OutFailure.Diagnostic.TargetPath = Source ? UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(*Source) : Object.GetPathName();
        OutFailure.Diagnostic.PropertyPath = TEXT("Expression");
        OutFailure.Diagnostic.ActualValuePresent = true;
        OutFailure.Diagnostic.ActualValue = MakeShared<FJsonValueBoolean>(false);
        OutFailure.Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(!Source ? TEXT("owning Blueprint") : TEXT("isolated Blueprint preview"));
        return false;
    }
    Preview->SetFlags(RF_Transient);
    auto* PreviewNode = FindObject<UK2Node_MathExpression>(Preview.Get(), *Node->GetPathName(Source));
    if (!PreviewNode || PreviewNode->NodeGuid != Node->NodeGuid) {
        OutFailure.Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(TEXT("blueprint_native_normalization_node_missing"),
            !PreviewNode ? TEXT("The duplicated Blueprint has no matching math-expression node.") : TEXT("The duplicated math-expression node has a different native identity."),
            TEXT("native_normalization"));
        OutFailure.Diagnostic.SourceKind = TEXT("native_result");
        OutFailure.Diagnostic.TargetPath = UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(*Source);
        OutFailure.Diagnostic.PropertyPath = TEXT("Expression");
        OutFailure.Diagnostic.ActualValuePresent = true;
        OutFailure.Diagnostic.ActualValue = MakeShared<FJsonValueString>(!PreviewNode ? TEXT("missing node") : TEXT("different node identity"));
        OutFailure.Diagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("preview node preserving the source node identity"));
        return false;
    }

    auto PreviewOptions = Options;
    PreviewOptions.bMarkPackageDirty = false;
    PreviewOptions.bRecordOwnerTransaction = false;
    PreviewOptions.bVerifyNativeStages = false;
    FPreparedApply Prepared;
    FApplyResult Result;
    UeAgentNativeNormalizationIntent::FTargetScope NormalizationTarget(
        UeAgentBlueprintAssetLifecycle::GetAuthoringTargetPath(*Source)
        + TEXT(":") + Node->GetPathName(Source));
    if (!PrepareObject(PreviewNode, Descriptors, PreviewOptions, Prepared, OutFailure)) return false;
    if (!Prepared.ApplyNativeNormalizationPreview(Result)) {
        OutFailure.Diagnostic = Result.Diagnostic;
        return false;
    }
    // Expected values come from the original request on this isolated native
    // context, never from the object being verified. The common codec preserves
    // descriptor provenance/types and rejects unsupported ownership changes.
    return Prepared.CaptureNativeNormalizedDescriptors(Descriptors, OutFailure);
}
}

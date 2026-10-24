#include "Domains/Deformer/Authoring/UeAgentMlDeformerAuthoring.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "MLDeformerAsset.h"
#include "MLDeformerModel.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Utilities/AuthoringCore/UeAgentDetailsAuthoringService.h"
#include "Utilities/AuthoringObjectApply/UeAgentAuthoringObjectApply.h"

namespace UeAgentMlDeformerAuthoring
{
namespace
{
UMLDeformerModel* GetModel(UObject& Object, UE::UAI::Contract::FDiagnostic& Error)
{
    const auto* Asset = Cast<UMLDeformerAsset>(&Object);
    UMLDeformerModel* Model = Asset ? Asset->GetModel() : nullptr;
    if (!Model || Model->GetOuter() != Asset)
    {
        Error = UE::UAI::Contract::FDiagnostic::Failure(TEXT("ml_deformer_owned_model_unavailable"),
            !Asset ? TEXT("The model owner is not an ML Deformer asset.")
                : !Model ? TEXT("The ML Deformer asset has no owned model.")
                : TEXT("The ML Deformer model is not owned by the requested asset."), TEXT("authoring"));
        Error.TargetPath = Object.GetPathName();
        Error.SourceKind = TEXT("native_result");
        if (!Asset)
        {
            Error.ActualValue = MakeShared<FJsonValueString>(Object.GetClass()->GetPathName());
            Error.ExpectedValue = MakeShared<FJsonValueString>(UMLDeformerAsset::StaticClass()->GetPathName());
        }
        else if (!Model)
        {
            Error.ActualValue = MakeShared<FJsonValueNull>();
            Error.ExpectedValue = MakeShared<FJsonValueString>(UMLDeformerModel::StaticClass()->GetPathName());
        }
        else
        {
            Error.PropertyPath = TEXT("model_owner");
            Error.ExpectedEvidence = MakeShared<FJsonValueString>(Object.GetPathName());
            if (const UObject* Outer = Model->GetOuter()) Error.ActualEvidence = MakeShared<FJsonValueString>(Outer->GetPathName());
            else Error.ActualEvidence = MakeShared<FJsonValueNull>();
            Error.ActualValue = MakeShared<FJsonValueBoolean>(false);
            Error.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
        }
        return nullptr;
    }
    return Model;
}

bool Capture(UMLDeformerModel& Model, UeAgentAuthoringState::FStateSnapshot& Snapshot,
    TSharedPtr<FJsonObject>& Properties, UE::UAI::Contract::FDiagnostic& Error)
{
    return UeAgentDetailsAuthoringService::CaptureObjectProperties(&Model, {}, Snapshot, Properties, Error)
        && UeAgentDetailsAuthoringService::ProjectReadableProperties(Snapshot, Properties, Error);
}
}

bool Read(UObject& Asset, TSharedPtr<FJsonValue>& Value, UE::UAI::Contract::FDiagnostic& Error)
{
    UMLDeformerModel* Model = GetModel(Asset, Error);
    if (!Model) return false;
    UeAgentAuthoringState::FStateSnapshot Snapshot;
    TSharedPtr<FJsonObject> Properties;
    if (!Capture(*Model, Snapshot, Properties, Error)) return false;
    Value = MakeShared<FJsonValueObject>(Properties);
    return true;
}

bool Apply(UObject& Asset, const TSharedPtr<FJsonValue>& Value, UE::UAI::Contract::FDiagnostic& Error)
{
    UMLDeformerModel* Model = GetModel(Asset, Error);
    if (!Model) return false;
    UeAgentAuthoringState::FStateSnapshot Snapshot;
    TSharedPtr<FJsonObject> Properties;
    TArray<TSharedPtr<FJsonValue>> Descriptors;
    if (!Capture(*Model, Snapshot, Properties, Error)
        || !UeAgentDetailsAuthoringService::DecodeReadableProperties(Snapshot, Value, Descriptors, Error)) return false;
    UeAgentAuthoringObjectApply::FOptions Options;
    Options.bCompactValues = true;
    // The Reflection domain owns both the asset and its preview-owned Model.
    Options.bAllowDomainAuthorizedOwners = true;
    UeAgentAuthoringObjectApply::FPreparedApply Prepared;
    UeAgentAuthoringObjectApply::FFailure Failure;
    if (!UeAgentAuthoringObjectApply::PrepareObject(Model, Descriptors, Options, Prepared, Failure))
    { Error = Failure.Diagnostic; return false; }
    UeAgentAuthoringObjectApply::FApplyResult Applied;
    if (!UeAgentAuthoringObjectApply::ApplyPrepared(Prepared, Applied))
    { Error = Applied.Diagnostic; return false; }
    // Native Serialize rebuilds VertexMap after serializing properties. Settle
    // mesh-derived data first so harvesting and saving see the same values.
    Model->UpdateCachedNumVertices();
    Model->InitVertexMap();
    UeAgentAuthoringObjectApply::ObservePrepared(Prepared);
    return true;
}
}

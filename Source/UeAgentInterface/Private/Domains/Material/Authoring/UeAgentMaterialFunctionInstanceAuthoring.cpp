#include "Domains/Material/Authoring/UeAgentMaterialFunctionInstanceAuthoring.h"

#include "Domains/Material/Authoring/UeAgentMaterialInstanceAuthoring.h"
#include "Dom/JsonValue.h"
#include "MaterialEditingLibrary.h"
#include "Materials/MaterialFunctionInstance.h"
#include "Materials/MaterialInstanceConstant.h"

namespace UeAgentMaterialAuthoring::FunctionInstance
{
bool ValidateParentChain(UMaterialFunctionInstance& Asset, UE::UAI::Contract::FDiagnostic& Error,
    const UMaterialFunctionInstance* Destination)
{
    // GetPreviewMaterial recursively visits Parent without a recursion guard.
    TSet<const UMaterialFunctionInstance*> Seen;
    for (const auto* Current = &Asset; Current; Current = Cast<UMaterialFunctionInstance>(Current->Parent))
    {
        if (Current == Destination) Current = &Asset;
        if (Seen.Contains(Current)) {
            Error = UE::UAI::Contract::FDiagnostic::Failure(TEXT("material_function_parent_cycle"),
                FString::Printf(TEXT("Material function instance '%s' has a parent cycle returning to '%s'."),
                    *Asset.GetName(), *Current->GetName()), TEXT("graph_authoring"));
            Error.TargetPath = Asset.GetPathName();
            Error.PropertyPath = TEXT("Parent");
            Error.ActualValue = MakeShared<FJsonValueString>(Current->GetName());
            Error.ExpectedValue = MakeShared<FJsonValueString>(TEXT("acyclic parent chain"));
            return false;
        }
        Seen.Add(Current);
    }
    return true;
}

namespace
{
UMaterialInstanceConstant* ParameterOwner(UMaterialFunctionInstance& Asset, UE::UAI::Contract::FDiagnostic& Error)
{
    if (!ValidateParentChain(Asset, Error)) return nullptr;
    auto* Preview = Cast<UMaterialInstanceConstant>(Asset.GetPreviewMaterial());
    if (!Preview) {
        Error = UE::UAI::Contract::FDiagnostic::Failure(TEXT("material_function_instance_parameter_owner_missing"),
            FString::Printf(TEXT("Material function instance '%s' has no material instance preview for reading parameters."), *Asset.GetName()),
            TEXT("graph_authoring"));
        Error.TargetPath = Asset.GetPathName();
        Error.PropertyPath = TEXT("preview_material");
        Error.ActualValuePresent = false;
        Error.ExpectedValue = MakeShared<FJsonValueString>(TEXT("MaterialInstanceConstant"));
        Error.SourceKind = TEXT("native_result");
    }
    return Preview;
}
}

bool Read(UMaterialFunctionInstance& Asset, TSharedPtr<FJsonValue>& Value, UE::UAI::Contract::FDiagnostic& Error,
    TArray<TSharedPtr<FJsonValue>>* Definitions)
{
    auto* Preview = ParameterOwner(Asset, Error);
    TArray<TSharedPtr<FJsonValue>> Parameters;
    if (!Preview || !CaptureInstanceParameters(*Preview, Parameters, Error, Definitions)) return false;
    Value = MakeShared<FJsonValueArray>(Parameters);
    return true;
}

bool Update(UMaterialFunctionInstance& Asset, UE::UAI::Contract::FDiagnostic& Error)
{
    if (!ValidateParentChain(Asset, Error)) return false;
    if (Asset.Parent) Asset.SetParent(Asset.Parent);
    else Asset.Base = nullptr;
    Asset.UpdateParameterSet();
    UMaterialEditingLibrary::UpdateMaterialFunction(&Asset);
    return true;
}

bool VerifyParameters(UMaterialFunctionInstance& Asset, const TArray<FInstanceParameterEdit>& Intent, UE::UAI::Contract::FDiagnostic& Error)
{
    return VerifyInstanceParameters(Asset, Intent, Error);
}

bool Apply(UMaterialFunctionInstance& Asset, const TSharedPtr<FJsonValue>& Value, UE::UAI::Contract::FDiagnostic& Error,
    TArray<FInstanceParameterEdit>* OutIntent)
{
    if (OutIntent) OutIntent->Reset();
    if (!Value || Value->Type != EJson::Array) {
        Error = UE::UAI::Contract::FDiagnostic::Failure(TEXT("material_function_instance_parameters_array_required"),
            TEXT("Material function instance parameters must be an array."), TEXT("graph_authoring"));
        Error.TargetPath = Asset.GetPathName();
        Error.PropertyPath = TEXT("instance_parameters");
        Error.ActualValue = Value;
        Error.ActualValuePresent = Value.IsValid();
        Error.ExpectedValue = MakeShared<FJsonValueString>(TEXT("array"));
        return false;
    }
    auto* Preview = ParameterOwner(Asset, Error);
    TArray<FInstanceParameterEdit> Edits;
    if (!Preview || !PrepareInstanceParameters(*Preview, Value->AsArray(), Edits, Error)
        || !ApplyInstanceParameters(*Preview, Edits, Error)) return false;
    Asset.Modify();
    // UE uses these exact carriers to expose a Function Instance in its material
    // instance editor. Reuse the same parameter decoder and native updates, then
    // publish the resulting local overrides back to the persistent function.
    Asset.ScalarParameterValues = Preview->ScalarParameterValues;
    Asset.VectorParameterValues = Preview->VectorParameterValues;
    Asset.DoubleVectorParameterValues = Preview->DoubleVectorParameterValues;
    Asset.TextureParameterValues = Preview->TextureParameterValues;
    Asset.TextureCollectionParameterValues = Preview->TextureCollectionParameterValues;
    Asset.RuntimeVirtualTextureParameterValues = Preview->RuntimeVirtualTextureParameterValues;
    Asset.SparseVolumeTextureParameterValues = Preview->SparseVolumeTextureParameterValues;
    Asset.FontParameterValues = Preview->FontParameterValues;
    const auto Static = Preview->GetStaticParameters();
    Asset.StaticSwitchParameterValues = Static.StaticSwitchParameters;
    Asset.StaticComponentMaskParameterValues = Static.EditorOnly.StaticComponentMaskParameters;
    if (!Update(Asset, Error) || !VerifyParameters(Asset, Edits, Error)) return false;
    if (OutIntent) *OutIntent = MoveTemp(Edits);
    return true;
}
}

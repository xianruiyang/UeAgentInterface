// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/Material/Authoring/UeAgentMaterialGraphAuthoring.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Domains/Material/Authoring/UeAgentMaterialExpressionResolver.h"
#include "MaterialEditingLibrary.h"
#include "MaterialShared.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionComment.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialFunctionInterface.h"
#include "UObject/StrongObjectPtr.h"
#include "Utilities/AuthoringObjectApply/UeAgentAuthoringObjectApply.h"
#include "Utilities/AuthoringOperationApply/UeAgentAuthoringObjectApplyProvider.h"

namespace UeAgentMaterialAuthoring {
namespace {
using namespace UeAgentAuthoringOperationApply;

struct FRootPropertySpec {
  const TCHAR *Name;
  EMaterialProperty Property;
};

const TArray<FRootPropertySpec> &RootPropertySpecs() {
  static const TArray<FRootPropertySpec> Specs = {
      {TEXT("BaseColor"), MP_BaseColor},
      {TEXT("EmissiveColor"), MP_EmissiveColor},
      {TEXT("Opacity"), MP_Opacity},
      {TEXT("OpacityMask"), MP_OpacityMask},
      {TEXT("Metallic"), MP_Metallic},
      {TEXT("Specular"), MP_Specular},
      {TEXT("Roughness"), MP_Roughness},
      {TEXT("Normal"), MP_Normal},
      {TEXT("Tangent"), MP_Tangent},
      {TEXT("WorldPositionOffset"), MP_WorldPositionOffset},
      {TEXT("SubsurfaceColor"), MP_SubsurfaceColor},
      {TEXT("AmbientOcclusion"), MP_AmbientOcclusion},
      {TEXT("Refraction"), MP_Refraction},
      {TEXT("PixelDepthOffset"), MP_PixelDepthOffset},
      {TEXT("Anisotropy"), MP_Anisotropy},
      {TEXT("FrontMaterial"), MP_FrontMaterial},
  };
  return Specs;
}

TConstArrayView<TObjectPtr<UMaterialExpression>>
GetExpressions(const UObject &Owner) {
  if (const UMaterial *Material = Cast<UMaterial>(&Owner))
    return Material->GetExpressions();
  if (const UMaterialFunction *Function = Cast<UMaterialFunction>(&Owner))
    return Function->GetExpressions();
  return {};
}

FMaterialExpressionCollection *GetCollectionInternal(UObject &Owner) {
  if (UMaterial *Material = Cast<UMaterial>(&Owner))
    return &Material->GetExpressionCollection();
  if (UMaterialFunction *Function = Cast<UMaterialFunction>(&Owner))
    return &Function->GetExpressionCollection();
  return nullptr;
}

const FMaterialExpressionCollection *
GetCollectionInternal(const UObject &Owner) {
  if (const UMaterial *Material = Cast<UMaterial>(&Owner))
    return &Material->GetExpressionCollection();
  if (const UMaterialFunction *Function = Cast<UMaterialFunction>(&Owner))
    return &Function->GetExpressionCollection();
  return nullptr;
}

UMaterialExpression *CreateExpressionInternal(UObject &Owner,
                                              UClass *ExpressionClass,
                                              const int32 X, const int32 Y) {
  if (UMaterial *Material = Cast<UMaterial>(&Owner)) {
    return UMaterialEditingLibrary::CreateMaterialExpression(
        Material, ExpressionClass, X, Y);
  }
  if (UMaterialFunction *Function = Cast<UMaterialFunction>(&Owner)) {
    return UMaterialEditingLibrary::CreateMaterialExpressionInFunction(
        Function, ExpressionClass, X, Y);
  }
  return nullptr;
}

FString GetInputDisplayName(UMaterialExpression &Expression,
                            const int32 InputIndex,
                            const FExpressionInput &Input) {
  FString Name;
  if (const UMaterialExpressionMaterialFunctionCall *FunctionCall =
          Cast<UMaterialExpressionMaterialFunctionCall>(&Expression)) {
    Name = FunctionCall->GetInputNameWithType(InputIndex, false).ToString();
  } else {
    Name = Expression.GetInputName(InputIndex).ToString();
  }
  return Name.IsEmpty() ? Input.InputName.ToString() : Name;
}

bool MatchesInputName(UMaterialExpression &Expression, const int32 InputIndex,
                      const FExpressionInput &Input,
                      const FString &Normalized) {
  TArray<FString, TInlineAllocator<3>> Candidates;
  Candidates.Add(GetInputDisplayName(Expression, InputIndex, Input));
  Candidates.Add(Input.InputName.ToString());
  if (!Expression.IsA<UMaterialExpressionMaterialFunctionCall>()) {
    Candidates.Add(
        Expression.UMaterialExpression::GetInputName(InputIndex).ToString());
  }
  for (const FString &Candidate : Candidates) {
    if (NormalizeExpressionPinName(Candidate).Equals(Normalized,
                                                     ESearchCase::IgnoreCase)) {
      return true;
    }
  }
  return false;
}

bool ResolveOutputIndexInternal(UMaterialExpression &Expression,
                                const FString &OutputName, int32 &OutIndex) {
  OutIndex = INDEX_NONE;
  const TArray<FExpressionOutput> &Outputs = Expression.GetOutputs();
  if (Outputs.IsEmpty())
    return false;
  const FString Normalized = NormalizeExpressionPinName(OutputName);
  if (Normalized.IsEmpty()) {
    OutIndex = 0;
    return true;
  }
  for (int32 Index = 0; Index < Outputs.Num(); ++Index) {
    const FString Candidate = Outputs[Index].OutputName.IsNone()
                                  ? FString()
                                  : Outputs[Index].OutputName.ToString();
    if (Candidate.Equals(Normalized, ESearchCase::IgnoreCase)) {
      OutIndex = Index;
      return true;
    }
  }
  return false;
}

bool ResolveInputInternal(UMaterialExpression &Expression,
                          const FString &InputName,
                          FExpressionInput *&OutInput) {
  OutInput = nullptr;
  const FString Normalized = NormalizeExpressionPinName(InputName);
  FExpressionInput *OnlyInput = nullptr;
  int32 Count = 0;
  for (FExpressionInputIterator It{&Expression}; It; ++It) {
    OnlyInput = Count == 0 ? It.Input : OnlyInput;
    ++Count;
    if (!Normalized.IsEmpty() &&
        MatchesInputName(Expression, It.Index, *It.Input, Normalized)) {
      OutInput = It.Input;
      return true;
    }
  }
  if (Normalized.IsEmpty() && Count == 1) {
    OutInput = OnlyInput;
    return true;
  }
  return false;
}

FString OutputNameForIndex(UMaterialExpression &Expression,
                           const int32 OutputIndex) {
  const TArray<FExpressionOutput> &Outputs = Expression.GetOutputs();
  if (!Outputs.IsValidIndex(OutputIndex) ||
      Outputs[OutputIndex].OutputName.IsNone())
    return FString();
  return Outputs[OutputIndex].OutputName.ToString();
}

bool TryGetObject(const TSharedPtr<FJsonObject> &Object, const TCHAR *Field,
                  TSharedPtr<FJsonObject> &OutObject) {
  const TSharedPtr<FJsonObject> *Found = nullptr;
  if (!Object.IsValid() || !Object->TryGetObjectField(Field, Found) || !Found ||
      !Found->IsValid()) {
    return false;
  }
  OutObject = *Found;
  return true;
}

bool ReadCollectionValues(const TSharedPtr<const FJsonObject> &Document,
                          const TCHAR *Field, const TCHAR *IdentityField,
                          TArray<TSharedPtr<FJsonValue>> &OutValues,
                          bool &bOutPresent, FString &OutError) {
  OutValues.Reset();
  bOutPresent = false;
  const TArray<TSharedPtr<FJsonValue>> *Array = nullptr;
  if (Document.IsValid() && Document->TryGetArrayField(Field, Array) && Array) {
    OutValues = *Array;
    bOutPresent = true;
    return true;
  }

  const TSharedPtr<FJsonObject> *Map = nullptr;
  if (!Document.IsValid() || !Document->TryGetObjectField(Field, Map) || !Map ||
      !Map->IsValid()) {
    return true;
  }
  bOutPresent = true;
  TArray<FString> Keys;
  (*Map)->Values.GenerateKeyArray(Keys);
  Keys.Sort();
  for (const FString &Key : Keys) {
    const TSharedPtr<FJsonValue> Source = (*Map)->TryGetField(Key);
    const TSharedPtr<FJsonObject> SourceObject =
        Source.IsValid() && Source->Type == EJson::Object ? Source->AsObject()
                                                          : nullptr;
    if (!SourceObject.IsValid()) {
      OutError = FString::Printf(TEXT("material_graph_%s_map_value_invalid:%s"),
                                 Field, *Key);
      return false;
    }
    TSharedRef<FJsonObject> Copy = MakeShared<FJsonObject>();
    for (const TPair<FString, TSharedPtr<FJsonValue>> &Pair :
         SourceObject->Values) {
      Copy->SetField(Pair.Key,
                     UeAgentAuthoringState::CloneCanonicalValue(Pair.Value));
    }
    if (IdentityField && *IdentityField && !Copy->HasField(IdentityField)) {
      Copy->SetStringField(IdentityField, Key);
    }
    OutValues.Add(MakeShared<FJsonValueObject>(Copy));
  }
  return true;
}

bool ReadRequiredString(const TSharedPtr<FJsonObject> &Object,
                        const TCHAR *Field, FString &OutValue,
                        FString &OutError) {
  OutValue.Reset();
  if (!Object.IsValid() || !Object->TryGetStringField(Field, OutValue)) {
    OutError = FString(TEXT("material_graph_missing_")) + Field;
    return false;
  }
  OutValue.TrimStartAndEndInline();
  if (OutValue.IsEmpty()) {
    OutError = FString(TEXT("material_graph_empty_")) + Field;
    return false;
  }
  return true;
}

FString NormalizeId(const FString &Id) {
  FString Result = Id.TrimStartAndEnd();
  Result.ToLowerInline();
  return Result;
}

void RetireCollection(const FMaterialExpressionCollection &Collection) {
  for (UMaterialExpression *Expression : Collection.Expressions) {
    if (Expression)
      Expression->MarkAsGarbage();
  }
  for (UMaterialExpressionComment *Comment : Collection.EditorComments) {
    if (Comment)
      Comment->MarkAsGarbage();
  }
}

void RefreshOwnerInternal(UObject &Owner) {
  Owner.Modify();
  if (UMaterial *Material = Cast<UMaterial>(&Owner)) {
    Material->BuildEditorParameterList();
    Material->UpdateCachedExpressionData();
  } else if (UMaterialFunction *Function = Cast<UMaterialFunction>(&Owner)) {
    Function->UpdateFromFunctionResource();
  }
  Owner.MarkPackageDirty();
  Owner.PostEditChange();
}

struct FExpressionSpec {
  FString Id;
  FString NormalizedId;
  FGuid Guid;
  UClass *ExpressionClass = nullptr;
  UMaterialFunctionInterface *MaterialFunction = nullptr;
  int32 X = 0;
  int32 Y = 0;
  TArray<TSharedPtr<FJsonValue>> Properties;
  TArray<UeAgentAuthoringObjectApply::FResolvedNode> ResolvedProperties;
  TArray<FName> CustomInputs;
};

struct FEdgeSpec {
  FString FromId;
  FString OutputName;
  FString ToId;
  FString InputName;
};

struct FRootInputSpec {
  EMaterialProperty Property = MP_MAX;
  FString FromId;
  FString OutputName;
};

struct FRootInputBackup {
  EMaterialProperty Property = MP_MAX;
  FExpressionInput Input;
};

bool PrepareProperties(
    UMaterialExpression &Expression,
    const TArray<TSharedPtr<FJsonValue>> &Properties,
    TUniquePtr<UeAgentAuthoringObjectApply::FPreparedApply> &OutPrepared,
    TArray<UeAgentAuthoringObjectApply::FResolvedNode> *OutResolved,
    FString &OutError) {
  OutPrepared.Reset();
  if (Properties.IsEmpty()) {
    if (OutResolved)
      OutResolved->Reset();
    return true;
  }
  OutPrepared = MakeUnique<UeAgentAuthoringObjectApply::FPreparedApply>();
  TArray<TSharedPtr<FJsonValue>> ProjectedProperties;
  ProjectedProperties.Reserve(Properties.Num());
  for (const TSharedPtr<FJsonValue> &PropertyValue : Properties) {
    const TSharedPtr<FJsonObject> Descriptor =
        PropertyValue.IsValid() && PropertyValue->Type == EJson::Object
            ? PropertyValue->AsObject()
            : nullptr;
    const TSharedPtr<FJsonObject> Projected =
        UeAgentAuthoringObjectApply::ProjectDescriptorForApply(Descriptor);
    ProjectedProperties.Add(Projected.IsValid()
                                ? MakeShared<FJsonValueObject>(Projected)
                                : PropertyValue);
  }
  UeAgentAuthoringObjectApply::FFailure PrepareFailure;
  UeAgentAuthoringObjectApply::FOptions Options;
  Options.bAllowDomainAuthorizedOwners = true;
  if (!UeAgentAuthoringObjectApply::PrepareObject(
          &Expression, ProjectedProperties, Options, *OutPrepared,
          PrepareFailure)) {
    const FString Detail = !PrepareFailure.ErrorDetail.IsEmpty()
                               ? PrepareFailure.ErrorDetail
                               : PrepareFailure.PropertyPath;
    OutError = PrepareFailure.ErrorCode.IsEmpty()
                   ? TEXT("material_expression_properties_prepare_failed")
                   : PrepareFailure.ErrorCode +
                         (Detail.IsEmpty() ? FString() : TEXT(":") + Detail);
    return false;
  }
  if (OutResolved)
    *OutResolved = OutPrepared->GetResolvedNodes();
  UeAgentAuthoringObjectApply::FApplyResult ApplyResult;
  if (!UeAgentAuthoringObjectApply::ApplyPrepared(*OutPrepared, ApplyResult) ||
      !UeAgentAuthoringObjectApply::VerifyPrepared(*OutPrepared, ApplyResult)) {
    const FString Detail = !ApplyResult.ErrorDetail.IsEmpty()
                               ? ApplyResult.ErrorDetail
                               : ApplyResult.PropertyPath;
    OutError = ApplyResult.ErrorCode.IsEmpty()
                   ? TEXT("material_expression_properties_apply_failed")
                   : ApplyResult.ErrorCode +
                         (Detail.IsEmpty() ? FString() : TEXT(":") + Detail);
    return false;
  }
  return true;
}
} // namespace

struct FPreparedGraph::FImpl {
  TArray<FExpressionSpec> Expressions;
  TArray<FEdgeSpec> Edges;
  TArray<FRootInputSpec> RootInputs;
  TArray<TUniquePtr<UeAgentAuthoringObjectApply::FPreparedApply>>
      AppliedPropertyPlans;
  FMaterialExpressionCollection OldCollection;
  TArray<TStrongObjectPtr<UObject>> OldObjects;
  TArray<FRootInputBackup> OldRootInputs;
  bool bReady = false;
  bool bRollbackCaptured = false;

  void ResetRollback() {
    OldCollection = FMaterialExpressionCollection();
    OldObjects.Reset();
    OldRootInputs.Reset();
    bRollbackCaptured = false;
  }

  bool ReplaceGraph(UObject &Owner, const bool bValidation, FString &OutError) {
    OutError.Reset();
    FMaterialExpressionCollection *Collection = GetCollectionInternal(Owner);
    if (!Collection) {
      OutError = TEXT("material_graph_owner_type_invalid");
      return false;
    }

    const FMaterialExpressionCollection Displaced = *Collection;
    Collection->Empty();
    if (UMaterial *Material = Cast<UMaterial>(&Owner)) {
      for (const FRootPropertySpec &RootSpec : RootPropertySpecs()) {
        if (FExpressionInput *Input =
                Material->GetExpressionInputForProperty(RootSpec.Property)) {
          *Input = FExpressionInput();
        }
      }
    }

    AppliedPropertyPlans.Reset();
    TMap<FString, UMaterialExpression *> ById;
    for (FExpressionSpec &Spec : Expressions) {
      UMaterialExpression *Expression =
          CreateExpressionInternal(Owner, Spec.ExpressionClass, Spec.X, Spec.Y);
      if (!Expression) {
        OutError = TEXT("material_expression_create_failed:") + Spec.Id;
        return false;
      }
      Expression->MaterialExpressionGuid = Spec.Guid;
      if (Spec.MaterialFunction) {
        UMaterialExpressionMaterialFunctionCall *FunctionCall =
            Cast<UMaterialExpressionMaterialFunctionCall>(Expression);
        if (!FunctionCall ||
            !FunctionCall->SetMaterialFunction(Spec.MaterialFunction)) {
          OutError = TEXT("material_function_assignment_failed:") + Spec.Id;
          return false;
        }
        FunctionCall->UpdateFromFunctionResource();
      }
      if (!Spec.CustomInputs.IsEmpty()) {
        UMaterialExpressionCustom *Custom =
            Cast<UMaterialExpressionCustom>(Expression);
        if (!Custom) {
          OutError = TEXT("material_custom_inputs_class_conflict:") + Spec.Id;
          return false;
        }
        Custom->Inputs.Reset();
        for (const FName InputName : Spec.CustomInputs) {
          FCustomInput &Input = Custom->Inputs.AddDefaulted_GetRef();
          Input.InputName = InputName;
        }
      }

      TUniquePtr<UeAgentAuthoringObjectApply::FPreparedApply> PropertyPlan;
      TArray<UeAgentAuthoringObjectApply::FResolvedNode> Resolved;
      if (!PrepareProperties(*Expression, Spec.Properties, PropertyPlan,
                             bValidation ? &Spec.ResolvedProperties : &Resolved,
                             OutError)) {
        OutError = Spec.Id + TEXT(":") + OutError;
        return false;
      }
      if (PropertyPlan.IsValid())
        AppliedPropertyPlans.Add(MoveTemp(PropertyPlan));
      if (UMaterialExpressionMaterialFunctionCall *FunctionCall =
              Cast<UMaterialExpressionMaterialFunctionCall>(Expression)) {
        FunctionCall->UpdateFromFunctionResource();
      }
      ById.Add(Spec.NormalizedId, Expression);
    }

    for (const FEdgeSpec &Edge : Edges) {
      UMaterialExpression *const *From = ById.Find(Edge.FromId);
      UMaterialExpression *const *To = ById.Find(Edge.ToId);
      if (!From || !*From || !To || !*To ||
          !ConnectExpressions(**From, Edge.OutputName, **To, Edge.InputName,
                              OutError)) {
        if (OutError.IsEmpty())
          OutError = TEXT("material_edge_expression_not_found");
        return false;
      }
    }

    if (!RootInputs.IsEmpty()) {
      UMaterial *Material = Cast<UMaterial>(&Owner);
      if (!Material) {
        OutError = TEXT("material_function_root_inputs_not_allowed");
        return false;
      }
      for (const FRootInputSpec &Root : RootInputs) {
        UMaterialExpression *const *From = ById.Find(Root.FromId);
        if (!From || !*From) {
          OutError = TEXT("material_root_input_expression_not_found");
          return false;
        }
        if (!UMaterialEditingLibrary::ConnectMaterialProperty(
                *From, NormalizeExpressionPinName(Root.OutputName),
                Root.Property)) {
          int32 OutputIndex = INDEX_NONE;
          FExpressionInput *Input =
              Material->GetExpressionInputForProperty(Root.Property);
          if (!Input || !ResolveOutputIndexInternal(**From, Root.OutputName,
                                                    OutputIndex)) {
            OutError = TEXT("material_root_input_connect_failed:") +
                       MaterialPropertyToString(Root.Property);
            return false;
          }
          (*From)->ConnectExpression(Input, OutputIndex);
        }
      }
    }

    if (!bRollbackCaptured)
      RetireCollection(Displaced);
    RefreshOwnerInternal(Owner);
    return true;
  }
};

FString NormalizeExpressionPinName(const FString &PinName) {
  const FString Result = PinName.TrimStartAndEnd();
  if (Result.Equals(TEXT("None"), ESearchCase::IgnoreCase) ||
      Result.Equals(TEXT("NAME_None"), ESearchCase::IgnoreCase) ||
      Result.Equals(TEXT("null"), ESearchCase::IgnoreCase)) {
    return FString();
  }
  return Result;
}

FMaterialExpressionCollection *GetExpressionCollection(UObject &Owner) {
  return GetCollectionInternal(Owner);
}

const FMaterialExpressionCollection *
GetExpressionCollection(const UObject &Owner) {
  return GetCollectionInternal(Owner);
}

UMaterialExpression *CreateExpression(UObject &Owner, UClass *ExpressionClass,
                                      const int32 X, const int32 Y) {
  return CreateExpressionInternal(Owner, ExpressionClass, X, Y);
}

bool ResolveExpressionInput(UMaterialExpression &Expression,
                            const FString &InputName,
                            FExpressionInput *&OutInput) {
  return ResolveInputInternal(Expression, InputName, OutInput);
}

bool ResolveExpressionOutputIndex(UMaterialExpression &Expression,
                                  const FString &OutputName, int32 &OutIndex) {
  return ResolveOutputIndexInternal(Expression, OutputName, OutIndex);
}

bool ConnectExpressionToProperty(UMaterialExpression &Expression,
                                 const FString &OutputName,
                                 const EMaterialProperty Property,
                                 FString &OutError) {
  OutError.Reset();
  UMaterial *Material = Cast<UMaterial>(Expression.GetOuter());
  if (!Material) {
    OutError = TEXT("material_root_input_owner_invalid");
    return false;
  }
  const FString NormalizedOutput = NormalizeExpressionPinName(OutputName);
  if (UMaterialEditingLibrary::ConnectMaterialProperty(
          &Expression, NormalizedOutput, Property)) {
    return true;
  }
  int32 OutputIndex = INDEX_NONE;
  FExpressionInput *Input = Material->GetExpressionInputForProperty(Property);
  if (!Input ||
      !ResolveOutputIndexInternal(Expression, NormalizedOutput, OutputIndex)) {
    OutError = TEXT("material_root_input_connect_failed:") +
               MaterialPropertyToString(Property);
    return false;
  }
  Expression.ConnectExpression(Input, OutputIndex);
  return true;
}

void RefreshGraphOwner(UObject &Owner) { RefreshOwnerInternal(Owner); }

bool ParseMaterialProperty(const FString &InPropertyName,
                           EMaterialProperty &OutProperty) {
  FString Key = InPropertyName.TrimStartAndEnd().ToLower();
  Key.ReplaceInline(TEXT("_"), TEXT(""));
  if (Key == TEXT("basecolor") || Key == TEXT("diffusecolor")) {
    OutProperty = MP_BaseColor;
    return true;
  }
  if (Key == TEXT("emissivecolor") || Key == TEXT("emissive")) {
    OutProperty = MP_EmissiveColor;
    return true;
  }
  if (Key == TEXT("opacity")) {
    OutProperty = MP_Opacity;
    return true;
  }
  if (Key == TEXT("opacitymask")) {
    OutProperty = MP_OpacityMask;
    return true;
  }
  if (Key == TEXT("metallic")) {
    OutProperty = MP_Metallic;
    return true;
  }
  if (Key == TEXT("specular")) {
    OutProperty = MP_Specular;
    return true;
  }
  if (Key == TEXT("roughness")) {
    OutProperty = MP_Roughness;
    return true;
  }
  if (Key == TEXT("normal")) {
    OutProperty = MP_Normal;
    return true;
  }
  if (Key == TEXT("tangent")) {
    OutProperty = MP_Tangent;
    return true;
  }
  if (Key == TEXT("worldpositionoffset") || Key == TEXT("wpo")) {
    OutProperty = MP_WorldPositionOffset;
    return true;
  }
  if (Key == TEXT("subsurfacecolor")) {
    OutProperty = MP_SubsurfaceColor;
    return true;
  }
  if (Key == TEXT("ambientocclusion") || Key == TEXT("ao")) {
    OutProperty = MP_AmbientOcclusion;
    return true;
  }
  if (Key == TEXT("refraction")) {
    OutProperty = MP_Refraction;
    return true;
  }
  if (Key == TEXT("pixeldepthoffset") || Key == TEXT("pdo")) {
    OutProperty = MP_PixelDepthOffset;
    return true;
  }
  if (Key == TEXT("anisotropy")) {
    OutProperty = MP_Anisotropy;
    return true;
  }
  if (Key == TEXT("frontmaterial")) {
    OutProperty = MP_FrontMaterial;
    return true;
  }
  return false;
}

FString MaterialPropertyToString(const EMaterialProperty Property) {
  for (const FRootPropertySpec &Spec : RootPropertySpecs()) {
    if (Spec.Property == Property)
      return Spec.Name;
  }
  return FString::Printf(TEXT("MaterialProperty_%d"),
                         static_cast<int32>(Property));
}

TArray<EMaterialProperty> GetSupportedMaterialProperties() {
  TArray<EMaterialProperty> Result;
  Result.Reserve(RootPropertySpecs().Num());
  for (const FRootPropertySpec &Spec : RootPropertySpecs())
    Result.Add(Spec.Property);
  return Result;
}

UMaterialExpression *
ResolveExpressionUnique(UObject &Owner, const FExpressionSelector &Selector,
                        FString &OutError) {
  OutError.Reset();
  if (!Selector.HasAny()) {
    OutError = TEXT("material_expression_selector_missing");
    return nullptr;
  }
  UMaterialExpression *Resolved = nullptr;
  auto MergeCandidate = [&](UMaterialExpression *Candidate,
                            const TCHAR *SelectorName) -> bool {
    if (!Candidate) {
      OutError = FString(TEXT("material_expression_not_found:")) + SelectorName;
      return false;
    }
    if (Resolved && Resolved != Candidate) {
      OutError = TEXT("material_expression_selector_conflict");
      return false;
    }
    Resolved = Candidate;
    return true;
  };

  const TConstArrayView<TObjectPtr<UMaterialExpression>> Expressions =
      GetExpressions(Owner);
  if (Selector.ExpressionGuid.IsValid()) {
    UMaterialExpression *Candidate = nullptr;
    for (UMaterialExpression *Expression : Expressions) {
      if (Expression &&
          Expression->MaterialExpressionGuid == Selector.ExpressionGuid) {
        if (Candidate) {
          OutError = TEXT("material_expression_guid_ambiguous");
          return nullptr;
        }
        Candidate = Expression;
      }
    }
    if (!MergeCandidate(Candidate, TEXT("expression_guid")))
      return nullptr;
  }
  if (!Selector.ExpressionName.TrimStartAndEnd().IsEmpty()) {
    UMaterialExpression *Candidate = nullptr;
    for (UMaterialExpression *Expression : Expressions) {
      if (Expression && Expression->GetName().Equals(
                            Selector.ExpressionName.TrimStartAndEnd(),
                            ESearchCase::IgnoreCase)) {
        if (Candidate) {
          OutError = TEXT("material_expression_name_ambiguous");
          return nullptr;
        }
        Candidate = Expression;
      }
    }
    if (!MergeCandidate(Candidate, TEXT("expression_name")))
      return nullptr;
  }
  if (Selector.ExpressionIndex != INDEX_NONE) {
    UMaterialExpression *Candidate =
        Expressions.IsValidIndex(Selector.ExpressionIndex)
            ? Expressions[Selector.ExpressionIndex]
            : nullptr;
    if (!MergeCandidate(Candidate, TEXT("expression_index")))
      return nullptr;
  }
  return Resolved;
}

bool ConnectExpressions(UMaterialExpression &FromExpression,
                        const FString &FromOutputName,
                        UMaterialExpression &ToExpression,
                        const FString &ToInputName, FString &OutError) {
  OutError.Reset();
  const FString OutputName = NormalizeExpressionPinName(FromOutputName);
  const FString InputName = NormalizeExpressionPinName(ToInputName);
  if (UMaterialEditingLibrary::ConnectMaterialExpressions(
          &FromExpression, OutputName, &ToExpression, InputName)) {
    return true;
  }
  int32 OutputIndex = INDEX_NONE;
  FExpressionInput *Input = nullptr;
  if (!ResolveExpressionOutputIndex(FromExpression, OutputName, OutputIndex) ||
      !ResolveExpressionInput(ToExpression, InputName, Input) || !Input) {
    OutError = FString::Printf(TEXT("material_expression_pin_unresolved:from=%"
                                    "s output=%s to=%s input=%s"),
                               *FromExpression.GetPathName(), *OutputName,
                               *ToExpression.GetPathName(), *InputName);
    return false;
  }
  ToExpression.Modify();
  FromExpression.ConnectExpression(Input, OutputIndex);
  return true;
}

FPreparedGraph::FPreparedGraph() : Impl(MakeUnique<FImpl>()) {}

FPreparedGraph::~FPreparedGraph() = default;
FPreparedGraph::FPreparedGraph(FPreparedGraph &&) noexcept = default;
FPreparedGraph &FPreparedGraph::operator=(FPreparedGraph &&) noexcept = default;

bool FPreparedGraph::Prepare(UObject &ValidationOwner,
                             const TSharedPtr<const FJsonObject> &GraphDocument,
                             const FString &ContextAssetPath,
                             const FString &ContextGraphName,
                             FString &OutError) {
  Impl = MakeUnique<FImpl>();
  OutError.Reset();
  if (!GetExpressionCollection(ValidationOwner) || !GraphDocument.IsValid()) {
    OutError = TEXT("material_graph_prepare_input_invalid");
    return false;
  }

  TArray<TSharedPtr<FJsonValue>> ExpressionStorage;
  bool bExpressionsPresent = false;
  if (!ReadCollectionValues(GraphDocument, TEXT("expressions"), TEXT("id"),
                            ExpressionStorage, bExpressionsPresent, OutError)) {
    return false;
  }
  if (!bExpressionsPresent) {
    OutError = TEXT("material_graph_expressions_missing");
    return false;
  }
  const TArray<TSharedPtr<FJsonValue>> *ExpressionValues = &ExpressionStorage;
  TSet<FString> SeenIds;
  TSet<FGuid> SeenGuids;
  for (int32 Index = 0; Index < ExpressionValues->Num(); ++Index) {
    const TSharedPtr<FJsonObject> *ExpressionObject = nullptr;
    if (!(*ExpressionValues)[Index].IsValid() ||
        !(*ExpressionValues)[Index]->TryGetObject(ExpressionObject) ||
        !ExpressionObject || !ExpressionObject->IsValid()) {
      OutError =
          FString::Printf(TEXT("material_graph_expression_invalid:%d"), Index);
      return false;
    }

    FExpressionSpec &Spec = Impl->Expressions.AddDefaulted_GetRef();
    if (!ReadRequiredString(*ExpressionObject, TEXT("id"), Spec.Id, OutError))
      return false;
    Spec.NormalizedId = NormalizeId(Spec.Id);
    if (SeenIds.Contains(Spec.NormalizedId)) {
      OutError = TEXT("material_graph_expression_id_duplicate:") + Spec.Id;
      return false;
    }
    SeenIds.Add(Spec.NormalizedId);

    FString GuidText;
    (*ExpressionObject)
        ->TryGetStringField(TEXT("ue_expression_guid"), GuidText);
    if (GuidText.IsEmpty())
      (*ExpressionObject)->TryGetStringField(TEXT("expression_guid"), GuidText);
    if (GuidText.IsEmpty())
      FGuid::Parse(Spec.Id, Spec.Guid);
    if (!GuidText.IsEmpty() && !FGuid::Parse(GuidText, Spec.Guid)) {
      OutError = TEXT("material_graph_expression_guid_invalid:") + Spec.Id;
      return false;
    }
    if (!Spec.Guid.IsValid())
      Spec.Guid = FGuid::NewGuid();
    if (SeenGuids.Contains(Spec.Guid)) {
      OutError = TEXT("material_graph_expression_guid_duplicate:") + Spec.Id;
      return false;
    }
    SeenGuids.Add(Spec.Guid);

    FExpressionResolveRequest ResolveRequest;
    (*ExpressionObject)
        ->TryGetStringField(TEXT("expression_class"),
                            ResolveRequest.ExpressionClassPath);
    (*ExpressionObject)->TryGetStringField(TEXT("path"), ResolveRequest.Path);
    (*ExpressionObject)
        ->TryGetStringField(TEXT("name"), ResolveRequest.CatalogName);
    if (ResolveRequest.CatalogName.IsEmpty()) {
      (*ExpressionObject)
          ->TryGetStringField(TEXT("catalog_name"), ResolveRequest.CatalogName);
    }
    (*ExpressionObject)
        ->TryGetStringField(TEXT("material_function_path"),
                            ResolveRequest.MaterialFunctionPath);
    ResolveRequest.ContextAssetPath = ContextAssetPath;
    ResolveRequest.ContextGraphName = ContextGraphName;
    FExpressionResolveResult ResolveResult;
    if (!ResolveExpression(ResolveRequest, ResolveResult, OutError)) {
      OutError = Spec.Id + TEXT(":") + OutError;
      return false;
    }
    Spec.ExpressionClass = ResolveResult.ExpressionClass;
    Spec.MaterialFunction = ResolveResult.MaterialFunction;

    TSharedPtr<FJsonObject> Position;
    if (TryGetObject(*ExpressionObject, TEXT("pos"), Position)) {
      double X = 0.0;
      double Y = 0.0;
      Position->TryGetNumberField(TEXT("x"), X);
      Position->TryGetNumberField(TEXT("y"), Y);
      Spec.X = static_cast<int32>(X);
      Spec.Y = static_cast<int32>(Y);
    }

    const TArray<TSharedPtr<FJsonValue>> *Properties = nullptr;
    if ((*ExpressionObject)->TryGetArrayField(TEXT("properties"), Properties) &&
        Properties) {
      Spec.Properties = *Properties;
    }

    const TArray<TSharedPtr<FJsonValue>> *CustomInputs = nullptr;
    if ((*ExpressionObject)
            ->TryGetArrayField(TEXT("custom_inputs"), CustomInputs) &&
        CustomInputs) {
      TSet<FName> SeenInputNames;
      for (const TSharedPtr<FJsonValue> &Value : *CustomInputs) {
        FString Name;
        if (Value.IsValid() && Value->Type == EJson::String) {
          Name = Value->AsString();
        } else {
          const TSharedPtr<FJsonObject> *InputObject = nullptr;
          if (!Value.IsValid() || !Value->TryGetObject(InputObject) ||
              !InputObject || !InputObject->IsValid() ||
              (!(*InputObject)->TryGetStringField(TEXT("name"), Name) &&
               !(*InputObject)->TryGetStringField(TEXT("input_name"), Name))) {
            OutError = TEXT("material_custom_input_invalid:") + Spec.Id;
            return false;
          }
        }
        Name = NormalizeExpressionPinName(Name);
        if (Name.IsEmpty() || SeenInputNames.Contains(FName(*Name))) {
          OutError = TEXT("material_custom_input_name_invalid:") + Spec.Id;
          return false;
        }
        SeenInputNames.Add(FName(*Name));
        Spec.CustomInputs.Add(FName(*Name));
      }
    }
  }

  TArray<TSharedPtr<FJsonValue>> EdgeStorage;
  bool bEdgesPresent = false;
  if (!ReadCollectionValues(GraphDocument, TEXT("edges"), nullptr, EdgeStorage,
                            bEdgesPresent, OutError)) {
    return false;
  }
  const TArray<TSharedPtr<FJsonValue>> *EdgeValues = &EdgeStorage;
  if (bEdgesPresent) {
    for (int32 Index = 0; Index < EdgeValues->Num(); ++Index) {
      const TSharedPtr<FJsonObject> *EdgeObject = nullptr;
      TSharedPtr<FJsonObject> From;
      TSharedPtr<FJsonObject> To;
      if (!(*EdgeValues)[Index].IsValid() ||
          !(*EdgeValues)[Index]->TryGetObject(EdgeObject) || !EdgeObject ||
          !EdgeObject->IsValid() ||
          !TryGetObject(*EdgeObject, TEXT("from"), From) ||
          !TryGetObject(*EdgeObject, TEXT("to"), To)) {
        OutError =
            FString::Printf(TEXT("material_graph_edge_invalid:%d"), Index);
        return false;
      }
      FEdgeSpec &Edge = Impl->Edges.AddDefaulted_GetRef();
      if (!ReadRequiredString(From, TEXT("node"), Edge.FromId, OutError) ||
          !ReadRequiredString(To, TEXT("node"), Edge.ToId, OutError))
        return false;
      Edge.FromId = NormalizeId(Edge.FromId);
      Edge.ToId = NormalizeId(Edge.ToId);
      From->TryGetStringField(TEXT("output"), Edge.OutputName);
      To->TryGetStringField(TEXT("input"), Edge.InputName);
      if (!SeenIds.Contains(Edge.FromId) || !SeenIds.Contains(Edge.ToId)) {
        OutError = FString::Printf(
            TEXT("material_graph_edge_target_missing:%d"), Index);
        return false;
      }
    }
  }
  for (const FEdgeSpec &Edge : Impl->Edges) {
    FExpressionSpec *Target = Impl->Expressions.FindByPredicate(
        [&Edge](const FExpressionSpec &Candidate) {
          return Candidate.NormalizedId == Edge.ToId;
        });
    if (!Target || !Target->ExpressionClass ||
        !Target->ExpressionClass->IsChildOf(
            UMaterialExpressionCustom::StaticClass()))
      continue;
    const FString InputName = NormalizeExpressionPinName(Edge.InputName);
    if (InputName.IsEmpty()) {
      OutError = TEXT("material_custom_input_name_missing:") + Target->Id;
      return false;
    }
    const FName Name(*InputName);
    if (!Target->CustomInputs.Contains(Name))
      Target->CustomInputs.Add(Name);
  }

  TArray<TSharedPtr<FJsonValue>> RootStorage;
  bool bRootInputsPresent = false;
  if (!ReadCollectionValues(GraphDocument, TEXT("root_inputs"),
                            TEXT("material_property"), RootStorage,
                            bRootInputsPresent, OutError)) {
    return false;
  }
  const TArray<TSharedPtr<FJsonValue>> *RootValues = &RootStorage;
  if (bRootInputsPresent) {
    if (!Cast<UMaterial>(&ValidationOwner) && !RootValues->IsEmpty()) {
      OutError = TEXT("material_function_root_inputs_not_allowed");
      return false;
    }
    TSet<EMaterialProperty> SeenProperties;
    for (int32 Index = 0; Index < RootValues->Num(); ++Index) {
      const TSharedPtr<FJsonObject> *RootObject = nullptr;
      TSharedPtr<FJsonObject> From;
      FString PropertyName;
      if (!(*RootValues)[Index].IsValid() ||
          !(*RootValues)[Index]->TryGetObject(RootObject) || !RootObject ||
          !RootObject->IsValid() ||
          !ReadRequiredString(*RootObject, TEXT("material_property"),
                              PropertyName, OutError) ||
          !TryGetObject(*RootObject, TEXT("from"), From)) {
        OutError = FString::Printf(
            TEXT("material_graph_root_input_invalid:%d:%s"), Index, *OutError);
        return false;
      }
      FRootInputSpec &Root = Impl->RootInputs.AddDefaulted_GetRef();
      if (!ParseMaterialProperty(PropertyName, Root.Property) ||
          !ReadRequiredString(From, TEXT("node"), Root.FromId, OutError)) {
        if (OutError.IsEmpty())
          OutError =
              TEXT("material_graph_root_property_invalid:") + PropertyName;
        return false;
      }
      Root.FromId = NormalizeId(Root.FromId);
      From->TryGetStringField(TEXT("output"), Root.OutputName);
      if (!SeenIds.Contains(Root.FromId) ||
          SeenProperties.Contains(Root.Property)) {
        OutError = FString::Printf(
            TEXT("material_graph_root_input_conflict:%d"), Index);
        return false;
      }
      SeenProperties.Add(Root.Property);
    }
  }

  if (!Impl->ReplaceGraph(ValidationOwner, true, OutError))
    return false;
  Impl->bReady = true;
  return true;
}

bool FPreparedGraph::CaptureRollback(UObject &Owner, FString &OutError) {
  OutError.Reset();
  if (!Impl.IsValid() || !Impl->bReady) {
    OutError = TEXT("material_graph_plan_not_ready");
    return false;
  }
  FMaterialExpressionCollection *Collection = GetCollectionInternal(Owner);
  if (!Collection) {
    OutError = TEXT("material_graph_owner_type_invalid");
    return false;
  }
  Impl->ResetRollback();
  Impl->OldCollection = *Collection;
  for (UMaterialExpression *Expression : Collection->Expressions) {
    if (Expression)
      Impl->OldObjects.Emplace(Expression);
  }
  for (UMaterialExpressionComment *Comment : Collection->EditorComments) {
    if (Comment)
      Impl->OldObjects.Emplace(Comment);
  }
  if (UMaterial *Material = Cast<UMaterial>(&Owner)) {
    for (const FRootPropertySpec &Spec : RootPropertySpecs()) {
      if (FExpressionInput *Input =
              Material->GetExpressionInputForProperty(Spec.Property)) {
        FRootInputBackup &Backup = Impl->OldRootInputs.AddDefaulted_GetRef();
        Backup.Property = Spec.Property;
        Backup.Input = *Input;
      }
    }
  }
  Impl->bRollbackCaptured = true;
  return true;
}

bool FPreparedGraph::ApplyTo(UObject &Owner, FString &OutError) {
  if (!Impl.IsValid() || !Impl->bReady) {
    OutError = TEXT("material_graph_plan_not_ready");
    return false;
  }
  return Impl->ReplaceGraph(Owner, false, OutError);
}

bool FPreparedGraph::AppendPropertyReadRequests(
    UObject &Owner, const FString &AssetPath,
    TArray<UeAgentAuthoringState::FReadRequest> &OutRequests,
    FOperationFailure &OutFailure) const {
  if (!Impl.IsValid() || !Impl->bReady) {
    OutFailure.ErrorCode = TEXT("material_graph_plan_not_ready");
    OutFailure.ErrorDetail = OutFailure.ErrorCode;
    return false;
  }
  for (const FExpressionSpec &Spec : Impl->Expressions) {
    if (Spec.ResolvedProperties.IsEmpty())
      continue;
    FExpressionSelector Selector;
    Selector.ExpressionGuid = Spec.Guid;
    FString ResolveError;
    UMaterialExpression *Expression =
        ResolveExpressionUnique(Owner, Selector, ResolveError);
    if (!Expression) {
      OutFailure.ErrorCode =
          ResolveError.IsEmpty()
              ? TEXT("material_expression_readback_target_missing")
              : ResolveError;
      OutFailure.ErrorDetail = Spec.Id;
      return false;
    }
    if (!UeAgentAuthoringOperationApply::AppendPropertyReadRequests(
            FReflectionRoot::MakeObject(Expression),
            AssetPath + TEXT("#") +
                Spec.Guid.ToString(EGuidFormats::DigitsWithHyphensLower),
            Spec.ResolvedProperties, OutRequests, OutFailure)) {
      return false;
    }
  }
  return true;
}

bool FPreparedGraph::Rollback(UObject &Owner, FString &OutError) {
  OutError.Reset();
  if (!Impl.IsValid() || !Impl->bRollbackCaptured)
    return true;
  FMaterialExpressionCollection *Collection = GetExpressionCollection(Owner);
  if (!Collection) {
    OutError = TEXT("material_graph_owner_type_invalid");
    return false;
  }
  RetireCollection(*Collection);
  *Collection = Impl->OldCollection;
  if (UMaterial *Material = Cast<UMaterial>(&Owner)) {
    for (const FRootInputBackup &Backup : Impl->OldRootInputs) {
      if (FExpressionInput *Input =
              Material->GetExpressionInputForProperty(Backup.Property)) {
        *Input = Backup.Input;
      }
    }
  }
  RefreshGraphOwner(Owner);
  Impl->ResetRollback();
  return true;
}

void FPreparedGraph::Commit() {
  if (!Impl.IsValid() || !Impl->bRollbackCaptured)
    return;
  RetireCollection(Impl->OldCollection);
  Impl->ResetRollback();
}

bool FPreparedGraph::IsReady() const { return Impl.IsValid() && Impl->bReady; }

int32 FPreparedGraph::GetExpressionCount() const {
  return Impl.IsValid() ? Impl->Expressions.Num() : 0;
}

int32 FPreparedGraph::GetEdgeCount() const {
  return Impl.IsValid() ? Impl->Edges.Num() : 0;
}

int32 FPreparedGraph::GetRootInputCount() const {
  return Impl.IsValid() ? Impl->RootInputs.Num() : 0;
}

bool BuildGraphState(const UObject &Owner, TSharedPtr<FJsonObject> &OutState,
                     FString &OutError) {
  OutState = MakeShared<FJsonObject>();
  OutError.Reset();
  const FMaterialExpressionCollection *Collection =
      GetCollectionInternal(Owner);
  if (!Collection) {
    OutError = TEXT("material_graph_owner_type_invalid");
    return false;
  }

  TArray<UMaterialExpression *> Expressions;
  TMap<const UMaterialExpression *, FString> GuidByExpression;
  for (UMaterialExpression *Expression : Collection->Expressions) {
    if (!Expression)
      continue;
    if (!Expression->MaterialExpressionGuid.IsValid()) {
      OutError =
          TEXT("material_expression_guid_missing:") + Expression->GetPathName();
      return false;
    }
    Expressions.Add(Expression);
    GuidByExpression.Add(Expression,
                         Expression->MaterialExpressionGuid.ToString(
                             EGuidFormats::DigitsWithHyphensLower));
  }
  Expressions.Sort(
      [](const UMaterialExpression &A, const UMaterialExpression &B) {
        return A.MaterialExpressionGuid.ToString(
                   EGuidFormats::DigitsWithHyphensLower) <
               B.MaterialExpressionGuid.ToString(
                   EGuidFormats::DigitsWithHyphensLower);
      });

  TArray<TSharedPtr<FJsonValue>> ExpressionStates;
  for (UMaterialExpression *Expression : Expressions) {
    TSharedRef<FJsonObject> State = MakeShared<FJsonObject>();
    State->SetStringField(TEXT("expression_guid"),
                          GuidByExpression[Expression]);
    State->SetStringField(TEXT("class_path"),
                          Expression->GetClass()->GetPathName());
    State->SetNumberField(TEXT("x"), Expression->MaterialExpressionEditorX);
    State->SetNumberField(TEXT("y"), Expression->MaterialExpressionEditorY);
    if (const UMaterialExpressionMaterialFunctionCall *FunctionCall =
            Cast<UMaterialExpressionMaterialFunctionCall>(Expression)) {
      if (FunctionCall->MaterialFunction) {
        State->SetStringField(TEXT("material_function_path"),
                              FunctionCall->MaterialFunction->GetPathName());
      }
    }
    if (const UMaterialExpressionCustom *Custom =
            Cast<UMaterialExpressionCustom>(Expression)) {
      TArray<TSharedPtr<FJsonValue>> Inputs;
      for (const FCustomInput &Input : Custom->Inputs) {
        Inputs.Add(MakeShared<FJsonValueString>(Input.InputName.ToString()));
      }
      State->SetArrayField(TEXT("custom_inputs"), Inputs);
    }
    ExpressionStates.Add(MakeShared<FJsonValueObject>(State));
  }
  OutState->SetArrayField(TEXT("expressions"), ExpressionStates);

  struct FSortableState {
    FString Key;
    TSharedPtr<FJsonValue> Value;
  };
  TArray<FSortableState> EdgeStates;
  for (UMaterialExpression *ToExpression : Expressions) {
    for (FExpressionInputIterator It{ToExpression}; It; ++It) {
      if (!It->Expression)
        continue;
      const FString *FromGuid = GuidByExpression.Find(It->Expression);
      const FString *ToGuid = GuidByExpression.Find(ToExpression);
      if (!FromGuid || !ToGuid)
        continue;
      const FString OutputName =
          OutputNameForIndex(*It->Expression, It->OutputIndex);
      const FString InputName =
          GetInputDisplayName(*ToExpression, It.Index, *It.Input);
      TSharedRef<FJsonObject> State = MakeShared<FJsonObject>();
      State->SetStringField(TEXT("from_guid"), *FromGuid);
      State->SetStringField(TEXT("output"), OutputName);
      State->SetStringField(TEXT("to_guid"), *ToGuid);
      State->SetStringField(TEXT("input"), InputName);
      EdgeStates.Add({*FromGuid + TEXT("|") + OutputName + TEXT("|") + *ToGuid +
                          TEXT("|") + InputName,
                      MakeShared<FJsonValueObject>(State)});
    }
  }
  EdgeStates.Sort([](const FSortableState &A, const FSortableState &B) {
    return A.Key < B.Key;
  });
  TArray<TSharedPtr<FJsonValue>> SortedEdges;
  for (const FSortableState &State : EdgeStates)
    SortedEdges.Add(State.Value);
  OutState->SetArrayField(TEXT("edges"), SortedEdges);

  TArray<FSortableState> RootStates;
  if (const UMaterial *Material = Cast<UMaterial>(&Owner)) {
    for (const FRootPropertySpec &Spec : RootPropertySpecs()) {
      const FExpressionInput *Input =
          const_cast<UMaterial *>(Material)->GetExpressionInputForProperty(
              Spec.Property);
      if (!Input || !Input->Expression)
        continue;
      const FString *FromGuid = GuidByExpression.Find(Input->Expression);
      if (!FromGuid)
        continue;
      TSharedRef<FJsonObject> State = MakeShared<FJsonObject>();
      State->SetStringField(TEXT("material_property"), Spec.Name);
      State->SetStringField(TEXT("from_guid"), *FromGuid);
      State->SetStringField(
          TEXT("output"),
          OutputNameForIndex(*Input->Expression, Input->OutputIndex));
      RootStates.Add({Spec.Name, MakeShared<FJsonValueObject>(State)});
    }
  }
  RootStates.Sort([](const FSortableState &A, const FSortableState &B) {
    return A.Key < B.Key;
  });
  TArray<TSharedPtr<FJsonValue>> SortedRoots;
  for (const FSortableState &State : RootStates)
    SortedRoots.Add(State.Value);
  OutState->SetArrayField(TEXT("root_inputs"), SortedRoots);
  return true;
}
} // namespace UeAgentMaterialAuthoring

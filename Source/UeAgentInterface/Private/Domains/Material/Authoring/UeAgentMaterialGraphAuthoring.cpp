// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/Material/Authoring/UeAgentMaterialGraphAuthoring.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "Domains/Material/Authoring/UeAgentMaterialAuthoringState.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringJsonFields.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Utilities/AuthoringCore/UeAgentDetailsAuthoringService.h"
#include "Utilities/UeAgentPropertyValueCodec.h"

#include "Core/HjsonWorkspace/UeAgentGraphAuthoringProjection.h"
#include "Core/HjsonWorkspace/DomainAuthoring/UeAgentDomainHjsonAuthoringKit.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Domains/Material/Authoring/UeAgentMaterialExpressionResolver.h"
#include "Domains/Material/Authoring/UeAgentMaterialInstanceAuthoring.h"
#include "MaterialEditingLibrary.h"
#include "MaterialGraph/MaterialGraph.h"
#include "MaterialGraph/MaterialGraphNode_Root.h"
#include "MaterialGraph/MaterialGraphNode_Comment.h"
#include "MaterialShared.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionComment.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialExpressionMaterialAttributeLayers.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialFunctionInterface.h"
#include "Misc/ScopeExit.h"
#include "Serialization/ObjectReader.h"
#include "Serialization/ObjectWriter.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectHash.h"
#include "Utilities/AuthoringObjectApply/UeAgentAuthoringObjectApply.h"
#include "Utilities/AuthoringOperationApply/UeAgentAuthoringObjectApplyProvider.h"

namespace UeAgentMaterialAuthoring {
namespace {
using namespace UeAgentAuthoringOperationApply;

UE::UAI::Contract::FDiagnostic GraphFailure(const TCHAR* Code, const FString& Reason,
    const FString& Field, TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected,
    const FJsonObject* Input = nullptr, const FString& InputField = FString(),
    const FString& Candidate = FString(), const FString& Pointer = FString()) {
  auto Error = UE::UAI::Contract::FDiagnostic::Failure(Code, Reason, TEXT("material_graph"));
  Error.PropertyPath = Field;
  Error.CandidateName = Candidate;
  Error.ActualValuePresent = Actual.IsValid();
  Error.ActualValue = MoveTemp(Actual);
  Error.ExpectedValue = MoveTemp(Expected);
  Error.Source.JsonPath = Pointer;
  if (Input) UeAgentAuthoringDiagnostics::LocateAuthoringSource(Error, *Input, InputField);
  else Error.SourceKind = TEXT("native_result");
  return Error;
}

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
      {TEXT("MaterialAttributes"), MP_MaterialAttributes},
  };
  return Specs;
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
  if (ExpressionClass && ExpressionClass->IsChildOf<UMaterialExpressionComment>()) {
    FMaterialExpressionCollection* Collection = GetCollectionInternal(Owner);
    if (!Collection) return nullptr;
    auto* Comment = NewObject<UMaterialExpressionComment>(&Owner, ExpressionClass, NAME_None, RF_Transactional);
    Comment->Material = Cast<UMaterial>(&Owner);
    Comment->MaterialExpressionEditorX = X;
    Comment->MaterialExpressionEditorY = Y;
    // The menu initializes a visible empty box, rather than the expression's
    // zero-size CDO. Use the native comment node dimensions without an editor.
    const auto* Defaults = GetDefault<UMaterialGraphNode_Comment>();
    Comment->SizeX = Defaults->NodeWidth;
    Comment->SizeY = Defaults->NodeHeight;
    Comment->Text = NSLOCTEXT("K2Node", "CommentBlock_NewEmptyComment", "Comment").ToString();
    Comment->UpdateMaterialExpressionGuid(true, false);
    Collection->AddComment(Comment);
    return Comment;
  }
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

struct FExpressionPinBinding {
  UE::UAI::HjsonWorkspace::FGraphAuthoringPinAliasInput Selector;
  int32 NativeIndex = INDEX_NONE;
  FExpressionInput* Input = nullptr;
};

TArray<FExpressionPinBinding> CaptureExpressionPins(UMaterialExpression& Expression) {
  TArray<FExpressionPinBinding> Pins;
  TMap<FString, int32> InputOccurrences;
  for (FExpressionInputIterator It{&Expression}; It; ++It) {
    FExpressionPinBinding& Pin = Pins.AddDefaulted_GetRef();
    Pin.Selector.SourceId = FString::FromInt(Pins.Num() - 1);
    Pin.Selector.SemanticName = GetInputDisplayName(Expression, It.Index, *It.Input);
    Pin.Selector.Occurrence = InputOccurrences.FindOrAdd(Pin.Selector.SemanticName)++;
    Pin.Selector.Direction = UE::UAI::HjsonWorkspace::EGraphAuthoringEndpointDirection::Input;
    Pin.NativeIndex = It.Index;
    Pin.Input = It.Input;
  }
  TMap<FString, int32> OutputOccurrences;
  const auto& Outputs = Expression.GetOutputs();
  for (int32 Index = 0; Index < Outputs.Num(); ++Index) {
    FExpressionPinBinding& Pin = Pins.AddDefaulted_GetRef();
    Pin.Selector.SourceId = FString::FromInt(Pins.Num() - 1);
    Pin.Selector.SemanticName = Outputs[Index].OutputName.IsNone()
        ? FString() : Outputs[Index].OutputName.ToString();
    Pin.Selector.Occurrence = OutputOccurrences.FindOrAdd(Pin.Selector.SemanticName)++;
    Pin.Selector.Direction = UE::UAI::HjsonWorkspace::EGraphAuthoringEndpointDirection::Output;
    Pin.NativeIndex = Index;
  }
  return Pins;
}

enum class EPinAliasResolution { Missing, Resolved, Rejected };

EPinAliasResolution ResolvePinAlias(UMaterialExpression& Expression, const FString& Name,
    UE::UAI::HjsonWorkspace::EGraphAuthoringEndpointDirection Direction,
    FExpressionPinBinding& OutPin) {
  const auto Pins = CaptureExpressionPins(Expression);
  TArray<UE::UAI::HjsonWorkspace::FGraphAuthoringPinAliasInput> AliasInputs;
  AliasInputs.Reserve(Pins.Num());
  for (const auto& Pin : Pins) AliasInputs.Add(Pin.Selector);
  UE::UAI::HjsonWorkspace::FGraphAuthoringPinAliasProjection Aliases;
  UE::UAI::HjsonWorkspace::FWorkspaceError AliasError;
  if (!UE::UAI::HjsonWorkspace::TryBuildGraphAuthoringPinAliasProjection(
          AliasInputs, Aliases, AliasError)) return EPinAliasResolution::Rejected;
  for (const auto& Pin : Pins) {
    if (Aliases.AliasBySourceId.FindRef(Pin.Selector.SourceId).Equals(
            Name.TrimStartAndEnd(), ESearchCase::IgnoreCase)) {
      if (Pin.Selector.Direction != Direction) return EPinAliasResolution::Rejected;
      OutPin = Pin;
      return EPinAliasResolution::Resolved;
    }
  }
  return EPinAliasResolution::Missing;
}

int32 PinOccurrenceForIndex(UMaterialExpression& Expression, const int32 NativeIndex,
    UE::UAI::HjsonWorkspace::EGraphAuthoringEndpointDirection Direction) {
  for (const auto& Pin : CaptureExpressionPins(Expression))
    if (Pin.NativeIndex == NativeIndex && Pin.Selector.Direction == Direction)
      return Pin.Selector.Occurrence;
  return 0;
}

bool ResolveOutputIndexInternal(UMaterialExpression &Expression,
                                const FString &OutputName, int32 &OutIndex) {
  OutIndex = INDEX_NONE;
  FExpressionPinBinding Pin;
  const auto Resolution = ResolvePinAlias(Expression, OutputName,
      UE::UAI::HjsonWorkspace::EGraphAuthoringEndpointDirection::Output, Pin);
  if (Resolution != EPinAliasResolution::Missing) {
    if (Resolution != EPinAliasResolution::Resolved) return false;
    OutIndex = Pin.NativeIndex;
    return true;
  }
  const TArray<FExpressionOutput> &Outputs = Expression.GetOutputs();
  if (Outputs.IsEmpty()) return false;
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
      if (OutIndex != INDEX_NONE) { OutIndex = INDEX_NONE; return false; }
      OutIndex = Index;
    }
  }
  return OutIndex != INDEX_NONE;
}

bool ResolveInputInternal(UMaterialExpression &Expression,
                          const FString &InputName,
                          FExpressionInput *&OutInput) {
  OutInput = nullptr;
  FExpressionPinBinding Pin;
  const auto Resolution = ResolvePinAlias(Expression, InputName,
      UE::UAI::HjsonWorkspace::EGraphAuthoringEndpointDirection::Input, Pin);
  if (Resolution != EPinAliasResolution::Missing) {
    if (Resolution != EPinAliasResolution::Resolved) return false;
    OutInput = Pin.Input;
    return OutInput != nullptr;
  }
  const FString Normalized = NormalizeExpressionPinName(InputName);
  FExpressionInput *OnlyInput = nullptr;
  int32 Count = 0;
  for (FExpressionInputIterator It{&Expression}; It; ++It) {
    OnlyInput = Count == 0 ? It.Input : OnlyInput;
    ++Count;
    if (!Normalized.IsEmpty() &&
        MatchesInputName(Expression, It.Index, *It.Input, Normalized)) {
      if (OutInput) { OutInput = nullptr; return false; }
      OutInput = It.Input;
    }
  }
  if (Normalized.IsEmpty() && Count == 1) {
    OutInput = OnlyInput;
    return true;
  }
  return OutInput != nullptr;
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

// These objects are planner projections, so their source paths must follow the
// explicit transformation rather than their generated field names.
struct FGraphSources {
  TSharedRef<FJsonObject> Objects = MakeShared<FJsonObject>();
  TMap<FString, UE::UAI::Contract::FSourceLocation> Locations;

  void Add(const TSharedRef<FJsonObject>& Derived, const FJsonObject& Original,
           const TMap<FString, FString>& Aliases = {}) {
    using FSources = UeAgentAuthoringDiagnostics::FScopedSourceLocations;
    const auto OriginalLocations = FSources::CaptureProjection(Original);
    const FString Key = FString::FromInt(Objects->Values.Num());
    const FString Prefix = TEXT("/") + Key;
    for (const auto& Pair : OriginalLocations) Locations.Add(Prefix + Pair.Key, Pair.Value);
    for (const auto& Alias : Aliases) {
      const auto Location = FSources::ResolveProjection(OriginalLocations, Alias.Value);
      Locations.Add(Prefix + Alias.Key, Location);
      for (const auto& Pair : OriginalLocations)
        if (Pair.Key.StartsWith(Alias.Value + TEXT("/")))
          Locations.Add(Prefix + Alias.Key + Pair.Key.Mid(Alias.Value.Len()), Pair.Value);
    }
    Objects->SetObjectField(Key, Derived);
  }
};

bool ReadCollectionValues(const TSharedPtr<const FJsonObject> &Document,
                          const TCHAR *Field, const TCHAR *IdentityField,
                          TArray<TSharedPtr<FJsonValue>> &OutValues,
                          bool &bOutPresent, UE::UAI::Contract::FDiagnostic& OutError,
                          FGraphSources& Sources) {
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
    if (Document.IsValid() && Document->HasField(Field)) {
      OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("material_graph_collection_invalid"), FString::Printf(TEXT("Collection '%s' must be an array or object."), Field), TEXT("input_decode"));
      OutError.PropertyPath = Field;
      OutError.ActualValue = Document->TryGetField(Field);
      OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("array or object"));
      UeAgentAuthoringDiagnostics::LocateFailure(OutError, *Document, TEXT("input_decode"), FString(), Field);
      return false;
    }
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
      OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("material_graph_map_value_invalid"), FString::Printf(TEXT("Collection '%s' item '%s' must be an object."), Field, *Key), TEXT("input_decode"));
      OutError.PropertyPath = FString(Field) + TEXT("/") + Key;
      OutError.ActualValue = Source;
      OutError.ActualValuePresent = Source.IsValid();
      OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("object"));
      UeAgentAuthoringDiagnostics::LocateAuthoringSource(OutError, **Map, Key);
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
    Sources.Add(Copy, *SourceObject);
    OutValues.Add(MakeShared<FJsonValueObject>(Copy));
  }
  return true;
}

bool ReadRequiredString(const TSharedPtr<FJsonObject> &Object,
                        const TCHAR *Field, FString &OutValue,
                        UE::UAI::Contract::FDiagnostic& OutError) {
  OutValue.Reset();
  if (!Object.IsValid() || !Object->TryGetStringField(Field, OutValue)) {
    OutError = GraphFailure(TEXT("material_graph_field_missing"),
        FString::Printf(TEXT("Required field '%s' must be present as a string."), Field), Field,
        Object.IsValid() ? Object->TryGetField(Field) : nullptr, MakeShared<FJsonValueString>(TEXT("non-empty string")),
        Object.Get(), Field);
    OutError.Stage = TEXT("input_decode");
    return false;
  }
  OutValue.TrimStartAndEndInline();
  if (OutValue.IsEmpty()) {
    OutError = GraphFailure(TEXT("material_graph_field_empty"),
        FString::Printf(TEXT("Required field '%s' cannot be empty or whitespace."), Field), Field,
        Object->TryGetField(Field), MakeShared<FJsonValueString>(TEXT("non-empty string")), Object.Get(), Field);
    OutError.Stage = TEXT("input_decode");
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

bool ReadNodePosition(const FJsonObject& Node, int32& X, int32& Y, UE::UAI::Contract::FDiagnostic& OutError) {
  const TSharedPtr<FJsonObject>* Position = nullptr;
  const FJsonObject* Fields = &Node;
  if (Node.HasField(TEXT("pos"))) {
    if (!Node.TryGetObjectField(TEXT("pos"), Position) || !Position || !Position->IsValid()) {
      OutError = GraphFailure(TEXT("material_graph_position_object_required"), TEXT("Node pos must be an object containing numeric x/y fields."),
          TEXT("pos"), Node.TryGetField(TEXT("pos")), MakeShared<FJsonValueString>(TEXT("position object")), &Node, TEXT("pos"));
      return false;
    }
    Fields = Position->Get();
  }
  const auto ReadAxis = [&](const TCHAR* Name, int32& Value) {
    if (!Fields->HasField(Name)) return true;
    double Number = 0.0;
    if (!Fields->TryGetNumberField(Name, Number)) {
      OutError = GraphFailure(TEXT("material_graph_position_number_required"),
          FString::Printf(TEXT("Node position '%s' must be a number."), Name), Name,
          Fields->TryGetField(Name), MakeShared<FJsonValueString>(TEXT("number convertible to int32")), Fields, Name);
      return false;
    }
    if (!UeAgentPropertyValueCodec::TryConvertInt32(Number, Value, OutError)) {
      UeAgentAuthoringDiagnostics::LocateAuthoringSource(OutError, *Fields, Name);
      return false;
    }
    return true;
  };
  return ReadAxis(TEXT("x"), X) && ReadAxis(TEXT("y"), Y);
}

void SetMaterialRootPosition(UMaterial& Material, const FIntPoint& Position) {
  Material.Modify();
  Material.EditorX = Position.X;
  Material.EditorY = Position.Y;
  // The editor may already hold the graph. Keep its root in sync so a later
  // native LinkMaterialExpressionsFromGraph cannot replay its old coordinates.
  auto* Graph = Cast<UMaterialGraph>(Material.MaterialGraph);
  if (Graph && Graph->RootNode) {
    auto* Root = Graph->RootNode.Get();
    Root->Modify();
    Root->NodePosX = Position.X;
    Root->NodePosY = Position.Y;
  }
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
  bool bHasCustomInputs = false;
  TSharedPtr<const FJsonObject> Source;
  TSharedPtr<FJsonValue> LayerStack;
};

struct FEdgeSpec {
  FString FromId;
  FString OutputName;
  FString ToId;
  FString InputName;
  TSharedPtr<const FJsonObject> Source;
  bool bContextOnly = false;
};

struct FRootInputSpec {
  EMaterialProperty Property = MP_MAX;
  FString FromId;
  FString OutputName;
  TSharedPtr<const FJsonObject> Source;
  bool bContextOnly = false;
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
    UE::UAI::Contract::FDiagnostic& OutError, const bool bCompactValues) {
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
  Options.AuthorizeProperty = ExpressionPropertyPolicy(Expression).Limits.AuthorizeProperty;
  Options.bCompactValues = bCompactValues;
  if (!UeAgentAuthoringObjectApply::PrepareObject(
          &Expression, ProjectedProperties, Options, *OutPrepared,
          PrepareFailure)) {
    OutError = PrepareFailure.Diagnostic;
    return false;
  }
  if (OutResolved)
    *OutResolved = OutPrepared->GetResolvedNodes();
  UeAgentAuthoringObjectApply::FApplyResult ApplyResult;
  if (!UeAgentAuthoringObjectApply::ApplyPrepared(*OutPrepared, ApplyResult)) {
    OutError = ApplyResult.Diagnostic;
    return false;
  }
  return true;
}
} // namespace

struct FPreparedGraph::FImpl {
  TSharedPtr<const FJsonObject> SourceDocument;
  TMap<FString, UE::UAI::Contract::FSourceLocation> SourceLocations;
  FGraphSources DerivedSources;
  TArray<FExpressionSpec> Expressions;
  TArray<FEdgeSpec> Edges;
  TArray<FRootInputSpec> RootInputs;
  TOptional<FIntPoint> RootPosition;
  TOptional<FIntPoint> OldRootPosition;
  TMap<FGuid, UeAgentAuthoringObjectApply::FPropertyIntent> PropertyIntents;
  FMaterialExpressionCollection OldCollection;
  TArray<TStrongObjectPtr<UObject>> OldObjects;
  TArray<TArray<uint8>> OldObjectStates;
  TArray<FRootInputBackup> OldRootInputs;
  EGraphApplyPolicy Policy = EGraphApplyPolicy::Replace;
  bool bHasConnectionIntent = false;
  TSet<UMaterialExpression*> RetainedExpressions;
  bool bReady = false;
  bool bRollbackCaptured = false;

  bool VerifyAppliedProperties(UObject& Owner, UE::UAI::Contract::FDiagnostic& OutError) const {
    for (const auto& Entry : PropertyIntents) {
      const FExpressionSpec* Spec = Expressions.FindByPredicate([&Entry](const FExpressionSpec& Candidate) { return Candidate.Guid == Entry.Key; });
      ON_SCOPE_EXIT {
        if (Spec && Spec->Source && !OutError.IsEmpty())
          UeAgentAuthoringDiagnostics::LocateFailure(OutError, *Spec->Source, TEXT("node_verify"));
      };
      FExpressionSelector Selector;
      Selector.ExpressionGuid = Entry.Key;
      UMaterialExpression* Expression = ResolveExpressionUnique(Owner, Selector, OutError);
      if (!Expression) return false;
      UeAgentAuthoringObjectApply::FApplyResult Result;
      if (!Entry.Value.VerifyObject(Expression, Result)) {
        OutError = Result.Diagnostic;
        return false;
      }
    }
    return true;
  }

  bool VerifyConnections(UObject& Owner, UE::UAI::Contract::FDiagnostic& OutError,
                         TArray<const FJsonObject*>* FailedSources = nullptr) const {
    TMap<FString, UMaterialExpression*> ById;
    for (const FExpressionSpec& Spec : Expressions) {
      FExpressionSelector Selector;
      Selector.ExpressionGuid = Spec.Guid;
      UMaterialExpression* Expression = ResolveExpressionUnique(Owner, Selector, OutError);
      if (!Expression || Expression->GetClass() != Spec.ExpressionClass) {
        if (OutError.IsEmpty()) OutError = GraphFailure(TEXT("material_expression_intent_not_retained"),
            FString::Printf(TEXT("Node '%s' did not retain the requested expression class."), *Spec.Id),
            TEXT("expression_class"), Expression ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Expression->GetClass()->GetPathName())) : nullptr,
            MakeShared<FJsonValueString>(Spec.ExpressionClass->GetPathName()), Spec.Source.Get(), TEXT("expression_class"));
        UeAgentAuthoringDiagnostics::LocateFailure(OutError, *Spec.Source, TEXT("node_verify"));
        return false;
      }
      ById.Add(Spec.NormalizedId, Expression);
    }
    const auto CheckLink = [&](UMaterialExpression* From, FExpressionInput* Input,
                               const FString& OutputName, const FJsonObject& Source) {
      int32 OutputIndex = INDEX_NONE;
      if (From && Input && ResolveOutputIndexInternal(*From, OutputName, OutputIndex)
          && Input->Expression == From && Input->OutputIndex == OutputIndex) return true;
      auto Actual = MakeShared<FJsonObject>();
      Actual->SetBoolField(TEXT("input_present"), Input != nullptr);
      Actual->SetStringField(TEXT("expression"), Input && Input->Expression ? Input->Expression->GetName() : FString());
      if (Input) Actual->SetNumberField(TEXT("output_index"), Input->OutputIndex);
      auto Expected = MakeShared<FJsonObject>();
      Expected->SetBoolField(TEXT("input_present"), true);
      Expected->SetStringField(TEXT("expression"), From ? From->GetName() : FString());
      Expected->SetStringField(TEXT("output"), OutputName);
      Expected->SetNumberField(TEXT("output_index"), OutputIndex);
      OutError = GraphFailure(TEXT("graph_connection_intent_not_retained"),
          TEXT("The destination input did not retain the requested source expression and output."),
          TEXT("connection"), MakeShared<FJsonValueObject>(Actual), MakeShared<FJsonValueObject>(Expected), &Source);
      UeAgentAuthoringDiagnostics::LocateFailure(OutError, Source, TEXT("edge_verify"));
      return false;
    };
    bool bEquivalent = true;
    for (const FEdgeSpec& Edge : Edges) {
      if (Edge.bContextOnly) continue;
      FExpressionInput* Input = nullptr;
      if (UMaterialExpression* To = ById.FindRef(Edge.ToId)) ResolveInputInternal(*To, Edge.InputName, Input);
      if (!CheckLink(ById.FindRef(Edge.FromId), Input, Edge.OutputName, *Edge.Source)) {
        if (!FailedSources) return false;
        FailedSources->Add(Edge.Source.Get());
        bEquivalent = false;
      }
    }
    for (const FRootInputSpec& Root : RootInputs) {
      if (Root.bContextOnly) continue;
      UMaterial* Material = Cast<UMaterial>(&Owner);
      FExpressionInput* Input = Material ? Material->GetExpressionInputForProperty(Root.Property) : nullptr;
      if (!CheckLink(ById.FindRef(Root.FromId), Input, Root.OutputName, *Root.Source)) {
        if (!FailedSources) return false;
        FailedSources->Add(Root.Source.Get());
        bEquivalent = false;
      }
    }
    return bEquivalent;
  }

  void ResetRollback() {
    OldCollection = FMaterialExpressionCollection();
    OldObjects.Reset();
    OldObjectStates.Reset();
    OldRootInputs.Reset();
    OldRootPosition.Reset();
    bRollbackCaptured = false;
  }

  bool ReplaceGraph(UObject &Owner, const bool bValidation, UE::UAI::Contract::FDiagnostic& OutError, const bool bRefresh = true) {
    OutError.Reset();
    FMaterialExpressionCollection *Collection = GetCollectionInternal(Owner);
    if (!Collection) {
OutError = GraphFailure(TEXT("material_graph_owner_type_invalid"),
              FString::Printf(TEXT("Graph owner '%s' has unsupported class '%s'."), *Owner.GetName(), *Owner.GetClass()->GetName()),
              TEXT("owner_class"), MakeShared<FJsonValueString>(Owner.GetClass()->GetPathName()),
              MakeShared<FJsonValueString>(TEXT("Material or MaterialFunction")));
      return false;
    }

    const FMaterialExpressionCollection Displaced = *Collection;
    const TArray<UMaterialExpression*> DisplacedExpressions = GetGraphExpressions(Displaced);
    if (Policy == EGraphApplyPolicy::PreserveExisting && bHasConnectionIntent) {
      TMap<FString, UMaterialExpression*> ExistingById;
      for (const FExpressionSpec& Spec : Expressions)
        for (UMaterialExpression* Expression : DisplacedExpressions)
          if (Expression && Expression->MaterialExpressionGuid == Spec.Guid && Expression->GetClass() == Spec.ExpressionClass)
            ExistingById.Add(Spec.NormalizedId, Expression);
      TSet<FExpressionInput*> ContextInputs;
      const auto Preserve = [&](UMaterialExpression* From, FExpressionInput* Input, const FString& Output) {
        int32 OutputIndex = INDEX_NONE;
        if (From && Input && Input->Expression == From && ResolveOutputIndexInternal(*From, Output, OutputIndex)
            && Input->OutputIndex == OutputIndex) ContextInputs.Add(Input);
      };
      for (const FEdgeSpec& Edge : Edges) {
        if (!Edge.bContextOnly) continue;
        FExpressionInput* Input = nullptr;
        if (auto* To = ExistingById.FindRef(Edge.ToId)) ResolveInputInternal(*To, Edge.InputName, Input);
        Preserve(ExistingById.FindRef(Edge.FromId), Input, Edge.OutputName);
      }
      if (auto* Material = Cast<UMaterial>(&Owner)) {
        for (const FRootInputSpec& Root : RootInputs)
          if (Root.bContextOnly) Preserve(ExistingById.FindRef(Root.FromId), Material->GetExpressionInputForProperty(Root.Property), Root.OutputName);
        for (const FRootPropertySpec& Root : RootPropertySpecs())
          if (FExpressionInput* Input = Material->GetExpressionInputForProperty(Root.Property); Input && !ContextInputs.Contains(Input))
            Input->Expression = nullptr;
      }
      for (UMaterialExpression* Expression : DisplacedExpressions) {
        if (!Expression) continue;
        for (FExpressionInputIterator It{Expression}; It; ++It)
          if (!ContextInputs.Contains(It.Input)) It->Expression = nullptr;
        if (ExistingById.FindKey(Expression)) continue;
        if (auto* Comment = Cast<UMaterialExpressionComment>(Expression)) {
          Collection->RemoveComment(Comment);
          Collection->RemoveExpression(Comment);
          if (Comment->GraphNode) Comment->GraphNode->DestroyNode();
          Comment->MarkAsGarbage();
        }
        else if (auto* Material = Cast<UMaterial>(&Owner)) UMaterialEditingLibrary::DeleteMaterialExpression(Material, Expression);
        else if (auto* Function = Cast<UMaterialFunction>(&Owner)) UMaterialEditingLibrary::DeleteMaterialExpressionInFunction(Function, Expression);
        // Keep the original object alive until this operation commits or rolls back.
        if (bRollbackCaptured) Expression->ClearGarbage();
      }
    }
    Collection->Empty();
    RetainedExpressions.Reset();
    if (UMaterial *Material = Cast<UMaterial>(&Owner); Material && !bHasConnectionIntent) {
      for (const FRootPropertySpec &RootSpec : RootPropertySpecs()) {
        if (FExpressionInput *Input =
                Material->GetExpressionInputForProperty(RootSpec.Property)) {
          const FName InputName = Input->InputName;
          *Input = FExpressionInput();
          if (Policy == EGraphApplyPolicy::PreserveExisting) Input->InputName = InputName;
        }
      }
    }

    PropertyIntents.Reset();
    TMap<FString, UMaterialExpression *> ById;
    UeAgentAuthoringDiagnostics::FDiagnostic Errors;
    TSet<FString> FailedNodes;
    auto CollectValidationError = [&](const FJsonObject& Source, const TCHAR* Stage) {
      UeAgentAuthoringDiagnostics::LocateFailure(OutError, Source, Stage);
      auto Failure = OutError;
      Failure.bRetrySafe = true;
      Errors.Append(Failure);
      OutError.Reset();
    };
    auto MarkDependentEdge = [&](const FJsonObject& Source) {
      UeAgentAuthoringDiagnostics::MarkUnchecked(Errors, TEXT("edge_prepare"), Source, TEXT("node_prepare_failed"));
    };
    for (FExpressionSpec &Spec : Expressions) {
      ON_SCOPE_EXIT {
        if (!OutError.IsEmpty() && Spec.Source.IsValid()) {
          UeAgentAuthoringDiagnostics::LocateFailure(OutError, *Spec.Source,
              bValidation ? TEXT("node_prepare") : TEXT("node_apply"));
          if (bValidation) {
            FailedNodes.Add(Spec.NormalizedId);
            CollectValidationError(*Spec.Source, TEXT("node_prepare"));
          }
        }
      };
      UMaterialExpression *Expression = nullptr;
      if (Policy == EGraphApplyPolicy::PreserveExisting) {
        for (UMaterialExpression* Existing : DisplacedExpressions) {
          if (Existing && Existing->MaterialExpressionGuid == Spec.Guid
              && Existing->GetClass() == Spec.ExpressionClass) {
            Expression = Existing;
            RetainedExpressions.Add(Existing);
            if (auto* Comment = Cast<UMaterialExpressionComment>(Existing)) Collection->AddComment(Comment);
            else Collection->AddExpression(Existing);
            Existing->Modify();
            Existing->MaterialExpressionEditorX = Spec.X;
            Existing->MaterialExpressionEditorY = Spec.Y;
            break;
          }
        }
      }
      if (!Expression)
        Expression = CreateExpressionInternal(Owner, Spec.ExpressionClass, Spec.X, Spec.Y);
      if (!Expression) {
        OutError = GraphFailure(TEXT("material_expression_create_failed"),
            FString::Printf(TEXT("UE could not create node '%s' of class '%s'."), *Spec.Id, *GetNameSafe(Spec.ExpressionClass)),
            TEXT("expression_created"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Spec.Source.Get());
        if (bValidation) continue;
        return false;
      }
      Expression->MaterialExpressionGuid = Spec.Guid;
      if (Spec.MaterialFunction) {
        UMaterialExpressionMaterialFunctionCall *FunctionCall =
            Cast<UMaterialExpressionMaterialFunctionCall>(Expression);
        if (!FunctionCall ||
            !FunctionCall->SetMaterialFunction(Spec.MaterialFunction)) {
          OutError = GraphFailure(TEXT("material_function_assignment_failed"),
              FString::Printf(TEXT("Node '%s' could not accept material function '%s': %s."), *Spec.Id,
                  *Spec.MaterialFunction->GetName(), FunctionCall ? TEXT("UE rejected the function assignment") : TEXT("the expression is not a function call")),
              TEXT("material_function_path"), MakeShared<FJsonValueString>(Spec.MaterialFunction->GetPathName()),
              MakeShared<FJsonValueString>(TEXT("function accepted by a material function call")), Spec.Source.Get(), TEXT("material_function_path"));
          if (bValidation) continue;
          return false;
        }
        FunctionCall->UpdateFromFunctionResource();
      }
      if (Spec.bHasCustomInputs) {
        UMaterialExpressionCustom *Custom =
            Cast<UMaterialExpressionCustom>(Expression);
        if (!Custom) {
          OutError = GraphFailure(TEXT("material_custom_inputs_class_conflict"),
              FString::Printf(TEXT("Node '%s' declares custom inputs but is not a Custom expression."), *Spec.Id),
              TEXT("expression_class"), MakeShared<FJsonValueString>(Expression->GetClass()->GetPathName()),
              MakeShared<FJsonValueString>(UMaterialExpressionCustom::StaticClass()->GetPathName()), Spec.Source.Get(), TEXT("custom_inputs"));
          if (bValidation) continue;
          return false;
        }
        TArray<FName> CurrentNames;
        for (const FCustomInput& Input : Custom->Inputs)
          CurrentNames.Add(Input.InputName);
        if (CurrentNames != Spec.CustomInputs) {
          TArray<FCustomInput> PreviousInputs;
          if (Policy == EGraphApplyPolicy::PreserveExisting) PreviousInputs = Custom->Inputs;
          Custom->Inputs.Reset();
          for (const FName InputName : Spec.CustomInputs) {
            FCustomInput &Input = Custom->Inputs.AddDefaulted_GetRef();
            const FCustomInput* Previous = PreviousInputs.FindByPredicate([InputName](const FCustomInput& Candidate) { return Candidate.InputName == InputName; });
            if (Previous) Input = *Previous;
            else Input.InputName = InputName;
          }
        }
      }

      if (Spec.LayerStack.IsValid()) {
        auto* LayerExpression = Cast<UMaterialExpressionMaterialAttributeLayers>(Expression);
        if (!LayerExpression) {
          OutError = GraphFailure(TEXT("material_layer_stack_expression_required"),
              FString::Printf(TEXT("Node '%s' declares a layer stack but is not a MaterialAttributeLayers expression."), *Spec.Id),
              TEXT("expression_class"), MakeShared<FJsonValueString>(Expression->GetClass()->GetPathName()),
              MakeShared<FJsonValueString>(UMaterialExpressionMaterialAttributeLayers::StaticClass()->GetPathName()), Spec.Source.Get(), TEXT("layer_stack"));
          if (bValidation) continue;
          return false;
        }
        FMaterialLayersFunctions Layers;
        if (!PrepareLayerStack(LayerExpression->DefaultLayers, Spec.LayerStack, Layers, OutError)) {
          if (bValidation) continue;
          return false;
        }
        LayerExpression->Modify();
        LayerExpression->DefaultLayers = MoveTemp(Layers);
      }
      TUniquePtr<UeAgentAuthoringObjectApply::FPreparedApply> PropertyPlan;
      TArray<UeAgentAuthoringObjectApply::FResolvedNode> Resolved;
      if (!PrepareProperties(*Expression, Spec.Properties, PropertyPlan,
                             bValidation ? &Spec.ResolvedProperties : &Resolved,
                             OutError, true)) {
        if (bValidation) continue;
        return false;
      }
      if (PropertyPlan.IsValid()) {
        UeAgentAuthoringObjectApply::FPropertyIntent Intent;
        UeAgentAuthoringObjectApply::FFailure Failure;
        if (!PropertyPlan->CaptureIntent(Intent, Failure, false)) {
          OutError = Failure.Diagnostic;
          if (bValidation) continue;
          return false;
        }
        PropertyIntents.Add(Spec.Guid, MoveTemp(Intent));
      }
      if (auto* CommentNode = Cast<UMaterialGraphNode_Comment>(Expression->GraphNode)) {
        CommentNode->Modify();
        CommentNode->PostPlacedNewNode();
      }
      if (UMaterialExpressionMaterialFunctionCall *FunctionCall =
              Cast<UMaterialExpressionMaterialFunctionCall>(Expression)) {
        FunctionCall->UpdateFromFunctionResource();
      }
      if (Policy == EGraphApplyPolicy::PreserveExisting && !bHasConnectionIntent) {
        for (FExpressionInputIterator It{Expression}; It; ++It) {
          const FName InputName = It->InputName;
          *It.Input = FExpressionInput();
          It->InputName = InputName;
        }
      }
      ById.Add(Spec.NormalizedId, Expression);
    }

    // Resolve declared/live input aliases before applying the documented Custom
    // input-creation shorthand. An existing alias is never a new native name.
    for (const FEdgeSpec& Edge : Edges) {
      if (Edge.bContextOnly || FailedNodes.Contains(Edge.ToId)) continue;
      auto* Custom = Cast<UMaterialExpressionCustom>(ById.FindRef(Edge.ToId));
      if (!Custom) continue;
      FExpressionInput* ExistingInput = nullptr;
      if (ResolveInputInternal(*Custom, Edge.InputName, ExistingInput)) continue;
      const FString Name = NormalizeExpressionPinName(Edge.InputName);
      if (Name.IsEmpty()) {
        OutError = GraphFailure(TEXT("material_custom_input_name_missing"),
            FString::Printf(TEXT("Connection to custom node '%s' must name a destination input."), *Edge.ToId),
            TEXT("to/input"), MakeShared<FJsonValueString>(Edge.InputName),
            MakeShared<FJsonValueString>(TEXT("existing input alias or non-empty new input name")), Edge.Source.Get(), TEXT("to"));
        UeAgentAuthoringDiagnostics::LocateFailure(OutError, *Edge.Source, TEXT("edge_prepare"));
        if (bValidation) {
          CollectValidationError(*Edge.Source, TEXT("edge_prepare"));
          FailedNodes.Add(Edge.ToId);
          continue;
        }
        return false;
      }
      Custom->Modify();
      Custom->Inputs.AddDefaulted_GetRef().InputName = FName(*Name);
    }

    for (const FEdgeSpec &Edge : Edges) {
      if (Edge.bContextOnly) continue;
      if (bValidation && (FailedNodes.Contains(Edge.FromId) || FailedNodes.Contains(Edge.ToId))) {
        MarkDependentEdge(*Edge.Source);
        continue;
      }
      UMaterialExpression *const *From = ById.Find(Edge.FromId);
      UMaterialExpression *const *To = ById.Find(Edge.ToId);
      if (!From || !*From || !To || !*To ||
          !ConnectExpressions(**From, Edge.OutputName, **To, Edge.InputName,
                              OutError)) {
        if (OutError.IsEmpty())
          OutError = GraphFailure(TEXT("material_edge_expression_not_found"),
              FString::Printf(TEXT("Connection '%s.%s' to '%s.%s' cannot resolve its %s node."),
                  *Edge.FromId, *Edge.OutputName, *Edge.ToId, *Edge.InputName, !From || !*From ? TEXT("source") : TEXT("destination")),
              !From || !*From ? TEXT("from") : TEXT("to"), nullptr, MakeShared<FJsonValueString>(TEXT("live expression")), Edge.Source.Get(),
              !From || !*From ? TEXT("from") : TEXT("to"));
        UeAgentAuthoringDiagnostics::LocateFailure(OutError, *Edge.Source, TEXT("edge_apply"));
        if (bValidation) { CollectValidationError(*Edge.Source, TEXT("edge_prepare")); continue; }
        return false;
      }
    }

    if (!RootInputs.IsEmpty()) {
      UMaterial *Material = Cast<UMaterial>(&Owner);
      if (!Material) {
        OutError = GraphFailure(TEXT("material_function_root_inputs_not_allowed"),
            TEXT("Material root inputs cannot be applied to a material function."), TEXT("root_inputs"),
            MakeShared<FJsonValueNumber>(RootInputs.Num()), MakeShared<FJsonValueNumber>(0), RootInputs[0].Source.Get());
        return false;
      }
      for (const FRootInputSpec &Root : RootInputs) {
        if (Root.bContextOnly) continue;
        if (bValidation && FailedNodes.Contains(Root.FromId)) {
          MarkDependentEdge(*Root.Source);
          continue;
        }
        UMaterialExpression *const *From = ById.Find(Root.FromId);
        if (!From || !*From) {
          OutError = GraphFailure(TEXT("material_root_input_expression_not_found"),
              FString::Printf(TEXT("Material input '%s' refers to missing source node '%s'."), *MaterialPropertyToString(Root.Property), *Root.FromId),
              TEXT("from"), MakeShared<FJsonValueString>(Root.FromId), MakeShared<FJsonValueString>(TEXT("live expression")), Root.Source.Get(), TEXT("from"));
          UeAgentAuthoringDiagnostics::LocateFailure(OutError, *Root.Source, TEXT("edge_apply"));
          if (bValidation) { CollectValidationError(*Root.Source, TEXT("edge_prepare")); continue; }
          return false;
        }
        if (!ConnectExpressionToProperty(**From, Root.OutputName, Root.Property, OutError)) {
          UeAgentAuthoringDiagnostics::LocateFailure(OutError, *Root.Source, TEXT("edge_apply"));
          if (bValidation) { CollectValidationError(*Root.Source, TEXT("edge_prepare")); continue; }
          return false;
        }
      }
    }

    if (!bRollbackCaptured) {
      FMaterialExpressionCollection Retired = Displaced;
      Retired.Expressions.RemoveAll([this](UMaterialExpression* Expression) { return RetainedExpressions.Contains(Expression); });
      Retired.EditorComments.RemoveAll([this](UMaterialExpressionComment* Comment) { return RetainedExpressions.Contains(Comment); });
      RetireCollection(Retired);
    }
    if (!Errors.Code.IsEmpty()) {
      OutError = MoveTemp(Errors);
      return false;
    }
    if (RootPosition.IsSet()) {
      if (auto* Material = Cast<UMaterial>(&Owner))
        SetMaterialRootPosition(*Material, RootPosition.GetValue());
    }
    if (bRefresh) RefreshOwnerInternal(Owner);
    if (!VerifyAppliedProperties(Owner, OutError)) {
      UeAgentAuthoringDiagnostics::ReportConsistencyWarning(TEXT("material_property_intent_not_retained"), OutError, TEXT("material_intent_observation"));
      OutError.Reset();
    }
    // Reuse the same request-derived relation verifier during apply and late
    // readback. Native setters can evict an earlier edge in this same batch.
    if (!VerifyConnections(Owner, OutError)) {
      UeAgentAuthoringDiagnostics::ReportConsistencyWarning(TEXT("graph_connection_intent_not_retained"), OutError, TEXT("material_intent_observation"));
      OutError.Reset();
    }
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

TArray<UMaterialExpression*> GetGraphExpressions(const FMaterialExpressionCollection& Collection) {
  TArray<UMaterialExpression*> Result;
  Result.Reserve(Collection.Expressions.Num() + Collection.EditorComments.Num());
  for (UMaterialExpression* Expression : Collection.Expressions) if (Expression) Result.AddUnique(Expression);
  for (UMaterialExpressionComment* Comment : Collection.EditorComments) if (Comment) Result.AddUnique(Comment);
  return Result;
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

UE::UAI::Contract::FDiagnostic DescribeExpressionPinFailure(UMaterialExpression& Expression,
    const FString& Name, bool bInput, const FString& Field) {
  TSharedRef<FJsonObject> Interface = MakeShared<FJsonObject>();
  UE::UAI::HjsonWorkspace::FWorkspaceError ProjectionError;
  if (!UE::UAI::HjsonWorkspace::TryProjectGraphAuthoringPinInterface(
          BuildExpressionPinInterface(Expression), Interface, ProjectionError)) {
    return ProjectionError.GetDiagnostic();
  }
  TArray<TSharedPtr<FJsonValue>> Choices;
  for (const auto& Pin : Interface->GetArrayField(TEXT("pins")))
    if (Pin->AsObject()->GetStringField(TEXT("direction")) == (bInput ? TEXT("input") : TEXT("output")))
      Choices.Add(Pin);
  return UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("material_expression_pin_unresolved"),
      TEXT("/") + Field, MakeShared<FJsonValueArray>(Choices), MakeShared<FJsonValueString>(Name), TEXT("edge_resolve"));
}

bool ConnectExpressionToProperty(UMaterialExpression &Expression,
                                 const FString &OutputName,
                                 const EMaterialProperty Property,
                                 UE::UAI::Contract::FDiagnostic& OutError) {
  OutError.Reset();
  UMaterial *Material = Cast<UMaterial>(Expression.GetOuter());
  if (!Material) {
    OutError = GraphFailure(TEXT("material_root_input_owner_invalid"),
        FString::Printf(TEXT("Expression '%s' is not owned by a material and cannot connect to material input '%s'."),
            *Expression.GetName(), *MaterialPropertyToString(Property)), TEXT("owner_class"),
        MakeShared<FJsonValueString>(GetNameSafe(Expression.GetOuter() ? Expression.GetOuter()->GetClass() : nullptr)),
        MakeShared<FJsonValueString>(TEXT("Material")));
    return false;
  }
  int32 OutputIndex = INDEX_NONE;
  FExpressionInput *Input = Material->GetExpressionInputForProperty(Property);
  if (!Input) {
    TArray<TSharedPtr<FJsonValue>> Available;
    for (const auto& Spec : RootPropertySpecs()) if (Material->GetExpressionInputForProperty(Spec.Property))
      Available.Add(MakeShared<FJsonValueString>(MaterialPropertyToString(Spec.Property)));
    OutError = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("material_root_input_unavailable"), TEXT("/to"),
        MakeShared<FJsonValueArray>(Available), MakeShared<FJsonValueString>(MaterialPropertyToString(Property)), TEXT("edge_resolve"));
    return false;
  }
  if (!ResolveOutputIndexInternal(Expression, OutputName, OutputIndex)) {
    OutError = DescribeExpressionPinFailure(Expression, OutputName, false, TEXT("from")); return false;
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
  if (Key == TEXT("materialattributes")) {
    OutProperty = MP_MaterialAttributes;
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
                        UE::UAI::Contract::FDiagnostic& OutError) {
  OutError.Reset();
  if (!Selector.HasAny()) {
    OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("material_expression_selector_missing"),
        TEXT("Select a material expression by name, index, or GUID."), TEXT("expression_resolve"));
    OutError.TargetPath = Owner.GetPathName();
    return nullptr;
  }
  const FMaterialExpressionCollection* Collection = GetCollectionInternal(Owner);
  const TArray<UMaterialExpression*> Expressions = Collection ? GetGraphExpressions(*Collection) : TArray<UMaterialExpression*>();
  const auto Choice = [&](UMaterialExpression* Expression) -> TSharedPtr<FJsonValue> {
    const auto Object = MakeShared<FJsonObject>();
    Object->SetStringField(TEXT("expression_name"), Expression->GetName());
    Object->SetNumberField(TEXT("expression_index"), Expressions.IndexOfByKey(Expression));
    Object->SetStringField(TEXT("class"), Expression->GetClass()->GetPathName());
    return MakeShared<FJsonValueObject>(Object);
  };
  const auto SelectorValue = [&](const TCHAR* Field) -> TSharedPtr<FJsonValue> {
    if (FCString::Strcmp(Field, TEXT("expression_index")) == 0) return MakeShared<FJsonValueNumber>(Selector.ExpressionIndex);
    return MakeShared<FJsonValueString>(FCString::Strcmp(Field, TEXT("expression_name")) == 0
        ? Selector.ExpressionName : Selector.ExpressionGuid.ToString());
  };
  const auto SetSelectorFailure = [&](const TCHAR* Code, const TCHAR* Reason, const TCHAR* Field) {
    OutError = UeAgentAuthoringDiagnostics::MakeValueFailure(Code, TEXT("/") + FString(Field),
        MakeShared<FJsonValueString>(TEXT("one uniquely matching expression")), SelectorValue(Field), TEXT("expression_resolve"));
    OutError.PublicMessage = Reason;
    OutError.TargetPath = Owner.GetPathName();
  };
  UMaterialExpression *Resolved = nullptr;
  auto MergeCandidate = [&](UMaterialExpression *Candidate,
                            const TCHAR *SelectorName) -> bool {
    if (!Candidate) {
      SetSelectorFailure(TEXT("material_expression_not_found"), TEXT("No material expression matches this selector."), SelectorName);
      return false;
    }
    if (Resolved && Resolved != Candidate) {
      SetSelectorFailure(TEXT("material_expression_selector_conflict"), TEXT("The supplied selectors resolve to different material expressions."), SelectorName);
      OutError.ExpectedValue = Choice(Resolved);
      OutError.ActualValue = Choice(Candidate);
      return false;
    }
    Resolved = Candidate;
    return true;
  };

  if (Selector.ExpressionGuid.IsValid()) {
    UMaterialExpression *Candidate = nullptr;
    for (UMaterialExpression *Expression : Expressions) {
      if (Expression &&
          Expression->MaterialExpressionGuid == Selector.ExpressionGuid) {
        if (Candidate) {
          SetSelectorFailure(TEXT("material_expression_guid_ambiguous"), TEXT("More than one material expression has this GUID; select by index."), TEXT("expression_guid"));
          OutError.CandidateChoices = MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{Choice(Candidate), Choice(Expression)});
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
          SetSelectorFailure(TEXT("material_expression_name_ambiguous"), TEXT("More than one material expression has this name; select by index."), TEXT("expression_name"));
          OutError.CandidateChoices = MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{Choice(Candidate), Choice(Expression)});
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
                        const FString &ToInputName, UE::UAI::Contract::FDiagnostic& OutError) {
  OutError.Reset();
  int32 OutputIndex = INDEX_NONE;
  FExpressionInput *Input = nullptr;
  UeAgentAuthoringDiagnostics::FDiagnostic Errors;
  if (!ResolveExpressionOutputIndex(FromExpression, FromOutputName, OutputIndex))
    Errors.Append(DescribeExpressionPinFailure(FromExpression, FromOutputName, false, TEXT("from")));
  if (!ResolveExpressionInput(ToExpression, ToInputName, Input) || !Input)
    Errors.Append(DescribeExpressionPinFailure(ToExpression, ToInputName, true, TEXT("to")));
  if (!Errors.Code.IsEmpty()) { OutError = MoveTemp(Errors); return false; }
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
                             UE::UAI::Contract::FDiagnostic& OutError, const FGraphApplyContext& ApplyContext) {
  Impl = MakeUnique<FImpl>();
  Impl->Policy = ApplyContext.Policy;
  Impl->bHasConnectionIntent = ApplyContext.Policy == EGraphApplyPolicy::PreserveExisting
      && ApplyContext.Connections && ApplyContext.Connections->bHasResolvedIntent;
  OutError.Reset();
  if (!GetExpressionCollection(ValidationOwner) || !GraphDocument.IsValid()) {
    OutError = GraphFailure(TEXT("material_graph_prepare_input_invalid"),
        !GraphDocument ? TEXT("A material graph document is required for preparation.") : TEXT("The graph validation owner must be a Material or MaterialFunction."),
        !GraphDocument ? TEXT("graph") : TEXT("owner_class"),
        GraphDocument ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(ValidationOwner.GetClass()->GetPathName())) : nullptr,
        MakeShared<FJsonValueString>(!GraphDocument ? TEXT("object") : TEXT("Material or MaterialFunction")), GraphDocument.Get());
    return false;
  }
  if (!UeAgentAuthoringJsonFields::ValidateOptionalFields(*GraphDocument,
      {{TEXT("nodes"), EJson::Object}, {TEXT("connections"), EJson::Array}}, OutError)) return false;
  Impl->SourceDocument = GraphDocument;
  Impl->SourceLocations = UeAgentAuthoringDiagnostics::FScopedSourceLocations::CaptureProjection(*GraphDocument);
  UeAgentAuthoringDiagnostics::FScopedSourceLocations OriginalSources(GraphDocument.Get(),
      [this](const FString& Pointer) { return UeAgentAuthoringDiagnostics::FScopedSourceLocations::ResolveProjection(Impl->SourceLocations, Pointer); });
  UeAgentAuthoringDiagnostics::FScopedSourceLocations DerivedSources(&Impl->DerivedSources.Objects.Get(),
      [this](const FString& Pointer) { return UeAgentAuthoringDiagnostics::FScopedSourceLocations::ResolveProjection(Impl->DerivedSources.Locations, Pointer); });

  UeAgentAuthoringDiagnostics::FDiagnostic Errors;
  auto CollectError = [&](const FJsonObject& Source, const TCHAR* Stage) {
    UeAgentAuthoringDiagnostics::LocateFailure(OutError, Source, Stage, ContextAssetPath);
    auto Failure = OutError;
    Failure.bRetrySafe = true; // Only the transient validation owner is touched during prepare.
    Errors.Append(Failure);
    OutError.Reset();
    };
  auto ReadCandidate = [&](const TSharedPtr<FJsonObject>& Candidate, const FJsonObject& Source,
      UE::UAI::HjsonWorkspace::DomainAuthoring::FDomainHjsonCandidateSelector& Parsed) {
    UE::UAI::HjsonWorkspace::FWorkspaceError SelectorError;
    if (UE::UAI::HjsonWorkspace::DomainAuthoring::TryReadCandidateSelector(
        Candidate, TEXT("candidate"), Parsed, SelectorError)) return true;
    OutError = SelectorError.GetDiagnostic();
    UeAgentAuthoringDiagnostics::LocateFailure(OutError, Source, TEXT("node_prepare"), ContextAssetPath, TEXT("candidate"));
    auto Failure = OutError;
    // The source attachment locates the candidate object. Keep the offending
    // relative field as well, without inventing a child-field line number.
    Failure.PropertyPath = SelectorError.GetJsonPointer();
    OutError = MoveTemp(Failure);
    return false;
  };

  // The public semantic graph is the canonical import surface.  Keep the
  // native planner's legacy arrays private by adapting nodes/connections here
  // at the typed Material boundary.
  TSharedPtr<FJsonObject> AdaptedGraph;
  TSet<const FJsonObject*> ContextConnections;
  if (GraphDocument->HasField(TEXT("nodes"))) {
    AdaptedGraph = MakeShared<FJsonObject>();
    const TSharedPtr<FJsonObject> *Nodes = nullptr;
    if (!GraphDocument->TryGetObjectField(TEXT("nodes"), Nodes)
        || Nodes == nullptr || !Nodes->IsValid()) {
      OutError = GraphFailure(TEXT("material_graph_nodes_invalid"), TEXT("Material graph nodes must be an object keyed by node alias."),
          TEXT("nodes"), GraphDocument->TryGetField(TEXT("nodes")), MakeShared<FJsonValueString>(TEXT("object")), GraphDocument.Get(), TEXT("nodes"));
      return false;
    }
    TMap<FString, FString> NodeIds;
	TSet<FString> RootNodeAliases;
    TArray<TSharedPtr<FJsonValue>> Expressions;
    for (const TPair<FString, TSharedPtr<FJsonValue>> &Pair : (*Nodes)->Values) {
      if (!Pair.Value.IsValid() || Pair.Value->Type != EJson::Object) {
        OutError = GraphFailure(TEXT("material_graph_node_invalid"), FString::Printf(TEXT("Node '%s' must be an object."), *Pair.Key),
            Pair.Key, Pair.Value, MakeShared<FJsonValueString>(TEXT("object")), Nodes->Get(), Pair.Key);
        CollectError(*GraphDocument, TEXT("node_prepare"));
        continue;
      }
      const TSharedPtr<FJsonObject> Node = Pair.Value->AsObject();
      if (!UeAgentAuthoringJsonFields::ValidateOptionalFields(*Node,
          {{TEXT("id"), EJson::String}, {TEXT("name"), EJson::String},
           {TEXT("candidate_context"), EJson::Object}, {TEXT("pos"), EJson::Object},
           {TEXT("x"), EJson::Number}, {TEXT("y"), EJson::Number}}, OutError)) {
        CollectError(*Node, TEXT("node_prepare"));
        continue;
      }
      ON_SCOPE_EXIT {
        if (!OutError.IsEmpty())
          UeAgentAuthoringDiagnostics::LocateFailure(OutError, *Node, TEXT("node_prepare"));
      };
      FString Id;
      Node->TryGetStringField(TEXT("id"), Id);
      const FGraphExpressionBinding* Binding = ApplyContext.ExpressionsByAlias.Find(Pair.Key);
      if (Binding) Id = Binding->Guid.ToString(EGuidFormats::DigitsWithHyphensLower);
      if (Id.IsEmpty()) Id = Pair.Key;
      NodeIds.Add(Pair.Key, Id);
      FString Name;
      Node->TryGetStringField(TEXT("name"), Name);
      const TSharedPtr<FJsonObject>* Candidate = nullptr;
      const bool bExplicitCandidate = Node->HasField(TEXT("candidate"));
      if (Node->TryGetObjectField(TEXT("candidate"), Candidate) && Candidate && Candidate->IsValid())
        (*Candidate)->TryGetStringField(TEXT("name"), Name);
      if (Id == TEXT("material_root")
          || Name.Equals(TEXT("MaterialOutput"), ESearchCase::IgnoreCase)) {
		if (bExplicitCandidate) {
		  UE::UAI::HjsonWorkspace::DomainAuthoring::FDomainHjsonCandidateSelector Parsed;
		  if (!ReadCandidate(Candidate ? *Candidate : nullptr, *Node, Parsed)) {
		    CollectError(*Node, TEXT("node_prepare"));
		  }
		}
		RootNodeAliases.Add(Pair.Key);
        if (Node->HasField(TEXT("pos")) || Node->HasField(TEXT("x")) || Node->HasField(TEXT("y"))) {
          const auto* Material = Cast<UMaterial>(&ValidationOwner);
          if (!Material) {
            OutError = GraphFailure(TEXT("material_graph_root_position_requires_material"),
                TEXT("Material output position requires a Material graph owner."), TEXT("owner_class"),
                MakeShared<FJsonValueString>(ValidationOwner.GetClass()->GetPathName()),
                MakeShared<FJsonValueString>(TEXT("Material")), Node.Get(), TEXT("pos"));
            CollectError(*Node, TEXT("node_prepare"));
          } else {
            FIntPoint Position(Material->EditorX, Material->EditorY);
            if (!ReadNodePosition(*Node, Position.X, Position.Y, OutError))
              CollectError(*Node, TEXT("node_prepare"));
            else
              Impl->RootPosition = Position;
          }
        }
        continue;
      }
      TSharedRef<FJsonObject> Expression = MakeShared<FJsonObject>();
      Expression->SetStringField(TEXT("id"), Id);
      // Preserve the original authored object until the common selector consumes it.
      if (bExplicitCandidate) Expression->SetField(TEXT("candidate"), Node->TryGetField(TEXT("candidate")));
      if (const auto Source = Node->TryGetField(TEXT("authoring_source")); Source.IsValid())
        Expression->SetField(TEXT("authoring_source"), Source);
      FGuid ExistingGuid;
      if (FGuid::Parse(Id, ExistingGuid)) {
        Expression->SetStringField(TEXT("ue_expression_guid"), Id);
      }
      if (Binding && !bExplicitCandidate) {
        Expression->SetStringField(TEXT("expression_class"), Binding->ClassPath);
        if (!Binding->FunctionPath.IsEmpty()) Expression->SetStringField(TEXT("material_function_path"), Binding->FunctionPath);
      }
      const TSharedPtr<FJsonObject> *Context = nullptr;
      if (!bExplicitCandidate && Node->TryGetObjectField(TEXT("candidate_context"), Context)
          && Context != nullptr && Context->IsValid()) {
        FString Value;
        if ((*Context)->TryGetStringField(TEXT("expression_class"), Value))
          Expression->SetStringField(TEXT("expression_class"), Value);
        if ((*Context)->TryGetStringField(TEXT("material_function_path"), Value))
          Expression->SetStringField(TEXT("material_function_path"), Value);
      }
      if (!Name.IsEmpty()
          && !Expression->HasField(TEXT("expression_class"))
          && !Expression->HasField(TEXT("material_function_path"))) {
        Expression->SetStringField(TEXT("name"), Name);
      }
      const TSharedPtr<FJsonObject>* CanonicalPosition = nullptr;
      if (Node->TryGetObjectField(TEXT("pos"), CanonicalPosition) && CanonicalPosition && CanonicalPosition->IsValid())
        Expression->SetObjectField(TEXT("pos"), *CanonicalPosition);
      double X = 0.0, Y = 0.0;
      if (!Expression->HasField(TEXT("pos")) && (Node->HasField(TEXT("x")) || Node->HasField(TEXT("y")))) {
        Node->TryGetNumberField(TEXT("x"), X);
        Node->TryGetNumberField(TEXT("y"), Y);
        TSharedRef<FJsonObject> Pos = MakeShared<FJsonObject>();
        Pos->SetNumberField(TEXT("x"), X);
        Pos->SetNumberField(TEXT("y"), Y);
        Expression->SetObjectField(TEXT("pos"), Pos);
      }
      for (const TCHAR *Field : {TEXT("custom_inputs"), TEXT("properties"), TEXT("layer_stack")}) {
        if (const TSharedPtr<FJsonValue> *Found = Node->Values.Find(Field))
          Expression->SetField(Field, *Found);
      }
      TMap<FString, FString> Aliases;
      if (!Node->HasField(TEXT("pos"))) {
        Aliases.Add(TEXT("/pos/x"), TEXT("/x"));
        Aliases.Add(TEXT("/pos/y"), TEXT("/y"));
      }
      if (!bExplicitCandidate && Context && Context->IsValid()) {
        Aliases.Add(TEXT("/expression_class"), TEXT("/candidate_context/expression_class"));
        Aliases.Add(TEXT("/material_function_path"), TEXT("/candidate_context/material_function_path"));
      }
      Impl->DerivedSources.Add(Expression, *Node, Aliases);
      Expressions.Add(MakeShared<FJsonValueObject>(Expression));
    }
    AdaptedGraph->SetArrayField(TEXT("expressions"), MoveTemp(Expressions));

    const TArray<TSharedPtr<FJsonValue>> *Connections = nullptr;
    if (GraphDocument->TryGetArrayField(TEXT("connections"), Connections)
        && Connections != nullptr) {
      TArray<TSharedPtr<FJsonValue>> Edges;
      TArray<TSharedPtr<FJsonValue>> RootInputs;
      for (const TSharedPtr<FJsonValue> &Value : *Connections) {
        const TSharedPtr<FJsonObject> Connection = Value.IsValid()
            && Value->Type == EJson::Object ? Value->AsObject() : nullptr;
        FString From, To;
        const bool bFromString = Connection && Connection->TryGetStringField(TEXT("from"), From);
        if (!bFromString || !Connection->TryGetStringField(TEXT("to"), To)) {
          const FString Field = Connection ? bFromString ? TEXT("to") : TEXT("from") : TEXT("connections");
          OutError = GraphFailure(TEXT("material_graph_connection_invalid"),
              TEXT("A graph connection must be an object with string 'from' and 'to' endpoints."),
              Field, Connection ? Connection->TryGetField(Field) : Value, MakeShared<FJsonValueString>(Connection ? TEXT("string endpoint") : TEXT("connection object")),
              Connection ? Connection.Get() : GraphDocument.Get(), Field, FString(),
              Connection ? TEXT("/") + Field : FString::Printf(TEXT("/connections/%d"), Connections->IndexOfByKey(Value)));
          CollectError(Connection.IsValid() ? *Connection : *GraphDocument, TEXT("edge_prepare"));
          continue;
        }
        FString FromAlias, FromPin, ToAlias, ToPin;
        UE::UAI::HjsonWorkspace::FWorkspaceError EndpointError;
        const bool bFromParsed = UE::UAI::HjsonWorkspace::TryParseGraphAuthoringEndpoint(
                From, FromAlias, FromPin, EndpointError);
        if (!bFromParsed || !UE::UAI::HjsonWorkspace::TryParseGraphAuthoringEndpoint(
                To, ToAlias, ToPin, EndpointError)) {
          OutError = EndpointError.GetDiagnostic();
          UeAgentAuthoringDiagnostics::LocateAuthoringSource(OutError, *Connection,
              bFromParsed ? TEXT("to") : TEXT("from"));
          CollectError(*Connection, TEXT("edge_prepare"));
          continue;
        }
        if (!NodeIds.Contains(FromAlias) || !NodeIds.Contains(ToAlias)) {
          const bool bMissingFrom = !NodeIds.Contains(FromAlias);
          const TCHAR* Field = bMissingFrom ? TEXT("from") : TEXT("to");
          OutError = GraphFailure(TEXT("material_graph_edge_target_missing"),
              FString::Printf(TEXT("Connection %s endpoint refers to undeclared node '%s'."), Field,
                  bMissingFrom ? *FromAlias : *ToAlias), Field,
              MakeShared<FJsonValueString>(bMissingFrom ? From : To),
              MakeShared<FJsonValueString>(TEXT("endpoint on a declared node")), Connection.Get(), Field);
          CollectError(*Connection, TEXT("edge_prepare"));
          continue;
        }
        if (RootNodeAliases.Contains(ToAlias)) {
          TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
          Root->SetStringField(TEXT("material_property"), ToPin);
          TSharedRef<FJsonObject> RootFrom = MakeShared<FJsonObject>();
          RootFrom->SetStringField(TEXT("node"), NodeIds.FindRef(FromAlias));
          RootFrom->SetStringField(TEXT("output"), FromPin);
          Root->SetObjectField(TEXT("from"), RootFrom);
          if (Impl->bHasConnectionIntent && ApplyContext.Connections->IsUnchanged(From, To)) ContextConnections.Add(&Root.Get());
          if (const auto Source = Connection->TryGetField(TEXT("authoring_source")); Source.IsValid())
            Root->SetField(TEXT("authoring_source"), Source);
          Impl->DerivedSources.Add(Root, *Connection,
              {{TEXT("/material_property"), TEXT("/to")}});
          RootInputs.Add(MakeShared<FJsonValueObject>(Root));
        } else {
          TSharedRef<FJsonObject> Edge = MakeShared<FJsonObject>();
          TSharedRef<FJsonObject> EdgeFrom = MakeShared<FJsonObject>();
          EdgeFrom->SetStringField(TEXT("node"), NodeIds.FindRef(FromAlias));
          EdgeFrom->SetStringField(TEXT("output"), FromPin);
          TSharedRef<FJsonObject> EdgeTo = MakeShared<FJsonObject>();
          EdgeTo->SetStringField(TEXT("node"), NodeIds.FindRef(ToAlias));
          EdgeTo->SetStringField(TEXT("input"), ToPin);
          Edge->SetObjectField(TEXT("from"), EdgeFrom);
          if (Impl->bHasConnectionIntent && ApplyContext.Connections->IsUnchanged(From, To)) ContextConnections.Add(&Edge.Get());
          Edge->SetObjectField(TEXT("to"), EdgeTo);
          if (const auto Source = Connection->TryGetField(TEXT("authoring_source")); Source.IsValid())
            Edge->SetField(TEXT("authoring_source"), Source);
          Impl->DerivedSources.Add(Edge, *Connection);
          Edges.Add(MakeShared<FJsonValueObject>(Edge));
        }
      }
      AdaptedGraph->SetArrayField(TEXT("edges"), MoveTemp(Edges));
      AdaptedGraph->SetArrayField(TEXT("root_inputs"), MoveTemp(RootInputs));
    }
  }
  const TSharedPtr<const FJsonObject> NativeGraphDocument =
      AdaptedGraph.IsValid()
          ? StaticCastSharedPtr<const FJsonObject>(AdaptedGraph)
          : GraphDocument;

  TArray<TSharedPtr<FJsonValue>> ExpressionStorage;
  bool bExpressionsPresent = false;
  if (!ReadCollectionValues(NativeGraphDocument, TEXT("expressions"), TEXT("id"),
                            ExpressionStorage, bExpressionsPresent, OutError, Impl->DerivedSources)) {
    return false;
  }
  if (!bExpressionsPresent) {
    OutError = GraphFailure(TEXT("material_graph_expressions_missing"), TEXT("The graph document must contain nodes or expressions."),
        TEXT("expressions"), nullptr, MakeShared<FJsonValueString>(TEXT("array or object")), GraphDocument.Get(), TEXT("expressions"));
    return false;
  }
  const TArray<TSharedPtr<FJsonValue>> *ExpressionValues = &ExpressionStorage;
  TSet<FString> SeenIds;
  TSet<FGuid> SeenGuids;
  TSet<FString> InvalidIds;
  for (int32 Index = 0; Index < ExpressionValues->Num(); ++Index) {
    const TSharedPtr<FJsonObject> *ExpressionObject = nullptr;
    if (!(*ExpressionValues)[Index].IsValid() ||
        !(*ExpressionValues)[Index]->TryGetObject(ExpressionObject) ||
        !ExpressionObject || !ExpressionObject->IsValid()) {
      OutError = GraphFailure(TEXT("material_graph_expression_invalid"), FString::Printf(TEXT("Expression at index %d must be an object."), Index),
          TEXT("expressions"), (*ExpressionValues)[Index], MakeShared<FJsonValueString>(TEXT("object")), NativeGraphDocument.Get(),
          TEXT("expressions"), FString(), FString::Printf(TEXT("/expressions/%d"), Index));
      CollectError(*NativeGraphDocument, TEXT("node_prepare"));
      continue;
    }

    FExpressionSpec &Spec = Impl->Expressions.AddDefaulted_GetRef();
    Spec.Source = *ExpressionObject;
    const auto ReadExpression = [&]() -> bool {
    if (!ReadRequiredString(*ExpressionObject, TEXT("id"), Spec.Id, OutError))
      return false;
    Spec.NormalizedId = NormalizeId(Spec.Id);
    if (SeenIds.Contains(Spec.NormalizedId)) {
      OutError = GraphFailure(TEXT("material_graph_expression_id_duplicate"), FString::Printf(TEXT("Expression id '%s' is duplicated after normalization."), *Spec.Id),
          TEXT("id"), MakeShared<FJsonValueString>(Spec.Id), MakeShared<FJsonValueString>(TEXT("unique expression id")), Spec.Source.Get(), TEXT("id"));
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
      OutError = GraphFailure(TEXT("material_graph_expression_guid_invalid"), FString::Printf(TEXT("Node '%s' has an invalid expression GUID."), *Spec.Id),
          TEXT("expression_guid"), MakeShared<FJsonValueString>(GuidText), MakeShared<FJsonValueString>(TEXT("GUID")), Spec.Source.Get(),
          (*ExpressionObject)->HasField(TEXT("ue_expression_guid")) ? TEXT("ue_expression_guid") : TEXT("expression_guid"));
      return false;
    }
    if (!Spec.Guid.IsValid())
      Spec.Guid = FGuid::NewGuid();
    if (SeenGuids.Contains(Spec.Guid)) {
      OutError = GraphFailure(TEXT("material_graph_expression_guid_duplicate"), FString::Printf(TEXT("Node '%s' reuses an expression GUID already assigned in this graph."), *Spec.Id),
          TEXT("expression_guid"), MakeShared<FJsonValueString>(GuidText.IsEmpty() ? Spec.Id : GuidText),
          MakeShared<FJsonValueString>(TEXT("unique expression GUID")), Spec.Source.Get(),
          (*ExpressionObject)->HasField(TEXT("ue_expression_guid")) ? TEXT("ue_expression_guid") : (*ExpressionObject)->HasField(TEXT("expression_guid")) ? TEXT("expression_guid") : TEXT("id"));
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
	if ((*ExpressionObject)->HasField(TEXT("candidate"))
		|| (!ResolveRequest.CatalogName.IsEmpty() && (*ExpressionObject)->HasField(TEXT("select")))) {
		TSharedPtr<FJsonObject> Candidate;
		if ((*ExpressionObject)->HasField(TEXT("candidate"))) {
			const auto Value = (*ExpressionObject)->TryGetField(TEXT("candidate"));
			if (Value.IsValid() && Value->Type == EJson::Object) Candidate = Value->AsObject();
		} else {
			// Internal legacy expression arrays have separate name/select fields.
			Candidate = MakeShared<FJsonObject>();
			Candidate->SetStringField(TEXT("name"), ResolveRequest.CatalogName);
			Candidate->SetField(TEXT("select"), (*ExpressionObject)->TryGetField(TEXT("select")));
		}
		UE::UAI::HjsonWorkspace::DomainAuthoring::FDomainHjsonCandidateSelector Parsed;
		if (!ReadCandidate(Candidate, **ExpressionObject, Parsed)) {
			return false;
		}
		ResolveRequest.CatalogName = MoveTemp(Parsed.Name);
		ResolveRequest.CatalogSelect = MoveTemp(Parsed.Select);
	}
    (*ExpressionObject)
        ->TryGetStringField(TEXT("material_function_path"),
                            ResolveRequest.MaterialFunctionPath);
    ResolveRequest.ContextAssetPath = ContextAssetPath;
    ResolveRequest.ContextGraphName = ContextGraphName;
    FExpressionResolveResult ResolveResult;
    if (!ResolveExpression(ResolveRequest, ResolveResult, OutError)) {
      if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Spec.Id;
      UeAgentAuthoringDiagnostics::LocateFailure(OutError, **ExpressionObject, TEXT("node_prepare"), ContextAssetPath);
      return false;
    }
    Spec.ExpressionClass = ResolveResult.ExpressionClass;
    Spec.MaterialFunction = ResolveResult.MaterialFunction;

    if (!ReadNodePosition(**ExpressionObject, Spec.X, Spec.Y, OutError)) {
      UeAgentAuthoringDiagnostics::LocateFailure(OutError, **ExpressionObject, TEXT("node_prepare"), ContextAssetPath, TEXT("pos")); return false;
    }

    TSharedPtr<FJsonObject> Properties;
    if (!UeAgentDetailsAuthoringService::ReadPropertyPayload(*ExpressionObject, TEXT("properties"), Properties, OutError)) return false;
    if (Properties) Spec.Properties = Properties->GetArrayField(TEXT("properties"));

    Spec.LayerStack = (*ExpressionObject)->TryGetField(TEXT("layer_stack"));
    const TArray<TSharedPtr<FJsonValue>> *CustomInputs = nullptr;
    if ((*ExpressionObject)->HasField(TEXT("custom_inputs")) &&
        !(*ExpressionObject)->TryGetArrayField(TEXT("custom_inputs"), CustomInputs)) {
      OutError = GraphFailure(TEXT("material_custom_inputs_array_invalid"), FString::Printf(TEXT("Node '%s' custom_inputs must be an array."), *Spec.Id),
          TEXT("custom_inputs"), (*ExpressionObject)->TryGetField(TEXT("custom_inputs")), MakeShared<FJsonValueString>(TEXT("array")), Spec.Source.Get(), TEXT("custom_inputs"));
      return false;
    }
    if ((*ExpressionObject)
            ->TryGetArrayField(TEXT("custom_inputs"), CustomInputs) &&
        CustomInputs) {
      Spec.bHasCustomInputs = true;
      TSet<FName> SeenInputNames;
      for (const TSharedPtr<FJsonValue> &Value : *CustomInputs) {
        const FString InputPointer = FString::Printf(TEXT("/custom_inputs/%d"), CustomInputs->IndexOfByKey(Value));
        FString Name;
        if (Value.IsValid() && Value->Type == EJson::String) {
          Name = Value->AsString();
        } else {
          const TSharedPtr<FJsonObject> *InputObject = nullptr;
          if (!Value.IsValid() || !Value->TryGetObject(InputObject) ||
              !InputObject || !InputObject->IsValid() ||
              (!(*InputObject)->TryGetStringField(TEXT("name"), Name) &&
               !(*InputObject)->TryGetStringField(TEXT("input_name"), Name))) {
            OutError = GraphFailure(TEXT("material_custom_input_invalid"), FString::Printf(TEXT("Node '%s' has a custom input that is neither a string nor an object with name/input_name."), *Spec.Id),
                TEXT("custom_inputs"), Value, MakeShared<FJsonValueString>(TEXT("string or named input object")), Spec.Source.Get(), TEXT("custom_inputs"), FString(), InputPointer);
            return false;
          }
        }
        Name = NormalizeExpressionPinName(Name);
        if (Name.IsEmpty() || SeenInputNames.Contains(FName(*Name))) {
          OutError = GraphFailure(TEXT("material_custom_input_name_invalid"), FString::Printf(TEXT("Node '%s' custom input name is %s."), *Spec.Id, Name.IsEmpty() ? TEXT("empty after normalization") : TEXT("duplicated")),
              TEXT("custom_inputs"), MakeShared<FJsonValueString>(Name), MakeShared<FJsonValueString>(TEXT("unique non-empty input name")), Spec.Source.Get(), TEXT("custom_inputs"), FString(), InputPointer);
          return false;
        }
        SeenInputNames.Add(FName(*Name));
        Spec.CustomInputs.Add(FName(*Name));
      }
    }
    return true;
    };
    if (!ReadExpression()) {
      InvalidIds.Add(Spec.NormalizedId);
      CollectError(*Spec.Source, TEXT("node_prepare"));
      UeAgentAuthoringDiagnostics::MarkUnchecked(Errors, TEXT("node_prepare"), *Spec.Source, TEXT("remaining_node_fields_require_valid_identity"));
      Impl->Expressions.Pop(EAllowShrinking::No);
    }
  }

  TArray<TSharedPtr<FJsonValue>> EdgeStorage;
  bool bEdgesPresent = false;
  if (!ReadCollectionValues(NativeGraphDocument, TEXT("edges"), nullptr, EdgeStorage,
                            bEdgesPresent, OutError, Impl->DerivedSources)) {
    CollectError(*NativeGraphDocument, TEXT("edge_prepare"));
  }
  const TArray<TSharedPtr<FJsonValue>> *EdgeValues = &EdgeStorage;
  if (bEdgesPresent) {
    for (int32 Index = 0; Index < EdgeValues->Num(); ++Index) {
      const TSharedPtr<FJsonObject> *EdgeObject = nullptr;
      const int32 PreviousEdgeCount = Impl->Edges.Num();
      const auto ReadEdge = [&]() -> bool {
      TSharedPtr<FJsonObject> From;
      TSharedPtr<FJsonObject> To;
      if (!(*EdgeValues)[Index].IsValid() ||
          !(*EdgeValues)[Index]->TryGetObject(EdgeObject) || !EdgeObject ||
          !EdgeObject->IsValid() ||
          !TryGetObject(*EdgeObject, TEXT("from"), From) ||
          !TryGetObject(*EdgeObject, TEXT("to"), To)) {
        OutError = GraphFailure(TEXT("material_graph_edge_invalid"), FString::Printf(TEXT("Edge %d requires object-valued from and to endpoints."), Index),
            TEXT("edges"), (*EdgeValues)[Index], MakeShared<FJsonValueString>(TEXT("object with from/to objects")), NativeGraphDocument.Get(), TEXT("edges"), FString(), FString::Printf(TEXT("/edges/%d"), Index));
        return false;
      }
      FEdgeSpec &Edge = Impl->Edges.AddDefaulted_GetRef();
      Edge.Source = *EdgeObject;
      Edge.bContextOnly = ContextConnections.Contains(EdgeObject->Get());
      if (!ReadRequiredString(From, TEXT("node"), Edge.FromId, OutError) ||
          !ReadRequiredString(To, TEXT("node"), Edge.ToId, OutError)) {
        UeAgentAuthoringDiagnostics::LocateFailure(OutError, *Edge.Source, TEXT("edge_prepare"));
        return false;
      }
      Edge.FromId = NormalizeId(Edge.FromId);
      Edge.ToId = NormalizeId(Edge.ToId);
      From->TryGetStringField(TEXT("output"), Edge.OutputName);
      To->TryGetStringField(TEXT("input"), Edge.InputName);
      if (InvalidIds.Contains(Edge.FromId) || InvalidIds.Contains(Edge.ToId)) {
        UeAgentAuthoringDiagnostics::MarkUnchecked(Errors, TEXT("edge_prepare"), *Edge.Source, TEXT("node_prepare_failed"));
        Impl->Edges.Pop(EAllowShrinking::No);
        return true;
      }
      if (!SeenIds.Contains(Edge.FromId) || !SeenIds.Contains(Edge.ToId)) {
        const bool bMissingFrom = !SeenIds.Contains(Edge.FromId);
        OutError = GraphFailure(TEXT("material_graph_edge_target_missing"), FString::Printf(TEXT("Edge %d refers to missing %s node '%s'."),
            Index, bMissingFrom ? TEXT("source") : TEXT("destination"), bMissingFrom ? *Edge.FromId : *Edge.ToId),
            bMissingFrom ? TEXT("from") : TEXT("to"), MakeShared<FJsonValueString>(bMissingFrom ? Edge.FromId : Edge.ToId),
            MakeShared<FJsonValueString>(TEXT("node declared in this graph")), Edge.Source.Get(), bMissingFrom ? TEXT("from") : TEXT("to"));
        UeAgentAuthoringDiagnostics::LocateFailure(OutError, *Edge.Source, TEXT("edge_prepare"));
        return false;
      }
      return true;
      };
      if (!ReadEdge()) {
        CollectError(EdgeObject && EdgeObject->IsValid() ? **EdgeObject : *NativeGraphDocument, TEXT("edge_prepare"));
        Impl->Edges.SetNum(PreviousEdgeCount, EAllowShrinking::No);
      }
    }
  }
  TArray<TSharedPtr<FJsonValue>> RootStorage;
  bool bRootInputsPresent = false;
  if (!ReadCollectionValues(NativeGraphDocument, TEXT("root_inputs"),
                            TEXT("material_property"), RootStorage,
                            bRootInputsPresent, OutError, Impl->DerivedSources)) {
    CollectError(*NativeGraphDocument, TEXT("edge_prepare"));
  }
  const TArray<TSharedPtr<FJsonValue>> *RootValues = &RootStorage;
  if (bRootInputsPresent) {
    if (!Cast<UMaterial>(&ValidationOwner) && !RootValues->IsEmpty()) {
      OutError = GraphFailure(TEXT("material_function_root_inputs_not_allowed"), TEXT("Material functions cannot declare material root inputs."),
          TEXT("root_inputs"), MakeShared<FJsonValueNumber>(RootValues->Num()), MakeShared<FJsonValueNumber>(0), NativeGraphDocument.Get(), TEXT("root_inputs"));
      CollectError(*NativeGraphDocument, TEXT("edge_prepare"));
    }
    TSet<EMaterialProperty> SeenProperties;
    for (int32 Index = 0; Index < RootValues->Num(); ++Index) {
      const TSharedPtr<FJsonObject> *RootObject = nullptr;
      const int32 PreviousRootCount = Impl->RootInputs.Num();
      const auto ReadRoot = [&]() -> bool {
      TSharedPtr<FJsonObject> From;
      FString PropertyName;
      if (!(*RootValues)[Index].IsValid() ||
          !(*RootValues)[Index]->TryGetObject(RootObject) || !RootObject ||
          !RootObject->IsValid() ||
          !ReadRequiredString(*RootObject, TEXT("material_property"),
                              PropertyName, OutError) ||
          !TryGetObject(*RootObject, TEXT("from"), From)) {
        if (OutError.IsEmpty()) OutError = GraphFailure(TEXT("material_graph_root_input_invalid"),
            FString::Printf(TEXT("Root input %d requires an object with material_property and a from object."), Index),
            TEXT("root_inputs"), (*RootValues)[Index], MakeShared<FJsonValueString>(TEXT("root input object")),
            NativeGraphDocument.Get(), TEXT("root_inputs"), FString(), FString::Printf(TEXT("/root_inputs/%d"), Index));
        return false;
      }
      FRootInputSpec &Root = Impl->RootInputs.AddDefaulted_GetRef();
      Root.Source = *RootObject;
      Root.bContextOnly = ContextConnections.Contains(RootObject->Get());
      if (!ParseMaterialProperty(PropertyName, Root.Property) ||
          !ReadRequiredString(From, TEXT("node"), Root.FromId, OutError)) {
        if (OutError.IsEmpty())
          OutError = GraphFailure(TEXT("material_graph_root_property_invalid"),
              FString::Printf(TEXT("Material root property '%s' is not supported."), *PropertyName),
              TEXT("material_property"), MakeShared<FJsonValueString>(PropertyName),
              MakeShared<FJsonValueString>(TEXT("supported material property name")), Root.Source.Get(), TEXT("material_property"));
        UeAgentAuthoringDiagnostics::LocateFailure(OutError, *Root.Source, TEXT("edge_prepare"));
        return false;
      }
      Root.FromId = NormalizeId(Root.FromId);
      From->TryGetStringField(TEXT("output"), Root.OutputName);
      if (InvalidIds.Contains(Root.FromId)) {
        UeAgentAuthoringDiagnostics::MarkUnchecked(Errors, TEXT("edge_prepare"), *Root.Source, TEXT("node_prepare_failed"));
        Impl->RootInputs.Pop(EAllowShrinking::No);
        return true;
      }
      if (!SeenIds.Contains(Root.FromId) ||
          SeenProperties.Contains(Root.Property)) {
        const bool bMissingNode = !SeenIds.Contains(Root.FromId);
        OutError = GraphFailure(TEXT("material_graph_root_input_conflict"),
            bMissingNode ? FString::Printf(TEXT("Root input '%s' refers to undeclared node '%s'."), *PropertyName, *Root.FromId)
                         : FString::Printf(TEXT("Material root input '%s' is assigned more than once."), *PropertyName),
            bMissingNode ? TEXT("from") : TEXT("material_property"), MakeShared<FJsonValueString>(bMissingNode ? Root.FromId : PropertyName),
            MakeShared<FJsonValueString>(bMissingNode ? TEXT("node declared in this graph") : TEXT("one assignment per material property")),
            Root.Source.Get(), bMissingNode ? TEXT("from") : TEXT("material_property"));
        UeAgentAuthoringDiagnostics::LocateFailure(OutError, *Root.Source, TEXT("edge_prepare"));
        return false;
      }
      SeenProperties.Add(Root.Property);
      return true;
      };
      if (!ReadRoot()) {
        const TArray<TSharedPtr<FJsonValue>>* OriginalRootArray = nullptr;
        if (NativeGraphDocument->TryGetArrayField(TEXT("root_inputs"), OriginalRootArray)) {
          const FString RootPath = FString::Printf(TEXT("root_inputs[%d]"), Index);
          OutError.PropertyPath = OutError.PropertyPath.IsEmpty()
              ? RootPath : RootPath + TEXT(".") + OutError.PropertyPath;
        }
        CollectError(RootObject && RootObject->IsValid() ? **RootObject : *NativeGraphDocument, TEXT("edge_prepare"));
        Impl->RootInputs.SetNum(PreviousRootCount, EAllowShrinking::No);
      }
    }
  }

  if (!Impl->ReplaceGraph(ValidationOwner, true, OutError, Errors.Code.IsEmpty()))
    CollectError(*NativeGraphDocument, TEXT("graph_validation"));
  if (!Errors.Code.IsEmpty()) {
    OutError = MoveTemp(Errors);
    return false;
  }
  Impl->bReady = true;
  return true;
}

bool FPreparedGraph::CaptureRollback(UObject &Owner, UE::UAI::Contract::FDiagnostic& OutError) {
  OutError.Reset();
  if (!Impl.IsValid() || !Impl->bReady) {
    OutError = GraphFailure(TEXT("material_graph_plan_not_ready"),
        TEXT("The material graph plan has not completed preparation."), TEXT("plan_ready"),
        MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
    return false;
  }
  FMaterialExpressionCollection *Collection = GetCollectionInternal(Owner);
  if (!Collection) {
OutError = GraphFailure(TEXT("material_graph_owner_type_invalid"),
            FString::Printf(TEXT("Graph owner '%s' has unsupported class '%s'."), *Owner.GetName(), *Owner.GetClass()->GetName()),
            TEXT("owner_class"), MakeShared<FJsonValueString>(Owner.GetClass()->GetPathName()),
            MakeShared<FJsonValueString>(TEXT("Material or MaterialFunction")));
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
  TArray<UObject*> OwnedChildren;
  const int32 ExpressionCount = Impl->OldObjects.Num();
  for (int32 Index = 0; Index < ExpressionCount; ++Index) {
    TArray<UObject*> Children;
    GetObjectsWithOuter(Impl->OldObjects[Index].Get(), Children, true);
    OwnedChildren.Append(Children);
  }
  for (UObject* Child : OwnedChildren)
    if (!Impl->OldObjects.ContainsByPredicate([Child](const auto& Object) { return Object.Get() == Child; }))
      Impl->OldObjects.Emplace(Child);
  Impl->OldObjectStates.SetNum(Impl->OldObjects.Num());
  for (int32 Index = 0; Index < Impl->OldObjects.Num(); ++Index) {
    FObjectWriter Writer(Impl->OldObjects[Index].Get(), Impl->OldObjectStates[Index], false, false, false, PPF_DuplicateVerbatim);
    if (Writer.IsError()) {
      OutError = GraphFailure(TEXT("material_graph_checkpoint_failed"),
          FString::Printf(TEXT("Could not capture rollback state for graph object '%s' at index %d."), *Impl->OldObjects[Index]->GetName(), Index),
          TEXT("checkpoint_written"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
      OutError.InternalContext = Impl->OldObjects[Index]->GetPathName();
      Impl->ResetRollback(); return false;
    }
  }
  if (UMaterial *Material = Cast<UMaterial>(&Owner)) {
    Impl->OldRootPosition = FIntPoint(Material->EditorX, Material->EditorY);
    for (int32 Index = 0; Index < MP_MAX; ++Index) {
      const auto Property = static_cast<EMaterialProperty>(Index);
      if (FExpressionInput *Input =
              Material->GetExpressionInputForProperty(Property)) {
        FRootInputBackup &Backup = Impl->OldRootInputs.AddDefaulted_GetRef();
        Backup.Property = Property;
        Backup.Input = *Input;
      }
    }
  }
  Impl->bRollbackCaptured = true;
  return true;
}

bool FPreparedGraph::ApplyTo(UObject &Owner, UE::UAI::Contract::FDiagnostic& OutError) {
  if (!Impl.IsValid() || !Impl->bReady) {
    OutError = GraphFailure(TEXT("material_graph_plan_not_ready"),
        TEXT("The material graph plan has not completed preparation."), TEXT("plan_ready"),
        MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
    return false;
  }
  UeAgentAuthoringDiagnostics::FScopedSourceLocations OriginalSources(Impl->SourceDocument.Get(),
      [this](const FString& Pointer) { return UeAgentAuthoringDiagnostics::FScopedSourceLocations::ResolveProjection(Impl->SourceLocations, Pointer); });
  UeAgentAuthoringDiagnostics::FScopedSourceLocations DerivedSources(&Impl->DerivedSources.Objects.Get(),
      [this](const FString& Pointer) { return UeAgentAuthoringDiagnostics::FScopedSourceLocations::ResolveProjection(Impl->DerivedSources.Locations, Pointer); });
  return Impl->ReplaceGraph(Owner, false, OutError);
}

bool FPreparedGraph::VerifyIntent(UObject& Owner, UE::UAI::Contract::FDiagnostic& OutError) const {
  if (!Impl.IsValid() || !Impl->bReady) {
    OutError = GraphFailure(TEXT("material_graph_plan_not_ready"),
        TEXT("The material graph plan has not completed preparation."), TEXT("plan_ready"),
        MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
    return false;
  }
  UeAgentAuthoringDiagnostics::FScopedSourceLocations OriginalSources(Impl->SourceDocument.Get(),
      [this](const FString& Pointer) { return UeAgentAuthoringDiagnostics::FScopedSourceLocations::ResolveProjection(Impl->SourceLocations, Pointer); });
  UeAgentAuthoringDiagnostics::FScopedSourceLocations DerivedSources(&Impl->DerivedSources.Objects.Get(),
      [this](const FString& Pointer) { return UeAgentAuthoringDiagnostics::FScopedSourceLocations::ResolveProjection(Impl->DerivedSources.Locations, Pointer); });
  return Impl->VerifyAppliedProperties(Owner, OutError)
      && Impl->VerifyConnections(Owner, OutError);
}

bool FPreparedGraph::AppendPropertyReadRequests(
    UObject &Owner, const FString &AssetPath,
    TArray<UeAgentAuthoringState::FReadRequest> &OutRequests,
    FOperationFailure &OutFailure) const {
  if (!Impl.IsValid() || !Impl->bReady) {
    OutFailure.Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(TEXT("material_graph_plan_not_ready"), TEXT("The material graph plan has not been prepared."), TEXT("readback"));
    return false;
  }
  UeAgentAuthoringDiagnostics::FScopedSourceLocations OriginalSources(Impl->SourceDocument.Get(),
      [this](const FString& Pointer) { return UeAgentAuthoringDiagnostics::FScopedSourceLocations::ResolveProjection(Impl->SourceLocations, Pointer); });
  UeAgentAuthoringDiagnostics::FScopedSourceLocations DerivedSources(&Impl->DerivedSources.Objects.Get(),
      [this](const FString& Pointer) { return UeAgentAuthoringDiagnostics::FScopedSourceLocations::ResolveProjection(Impl->DerivedSources.Locations, Pointer); });
  UE::UAI::Contract::FDiagnostic IntentError;
  if (!VerifyIntent(Owner, IntentError)) {
    if (IntentError.Stage.IsEmpty()) IntentError.Stage = TEXT("material_readback");
    UeAgentAuthoringDiagnostics::ReportConsistencyWarning(IntentError);
  }
  for (const FExpressionSpec &Spec : Impl->Expressions) {
    if (Spec.ResolvedProperties.IsEmpty())
      continue;
    FExpressionSelector Selector;
    Selector.ExpressionGuid = Spec.Guid;
    UE::UAI::Contract::FDiagnostic ResolveError;
    UMaterialExpression *Expression =
        ResolveExpressionUnique(Owner, Selector, ResolveError);
    if (!Expression) {
      OutFailure.Diagnostic = ResolveError;
      UeAgentAuthoringDiagnostics::LocateFailure(OutFailure.Diagnostic, *Spec.Source, TEXT("readback"));
      if (OutFailure.Diagnostic.SubjectId.IsEmpty()) OutFailure.Diagnostic.SubjectId = Spec.Id;
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

bool FPreparedGraph::Rollback(UObject &Owner, UE::UAI::Contract::FDiagnostic& OutError) {
  OutError.Reset();
  if (!Impl.IsValid() || !Impl->bRollbackCaptured)
    return true;
  FMaterialExpressionCollection *Collection = GetExpressionCollection(Owner);
  if (!Collection) {
OutError = GraphFailure(TEXT("material_graph_owner_type_invalid"),
            FString::Printf(TEXT("Graph owner '%s' has unsupported class '%s'."), *Owner.GetName(), *Owner.GetClass()->GetName()),
            TEXT("owner_class"), MakeShared<FJsonValueString>(Owner.GetClass()->GetPathName()),
            MakeShared<FJsonValueString>(TEXT("Material or MaterialFunction")));
    return false;
  }
  FMaterialExpressionCollection Created = *Collection;
  Created.Expressions.RemoveAll([this](UMaterialExpression* Expression) { return Impl->OldCollection.Expressions.Contains(Expression); });
  Created.EditorComments.RemoveAll([this](UMaterialExpressionComment* Comment) { return Impl->OldCollection.EditorComments.Contains(Comment); });
  RetireCollection(Created);
  for (const auto& Object : Impl->OldObjects) Object->ClearGarbage();
  for (int32 Index = 0; Index < Impl->OldObjects.Num(); ++Index) {
    FObjectReader Reader(Impl->OldObjectStates[Index]);
    Impl->OldObjects[Index]->Serialize(Reader);
    if (Reader.IsError()) {
      OutError = GraphFailure(TEXT("material_graph_checkpoint_restore_failed"),
          FString::Printf(TEXT("Could not restore rollback state for graph object '%s' at index %d."), *Impl->OldObjects[Index]->GetName(), Index),
          TEXT("checkpoint_restored"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
      OutError.InternalContext = Impl->OldObjects[Index]->GetPathName();
      return false;
    }
  }
  *Collection = Impl->OldCollection;
  if (UMaterial *Material = Cast<UMaterial>(&Owner)) {
    if (Impl->OldRootPosition.IsSet())
      SetMaterialRootPosition(*Material, Impl->OldRootPosition.GetValue());
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
  FMaterialExpressionCollection Retired = Impl->OldCollection;
  Retired.Expressions.RemoveAll([this](UMaterialExpression* Expression) { return Impl->RetainedExpressions.Contains(Expression); });
  Retired.EditorComments.RemoveAll([this](UMaterialExpressionComment* Comment) { return Impl->RetainedExpressions.Contains(Comment); });
  RetireCollection(Retired);
  Impl->ResetRollback();
}

bool FPreparedGraph::IsReady() const { return Impl.IsValid() && Impl->bReady; }
bool FPreparedGraph::PreservesExisting() const { return Impl.IsValid() && Impl->Policy == EGraphApplyPolicy::PreserveExisting; }

void FPreparedGraph::ReleaseForPersistentReadback() {
  if (!Impl.IsValid()) return;
  Commit();
  Impl->RetainedExpressions.Reset();
}

int32 FPreparedGraph::GetExpressionCount() const {
  return Impl.IsValid() ? Impl->Expressions.Num() : 0;
}

int32 FPreparedGraph::GetEdgeCount() const {
  return Impl.IsValid() ? Impl->Edges.Num() : 0;
}

int32 FPreparedGraph::GetRootInputCount() const {
  return Impl.IsValid() ? Impl->RootInputs.Num() : 0;
}

TSharedRef<FJsonObject> BuildExpressionPinInterface(
    UMaterialExpression &Expression) {
  TSharedRef<FJsonObject> Interface = MakeShared<FJsonObject>();
  TArray<TSharedPtr<FJsonValue>> Pins;
  if (Expression.IsA<UMaterialExpressionComment>()) {
    Interface->SetArrayField(TEXT("pins"), MoveTemp(Pins));
    return Interface;
  }
  for (const auto& Binding : CaptureExpressionPins(Expression)) {
    TSharedRef<FJsonObject> Pin = MakeShared<FJsonObject>();
    Pin->SetStringField(TEXT("name"), Binding.Selector.SemanticName);
    Pin->SetStringField(TEXT("direction"), Binding.Selector.Direction ==
        UE::UAI::HjsonWorkspace::EGraphAuthoringEndpointDirection::Input ? TEXT("input") : TEXT("output"));
    Pin->SetNumberField(TEXT("occurrence"), Binding.Selector.Occurrence);
    Pins.Add(MakeShared<FJsonValueObject>(Pin));
  }
  Interface->SetArrayField(TEXT("pins"), MoveTemp(Pins));
  return Interface;
}

bool BuildGraphState(const UObject &Owner, TSharedPtr<FJsonObject> &OutState,
                     UE::UAI::Contract::FDiagnostic& OutError) {
  OutState = MakeShared<FJsonObject>();
  OutError.Reset();
  const FMaterialExpressionCollection *Collection =
      GetCollectionInternal(Owner);
  if (!Collection) {
OutError = GraphFailure(TEXT("material_graph_owner_type_invalid"),
            FString::Printf(TEXT("Graph owner '%s' has unsupported class '%s'."), *Owner.GetName(), *Owner.GetClass()->GetName()),
            TEXT("owner_class"), MakeShared<FJsonValueString>(Owner.GetClass()->GetPathName()),
            MakeShared<FJsonValueString>(TEXT("Material or MaterialFunction")));
    return false;
  }

  TArray<UMaterialExpression *> Expressions;
  TMap<const UMaterialExpression *, FString> GuidByExpression;
  for (UMaterialExpression *Expression : GetGraphExpressions(*Collection)) {
    if (!Expression)
      continue;
    if (!Expression->MaterialExpressionGuid.IsValid()) {
      OutError = GraphFailure(TEXT("material_expression_guid_missing"),
          FString::Printf(TEXT("Expression '%s' has no valid GUID for graph export."), *Expression->GetName()),
          TEXT("expression_guid"), nullptr, MakeShared<FJsonValueString>(TEXT("valid expression GUID")));
      OutError.TargetPath = Owner.GetPathName();
      OutError.InternalContext = Expression->GetPathName();
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
        if (Input.InputName.IsNone()) continue;
        Inputs.Add(MakeShared<FJsonValueString>(Input.InputName.ToString()));
      }
      State->SetArrayField(TEXT("custom_inputs"), Inputs);
    }
    if (auto* LayerExpression = Cast<UMaterialExpressionMaterialAttributeLayers>(Expression)) {
      TSharedPtr<FJsonValue> Layers;
      if (!CaptureLayerStack(*LayerExpression, LayerExpression->DefaultLayers, Layers, OutError)) return false;
      State->SetField(TEXT("layer_stack"), Layers);
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
      const int32 OutputOccurrence = PinOccurrenceForIndex(*It->Expression, It->OutputIndex,
          UE::UAI::HjsonWorkspace::EGraphAuthoringEndpointDirection::Output);
      const int32 InputOccurrence = PinOccurrenceForIndex(*ToExpression, It.Index,
          UE::UAI::HjsonWorkspace::EGraphAuthoringEndpointDirection::Input);
      TSharedRef<FJsonObject> State = MakeShared<FJsonObject>();
      State->SetStringField(TEXT("from_guid"), *FromGuid);
      State->SetStringField(TEXT("output"), OutputName);
      State->SetNumberField(TEXT("output_occurrence"), OutputOccurrence);
      State->SetStringField(TEXT("to_guid"), *ToGuid);
      State->SetStringField(TEXT("input"), InputName);
      State->SetNumberField(TEXT("input_occurrence"), InputOccurrence);
      EdgeStates.Add({*FromGuid + TEXT("|") + OutputName + TEXT("|") + *ToGuid +
                          TEXT("|") + InputName + FString::Printf(TEXT("|%d|%d"), OutputOccurrence, InputOccurrence),
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
    auto Position = MakeShared<FJsonObject>();
    Position->SetNumberField(TEXT("x"), Material->EditorX);
    Position->SetNumberField(TEXT("y"), Material->EditorY);
    OutState->SetObjectField(TEXT("root_position"), Position);
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
      State->SetNumberField(TEXT("output_occurrence"), PinOccurrenceForIndex(
          *Input->Expression, Input->OutputIndex,
          UE::UAI::HjsonWorkspace::EGraphAuthoringEndpointDirection::Output));
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

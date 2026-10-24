// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/Niagara/Authoring/UeAgentNiagaraAuthoringStateAccessors.h"
#include "Domains/Niagara/UeAgentNiagaraRendererBindings.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Domains/Niagara/Authoring/UeAgentNiagaraAuthoringDescriptors.h"
#include "Domains/Niagara/Authoring/UeAgentNiagaraStageModuleAuthoring.h"
#include "Domains/Niagara/Authoring/UeAgentNiagaraStandaloneModuleAuthoring.h"
#include "Domains/Niagara/Authoring/UeAgentNiagaraUserParameterAuthoring.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "NiagaraDataInterface.h"
#include "NiagaraEditorSettings.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterFactoryNew.h"
#include "NiagaraNode.h"
#include "NiagaraNodeAssignment.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraNodeInput.h"
#include "NiagaraRendererProperties.h"
#include "NiagaraScript.h"
#include "NiagaraScriptSource.h"
#include "NiagaraSystem.h"
#include "NiagaraSystemFactoryNew.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringNativeDefaults.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringEncodedTypes.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringState.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringTypeDescriptors.h"
#include "Utilities/AuthoringCore/UeAgentDetailsAuthoringService.h"
#include "Utilities/UeAgentPropertyValueCodec.h"
#include "Utilities/UeAgentStructuredHjsonFormatter.h"

namespace UeAgentNiagaraAuthoring {
namespace {

using namespace UeAgentNiagaraStageModuleAuthoring;

UE::UAI::Contract::FDiagnostic StateValueFailure(const TCHAR* Code, const FString& Reason,
    const FString& Field, const TSharedPtr<FJsonValue>& Actual,
    const TSharedPtr<FJsonValue>& Expected, const bool bNativeResult = false) {
  auto Error = UE::UAI::Contract::FDiagnostic::Failure(Code, Reason, TEXT("state_capture"));
  Error.PropertyPath = Field;
  Error.ActualValue = Actual;
  Error.ActualValuePresent = Actual.IsValid();
  Error.ExpectedValue = Expected;
  Error.ExpectedValuePresent = Expected.IsValid();
  if (bNativeResult) Error.SourceKind = TEXT("native_result");
  return Error;
}

void SetInputPatchState(const TSharedRef<FJsonObject>& State, const TSharedRef<FJsonObject>& Inputs) {
  // All native input consumers apply supplied entries only. Preserve that
  // contract through aggregate capture and original-intent verification.
  const TSharedPtr<FJsonValue> Value = MakeShared<FJsonValueObject>(Inputs);
  UeAgentAuthoringTypeDescriptors::FDescriptor Type;
  Type.Kind = TEXT("object");
  const auto Definition = MakeShared<FJsonObject>();
  Definition->SetStringField(TEXT("omitted_patch_field_policy"), TEXT("preserve_current"));
  UeAgentAuthoringEncodedTypes::FScope::Record(Value, Type, Definition);
  UeAgentAuthoringEncodedTypes::FScope::SetNativeField(State, TEXT("inputs"), Value,
      UeAgentAuthoringEncodedTypes::EFieldEmission::NonEmptyObject);
}

TSharedRef<FJsonObject> BuildFunctionInputDefinition(UNiagaraNodeFunctionCall &Node, const UObject* OwningAsset) {
  const auto InputsDefinition = MakeShared<FJsonObject>();
  InputsDefinition->SetObjectField(TEXT("fields"), BuildModuleInputSchema(Node, OwningAsset));
  const auto Fields = MakeShared<FJsonObject>();
  Fields->SetObjectField(TEXT("inputs"), InputsDefinition);
  const auto Definition = MakeShared<FJsonObject>();
  Definition->SetObjectField(TEXT("fields"), Fields);
  return Definition;
}

UObject *
ResolveRequestObject(const UeAgentAuthoringState::FReadRequest &Request,
                     UE::UAI::Contract::FDiagnostic& OutError) {
  UObject *Object =
      UeAgentAuthoringState::ResolveRequestRootObject(Request, OutError);
  if (!IsValid(Object))
    return nullptr;
  return Object;
}

bool BuildStageTarget(const UeAgentAuthoringState::FReadRequest &Request,
                      FStageTarget &OutTarget, UE::UAI::Contract::FDiagnostic& OutError) {
  OutTarget = FStageTarget();
  OutTarget.AssetPath = Request.TargetIdentity.GetSelector(TEXT("asset_path"));
  OutTarget.EmitterName =
      Request.TargetIdentity.GetSelector(TEXT("emitter_name"));
  const FString EmitterHandleIdText =
      Request.TargetIdentity.GetSelector(TEXT("emitter_handle_id"));
  if (!EmitterHandleIdText.IsEmpty()) {
    if (!FGuid::Parse(EmitterHandleIdText, OutTarget.EmitterHandleId)) {
      OutError = StateValueFailure(TEXT("niagara_emitter_handle_id_invalid"),
          TEXT("The emitter handle selector must be a valid GUID."), TEXT("emitter_handle_id"),
          MakeShared<FJsonValueString>(EmitterHandleIdText), MakeShared<FJsonValueString>(TEXT("GUID")));
      return false;
    }
    OutTarget.bEmitterHandleIdExplicit = true;
  }
  const FString UsageText = Request.TargetIdentity.GetSelector(TEXT("usage"));
  if (!UsageText.IsEmpty()) {
    if (!TryParseScriptUsage(UsageText, OutTarget.Usage)) {
      OutError = StateValueFailure(TEXT("niagara_script_usage_invalid"),
          TEXT("The script usage selector is not a recognized ENiagaraScriptUsage name."), TEXT("usage"),
          MakeShared<FJsonValueString>(UsageText), MakeShared<FJsonValueString>(TEXT("ENiagaraScriptUsage enum name")));
      return false;
    }
    OutTarget.bUsageExplicit = true;
  }
  const FString UsageIdText =
      Request.TargetIdentity.GetSelector(TEXT("usage_id"));
  if (!UsageIdText.IsEmpty()) {
    if (!FGuid::Parse(UsageIdText, OutTarget.UsageId)) {
      OutError = StateValueFailure(TEXT("niagara_script_usage_id_invalid"),
          TEXT("The stage usage identifier must be a valid GUID."), TEXT("usage_id"),
          MakeShared<FJsonValueString>(UsageIdText), MakeShared<FJsonValueString>(TEXT("GUID")));
      return false;
    }
    OutTarget.bUsageIdExplicit = true;
  }
  if (OutTarget.AssetPath.IsEmpty()) {
    OutError = StateValueFailure(TEXT("niagara_stage_asset_path_missing"),
        TEXT("The Niagara stage read request requires an asset path selector."), TEXT("asset_path"),
        MakeShared<FJsonValueString>(OutTarget.AssetPath), MakeShared<FJsonValueString>(TEXT("non-empty asset path")));
    return false;
  }
  return true;
}

bool BuildModuleSelector(const UeAgentAuthoringState::FReadRequest &Request,
                         FModuleSelector &OutSelector, UE::UAI::Contract::FDiagnostic& OutError) {
  OutSelector = FModuleSelector();
  const FString GuidText =
      Request.TargetIdentity.GetSelector(TEXT("module_guid"));
  if (!GuidText.IsEmpty() && !FGuid::Parse(GuidText, OutSelector.NodeGuid)) {
    OutError = StateValueFailure(TEXT("niagara_module_guid_invalid"),
        TEXT("The module node selector must be a valid GUID."), TEXT("module_guid"),
        MakeShared<FJsonValueString>(GuidText), MakeShared<FJsonValueString>(TEXT("GUID")));
    return false;
  }
  OutSelector.ScriptPath =
      Request.TargetIdentity.GetSelector(TEXT("script_asset_path"));
  const FString OccurrenceText =
      Request.TargetIdentity.GetSelector(TEXT("occurrence"));
  if (!OccurrenceText.IsEmpty()) {
    OutSelector.Occurrence = FCString::Atoi(*OccurrenceText);
    OutSelector.bOccurrenceExplicit = true;
  }
  return true;
}

TSharedRef<FJsonObject> BuildStageState(const FResolvedStage &Stage) {
  TSharedRef<FJsonObject> State = MakeShared<FJsonObject>();
  if (!Stage.EmitterName.IsEmpty())
    State->SetStringField(TEXT("emitter_name"), Stage.EmitterName);
  const FString UsageName = ScriptUsageToString(Stage.Usage);
  State->SetStringField(TEXT("name"), UsageName);
  State->SetStringField(TEXT("usage"), UsageName);
  return State;
}

TSharedPtr<FJsonObject> BuildModuleState(const FResolvedStage &Stage,
                                         UNiagaraNodeFunctionCall &Module,
                                         const int32 ModuleIndex,
                                         UE::UAI::Contract::FDiagnostic& OutError) {
  TSharedRef<FJsonObject> State = MakeShared<FJsonObject>();
  State->SetStringField(TEXT("name"), Module.GetFunctionName());
  if (Cast<UNiagaraNodeAssignment>(&Module)) {
    State->SetStringField(
        TEXT("display_name"), Module.GetNodeTitle(ENodeTitleType::ListView).ToString());
    State->SetStringField(TEXT("module_kind"), TEXT("assignment"));
    TSharedRef<FJsonObject> AssignmentTargets = MakeShared<FJsonObject>();
    if (!BuildAssignmentTargets(Module, AssignmentTargets, OutError))
      return nullptr;
    State->SetObjectField(TEXT("assignment_targets"), AssignmentTargets);
  } else {
    State->SetStringField(TEXT("script_asset_path"),
                          GetModuleScriptPath(Module));
  }
  State->SetNumberField(TEXT("module_index"), ModuleIndex);
  TSharedRef<FJsonObject> Inputs = MakeShared<FJsonObject>();
  if (!BuildInputOverrides(Module, Inputs, OutError))
    return nullptr;
  SetInputPatchState(State, Inputs);
  return State;
}

TSharedPtr<FJsonObject>
BuildStandaloneAssignmentState(UNiagaraNode &AssignmentNode,
                               const int32 ModuleIndex, UE::UAI::Contract::FDiagnostic& OutError) {
  using namespace UeAgentNiagaraStandaloneModuleAuthoring;
  TSharedRef<FJsonObject> State = MakeShared<FJsonObject>();
  State->SetStringField(TEXT("name"), AssignmentNode.GetName());
  State->SetStringField(
      TEXT("display_name"), AssignmentNode.GetNodeTitle(ENodeTitleType::ListView).ToString());
  State->SetStringField(TEXT("module_kind"), TEXT("assignment"));
  State->SetNumberField(TEXT("module_index"), ModuleIndex);

  TArray<FNiagaraVariable> Targets;
  GetAssignmentTargets(AssignmentNode, Targets);
  TSharedRef<FJsonObject> AssignmentTargets = MakeShared<FJsonObject>();
  TSharedRef<FJsonObject> Inputs = MakeShared<FJsonObject>();
  for (const FNiagaraVariable &Target : Targets) {
    AssignmentTargets->SetStringField(
        Target.GetName().ToString(),
        UeAgentNiagaraStageModuleAuthoring::GetNiagaraTypeToken(
            Target.GetType()));
    FNiagaraVariable ResolvedTarget;
    UEdGraphPin *TargetPin = FindAssignmentTargetPin(
        AssignmentNode, Target.GetName().ToString(), ResolvedTarget);
    if (!TargetPin)
      continue;
    TSharedPtr<FJsonObject> Payload;
    bool bHasValue = false;
    if (!UeAgentNiagaraStageModuleAuthoring::BuildInputValueForPin(
            *TargetPin, ResolvedTarget.GetType(), Payload, bHasValue, OutError))
      return nullptr;
    if (Payload) UeAgentAuthoringEncodedTypes::FScope::SetNativeField(Inputs, Target.GetName().ToString(),
        MakeShared<FJsonValueObject>(Payload), bHasValue
            ? UeAgentAuthoringEncodedTypes::EFieldEmission::Always
            : UeAgentAuthoringEncodedTypes::EFieldEmission::NativeDefault);
  }
  State->SetObjectField(TEXT("assignment_targets"), AssignmentTargets);
  SetInputPatchState(State, Inputs);
  return State;
}

bool BuildRendererAuthoringProperties(UNiagaraRendererProperties &Renderer,
                                      TSharedPtr<FJsonObject> &OutProperties,
                                      UE::UAI::Contract::FDiagnostic& OutError) {
  UeAgentDetailsAuthoringService::FCapturePolicy Policy;
  Policy.StructFieldFilter = UeAgentNiagaraRendererBindings::IncludeAuthoringField;
  Policy.AuthorizeProperty = UeAgentNiagaraRendererBindings::AuthorizeAuthoringField;
  UeAgentAuthoringState::FStateSnapshot Snapshot;
  if (!UeAgentDetailsAuthoringService::CaptureObjectProperties(
          &Renderer, Policy, Snapshot, OutProperties, OutError)) {
    return false;
  }

  const TArray<TSharedPtr<FJsonValue>> *ExistingProperties = nullptr;
  if (!OutProperties.IsValid() ||
      !OutProperties->TryGetArrayField(TEXT("properties"),
                                       ExistingProperties) ||
      !ExistingProperties) {
    OutError = StateValueFailure(TEXT("niagara_renderer_properties_document_invalid"),
        TEXT("Renderer capture did not produce a properties array."), TEXT("properties"),
        OutProperties.IsValid() ? OutProperties->TryGetField(TEXT("properties")) : TSharedPtr<FJsonValue>(),
        MakeShared<FJsonValueString>(TEXT("array")), true);
    OutError.CandidateName = Renderer.GetName();
    OutError.InternalContext = Renderer.GetPathName();
    return false;
  }
  TArray<TSharedPtr<FJsonValue>> Properties = *ExistingProperties;
  for (const auto& Value : Properties) {
    const auto Descriptor = Value->AsObject();
    const FString Path = Descriptor->GetStringField(TEXT("property_path"));
    auto* Property = FindFProperty<FStructProperty>(Renderer.GetClass(), *Path);
    if (Property && Property->Struct == FNiagaraVariableAttributeBinding::StaticStruct()) {
      Descriptor->SetField(TEXT("value"), UeAgentNiagaraRendererBindings::EncodeAuthoringBinding(
          *Property, *Property->ContainerPtrToValuePtr<FNiagaraVariableAttributeBinding>(&Renderer), Renderer));
    }
  }
  Properties.RemoveAll([](const TSharedPtr<FJsonValue> &Value) {
    const TSharedPtr<FJsonObject> Descriptor =
        Value.IsValid() && Value->Type == EJson::Object ? Value->AsObject()
                                                        : nullptr;
    FString PropertyPath;
    return Descriptor.IsValid() &&
           Descriptor->TryGetStringField(TEXT("property_path"), PropertyPath) &&
           PropertyPath == TEXT("bIsEnabled");
  });

  TSharedRef<FJsonObject> Literal = MakeShared<FJsonObject>();
  Literal->SetBoolField(TEXT("literal"), Renderer.GetIsEnabled());
  TSharedRef<FJsonObject> Descriptor = MakeShared<FJsonObject>();
  Descriptor->SetStringField(TEXT("property_path"), TEXT("bIsEnabled"));
  Descriptor->SetStringField(TEXT("kind"), TEXT("bool"));
  Descriptor->SetObjectField(TEXT("value"), Literal);
  Properties.Add(MakeShared<FJsonValueObject>(Descriptor));
  Properties.Sort([](const TSharedPtr<FJsonValue> &Left,
                     const TSharedPtr<FJsonValue> &Right) {
    const TSharedPtr<FJsonObject> LeftObject =
        Left.IsValid() && Left->Type == EJson::Object ? Left->AsObject()
                                                      : nullptr;
    const TSharedPtr<FJsonObject> RightObject =
        Right.IsValid() && Right->Type == EJson::Object ? Right->AsObject()
                                                        : nullptr;
    FString LeftPath;
    FString RightPath;
    if (LeftObject.IsValid()) {
      LeftObject->TryGetStringField(TEXT("property_path"), LeftPath);
    }
    if (RightObject.IsValid()) {
      RightObject->TryGetStringField(TEXT("property_path"), RightPath);
    }
    return LeftPath < RightPath;
  });
  OutProperties->SetArrayField(TEXT("properties"), Properties);
  OutProperties->SetNumberField(TEXT("property_count"), Properties.Num());
  OutError.Reset();
  return true;
}

bool AppendEmitterPropertyReadRequests(
    const FResolvedEmitterOwner &Owner,
    const TMap<FString, FString> &OwnerSelectors,
    TArray<UeAgentAuthoringState::FReadRequest> &OutRequests,
    UE::UAI::Contract::FDiagnostic& OutError) {
  OutError.Reset();
  if (!Owner.Emitter || !Owner.EmitterData) {
    OutError = StateValueFailure(TEXT("niagara_emitter_owner_invalid"),
        TEXT("Emitter property capture requires an emitter instance and its version data."),
        Owner.Emitter ? TEXT("emitter.version_data.available") : TEXT("emitter.available"),
        MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), true);
    OutError.TargetPath = OwnerSelectors.FindRef(TEXT("asset_path"));
    OutError.CandidateName = Owner.EmitterName;
    return false;
  }

  UeAgentDetailsAuthoringService::FCapturePolicy Policy;
  Policy.SemanticId = EmitterPropertySemanticId;
  Policy.BuildSnapshotIdentity =
      [OwnerSelectors](
          const FString &PropertyPath,
          UeAgentAuthoringIdentity::FCanonicalIdentity &OutIdentity,
          UE::UAI::Contract::FDiagnostic& IdentityError) {
        TMap<FString, FString> Selectors = OwnerSelectors;
        Selectors.Add(TEXT("property_path"), PropertyPath);
        return UeAgentAuthoringIdentity::BuildCanonicalIdentity(
            TEXT("niagara_emitter_property_identity"),
            TEXT("niagara_emitter_property"), Selectors, OutIdentity,
            IdentityError);
      };
  return UeAgentDetailsAuthoringService::AppendStructPropertyReadRequests(
      FVersionedNiagaraEmitterData::StaticStruct(), Owner.EmitterData,
      Owner.Emitter->GetPathName(), Owner.Emitter, Policy, OutRequests,
      OutError);
}

FString EscapeResourcePointerToken(FString Token) {
  Token.ReplaceInline(TEXT("~"), TEXT("~0"));
  Token.ReplaceInline(TEXT("/"), TEXT("~1"));
  return Token;
}

bool BuildDynamicInputState(UNiagaraNodeFunctionCall &DynamicInput,
                            const FString &InputName,
                            const UObject* OwningAsset,
                            TSharedRef<FJsonObject> State,
                            TSharedRef<FJsonObject> Definition,
                            UE::UAI::Contract::FDiagnostic& OutError) {
  struct FStateFrame {
    UNiagaraNodeFunctionCall *Node = nullptr;
    FString InputName;
    TSharedPtr<FJsonObject> State;
    TSharedPtr<FJsonObject> Definition;
    FString Pointer;
    bool bExit = false;
  };

  // The shared tracker emits resource, limit, consumed and pointer diagnostics.
  FUeAgentStructuredResourceTracker ResourceTracker;
  TSet<UNiagaraNodeFunctionCall *> ActiveNodes;
  TArray<FStateFrame> Stack;
  Stack.Add({&DynamicInput, InputName, State, Definition,
             TEXT("/dynamic_inputs/") +
                 EscapeResourcePointerToken(InputName),
             false});

  while (!Stack.IsEmpty()) {
    FStateFrame Frame = Stack.Pop(EAllowShrinking::No);
    if (Frame.bExit) {
      ActiveNodes.Remove(Frame.Node);
      continue;
    }
    if (!Frame.Node || !Frame.State.IsValid() || !Frame.Definition.IsValid()) {
      OutError = StateValueFailure(TEXT("niagara_dynamic_input_state_frame_invalid"),
          TEXT("Dynamic input capture requires a function node, output state, and value definition."),
          !Frame.Node ? TEXT("node.available") : !Frame.State.IsValid() ? TEXT("state.available") : TEXT("definition.available"),
          MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), true);
      OutError.CandidateName = Frame.InputName;
      OutError.InternalContext = Frame.Pointer;
      return false;
    }
    if (ActiveNodes.Contains(Frame.Node)) {
      OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("niagara_dynamic_input_cycle_detected"), TEXT("The Niagara dynamic inputs contain a cycle."), TEXT("state_capture"));
      OutError.SourceKind = TEXT("native_result");
      OutError.PropertyPath = Frame.Pointer;
      OutError.CandidateName = Frame.Node->GetFunctionName();
      OutError.InternalContext = Frame.Node->GetPathName();
      return false;
    }
    if (!ResourceTracker.ChargeNodes(1, Frame.Pointer, OutError) ||
        !ResourceTracker.ChargeItems(1, Frame.Pointer, OutError) ||
        !ResourceTracker.ChargeWorkUnits(1, Frame.Pointer, OutError) ||
        !ResourceTracker.ChargeEstimatedMemoryBytes(
            sizeof(FStateFrame) + 2 * sizeof(FJsonObject), Frame.Pointer,
            OutError)) {
      return false;
    }

    ActiveNodes.Add(Frame.Node);
    Stack.Add(
        {Frame.Node, FString(), nullptr, nullptr, Frame.Pointer, true});

    Frame.State->SetStringField(TEXT("script_asset_path"),
                                GetModuleScriptPath(*Frame.Node));
    Frame.State->SetStringField(TEXT("name"),
                                Frame.Node->GetFunctionName());
    Frame.State->SetStringField(TEXT("input_name"), Frame.InputName);
    TSharedRef<FJsonObject> Inputs = MakeShared<FJsonObject>();
    if (!BuildInputOverrides(*Frame.Node, Inputs, OutError))
      return false;
    SetInputPatchState(Frame.State.ToSharedRef(), Inputs);

    TArray<FResolvedModuleInput> ResolvedInputs;
    BuildResolvedModuleInputs(*Frame.Node, ResolvedInputs);
    if (!ResourceTracker.ChargeItems(
            ResolvedInputs.Num(), Frame.Pointer + TEXT("/inputs"), OutError) ||
        !ResourceTracker.ChargeEstimatedMemoryBytes(
            static_cast<int64>(ResolvedInputs.Num()) *
                sizeof(FResolvedModuleInput),
            Frame.Pointer + TEXT("/inputs"), OutError)) {
      return false;
    }

    TSharedRef<FJsonObject> DataInterfaces = MakeShared<FJsonObject>();
    for (const FResolvedModuleInput &Input : ResolvedInputs) {
      if (!Input.InputVariable.IsValid() ||
          !Input.InputVariable.GetType().IsDataInterface())
        continue;
      UNiagaraNodeInput *InputNode = nullptr;
      UNiagaraDataInterface *DataInterface = nullptr;
      FString ResolvedName;
      UE::UAI::Contract::FDiagnostic ResolveError;
      if (!ResolveDynamicInputDataInterface(
              *Frame.Node, Input.FullInputName, InputNode, DataInterface,
              ResolvedName, ResolveError)) {
        continue;
      }
      UeAgentAuthoringState::FStateSnapshot DataInterfaceSnapshot;
      TSharedPtr<FJsonObject> Properties;
      UE::UAI::Contract::FDiagnostic CaptureError;
      if (!UeAgentDetailsAuthoringService::CaptureObjectProperties(
              DataInterface,
              UeAgentDetailsAuthoringService::FCapturePolicy(),
              DataInterfaceSnapshot, Properties, CaptureError))
        continue;
      TSharedRef<FJsonObject> DataInterfaceState =
          MakeShared<FJsonObject>();
      DataInterfaceState->SetStringField(TEXT("input_name"), ResolvedName);
      DataInterfaceState->SetStringField(
          TEXT("data_interface_class"),
          DataInterface->GetClass()->GetPathName());
      DataInterfaceState->SetObjectField(TEXT("properties"), Properties);
      DataInterfaces->SetObjectField(ResolvedName, DataInterfaceState);
    }
    if (!DataInterfaces->Values.IsEmpty()) {
      Frame.State->SetObjectField(TEXT("data_interfaces"), DataInterfaces);
    }

    TSharedRef<FJsonObject> Children = MakeShared<FJsonObject>();
    TSharedRef<FJsonObject> ChildDefinitions = MakeShared<FJsonObject>();
    TArray<FStateFrame> ChildFrames;
    for (const FResolvedModuleInput &Input : ResolvedInputs) {
      FString ResolvedName;
      UNiagaraNodeFunctionCall *Child = FindDynamicInputForModuleInput(*Frame.Node, Input.FullInputName, ResolvedName, OutError);
        if (OutError.IsValid()) return false;
      if (!Child)
        continue;
      if (ResolvedName.IsEmpty())
        ResolvedName = Input.FullInputName;
      TSharedRef<FJsonObject> ChildState = MakeShared<FJsonObject>();
      TSharedRef<FJsonObject> ChildDefinition = BuildFunctionInputDefinition(*Child, OwningAsset);
      Children->SetObjectField(ResolvedName, ChildState);
      ChildDefinitions->SetObjectField(ResolvedName, ChildDefinition);
      ChildFrames.Add(
          {Child, ResolvedName, ChildState, ChildDefinition,
           Frame.Pointer + TEXT("/dynamic_inputs/") +
               EscapeResourcePointerToken(ResolvedName),
           false});
    }
    if (!Children->Values.IsEmpty()) {
      Frame.State->SetObjectField(TEXT("dynamic_inputs"), Children);
      TSharedRef<FJsonObject> ChildrenDefinition = MakeShared<FJsonObject>();
      ChildrenDefinition->SetObjectField(TEXT("fields"), ChildDefinitions);
      Frame.Definition->GetObjectField(TEXT("fields"))->SetObjectField(
          TEXT("dynamic_inputs"), ChildrenDefinition);
    }
    for (int32 Index = ChildFrames.Num() - 1; Index >= 0; --Index) {
      Stack.Add(MoveTemp(ChildFrames[Index]));
    }
  }
  return ResourceTracker.Checkpoint(TEXT("/dynamic_inputs"), OutError);
}

TSharedRef<FJsonObject>
BuildAssetState(const UObject &Object,
                const FString &CanonicalAssetPath = FString()) {
  TSharedRef<FJsonObject> State = MakeShared<FJsonObject>();
  State->SetStringField(TEXT("asset_path"),
                        CanonicalAssetPath.IsEmpty()
                            ? Object.GetOutermost()->GetName()
                            : CanonicalAssetPath);
  State->SetStringField(TEXT("asset_kind"), GetAssetKind(&Object));
  return State;
}

UeAgentAuthoringState::EReadOutcome
ReadState(const UeAgentAuthoringState::FReadRequest &Request,
          UeAgentAuthoringState::FStateValue &OutValue, UE::UAI::Contract::FDiagnostic& OutError) {
  OutValue = UeAgentAuthoringState::FStateValue();
  OutValue.TargetIdentity = Request.TargetIdentity;
  OutValue.Type.Kind = TEXT("object");
  OutValue.bEditable = true;
  OutValue.Provenance = TEXT("niagara.state");
  OutError.Reset();
  ON_SCOPE_EXIT {
    if (OutError.IsValid()) {
      if (OutError.TargetPath.IsEmpty())
        OutError.TargetPath = Request.TargetIdentity.GetSelector(TEXT("asset_path"));
      if (OutError.CandidateName.IsEmpty())
        OutError.CandidateName = Request.TargetIdentity.GetSelector(TEXT("emitter_name"));
    }
  };

  const FString *ExpectedAbsent = Request.Context.Find(TEXT("expected_absent"));
  if (ExpectedAbsent &&
      ExpectedAbsent->Equals(TEXT("true"), ESearchCase::IgnoreCase)) {
    OutValue.bAvailable = false;
    OutValue.AvailabilityReason = TEXT("target_not_found");
    return UeAgentAuthoringState::EReadOutcome::Unavailable;
  }

  if (Request.SemanticId == UserParameterSemanticId) {
    UNiagaraSystem *System =
        Cast<UNiagaraSystem>(ResolveRequestObject(Request, OutError));
    if (!System) {
      if (OutError.IsValid() && OutError.Code != TEXT("state_request_object_not_found"))
        return UeAgentAuthoringState::EReadOutcome::Failed;
      OutError.Reset();
      OutValue.bAvailable = false;
      OutValue.AvailabilityReason =
          TEXT("niagara_user_parameter_system_not_found");
      return UeAgentAuthoringState::EReadOutcome::Unavailable;
    }
    const FString ParameterName =
        Request.TargetIdentity.GetSelector(TEXT("parameter_name"));
    FNiagaraVariable Variable;
    if (!UeAgentNiagaraUserParameterAuthoring::FindByName(
            *System, ParameterName, Variable)) {
      OutValue.bAvailable = false;
      OutValue.AvailabilityReason = TEXT("niagara_user_parameter_not_found");
      return UeAgentAuthoringState::EReadOutcome::Unavailable;
    }
    TSharedRef<FJsonObject> State =
        UeAgentNiagaraUserParameterAuthoring::BuildState(*System, Variable, true);
    State->SetStringField(TEXT("name"), ParameterName);
    OutValue.Value = MakeShared<FJsonValueObject>(State);
    OutValue.bAvailable = true;
    return UeAgentAuthoringState::EReadOutcome::Resolved;
  }

  if (Request.SemanticId == EmitterOwnerSemanticId ||
      Request.SemanticId == RendererSemanticId ||
      Request.SemanticId == EventHandlerSemanticId) {
    FEmitterOwnerTarget OwnerTarget;
    OwnerTarget.AssetPath =
        Request.TargetIdentity.GetSelector(TEXT("asset_path"));
    OwnerTarget.EmitterName =
        Request.TargetIdentity.GetSelector(TEXT("emitter_name"));
    const FString HandleIdText =
        Request.TargetIdentity.GetSelector(TEXT("emitter_handle_id"));
    if (!HandleIdText.IsEmpty()) {
      if (!FGuid::Parse(HandleIdText, OwnerTarget.EmitterHandleId)) {
        OutError = StateValueFailure(TEXT("niagara_emitter_handle_id_invalid"),
            TEXT("The emitter handle selector must be a valid GUID."), TEXT("emitter_handle_id"),
            MakeShared<FJsonValueString>(HandleIdText), MakeShared<FJsonValueString>(TEXT("GUID")));
        return UeAgentAuthoringState::EReadOutcome::Failed;
      }
      OwnerTarget.bEmitterHandleIdExplicit = true;
    }
    FResolvedEmitterOwner Owner;
    UObject* Root = ResolveRequestObject(Request, OutError);
    if (!Root && OutError.IsValid() && OutError.Code != TEXT("state_request_object_not_found"))
      return UeAgentAuthoringState::EReadOutcome::Failed;
    if (!ResolveEmitterOwner(OwnerTarget, Owner, OutError, Root)) {
      OutValue.bAvailable = false;
      OutValue.AvailabilityReason = OutError.Code;
      if (OutError.Code != TEXT("niagara_emitter_owner_not_found")
          && OutError.Code != TEXT("niagara_emitter_owner_asset_not_found"))
        return UeAgentAuthoringState::EReadOutcome::Failed;
      OutError.Reset();
      return UeAgentAuthoringState::EReadOutcome::Unavailable;
    }
    if (Request.SemanticId == EmitterOwnerSemanticId) {
      TSharedRef<FJsonObject> State = MakeShared<FJsonObject>();
      if (!Owner.EmitterName.IsEmpty()) {
        State->SetStringField(TEXT("name"), Owner.EmitterName);
        State->SetStringField(TEXT("emitter_name"), Owner.EmitterName);
      }
      if (Owner.System && Owner.EmitterData) {
        const FVersionedNiagaraEmitter Parent = Owner.EmitterData->GetParent();
        if (Parent.Emitter && Parent.Emitter->GetOutermost()) {
          State->SetStringField(TEXT("source_asset_path"),
                               Parent.Emitter->GetOutermost()->GetName());
          if (Parent.Version.IsValid())
            State->SetStringField(TEXT("emitter_version"),
                                 Parent.Version.ToString(EGuidFormats::DigitsWithHyphensLower));
        }
      }
      if (Owner.EmitterHandleId.IsValid()) {
        if (Owner.System) {
          const FNiagaraEmitterHandle *Handle =
              Owner.System->GetEmitterHandles().FindByPredicate(
                  [&Owner](const FNiagaraEmitterHandle &Candidate) {
                    return Candidate.GetId() == Owner.EmitterHandleId;
                  });
          if (Handle)
            State->SetBoolField(TEXT("enabled"), Handle->GetIsEnabled());
        }
      }
      OutValue.Value = MakeShared<FJsonValueObject>(State);
      OutValue.bAvailable = true;
      return UeAgentAuthoringState::EReadOutcome::Resolved;
    }

    if (Request.SemanticId == RendererSemanticId) {
      const FString RendererName =
          Request.TargetIdentity.GetSelector(TEXT("renderer_name"));
      const TArray<UNiagaraRendererProperties *> &Renderers =
          Owner.EmitterData->GetRenderers();
      for (int32 Index = 0; Index < Renderers.Num(); ++Index) {
        UNiagaraRendererProperties *Renderer = Renderers[Index];
        if (!Renderer || !Renderer->GetFName().ToString().Equals(
                             RendererName, ESearchCase::IgnoreCase))
          continue;
        TSharedRef<FJsonObject> State = MakeShared<FJsonObject>();
        State->SetStringField(TEXT("renderer_name"),
                              Renderer->GetFName().ToString());
        State->SetStringField(TEXT("renderer_class"),
                              Renderer->GetClass()->GetPathName());
        State->SetNumberField(TEXT("renderer_index"), Index);
        TSharedPtr<FJsonObject> RendererProperties;
        if (!BuildRendererAuthoringProperties(*Renderer, RendererProperties,
                                              OutError)) {
          return UeAgentAuthoringState::EReadOutcome::Failed;
        }
        State->SetObjectField(TEXT("properties"), RendererProperties);
        OutValue.Value = MakeShared<FJsonValueObject>(State);
        OutValue.bAvailable = true;
        return UeAgentAuthoringState::EReadOutcome::Resolved;
      }
      OutValue.bAvailable = false;
      OutValue.AvailabilityReason = TEXT("niagara_renderer_not_found");
      return UeAgentAuthoringState::EReadOutcome::Unavailable;
    }

    const FString UsageIdText =
        Request.TargetIdentity.GetSelector(TEXT("script_usage_id"));
    FGuid UsageId;
    if (!FGuid::Parse(UsageIdText, UsageId)) {
      OutError = StateValueFailure(TEXT("niagara_event_handler_usage_id_invalid"),
          TEXT("The event handler usage selector must be a valid GUID."), TEXT("script_usage_id"),
          MakeShared<FJsonValueString>(UsageIdText), MakeShared<FJsonValueString>(TEXT("GUID")));
      return UeAgentAuthoringState::EReadOutcome::Failed;
    }
    for (int32 Index = 0;
         Index < Owner.EmitterData->EventHandlerScriptProps.Num(); ++Index) {
      const FNiagaraEventScriptProperties &Event =
          Owner.EmitterData->EventHandlerScriptProps[Index];
      if (!Event.Script || Event.Script->GetUsageId() != UsageId)
        continue;
      TSharedRef<FJsonObject> State = MakeShared<FJsonObject>();
      State->SetStringField(TEXT("name"), Event.SourceEventName.ToString());
      State->SetNumberField(TEXT("event_index"), Index);
      State->SetStringField(TEXT("source_event_name"),
                            Event.SourceEventName.ToString());
      State->SetStringField(
          TEXT("source_emitter_id"),
          Event.SourceEmitterID.ToString(EGuidFormats::DigitsWithHyphensLower));
      if (const UEnum *Enum = StaticEnum<EScriptExecutionMode>()) {
        State->SetStringField(TEXT("execution_mode"),
                              Enum->GetNameStringByValue(
                                  static_cast<int64>(Event.ExecutionMode)));
      }
      State->SetNumberField(TEXT("spawn_number"), Event.SpawnNumber);
      State->SetNumberField(TEXT("min_spawn_number"), Event.MinSpawnNumber);
      State->SetBoolField(TEXT("random_spawn_number"),
                          Event.bRandomSpawnNumber);
      State->SetNumberField(TEXT("max_events_per_frame"),
                            Event.MaxEventsPerFrame);
      State->SetBoolField(TEXT("update_attribute_initial_values"),
                          Event.UpdateAttributeInitialValues);
      OutValue.Value = MakeShared<FJsonValueObject>(State);
      OutValue.bAvailable = true;
      return UeAgentAuthoringState::EReadOutcome::Resolved;
    }
    OutValue.bAvailable = false;
    OutValue.AvailabilityReason = TEXT("niagara_event_handler_not_found");
    return UeAgentAuthoringState::EReadOutcome::Unavailable;
  }

  if (Request.SemanticId == StackSemanticId ||
      Request.SemanticId == ModuleSemanticId ||
      Request.SemanticId == DynamicInputSemanticId) {
    FStageTarget Target;
    FResolvedStage Stage;
    if (!BuildStageTarget(Request, Target, OutError)) {
      OutValue.bAvailable = false;
      OutValue.AvailabilityReason = OutError.Code;
      return UeAgentAuthoringState::EReadOutcome::Failed;
    }
    bool bStageResolved = false;
    if (Request.RootObject) {
      TArray<FResolvedStage> Stages;
      if (!EnumerateStages(*const_cast<UObject *>(Request.RootObject), Stages,
                           OutError)) {
        return UeAgentAuthoringState::EReadOutcome::Failed;
      }
      for (const FResolvedStage &Candidate : Stages) {
        if ((Target.bEmitterHandleIdExplicit &&
             Candidate.EmitterHandleId != Target.EmitterHandleId) ||
            (!Target.EmitterName.IsEmpty() &&
             !Candidate.EmitterName.Equals(Target.EmitterName,
                                           ESearchCase::IgnoreCase)) ||
            (Target.bUsageExplicit && Candidate.Usage != Target.Usage) ||
            (Target.bUsageIdExplicit && Candidate.UsageId != Target.UsageId))
          continue;
        if (bStageResolved) {
          OutError = StateValueFailure(TEXT("niagara_stage_identity_ambiguous"),
              TEXT("At least two Niagara stages match the requested stage selector."), TEXT("observed_matching_stages"),
              MakeShared<FJsonValueNumber>(2), MakeShared<FJsonValueNumber>(1), true);
          OutError.CandidateName = Target.EmitterName;
          return UeAgentAuthoringState::EReadOutcome::Failed;
        }
        Stage = Candidate;
        bStageResolved = true;
      }
    } else {
      bStageResolved = ResolveStage(Target, Stage, OutError);
    }
    if (!bStageResolved) {
      if (OutError.IsEmpty())
        OutError = StateValueFailure(TEXT("niagara_stage_not_found"),
            TEXT("No Niagara stage matches the requested stage selector."), TEXT("matching_stages"),
            MakeShared<FJsonValueNumber>(0), MakeShared<FJsonValueNumber>(1), true);
      OutValue.bAvailable = false;
      OutValue.AvailabilityReason = OutError.Code;
      if (OutError.Code != TEXT("niagara_stage_not_found")
          && OutError.Code != TEXT("niagara_stage_asset_not_found")
          && OutError.Code != TEXT("niagara_emitter_handle_not_found"))
        return UeAgentAuthoringState::EReadOutcome::Failed;
      OutError.Reset();
      return UeAgentAuthoringState::EReadOutcome::Unavailable;
    }
    if (Request.SemanticId == StackSemanticId) {
      OutValue.Value = MakeShared<FJsonValueObject>(BuildStageState(Stage));
      OutValue.bAvailable = true;
      return UeAgentAuthoringState::EReadOutcome::Resolved;
    }

    FModuleSelector ModuleSelector;
    if (!BuildModuleSelector(Request, ModuleSelector, OutError))
      return UeAgentAuthoringState::EReadOutcome::Failed;
    UNiagaraNodeFunctionCall *Module = nullptr;
    UNiagaraNode *StandaloneAssignment = nullptr;
    int32 ModuleIndex = INDEX_NONE;
    if (Request.SemanticId == ModuleSemanticId) {
      if (UeAgentNiagaraStandaloneModuleAuthoring::IsStandaloneModuleStage(
              Stage) &&
          ModuleSelector.NodeGuid.IsValid()) {
        if (!UeAgentNiagaraStandaloneModuleAuthoring::ResolveAssignmentNode(
                *Stage.OutputNode, ModuleSelector.NodeGuid, StandaloneAssignment,
                ModuleIndex, OutError)) {
          if (OutError.Code != TEXT("module_selector_not_found"))
            return UeAgentAuthoringState::EReadOutcome::Failed;
          OutError.Reset();
        }
      }
      if (!StandaloneAssignment &&
          !ResolveModule(*Stage.OutputNode, ModuleSelector, Module, ModuleIndex,
                         OutError)) {
        OutValue.bAvailable = false;
        OutValue.AvailabilityReason = OutError.Code;
        if (OutError.Code != TEXT("module_selector_not_found"))
          return UeAgentAuthoringState::EReadOutcome::Failed;
        OutError.Reset();
        return UeAgentAuthoringState::EReadOutcome::Unavailable;
      }
      TSharedPtr<FJsonObject> ModuleState =
          StandaloneAssignment
              ? BuildStandaloneAssignmentState(*StandaloneAssignment,
                                               ModuleIndex, OutError)
              : BuildModuleState(Stage, *Module, ModuleIndex, OutError);
      if (!ModuleState.IsValid())
        return UeAgentAuthoringState::EReadOutcome::Failed;
      OutValue.Value = MakeShared<FJsonValueObject>(ModuleState.ToSharedRef());
      if (Module) {
        OutValue.ValueDefinition = MakeShared<FJsonValueObject>(
            BuildFunctionInputDefinition(*Module, Stage.Asset));
      }
      OutValue.bAvailable = true;
      return UeAgentAuthoringState::EReadOutcome::Resolved;
    }

    FDynamicInputSelector DynamicSelector;
    DynamicSelector.Module = ModuleSelector;
    DynamicSelector.InputName =
        Request.TargetIdentity.GetSelector(TEXT("input_name"));
    const FString DynamicGuidText =
        Request.TargetIdentity.GetSelector(TEXT("dynamic_input_guid"));
    if (!DynamicGuidText.IsEmpty() &&
        !FGuid::Parse(DynamicGuidText, DynamicSelector.NodeGuid)) {
      OutError = StateValueFailure(TEXT("niagara_dynamic_input_guid_invalid"),
          TEXT("The dynamic input node selector must be a valid GUID."), TEXT("dynamic_input_guid"),
          MakeShared<FJsonValueString>(DynamicGuidText), MakeShared<FJsonValueString>(TEXT("GUID")));
      return UeAgentAuthoringState::EReadOutcome::Failed;
    }
    UNiagaraNodeFunctionCall *DynamicInput = nullptr;
    FString ResolvedInputName;
    if (UeAgentNiagaraStandaloneModuleAuthoring::IsStandaloneModuleStage(
            Stage) &&
        DynamicSelector.Module.NodeGuid.IsValid()) {
      int32 AssignmentIndex = INDEX_NONE;
      if (UeAgentNiagaraStandaloneModuleAuthoring::ResolveAssignmentNode(
              *Stage.OutputNode, DynamicSelector.Module.NodeGuid,
              StandaloneAssignment, AssignmentIndex, OutError)) {
        FNiagaraVariable TargetVariable;
        UEdGraphPin *TargetPin =
            UeAgentNiagaraStandaloneModuleAuthoring::FindAssignmentTargetPin(
                *StandaloneAssignment, DynamicSelector.InputName,
                TargetVariable);
        DynamicInput =
            TargetPin
                ? UeAgentNiagaraStageModuleAuthoring::FindDynamicInputForPin(
                      *TargetPin)
                : nullptr;
        ResolvedInputName = TargetVariable.IsValid()
                                ? TargetVariable.GetName().ToString()
                                : FString();
      } else {
        if (OutError.Code != TEXT("module_selector_not_found"))
          return UeAgentAuthoringState::EReadOutcome::Failed;
        OutError.Reset();
      }
    }
    if (!DynamicInput &&
        !ResolveDynamicInput(Stage, DynamicSelector, DynamicInput,
                             ResolvedInputName, OutError)) {
      OutValue.bAvailable = false;
      OutValue.AvailabilityReason = OutError.Code;
      if (OutError.Code != TEXT("niagara_dynamic_input_not_found")
          && OutError.Code != TEXT("module_selector_not_found"))
        return UeAgentAuthoringState::EReadOutcome::Failed;
      OutError.Reset();
      return UeAgentAuthoringState::EReadOutcome::Unavailable;
    }
    TSharedRef<FJsonObject> DynamicState = MakeShared<FJsonObject>();
    TSharedRef<FJsonObject> DynamicDefinition =
        BuildFunctionInputDefinition(*DynamicInput, Stage.Asset);
    if (!BuildDynamicInputState(
            *DynamicInput,
            ResolvedInputName.IsEmpty() ? DynamicSelector.InputName
                                        : ResolvedInputName,
            Stage.Asset,
            DynamicState, DynamicDefinition, OutError)) {
      return UeAgentAuthoringState::EReadOutcome::Failed;
    }
    OutValue.Value = MakeShared<FJsonValueObject>(DynamicState);
    OutValue.ValueDefinition = MakeShared<FJsonValueObject>(DynamicDefinition);
    OutValue.bAvailable = true;
    return UeAgentAuthoringState::EReadOutcome::Resolved;
  }

  UObject *Object = ResolveRequestObject(Request, OutError);
  if (!IsValid(Object)) {
    if (OutError.IsValid() && OutError.Code != TEXT("state_request_object_not_found"))
      return UeAgentAuthoringState::EReadOutcome::Failed;
    OutError.Reset();
    OutValue.bAvailable = false;
    OutValue.AvailabilityReason = TEXT("target_not_found");
    return UeAgentAuthoringState::EReadOutcome::Unavailable;
  }
  const FString ActualKind = GetAssetKind(Object);
  if (ActualKind.IsEmpty()) {
    OutError = StateValueFailure(TEXT("niagara_state_target_type_invalid"),
        TEXT("Niagara state capture requires a Niagara system, emitter, or script asset."), TEXT("asset.class"),
        MakeShared<FJsonValueString>(Object->GetClass()->GetName()),
        MakeShared<FJsonValueString>(TEXT("NiagaraSystem | NiagaraEmitter | NiagaraScript")), true);
    return UeAgentAuthoringState::EReadOutcome::Failed;
  }
  OutValue.Value = MakeShared<FJsonValueObject>(BuildAssetState(
      *Object, Request.TargetIdentity.GetSelector(TEXT("asset_path"))));
  OutValue.bAvailable = true;
  return UeAgentAuthoringState::EReadOutcome::Resolved;
}

UeAgentAuthoringNativeDefaults::EProbeOutcome
ProbeNativeDefault(const UeAgentAuthoringNativeDefaults::FRequest &Request,
                   UeAgentAuthoringNativeDefaults::FScratchScope &Scratch,
                   UeAgentAuthoringNativeDefaults::FProbeValue &OutValue,
                   UE::UAI::Contract::FDiagnostic& OutError) {
  OutValue = UeAgentAuthoringNativeDefaults::FProbeValue();
  OutError.Reset();
  const FString TargetKind = Request.TargetIdentity.TargetKind;
  if (TargetKind == TEXT("niagara_stack_entry") ||
      TargetKind == TEXT("niagara_module") ||
      TargetKind == TEXT("niagara_dynamic_input") ||
      TargetKind == TEXT("niagara_emitter_owner") ||
      TargetKind == TEXT("niagara_renderer") ||
      TargetKind == TEXT("niagara_event_handler") ||
      TargetKind == TEXT("niagara_user_parameter")) {
    TSharedRef<FJsonObject> Deferred = MakeShared<FJsonObject>();
    Deferred->SetStringField(TEXT("generated_by"), TEXT("niagara_domain_api"));
    Deferred->SetBoolField(TEXT("deferred_until_apply"), true);
    OutValue.Value = MakeShared<FJsonValueObject>(Deferred);
    OutValue.Provenance = TEXT("niagara.domain_api");
    return UeAgentAuthoringNativeDefaults::EProbeOutcome::Resolved;
  }

  UClass *ObjectClass = Request.ObjectClass;
  if (!ObjectClass) {
    if (TargetKind == TEXT("niagara_system"))
      ObjectClass = UNiagaraSystem::StaticClass();
    else if (TargetKind == TEXT("niagara_emitter"))
      ObjectClass = UNiagaraEmitter::StaticClass();
    else if (TargetKind == TEXT("niagara_script"))
      ObjectClass = UNiagaraScript::StaticClass();
  }
  if (!ObjectClass) {
    return UeAgentAuthoringNativeDefaults::EProbeOutcome::Unavailable;
  }

  UPackage *ScratchPackage = Scratch.CreateScratchPackage(OutError);
  if (!ScratchPackage) {
    return UeAgentAuthoringNativeDefaults::EProbeOutcome::Failed;
  }
  const FString AssetKind = TargetKind.RightChop(8);
  UObject *Seed = CreateFactorySeed(ObjectClass, ScratchPackage, AssetKind,
                                    true, true, OutError);
  if (!Seed) {
    return UeAgentAuthoringNativeDefaults::EProbeOutcome::Failed;
  }
  OutValue.Value = MakeShared<FJsonValueObject>(BuildAssetState(*Seed));
  OutValue.Provenance = TEXT("niagara.factory");
  return UeAgentAuthoringNativeDefaults::EProbeOutcome::Resolved;
}
} // namespace

bool BuildAssetExportSnapshotReadRequests(
    UObject &Asset, TArray<UeAgentAuthoringState::FReadRequest> &OutRequests,
    UE::UAI::Contract::FDiagnostic& OutError) {
  if (!BuildAssetSnapshotReadRequests(Asset, true, OutRequests, OutError))
    return false;
  UeAgentAuthoringState::FEditablePropertyReadOptions Options;
  Options.SemanticId = TEXT("core.property");
  // UE's structural storage (System handles/scripts/user parameters and
  // Emitter/Script VersionData) has no CPF_Edit. The common flags filter keeps
  // those under their existing domain owners, without a duplicate field list.
  return UeAgentAuthoringState::AppendEditableObjectPropertyReadRequests(
      &Asset, Options, OutRequests, OutError);
}

bool BuildAssetSnapshotReadRequests(
    UObject &Asset, const bool bIncludeStack,
    TArray<UeAgentAuthoringState::FReadRequest> &OutRequests, UE::UAI::Contract::FDiagnostic& OutError,
    const FString &CanonicalAssetPathOverride) {
  OutRequests.Reset();
  OutError.Reset();
  const FString AssetKind = GetAssetKind(&Asset);
  const FString SemanticId = GetAssetSemanticId(&Asset);
  const FString IdentityModelId = GetAssetIdentityModelId(&Asset);
  const FString AssetPath = CanonicalAssetPathOverride.IsEmpty()
                                ? Asset.GetOutermost()->GetName()
                                : CanonicalAssetPathOverride;
  if (AssetKind.IsEmpty() || SemanticId.IsEmpty() ||
      IdentityModelId.IsEmpty()) {
    OutError = StateValueFailure(TEXT("niagara_snapshot_asset_type_invalid"),
        TEXT("The asset class has no complete Niagara snapshot kind, semantic, and identity registration."),
        AssetKind.IsEmpty() ? TEXT("asset_kind") : SemanticId.IsEmpty() ? TEXT("semantic_id") : TEXT("identity_model_id"),
        MakeShared<FJsonValueString>(FString()), MakeShared<FJsonValueString>(TEXT("registered non-empty identifier")), true);
    OutError.TargetPath = AssetPath;
    OutError.CandidateName = Asset.GetClass()->GetName();
    return false;
  }

  auto AddRequest = [&](const FString &InSemanticId, const FString &ModelId,
                        const FString &TargetKind,
                        const TMap<FString, FString> &Selectors) -> bool {
    UeAgentAuthoringIdentity::FCanonicalIdentity Identity;
    if (!UeAgentAuthoringIdentity::BuildCanonicalIdentity(
            ModelId, TargetKind, Selectors, Identity, OutError)) {
      const auto Cause = OutError;
      OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("niagara_snapshot_identity_invalid"), TEXT("The Niagara snapshot target identity is invalid."), TEXT("state_capture"));
      OutError.PublicCause = MakeShared<UE::UAI::Contract::FDiagnostic>(Cause);
      return false;
    }
    UeAgentAuthoringState::FReadRequest &Request =
        OutRequests.AddDefaulted_GetRef();
    Request.SemanticId = InSemanticId;
    Request.TargetIdentity = MoveTemp(Identity);
    Request.TargetQuery.ModelId = Request.TargetIdentity.ModelId;
    Request.TargetQuery.TargetKind = TargetKind;
    Request.TargetQuery.CanonicalKey = Request.TargetIdentity.CanonicalKey;
    Request.RootObject = &Asset;
    Request.RootOwnerPath = AssetPath;
    return true;
  };

  if (!AddRequest(SemanticId, IdentityModelId,
                  FString(TEXT("niagara_")) + AssetKind,
                  {{TEXT("asset_path"), AssetPath}})) {
    return false;
  }
  if (!bIncludeStack)
    return true;

  if (const UNiagaraSystem *System = Cast<UNiagaraSystem>(&Asset)) {
    TArray<FNiagaraVariable> UserParameters;
    UeAgentNiagaraUserParameterAuthoring::Enumerate(*System, UserParameters);
    for (const FNiagaraVariable &Parameter : UserParameters) {
      if (!AddRequest(
              UserParameterSemanticId, TEXT("niagara_user_parameter_identity"),
              TEXT("niagara_user_parameter"),
              {
                  {TEXT("asset_path"), AssetPath},
                  {TEXT("parameter_name"),
                   UeAgentNiagaraUserParameterAuthoring::CanonicalizeName(
                       Parameter.GetName().ToString())},
              }))
        return false;
    }
  }

  TArray<FResolvedEmitterOwner> EmitterOwners;
  if (!Cast<UNiagaraScript>(&Asset) &&
      !EnumerateEmitterOwners(Asset, EmitterOwners, OutError))
    return false;
  for (const FResolvedEmitterOwner &Owner : EmitterOwners) {
    const FString EmitterOwnerId =
        Owner.EmitterHandleId.IsValid()
            ? Owner.EmitterHandleId.ToString(
                  EGuidFormats::DigitsWithHyphensLower)
            : FString(TEXT("standalone"));
    TMap<FString, FString> OwnerSelectors = {
        {TEXT("asset_path"), AssetPath},
        {TEXT("emitter_owner_id"), EmitterOwnerId},
    };
    if (!Owner.EmitterName.IsEmpty())
      OwnerSelectors.Add(TEXT("emitter_name"), Owner.EmitterName);
    if (Owner.EmitterHandleId.IsValid()) {
      OwnerSelectors.Add(
          TEXT("emitter_handle_id"),
          Owner.EmitterHandleId.ToString(EGuidFormats::DigitsWithHyphensLower));
    }
    if (!AddRequest(EmitterOwnerSemanticId,
                    TEXT("niagara_emitter_owner_identity"),
                    TEXT("niagara_emitter_owner"), OwnerSelectors))
      return false;
    if (!AppendEmitterPropertyReadRequests(Owner, OwnerSelectors, OutRequests,
                                           OutError))
      return false;
    const TArray<UNiagaraRendererProperties *> &Renderers =
        Owner.EmitterData->GetRenderers();
    for (UNiagaraRendererProperties *Renderer : Renderers) {
      if (!Renderer)
        continue;
      TMap<FString, FString> Selectors = OwnerSelectors;
      Selectors.Add(TEXT("renderer_name"), Renderer->GetFName().ToString());
      Selectors.Add(TEXT("renderer_class"),
                    Renderer->GetClass()->GetPathName());
      if (!AddRequest(RendererSemanticId, TEXT("niagara_renderer_identity"),
                      TEXT("niagara_renderer"), Selectors))
        return false;
    }
    for (const FNiagaraEventScriptProperties &Event :
         Owner.EmitterData->EventHandlerScriptProps) {
      if (!Event.Script || !Event.Script->GetUsageId().IsValid())
        continue;
      TMap<FString, FString> Selectors = OwnerSelectors;
      Selectors.Add(TEXT("script_usage_id"),
                    Event.Script->GetUsageId().ToString(
                        EGuidFormats::DigitsWithHyphensLower));
      if (!AddRequest(EventHandlerSemanticId,
                      TEXT("niagara_event_handler_identity"),
                      TEXT("niagara_event_handler"), Selectors))
        return false;
    }
  }

  TArray<FResolvedStage> Stages;
  if (!EnumerateStages(Asset, Stages, OutError))
    return false;
  for (const FResolvedStage &Stage : Stages) {
    const TMap<FString, FString> StageSelectors = BuildStageIdentitySelectors(Stage, AssetPath);
    if (!AddRequest(StackSemanticId, TEXT("niagara_stack_entry_identity"),
                    TEXT("niagara_stack_entry"), StageSelectors))
      return false;

    if (UeAgentNiagaraStandaloneModuleAuthoring::IsStandaloneModuleStage(
            Stage)) {
      TArray<UNiagaraNode *> AssignmentNodes;
      if (!UeAgentNiagaraStandaloneModuleAuthoring::GetAssignmentNodes(
              *Stage.OutputNode, AssignmentNodes, OutError)) return false;
      if (!AssignmentNodes.IsEmpty()) {
        for (UNiagaraNode *AssignmentNode : AssignmentNodes) {
          if (!AssignmentNode || !AssignmentNode->NodeGuid.IsValid())
            continue;
          TMap<FString, FString> ModuleSelectors = StageSelectors;
          ModuleSelectors.Add(TEXT("module_guid"),
                              AssignmentNode->NodeGuid.ToString(
                                  EGuidFormats::DigitsWithHyphensLower));
          if (!AddRequest(ModuleSemanticId, TEXT("niagara_module_identity"),
                          TEXT("niagara_module"), ModuleSelectors))
            return false;

          TArray<FNiagaraVariable> Targets;
          UeAgentNiagaraStandaloneModuleAuthoring::GetAssignmentTargets(
              *AssignmentNode, Targets);
          for (const FNiagaraVariable &Target : Targets) {
            FNiagaraVariable ResolvedTarget;
            UEdGraphPin *TargetPin = UeAgentNiagaraStandaloneModuleAuthoring::
                FindAssignmentTargetPin(*AssignmentNode,
                                        Target.GetName().ToString(),
                                        ResolvedTarget);
            UNiagaraNodeFunctionCall *DynamicInput =
                TargetPin ? UeAgentNiagaraStageModuleAuthoring::
                                FindDynamicInputForPin(*TargetPin)
                          : nullptr;
            if (!DynamicInput || !DynamicInput->NodeGuid.IsValid())
              continue;
            TMap<FString, FString> DynamicSelectors = ModuleSelectors;
            DynamicSelectors.Add(TEXT("input_name"),
                                 Target.GetName().ToString());
            DynamicSelectors.Add(TEXT("dynamic_input_guid"),
                                 DynamicInput->NodeGuid.ToString(
                                     EGuidFormats::DigitsWithHyphensLower));
            if (!AddRequest(DynamicInputSemanticId,
                            TEXT("niagara_dynamic_input_identity"),
                            TEXT("niagara_dynamic_input"), DynamicSelectors))
              return false;
          }
        }
        continue;
      }
    }

    TArray<UNiagaraNodeFunctionCall *> Modules;
    if (!GetModules(*Stage.OutputNode, Modules, OutError)) return false;
    for (int32 ModuleIndex = 0; ModuleIndex < Modules.Num(); ++ModuleIndex) {
      UNiagaraNodeFunctionCall *Module = Modules[ModuleIndex];
      if (!Module || !Module->NodeGuid.IsValid())
        continue;
      TMap<FString, FString> ModuleSelectors = StageSelectors;
      ModuleSelectors.Add(
          TEXT("module_guid"),
          Module->NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
      const FString ModuleScriptPath = GetModuleScriptPath(*Module);
      if (!ModuleScriptPath.IsEmpty())
        ModuleSelectors.Add(TEXT("script_asset_path"), ModuleScriptPath);
      if (!AddRequest(ModuleSemanticId, TEXT("niagara_module_identity"),
                      TEXT("niagara_module"), ModuleSelectors))
        return false;

      TArray<FResolvedModuleInput> Inputs;
      BuildResolvedModuleInputs(*Module, Inputs);
      for (const FResolvedModuleInput &Input : Inputs) {
        FString ResolvedInputName;
        UNiagaraNodeFunctionCall *DynamicInput = FindDynamicInputForModuleInput(*Module, Input.FullInputName, ResolvedInputName, OutError);
        if (OutError.IsValid()) return false;
        if (!DynamicInput || !DynamicInput->NodeGuid.IsValid() ||
            ResolvedInputName.IsEmpty())
          continue;
        TMap<FString, FString> DynamicSelectors = ModuleSelectors;
        DynamicSelectors.Add(TEXT("input_name"), ResolvedInputName);
        DynamicSelectors.Add(TEXT("dynamic_input_guid"),
                             DynamicInput->NodeGuid.ToString(
                                 EGuidFormats::DigitsWithHyphensLower));
        if (!AddRequest(DynamicInputSemanticId,
                        TEXT("niagara_dynamic_input_identity"),
                        TEXT("niagara_dynamic_input"), DynamicSelectors))
          return false;
      }
    }
  }
  return true;
}

UObject *CreateFactorySeed(UClass *ObjectClass, UObject *Outer,
                           const FString &AssetKind,
                           const bool bCreateDefaultSystemNodes,
                           const bool bAddDefaultEmitterModulesAndRenderers,
                           UE::UAI::Contract::FDiagnostic& OutError, const FString &ScriptUsage) {
  OutError.Reset();
  if (!ObjectClass || !Outer) {
    OutError = StateValueFailure(TEXT("niagara_factory_seed_invalid"),
        TEXT("Creating a Niagara factory seed requires an object class and an outer."),
        ObjectClass ? TEXT("outer.available") : TEXT("object_class.available"),
        MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), true);
    return nullptr;
  }
  UObject *Object = nullptr;
  ENiagaraScriptUsage RequestedScriptUsage = ENiagaraScriptUsage::Module;
  if (ObjectClass->IsChildOf(UNiagaraScript::StaticClass())) {
    if (!ScriptUsage.IsEmpty() &&
        !TryParseScriptUsage(ScriptUsage, RequestedScriptUsage)) {
      OutError = StateValueFailure(TEXT("niagara_workspace_script_usage_invalid"),
          TEXT("The requested script usage is not a recognized ENiagaraScriptUsage name."), TEXT("script_usage"),
          MakeShared<FJsonValueString>(ScriptUsage), MakeShared<FJsonValueString>(TEXT("ENiagaraScriptUsage enum name")));
      return nullptr;
    }
    if (RequestedScriptUsage != ENiagaraScriptUsage::Module &&
        RequestedScriptUsage != ENiagaraScriptUsage::Function &&
        RequestedScriptUsage != ENiagaraScriptUsage::DynamicInput) {
      OutError = StateValueFailure(TEXT("niagara_workspace_script_usage_unsupported"),
          TEXT("A standalone Niagara script asset must use Module, Function, or DynamicInput."), TEXT("script_usage"),
          MakeShared<FJsonValueString>(ScriptUsageToString(RequestedScriptUsage)),
          MakeShared<FJsonValueString>(TEXT("Module | Function | DynamicInput")));
      return nullptr;
    }

    const UNiagaraEditorSettings *Settings =
        GetDefault<UNiagaraEditorSettings>();
    const FSoftObjectPath *DefaultPath =
        Settings ? &Settings->DefaultModuleScript : nullptr;
    if (Settings && RequestedScriptUsage == ENiagaraScriptUsage::Function) {
      DefaultPath = &Settings->DefaultFunctionScript;
    } else if (Settings &&
               RequestedScriptUsage == ENiagaraScriptUsage::DynamicInput) {
      DefaultPath = &Settings->DefaultDynamicInputScript;
    }
    UNiagaraScript *DefaultScript =
        DefaultPath ? Cast<UNiagaraScript>(DefaultPath->TryLoad()) : nullptr;
    UNiagaraScriptSource *DefaultSource =
        DefaultScript
            ? Cast<UNiagaraScriptSource>(DefaultScript->GetLatestSource())
            : nullptr;
    if (!DefaultScript || !DefaultSource || !DefaultSource->NodeGraph) {
      OutError = StateValueFailure(TEXT("niagara_factory_default_script_unavailable"),
          TEXT("The configured default Niagara script or its editable source graph is unavailable."),
          !DefaultScript ? TEXT("default_script.available") : !DefaultSource ? TEXT("default_script.source.available") : TEXT("default_script.graph.available"),
          MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), true);
      OutError.CandidateName = ScriptUsageToString(RequestedScriptUsage);
      if (DefaultPath) OutError.TargetPath = DefaultPath->GetAssetPathString();
      return nullptr;
    }
    Object = StaticDuplicateObject(DefaultScript, Outer, NAME_None,
                                   RF_Transactional, ObjectClass);
  } else {
    Object =
        NewObject<UObject>(Outer, ObjectClass, NAME_None, RF_Transactional);
  }
  if (!Object) {
    OutError = StateValueFailure(TEXT("niagara_factory_seed_create_failed"),
        TEXT("Unreal could not create the Niagara factory seed object."), TEXT("factory_seed.available"),
        MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), true);
    OutError.CandidateName = ObjectClass->GetName();
    OutError.InternalContext = Outer->GetPathName();
    return nullptr;
  }
  if (UNiagaraSystem *System = Cast<UNiagaraSystem>(Object)) {
    UNiagaraSystemFactoryNew::InitializeSystem(System,
                                               bCreateDefaultSystemNodes);
  } else if (UNiagaraEmitter *Emitter = Cast<UNiagaraEmitter>(Object)) {
    UNiagaraEmitterFactoryNew::InitializeEmitter(
        Emitter, bAddDefaultEmitterModulesAndRenderers);
  } else if (UNiagaraScript *Script = Cast<UNiagaraScript>(Object)) {
    Script->SetUsage(RequestedScriptUsage);
  } else {
    OutError = StateValueFailure(TEXT("niagara_factory_seed_class_unsupported"),
        TEXT("The factory seed class must be a Niagara system, emitter, or script."), TEXT("factory_seed.class"),
        MakeShared<FJsonValueString>(Object->GetClass()->GetName()),
        MakeShared<FJsonValueString>(TEXT("NiagaraSystem | NiagaraEmitter | NiagaraScript")), true);
    return nullptr;
  }
  if (!AssetKind.IsEmpty() &&
      !GetAssetKind(Object).Equals(AssetKind, ESearchCase::IgnoreCase)) {
    OutError = StateValueFailure(TEXT("niagara_factory_seed_kind_mismatch"),
        TEXT("The created Niagara factory seed has a different asset kind from the requested kind."), TEXT("asset_kind"),
        MakeShared<FJsonValueString>(GetAssetKind(Object)), MakeShared<FJsonValueString>(AssetKind), true);
    return nullptr;
  }
  return Object;
}

bool RegisterStateAccessors(UE::UAI::Contract::FDiagnostic& OutError) {
  UeAgentAuthoringState::FStateAccessorDescriptor Descriptor;
  Descriptor.AccessorId = StateAccessorId;
  Descriptor.TargetKinds = {
      TEXT("niagara_asset"),         TEXT("niagara_system"),
      TEXT("niagara_emitter"),       TEXT("niagara_script"),
      TEXT("niagara_stack_entry"),   TEXT("niagara_module"),
      TEXT("niagara_dynamic_input"), TEXT("niagara_emitter_owner"),
      TEXT("niagara_renderer"),      TEXT("niagara_event_handler"),
      TEXT("niagara_user_parameter")};
  Descriptor.CapabilityTags = {TEXT("asset"),         TEXT("stack"),
                               TEXT("module"),        TEXT("dynamic_input"),
                               TEXT("renderer"),      TEXT("event_handler"),
                               TEXT("user_parameter")};
  UeAgentAuthoringState::ERegistrationResult Result;
  return UeAgentAuthoringState::FStateAccessorRegistry::Get().RegisterAccessor(
      Descriptor, ReadState, Result, OutError);
}

bool RegisterNativeDefaultProviders(UE::UAI::Contract::FDiagnostic& OutError) {
  UeAgentAuthoringNativeDefaults::ERegistrationResult Result;
  UeAgentAuthoringNativeDefaults::FProviderDescriptor Factory;
  Factory.ProviderId = FactoryDefaultProviderId;
  Factory.SourceKind =
      UeAgentAuthoringSemanticRegistry::EDefaultSourceKind::Factory;
  Factory.TargetKinds = {TEXT("niagara_system"), TEXT("niagara_emitter")};
  Factory.CapabilityTags = {TEXT("factory"), TEXT("asset")};
  Factory.bRequiresScratch = true;
  if (!UeAgentAuthoringNativeDefaults::FProviderRegistry::Get()
           .RegisterProvider(Factory, ProbeNativeDefault, Result, OutError))
    return false;

  UeAgentAuthoringNativeDefaults::FProviderDescriptor Schema;
  Schema.ProviderId = SchemaDefaultProviderId;
  Schema.SourceKind =
      UeAgentAuthoringSemanticRegistry::EDefaultSourceKind::Schema;
  Schema.TargetKinds = {TEXT("niagara_stack_entry")};
  Schema.CapabilityTags = {TEXT("schema"), TEXT("stack")};
  Schema.bRequiresScratch = false;
  if (!UeAgentAuthoringNativeDefaults::FProviderRegistry::Get()
           .RegisterProvider(Schema, ProbeNativeDefault, Result, OutError))
    return false;

  UeAgentAuthoringNativeDefaults::FProviderDescriptor DomainNative;
  DomainNative.ProviderId = DomainNativeDefaultProviderId;
  DomainNative.SourceKind =
      UeAgentAuthoringSemanticRegistry::EDefaultSourceKind::DomainNative;
  DomainNative.TargetKinds = {
      TEXT("niagara_script"),        TEXT("niagara_module"),
      TEXT("niagara_dynamic_input"), TEXT("niagara_emitter_owner"),
      TEXT("niagara_renderer"),      TEXT("niagara_event_handler"),
      TEXT("niagara_user_parameter")};
  DomainNative.CapabilityTags = {TEXT("domain_api"),    TEXT("script"),
                                 TEXT("module"),        TEXT("dynamic_input"),
                                 TEXT("renderer"),      TEXT("event_handler"),
                                 TEXT("user_parameter")};
  DomainNative.bRequiresScratch = true;
  return UeAgentAuthoringNativeDefaults::FProviderRegistry::Get()
      .RegisterProvider(DomainNative, ProbeNativeDefault, Result, OutError);
}
} // namespace UeAgentNiagaraAuthoring

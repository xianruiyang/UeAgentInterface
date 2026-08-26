// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/Niagara/Authoring/UeAgentNiagaraAuthoringApplyProvider.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Domains/Niagara/Authoring/UeAgentNiagaraAuthoringDescriptors.h"
#include "Domains/Niagara/Authoring/UeAgentNiagaraAuthoringScriptResolver.h"
#include "Domains/Niagara/Authoring/UeAgentNiagaraAuthoringStateAccessors.h"
#include "Domains/Niagara/Authoring/UeAgentNiagaraStageModuleAuthoring.h"
#include "Domains/Niagara/Authoring/UeAgentNiagaraStandaloneModuleAuthoring.h"
#include "Domains/Niagara/Authoring/UeAgentNiagaraUserParameterAuthoring.h"
#include "Domains/Niagara/UeAgentNiagaraCompileValidation.h"
#include "Domains/Niagara/UeAgentNiagaraEmitterCompileContext.h"
#include "Domains/Niagara/UeAgentNiagaraStackValidation.h"
#include "Domains/Niagara/UeAgentNiagaraSystemStructuralRefresh.h"
#include "HAL/FileManager.h"
#include "INiagaraMergeManager.h"
#include "Misc/PackageName.h"
#include "Module/UeAgentInterfaceLogger.h"
#include "NiagaraDataInterface.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraGraph.h"
#include "NiagaraNode.h"
#include "NiagaraNodeAssignment.h"
#include "NiagaraNodeFunctionCall.h"
#include "NiagaraNodeInput.h"
#include "NiagaraNodeOutput.h"
#include "NiagaraRendererProperties.h"
#include "NiagaraScript.h"
#include "NiagaraScriptSource.h"
#include "NiagaraSystem.h"
#include "ObjectTools.h"
#include "QueryOwners/Niagara/UeAgentNiagaraModuleCandidateQuery.h"
#include "UObject/SavePackage.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectHash.h"
#include "UObject/UnrealType.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringIdentity.h"
#include "Utilities/AuthoringCore/UeAgentDetailsAuthoringService.h"
#include "Utilities/AuthoringObjectApply/UeAgentAuthoringObjectApply.h"
#include "Utilities/AuthoringOperationApply/UeAgentAuthoringAssetRemovalStage.h"
#include "Utilities/AuthoringOperationApply/UeAgentAuthoringObjectApplyProvider.h"
#include "Utilities/AuthoringOperationApply/UeAgentAuthoringOperationCoordinator.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentApplyWorkspaceContract.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringWorkspaceDependencyGraph.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringWorkspaceLoader.h"
#include "Utilities/UeAgentStructuredHjsonFormatter.h"

namespace UeAgentNiagaraAuthoring {
namespace {
using namespace UeAgentApplyInputContract;
using namespace UeAgentAuthoringOperationApply;

FString NormalizePackagePath(FString Input) {
  Input.TrimStartAndEndInline();
  if (Input.Contains(TEXT("."))) {
    const FString Package = FPackageName::ObjectPathToPackageName(Input);
    if (!Package.IsEmpty())
      return Package;
  }
  return Input;
}

FString ToObjectPath(const FString &PackagePath) {
  const FString AssetName = FPackageName::GetLongPackageAssetName(PackagePath);
  return AssetName.IsEmpty()
             ? FString()
             : FString::Printf(TEXT("%s.%s"), *PackagePath, *AssetName);
}

UObject *LoadAsset(const FString &Path) {
  const FString PackagePath = NormalizePackagePath(Path);
  const FString ObjectPath = ToObjectPath(PackagePath);
  if (ObjectPath.IsEmpty())
    return nullptr;
  const FString AssetName = FPackageName::GetLongPackageAssetName(PackagePath);
  if (UPackage *LoadedPackage = FindPackage(nullptr, *PackagePath)) {
    if (UObject *LoadedObject = StaticFindObject(UObject::StaticClass(),
                                                 LoadedPackage, *AssetName)) {
      return LoadedObject;
    }
  }
  if (UObject *LoadedObject =
          StaticFindObject(UObject::StaticClass(), nullptr, *ObjectPath)) {
    return LoadedObject;
  }
  return StaticLoadObject(UObject::StaticClass(), nullptr, *ObjectPath);
}

bool ReadBool(const TSharedPtr<const FJsonObject> &Object, const TCHAR *Field,
              const bool DefaultValue) {
  bool Value = DefaultValue;
  if (Object.IsValid())
    Object->TryGetBoolField(Field, Value);
  return Value;
}

FString IdentityModelForKind(const FString &Kind) {
  if (Kind == TEXT("system"))
    return TEXT("niagara_system_identity");
  if (Kind == TEXT("emitter"))
    return TEXT("niagara_emitter_identity");
  if (Kind == TEXT("script"))
    return TEXT("niagara_script_identity");
  return FString();
}

FString TargetKindForKind(const FString &Kind) {
  return Kind.IsEmpty() ? FString() : TEXT("niagara_") + Kind;
}

FString SemanticForKind(const FString &Kind) {
  if (Kind == TEXT("system"))
    return SystemSemanticId;
  if (Kind == TEXT("emitter"))
    return EmitterSemanticId;
  if (Kind == TEXT("script"))
    return ScriptSemanticId;
  return FString();
}

void SetFailure(FOperationFailure &OutFailure, const FString &Code,
                const FString &Detail = FString()) {
  OutFailure = FOperationFailure();
  OutFailure.ErrorCode = Code;
  OutFailure.ErrorDetail = Detail.IsEmpty() ? Code : Detail;
}

int32 RepairInvalidEmitterVersions(UNiagaraSystem &System) {
  int32 RepairedCount = 0;
  for (FNiagaraEmitterHandle &Handle : System.GetEmitterHandles()) {
    FVersionedNiagaraEmitter Instance = Handle.GetInstance();
    if (!Instance.Emitter ||
        (Instance.Version.IsValid() && Instance.GetEmitterData())) {
      continue;
    }
    const FGuid ExposedVersion =
        Instance.Emitter->GetExposedVersion().VersionGuid;
    if (!ExposedVersion.IsValid()) {
      continue;
    }
    if (!System.ChangeEmitterVersion(Instance, ExposedVersion)) {
      Handle.GetInstance().Version = ExposedVersion;
    }
    ++RepairedCount;
  }
  return RepairedCount;
}

int32 CountInvalidEmitterVersions(const UNiagaraSystem &System) {
  int32 Count = 0;
  for (const FNiagaraEmitterHandle &Handle : System.GetEmitterHandles()) {
    const FVersionedNiagaraEmitter Instance = Handle.GetInstance();
    if (Instance.Emitter &&
        (!Instance.Version.IsValid() || !Instance.GetEmitterData())) {
      ++Count;
    }
  }
  return Count;
}

bool ValidateAssetForDuplication(const UObject *Asset, FString &OutError) {
  if (!Asset) {
    OutError = TEXT("niagara_duplicate_source_missing");
    return false;
  }
  if (const UNiagaraSystem *System = Cast<UNiagaraSystem>(Asset)) {
    const UNiagaraScript *SpawnScript = System->GetSystemSpawnScript();
    const UNiagaraScript *UpdateScript = System->GetSystemUpdateScript();
    if (!SpawnScript || !UpdateScript || !SpawnScript->GetLatestSource() ||
        !UpdateScript->GetLatestSource()) {
      OutError = TEXT("niagara_system_duplicate_source_incomplete");
      return false;
    }
  }
  OutError.Reset();
  return true;
}

bool DeleteAssetObject(UObject *Asset, const bool bForce, const bool bUnchecked,
                       FString &OutError) {
  if (!Asset) {
    OutError = TEXT("niagara_delete_target_missing");
    return false;
  }
  TGuardValue<bool> UnattendedGuard(GIsRunningUnattendedScript, true);
  bool bDeleted = false;
  if (bForce) {
    TArray<UObject *> Objects = {Asset};
    bDeleted = bUnchecked ? ObjectTools::DeleteObjectsUnchecked(Objects) > 0
                          : ObjectTools::ForceDeleteObjects(Objects, false) > 0;
  } else {
    bDeleted = ObjectTools::DeleteSingleObject(Asset, true);
  }
  if (!bDeleted) {
    OutError = bForce ? TEXT("force_delete_asset_failed")
                      : TEXT("delete_asset_failed_or_blocked_by_references");
  }
  return bDeleted;
}

bool SaveAssetObject(UObject *Asset, FString &OutError) {
  if (!Asset || !Asset->GetOutermost()) {
    OutError = TEXT("niagara_save_target_missing");
    return false;
  }
  UPackage *Package = Asset->GetOutermost();
  const FString PackageName = Package->GetName();
  const FString Filename = FPackageName::LongPackageNameToFilename(
      PackageName, FPackageName::GetAssetPackageExtension());
  FSavePackageArgs Args;
  Args.TopLevelFlags = RF_Public | RF_Standalone;
  Args.SaveFlags = SAVE_NoError;
  if (!UPackage::SavePackage(Package, Asset, *Filename, Args)) {
    OutError = TEXT("save_asset_failed");
    return false;
  }
  return true;
}

UObject *DuplicatePublishedAsset(UObject *Source, UPackage *TargetPackage,
                                 const FString &AssetName) {
  if (!Source || !TargetPackage || AssetName.IsEmpty())
    return nullptr;
  UObject *Published =
      StaticDuplicateObject(Source, TargetPackage, *AssetName, RF_AllFlags);
  if (Published) {
    Published->ClearFlags(RF_Transient);
    Published->SetFlags(RF_Public | RF_Standalone | RF_Transactional);
  }
  return Published;
}

struct FAssetCompileReceipt {
  FString Status = TEXT("not_requested");
  bool bCompilationComplete = false;
  int32 ErrorCount = 0;
  int32 WarningCount = 0;
  int32 StackIssueCount = 0;
  int32 StackWarningCount = 0;
  TArray<FString> CompiledTargets;
};

bool CompileAssetObject(UObject *Asset, FAssetCompileReceipt &OutReceipt,
                        FString &OutError) {
  OutReceipt = FAssetCompileReceipt();
  UeAgentNiagaraCompileValidation::FResult Validation;
  if (UNiagaraSystem *System = Cast<UNiagaraSystem>(Asset)) {
    System->RequestCompile(true);
    System->WaitForCompilationComplete(true, false);
    System->PollForCompilationComplete(true);
    if (System->HasOutstandingCompilationRequests(true)) {
      OutReceipt.Status = TEXT("incomplete");
      OutError = TEXT("niagara_compile_incomplete");
      return false;
    }
    System->CacheFromCompiledData();
    Validation = UeAgentNiagaraCompileValidation::InspectAsset(System);
  } else if (UNiagaraEmitter *Emitter = Cast<UNiagaraEmitter>(Asset)) {
    UeAgentNiagaraEmitterCompileContext::FResult CompileResult;
    if (!UeAgentNiagaraEmitterCompileContext::Compile(
            *Emitter, Emitter->GetExposedVersion().VersionGuid, true,
            CompileResult, OutError)) {
      OutReceipt.Status = TEXT("failed");
      return false;
    }
    if (!CompileResult.bCompilationComplete) {
      OutReceipt.Status = TEXT("incomplete");
      OutError = TEXT("niagara_compile_incomplete");
      return false;
    }
    Validation = CompileResult.Validation;
  } else if (UNiagaraScript *Script = Cast<UNiagaraScript>(Asset)) {
    Script->RequestCompile(Script->GetExposedVersion().VersionGuid, true);
    Validation = UeAgentNiagaraCompileValidation::InspectAsset(Script);
  } else {
    OutReceipt.Status = TEXT("failed");
    OutError = TEXT("niagara_compile_target_invalid");
    return false;
  }

  OutReceipt.bCompilationComplete = Validation.bCompilationComplete;
  OutReceipt.ErrorCount = Validation.ErrorScriptCount;
  OutReceipt.WarningCount = Validation.WarningScriptCount;
  OutReceipt.CompiledTargets = Validation.CompiledTargets;
  if (!Validation.bCompilationComplete) {
    OutReceipt.Status = TEXT("incomplete");
    OutError = TEXT("niagara_compile_incomplete");
    return false;
  }
  if (Validation.bHasError) {
    OutReceipt.Status = TEXT("failed");
    OutError = FString::Printf(
        TEXT("niagara_compile_failed:error_script_count=%d:first_error=%s"),
        Validation.ErrorScriptCount, *Validation.FirstError);
    return false;
  }

  const UeAgentNiagaraStackValidation::FResult StackResult =
      UeAgentNiagaraStackValidation::InspectAsset(Asset);
  OutReceipt.StackIssueCount = StackResult.ErrorCount;
  OutReceipt.StackWarningCount = StackResult.WarningCount;
  if (!StackResult.bInspectionComplete) {
    OutReceipt.Status = TEXT("stack_inspection_failed");
    OutError = StackResult.FirstError.IsEmpty()
                   ? TEXT("niagara_stack_inspection_failed")
                   : StackResult.FirstError;
    return false;
  }
  if (OutReceipt.StackIssueCount > 0) {
    OutReceipt.Status = TEXT("stack_issues");
    OutError = FString::Printf(
        TEXT("niagara_stack_validation_failed:error_count=%d:warning_count=%d"),
        StackResult.ErrorCount, StackResult.WarningCount);
    return false;
  }
  OutReceipt.Status = TEXT("complete");
  OutError.Reset();
  return true;
}

bool CompileAssetObject(UObject *Asset, FString &OutError) {
  FAssetCompileReceipt Receipt;
  return CompileAssetObject(Asset, Receipt, OutError);
}

struct FLiteralInputPatch {
  FString InputName;
  TSharedPtr<const FJsonObject> Payload;
  bool bNeedsMutation = true;
};

struct FDataInterfacePatch {
  FString InputName;
  FString DataInterfaceClass;
  TSharedPtr<FJsonObject> Properties;
};

struct FDynamicInputPatch {
  FString InputName;
  bool bRemove = false;
  bool bNeedsMutation = false;
  TStrongObjectPtr<UNiagaraScript> Script;
  TArray<FLiteralInputPatch> Inputs;
  TArray<FDataInterfacePatch> DataInterfaces;
  TArray<TSharedPtr<FDynamicInputPatch>> DynamicInputs;
};

void MarkDynamicInputPatchForRebuild(FDynamicInputPatch &Patch) {
  TArray<FDynamicInputPatch *> Pending;
  Pending.Add(&Patch);
  while (!Pending.IsEmpty()) {
    FDynamicInputPatch *Current = Pending.Pop(EAllowShrinking::No);
    if (!Current)
      continue;
    Current->bNeedsMutation = true;
    for (FLiteralInputPatch &Input : Current->Inputs)
      Input.bNeedsMutation = true;
    for (const TSharedPtr<FDynamicInputPatch> &Child :
         Current->DynamicInputs) {
      if (Child.IsValid())
        Pending.Add(Child.Get());
    }
  }
}

struct FModulePatch {
  FString SourceKey;
  FString NativeCandidateId;
  FGuid ModuleGuid;
  FGuid LegacyNodeGuid;
  bool bRemove = false;
  bool bExists = false;
  bool bReplaceScript = false;
  bool bAssignmentModule = false;
  bool bStandaloneAssignment = false;
  bool bLegacyStandaloneNode = false;
  bool bAssignmentTargetsExplicit = false;
  bool bNeedsMutation = false;
  int32 ExistingIndex = INDEX_NONE;
  int32 TargetIndex = INDEX_NONE;
  TStrongObjectPtr<UNiagaraScript> Script;
  TArray<FNiagaraVariable> AssignmentTargets;
  TArray<FLiteralInputPatch> Inputs;
  TArray<FDynamicInputPatch> DynamicInputs;
};

bool AssignmentTargetsMatch(const UNiagaraNodeAssignment &Assignment,
                            const TArray<FNiagaraVariable> &RequestedTargets) {
  if (Assignment.NumTargets() != RequestedTargets.Num())
    return false;
  for (const FNiagaraVariable &Requested : RequestedTargets) {
    const bool bFound = Assignment.GetAssignmentTargets().ContainsByPredicate(
        [&Requested](const FNiagaraVariable &Existing) {
          return Existing.GetName() == Requested.GetName() &&
                 Existing.GetType() == Requested.GetType();
        });
    if (!bFound) {
      return false;
    }
  }
  return true;
}

bool ParseAssignmentTargets(const TSharedPtr<FJsonObject> &Object,
                            FModulePatch &OutPatch, FString &OutError) {
  OutPatch.bAssignmentTargetsExplicit =
      Object.IsValid() && Object->HasField(TEXT("assignment_targets"));
  if (!OutPatch.bAssignmentTargetsExplicit)
    return true;
  const TSharedPtr<FJsonObject> *Targets = nullptr;
  if (!Object->TryGetObjectField(TEXT("assignment_targets"), Targets) ||
      !Targets || !Targets->IsValid()) {
    OutError = TEXT("niagara_workspace_assignment_targets_invalid");
    return false;
  }
  TArray<FString> TargetNames;
  (*Targets)->Values.GetKeys(TargetNames);
  TargetNames.Sort();
  for (const FString &TargetName : TargetNames) {
    const TSharedPtr<FJsonValue> TargetValue =
        (*Targets)->Values.FindRef(TargetName);
    FString TypeText;
    if (TargetValue.IsValid() && TargetValue->Type == EJson::String) {
      TypeText = TargetValue->AsString();
    } else if (TargetValue.IsValid() && TargetValue->Type == EJson::Object) {
      TargetValue->AsObject()->TryGetStringField(TEXT("type"), TypeText);
    } else if (!TargetValue.IsValid() || TargetValue->Type != EJson::Null) {
      OutError =
          TEXT("niagara_workspace_assignment_target_invalid:") + TargetName;
      return false;
    }
    FNiagaraVariable Variable;
    if (!UeAgentNiagaraStageModuleAuthoring::ResolveAssignmentTargetVariable(
            TargetName, TypeText, Variable, OutError))
      return false;
    OutPatch.AssignmentTargets.Add(MoveTemp(Variable));
  }
  if (OutPatch.AssignmentTargets.IsEmpty()) {
    OutError = TEXT("niagara_workspace_assignment_targets_missing");
    return false;
  }
  return true;
}

struct FStagePatch {
  FString SourceKey;
  UeAgentNiagaraStageModuleAuthoring::FStageTarget Target;
  UeAgentNiagaraStageModuleAuthoring::FResolvedStage Stage;
  TArray<FModulePatch> Modules;
  bool bReplaceStage = false;
  bool bNeedsMutation = false;
};

struct FRendererPatch {
  FString RendererName;
  FString RendererClass;
  int32 TargetIndex = INDEX_NONE;
  bool bRemove = false;
  TSharedPtr<FJsonObject> Properties;
};

struct FEventHandlerPatch {
  FGuid UsageId;
  bool bRemove = false;
  FName SourceEventName = TEXT("Event");
  FGuid SourceEmitterId;
  EScriptExecutionMode ExecutionMode = EScriptExecutionMode::EveryParticle;
  uint32 SpawnNumber = 1;
  uint32 MinSpawnNumber = 0;
  bool bRandomSpawnNumber = false;
  uint32 MaxEventsPerFrame = 0;
  bool bUpdateAttributeInitialValues = false;
};

enum class EEmitterSourceSyncMode : uint8 {
  Merge,
  Replace,
};

struct FEmitterStructurePatch {
  FString SourceKey;
  UeAgentNiagaraStageModuleAuthoring::FEmitterOwnerTarget Target;
  UeAgentNiagaraStageModuleAuthoring::FResolvedEmitterOwner Owner;
  TWeakObjectPtr<UNiagaraSystem> TargetSystem;
  TStrongObjectPtr<UNiagaraEmitter> SourceEmitter;
  FGuid SourceEmitterVersion;
  FGuid AddedHandleId;
  bool bAddEmitterHandle = false;
  bool bAddedEmitterHandle = false;
  bool bRefreshEmitterFromSource = false;
  bool bReplaceEmitterFromSource = false;
  bool bReplacedEmitterFromSource = false;
  TStrongObjectPtr<UNiagaraEmitter> PreviousEmitterInstance;
  FGuid PreviousEmitterVersion;
  bool bEnsureSystemIntegration = false;
  TOptional<bool> Enabled;
  bool bEmitterEnabledNeedsMutation = false;
  bool bEmitterEnabledChanged = false;
  bool bRestoreEmitterEnabledOnRollback = false;
  bool bPreviousEmitterEnabled = true;
  TSharedPtr<FJsonObject> Properties;
  bool bReplaceRenderers = false;
  TArray<FRendererPatch> Renderers;
  TArray<FEventHandlerPatch> EventHandlers;
  bool bNeedsMutation = false;
};

struct FGraphBackup {
  TWeakObjectPtr<UNiagaraScriptSource> Source;
  TStrongObjectPtr<UNiagaraGraph> Graph;
  FName OriginalName;
};

struct FRendererBackup {
  TStrongObjectPtr<UNiagaraRendererProperties> Snapshot;
  FName OriginalName;
};

struct FEmitterStructureBackup {
  TWeakObjectPtr<UNiagaraEmitter> Emitter;
  FGuid EmitterVersion;
  TSharedPtr<FJsonObject> Properties;
  TArray<FRendererBackup> Renderers;
  TArray<FNiagaraEventScriptProperties> EventHandlers;
};

bool ValidateEmitterStructurePropertyPatches(
    const FEmitterStructurePatch &Patch, FString &OutError);

void NormalizeModuleTargetIndices(FStagePatch &StagePatch) {
  TArray<FModulePatch *> OrderedPatches;
  for (FModulePatch &Patch : StagePatch.Modules) {
    if (!Patch.bRemove && Patch.TargetIndex != INDEX_NONE) {
      OrderedPatches.Add(&Patch);
    }
  }
  OrderedPatches.Sort([](const FModulePatch &Left, const FModulePatch &Right) {
    if (Left.TargetIndex != Right.TargetIndex)
      return Left.TargetIndex < Right.TargetIndex;
    return Left.SourceKey < Right.SourceKey;
  });
  int32 LastTargetIndex = INDEX_NONE;
  for (FModulePatch *Patch : OrderedPatches) {
    Patch->TargetIndex = FMath::Max(Patch->TargetIndex, LastTargetIndex + 1);
    LastTargetIndex = Patch->TargetIndex;
    if (Patch->bExists && Patch->ExistingIndex != Patch->TargetIndex) {
      Patch->bNeedsMutation = true;
    }
  }
  for (const FModulePatch &Patch : StagePatch.Modules) {
    StagePatch.bNeedsMutation |= Patch.bNeedsMutation;
  }
}

bool ReadStringField(const TSharedPtr<const FJsonObject> &Object,
                     const TCHAR *Field, FString &OutValue) {
  OutValue.Reset();
  return Object.IsValid() && Object->TryGetStringField(Field, OutValue) &&
         !OutValue.IsEmpty();
}

bool ResolvePatchScript(const FString &ScriptPath, const EScriptKind Kind,
                        TStrongObjectPtr<UNiagaraScript> &OutScript,
                        FString &OutError) {
  TSharedRef<FJsonObject> ScriptReference = MakeShared<FJsonObject>();
  ScriptReference->SetStringField(TEXT("script_asset_path"), ScriptPath);
  FScriptResolveRequest Request;
  Request.Kind = Kind;
  Request.Input = ScriptReference;
  FScriptResolveResult Result;
  if (!ResolveScript(Request, Result, OutError))
    return false;
  OutScript.Reset(Result.Script);
  return OutScript.IsValid();
}

bool ParseStageTarget(
    const FString &AssetPath, const TSharedPtr<const FJsonObject> &Object,
    UeAgentNiagaraStageModuleAuthoring::FStageTarget &OutTarget,
    FString &OutError) {
  using namespace UeAgentNiagaraStageModuleAuthoring;
  OutTarget = FStageTarget();
  OutTarget.AssetPath = AssetPath;
  if (!Object.IsValid()) {
    OutError = TEXT("niagara_workspace_stage_target_missing");
    return false;
  }
  Object->TryGetStringField(TEXT("emitter_name"), OutTarget.EmitterName);
  FString EmitterHandleId;
  if (Object->TryGetStringField(TEXT("emitter_handle_id"), EmitterHandleId) &&
      !EmitterHandleId.IsEmpty()) {
    if (!FGuid::Parse(EmitterHandleId, OutTarget.EmitterHandleId)) {
      OutError = TEXT("niagara_workspace_emitter_handle_id_invalid");
      return false;
    }
    OutTarget.bEmitterHandleIdExplicit = true;
  }
  FString Usage;
  if (!Object->TryGetStringField(TEXT("usage"), Usage) ||
      !TryParseScriptUsage(Usage, OutTarget.Usage)) {
    OutError = TEXT("niagara_workspace_stage_usage_invalid");
    return false;
  }
  OutTarget.bUsageExplicit = true;
  FString UsageId;
  if (Object->TryGetStringField(TEXT("usage_id"), UsageId) &&
      !UsageId.IsEmpty()) {
    if (!FGuid::Parse(UsageId, OutTarget.UsageId)) {
      OutError = TEXT("niagara_workspace_stage_usage_id_invalid");
      return false;
    }
    OutTarget.bUsageIdExplicit = true;
  }
  return true;
}

bool ParseLiteralInputPatches(const TSharedPtr<const FJsonObject> &Owner,
                              TArray<FLiteralInputPatch> &OutPatches,
                              FString &OutError) {
  OutPatches.Reset();
  const TSharedPtr<FJsonObject> *Inputs = nullptr;
  if (!Owner.IsValid() || !Owner->TryGetObjectField(TEXT("inputs"), Inputs))
    return true;
  if (!Inputs || !Inputs->IsValid()) {
    OutError = TEXT("niagara_workspace_inputs_invalid");
    return false;
  }
  for (const TPair<FString, TSharedPtr<FJsonValue>> &Pair : (*Inputs)->Values) {
    if (Pair.Key.IsEmpty() || !Pair.Value.IsValid() ||
        Pair.Value->Type != EJson::Object) {
      OutError = TEXT("niagara_workspace_literal_input_invalid:") + Pair.Key;
      return false;
    }
    FLiteralInputPatch &Patch = OutPatches.AddDefaulted_GetRef();
    Patch.InputName = Pair.Key;
    Patch.Payload = Pair.Value->AsObject();
  }
  return true;
}

bool ResolveLiteralInputMutationFlags(
    UNiagaraNodeFunctionCall *ExistingModule,
    UNiagaraNode *ExistingStandaloneAssignment,
    TArray<FLiteralInputPatch> &Patches, FString &OutError) {
  using namespace UeAgentNiagaraStageModuleAuthoring;
  using namespace UeAgentNiagaraStandaloneModuleAuthoring;
  for (FLiteralInputPatch &Patch : Patches) {
    Patch.bNeedsMutation = true;
    bool bMatches = false;
    if (ExistingStandaloneAssignment) {
      FNiagaraVariable TargetVariable;
      UEdGraphPin *TargetPin = FindAssignmentTargetPin(
          *ExistingStandaloneAssignment, Patch.InputName, TargetVariable);
      if (!TargetPin)
        continue;
      if (!DoesInputValueMatchPin(*TargetPin, TargetVariable.GetType(),
                                  Patch.Payload, bMatches, OutError))
        return false;
    } else if (ExistingModule) {
      FString ResolvedInputName;
      if (!DoesInputValueMatch(*ExistingModule, Patch.InputName, Patch.Payload,
                               ResolvedInputName, bMatches, OutError))
        return false;
      if (!ResolvedInputName.IsEmpty())
        Patch.InputName = ResolvedInputName;
    }
    Patch.bNeedsMutation = !bMatches;
  }
  return true;
}

FString EscapeWorkspacePointerToken(FString Token) {
  Token.ReplaceInline(TEXT("~"), TEXT("~0"));
  Token.ReplaceInline(TEXT("/"), TEXT("~1"));
  return Token;
}

bool ParseDynamicInputPatch(
    UNiagaraNodeFunctionCall *ExistingModule, UEdGraphPin *ExistingInputPin,
    const FString &InputKey, const TSharedPtr<FJsonValue> &Value,
    FUeAgentStructuredResourceTracker &ResourceTracker,
    const FString &RootPointer, FDynamicInputPatch &OutPatch,
    FString &OutError) {
  using namespace UeAgentNiagaraStageModuleAuthoring;
  struct FParseFrame {
    UNiagaraNodeFunctionCall *ExistingModule = nullptr;
    UEdGraphPin *ExistingInputPin = nullptr;
    FString InputKey;
    TSharedPtr<FJsonValue> Value;
    FDynamicInputPatch *Patch = nullptr;
    UNiagaraNodeFunctionCall *ExistingDynamic = nullptr;
    TSharedPtr<const FJsonObject> Object;
    FString Pointer;
    bool bFinalize = false;
  };

  OutPatch = FDynamicInputPatch();
  TArray<FParseFrame> Stack;
  FParseFrame Root;
  Root.ExistingModule = ExistingModule;
  Root.ExistingInputPin = ExistingInputPin;
  Root.InputKey = InputKey;
  Root.Value = Value;
  Root.Patch = &OutPatch;
  Root.Pointer = RootPointer;
  Stack.Add(MoveTemp(Root));
  TSet<const FJsonObject *> ActiveObjects;

  // Budget failures use the shared resource, limit, consumed and pointer
  // diagnostic contract and occur before graph mutation.
  while (!Stack.IsEmpty()) {
    FParseFrame Frame = Stack.Pop(EAllowShrinking::No);
    if (!Frame.Patch) {
      OutError = TEXT("niagara_workspace_dynamic_input_frame_invalid");
      return false;
    }
    if (Frame.bFinalize) {
      if (!ResolveLiteralInputMutationFlags(
              Frame.ExistingDynamic, nullptr, Frame.Patch->Inputs, OutError))
        return false;
      if (Frame.Patch->bRemove) {
        Frame.Patch->bNeedsMutation = Frame.ExistingDynamic != nullptr;
      } else {
        const FString CurrentPath =
            Frame.ExistingDynamic
                ? GetModuleScriptPath(*Frame.ExistingDynamic)
                : FString();
        const FString DesiredPath =
            Frame.Patch->Script.IsValid()
                ? Frame.Patch->Script->GetPathName()
                : CurrentPath;
        const bool bScriptNeedsMutation =
            !CurrentPath.Equals(DesiredPath, ESearchCase::IgnoreCase);
        const bool bLiteralNeedsMutation =
            Frame.Patch->Inputs.ContainsByPredicate(
                [](const FLiteralInputPatch &Patch) {
                  return Patch.bNeedsMutation;
                });
        const bool bChildNeedsMutation =
            Frame.Patch->DynamicInputs.ContainsByPredicate(
                [](const TSharedPtr<FDynamicInputPatch> &Patch) {
                  return Patch.IsValid() && Patch->bNeedsMutation;
                });
        Frame.Patch->bNeedsMutation =
            bScriptNeedsMutation || bLiteralNeedsMutation ||
            !Frame.Patch->DataInterfaces.IsEmpty() ||
            bChildNeedsMutation;
        if (bScriptNeedsMutation)
          MarkDynamicInputPatchForRebuild(*Frame.Patch);
      }
      for (const FLiteralInputPatch &LiteralPatch : Frame.Patch->Inputs) {
        for (const TSharedPtr<FDynamicInputPatch> &DynamicPatch :
             Frame.Patch->DynamicInputs) {
          if (DynamicPatch.IsValid() &&
              LiteralPatch.InputName.Equals(
                  DynamicPatch->InputName, ESearchCase::IgnoreCase)) {
            OutError = TEXT("niagara_workspace_input_source_conflict:") +
                       LiteralPatch.InputName;
            return false;
          }
        }
      }
      if (Frame.Object.IsValid())
        ActiveObjects.Remove(Frame.Object.Get());
      continue;
    }

    if (!ResourceTracker.ChargeNodes(1, Frame.Pointer, OutError) ||
        !ResourceTracker.ChargeItems(1, Frame.Pointer, OutError) ||
        !ResourceTracker.ChargeWorkUnits(1, Frame.Pointer, OutError) ||
        !ResourceTracker.ChargeEstimatedMemoryBytes(
            sizeof(FDynamicInputPatch) + sizeof(FParseFrame), Frame.Pointer,
            OutError)) {
      return false;
    }

    FDynamicInputPatch &Patch = *Frame.Patch;
    Patch = FDynamicInputPatch();
    Patch.InputName = Frame.InputKey;
    if (!Frame.Value.IsValid() || Frame.Value->Type == EJson::Null) {
      Patch.bRemove = true;
    } else if (Frame.Value->Type == EJson::Object) {
      Frame.Object = Frame.Value->AsObject();
      if (!Frame.Object.IsValid()) {
        OutError = TEXT("niagara_workspace_dynamic_input_invalid");
        return false;
      }
      if (ActiveObjects.Contains(Frame.Object.Get())) {
        OutError =
            TEXT("niagara_workspace_dynamic_input_cycle_detected:pointer=") +
            Frame.Pointer;
        return false;
      }
      ActiveObjects.Add(Frame.Object.Get());
      Frame.Object->TryGetStringField(TEXT("input_name"), Patch.InputName);
      if (Patch.InputName.IsEmpty())
        Patch.InputName = Frame.InputKey;
      Frame.Object->TryGetBoolField(TEXT("remove"), Patch.bRemove);
      if (!ParseLiteralInputPatches(
              Frame.Object, Patch.Inputs, OutError))
        return false;
    } else {
      OutError = TEXT("niagara_workspace_dynamic_input_invalid");
      return false;
    }
    if (Patch.InputName.IsEmpty()) {
      OutError = TEXT("niagara_workspace_dynamic_input_name_missing");
      return false;
    }

    FString ResolvedInputName;
    if (Frame.ExistingInputPin) {
      Frame.ExistingDynamic =
          FindDynamicInputForPin(*Frame.ExistingInputPin);
      ResolvedInputName = Frame.ExistingInputPin->PinName.ToString();
    } else if (Frame.ExistingModule) {
      Frame.ExistingDynamic = FindDynamicInputForModuleInput(
          *Frame.ExistingModule, Patch.InputName, ResolvedInputName);
      if (!ResolvedInputName.IsEmpty())
        Patch.InputName = ResolvedInputName;
    }

    TArray<FParseFrame> ChildFrames;
    if (!Patch.bRemove && Frame.Object.IsValid()) {
      FString ScriptPath;
      if (ReadStringField(
              Frame.Object, TEXT("script_asset_path"), ScriptPath)) {
        if (!ResolvePatchScript(
                ScriptPath, EScriptKind::DynamicInput, Patch.Script,
                OutError))
          return false;
      } else if (!Frame.ExistingDynamic) {
        OutError = TEXT("niagara_workspace_dynamic_input_script_missing");
        return false;
      }

      const TSharedPtr<FJsonObject> *DataInterfaces = nullptr;
      if (Frame.Object->TryGetObjectField(
              TEXT("data_interfaces"), DataInterfaces) &&
          DataInterfaces && DataInterfaces->IsValid()) {
        if (!ResourceTracker.ChargeItems(
                (*DataInterfaces)->Values.Num(),
                Frame.Pointer + TEXT("/data_interfaces"), OutError))
          return false;
        for (const TPair<FString, TSharedPtr<FJsonValue>> &Pair :
             (*DataInterfaces)->Values) {
          if (Pair.Key.IsEmpty() || !Pair.Value.IsValid() ||
              Pair.Value->Type != EJson::Object) {
            OutError =
                TEXT("niagara_workspace_data_interface_invalid:") +
                Pair.Key;
            return false;
          }
          const TSharedPtr<FJsonObject> DataInterfaceObject =
              Pair.Value->AsObject();
          FDataInterfacePatch &DataInterface =
              Patch.DataInterfaces.AddDefaulted_GetRef();
          DataInterface.InputName = Pair.Key;
          DataInterfaceObject->TryGetStringField(
              TEXT("input_name"), DataInterface.InputName);
          DataInterfaceObject->TryGetStringField(
              TEXT("data_interface_class"),
              DataInterface.DataInterfaceClass);
          const TSharedPtr<FJsonObject> *Properties = nullptr;
          if (DataInterfaceObject->TryGetObjectField(
                  TEXT("properties"), Properties) &&
              Properties && Properties->IsValid()) {
            DataInterface.Properties = *Properties;
          }
          if (DataInterface.InputName.IsEmpty() ||
              !DataInterface.Properties.IsValid()) {
            OutError =
                TEXT(
                    "niagara_workspace_data_interface_properties_missing:") +
                Pair.Key;
            return false;
          }
        }
      }

      const TSharedPtr<FJsonObject> *DynamicInputs = nullptr;
      if (Frame.Object->TryGetObjectField(
              TEXT("dynamic_inputs"), DynamicInputs) &&
          DynamicInputs && DynamicInputs->IsValid()) {
        if (!ResourceTracker.ChargeItems(
                (*DynamicInputs)->Values.Num(),
                Frame.Pointer + TEXT("/dynamic_inputs"), OutError))
          return false;
        for (const TPair<FString, TSharedPtr<FJsonValue>> &Pair :
             (*DynamicInputs)->Values) {
          TSharedPtr<FDynamicInputPatch> Child =
              MakeShared<FDynamicInputPatch>();
          Patch.DynamicInputs.Add(Child);
          FParseFrame ChildFrame;
          ChildFrame.ExistingModule = Frame.ExistingDynamic;
          ChildFrame.InputKey = Pair.Key;
          ChildFrame.Value = Pair.Value;
          ChildFrame.Patch = Child.Get();
          ChildFrame.Pointer =
              Frame.Pointer + TEXT("/dynamic_inputs/") +
              EscapeWorkspacePointerToken(Pair.Key);
          ChildFrames.Add(MoveTemp(ChildFrame));
        }
      }
    }

    Frame.bFinalize = true;
    Stack.Add(MoveTemp(Frame));
    for (int32 Index = ChildFrames.Num() - 1; Index >= 0; --Index)
      Stack.Add(MoveTemp(ChildFrames[Index]));
  }
  return ResourceTracker.Checkpoint(RootPointer, OutError);
}

bool ParseModulePatch(UeAgentNiagaraStageModuleAuthoring::FResolvedStage &Stage,
                      const FString &StableAssetPath, const FString &ModuleKey,
                      const TSharedPtr<FJsonObject> &Object,
                      const bool bForceScriptResolution,
                      FUeAgentStructuredResourceTracker &ResourceTracker,
                      FModulePatch &OutPatch, FString &OutError) {
  using namespace UeAgentNiagaraStageModuleAuthoring;
  OutPatch = FModulePatch();
  OutPatch.SourceKey = ModuleKey;
  FString ModuleGuidText;
  if (!Object.IsValid()) {
    OutError = TEXT("niagara_workspace_module_invalid");
    return false;
  }
  if (Object->HasField(TEXT("candidate"))) {
    const TSharedPtr<FJsonObject> *Candidate = nullptr;
    if (!Object->TryGetObjectField(TEXT("candidate"), Candidate) ||
        !Candidate || !Candidate->IsValid() ||
        !(*Candidate)->TryGetStringField(TEXT("id"),
                                         OutPatch.NativeCandidateId) ||
        OutPatch.NativeCandidateId.TrimStartAndEnd().IsEmpty()) {
      OutError = TEXT("niagara_workspace_native_candidate_id_invalid:") +
                 ModuleKey;
      return false;
    }
    OutPatch.NativeCandidateId.TrimStartAndEndInline();
  }
  if (!Object->TryGetStringField(TEXT("module_guid"), ModuleGuidText))
    ModuleGuidText = ModuleKey;
  FGuid::Parse(ModuleGuidText, OutPatch.ModuleGuid);
  if (!OutPatch.ModuleGuid.IsValid()) {
    FString NormalizedAssetPath = StableAssetPath.TrimStartAndEnd();
    NormalizedAssetPath.ToLowerInline();
    FString StableOwnerIdentity = TEXT("root");
    if (Stage.System) {
      StableOwnerIdentity = Stage.EmitterHandleId.IsValid()
                                ? Stage.EmitterHandleId.ToString(
                                      EGuidFormats::DigitsWithHyphensLower)
                                : Stage.EmitterName.TrimStartAndEnd().ToLower();
    }
    const FString TrimmedModuleKey = ModuleKey.TrimStartAndEnd();
    const FString StableIdentity = FString::Printf(
        TEXT("%s|%s|%s|%s"), *NormalizedAssetPath, *StableOwnerIdentity,
        *MakeStageKey(Stage.Usage, Stage.UsageId), *TrimmedModuleKey);
    OutPatch.ModuleGuid = FGuid::NewDeterministicGuid(StableIdentity);
  }
  Object->TryGetBoolField(TEXT("remove"), OutPatch.bRemove);
  double TargetIndex = 0.0;
  if (Object->TryGetNumberField(TEXT("module_index"), TargetIndex)) {
    OutPatch.TargetIndex = FMath::Max(0, FMath::RoundToInt(TargetIndex));
  }

  const bool bStandaloneModuleStage =
      UeAgentNiagaraStandaloneModuleAuthoring::IsStandaloneModuleStage(Stage);
  UNiagaraNodeFunctionCall *ExistingModule = nullptr;
  UNiagaraNode *ExistingStandaloneAssignment = nullptr;
  int32 ExistingIndex = INDEX_NONE;
  if (OutPatch.ModuleGuid.IsValid()) {
    FString ResolveError;
    if (bStandaloneModuleStage) {
      OutPatch.bExists =
          UeAgentNiagaraStandaloneModuleAuthoring::ResolveAssignmentNode(
              *Stage.OutputNode, OutPatch.ModuleGuid,
              ExistingStandaloneAssignment, ExistingIndex, ResolveError);
      if (!OutPatch.bExists) {
        FModuleSelector Selector;
        Selector.NodeGuid = OutPatch.ModuleGuid;
        OutPatch.bExists =
            ResolveModule(*Stage.OutputNode, Selector, ExistingModule,
                          ExistingIndex, ResolveError);
        OutPatch.bLegacyStandaloneNode = OutPatch.bExists;
        if (OutPatch.bLegacyStandaloneNode && ExistingModule) {
          OutPatch.LegacyNodeGuid = ExistingModule->NodeGuid;
        }
      }
    } else {
      FModuleSelector Selector;
      Selector.NodeGuid = OutPatch.ModuleGuid;
      OutPatch.bExists =
          ResolveModule(*Stage.OutputNode, Selector, ExistingModule,
                        ExistingIndex, ResolveError);
    }
  }
  OutPatch.ExistingIndex = ExistingIndex;
  if (OutPatch.bRemove) {
    OutPatch.bNeedsMutation = OutPatch.bExists;
    return true;
  }

  FString DesiredScriptPath;
  Object->TryGetStringField(TEXT("script_asset_path"), DesiredScriptPath);
  const FString CurrentScriptPath =
      ExistingModule ? GetModuleScriptPath(*ExistingModule) : FString();
  FString ModuleKind;
  Object->TryGetStringField(TEXT("module_kind"), ModuleKind);
  ModuleKind.TrimStartAndEndInline();
  ModuleKind.ToLowerInline();
  if (!ModuleKind.IsEmpty() && ModuleKind != TEXT("assignment") &&
      ModuleKind != TEXT("script_module")) {
    OutError = TEXT("niagara_workspace_module_kind_invalid:") + ModuleKind;
    return false;
  }
  if (bStandaloneModuleStage && ModuleKind == TEXT("script_module")) {
    OutError = TEXT("niagara_standalone_module_script_module_unsupported:") +
               ModuleKey;
    return false;
  }
  if (!ParseAssignmentTargets(Object, OutPatch, OutError))
    return false;
  if (bStandaloneModuleStage && !OutPatch.bExists &&
      OutPatch.bAssignmentTargetsExplicit) {
    UNiagaraNodeAssignment *LegacyAssignment = nullptr;
    int32 LegacyIndex = INDEX_NONE;
    FString LegacyResolveError;
    if (UeAgentNiagaraStandaloneModuleAuthoring::
            ResolveLegacyAssignmentNodeByTargets(
                *Stage.OutputNode, OutPatch.AssignmentTargets, LegacyAssignment,
                LegacyIndex, LegacyResolveError)) {
      ExistingModule = LegacyAssignment;
      ExistingIndex = LegacyIndex;
      OutPatch.ExistingIndex = LegacyIndex;
      OutPatch.bExists = true;
      OutPatch.bLegacyStandaloneNode = true;
      OutPatch.LegacyNodeGuid = LegacyAssignment->NodeGuid;
    } else if (LegacyResolveError != TEXT("module_selector_not_found")) {
      OutError = LegacyResolveError;
      return false;
    }
  }
  UNiagaraNodeAssignment *ExistingAssignment =
      Cast<UNiagaraNodeAssignment>(ExistingModule);
  OutPatch.bStandaloneAssignment = bStandaloneModuleStage;
  OutPatch.bAssignmentModule =
      bStandaloneModuleStage || ModuleKind == TEXT("assignment") ||
      OutPatch.bAssignmentTargetsExplicit ||
      (ExistingAssignment && ModuleKind != TEXT("script_module") &&
       DesiredScriptPath.IsEmpty());
  if (OutPatch.bAssignmentModule) {
    if (!DesiredScriptPath.IsEmpty()) {
      OutError =
          TEXT("niagara_workspace_assignment_script_conflict:") + ModuleKey;
      return false;
    }
    if ((!OutPatch.bExists ||
         (!ExistingAssignment && !ExistingStandaloneAssignment)) &&
        !OutPatch.bAssignmentTargetsExplicit) {
      OutError =
          TEXT("niagara_workspace_assignment_targets_missing:") + ModuleKey;
      return false;
    }
    const bool bTargetsMatch =
        ExistingStandaloneAssignment
            ? UeAgentNiagaraStandaloneModuleAuthoring::AssignmentTargetsMatch(
                  *ExistingStandaloneAssignment, OutPatch.AssignmentTargets)
        : ExistingAssignment
            ? AssignmentTargetsMatch(*ExistingAssignment,
                                     OutPatch.AssignmentTargets)
            : false;
    if (OutPatch.bExists &&
        (OutPatch.bLegacyStandaloneNode ||
         (!ExistingStandaloneAssignment && !ExistingAssignment) ||
         (OutPatch.bAssignmentTargetsExplicit && !bTargetsMatch))) {
      OutPatch.bReplaceScript = true;
      OutPatch.bNeedsMutation = true;
    }
    OutPatch.bNeedsMutation |= !OutPatch.bExists;
  } else if (ExistingAssignment && ModuleKind == TEXT("script_module") &&
             DesiredScriptPath.IsEmpty()) {
    OutError = TEXT("niagara_workspace_module_script_missing:") + ModuleKey;
    return false;
  } else if (bForceScriptResolution || !OutPatch.bExists ||
             (!DesiredScriptPath.IsEmpty() &&
              !CurrentScriptPath.Equals(DesiredScriptPath,
                                        ESearchCase::IgnoreCase))) {
    if (DesiredScriptPath.IsEmpty() ||
        !ResolvePatchScript(DesiredScriptPath, EScriptKind::Module,
                            OutPatch.Script, OutError)) {
      if (OutError.IsEmpty())
        OutError = TEXT("niagara_workspace_module_script_missing:") + ModuleKey;
      return false;
    }
    OutPatch.bReplaceScript = OutPatch.bExists;
    OutPatch.bNeedsMutation = true;
  }
  if (!ParseLiteralInputPatches(Object, OutPatch.Inputs, OutError))
    return false;
  if (!ResolveLiteralInputMutationFlags(ExistingModule,
                                        ExistingStandaloneAssignment,
                                        OutPatch.Inputs, OutError))
    return false;
  OutPatch.bNeedsMutation |= OutPatch.Inputs.ContainsByPredicate(
      [](const FLiteralInputPatch &Patch) { return Patch.bNeedsMutation; });
  const TSharedPtr<FJsonObject> *DynamicInputs = nullptr;
  if (Object->TryGetObjectField(TEXT("dynamic_inputs"), DynamicInputs) &&
      DynamicInputs && DynamicInputs->IsValid()) {
    for (const TPair<FString, TSharedPtr<FJsonValue>> &Pair :
         (*DynamicInputs)->Values) {
      FDynamicInputPatch &DynamicPatch =
          OutPatch.DynamicInputs.AddDefaulted_GetRef();
      UEdGraphPin *ExistingTargetPin = nullptr;
      if (ExistingStandaloneAssignment) {
        FNiagaraVariable ExistingTarget;
        ExistingTargetPin =
            UeAgentNiagaraStandaloneModuleAuthoring::FindAssignmentTargetPin(
                *ExistingStandaloneAssignment, Pair.Key, ExistingTarget);
      }
      if (!ParseDynamicInputPatch(
              ExistingModule, ExistingTargetPin, Pair.Key, Pair.Value,
              ResourceTracker,
              TEXT("/provider_extension/stages/") +
                  EscapeWorkspacePointerToken(Stage.StageKey) +
                  TEXT("/modules/") +
                  EscapeWorkspacePointerToken(ModuleKey) +
                  TEXT("/dynamic_inputs/") +
                  EscapeWorkspacePointerToken(Pair.Key),
              DynamicPatch, OutError))
        return false;
      OutPatch.bNeedsMutation |= DynamicPatch.bNeedsMutation ||
                                 !OutPatch.bExists || OutPatch.bReplaceScript;
    }
  }
  if (!OutPatch.bExists || OutPatch.bReplaceScript) {
    for (FLiteralInputPatch &Patch : OutPatch.Inputs)
      Patch.bNeedsMutation = true;
    for (FDynamicInputPatch &Patch : OutPatch.DynamicInputs) {
      MarkDynamicInputPatchForRebuild(Patch);
    }
    OutPatch.bNeedsMutation = true;
  }
  for (const FLiteralInputPatch &LiteralPatch : OutPatch.Inputs) {
    for (const FDynamicInputPatch &DynamicPatch : OutPatch.DynamicInputs) {
      if (LiteralPatch.InputName.Equals(DynamicPatch.InputName,
                                        ESearchCase::IgnoreCase)) {
        OutError = TEXT("niagara_workspace_input_source_conflict:") +
                   LiteralPatch.InputName;
        return false;
      }
    }
  }
  return true;
}

bool ParseStagePatches(UObject *ProposedAsset, const FString &AssetPath,
                       const TSharedPtr<const FJsonObject> &ProviderExtension,
                       TArray<FStagePatch> &OutPatches, FString &OutError) {
  using namespace UeAgentNiagaraStageModuleAuthoring;
  FUeAgentStructuredResourceTracker ResourceTracker;
  OutPatches.Reset();
  if (!ProviderExtension.IsValid() ||
      !ProviderExtension->HasField(TEXT("stages")))
    return true;
  if (!ProposedAsset) {
    OutError = TEXT("niagara_workspace_stage_target_asset_missing");
    return false;
  }
  const TSharedPtr<FJsonObject> *Stages = nullptr;
  if (!ProviderExtension->TryGetObjectField(TEXT("stages"), Stages) ||
      !Stages || !Stages->IsValid()) {
    OutError = TEXT("niagara_workspace_stages_invalid");
    return false;
  }
  if (!ResourceTracker.ChargeNodes(
          1, TEXT("/provider_extension/stages"), OutError) ||
      !ResourceTracker.ChargeItems(
          (*Stages)->Values.Num(), TEXT("/provider_extension/stages"),
          OutError)) {
    return false;
  }
  for (const TPair<FString, TSharedPtr<FJsonValue>> &StagePair :
       (*Stages)->Values) {
    const FString StagePointer =
        TEXT("/provider_extension/stages/") +
        EscapeWorkspacePointerToken(StagePair.Key);
    const TSharedPtr<FJsonObject> StageObject =
        StagePair.Value.IsValid() && StagePair.Value->Type == EJson::Object
            ? StagePair.Value->AsObject()
            : nullptr;
    const TSharedPtr<FJsonObject> *TargetObject = nullptr;
    if (!StageObject.IsValid() ||
        !StageObject->TryGetObjectField(TEXT("target"), TargetObject) ||
        !TargetObject || !TargetObject->IsValid()) {
      OutError = TEXT("niagara_workspace_stage_invalid:") + StagePair.Key;
      return false;
    }
    if (!ResourceTracker.ChargeNodes(1, StagePointer, OutError) ||
        !ResourceTracker.ChargeItems(
            StageObject->Values.Num(), StagePointer, OutError)) {
      return false;
    }
    FStagePatch &StagePatch = OutPatches.AddDefaulted_GetRef();
    StagePatch.SourceKey = StagePair.Key;
    FString StageOperation;
    StageObject->TryGetStringField(TEXT("operation"), StageOperation);
    StageOperation.TrimStartAndEndInline();
    StageOperation.ToLowerInline();
    if (StageOperation == TEXT("replace")) {
      StagePatch.bReplaceStage = true;
      StagePatch.bNeedsMutation = true;
    } else if (!StageOperation.IsEmpty() && StageOperation != TEXT("update") &&
               StageOperation != TEXT("upsert")) {
      OutError =
          TEXT("niagara_workspace_stage_operation_invalid:") + StageOperation;
      return false;
    }
    if (!ParseStageTarget(AssetPath, *TargetObject, StagePatch.Target,
                          OutError))
      return false;

    TArray<FResolvedStage> CandidateStages;
    if (!EnumerateStages(*ProposedAsset, CandidateStages, OutError))
      return false;
    for (const FResolvedStage &Candidate : CandidateStages) {
      if ((StagePatch.Target.bEmitterHandleIdExplicit &&
           Candidate.EmitterHandleId != StagePatch.Target.EmitterHandleId) ||
          (!StagePatch.Target.EmitterName.IsEmpty() &&
           !Candidate.EmitterName.Equals(StagePatch.Target.EmitterName,
                                         ESearchCase::IgnoreCase)) ||
          Candidate.Usage != StagePatch.Target.Usage ||
          (StagePatch.Target.bUsageIdExplicit &&
           Candidate.UsageId != StagePatch.Target.UsageId)) {
        continue;
      }
      StagePatch.Stage = Candidate;
      break;
    }
    if (!StagePatch.Stage.OutputNode) {
      OutError = TEXT("niagara_stage_not_found:") + StagePair.Key;
      return false;
    }

    const TSharedPtr<FJsonObject> *Modules = nullptr;
    if (StageObject->TryGetObjectField(TEXT("modules"), Modules) && Modules &&
        Modules->IsValid()) {
      for (const TPair<FString, TSharedPtr<FJsonValue>> &ModulePair :
           (*Modules)->Values) {
        const FString ModulePointer =
            StagePointer + TEXT("/modules/") +
            EscapeWorkspacePointerToken(ModulePair.Key);
        const TSharedPtr<FJsonObject> ModuleObject =
            ModulePair.Value.IsValid() &&
                    ModulePair.Value->Type == EJson::Object
                ? ModulePair.Value->AsObject()
                : nullptr;
        if (!ModuleObject.IsValid()) {
          OutError =
              TEXT("niagara_workspace_module_invalid:") + ModulePair.Key;
          return false;
        }
        if (!ResourceTracker.ChargeNodes(1, ModulePointer, OutError) ||
            !ResourceTracker.ChargeItems(
                ModuleObject->Values.Num(), ModulePointer, OutError)) {
          return false;
        }
        FModulePatch &ModulePatch = StagePatch.Modules.AddDefaulted_GetRef();
        if (!ParseModulePatch(StagePatch.Stage, AssetPath, ModulePair.Key,
                              ModuleObject, StagePatch.bReplaceStage,
                              ResourceTracker, ModulePatch, OutError))
          return false;
      }
    }
    NormalizeModuleTargetIndices(StagePatch);
    if (StagePatch.bReplaceStage &&
        StagePatch.Modules.ContainsByPredicate(
            [](const FModulePatch &Patch) { return Patch.bRemove; })) {
      OutError =
          TEXT("niagara_workspace_stage_replace_module_remove_conflict:") +
          StagePair.Key;
      return false;
    }
  }
  return true;
}

bool ParseEmitterOwnerTarget(
    const FString &AssetPath, const TSharedPtr<const FJsonObject> &Object,
    UeAgentNiagaraStageModuleAuthoring::FEmitterOwnerTarget &OutTarget,
    FString &OutError) {
  OutTarget = UeAgentNiagaraStageModuleAuthoring::FEmitterOwnerTarget();
  OutTarget.AssetPath = AssetPath;
  if (!Object.IsValid()) {
    OutError = TEXT("niagara_workspace_emitter_target_missing");
    return false;
  }
  Object->TryGetStringField(TEXT("emitter_name"), OutTarget.EmitterName);
  FString HandleId;
  if (Object->TryGetStringField(TEXT("emitter_handle_id"), HandleId) &&
      !HandleId.IsEmpty()) {
    if (!FGuid::Parse(HandleId, OutTarget.EmitterHandleId)) {
      OutError = TEXT("niagara_workspace_emitter_handle_id_invalid");
      return false;
    }
    OutTarget.bEmitterHandleIdExplicit = true;
  }
  return true;
}

bool ParseExecutionMode(const FString &Text, EScriptExecutionMode &OutMode) {
  const UEnum *Enum = StaticEnum<EScriptExecutionMode>();
  if (!Enum)
    return false;
  int64 Value =
      Enum->GetValueByNameString(Text, EGetByNameFlags::CheckAuthoredName);
  if (Value == INDEX_NONE)
    Value = Enum->GetValueByNameString(Text, EGetByNameFlags::None);
  if (Value == INDEX_NONE)
    return false;
  OutMode = static_cast<EScriptExecutionMode>(Value);
  return true;
}

bool ReadUIntField(const TSharedPtr<const FJsonObject> &Object,
                   const TCHAR *Field, uint32 &OutValue) {
  double Number = 0.0;
  if (!Object.IsValid() || !Object->TryGetNumberField(Field, Number))
    return true;
  if (!FMath::IsFinite(Number) || Number < 0.0 ||
      Number > static_cast<double>(MAX_uint32) ||
      !FMath::IsNearlyEqual(Number, FMath::RoundToDouble(Number)))
    return false;
  OutValue = static_cast<uint32>(Number);
  return true;
}

bool ParseEmitterStructurePatches(
    UObject *ProposedAsset, const FString &AssetPath,
    const TSharedPtr<const FJsonObject> &ProviderExtension,
    TArray<FEmitterStructurePatch> &OutPatches, FString &OutError) {
  using namespace UeAgentNiagaraStageModuleAuthoring;
  OutPatches.Reset();
  if (!ProviderExtension.IsValid() ||
      !ProviderExtension->HasField(TEXT("emitters")))
    return true;
  if (!ProposedAsset) {
    OutError = TEXT("niagara_workspace_emitter_structure_asset_missing");
    return false;
  }
  const TSharedPtr<FJsonObject> *Emitters = nullptr;
  if (!ProviderExtension->TryGetObjectField(TEXT("emitters"), Emitters) ||
      !Emitters || !Emitters->IsValid()) {
    OutError = TEXT("niagara_workspace_emitters_invalid");
    return false;
  }
  for (const TPair<FString, TSharedPtr<FJsonValue>> &OwnerPair :
       (*Emitters)->Values) {
    if (!OwnerPair.Value.IsValid() || OwnerPair.Value->Type != EJson::Object) {
      OutError =
          TEXT("niagara_workspace_emitter_structure_invalid:") + OwnerPair.Key;
      return false;
    }
    const TSharedPtr<FJsonObject> OwnerObject = OwnerPair.Value->AsObject();
    bool bRequestedEnabled = true;
    const bool bHasEnabled = OwnerObject->HasField(TEXT("enabled"));
    if (bHasEnabled &&
        !OwnerObject->TryGetBoolField(TEXT("enabled"), bRequestedEnabled)) {
      OutError =
          TEXT("niagara_workspace_emitter_enabled_invalid:") + OwnerPair.Key;
      return false;
    }
    const TSharedPtr<FJsonObject> *TargetObject = nullptr;
    FEmitterStructurePatch &Patch = OutPatches.AddDefaulted_GetRef();
    Patch.SourceKey = OwnerPair.Key;
    if (!OwnerObject->TryGetObjectField(TEXT("target"), TargetObject) ||
        !TargetObject ||
        !ParseEmitterOwnerTarget(AssetPath, *TargetObject, Patch.Target,
                                 OutError))
      return false;

    FString SourceAssetPath;
    OwnerObject->TryGetStringField(TEXT("source_asset_path"), SourceAssetPath);
    EEmitterSourceSyncMode SourceSyncMode = EEmitterSourceSyncMode::Merge;
    FString SourceSyncModeText;
    const bool bHasSourceSyncMode = OwnerObject->TryGetStringField(
        TEXT("source_sync_mode"), SourceSyncModeText);
    if (bHasSourceSyncMode) {
      SourceSyncModeText.TrimStartAndEndInline();
      if (SourceSyncModeText.Equals(TEXT("merge"), ESearchCase::IgnoreCase)) {
        SourceSyncMode = EEmitterSourceSyncMode::Merge;
      } else if (SourceSyncModeText.Equals(TEXT("replace"),
                                           ESearchCase::IgnoreCase)) {
        SourceSyncMode = EEmitterSourceSyncMode::Replace;
      } else {
        OutError = TEXT("niagara_workspace_emitter_source_sync_mode_invalid:") +
                   OwnerPair.Key + TEXT(":") + SourceSyncModeText;
        return false;
      }
      if (SourceAssetPath.IsEmpty()) {
        OutError =
            TEXT(
                "niagara_workspace_emitter_source_sync_mode_requires_source:") +
            OwnerPair.Key;
        return false;
      }
    }
    UNiagaraEmitter *SourceEmitter = nullptr;
    if (!SourceAssetPath.IsEmpty()) {
      Patch.TargetSystem = Cast<UNiagaraSystem>(ProposedAsset);
      if (!Patch.TargetSystem.IsValid()) {
        OutError = TEXT("niagara_workspace_emitter_handle_add_requires_system");
        return false;
      }
      Patch.bEnsureSystemIntegration = true;
      SourceEmitter = Cast<UNiagaraEmitter>(LoadAsset(SourceAssetPath));
      if (!SourceEmitter) {
        OutError = TEXT("niagara_workspace_emitter_source_not_found:") +
                   SourceAssetPath;
        return false;
      }
      Patch.SourceEmitter.Reset(SourceEmitter);
      Patch.SourceEmitterVersion =
          SourceEmitter->GetExposedVersion().VersionGuid;
      FString VersionText;
      if (OwnerObject->TryGetStringField(TEXT("emitter_version"),
                                         VersionText) &&
          !VersionText.IsEmpty() &&
          !FGuid::Parse(VersionText, Patch.SourceEmitterVersion)) {
        OutError =
            TEXT("niagara_workspace_emitter_version_invalid:") + OwnerPair.Key;
        return false;
      }
    }
    if (!ResolveEmitterOwner(Patch.Target, Patch.Owner, OutError,
                             ProposedAsset)) {
      if (SourceAssetPath.IsEmpty())
        return false;
      if (Patch.Target.bEmitterHandleIdExplicit) {
        OutError =
            TEXT("niagara_workspace_new_emitter_handle_id_not_allowed:") +
            OwnerPair.Key;
        return false;
      }
      if (Patch.Target.EmitterName.IsEmpty()) {
        OutError =
            TEXT("niagara_workspace_new_emitter_name_missing:") + OwnerPair.Key;
        return false;
      }
      UNiagaraSystem *System = Cast<UNiagaraSystem>(ProposedAsset);
      if (!System) {
        OutError = TEXT("niagara_workspace_emitter_handle_add_requires_system");
        return false;
      }
      Patch.TargetSystem = System;
      Patch.bAddEmitterHandle = true;
      OutError.Reset();
    } else if (SourceEmitter) {
      const FVersionedNiagaraEmitter Parent =
          Patch.Owner.EmitterData ? Patch.Owner.EmitterData->GetParent()
                                  : FVersionedNiagaraEmitter();
      const FString ParentPath =
          Parent.Emitter && Parent.Emitter->GetOutermost()
              ? Parent.Emitter->GetOutermost()->GetName()
              : FString();
      const FString RequestedPath = NormalizePackagePath(SourceAssetPath);
      if (!Parent.Emitter || ParentPath != RequestedPath) {
        OutError = TEXT("niagara_workspace_emitter_source_conflict:") +
                   OwnerPair.Key + TEXT(":") + ParentPath + TEXT("!=") +
                   RequestedPath;
        return false;
      }
      if (Patch.SourceEmitterVersion.IsValid() && Parent.Version.IsValid() &&
          Patch.SourceEmitterVersion != Parent.Version) {
        OutError =
            TEXT("niagara_workspace_emitter_version_conflict:") + OwnerPair.Key;
        return false;
      }
      Patch.bRefreshEmitterFromSource =
          SourceSyncMode == EEmitterSourceSyncMode::Merge;
      Patch.bReplaceEmitterFromSource =
          SourceSyncMode == EEmitterSourceSyncMode::Replace;
      if (Patch.bReplaceEmitterFromSource) {
        Patch.PreviousEmitterInstance.Reset(Patch.Owner.Emitter);
        Patch.PreviousEmitterVersion = Patch.Owner.EmitterVersion;
      }
    }
    if (Patch.Owner.System) {
      Patch.TargetSystem = Patch.Owner.System;
    }
    if (bHasEnabled) {
      UNiagaraSystem *System = Patch.TargetSystem.Get();
      if (!System) {
        OutError =
            TEXT("niagara_workspace_emitter_enabled_requires_system_handle:") +
            OwnerPair.Key;
        return false;
      }
      Patch.Enabled = bRequestedEnabled;
      if (Patch.bAddEmitterHandle) {
        Patch.bEmitterEnabledNeedsMutation = !bRequestedEnabled;
      } else {
        const FNiagaraEmitterHandle *Handle =
            System->GetEmitterHandles().FindByPredicate(
                [&Patch](const FNiagaraEmitterHandle &Candidate) {
                  return Candidate.GetId() == Patch.Owner.EmitterHandleId;
                });
        if (!Handle) {
          OutError = TEXT("niagara_workspace_emitter_enabled_handle_missing:") +
                     OwnerPair.Key;
          return false;
        }
        Patch.bEmitterEnabledNeedsMutation =
            Handle->GetIsEnabled() != bRequestedEnabled;
      }
      Patch.bEnsureSystemIntegration |= Patch.bEmitterEnabledNeedsMutation;
    }
    const TSharedPtr<FJsonObject> *EmitterProperties = nullptr;
    if (OwnerObject->TryGetObjectField(TEXT("properties"), EmitterProperties) &&
        EmitterProperties) {
      Patch.Properties = *EmitterProperties;
    }

    const TSharedPtr<FJsonObject> *Renderers = nullptr;
    if (OwnerObject->TryGetObjectField(TEXT("renderers"), Renderers) &&
        Renderers && Renderers->IsValid()) {
      Patch.bReplaceRenderers = true;
      for (const TPair<FString, TSharedPtr<FJsonValue>> &Pair :
           (*Renderers)->Values) {
        FRendererPatch &Renderer = Patch.Renderers.AddDefaulted_GetRef();
        Renderer.RendererName = Pair.Key;
        if (!Pair.Value.IsValid() || Pair.Value->Type == EJson::Null) {
          Renderer.bRemove = true;
          continue;
        }
        if (Pair.Value->Type != EJson::Object) {
          OutError = TEXT("niagara_workspace_renderer_invalid:") + Pair.Key;
          return false;
        }
        const TSharedPtr<FJsonObject> Object = Pair.Value->AsObject();
        Object->TryGetStringField(TEXT("renderer_name"), Renderer.RendererName);
        if (Renderer.RendererName.IsEmpty())
          Renderer.RendererName = Pair.Key;
        Object->TryGetStringField(TEXT("renderer_class"),
                                  Renderer.RendererClass);
        Object->TryGetBoolField(TEXT("remove"), Renderer.bRemove);
        double Index = 0.0;
        if (Object->TryGetNumberField(TEXT("renderer_index"), Index))
          Renderer.TargetIndex = FMath::Max(0, FMath::RoundToInt(Index));
        const TSharedPtr<FJsonObject> *Properties = nullptr;
        if (Object->TryGetObjectField(TEXT("properties"), Properties) &&
            Properties)
          Renderer.Properties = *Properties;
      }
    }

    const TSharedPtr<FJsonObject> *Events = nullptr;
    if (OwnerObject->TryGetObjectField(TEXT("event_handlers"), Events) &&
        Events && Events->IsValid()) {
      for (const TPair<FString, TSharedPtr<FJsonValue>> &Pair :
           (*Events)->Values) {
        FEventHandlerPatch &Event = Patch.EventHandlers.AddDefaulted_GetRef();
        if (!FGuid::Parse(Pair.Key, Event.UsageId)) {
          OutError =
              TEXT("niagara_workspace_event_usage_id_invalid:") + Pair.Key;
          return false;
        }
        if (!Pair.Value.IsValid() || Pair.Value->Type == EJson::Null) {
          Event.bRemove = true;
          continue;
        }
        if (Pair.Value->Type != EJson::Object) {
          OutError =
              TEXT("niagara_workspace_event_handler_invalid:") + Pair.Key;
          return false;
        }
        const TSharedPtr<FJsonObject> Object = Pair.Value->AsObject();
        Object->TryGetBoolField(TEXT("remove"), Event.bRemove);
        FVersionedNiagaraEmitterData *ExistingEventData =
            Patch.Owner.EmitterData;
        if (!ExistingEventData && SourceEmitter &&
            Patch.SourceEmitterVersion.IsValid()) {
          ExistingEventData =
              SourceEmitter->GetEmitterData(Patch.SourceEmitterVersion);
        }
        if (ExistingEventData) {
          for (const FNiagaraEventScriptProperties &Existing :
               ExistingEventData->EventHandlerScriptProps) {
            if (!Existing.Script ||
                Existing.Script->GetUsageId() != Event.UsageId)
              continue;
            Event.SourceEventName = Existing.SourceEventName;
            Event.SourceEmitterId = Existing.SourceEmitterID;
            Event.ExecutionMode = Existing.ExecutionMode;
            Event.SpawnNumber = Existing.SpawnNumber;
            Event.MinSpawnNumber = Existing.MinSpawnNumber;
            Event.bRandomSpawnNumber = Existing.bRandomSpawnNumber;
            Event.MaxEventsPerFrame = Existing.MaxEventsPerFrame;
            Event.bUpdateAttributeInitialValues =
                Existing.UpdateAttributeInitialValues;
            break;
          }
        }
        FString Text;
        if (Object->TryGetStringField(TEXT("source_event_name"), Text))
          Event.SourceEventName = FName(*Text);
        if (Object->TryGetStringField(TEXT("source_emitter_id"), Text) &&
            !Text.IsEmpty() && !FGuid::Parse(Text, Event.SourceEmitterId)) {
          OutError =
              TEXT("niagara_workspace_source_emitter_id_invalid:") + Pair.Key;
          return false;
        }
        if (Object->TryGetStringField(TEXT("execution_mode"), Text) &&
            !ParseExecutionMode(Text, Event.ExecutionMode)) {
          OutError =
              TEXT("niagara_workspace_execution_mode_invalid:") + Pair.Key;
          return false;
        }
        if (!ReadUIntField(Object, TEXT("spawn_number"), Event.SpawnNumber) ||
            !ReadUIntField(Object, TEXT("min_spawn_number"),
                           Event.MinSpawnNumber) ||
            !ReadUIntField(Object, TEXT("max_events_per_frame"),
                           Event.MaxEventsPerFrame)) {
          OutError = TEXT("niagara_workspace_event_number_invalid:") + Pair.Key;
          return false;
        }
        Object->TryGetBoolField(TEXT("random_spawn_number"),
                                Event.bRandomSpawnNumber);
        Object->TryGetBoolField(TEXT("update_attribute_initial_values"),
                                Event.bUpdateAttributeInitialValues);
      }
    }
    if (!ValidateEmitterStructurePropertyPatches(Patch, OutError))
      return false;
    Patch.bNeedsMutation =
        Patch.bAddEmitterHandle || Patch.bRefreshEmitterFromSource ||
        Patch.bReplaceEmitterFromSource || Patch.bEmitterEnabledNeedsMutation ||
        Patch.Properties.IsValid() || Patch.bReplaceRenderers ||
        !Patch.Renderers.IsEmpty() ||
        !Patch.EventHandlers.IsEmpty();
  }
  return true;
}

bool ApplyLiteralInputPatches(UNiagaraNodeFunctionCall &Module,
                               const TArray<FLiteralInputPatch> &Patches,
                               FString &OutError) {
  using namespace UeAgentNiagaraStageModuleAuthoring;
  const bool bAssignmentModule = Cast<UNiagaraNodeAssignment>(&Module) != nullptr;
  for (const FLiteralInputPatch &Patch : Patches) {
    if (!Patch.bNeedsMutation && !bAssignmentModule)
      continue;
    FString ResolvedInputName;
    bool bChanged = false;
    if (!SetInputValue(Module, Patch.InputName, Patch.Payload,
                       ResolvedInputName, bChanged, OutError)) {
      if (OutError.IsEmpty())
        OutError = TEXT("niagara_workspace_literal_input_apply_failed:") +
                   Patch.InputName;
      return false;
    }
  }
  return true;
}

bool ApplyLiteralInputPatches(UNiagaraNode &AssignmentNode,
                              const TArray<FLiteralInputPatch> &Patches,
                              FString &OutError) {
  using namespace UeAgentNiagaraStandaloneModuleAuthoring;
  for (const FLiteralInputPatch &Patch : Patches) {
    if (!Patch.bNeedsMutation)
      continue;
    FNiagaraVariable TargetVariable;
    UEdGraphPin *TargetPin = FindAssignmentTargetPin(
        AssignmentNode, Patch.InputName, TargetVariable);
    if (!TargetPin) {
      TArray<FNiagaraVariable> Targets;
      GetAssignmentTargets(AssignmentNode, Targets);
      TArray<FString> Available;
      for (const FNiagaraVariable &Target : Targets) {
        Available.Add(Target.GetName().ToString());
      }
      OutError =
          UeAgentNiagaraStageModuleAuthoring::BuildInputNameNotFoundError(
              Patch.InputName, TEXT("standalone_module_parameter_map_set"),
              Available);
      return false;
    }
    bool bChanged = false;
    if (!UeAgentNiagaraStageModuleAuthoring::SetInputValueForPin(
            *TargetPin, TargetVariable.GetType(), Patch.Payload, bChanged,
            OutError)) {
      if (OutError.IsEmpty()) {
        OutError = TEXT("niagara_workspace_literal_input_apply_failed:") +
                   Patch.InputName;
      }
      return false;
    }
  }
  return true;
}

bool ApplyDataInterfacePatches(UNiagaraNodeFunctionCall &AppliedDynamic,
                               const TArray<FDataInterfacePatch> &Patches,
                               FString &OutError) {
  using namespace UeAgentNiagaraStageModuleAuthoring;
  for (const FDataInterfacePatch &DataInterfacePatch : Patches) {
    UNiagaraNodeInput *InputNode = nullptr;
    UNiagaraDataInterface *DataInterface = nullptr;
    FString ResolvedDataInterfaceInputName;
    if (!ResolveDynamicInputDataInterface(
            AppliedDynamic, DataInterfacePatch.InputName, InputNode,
            DataInterface, ResolvedDataInterfaceInputName, OutError))
      return false;
    if (!DataInterfacePatch.DataInterfaceClass.IsEmpty() &&
        !DataInterface->GetClass()->GetPathName().Equals(
            DataInterfacePatch.DataInterfaceClass, ESearchCase::IgnoreCase)) {
      OutError = TEXT("niagara_workspace_data_interface_class_mismatch:") +
                 ResolvedDataInterfaceInputName;
      return false;
    }
    UeAgentAuthoringObjectApply::FOptions Options;
    Options.bAllowDomainAuthorizedOwners = true;
    Options.bMarkPackageDirty = false;
    UeAgentAuthoringDirectApply::FResult Result;
    if (!UeAgentDetailsAuthoringService::ApplyObjectProperties(
            DataInterface, DataInterfacePatch.Properties, Options, Result,
            OutError)) {
      OutError = Result.ErrorCode.IsEmpty()
                     ? TEXT("niagara_data_interface_properties_apply_failed")
                     : Result.ErrorCode;
      if (!Result.PropertyPath.IsEmpty())
        OutError += TEXT(":") + Result.PropertyPath;
      return false;
    }
  }
  return true;
}

struct FDynamicInputApplyFrame {
  UNiagaraNodeFunctionCall *Owner = nullptr;
  const FDynamicInputPatch *Patch = nullptr;
  FString ParentPointer;
};

bool ApplyDynamicInputPatchStack(
    TArray<FDynamicInputApplyFrame> Stack,
    FUeAgentStructuredResourceTracker &ResourceTracker, FString &OutError) {
  using namespace UeAgentNiagaraStageModuleAuthoring;
  auto AddFailureContext = [&OutError](
                               const FString &WorkspacePointer,
                               const UNiagaraNodeFunctionCall &OwnerNode) {
    if (!OutError.Contains(TEXT("\"workspace_pointer\""))) {
      OutError +=
          FString::Printf(TEXT(":context={\"workspace_pointer\":\"%s\",\"owner_"
                               "script\":\"%s\"}"),
                          *WorkspacePointer, *GetModuleScriptPath(OwnerNode));
    }
    return false;
  };

  while (!Stack.IsEmpty()) {
    FDynamicInputApplyFrame Frame = Stack.Pop(EAllowShrinking::No);
    if (!Frame.Owner || !Frame.Patch) {
      OutError = TEXT("niagara_workspace_dynamic_input_apply_frame_invalid");
      return false;
    }
    const FDynamicInputPatch &Patch = *Frame.Patch;
    if (!Patch.bNeedsMutation)
      continue;
    const FString WorkspacePointer =
        Frame.ParentPointer + TEXT("/dynamic_inputs/") +
        EscapeWorkspacePointerToken(Patch.InputName);
    if (!ResourceTracker.ChargeNodes(1, WorkspacePointer, OutError) ||
        !ResourceTracker.ChargeItems(1, WorkspacePointer, OutError) ||
        !ResourceTracker.ChargeWorkUnits(1, WorkspacePointer, OutError) ||
        !ResourceTracker.ChargeEstimatedMemoryBytes(
            sizeof(FDynamicInputApplyFrame), WorkspacePointer, OutError)) {
      return false;
    }

    UNiagaraNodeFunctionCall &OwnerNode = *Frame.Owner;
    FString ResolvedInputName;
    UNiagaraNodeFunctionCall *Existing = FindDynamicInputForModuleInput(
        OwnerNode, Patch.InputName, ResolvedInputName);
    if (Patch.bRemove) {
      if (!Existing || ClearDynamicInput(OwnerNode, Patch.InputName,
                                         ResolvedInputName, OutError))
        continue;
      return AddFailureContext(WorkspacePointer, OwnerNode);
    }
    const FString ExistingPath =
        Existing ? GetModuleScriptPath(*Existing) : FString();
    const FString DesiredPath =
        Patch.Script.IsValid() ? Patch.Script->GetPathName() : ExistingPath;
    UNiagaraNodeFunctionCall *AppliedDynamic = Existing;
    if (!ExistingPath.Equals(DesiredPath, ESearchCase::IgnoreCase) &&
        (!Patch.Script.IsValid() ||
         !SetDynamicInput(OwnerNode, Patch.InputName, *Patch.Script, FString(),
                          AppliedDynamic, ResolvedInputName, OutError))) {
      if (OutError.IsEmpty())
        OutError = TEXT("niagara_workspace_dynamic_input_apply_failed");
      return AddFailureContext(WorkspacePointer, OwnerNode);
    }
    if (!AppliedDynamic) {
      OutError =
          TEXT("niagara_workspace_dynamic_input_not_found_after_apply:") +
          Patch.InputName;
      return AddFailureContext(WorkspacePointer, OwnerNode);
    }
    FString InitializationSummary;
    if (!InitializeDynamicInputDataInterfaces(
            *AppliedDynamic, InitializationSummary, OutError)) {
      return AddFailureContext(WorkspacePointer, *AppliedDynamic);
    }
    if (!ApplyLiteralInputPatches(*AppliedDynamic, Patch.Inputs, OutError)) {
      return AddFailureContext(WorkspacePointer, *AppliedDynamic);
    }
    if (!ApplyDataInterfacePatches(*AppliedDynamic, Patch.DataInterfaces,
                                   OutError)) {
      return AddFailureContext(WorkspacePointer, *AppliedDynamic);
    }
    for (int32 Index = Patch.DynamicInputs.Num() - 1; Index >= 0; --Index) {
      const TSharedPtr<FDynamicInputPatch> &Child =
          Patch.DynamicInputs[Index];
      if (Child.IsValid()) {
        Stack.Add({AppliedDynamic, Child.Get(), WorkspacePointer});
      }
    }
  }
  return ResourceTracker.Checkpoint(TEXT("/dynamic_inputs"), OutError);
}

bool ApplyDynamicInputPatches(UNiagaraNodeFunctionCall &Module,
                              const TArray<FDynamicInputPatch> &Patches,
                              FUeAgentStructuredResourceTracker &ResourceTracker,
                              FString &OutError) {
  TArray<FDynamicInputApplyFrame> Stack;
  for (int32 Index = Patches.Num() - 1; Index >= 0; --Index) {
    Stack.Add({&Module, &Patches[Index], FString()});
  }
  return ApplyDynamicInputPatchStack(
      MoveTemp(Stack), ResourceTracker, OutError);
}

bool ApplyDynamicInputPatches(UNiagaraNode &AssignmentNode,
                              const TArray<FDynamicInputPatch> &Patches,
                              FUeAgentStructuredResourceTracker &ResourceTracker,
                              FString &OutError) {
  using namespace UeAgentNiagaraStageModuleAuthoring;
  using namespace UeAgentNiagaraStandaloneModuleAuthoring;
  for (const FDynamicInputPatch &Patch : Patches) {
    if (!Patch.bNeedsMutation)
      continue;
    const FString WorkspacePointer =
        TEXT("/dynamic_inputs/") +
        EscapeWorkspacePointerToken(Patch.InputName);
    if (!ResourceTracker.ChargeNodes(1, WorkspacePointer, OutError) ||
        !ResourceTracker.ChargeItems(1, WorkspacePointer, OutError) ||
        !ResourceTracker.ChargeWorkUnits(1, WorkspacePointer, OutError) ||
        !ResourceTracker.ChargeEstimatedMemoryBytes(
            sizeof(FDynamicInputApplyFrame), WorkspacePointer, OutError)) {
      return false;
    }
    FNiagaraVariable TargetVariable;
    UEdGraphPin *TargetPin = FindAssignmentTargetPin(
        AssignmentNode, Patch.InputName, TargetVariable);
    if (!TargetPin) {
      TArray<FNiagaraVariable> Targets;
      GetAssignmentTargets(AssignmentNode, Targets);
      TArray<FString> Available;
      for (const FNiagaraVariable &Target : Targets)
        Available.Add(Target.GetName().ToString());
      OutError = BuildInputNameNotFoundError(
          Patch.InputName, TEXT("standalone_module_parameter_map_set"),
          Available);
      return false;
    }

    UNiagaraNodeFunctionCall *Existing = FindDynamicInputForPin(*TargetPin);
    if (Patch.bRemove) {
      if (Existing && !ClearInputValueNodesForPin(*TargetPin, OutError))
        return false;
      continue;
    }
    const FString ExistingPath =
        Existing ? GetModuleScriptPath(*Existing) : FString();
    const FString DesiredPath =
        Patch.Script.IsValid() ? Patch.Script->GetPathName() : ExistingPath;
    UNiagaraNodeFunctionCall *AppliedDynamic = Existing;
    if (!ExistingPath.Equals(DesiredPath, ESearchCase::IgnoreCase)) {
      if (!Patch.Script.IsValid() ||
          !SetDynamicInputForPin(*TargetPin, *Patch.Script, FString(),
                                 AppliedDynamic, OutError))
        return false;
    }
    if (!AppliedDynamic) {
      OutError =
          TEXT("niagara_workspace_dynamic_input_not_found_after_apply:") +
          Patch.InputName;
      return false;
    }
    FString InitializationSummary;
    if (!InitializeDynamicInputDataInterfaces(
            *AppliedDynamic, InitializationSummary, OutError) ||
        !ApplyLiteralInputPatches(*AppliedDynamic, Patch.Inputs, OutError) ||
        !ApplyDataInterfacePatches(*AppliedDynamic, Patch.DataInterfaces,
                                   OutError)) {
      return false;
    }
    TArray<FDynamicInputApplyFrame> ChildStack;
    for (int32 Index = Patch.DynamicInputs.Num() - 1; Index >= 0; --Index) {
      const TSharedPtr<FDynamicInputPatch> &Child =
          Patch.DynamicInputs[Index];
      if (Child.IsValid()) {
        ChildStack.Add(
            {AppliedDynamic, Child.Get(), WorkspacePointer});
      }
    }
    if (!ApplyDynamicInputPatchStack(
            MoveTemp(ChildStack), ResourceTracker, OutError)) {
      return false;
    }
  }
  return ResourceTracker.Checkpoint(TEXT("/dynamic_inputs"), OutError);
}

UClass *ResolveRendererClass(FString ClassPath) {
  ClassPath.TrimStartAndEndInline();
  const FString Alias = ClassPath.ToLower();
  if (Alias == TEXT("sprite"))
    ClassPath = TEXT("/Script/Niagara.NiagaraSpriteRendererProperties");
  else if (Alias == TEXT("ribbon"))
    ClassPath = TEXT("/Script/Niagara.NiagaraRibbonRendererProperties");
  else if (Alias == TEXT("mesh"))
    ClassPath = TEXT("/Script/Niagara.NiagaraMeshRendererProperties");
  else if (Alias == TEXT("light"))
    ClassPath = TEXT("/Script/Niagara.NiagaraLightRendererProperties");
  else if (Alias == TEXT("decal"))
    ClassPath = TEXT("/Script/Niagara.NiagaraDecalRendererProperties");
  UClass *Class =
      ClassPath.IsEmpty() ? nullptr : LoadObject<UClass>(nullptr, *ClassPath);
  return Class && Class->IsChildOf(UNiagaraRendererProperties::StaticClass()) &&
                 !Class->HasAnyClassFlags(CLASS_Abstract)
             ? Class
             : nullptr;
}

bool TryReadRendererBindingName(const TSharedPtr<FJsonObject> &Descriptor,
                                FString &OutBindingName) {
  OutBindingName.Reset();
  const TSharedPtr<FJsonObject> *Value = nullptr;
  const TSharedPtr<FJsonObject> *Fields = nullptr;
  const TSharedPtr<FJsonObject> *RootVariable = nullptr;
  const TSharedPtr<FJsonObject> *RootFields = nullptr;
  const TSharedPtr<FJsonObject> *Name = nullptr;
  return Descriptor.IsValid() &&
         Descriptor->TryGetObjectField(TEXT("value"), Value) && Value &&
         Value->IsValid() &&
         (*Value)->TryGetObjectField(TEXT("fields"), Fields) && Fields &&
         Fields->IsValid() &&
         (*Fields)->TryGetObjectField(TEXT("RootVariable"), RootVariable) &&
         RootVariable && RootVariable->IsValid() &&
         (*RootVariable)->TryGetObjectField(TEXT("fields"), RootFields) &&
         RootFields && RootFields->IsValid() &&
         (*RootFields)->TryGetObjectField(TEXT("Name"), Name) && Name &&
         Name->IsValid() &&
         (*Name)->TryGetStringField(TEXT("literal"), OutBindingName) &&
         !OutBindingName.IsEmpty();
}

bool ReadProjectedPropertyDescriptors(
    const TSharedPtr<FJsonObject> &Properties, const TCHAR *MissingArrayError,
    TArray<TSharedPtr<FJsonValue>> &OutDescriptors, FString &OutError) {
  OutDescriptors.Reset();
  if (!Properties.IsValid())
    return true;
  if (!UeAgentDetailsAuthoringService::DecodePropertiesDocument(
          Properties, OutDescriptors, OutError)) {
    if (OutError == TEXT("missing_properties_array"))
      OutError = MissingArrayError;
    return false;
  }
  return true;
}

void SetPreparedPropertyError(
    const UeAgentAuthoringObjectApply::FFailure &Failure, const TCHAR *Fallback,
    FString &OutError) {
  OutError = Failure.ErrorCode.IsEmpty() ? Fallback : Failure.ErrorCode;
  if (!Failure.PropertyPath.IsEmpty() &&
      !OutError.EndsWith(TEXT(":") + Failure.PropertyPath)) {
    OutError += TEXT(":") + Failure.PropertyPath;
  }
}

bool ResolveEmitterValidationContext(
    const FEmitterStructurePatch &Patch, UNiagaraEmitter *&OutEmitter,
    FVersionedNiagaraEmitterData *&OutEmitterData) {
  const bool bUsesSourceState =
      Patch.bAddEmitterHandle || Patch.bReplaceEmitterFromSource;
  OutEmitter =
      bUsesSourceState ? Patch.SourceEmitter.Get() : Patch.Owner.Emitter;
  const FGuid Version = bUsesSourceState ? Patch.SourceEmitterVersion
                                         : Patch.Owner.EmitterVersion;
  OutEmitterData = OutEmitter && Version.IsValid()
                       ? OutEmitter->GetEmitterData(Version)
                       : nullptr;
  return OutEmitter && OutEmitterData;
}

UNiagaraRendererProperties *
FindRendererByName(FVersionedNiagaraEmitterData *EmitterData,
                   const FString &RendererName) {
  if (!EmitterData)
    return nullptr;
  for (UNiagaraRendererProperties *Renderer : EmitterData->GetRenderers()) {
    if (Renderer && Renderer->GetFName().ToString().Equals(
                        RendererName, ESearchCase::IgnoreCase)) {
      return Renderer;
    }
  }
  return nullptr;
}

bool ValidateEmitterProperties(FVersionedNiagaraEmitterData &EmitterData,
                               UNiagaraEmitter &Emitter,
                               const TSharedPtr<FJsonObject> &Properties,
                               FString &OutError) {
  if (!Properties.IsValid())
    return true;
  TArray<TSharedPtr<FJsonValue>> Descriptors;
  if (!ReadProjectedPropertyDescriptors(
          Properties, TEXT("niagara_emitter_properties_missing_array"),
          Descriptors, OutError)) {
    return false;
  }
  UeAgentAuthoringObjectApply::FOptions Options;
  Options.bAllowDomainAuthorizedOwners = true;
  Options.bMarkPackageDirty = false;
  UNiagaraEmitter *ValidationEmitter = &Emitter;
  FVersionedNiagaraEmitterData *ValidationData = &EmitterData;
  TStrongObjectPtr<UNiagaraEmitter> ValidationEmitterGuard;
  if (Emitter.GetPackage() &&
      Emitter.GetPackage()->GetName().StartsWith(TEXT("/Engine/"))) {
    UNiagaraEmitter *Duplicate =
        DuplicateObject<UNiagaraEmitter>(&Emitter, GetTransientPackage());
    ValidationEmitterGuard.Reset(Duplicate);
    ValidationEmitter = Duplicate;
    ValidationData =
        Duplicate ? Duplicate->GetEmitterData(EmitterData.Version.VersionGuid)
                  : nullptr;
    if (!ValidationEmitter || !ValidationData) {
      OutError = TEXT("niagara_emitter_properties_preflight_clone_failed");
      return false;
    }
  }
  UeAgentAuthoringObjectApply::FPreparedApply Prepared;
  UeAgentAuthoringObjectApply::FFailure Failure;
  if (!UeAgentAuthoringObjectApply::PrepareStruct(
          FVersionedNiagaraEmitterData::StaticStruct(), ValidationData,
          ValidationEmitter, Descriptors, Options, Prepared, Failure)) {
    SetPreparedPropertyError(
        Failure, TEXT("niagara_emitter_properties_preflight_failed"), OutError);
    return false;
  }
  return true;
}

bool ReadRendererEnabledDescriptor(const TSharedPtr<FJsonObject> &Descriptor,
                                   bool &OutEnabled, FString &OutError) {
  const TSharedPtr<FJsonObject> Projected =
      UeAgentAuthoringObjectApply::ProjectDescriptorForApply(Descriptor);
  if (!Projected.IsValid()) {
    OutError = TEXT("niagara_renderer_enabled_descriptor_invalid");
    return false;
  }
  for (const TPair<FString, TSharedPtr<FJsonValue>> &Field :
       Projected->Values) {
    if (Field.Key != TEXT("property_path") && Field.Key != TEXT("value_kind") &&
        Field.Key != TEXT("value")) {
      OutError = TEXT("niagara_renderer_enabled_descriptor_field_invalid:") +
                 Field.Key;
      return false;
    }
  }
  FString ValueKind;
  if (Projected->TryGetStringField(TEXT("value_kind"), ValueKind) &&
      ValueKind != TEXT("bool")) {
    OutError = TEXT("niagara_renderer_enabled_value_kind_invalid");
    return false;
  }
  const TSharedPtr<FJsonObject> *Value = nullptr;
  if (!Projected->TryGetObjectField(TEXT("value"), Value) || !Value ||
      !Value->IsValid() || (*Value)->Values.Num() != 1 ||
      !(*Value)->TryGetBoolField(TEXT("literal"), OutEnabled)) {
    OutError = TEXT("niagara_renderer_enabled_literal_invalid");
    return false;
  }
  return true;
}

bool ValidateRendererProperties(UNiagaraRendererProperties &Renderer,
                                const TSharedPtr<FJsonObject> &Properties,
                                FString &OutError) {
  if (!Properties.IsValid())
    return true;
  TArray<TSharedPtr<FJsonValue>> Descriptors;
  if (!ReadProjectedPropertyDescriptors(
          Properties,
          TEXT("niagara_renderer_properties_missing_properties_array"),
          Descriptors, OutError)) {
    return false;
  }

  TArray<TSharedPtr<FJsonValue>> GenericDescriptors;
  TOptional<bool> Enabled;
  for (const TSharedPtr<FJsonValue> &Value : Descriptors) {
    const TSharedPtr<FJsonObject> Descriptor = Value->AsObject();
    const FString PropertyPath =
        Descriptor->GetStringField(TEXT("property_path"));
    if (PropertyPath == TEXT("bIsEnabled")) {
      bool bEnabled = true;
      if (Enabled.IsSet()) {
        OutError = TEXT("niagara_renderer_enabled_descriptor_duplicate");
        return false;
      }
      if (!ReadRendererEnabledDescriptor(Descriptor, bEnabled, OutError)) {
        return false;
      }
      Enabled = bEnabled;
      continue;
    }
    FStructProperty *StructProperty =
        FindFProperty<FStructProperty>(Renderer.GetClass(), *PropertyPath);
    if (!StructProperty ||
        StructProperty->Struct !=
            FNiagaraVariableAttributeBinding::StaticStruct()) {
      GenericDescriptors.Add(Value);
      continue;
    }
    FString BindingName;
    if (!TryReadRendererBindingName(Descriptor, BindingName)) {
      OutError = TEXT("niagara_renderer_binding_missing_root_variable:") +
                 PropertyPath;
      return false;
    }
  }

  if (GenericDescriptors.IsEmpty())
    return true;
  UeAgentAuthoringObjectApply::FOptions Options;
  Options.bAllowDomainAuthorizedOwners = true;
  Options.bMarkPackageDirty = false;
  UeAgentAuthoringObjectApply::FPreparedApply Prepared;
  UeAgentAuthoringObjectApply::FFailure Failure;
  if (!UeAgentAuthoringObjectApply::PrepareObject(&Renderer, GenericDescriptors,
                                                  Options, Prepared, Failure)) {
    SetPreparedPropertyError(
        Failure, TEXT("niagara_renderer_properties_preflight_failed"),
        OutError);
    return false;
  }
  return true;
}

bool ValidateEmitterStructurePropertyPatches(
    const FEmitterStructurePatch &Patch, FString &OutError) {
  UNiagaraEmitter *ValidationEmitter = nullptr;
  FVersionedNiagaraEmitterData *ValidationData = nullptr;
  if (!ResolveEmitterValidationContext(Patch, ValidationEmitter,
                                       ValidationData)) {
    if (!Patch.Properties.IsValid() && Patch.Renderers.IsEmpty())
      return true;
    OutError =
        TEXT("niagara_workspace_emitter_property_preflight_context_missing:") +
        Patch.SourceKey;
    return false;
  }
  if (!ValidateEmitterProperties(*ValidationData, *ValidationEmitter,
                                 Patch.Properties, OutError))
    return false;

  FVersionedNiagaraEmitterData *SourceData =
      Patch.SourceEmitter.IsValid() && Patch.SourceEmitterVersion.IsValid()
          ? Patch.SourceEmitter->GetEmitterData(Patch.SourceEmitterVersion)
          : nullptr;
  for (const FRendererPatch &RendererPatch : Patch.Renderers) {
    if (RendererPatch.bRemove)
      continue;
    UNiagaraRendererProperties *Existing =
        FindRendererByName(ValidationData, RendererPatch.RendererName);
    if (!Existing && SourceData != ValidationData) {
      Existing = FindRendererByName(SourceData, RendererPatch.RendererName);
    }
    UClass *DesiredClass =
        RendererPatch.RendererClass.IsEmpty()
            ? (Existing ? Existing->GetClass() : nullptr)
            : ResolveRendererClass(RendererPatch.RendererClass);
    if (!DesiredClass) {
      OutError = TEXT("niagara_workspace_renderer_class_invalid:") +
                 RendererPatch.RendererName;
      return false;
    }
    UNiagaraRendererProperties *Probe =
        Existing && Existing->GetClass() == DesiredClass
            ? DuplicateObject<UNiagaraRendererProperties>(Existing,
                                                          GetTransientPackage())
            : NewObject<UNiagaraRendererProperties>(
                  GetTransientPackage(), DesiredClass, NAME_None, RF_Transient);
    TStrongObjectPtr<UNiagaraRendererProperties> ProbeGuard(Probe);
    if (!Probe) {
      OutError = TEXT("niagara_workspace_renderer_preflight_create_failed:") +
                 RendererPatch.RendererName;
      return false;
    }
    if (!ValidateRendererProperties(*Probe, RendererPatch.Properties, OutError))
      return false;
  }
  return true;
}

bool ApplyRendererProperties(UNiagaraRendererProperties &Renderer,
                             UNiagaraEmitter &Emitter,
                             const FGuid &EmitterVersion,
                             const TSharedPtr<FJsonObject> &Properties,
                             FString &OutError) {
  if (!Properties.IsValid())
    return true;

  TArray<TSharedPtr<FJsonValue>> Descriptors;
  if (!ReadProjectedPropertyDescriptors(
          Properties,
          TEXT("niagara_renderer_properties_missing_properties_array"),
          Descriptors, OutError)) {
    return false;
  }

  TArray<TSharedPtr<FJsonValue>> GenericDescriptors;
  GenericDescriptors.Reserve(Descriptors.Num());
  TOptional<bool> Enabled;
  const FVersionedNiagaraEmitter VersionedEmitter(&Emitter, EmitterVersion);
  for (const TSharedPtr<FJsonValue> &Value : Descriptors) {
    const TSharedPtr<FJsonObject> Descriptor =
        Value.IsValid() && Value->Type == EJson::Object ? Value->AsObject()
                                                        : nullptr;
    FString PropertyPath;
    if (!Descriptor.IsValid() ||
        !Descriptor->TryGetStringField(TEXT("property_path"), PropertyPath)) {
      GenericDescriptors.Add(Value);
      continue;
    }
    if (PropertyPath == TEXT("bIsEnabled")) {
      bool bEnabled = true;
      if (Enabled.IsSet()) {
        OutError = TEXT("niagara_renderer_enabled_descriptor_duplicate");
        return false;
      }
      if (!ReadRendererEnabledDescriptor(Descriptor, bEnabled, OutError)) {
        return false;
      }
      Enabled = bEnabled;
      continue;
    }
    FStructProperty *StructProperty =
        FindFProperty<FStructProperty>(Renderer.GetClass(), *PropertyPath);
    if (!StructProperty ||
        StructProperty->Struct !=
            FNiagaraVariableAttributeBinding::StaticStruct()) {
      GenericDescriptors.Add(Value);
      continue;
    }

    FString BindingName;
    if (!TryReadRendererBindingName(Descriptor, BindingName)) {
      OutError = TEXT("niagara_renderer_binding_missing_root_variable:") +
                 PropertyPath;
      return false;
    }
    FNiagaraVariableAttributeBinding *Binding =
        StructProperty
            ->ContainerPtrToValuePtr<FNiagaraVariableAttributeBinding>(
                &Renderer);
    if (!Binding) {
      OutError =
          TEXT("niagara_renderer_binding_storage_missing:") + PropertyPath;
      return false;
    }

    Renderer.PreEditChange(StructProperty);
    Binding->SetValue(FName(*BindingName), VersionedEmitter,
                      Renderer.GetCurrentSourceMode());
    FPropertyChangedEvent ChangeEvent(StructProperty,
                                      EPropertyChangeType::ValueSet);
    Renderer.PostEditChangeProperty(ChangeEvent);
  }

  if (!GenericDescriptors.IsEmpty()) {
    TSharedRef<FJsonObject> GenericProperties = MakeShared<FJsonObject>();
    for (const TPair<FString, TSharedPtr<FJsonValue>> &Field :
         Properties->Values) {
      if (Field.Key != TEXT("properties"))
        GenericProperties->SetField(Field.Key, Field.Value);
    }
    GenericProperties->SetArrayField(TEXT("properties"), GenericDescriptors);
    UeAgentAuthoringObjectApply::FOptions Options;
    Options.bAllowDomainAuthorizedOwners = true;
    Options.bMarkPackageDirty = false;
    UeAgentAuthoringDirectApply::FResult Result;
    if (!UeAgentDetailsAuthoringService::ApplyObjectProperties(
            &Renderer, GenericProperties, Options, Result, OutError)) {
      OutError = Result.ErrorCode.IsEmpty()
                     ? TEXT("niagara_renderer_properties_apply_failed")
                     : Result.ErrorCode;
      if (!Result.PropertyPath.IsEmpty())
        OutError += TEXT(":") + Result.PropertyPath;
      return false;
    }
  }
  if (Enabled.IsSet())
    Renderer.SetIsEnabled(Enabled.GetValue());
  return true;
}

bool ApplyEmitterProperties(FVersionedNiagaraEmitterData &EmitterData,
                            UNiagaraEmitter &Emitter,
                            const TSharedPtr<FJsonObject> &Properties,
                            FString &OutError) {
  if (!Properties.IsValid())
    return true;
  UeAgentAuthoringObjectApply::FOptions Options;
  Options.bAllowDomainAuthorizedOwners = true;
  Options.bMarkPackageDirty = false;
  UeAgentAuthoringDirectApply::FResult Result;
  if (!UeAgentDetailsAuthoringService::ApplyStructProperties(
          FVersionedNiagaraEmitterData::StaticStruct(), &EmitterData, &Emitter,
          Properties, Options, Result, OutError)) {
    OutError = Result.ErrorCode.IsEmpty()
                   ? TEXT("niagara_emitter_properties_apply_failed")
                   : Result.ErrorCode;
    if (!Result.PropertyPath.IsEmpty())
      OutError += TEXT(":") + Result.PropertyPath;
    return false;
  }
  return true;
}

FString GetRootPropertyName(const FString &PropertyPath) {
  int32 End = PropertyPath.Len();
  int32 Separator = INDEX_NONE;
  if (PropertyPath.FindChar(TEXT('.'), Separator))
    End = FMath::Min(End, Separator);
  if (PropertyPath.FindChar(TEXT('['), Separator))
    End = FMath::Min(End, Separator);
  return PropertyPath.Left(End);
}

bool CaptureEmitterProperties(const FEmitterStructurePatch &Patch,
                              TSharedPtr<FJsonObject> &OutProperties,
                              FString &OutError) {
  OutProperties.Reset();
  if (!Patch.Properties.IsValid())
    return true;
  TArray<TSharedPtr<FJsonValue>> Descriptors;
  if (!UeAgentDetailsAuthoringService::DecodePropertiesDocument(
          Patch.Properties, Descriptors, OutError))
    return false;
  TSet<FString> CapturedRoots;
  for (const TSharedPtr<FJsonValue> &DescriptorValue : Descriptors) {
    const TSharedPtr<FJsonObject> Descriptor =
        DescriptorValue.IsValid() && DescriptorValue->Type == EJson::Object
            ? DescriptorValue->AsObject()
            : nullptr;
    FString PropertyPath;
    if (!Descriptor.IsValid() ||
        !Descriptor->TryGetStringField(TEXT("property_path"), PropertyPath)) {
      OutError = TEXT("property_descriptor_invalid");
      return false;
    }
    const FString RootName = GetRootPropertyName(PropertyPath);
    if (RootName.IsEmpty() || CapturedRoots.Contains(RootName))
      continue;
    CapturedRoots.Add(RootName);
  }
  TArray<FString> RootPaths = CapturedRoots.Array();
  RootPaths.Sort();
  return UeAgentDetailsAuthoringService::CaptureStructPropertyPaths(
      FVersionedNiagaraEmitterData::StaticStruct(), Patch.Owner.EmitterData,
      Patch.Owner.Emitter ? Patch.Owner.Emitter->GetPathName()
                          : FString(TEXT("niagara_emitter")),
      Patch.Owner.Emitter, RootPaths,
      UeAgentDetailsAuthoringService::FCapturePolicy(), OutProperties,
      OutError);
}

bool ApplyEmitterStructurePatches(TArray<FEmitterStructurePatch> &Patches,
                                  FString &OutError) {
  using namespace UeAgentNiagaraStageModuleAuthoring;
  for (FEmitterStructurePatch &Patch : Patches) {
    if (!Patch.bAddEmitterHandle)
      continue;
    UNiagaraSystem *System = Patch.TargetSystem.Get();
    UNiagaraEmitter *SourceEmitter = Patch.SourceEmitter.Get();
    if (!System || !SourceEmitter || !Patch.SourceEmitterVersion.IsValid()) {
      OutError = TEXT("niagara_workspace_emitter_handle_add_state_invalid:") +
                 Patch.SourceKey;
      return false;
    }
    System->Modify();
    const FNiagaraEmitterHandle Added = System->AddEmitterHandle(
        *SourceEmitter, *Patch.Target.EmitterName, Patch.SourceEmitterVersion);
    if (!Added.GetId().IsValid()) {
      OutError = TEXT("niagara_workspace_emitter_handle_add_failed:") +
                 Patch.SourceKey;
      return false;
    }
    Patch.AddedHandleId = Added.GetId();
    Patch.bAddedEmitterHandle = true;
    Patch.Target.EmitterHandleId = Added.GetId();
    Patch.Target.bEmitterHandleIdExplicit = true;
    if (!ResolveEmitterOwner(Patch.Target, Patch.Owner, OutError, System))
      return false;
  }
  for (FEmitterStructurePatch &Patch : Patches) {
    if (!Patch.bNeedsMutation || !Patch.Owner.Emitter ||
        !Patch.Owner.EmitterData)
      continue;
    if (Patch.bReplaceEmitterFromSource) {
      UNiagaraSystem *System = Patch.TargetSystem.Get();
      UNiagaraEmitter *SourceEmitter = Patch.SourceEmitter.Get();
      if (!System || !SourceEmitter || !Patch.SourceEmitterVersion.IsValid()) {
        OutError =
            TEXT("niagara_workspace_emitter_source_replace_state_invalid:") +
            Patch.SourceKey;
        return false;
      }
      FNiagaraEmitterHandle *Handle = nullptr;
      for (FNiagaraEmitterHandle &Candidate : System->GetEmitterHandles()) {
        if (Candidate.GetId() == Patch.Owner.EmitterHandleId) {
          Handle = &Candidate;
          break;
        }
      }
      if (!Handle) {
        OutError =
            TEXT("niagara_workspace_emitter_source_replace_handle_missing:") +
            Patch.SourceKey;
        return false;
      }

      System->Modify();
      const FName ReplacementObjectName = MakeUniqueObjectName(
          System, UNiagaraEmitter::StaticClass(),
          FName(*FString::Printf(TEXT("%s_SourceInstance"),
                                 *Patch.Target.EmitterName)));
      UNiagaraEmitter *Replacement = UNiagaraEmitter::CreateWithParentAndOwner(
          FVersionedNiagaraEmitter(SourceEmitter, Patch.SourceEmitterVersion),
          System, ReplacementObjectName, ~(RF_Public | RF_Standalone));
      if (!Replacement) {
        OutError = TEXT("niagara_workspace_emitter_source_replace_failed:") +
                   Patch.SourceKey;
        return false;
      }
      Replacement->SetUniqueEmitterName(Patch.Target.EmitterName);
      Handle->GetInstance() =
          FVersionedNiagaraEmitter(Replacement, Patch.SourceEmitterVersion);
      Patch.bReplacedEmitterFromSource = true;
      if (!ResolveEmitterOwner(Patch.Target, Patch.Owner, OutError, System))
        return false;
    }
    if (Patch.Enabled.IsSet()) {
      UNiagaraSystem *System = Patch.TargetSystem.Get();
      FNiagaraEmitterHandle *Handle =
          System
              ? System->GetEmitterHandles().FindByPredicate(
                    [&Patch](const FNiagaraEmitterHandle &Candidate) {
                      return Candidate.GetId() == Patch.Owner.EmitterHandleId;
                    })
              : nullptr;
      if (!System || !Handle) {
        OutError =
            TEXT("niagara_workspace_emitter_enabled_apply_handle_missing:") +
            Patch.SourceKey;
        return false;
      }
      const bool bPreviousEnabled = Handle->GetIsEnabled();
      System->Modify();
      Patch.bEmitterEnabledChanged =
          Handle->SetIsEnabled(Patch.Enabled.GetValue(), *System, false);
      if (Patch.bEmitterEnabledChanged) {
        Patch.bPreviousEmitterEnabled = bPreviousEnabled;
        Patch.bRestoreEmitterEnabledOnRollback = !Patch.bAddedEmitterHandle;
        Patch.bEnsureSystemIntegration = true;
      }
    }

    UNiagaraEmitter &Emitter = *Patch.Owner.Emitter;
    Emitter.Modify();
    if (Patch.bRefreshEmitterFromSource) {
      const TArray<INiagaraMergeManager::FMergeEmitterResults> MergeResults =
          Emitter.MergeChangesFromParent();
      for (const INiagaraMergeManager::FMergeEmitterResults &Result :
           MergeResults) {
        if (Result.MergeResult == INiagaraMergeManager::EMergeEmitterResult::
                                      SucceededNoDifferences ||
            Result.MergeResult == INiagaraMergeManager::EMergeEmitterResult::
                                      SucceededDifferencesApplied) {
          continue;
        }
        OutError = TEXT("niagara_workspace_emitter_source_merge_failed:") +
                   Patch.SourceKey + TEXT(":") +
                   Result.GetErrorMessagesString();
        return false;
      }
      Patch.Owner.EmitterData =
          Emitter.GetEmitterData(Patch.Owner.EmitterVersion);
      Patch.Owner.ScriptSource =
          Patch.Owner.EmitterData
              ? Cast<UNiagaraScriptSource>(Patch.Owner.EmitterData->GraphSource)
              : nullptr;
      Patch.Owner.Graph = Patch.Owner.ScriptSource
                              ? Patch.Owner.ScriptSource->NodeGraph
                              : nullptr;
      if (!Patch.Owner.EmitterData) {
        OutError =
            TEXT("niagara_workspace_emitter_source_merge_data_missing:") +
            Patch.SourceKey;
        return false;
      }
    }
    if (!ApplyEmitterProperties(*Patch.Owner.EmitterData, Emitter,
                                Patch.Properties, OutError))
      return false;

    if (Patch.bReplaceRenderers) {
      TSet<FString> DesiredRendererNames;
      for (const FRendererPatch &RendererPatch : Patch.Renderers) {
        if (!RendererPatch.bRemove) {
          DesiredRendererNames.Add(RendererPatch.RendererName.ToLower());
        }
      }
      const TArray<UNiagaraRendererProperties *> CurrentRenderers =
          Patch.Owner.EmitterData->GetRenderers();
      for (UNiagaraRendererProperties *CurrentRenderer : CurrentRenderers) {
        if (CurrentRenderer &&
            !DesiredRendererNames.Contains(
                CurrentRenderer->GetFName().ToString().ToLower())) {
          Emitter.RemoveRenderer(CurrentRenderer, Patch.Owner.EmitterVersion);
        }
      }
      Patch.Owner.EmitterData =
          Emitter.GetEmitterData(Patch.Owner.EmitterVersion);
      if (!Patch.Owner.EmitterData) {
        OutError =
            TEXT("niagara_workspace_renderer_collection_data_missing:") +
            Patch.SourceKey;
        return false;
      }
      TArray<FString> UnexpectedRendererNames;
      for (UNiagaraRendererProperties *CurrentRenderer :
           Patch.Owner.EmitterData->GetRenderers()) {
        if (CurrentRenderer &&
            !DesiredRendererNames.Contains(
                CurrentRenderer->GetFName().ToString().ToLower())) {
          UnexpectedRendererNames.Add(CurrentRenderer->GetName());
        }
      }
      if (!UnexpectedRendererNames.IsEmpty()) {
        UnexpectedRendererNames.Sort();
        OutError =
            TEXT("niagara_workspace_renderer_collection_replace_failed:") +
            Patch.SourceKey + TEXT(":remaining=") +
            FString::Join(UnexpectedRendererNames, TEXT(","));
        return false;
      }
    }

    for (const FRendererPatch &RendererPatch : Patch.Renderers) {
      UNiagaraRendererProperties *Existing = nullptr;
      int32 ExistingIndex = INDEX_NONE;
      const TArray<UNiagaraRendererProperties *> &Renderers =
          Patch.Owner.EmitterData->GetRenderers();
      for (int32 Index = 0; Index < Renderers.Num(); ++Index) {
        UNiagaraRendererProperties *Candidate = Renderers[Index];
        if (Candidate &&
            Candidate->GetFName().ToString().Equals(RendererPatch.RendererName,
                                                    ESearchCase::IgnoreCase)) {
          Existing = Candidate;
          ExistingIndex = Index;
          break;
        }
      }
      if (RendererPatch.bRemove) {
        if (Existing)
          Emitter.RemoveRenderer(Existing, Patch.Owner.EmitterVersion);
        continue;
      }
      UClass *DesiredClass =
          RendererPatch.RendererClass.IsEmpty()
              ? (Existing ? Existing->GetClass() : nullptr)
              : ResolveRendererClass(RendererPatch.RendererClass);
      if (!DesiredClass) {
        OutError = TEXT("niagara_workspace_renderer_class_invalid:") +
                   RendererPatch.RendererName;
        return false;
      }
      if (Existing && Existing->GetClass() != DesiredClass) {
        Emitter.RemoveRenderer(Existing, Patch.Owner.EmitterVersion);
        Existing = nullptr;
      }
      if (!Existing) {
        Existing = NewObject<UNiagaraRendererProperties>(
            &Emitter, DesiredClass, FName(*RendererPatch.RendererName),
            RF_Transactional);
        if (!Existing) {
          OutError = TEXT("niagara_workspace_renderer_create_failed:") +
                     RendererPatch.RendererName;
          return false;
        }
        Emitter.AddRenderer(Existing, Patch.Owner.EmitterVersion);
      }
      Existing->Modify();
      if (!ApplyRendererProperties(*Existing, Emitter,
                                   Patch.Owner.EmitterVersion,
                                   RendererPatch.Properties, OutError))
        return false;
      if (RendererPatch.TargetIndex != INDEX_NONE) {
        Emitter.MoveRenderer(Existing, RendererPatch.TargetIndex,
                             Patch.Owner.EmitterVersion);
      }
    }
    if (Patch.bReplaceRenderers) {
      Patch.Owner.EmitterData =
          Emitter.GetEmitterData(Patch.Owner.EmitterVersion);
      if (!Patch.Owner.EmitterData) {
        OutError =
            TEXT("niagara_workspace_renderer_collection_data_missing:") +
            Patch.SourceKey;
        return false;
      }
      TArray<FString> ExpectedRendererNames;
      for (const FRendererPatch &RendererPatch : Patch.Renderers) {
        if (!RendererPatch.bRemove) {
          ExpectedRendererNames.Add(RendererPatch.RendererName.ToLower());
        }
      }
      const TArray<UNiagaraRendererProperties *> &AppliedRenderers =
          Patch.Owner.EmitterData->GetRenderers();
      if (AppliedRenderers.Num() != ExpectedRendererNames.Num()) {
        OutError = FString::Printf(
            TEXT("niagara_workspace_renderer_collection_replace_"
                 "postcondition_failed:%s:expected=%d:actual=%d"),
            *Patch.SourceKey, ExpectedRendererNames.Num(),
            AppliedRenderers.Num());
        return false;
      }
      for (int32 Index = 0; Index < ExpectedRendererNames.Num(); ++Index) {
        const UNiagaraRendererProperties *AppliedRenderer =
            AppliedRenderers[Index];
        const FString AppliedName =
            AppliedRenderer
                ? AppliedRenderer->GetFName().ToString().ToLower()
                : FString();
        if (!AppliedRenderer ||
            !ExpectedRendererNames.Contains(AppliedName)) {
          OutError = FString::Printf(
              TEXT("niagara_workspace_renderer_collection_replace_"
                   "postcondition_failed:%s:index=%d:actual=%s"),
              *Patch.SourceKey, Index,
              AppliedRenderer ? *AppliedRenderer->GetName()
                              : TEXT("<missing>"));
          return false;
        }
      }
      for (const FRendererPatch &RendererPatch : Patch.Renderers) {
        if (RendererPatch.bRemove ||
            RendererPatch.TargetIndex == INDEX_NONE)
          continue;
        if (!AppliedRenderers.IsValidIndex(RendererPatch.TargetIndex) ||
            !AppliedRenderers[RendererPatch.TargetIndex] ||
            !AppliedRenderers[RendererPatch.TargetIndex]
                 ->GetFName()
                 .ToString()
                 .Equals(RendererPatch.RendererName,
                         ESearchCase::IgnoreCase)) {
          OutError = FString::Printf(
              TEXT("niagara_workspace_renderer_collection_replace_"
                   "postcondition_failed:%s:index=%d:expected=%s:actual=%s"),
              *Patch.SourceKey, RendererPatch.TargetIndex,
              *RendererPatch.RendererName,
              AppliedRenderers.IsValidIndex(RendererPatch.TargetIndex) &&
                      AppliedRenderers[RendererPatch.TargetIndex]
                  ? *AppliedRenderers[RendererPatch.TargetIndex]->GetName()
                  : TEXT("<missing>"));
          return false;
        }
      }
    }

    for (const FEventHandlerPatch &EventPatch : Patch.EventHandlers) {
      FNiagaraEventScriptProperties *Existing = nullptr;
      for (FNiagaraEventScriptProperties &Candidate :
           Patch.Owner.EmitterData->EventHandlerScriptProps) {
        if (Candidate.Script &&
            Candidate.Script->GetUsageId() == EventPatch.UsageId) {
          Existing = &Candidate;
          break;
        }
      }
      if (EventPatch.bRemove) {
        if (Existing) {
          Emitter.RemoveEventHandlerByUsageId(EventPatch.UsageId,
                                              Patch.Owner.EmitterVersion);
          if (Patch.Owner.Graph) {
            FString RemoveError;
            RemoveStageGraph(*Patch.Owner.Graph,
                             ENiagaraScriptUsage::ParticleEventScript,
                             EventPatch.UsageId, RemoveError);
          }
        }
        continue;
      }
      if (!Existing) {
        FNiagaraEventScriptProperties NewEvent;
        NewEvent.Script = NewObject<UNiagaraScript>(
            &Emitter,
            MakeUniqueObjectName(&Emitter, UNiagaraScript::StaticClass(),
                                 TEXT("EventHandler")),
            RF_Transactional);
        if (!NewEvent.Script) {
          OutError = TEXT("niagara_workspace_event_script_create_failed");
          return false;
        }
        NewEvent.Script->SetUsage(ENiagaraScriptUsage::ParticleEventScript);
        NewEvent.Script->SetUsageId(EventPatch.UsageId);
        NewEvent.Script->SetLatestSource(Patch.Owner.ScriptSource);
        Emitter.AddEventHandler(NewEvent, Patch.Owner.EmitterVersion);
        for (FNiagaraEventScriptProperties &Candidate :
             Patch.Owner.EmitterData->EventHandlerScriptProps) {
          if (Candidate.Script &&
              Candidate.Script->GetUsageId() == EventPatch.UsageId) {
            Existing = &Candidate;
            break;
          }
        }
        if (!Patch.Owner.Graph ||
            !EnsureStageBridge(*Patch.Owner.Graph,
                               ENiagaraScriptUsage::ParticleEventScript,
                               EventPatch.UsageId, OutError))
          return false;
      }
      if (!Existing) {
        OutError = TEXT("niagara_workspace_event_handler_resolve_failed");
        return false;
      }
      Existing->SourceEventName = EventPatch.SourceEventName;
      Existing->SourceEmitterID = EventPatch.SourceEmitterId;
      Existing->ExecutionMode = EventPatch.ExecutionMode;
      Existing->SpawnNumber = EventPatch.SpawnNumber;
      Existing->MinSpawnNumber = EventPatch.MinSpawnNumber;
      Existing->bRandomSpawnNumber = EventPatch.bRandomSpawnNumber;
      Existing->MaxEventsPerFrame = EventPatch.MaxEventsPerFrame;
      Existing->UpdateAttributeInitialValues =
          EventPatch.bUpdateAttributeInitialValues;
    }
    Emitter.MarkPackageDirty();
  }
  return true;
}

bool RollbackAddedEmitterHandles(TArray<FEmitterStructurePatch> &Patches,
                                 FString &OutError) {
  OutError.Reset();
  for (int32 PatchIndex = Patches.Num() - 1; PatchIndex >= 0; --PatchIndex) {
    FEmitterStructurePatch &Patch = Patches[PatchIndex];
    if (!Patch.bAddedEmitterHandle || !Patch.AddedHandleId.IsValid())
      continue;
    UNiagaraSystem *System = Patch.TargetSystem.Get();
    if (!System) {
      OutError =
          TEXT("niagara_workspace_emitter_handle_rollback_system_missing:") +
          Patch.SourceKey;
      return false;
    }
    FNiagaraEmitterHandle *AddedHandle = nullptr;
    for (FNiagaraEmitterHandle &Handle : System->GetEmitterHandles()) {
      if (Handle.GetId() == Patch.AddedHandleId) {
        AddedHandle = &Handle;
        break;
      }
    }
    if (AddedHandle)
      System->RemoveEmitterHandle(*AddedHandle);
    Patch.bAddedEmitterHandle = false;
    Patch.AddedHandleId.Invalidate();
  }
  return true;
}

bool RollbackReplacedEmitterHandles(TArray<FEmitterStructurePatch> &Patches,
                                    FString &OutError) {
  OutError.Reset();
  for (int32 PatchIndex = Patches.Num() - 1; PatchIndex >= 0; --PatchIndex) {
    FEmitterStructurePatch &Patch = Patches[PatchIndex];
    if (!Patch.bReplacedEmitterFromSource)
      continue;
    UNiagaraSystem *System = Patch.TargetSystem.Get();
    UNiagaraEmitter *PreviousEmitter = Patch.PreviousEmitterInstance.Get();
    if (!System || !PreviousEmitter ||
        !Patch.PreviousEmitterVersion.IsValid()) {
      OutError = TEXT("niagara_workspace_emitter_source_replace_rollback_state_"
                      "invalid:") +
                 Patch.SourceKey;
      return false;
    }
    FNiagaraEmitterHandle *Handle = nullptr;
    for (FNiagaraEmitterHandle &Candidate : System->GetEmitterHandles()) {
      if (Candidate.GetId() == Patch.Owner.EmitterHandleId) {
        Handle = &Candidate;
        break;
      }
    }
    if (!Handle) {
      OutError = TEXT("niagara_workspace_emitter_source_replace_rollback_"
                      "handle_missing:") +
                 Patch.SourceKey;
      return false;
    }
    System->Modify();
    Handle->GetInstance() =
        FVersionedNiagaraEmitter(PreviousEmitter, Patch.PreviousEmitterVersion);
    Patch.bReplacedEmitterFromSource = false;
  }
  return true;
}

bool RollbackEmitterEnabledStates(TArray<FEmitterStructurePatch> &Patches,
                                  FString &OutError) {
  OutError.Reset();
  for (int32 PatchIndex = Patches.Num() - 1; PatchIndex >= 0; --PatchIndex) {
    FEmitterStructurePatch &Patch = Patches[PatchIndex];
    if (!Patch.bEmitterEnabledChanged ||
        !Patch.bRestoreEmitterEnabledOnRollback)
      continue;
    UNiagaraSystem *System = Patch.TargetSystem.Get();
    FNiagaraEmitterHandle *Handle =
        System ? System->GetEmitterHandles().FindByPredicate(
                     [&Patch](const FNiagaraEmitterHandle &Candidate) {
                       return Candidate.GetId() == Patch.Owner.EmitterHandleId;
                     })
               : nullptr;
    if (!System || !Handle) {
      OutError =
          TEXT("niagara_workspace_emitter_enabled_rollback_handle_missing:") +
          Patch.SourceKey;
      return false;
    }
    System->Modify();
    Handle->SetIsEnabled(Patch.bPreviousEmitterEnabled, *System, false);
    Patch.bEmitterEnabledChanged = false;
    Patch.bRestoreEmitterEnabledOnRollback = false;
  }
  return true;
}

bool CaptureEmitterStructureBackups(
    const TArray<FEmitterStructurePatch> &Patches,
    TArray<FEmitterStructureBackup> &OutBackups, FString &OutError) {
  OutBackups.Reset();
  for (const FEmitterStructurePatch &Patch : Patches) {
    if (!Patch.bNeedsMutation || !Patch.Owner.Emitter ||
        !Patch.Owner.EmitterData)
      continue;
    FEmitterStructureBackup &Backup = OutBackups.AddDefaulted_GetRef();
    Backup.Emitter = Patch.Owner.Emitter;
    Backup.EmitterVersion = Patch.Owner.EmitterVersion;
    if (!CaptureEmitterProperties(Patch, Backup.Properties, OutError))
      return false;
    for (UNiagaraRendererProperties *Renderer :
         Patch.Owner.EmitterData->GetRenderers()) {
      UNiagaraRendererProperties *Copy =
          Renderer ? DuplicateObject<UNiagaraRendererProperties>(
                         Renderer, GetTransientPackage())
                   : nullptr;
      if (!Copy) {
        OutError = TEXT("niagara_renderer_rollback_capture_failed");
        return false;
      }
      FRendererBackup &RendererBackup = Backup.Renderers.AddDefaulted_GetRef();
      RendererBackup.Snapshot.Reset(Copy);
      RendererBackup.OriginalName = Renderer->GetFName();
    }
    Backup.EventHandlers = Patch.Owner.EmitterData->EventHandlerScriptProps;
  }
  return true;
}

bool RestoreEmitterStructureBackups(TArray<FEmitterStructureBackup> &Backups,
                                    FString &OutError) {
  for (FEmitterStructureBackup &Backup : Backups) {
    UNiagaraEmitter *Emitter = Backup.Emitter.Get();
    FVersionedNiagaraEmitterData *Data =
        Emitter ? Emitter->GetEmitterData(Backup.EmitterVersion) : nullptr;
    if (!Emitter || !Data) {
      OutError = TEXT("niagara_emitter_rollback_target_missing");
      return false;
    }
    if (!ApplyEmitterProperties(*Data, *Emitter, Backup.Properties, OutError))
      return false;
    const TArray<UNiagaraRendererProperties *> CurrentRenderers =
        Data->GetRenderers();
    for (UNiagaraRendererProperties *Renderer : CurrentRenderers) {
      if (Renderer)
        Emitter->RemoveRenderer(Renderer, Backup.EmitterVersion);
    }

    TArray<UObject *> EmitterChildren;
    GetObjectsWithOuter(Emitter, EmitterChildren, false);
    for (UObject *Child : EmitterChildren) {
      UNiagaraRendererProperties *Renderer =
          Cast<UNiagaraRendererProperties>(Child);
      if (!Renderer)
        continue;
      const FName RetiredName = MakeUniqueObjectName(
          GetTransientPackage(), Renderer->GetClass(),
          FName(*FString::Printf(TEXT("%s_Retired"), *Renderer->GetName())));
      if (!Renderer->Rename(*RetiredName.ToString(), GetTransientPackage(),
                            REN_DontCreateRedirectors | REN_NonTransactional |
                                REN_ForceNoResetLoaders)) {
        OutError = TEXT("niagara_renderer_rollback_retire_failed:") +
                   Renderer->GetPathName();
        return false;
      }
    }

    for (const FRendererBackup &RendererBackup : Backup.Renderers) {
      UNiagaraRendererProperties *Restored =
          RendererBackup.Snapshot.IsValid()
              ? DuplicateObject<UNiagaraRendererProperties>(
                    RendererBackup.Snapshot.Get(), Emitter,
                    RendererBackup.OriginalName)
              : nullptr;
      if (!Restored) {
        OutError = TEXT("niagara_renderer_rollback_restore_failed");
        return false;
      }
      Emitter->AddRenderer(Restored, Backup.EmitterVersion);
    }
    Data->EventHandlerScriptProps = Backup.EventHandlers;
    Emitter->MarkPackageDirty();
  }
  return true;
}

bool ApplyStagePatches(TArray<FStagePatch> &StagePatches, FString &OutError) {
  using namespace UeAgentNiagaraStageModuleAuthoring;
  FUeAgentStructuredResourceTracker DynamicInputResourceTracker;
  for (FStagePatch &StagePatch : StagePatches) {
    if (!StagePatch.bNeedsMutation)
      continue;
    FResolvedStage &Stage = StagePatch.Stage;
    if (!Stage.Asset || !Stage.ScriptSource || !Stage.Graph ||
        !Stage.OutputNode) {
      OutError = TEXT("niagara_workspace_stage_context_invalid:") +
                 StagePatch.SourceKey;
      return false;
    }
    Stage.Asset->Modify();
    Stage.ScriptSource->Modify();
    Stage.Graph->Modify();
    Stage.OutputNode->Modify();

    TArray<FModulePatch *> Removals;
    TArray<FModulePatch *> Upserts;
    for (FModulePatch &ModulePatch : StagePatch.Modules) {
      if (!ModulePatch.bNeedsMutation)
        continue;
      (ModulePatch.bRemove ? Removals : Upserts).Add(&ModulePatch);
    }

    if (StagePatch.bReplaceStage) {
      if (!RemoveStageGraph(*Stage.Graph, Stage.Usage, Stage.UsageId, OutError))
        return false;
      Stage.OutputNode =
          EnsureStageBridge(*Stage.Graph, Stage.Usage, Stage.UsageId, OutError);
      if (!Stage.OutputNode)
        return false;
      Stage.OutputNode->Modify();
      Removals.Reset();
      Upserts.Reset();
      for (FModulePatch &Patch : StagePatch.Modules) {
        Patch.bExists = false;
        Patch.bReplaceScript = false;
        Patch.bNeedsMutation = true;
        Patch.bLegacyStandaloneNode = false;
        Patch.LegacyNodeGuid.Invalidate();
        for (FLiteralInputPatch &InputPatch : Patch.Inputs)
          InputPatch.bNeedsMutation = true;
        for (FDynamicInputPatch &DynamicPatch : Patch.DynamicInputs)
          MarkDynamicInputPatchForRebuild(DynamicPatch);
        Upserts.Add(&Patch);
      }
    }

    if (UeAgentNiagaraStandaloneModuleAuthoring::IsStandaloneModuleStage(
            Stage)) {
      TArray<UNiagaraNodeFunctionCall *> CurrentLegacyModules;
      GetModules(*Stage.OutputNode, CurrentLegacyModules);
      TArray<UNiagaraNode *> CurrentNativeAssignments;
      UeAgentNiagaraStandaloneModuleAuthoring::GetAssignmentNodes(
          *Stage.OutputNode, CurrentNativeAssignments);
      TArray<FModulePatch *> LegacyMigrations =
          Upserts.FilterByPredicate([](const FModulePatch *Patch) {
            return Patch && Patch->bLegacyStandaloneNode &&
                   Patch->LegacyNodeGuid.IsValid();
          });
      const bool bNeedsNativeStructuralRebuild =
          StagePatch.Modules.ContainsByPredicate([](const FModulePatch &Patch) {
            return !Patch.bRemove && Patch.bStandaloneAssignment &&
                   Patch.bReplaceScript;
          });
      if (!LegacyMigrations.IsEmpty() || bNeedsNativeStructuralRebuild) {
        TSet<FGuid> CoveredLegacyGuids;
        TSet<FGuid> CoveredNativeGuids;
        for (const FModulePatch &Patch : StagePatch.Modules) {
          if (Patch.bRemove)
            continue;
          if (Patch.LegacyNodeGuid.IsValid())
            CoveredLegacyGuids.Add(Patch.LegacyNodeGuid);
          if (Patch.ModuleGuid.IsValid())
            CoveredNativeGuids.Add(Patch.ModuleGuid);
        }
        bool bCoversEveryLegacyModule =
            CurrentLegacyModules.Num() == CoveredLegacyGuids.Num();
        for (const UNiagaraNodeFunctionCall *CurrentModule :
             CurrentLegacyModules) {
          bCoversEveryLegacyModule &=
              CurrentModule &&
              CoveredLegacyGuids.Contains(CurrentModule->NodeGuid);
        }
        bool bCoversEveryNativeAssignment =
            CurrentNativeAssignments.Num() == CoveredNativeGuids.Num();
        for (const UNiagaraNode *CurrentAssignment : CurrentNativeAssignments) {
          bCoversEveryNativeAssignment &=
              CurrentAssignment &&
              CoveredNativeGuids.Contains(CurrentAssignment->NodeGuid);
        }
        const bool bLegacyOnlyRebuild = !LegacyMigrations.IsEmpty() &&
                                        CurrentNativeAssignments.IsEmpty() &&
                                        bCoversEveryLegacyModule;
        const bool bNativeOnlyRebuild = bNeedsNativeStructuralRebuild &&
                                        CurrentLegacyModules.IsEmpty() &&
                                        bCoversEveryNativeAssignment;
        if (!Removals.IsEmpty() ||
            (!bLegacyOnlyRebuild && !bNativeOnlyRebuild)) {
          OutError = TEXT(
              "niagara_standalone_structural_rebuild_requires_complete_stage");
          return false;
        }
        if (!RemoveStageGraph(*Stage.Graph, Stage.Usage, Stage.UsageId,
                              OutError))
          return false;
        Stage.OutputNode = EnsureStageBridge(*Stage.Graph, Stage.Usage,
                                             Stage.UsageId, OutError);
        if (!Stage.OutputNode)
          return false;
        Stage.OutputNode->Modify();
        for (FModulePatch &Patch : StagePatch.Modules) {
          if (Patch.bRemove || !Patch.bStandaloneAssignment)
            continue;
          Patch.bLegacyStandaloneNode = false;
          Patch.bExists = false;
          Patch.bReplaceScript = false;
          Patch.bNeedsMutation = true;
          Patch.LegacyNodeGuid.Invalidate();
          if (!Upserts.Contains(&Patch))
            Upserts.Add(&Patch);
        }
      }
    }

    Removals.Sort(
        [&Stage](const FModulePatch &Left, const FModulePatch &Right) {
          auto IndexOf = [&Stage](const FModulePatch &Patch) {
            FModuleSelector Selector;
            Selector.NodeGuid = Patch.ModuleGuid;
            UNiagaraNodeFunctionCall *Node = nullptr;
            int32 Index = INDEX_NONE;
            FString Error;
            ResolveModule(*Stage.OutputNode, Selector, Node, Index, Error);
            return Index;
          };
          return IndexOf(Left) > IndexOf(Right);
        });
    for (FModulePatch *Patch : Removals) {
      if (Patch->bStandaloneAssignment) {
        UNiagaraNode *AssignmentNode = nullptr;
        int32 CurrentIndex = INDEX_NONE;
        if (UeAgentNiagaraStandaloneModuleAuthoring::ResolveAssignmentNode(
                *Stage.OutputNode, Patch->ModuleGuid, AssignmentNode,
                CurrentIndex, OutError)) {
          if (!UeAgentNiagaraStandaloneModuleAuthoring::RemoveAssignmentNode(
                  *Stage.OutputNode, *AssignmentNode, OutError))
            return false;
          continue;
        }
      }
      FModuleSelector Selector;
      Selector.NodeGuid = Patch->ModuleGuid;
      UNiagaraNodeFunctionCall *Module = nullptr;
      int32 CurrentIndex = INDEX_NONE;
      if (!ResolveModule(*Stage.OutputNode, Selector, Module, CurrentIndex,
                         OutError) ||
          !RemoveModule(*Stage.OutputNode, *Module, OutError))
        return false;
    }

    Upserts.Sort([](const FModulePatch &Left, const FModulePatch &Right) {
      const int32 LeftIndex =
          Left.TargetIndex == INDEX_NONE ? MAX_int32 : Left.TargetIndex;
      const int32 RightIndex =
          Right.TargetIndex == INDEX_NONE ? MAX_int32 : Right.TargetIndex;
      if (LeftIndex != RightIndex)
        return LeftIndex < RightIndex;
      return Left.SourceKey < Right.SourceKey;
    });
    for (FModulePatch *Patch : Upserts) {
      if (Patch->bStandaloneAssignment) {
        UNiagaraNode *AssignmentNode = nullptr;
        int32 CurrentIndex = INDEX_NONE;
        bool bExists =
            Patch->ModuleGuid.IsValid() &&
            UeAgentNiagaraStandaloneModuleAuthoring::ResolveAssignmentNode(
                *Stage.OutputNode, Patch->ModuleGuid, AssignmentNode,
                CurrentIndex, OutError);
        FGuid PreservedGuid = Patch->ModuleGuid;
        if (!bExists && Patch->bLegacyStandaloneNode) {
          FModuleSelector LegacySelector;
          LegacySelector.NodeGuid = Patch->LegacyNodeGuid.IsValid()
                                        ? Patch->LegacyNodeGuid
                                        : Patch->ModuleGuid;
          UNiagaraNodeFunctionCall *LegacyNode = nullptr;
          int32 LegacyIndex = INDEX_NONE;
          if (ResolveModule(*Stage.OutputNode, LegacySelector, LegacyNode,
                            LegacyIndex, OutError)) {
            if (!RemoveModule(*Stage.OutputNode, *LegacyNode, OutError))
              return false;
            CurrentIndex = LegacyIndex;
          }
        } else if (bExists && Patch->bReplaceScript) {
          if (!UeAgentNiagaraStandaloneModuleAuthoring::RemoveAssignmentNode(
                  *Stage.OutputNode, *AssignmentNode, OutError))
            return false;
          AssignmentNode = nullptr;
          bExists = false;
        }

        if (!AssignmentNode) {
          const int32 AddIndex =
              Patch->TargetIndex == INDEX_NONE
                  ? (CurrentIndex == INDEX_NONE ? MAX_int32 : CurrentIndex)
                  : Patch->TargetIndex;
          AssignmentNode =
              UeAgentNiagaraStandaloneModuleAuthoring::AddAssignmentNode(
                  *Stage.OutputNode, Patch->AssignmentTargets, AddIndex,
                  OutError);
          if (!AssignmentNode)
            return false;
          if (PreservedGuid.IsValid())
            AssignmentNode->NodeGuid = PreservedGuid;
          Patch->ModuleGuid = AssignmentNode->NodeGuid;
        }
        if (!UeAgentNiagaraStandaloneModuleAuthoring::
                EnsureAssignmentTargetsRegistered(*AssignmentNode, OutError))
          return false;
        if (!ApplyLiteralInputPatches(*AssignmentNode, Patch->Inputs,
                                      OutError) ||
            !ApplyDynamicInputPatches(*AssignmentNode, Patch->DynamicInputs,
                                      DynamicInputResourceTracker, OutError))
          return false;
        if (Patch->TargetIndex != INDEX_NONE) {
          TArray<UNiagaraNode *> AssignmentNodes;
          UeAgentNiagaraStandaloneModuleAuthoring::GetAssignmentNodes(
              *Stage.OutputNode, AssignmentNodes);
          const int32 AppliedIndex =
              AssignmentNodes.IndexOfByKey(AssignmentNode);
          if (AppliedIndex != Patch->TargetIndex) {
            int32 NewIndex = INDEX_NONE;
            if (!UeAgentNiagaraStandaloneModuleAuthoring::MoveAssignmentNode(
                    *Stage.OutputNode, *AssignmentNode, Patch->TargetIndex,
                    NewIndex, OutError))
              return false;
          }
        }
        continue;
      }
      if (!Patch->NativeCandidateId.IsEmpty()) {
        if (!Stage.Asset) {
          OutError = TEXT("niagara_native_candidate_target_missing:") +
                     Patch->SourceKey;
          return false;
        }
        UE::UAI::QueryOwners::Niagara::FNiagaraModuleCandidateContext
            CandidateContext;
        CandidateContext.TargetUsage = ScriptUsageToString(Stage.Usage);
        CandidateContext.TargetUsageId = Stage.UsageId.ToString(
            EGuidFormats::DigitsWithHyphensLower);
        if (Stage.EmitterHandleId.IsValid()) {
          CandidateContext.EmitterHandleId = Stage.EmitterHandleId.ToString(
              EGuidFormats::DigitsWithHyphensLower);
        }
        UE::UAI::QueryOwners::Niagara::FNiagaraModuleCandidate
            ResolvedCandidate;
        UE::UAI::QueryOwners::Niagara::FNiagaraModuleCandidateContext
            ResolvedContext;
        UE::UAI::QueryOwners::Niagara::FNiagaraModuleCandidateQueryError
            CandidateError;
        if (!UE::UAI::QueryOwners::Niagara::
                TryResolveNiagaraModuleCandidate(
                    *Stage.Asset, FString(), Patch->NativeCandidateId,
                    CandidateContext, ResolvedCandidate, ResolvedContext,
                    CandidateError)) {
          OutError = CandidateError.Code.IsEmpty()
                         ? TEXT("niagara_native_candidate_unresolved:") +
                               Patch->SourceKey
                         : CandidateError.Code + TEXT(":") +
                               CandidateError.SubjectId + TEXT(":") +
                               CandidateError.Detail;
          return false;
        }
        if (!Patch->Script.IsValid() ||
            !ResolvedCandidate.ScriptAssetPath.Equals(
                Patch->Script->GetPathName(), ESearchCase::CaseSensitive) ||
            ResolvedContext.StageKey != Stage.StageKey) {
          OutError = TEXT("niagara_native_candidate_context_changed:") +
                     Patch->SourceKey;
          return false;
        }
      }
      auto AddRequestedModule = [&]() -> UNiagaraNodeFunctionCall * {
        if (Patch->bAssignmentModule) {
          return AddAssignmentModule(*Stage.OutputNode,
                                     Patch->AssignmentTargets,
                                     Patch->TargetIndex, OutError);
        }
        if (!Patch->Script.IsValid()) {
          OutError = TEXT("niagara_workspace_module_script_unresolved:") +
                     Patch->SourceKey;
          return nullptr;
        }
        if (Patch->NativeCandidateId.IsEmpty()) {
          return AddModule(*Stage.OutputNode, *Patch->Script, FString(),
                           Patch->TargetIndex, OutError);
        }
        UE::UAI::QueryOwners::Niagara::FNiagaraModuleCandidateContext
            CandidateContext;
        CandidateContext.TargetUsage = ScriptUsageToString(Stage.Usage);
        CandidateContext.TargetUsageId = Stage.UsageId.ToString(
            EGuidFormats::DigitsWithHyphensLower);
        if (Stage.EmitterHandleId.IsValid()) {
          CandidateContext.EmitterHandleId = Stage.EmitterHandleId.ToString(
              EGuidFormats::DigitsWithHyphensLower);
        }
        UE::UAI::QueryOwners::Niagara::FNiagaraModuleCandidate
            PerformedCandidate;
        UE::UAI::QueryOwners::Niagara::FNiagaraModuleCandidateContext
            PerformedContext;
        UE::UAI::QueryOwners::Niagara::FNiagaraModuleCandidateQueryError
            CandidateError;
        UNiagaraNodeFunctionCall *CreatedNode = nullptr;
        if (!UE::UAI::QueryOwners::Niagara::
                TryPerformNiagaraModuleCandidate(
                    *Stage.Asset, FString(), Patch->NativeCandidateId,
                    CandidateContext, Patch->TargetIndex, PerformedCandidate,
                    PerformedContext, CreatedNode, CandidateError)) {
          OutError = CandidateError.Code.IsEmpty()
                         ? TEXT("niagara_native_candidate_execute_failed:") +
                               Patch->SourceKey
                         : CandidateError.Code + TEXT(":") +
                               CandidateError.SubjectId + TEXT(":") +
                               CandidateError.Detail;
          return nullptr;
        }
        return CreatedNode;
      };
      FModuleSelector Selector;
      Selector.NodeGuid = Patch->ModuleGuid;
      UNiagaraNodeFunctionCall *Module = nullptr;
      int32 CurrentIndex = INDEX_NONE;
      FString ExistingResolveError;
      const bool bExists = Patch->ModuleGuid.IsValid() &&
                           ResolveModule(*Stage.OutputNode, Selector, Module,
                                         CurrentIndex, ExistingResolveError);
      if (bExists && Patch->bReplaceScript) {
        const FGuid PreservedGuid = Module->NodeGuid;
        const int32 ReplacementIndex = Patch->TargetIndex == INDEX_NONE
                                           ? CurrentIndex
                                           : Patch->TargetIndex;
        if (!RemoveModule(*Stage.OutputNode, *Module, OutError))
          return false;
        const int32 RequestedIndex = Patch->TargetIndex;
        Patch->TargetIndex = ReplacementIndex;
        Module = AddRequestedModule();
        Patch->TargetIndex = RequestedIndex;
        if (!Module)
          return false;
        Module->NodeGuid = PreservedGuid;
      } else if (!bExists) {
        Module = AddRequestedModule();
        if (!Module)
          return false;
        if (Patch->ModuleGuid.IsValid())
          Module->NodeGuid = Patch->ModuleGuid;
        Patch->ModuleGuid = Module->NodeGuid;
      }
      if (!Module) {
        OutError =
            TEXT("niagara_workspace_module_apply_failed:") + Patch->SourceKey;
        return false;
      }
      if (!ApplyLiteralInputPatches(*Module, Patch->Inputs, OutError))
        return false;
      if (!ApplyDynamicInputPatches(*Module, Patch->DynamicInputs,
                                    DynamicInputResourceTracker, OutError))
        return false;
      if (Patch->TargetIndex != INDEX_NONE) {
        FModuleSelector AppliedSelector;
        AppliedSelector.NodeGuid = Module->NodeGuid;
        UNiagaraNodeFunctionCall *AppliedModule = nullptr;
        int32 AppliedIndex = INDEX_NONE;
        FString AppliedResolveError;
        if (!ResolveModule(*Stage.OutputNode, AppliedSelector, AppliedModule,
                           AppliedIndex, AppliedResolveError)) {
          OutError =
              TEXT("niagara_workspace_module_post_apply_resolve_failed:") +
              Patch->SourceKey + TEXT(":") + AppliedResolveError;
          return false;
        }
        if (AppliedIndex != Patch->TargetIndex &&
            !MoveModule(*Stage.OutputNode, *AppliedModule, Patch->TargetIndex,
                        AppliedIndex, OutError))
          return false;
      }
    }
    if (StagePatch.bReplaceStage) {
      TArray<const FModulePatch *> ExpectedModules;
      for (const FModulePatch &Patch : StagePatch.Modules) {
        if (!Patch.bRemove)
          ExpectedModules.Add(&Patch);
      }
      ExpectedModules.Sort(
          [](const FModulePatch &Left, const FModulePatch &Right) {
            const int32 LeftIndex =
                Left.TargetIndex == INDEX_NONE ? MAX_int32 : Left.TargetIndex;
            const int32 RightIndex =
                Right.TargetIndex == INDEX_NONE ? MAX_int32 : Right.TargetIndex;
            if (LeftIndex != RightIndex)
              return LeftIndex < RightIndex;
            return Left.SourceKey < Right.SourceKey;
          });
      if (UeAgentNiagaraStandaloneModuleAuthoring::IsStandaloneModuleStage(
              Stage)) {
        TArray<UNiagaraNode *> AppliedAssignments;
        UeAgentNiagaraStandaloneModuleAuthoring::GetAssignmentNodes(
            *Stage.OutputNode, AppliedAssignments);
        if (AppliedAssignments.Num() != ExpectedModules.Num()) {
          OutError = FString::Printf(
              TEXT("niagara_workspace_stage_replace_postcondition_failed:%s:"
                   "expected=%d:actual=%d"),
              *StagePatch.SourceKey, ExpectedModules.Num(),
              AppliedAssignments.Num());
          return false;
        }
        for (int32 Index = 0; Index < ExpectedModules.Num(); ++Index) {
          if (!AppliedAssignments[Index] ||
              !UeAgentNiagaraStandaloneModuleAuthoring::
                  AssignmentTargetsMatch(
                      *AppliedAssignments[Index],
                      ExpectedModules[Index]->AssignmentTargets)) {
            OutError = FString::Printf(
                TEXT("niagara_workspace_stage_replace_postcondition_failed:%s:"
                     "index=%d:assignment_targets_mismatch"),
                *StagePatch.SourceKey, Index);
            return false;
          }
        }
      } else {
        TArray<UNiagaraNodeFunctionCall *> AppliedModules;
        GetModules(*Stage.OutputNode, AppliedModules);
        if (AppliedModules.Num() != ExpectedModules.Num()) {
          OutError = FString::Printf(
              TEXT("niagara_workspace_stage_replace_postcondition_failed:%s:"
                   "expected=%d:actual=%d"),
              *StagePatch.SourceKey, ExpectedModules.Num(),
              AppliedModules.Num());
          return false;
        }
        for (int32 Index = 0; Index < ExpectedModules.Num(); ++Index) {
          const FModulePatch &Expected = *ExpectedModules[Index];
          UNiagaraNodeFunctionCall *Applied = AppliedModules[Index];
          bool bMatches = Applied != nullptr;
          if (bMatches && Expected.bAssignmentModule) {
            const UNiagaraNodeAssignment *AppliedAssignment =
                Cast<UNiagaraNodeAssignment>(Applied);
            bMatches =
                AppliedAssignment &&
                AssignmentTargetsMatch(*AppliedAssignment,
                                       Expected.AssignmentTargets);
          } else if (bMatches) {
            bMatches =
                Expected.Script.IsValid() &&
                GetModuleScriptPath(*Applied).Equals(
                    Expected.Script->GetPathName(),
                    ESearchCase::IgnoreCase);
          }
          if (!bMatches) {
            OutError = FString::Printf(
                TEXT("niagara_workspace_stage_replace_postcondition_failed:%s:"
                     "index=%d:module_identity_mismatch"),
                *StagePatch.SourceKey, Index);
            return false;
          }
        }
      }
    }
    if (!ValidateGraphInputDependenciesAcyclic(*Stage.Graph, OutError))
      return false;
    Stage.Graph->NotifyGraphChanged();
    Stage.Asset->MarkPackageDirty();
  }
  return true;
}

bool RestoreGraphBackups(TArray<FGraphBackup> &Backups, FString &OutError) {
  for (FGraphBackup &Backup : Backups) {
    UNiagaraScriptSource *Source = Backup.Source.Get();
    UNiagaraGraph *Current = Source ? Source->NodeGraph : nullptr;
    if (!Source || !Current || !Backup.Graph.IsValid()) {
      OutError = TEXT("niagara_workspace_graph_rollback_state_invalid");
      return false;
    }
    const FString RetiredName = FString::Printf(
        TEXT("%s_UAIRetired_%s"), *Backup.OriginalName.ToString(),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    if (!Current->Rename(*RetiredName, GetTransientPackage(),
                         REN_DontCreateRedirectors | REN_NonTransactional)) {
      OutError = TEXT("niagara_workspace_graph_rollback_retire_failed");
      return false;
    }
    UNiagaraGraph *Restored = DuplicateObject<UNiagaraGraph>(
        Backup.Graph.Get(), Source, Backup.OriginalName);
    if (!Restored) {
      OutError = TEXT("niagara_workspace_graph_rollback_restore_failed");
      return false;
    }
    Source->NodeGraph = Restored;
  }
  return true;
}

class FAssetLifecycleState final : public IReflectionOperationLifecycleState {
public:
  FAssetLifecycleState(FString InPackagePath, FString InKind,
                       EOperation InOperation, UObject *InExisting,
                       UObject *InProposed, TArray<FStagePatch> InStagePatches,
                       TArray<FEmitterStructurePatch> InEmitterStructurePatches,
                       TArray<UeAgentNiagaraUserParameterAuthoring::FPatch>
                           InUserParameterPatches,
                       TSharedPtr<const FJsonObject> InProviderExtension,
                       bool bInCompile, bool bInSave, bool bInForceDelete,
                       bool bInUncheckedDelete)
      : PackagePath(MoveTemp(InPackagePath)), Kind(MoveTemp(InKind)),
        Operation(InOperation), Existing(InExisting), Proposed(InProposed),
        StagePatches(MoveTemp(InStagePatches)),
        EmitterStructurePatches(MoveTemp(InEmitterStructurePatches)),
        UserParameterPatches(MoveTemp(InUserParameterPatches)),
        ProviderExtension(MoveTemp(InProviderExtension)), bCompile(bInCompile),
        bSave(bInSave), bForceDelete(bInForceDelete),
        bUncheckedDelete(bInUncheckedDelete) {}

  virtual FString GetTargetKey() const override { return PackagePath; }
  virtual EOperation GetResolvedOperation() const override { return Operation; }
  virtual bool IsNoOp() const override {
    if (Operation != EOperation::Update || bCompile || bSave)
      return false;
    for (const FStagePatch &Patch : StagePatches) {
      if (Patch.bNeedsMutation)
        return false;
    }
    for (const FEmitterStructurePatch &Patch : EmitterStructurePatches) {
      if (Patch.bNeedsMutation)
        return false;
    }
    if (!UserParameterPatches.IsEmpty())
      return false;
    return true;
  }
  virtual FReflectionRoot GetProposedRoot() const override {
    return FReflectionRoot::MakeObject(Proposed.Get());
  }
  virtual FReflectionRoot GetActualRoot() const override {
    return FReflectionRoot::MakeObject(Actual ? Actual : Existing);
  }

  virtual bool CaptureRollback(FOperationFailure &OutFailure) override {
    FUeAgentInterfaceLogger::Log(
        TEXT("NiagaraWorkspaceApply phase=capture_rollback_begin asset=%s "
             "stages=%d emitters=%d user_parameters=%d"),
        *PackagePath, StagePatches.Num(), EmitterStructurePatches.Num(),
        UserParameterPatches.Num());
    OutFailure = FOperationFailure();
    if (Operation == EOperation::Update) {
      const bool bNeedsStructuralIsolation =
          StagePatches.ContainsByPredicate(
              [](const FStagePatch &Patch) { return Patch.bNeedsMutation; }) ||
          EmitterStructurePatches.ContainsByPredicate(
              [](const FEmitterStructurePatch &Patch) {
                return Patch.bNeedsMutation;
              });
      if (IsValid(Existing) && bNeedsStructuralIsolation) {
        FString IsolationError;
        if (!UeAgentNiagaraOps::PrepareAssetForStructuralNiagaraEdit(
                *Existing, ClosedEditorCount, IsolationError)) {
          SetFailure(OutFailure, IsolationError);
          return false;
        }
        bEditorIsolationPerformed = true;
      }
      TSet<UNiagaraGraph *> SeenGraphs;
      for (const FStagePatch &Patch : StagePatches) {
        if (!Patch.bNeedsMutation || !Patch.Stage.ScriptSource ||
            !Patch.Stage.Graph || SeenGraphs.Contains(Patch.Stage.Graph))
          continue;
        FGraphBackup &GraphBackup = GraphBackups.AddDefaulted_GetRef();
        GraphBackup.Source = Patch.Stage.ScriptSource;
        GraphBackup.OriginalName = Patch.Stage.Graph->GetFName();
        GraphBackup.Graph.Reset(DuplicateObject<UNiagaraGraph>(
            Patch.Stage.Graph, GetTransientPackage()));
        if (!GraphBackup.Graph.IsValid()) {
          SetFailure(OutFailure,
                     TEXT("niagara_workspace_graph_rollback_capture_failed"));
          return false;
        }
        SeenGraphs.Add(Patch.Stage.Graph);
      }
      for (const FEmitterStructurePatch &Patch : EmitterStructurePatches) {
        if (!Patch.bNeedsMutation || !Patch.Owner.ScriptSource ||
            !Patch.Owner.Graph || SeenGraphs.Contains(Patch.Owner.Graph))
          continue;
        FGraphBackup &GraphBackup = GraphBackups.AddDefaulted_GetRef();
        GraphBackup.Source = Patch.Owner.ScriptSource;
        GraphBackup.OriginalName = Patch.Owner.Graph->GetFName();
        GraphBackup.Graph.Reset(DuplicateObject<UNiagaraGraph>(
            Patch.Owner.Graph, GetTransientPackage()));
        if (!GraphBackup.Graph.IsValid()) {
          SetFailure(OutFailure,
                     TEXT("niagara_workspace_graph_rollback_capture_failed"));
          return false;
        }
        SeenGraphs.Add(Patch.Owner.Graph);
      }
      FString BackupError;
      if (!CaptureEmitterStructureBackups(
              EmitterStructurePatches, EmitterStructureBackups, BackupError)) {
        SetFailure(OutFailure, BackupError);
        return false;
      }
      if (!UserParameterPatches.IsEmpty()) {
        UNiagaraSystem *System = Cast<UNiagaraSystem>(Existing);
        if (!System) {
          SetFailure(OutFailure,
                     TEXT("niagara_workspace_user_parameters_require_system"));
          return false;
        }
        UeAgentNiagaraUserParameterAuthoring::CaptureSnapshot(
            *System, UserParameterSnapshot);
      }
      FUeAgentInterfaceLogger::Log(
          TEXT("NiagaraWorkspaceApply phase=capture_rollback_end asset=%s"),
          *PackagePath);
      return true;
    }
    if (!IsValid(Existing))
      return true;
    FString Error;
    if (!RemovalStage.Capture(Existing, Error)) {
      SetFailure(OutFailure, Error);
      return false;
    }
    return true;
  }

  virtual bool ApplyProposed(FOperationFailure &OutFailure) override {
    FUeAgentInterfaceLogger::Log(
        TEXT("NiagaraWorkspaceApply phase=apply_proposed_begin asset=%s"),
        *PackagePath);
    OutFailure = FOperationFailure();
    FString Error;
    if (!UserParameterPatches.IsEmpty()) {
      FUeAgentInterfaceLogger::Log(
          TEXT("NiagaraWorkspaceApply phase=user_parameters_begin asset=%s "
               "count=%d"),
          *PackagePath, UserParameterPatches.Num());
      UNiagaraSystem *System = Cast<UNiagaraSystem>(Proposed.Get());
      if (!System || !UeAgentNiagaraUserParameterAuthoring::ApplyPatches(
                         *System, UserParameterPatches, Error)) {
        SetFailure(OutFailure,
                   Error.IsEmpty()
                       ? TEXT("niagara_workspace_user_parameter_apply_failed")
                       : Error);
        return false;
      }
      FUeAgentInterfaceLogger::Log(
          TEXT("NiagaraWorkspaceApply phase=user_parameters_end asset=%s"),
          *PackagePath);
    }
    FUeAgentInterfaceLogger::Log(
        TEXT("NiagaraWorkspaceApply phase=emitter_structure_begin asset=%s "
             "count=%d"),
        *PackagePath, EmitterStructurePatches.Num());
    if (!ApplyEmitterStructurePatches(EmitterStructurePatches, Error)) {
      SetFailure(OutFailure, Error);
      return false;
    }
    FUeAgentInterfaceLogger::Log(
        TEXT("NiagaraWorkspaceApply phase=emitter_structure_end asset=%s"),
        *PackagePath);
    if (!EmitterStructurePatches.IsEmpty() && ProviderExtension.IsValid() &&
        ProviderExtension->HasField(TEXT("stages"))) {
      FUeAgentInterfaceLogger::Log(
          TEXT("NiagaraWorkspaceApply phase=reparse_stages_begin asset=%s"),
          *PackagePath);
      if (!ParseStagePatches(Proposed.Get(), PackagePath, ProviderExtension,
                             StagePatches, Error)) {
        SetFailure(OutFailure, Error);
        return false;
      }
      FUeAgentInterfaceLogger::Log(
          TEXT("NiagaraWorkspaceApply phase=reparse_stages_end asset=%s "
               "count=%d"),
          *PackagePath, StagePatches.Num());
    }
    if (!ApplyStagePatches(StagePatches, Error)) {
      SetFailure(OutFailure, Error);
      return false;
    }
    FUeAgentInterfaceLogger::Log(
        TEXT("NiagaraWorkspaceApply phase=stages_end asset=%s count=%d"),
        *PackagePath, StagePatches.Num());
    TSet<UNiagaraSystem *> RefreshedSystems;
    for (const FEmitterStructurePatch &Patch : EmitterStructurePatches) {
      UNiagaraSystem *System = Patch.TargetSystem.Get();
      if (!Patch.bEnsureSystemIntegration || !System ||
          RefreshedSystems.Contains(System))
        continue;
      UeAgentNiagaraOps::RefreshNiagaraSystemAfterStructuralEdit(*System, true,
                                                                 true);
      RefreshedSystems.Add(System);
    }
    FUeAgentInterfaceLogger::Log(
        TEXT("NiagaraWorkspaceApply phase=apply_proposed_end asset=%s"),
        *PackagePath);
    return true;
  }

  virtual bool ApplyLifecycle(FOperationFailure &OutFailure) override {
    OutFailure = FOperationFailure();
    if (Operation == EOperation::Remove) {
      FString Error;
      if (!RemovalStage.Stage(Error)) {
        SetFailure(OutFailure, Error);
        return false;
      }
      Actual = nullptr;
      return true;
    }

    if (!Proposed.IsValid()) {
      SetFailure(OutFailure, TEXT("niagara_proposed_asset_missing"));
      return false;
    }
    if (Operation == EOperation::Update) {
      Actual = Existing;
    } else {
      UPackage *TargetPackage = CreatePackage(*PackagePath);
      const FString AssetName =
          FPackageName::GetLongPackageAssetName(PackagePath);
      Actual =
          DuplicatePublishedAsset(Proposed.Get(), TargetPackage, AssetName);
      if (!Actual) {
        SetFailure(OutFailure, TEXT("niagara_asset_publish_failed"));
        return false;
      }
      FAssetRegistryModule::AssetCreated(Actual);
    }
    if (UNiagaraSystem *System = Cast<UNiagaraSystem>(Actual)) {
      RepairInvalidEmitterVersions(*System);
    }
    Actual->MarkPackageDirty();
    Actual->GetOutermost()->MarkPackageDirty();
    return true;
  }

  virtual bool Compile(FOperationFailure &OutFailure) override {
    OutFailure = FOperationFailure();
    if (Operation == EOperation::Remove) {
      CompileReceipt.Status = TEXT("not_applicable");
      CompileReceipt.bCompilationComplete = true;
      return true;
    }
    if (!bCompile) {
      SetFailure(OutFailure, TEXT("niagara_compile_required"));
      return false;
    }
    FUeAgentInterfaceLogger::Log(
        TEXT("NiagaraWorkspaceApply phase=compile_begin asset=%s"),
        *PackagePath);
    FString Error;
    if (!CompileAssetObject(Actual ? Actual : Existing, CompileReceipt,
                            Error)) {
      SetFailure(OutFailure, Error);
      return false;
    }
    FUeAgentInterfaceLogger::Log(
        TEXT("NiagaraWorkspaceApply phase=compile_end asset=%s"), *PackagePath);
    return true;
  }

  virtual bool Save(FOperationFailure &OutFailure) override {
    OutFailure = FOperationFailure();
    if (!bSave || Operation == EOperation::Remove)
      return true;
    FUeAgentInterfaceLogger::Log(
        TEXT("NiagaraWorkspaceApply phase=save_begin asset=%s"), *PackagePath);
    FString Error;
    if (!SaveAssetObject(Actual ? Actual : Existing, Error)) {
      SetFailure(OutFailure, Error);
      return false;
    }
    FUeAgentInterfaceLogger::Log(
        TEXT("NiagaraWorkspaceApply phase=save_end asset=%s"), *PackagePath);
    return true;
  }

  virtual bool
  BuildDurablePackagePlans(TArray<FPackageDurablePlan> &OutPlans,
                           FOperationFailure &OutFailure) const override {
    return BuildPackageDurablePlan(PackagePath, PackagePath,
                                   Operation == EOperation::Remove
                                       ? Existing
                                       : (Existing ? Existing : Proposed.Get()),
                                   Operation == EOperation::Remove
                                       ? EPackageDurableIntent::Delete
                                       : EPackageDurableIntent::Save,
                                   OutPlans, OutFailure,
                                   Operation != EOperation::Remove ||
                                       (!bForceDelete && !bUncheckedDelete));
  }

  virtual TSharedPtr<FJsonObject> BuildResultMetadata() const override {
    TSharedPtr<FJsonObject> Metadata = MakeShared<FJsonObject>();
    Metadata->SetBoolField(TEXT("editor_isolation_performed"),
                           bEditorIsolationPerformed);
    Metadata->SetNumberField(TEXT("closed_editor_count"), ClosedEditorCount);
    Metadata->SetStringField(TEXT("compile_status"), CompileReceipt.Status);
    Metadata->SetBoolField(TEXT("compilation_complete"),
                           CompileReceipt.bCompilationComplete);
    Metadata->SetNumberField(TEXT("compile_error_count"),
                             CompileReceipt.ErrorCount);
    Metadata->SetNumberField(TEXT("compile_warning_count"),
                             CompileReceipt.WarningCount);
    Metadata->SetNumberField(TEXT("stack_issue_count"),
                             CompileReceipt.StackIssueCount);
    Metadata->SetNumberField(TEXT("stack_warning_count"),
                             CompileReceipt.StackWarningCount);
    TArray<TSharedPtr<FJsonValue>> CompiledTargets;
    CompiledTargets.Reserve(CompileReceipt.CompiledTargets.Num());
    for (const FString &Target : CompileReceipt.CompiledTargets) {
      CompiledTargets.Add(MakeShared<FJsonValueString>(Target));
    }
    Metadata->SetArrayField(TEXT("compiled_targets"), CompiledTargets);
    return Metadata;
  }

  virtual bool BuildDomainExpectedReadRequests(
      TArray<UeAgentAuthoringState::FReadRequest> &OutRequests,
      FOperationFailure &OutFailure) const override {
    return BuildReadRequest(
        Operation == EOperation::Remove ? nullptr : Proposed.Get(),
        Operation == EOperation::Remove, OutRequests, OutFailure);
  }

  virtual bool BuildDomainActualReadRequests(
      TArray<UeAgentAuthoringState::FReadRequest> &OutRequests,
      FOperationFailure &OutFailure) const override {
    return BuildReadRequest(
        Operation == EOperation::Remove ? nullptr
                                        : (Actual ? Actual : Existing),
        Operation == EOperation::Remove, OutRequests, OutFailure);
  }

  virtual bool Commit(FOperationFailure &OutFailure) override {
    OutFailure = FOperationFailure();
    if (Operation == EOperation::Remove && RemovalStage.IsStaged()) {
      FString Error;
      if (!DeleteAssetObject(RemovalStage.GetAsset(), bForceDelete,
                             bUncheckedDelete, Error)) {
        SetFailure(OutFailure, Error);
        return false;
      }
      RemovalStage.Reset();
      Existing = nullptr;
    }
    return true;
  }

  virtual bool Rollback(FOperationFailure &OutFailure) override {
    OutFailure = FOperationFailure();
    if (Operation == EOperation::Update) {
      FString Error;
      if (!UserParameterPatches.IsEmpty()) {
        UNiagaraSystem *System = Cast<UNiagaraSystem>(Existing);
        if (!System || !UeAgentNiagaraUserParameterAuthoring::RestoreSnapshot(
                           *System, UserParameterSnapshot, Error)) {
          SetFailure(
              OutFailure,
              Error.IsEmpty()
                  ? TEXT("niagara_user_parameter_snapshot_restore_failed")
                  : Error);
          return false;
        }
      }
      if (!RollbackAddedEmitterHandles(EmitterStructurePatches, Error)) {
        SetFailure(OutFailure, Error);
        return false;
      }
      if (!RollbackReplacedEmitterHandles(EmitterStructurePatches, Error)) {
        SetFailure(OutFailure, Error);
        return false;
      }
      if (!RollbackEmitterEnabledStates(EmitterStructurePatches, Error)) {
        SetFailure(OutFailure, Error);
        return false;
      }
      if (!RestoreEmitterStructureBackups(EmitterStructureBackups, Error)) {
        SetFailure(OutFailure, Error);
        return false;
      }
      if (!RestoreGraphBackups(GraphBackups, Error)) {
        SetFailure(OutFailure, Error);
        return false;
      }
      return true;
    }
    FString Error;
    if (Operation != EOperation::Remove) {
      if (Actual && !DeleteAssetObject(Actual, true, true, Error)) {
        SetFailure(OutFailure, Error);
        return false;
      }
      Actual = nullptr;
      return true;
    }

    if (!RemovalStage.Rollback(Error)) {
      SetFailure(OutFailure, Error);
      return false;
    }
    Existing->MarkPackageDirty();
    Actual = Existing;
    return true;
  }

private:
  bool
  BuildReadRequest(const UObject *Root, const bool bExpectedAbsent,
                   TArray<UeAgentAuthoringState::FReadRequest> &OutRequests,
                   FOperationFailure &OutFailure) const {
    OutFailure = FOperationFailure();
    UeAgentAuthoringIdentity::FCanonicalIdentity Identity;
    FString IdentityError;
    if (!UeAgentAuthoringIdentity::BuildCanonicalIdentity(
            IdentityModelForKind(Kind), TargetKindForKind(Kind),
            {{TEXT("asset_path"), PackagePath}}, Identity, IdentityError)) {
      SetFailure(OutFailure, TEXT("niagara_asset_identity_invalid"),
                 IdentityError);
      return false;
    }
    UeAgentAuthoringState::FReadRequest Request;
    Request.SemanticId = SemanticForKind(Kind);
    Request.TargetIdentity = Identity;
    Request.TargetQuery.ModelId = Identity.ModelId;
    Request.TargetQuery.TargetKind = Identity.TargetKind;
    Request.TargetQuery.CanonicalKey = Identity.CanonicalKey;
    Request.RootObject = Root;
    Request.RootOwnerPath = PackagePath;
    if (bExpectedAbsent)
      Request.Context.Add(TEXT("expected_absent"), TEXT("true"));
    if (!bExpectedAbsent && Root &&
        (!StagePatches.IsEmpty() || !EmitterStructurePatches.IsEmpty() ||
         !UserParameterPatches.IsEmpty())) {
      FString Error;
      if (!BuildAssetSnapshotReadRequests(*const_cast<UObject *>(Root), true,
                                          OutRequests, Error, PackagePath)) {
        SetFailure(OutFailure,
                   TEXT("niagara_workspace_snapshot_request_failed"), Error);
        return false;
      }
    } else {
      OutRequests.Add(MoveTemp(Request));
    }
    return true;
  }

  FString PackagePath;
  FString Kind;
  EOperation Operation;
  UObject *Existing = nullptr;
  TStrongObjectPtr<UObject> Proposed;
  FAssetRemovalStage RemovalStage;
  UObject *Actual = nullptr;
  TArray<FStagePatch> StagePatches;
  TArray<FEmitterStructurePatch> EmitterStructurePatches;
  TArray<UeAgentNiagaraUserParameterAuthoring::FPatch> UserParameterPatches;
  TSharedPtr<const FJsonObject> ProviderExtension;
  TArray<FGraphBackup> GraphBackups;
  TArray<FEmitterStructureBackup> EmitterStructureBackups;
  TArray<UeAgentNiagaraUserParameterAuthoring::FSnapshotEntry>
      UserParameterSnapshot;
  FAssetCompileReceipt CompileReceipt;
  bool bCompile = false;
  bool bSave = true;
  bool bForceDelete = false;
  bool bUncheckedDelete = false;
  bool bEditorIsolationPerformed = false;
  int32 ClosedEditorCount = 0;
};

class FAssetLifecycle final : public IReflectionOperationLifecycle {
public:
  virtual bool
  PrepareLifecycle(const FPrepareRequest &Request,
                   TUniquePtr<IReflectionOperationLifecycleState> &OutState,
                   FOperationFailure &OutFailure) const override {
    OutState.Reset();
    OutFailure = FOperationFailure();
    if (!Request.IsValid() || !Request.Input->Target.IsValid()) {
      SetFailure(OutFailure, TEXT("niagara_asset_apply_request_invalid"));
      return false;
    }

    FString PackagePath;
    Request.Input->Target->TryGetStringField(TEXT("asset_path"), PackagePath);
    PackagePath = NormalizePackagePath(PackagePath);
    if (!FPackageName::IsValidLongPackageName(PackagePath) ||
        FPackageName::GetLongPackageAssetName(PackagePath).IsEmpty()) {
      SetFailure(OutFailure, TEXT("invalid_asset_path"), PackagePath);
      return false;
    }

    UObject *Existing = LoadAsset(PackagePath);
    EOperation Operation = Request.Input->RequestedOperation;
    if (Operation == EOperation::Upsert) {
      Operation = Existing ? EOperation::Update : EOperation::Create;
    }
    if ((Operation == EOperation::Create || Operation == EOperation::Clone) &&
        Existing) {
      SetFailure(OutFailure, TEXT("asset_already_exists"), PackagePath);
      return false;
    }
    if ((Operation == EOperation::Update || Operation == EOperation::Remove) &&
        !Existing) {
      SetFailure(OutFailure, TEXT("asset_not_found"), PackagePath);
      return false;
    }

    FString Kind;
    Request.Input->Target->TryGetStringField(TEXT("asset_kind"), Kind);
    Kind.TrimStartAndEndInline();
    Kind.ToLowerInline();
    UObject *Source = nullptr;
    if (Operation == EOperation::Clone) {
      FString SourcePath;
      Request.Input->Target->TryGetStringField(TEXT("source_asset_path"),
                                               SourcePath);
      Source = LoadAsset(SourcePath);
      if (!Source) {
        SetFailure(OutFailure, TEXT("source_asset_not_found"), SourcePath);
        return false;
      }
    }
    const FString InferredKind = GetAssetKind(Existing ? Existing : Source);
    if (Kind.IsEmpty())
      Kind = InferredKind;
    if (!InferredKind.IsEmpty() && Kind != InferredKind) {
      SetFailure(OutFailure, TEXT("niagara_asset_kind_conflict"),
                 Kind + TEXT(" != ") + InferredKind);
      return false;
    }
    if (Kind != TEXT("system") && Kind != TEXT("emitter") &&
        Kind != TEXT("script")) {
      SetFailure(OutFailure, TEXT("niagara_asset_kind_invalid"), Kind);
      return false;
    }
    if (Kind != TEXT("script") && Request.Input->ProviderExtension.IsValid() &&
        Request.Input->ProviderExtension->HasField(TEXT("script_usage"))) {
      SetFailure(OutFailure,
                 TEXT("niagara_workspace_script_usage_asset_kind_invalid"),
                 Kind);
      return false;
    }

    UObject *Proposed = nullptr;
    FString RequestedScriptUsage;
    if (Kind == TEXT("script") && Request.Input->ProviderExtension.IsValid()) {
      Request.Input->ProviderExtension->TryGetStringField(TEXT("script_usage"),
                                                          RequestedScriptUsage);
    }
    if (Operation != EOperation::Remove) {
      UPackage *ScratchPackage = CreatePackage(
          *FString::Printf(TEXT("/Temp/UAI/Niagara/Proposed_%s"),
                           *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
      if (!ScratchPackage) {
        SetFailure(OutFailure, TEXT("niagara_proposed_package_create_failed"));
        return false;
      }
      if (Operation == EOperation::Update) {
        Proposed = Existing;
      } else if (Operation == EOperation::Create) {
        UClass *Class = Kind == TEXT("system") ? UNiagaraSystem::StaticClass()
                        : Kind == TEXT("emitter")
                            ? UNiagaraEmitter::StaticClass()
                            : UNiagaraScript::StaticClass();
        FString Error;
        Proposed = CreateFactorySeed(
            Class, ScratchPackage, Kind,
            ReadBool(Request.Input->ProviderExtension,
                     TEXT("create_default_nodes"), true),
            ReadBool(Request.Input->ProviderExtension,
                     TEXT("add_default_modules_and_renderers"), true),
            Error, RequestedScriptUsage);
        if (!Proposed) {
          SetFailure(OutFailure, Error);
          return false;
        }
      } else {
        FString ValidationError;
        if (!ValidateAssetForDuplication(Source, ValidationError)) {
          SetFailure(OutFailure, ValidationError);
          return false;
        }
        Proposed = StaticDuplicateObject(Source, ScratchPackage, NAME_None,
                                         RF_Transactional);
        if (!Proposed) {
          SetFailure(OutFailure, TEXT("niagara_proposed_asset_create_failed"));
          return false;
        }
      }
    }
    if (UNiagaraScript *Script = Cast<UNiagaraScript>(Proposed)) {
      if (!RequestedScriptUsage.IsEmpty()) {
        ENiagaraScriptUsage RequestedUsage = ENiagaraScriptUsage::Module;
        if (!UeAgentNiagaraStageModuleAuthoring::TryParseScriptUsage(
                RequestedScriptUsage, RequestedUsage)) {
          SetFailure(OutFailure, TEXT("niagara_workspace_script_usage_invalid"),
                     RequestedScriptUsage);
          return false;
        }
        if (Script->GetUsage() != RequestedUsage) {
          SetFailure(
              OutFailure, TEXT("niagara_workspace_script_usage_conflict"),
              UeAgentNiagaraStageModuleAuthoring::ScriptUsageToString(
                  Script->GetUsage()) +
                  TEXT(" != ") +
                  UeAgentNiagaraStageModuleAuthoring::ScriptUsageToString(
                      RequestedUsage));
          return false;
        }
      }
      if (Request.Input->ProviderExtension.IsValid() &&
          Request.Input->ProviderExtension->HasField(TEXT("stages")) &&
          Script->GetUsage() != ENiagaraScriptUsage::Module) {
        SetFailure(OutFailure,
                   TEXT("niagara_workspace_script_stage_usage_unsupported"),
                   UeAgentNiagaraStageModuleAuthoring::ScriptUsageToString(
                       Script->GetUsage()));
        return false;
      }
    }
    TArray<FStagePatch> StagePatches;
    if (Operation == EOperation::Remove &&
        Request.Input->ProviderExtension.IsValid() &&
        (Request.Input->ProviderExtension->HasField(TEXT("stages")) ||
         Request.Input->ProviderExtension->HasField(TEXT("emitters")) ||
         Request.Input->ProviderExtension->HasField(TEXT("user_parameters")))) {
      SetFailure(OutFailure,
                 TEXT("niagara_workspace_remove_extensions_not_allowed"));
      return false;
    }
    TArray<FEmitterStructurePatch> EmitterStructurePatches;
    FString StructurePatchError;
    FUeAgentInterfaceLogger::Log(
        TEXT("NiagaraWorkspaceApply phase=parse_emitters_begin asset=%s"),
        *PackagePath);
    if (!ParseEmitterStructurePatches(
            Proposed, PackagePath, Request.Input->ProviderExtension,
            EmitterStructurePatches, StructurePatchError)) {
      SetFailure(OutFailure, StructurePatchError);
      return false;
    }
    FUeAgentInterfaceLogger::Log(
        TEXT(
            "NiagaraWorkspaceApply phase=parse_emitters_end asset=%s count=%d"),
        *PackagePath, EmitterStructurePatches.Num());
    TArray<UeAgentNiagaraUserParameterAuthoring::FPatch> UserParameterPatches;
    FString UserParameterPatchError;
    FUeAgentInterfaceLogger::Log(
        TEXT(
            "NiagaraWorkspaceApply phase=parse_user_parameters_begin asset=%s"),
        *PackagePath);
    if (!UeAgentNiagaraUserParameterAuthoring::ParsePatches(
            Cast<UNiagaraSystem>(Proposed), Request.Input->ProviderExtension,
            UserParameterPatches, UserParameterPatchError)) {
      SetFailure(OutFailure, UserParameterPatchError);
      return false;
    }
    FUeAgentInterfaceLogger::Log(
        TEXT("NiagaraWorkspaceApply phase=parse_user_parameters_end asset=%s "
             "count=%d"),
        *PackagePath, UserParameterPatches.Num());
    FString StagePatchError;
    FUeAgentInterfaceLogger::Log(
        TEXT("NiagaraWorkspaceApply phase=parse_stages_begin asset=%s"),
        *PackagePath);
    if (!ParseStagePatches(Proposed, PackagePath,
                           Request.Input->ProviderExtension, StagePatches,
                           StagePatchError)) {
      const bool bDeferredEventStage =
          !EmitterStructurePatches.IsEmpty() &&
          StagePatchError.StartsWith(TEXT("niagara_stage_not_found:"));
      if (!bDeferredEventStage) {
        SetFailure(OutFailure, StagePatchError);
        return false;
      }
      StagePatches.Reset();
    }
    FUeAgentInterfaceLogger::Log(
        TEXT("NiagaraWorkspaceApply phase=parse_stages_end asset=%s count=%d"),
        *PackagePath, StagePatches.Num());

    OutState = MakeUnique<FAssetLifecycleState>(
        PackagePath, Kind, Operation, Existing, Proposed,
        MoveTemp(StagePatches), MoveTemp(EmitterStructurePatches),
        MoveTemp(UserParameterPatches), Request.Input->ProviderExtension,
        ReadBool(Request.Input->ProviderExtension, TEXT("compile"), true),
        ReadBool(Request.Input->ProviderExtension, TEXT("save"), true),
        ReadBool(Request.Input->ProviderExtension, TEXT("force_delete"), false),
        ReadBool(Request.Input->ProviderExtension, TEXT("use_unchecked_delete"),
                 false));
    return true;
  }
};

FInputFieldDescriptor Field(const TCHAR *Name, const TCHAR *Role,
                            const TCHAR *TypeId,
                            const TArray<EOperation> &RequiredFor = {}) {
  FInputFieldDescriptor Result;
  Result.Name = Name;
  Result.Role = Role;
  Result.TypeId = TypeId;
  Result.RequiredFor = RequiredFor;
  return Result;
}

FOperationDescriptor OperationDescriptor(const EOperation Operation) {
  FOperationDescriptor Result;
  Result.Operation = Operation;
  Result.IdentityOneOf = {{TEXT("asset_path")}};
  Result.AcceptedSections = {TEXT("target"), TEXT("provider_extension")};
  if (Operation != EOperation::Remove) {
    Result.AcceptedSections.Add(TEXT("properties"));
  }
  if (Operation == EOperation::Create) {
    Result.OmittedFieldPolicy = TEXT("ue_native_default");
    Result.NativeDefaultPolicy = TEXT("required");
  } else if (Operation == EOperation::Update) {
    Result.OmittedFieldPolicy = TEXT("preserve_current");
    Result.NativeDefaultPolicy = TEXT("forbidden");
  } else if (Operation == EOperation::Clone ||
             Operation == EOperation::Upsert) {
    Result.OmittedFieldPolicy = TEXT("ue_native_default");
    Result.NativeDefaultPolicy = TEXT("allowed");
  } else {
    Result.OmittedFieldPolicy = TEXT("not_applicable");
    Result.NativeDefaultPolicy = TEXT("not_applicable");
  }
  Result.bDestructive = Operation == EOperation::Remove;
  return Result;
}

FContract MakeInputContract() {
  FContract Result;
  Result.Schema = TEXT("uai.apply_input_contract.v1");
  Result.ContractId = AssetInputContractId;
  Result.ContractVersion = TEXT("1.0.0");
  Result.ProfileId = TEXT("niagara.asset");
  Result.TargetKind = TEXT("niagara_asset");
  Result.SemanticDescriptorRefs = {
      SystemSemanticId,       EmitterSemanticId,  ScriptSemanticId,
      EmitterOwnerSemanticId, RendererSemanticId, EventHandlerSemanticId,
      UserParameterSemanticId};
  Result.ReflectionBaseline.bEnabled = true;
  Result.ReflectionBaseline.RootKinds = {TEXT("uobject"), TEXT("ustruct")};
  Result.ReflectionBaseline.RecursiveNodes = {
      TEXT("scalar"),    TEXT("struct"),      TEXT("owned_object"),
      TEXT("reference"), TEXT("array"),       TEXT("set"),
      TEXT("map"),       TEXT("fixed_array"), TEXT("optional")};
  Result.ReflectionBaseline.UnknownFieldPolicy = TEXT("reject");
  Result.ReflectionBaseline.ExternalReferencePolicy =
      TEXT("replace_reference_only");
  Result.ReflectionBaseline.OwnedObjectPolicy =
      TEXT("create_or_locate_with_real_outer_then_recurse");
  Result.ReflectionBaseline.RuntimeLimitsSource = TEXT("shared_runtime_limits");
  Result.PropertyPayload.Mode = TEXT("reflection");
  Result.PropertyPayload.PropertyPathField = TEXT("property_path");
  Result.PropertyPayload.ValueField = TEXT("value");
  Result.PropertyPayload.OptionalAssertionFields = {
      TEXT("value_kind"),   TEXT("struct_path"),  TEXT("enum_path"),
      TEXT("element_type"), TEXT("key_type"),     TEXT("mapped_type"),
      TEXT("inner_type"),   TEXT("fixed_length"), TEXT("allowed_class")};
  Result.PropertyPayload.OmittedPatchFieldPolicy = TEXT("preserve_current");
  const TArray<EOperation> All = {EOperation::Create, EOperation::Update,
                                  EOperation::Upsert, EOperation::Remove,
                                  EOperation::Clone};
  Result.InputFields = {
      Field(TEXT("asset_path"), TEXT("identity"), TEXT("ue_asset_path"), All),
      Field(TEXT("asset_kind"), TEXT("type_assertion"),
            TEXT("niagara_asset_kind"), {EOperation::Create}),
      Field(TEXT("source_asset_path"), TEXT("selector"), TEXT("ue_asset_path"),
            {EOperation::Clone}),
      Field(TEXT("operation"), TEXT("operation_control"),
            TEXT("apply_operation"), All),
      Field(TEXT("properties"), TEXT("intent"),
            TEXT("property_descriptor_list")),
      Field(TEXT("create_default_nodes"), TEXT("provider_extension"),
            TEXT("bool")),
      Field(TEXT("add_default_modules_and_renderers"),
            TEXT("provider_extension"), TEXT("bool")),
      Field(TEXT("compile"), TEXT("provider_extension"), TEXT("bool")),
      Field(TEXT("save"), TEXT("provider_extension"), TEXT("bool")),
      Field(TEXT("force_delete"), TEXT("provider_extension"), TEXT("bool")),
      Field(TEXT("use_unchecked_delete"), TEXT("provider_extension"),
            TEXT("bool")),
      Field(TEXT("script_usage"), TEXT("provider_extension"),
            TEXT("niagara_script_usage")),
      Field(TEXT("stages"), TEXT("provider_extension"), TEXT("object")),
      Field(TEXT("emitters"), TEXT("provider_extension"), TEXT("object")),
      Field(TEXT("user_parameters"), TEXT("provider_extension"),
            TEXT("object")),
  };
  for (const EOperation Item : All)
    Result.Operations.Add(OperationDescriptor(Item));
  Result.ProviderId = ApplyProviderId;
  Result.UnknownInputFieldPolicy = TEXT("reject");
  Result.Verification.StateSource = TEXT("shared_state_snapshot");
  Result.Verification.CompareSource = TEXT("shared_semantic_compare");
  Result.Verification.bRequired = true;
  return Result;
}

UeAgentApplyWorkspaceContract::FContract MakeWorkspaceContract() {
  using namespace UeAgentApplyWorkspaceContract;
  UeAgentApplyWorkspaceContract::FContract Result;
  Result.Schema = TEXT("uai.apply_workspace_contract.v1");
  Result.ContractId = AssetWorkspaceContractId;
  Result.ContractVersion = TEXT("1.0.0");
  Result.ProfileId = TEXT("niagara.asset");
  Result.Sources = {
      {ESourceKind::Directory, TEXT("physical"), TEXT("index")},
      {ESourceKind::SingleFile, TEXT("virtual"), TEXT("object")},
      {ESourceKind::Inline, TEXT("virtual"), TEXT("object")},
  };
  Result.Entry.DirectoryEntry = TEXT("index.hjson");
  Result.Entry.VirtualUnitId = TEXT("asset");
  FUnitDescriptor Unit;
  Unit.UnitId = TEXT("asset");
  Unit.PathPattern = TEXT("index.hjson");
  Unit.Role = TEXT("object");
  Unit.Cardinality = TEXT("one");
  Unit.bRequired = true;
  Unit.RouteId = TEXT("niagara.asset");
  Unit.ApplyContractRef = AssetInputContractId;
  Unit.MergePolicy = TEXT("replace");
  Unit.AbsentSemantics = TEXT("error");
  Unit.EmptySemantics = TEXT("error");
  Result.Units = {Unit};
  Result.ReferencePolicy.Base = TEXT("workspace_root");
  Result.ReferencePolicy.RootContainment = TEXT("required");
  Result.ReferencePolicy.ExternalReferences = TEXT("reject");
  Result.ReferencePolicy.CaseCollision = TEXT("reject");
  Result.DependencyPolicy.EdgeSource = TEXT("contract_and_references");
  Result.DependencyPolicy.CyclePolicy = TEXT("reject");
  Result.DependencyPolicy.UnresolvedPolicy = TEXT("reject");
  Result.DependencyPolicy.Order = TEXT("stable_topological");
  Result.Preflight.Scope = TEXT("whole_workspace");
  Result.Preflight.MutationBeforeSuccess = TEXT("forbid");
  Result.Preflight.bDiagnosticsIncludeSource = true;
  Result.Transaction.Mode = TEXT("atomic");
  Result.Transaction.RollbackOn = {TEXT("mutation_failed"),
                                   TEXT("compile_failed"), TEXT("save_failed"),
                                   TEXT("verification_failed")};
  Result.Execution.Compile = TEXT("provider_hook_required");
  Result.Execution.Save = TEXT("caller_controlled_required_default");
  Result.Execution.Verification = TEXT("shared_snapshot_required");
  Result.UnknownFilePolicy = TEXT("reject");
  Result.DuplicateTargetPolicy = TEXT("reject");
  return Result;
}

TSharedRef<FJsonObject> MakeApplyDocument(const FAssetApplyRequest &Request) {
  TSharedRef<FJsonObject> Target = MakeShared<FJsonObject>();
  Target->SetStringField(TEXT("asset_path"), Request.AssetPath);
  if (!Request.AssetKind.IsEmpty())
    Target->SetStringField(TEXT("asset_kind"), Request.AssetKind);
  if (!Request.SourceAssetPath.IsEmpty())
    Target->SetStringField(TEXT("source_asset_path"), Request.SourceAssetPath);

  TSharedRef<FJsonObject> Extension = MakeShared<FJsonObject>();
  Extension->SetBoolField(TEXT("create_default_nodes"),
                          Request.bCreateDefaultSystemNodes);
  Extension->SetBoolField(TEXT("add_default_modules_and_renderers"),
                          Request.bAddDefaultEmitterModulesAndRenderers);
  Extension->SetBoolField(TEXT("compile"), Request.bCompile);
  Extension->SetBoolField(TEXT("save"), Request.bSave);
  Extension->SetBoolField(TEXT("force_delete"), Request.bForceDelete);
  Extension->SetBoolField(TEXT("use_unchecked_delete"),
                          Request.bUseUncheckedDelete);

  TSharedRef<FJsonObject> Document = MakeShared<FJsonObject>();
  Document->SetStringField(TEXT("operation"),
                           OperationToString(Request.Operation));
  Document->SetObjectField(TEXT("target"), Target);
  Document->SetObjectField(TEXT("provider_extension"), Extension);
  if (!Request.Properties.IsEmpty())
    Document->SetArrayField(TEXT("properties"), Request.Properties);
  return Document;
}

bool ExecuteAssetWorkspaceSource(
    const UeAgentAuthoringWorkspaceLoader::FSourceRequest &Source,
    const FString &ExpectedAssetKind, const FString &ExpectedAssetPath,
    FAssetApplyResult &OutResult, const FString &RequestId) {
  OutResult = FAssetApplyResult();
  OutResult.Status = TEXT("failed");
  UeAgentApplyWorkspaceContract::FContract WorkspaceContract;
  if (!UeAgentApplyWorkspaceContract::FRegistry::Get().FindContract(
          AssetWorkspaceContractId, WorkspaceContract)) {
    OutResult.Phase = TEXT("registration");
    OutResult.ErrorCode = TEXT("niagara_workspace_contract_missing");
    OutResult.ErrorDetail = OutResult.ErrorCode;
    return false;
  }

  UeAgentAuthoringWorkspaceLoader::FLoadResult LoadResult;
  if (!UeAgentAuthoringWorkspaceLoader::LoadWorkspace(WorkspaceContract, Source,
                                                      LoadResult)) {
    OutResult.Phase = TEXT("load");
    OutResult.ErrorCode = LoadResult.ErrorCode;
    OutResult.ErrorDetail = LoadResult.ErrorDetail;
    return false;
  }

  const UeAgentAuthoringWorkspaceRouter::FWorkspaceUnit *AssetUnit =
      LoadResult.Workspace.FindSingleUnit(TEXT("asset"));
  const TSharedPtr<const FJsonObject> Document =
      AssetUnit ? AssetUnit->Document : nullptr;
  const TSharedPtr<FJsonObject> *Target = nullptr;
  FString OperationText;
  EOperation RequestedOperation = EOperation::Update;
  if (!Document.IsValid() ||
      !Document->TryGetStringField(TEXT("operation"), OperationText) ||
      !ParseOperation(OperationText, RequestedOperation) ||
      !Document->TryGetObjectField(TEXT("target"), Target) || !Target ||
      !Target->IsValid()) {
    OutResult.Phase = TEXT("load");
    OutResult.ErrorCode = TEXT("niagara_workspace_entry_invalid");
    OutResult.ErrorDetail = OutResult.ErrorCode;
    return false;
  }

  (*Target)->TryGetStringField(TEXT("asset_path"), OutResult.AssetPath);
  OutResult.AssetPath = NormalizePackagePath(OutResult.AssetPath);
  (*Target)->TryGetStringField(TEXT("asset_kind"), OutResult.AssetKind);
  OutResult.AssetKind.TrimStartAndEndInline();
  OutResult.AssetKind.ToLowerInline();
  const FString NormalizedExpectedPath =
      NormalizePackagePath(ExpectedAssetPath);
  FString NormalizedExpectedKind = ExpectedAssetKind.TrimStartAndEnd();
  NormalizedExpectedKind.ToLowerInline();
  if (!NormalizedExpectedPath.IsEmpty() &&
      !OutResult.AssetPath.Equals(NormalizedExpectedPath,
                                  ESearchCase::IgnoreCase)) {
    OutResult.Phase = TEXT("preflight");
    OutResult.ErrorCode = TEXT("niagara_workspace_asset_path_conflict");
    OutResult.ErrorDetail =
        OutResult.AssetPath + TEXT(" != ") + NormalizedExpectedPath;
    return false;
  }
  UObject *ExistingBeforeApply = LoadAsset(OutResult.AssetPath);
  const bool bExistedBeforeApply = ExistingBeforeApply != nullptr;
  if (OutResult.AssetKind.IsEmpty())
    OutResult.AssetKind = GetAssetKind(ExistingBeforeApply);
  if (!NormalizedExpectedKind.IsEmpty() &&
      !OutResult.AssetKind.Equals(NormalizedExpectedKind,
                                  ESearchCase::IgnoreCase)) {
    OutResult.Phase = TEXT("preflight");
    OutResult.ErrorCode = TEXT("niagara_workspace_asset_kind_conflict");
    OutResult.ErrorDetail =
        OutResult.AssetKind + TEXT(" != ") + NormalizedExpectedKind;
    return false;
  }
  if (RequestedOperation == EOperation::Clone) {
    FString SourceAssetPath;
    (*Target)->TryGetStringField(TEXT("source_asset_path"), SourceAssetPath);
    if (const UNiagaraSystem *SourceSystem =
            Cast<UNiagaraSystem>(LoadAsset(SourceAssetPath))) {
      OutResult.RepairedEmitterVersionCount =
          CountInvalidEmitterVersions(*SourceSystem);
    }
  }

  UeAgentAuthoringWorkspaceDependencyGraph::FDependencyGraph Graph;
  UeAgentAuthoringWorkspaceDependencyGraph::FGraphFailure GraphFailure;
  if (!UeAgentAuthoringWorkspaceDependencyGraph::BuildDependencyGraph(
          WorkspaceContract, LoadResult.Workspace,
          UeAgentAuthoringWorkspaceDependencyGraph::FDependencyInputs(), Graph,
          GraphFailure)) {
    OutResult.Phase = TEXT("preflight");
    OutResult.ErrorCode = GraphFailure.ErrorCode;
    OutResult.ErrorDetail = GraphFailure.ErrorDetail;
    return false;
  }

  bool bSave = false;
  const TSharedPtr<FJsonObject> *ProviderExtension = nullptr;
  if (Document->TryGetObjectField(TEXT("provider_extension"),
                                  ProviderExtension) &&
      ProviderExtension && ProviderExtension->IsValid()) {
    (*ProviderExtension)->TryGetBoolField(TEXT("save"), bSave);
  }
  FBackendOptions BackendOptions;
  BackendOptions.bUseEditorTransaction =
      RequestedOperation != EOperation::Remove;
  BackendOptions.TransactionDescription =
      NSLOCTEXT("UeAgentInterface", "NiagaraAssetWorkspaceApply",
                "Apply Niagara asset workspace");
  UeAgentAuthoringWorkspaceTransaction::FExecutionOptions ExecutionOptions;
  FOperationFailure RequestFailure;
  if (!BuildApplyEntryExecutionOptions(RequestId, ExecutionOptions,
                                       RequestFailure)) {
    OutResult.Phase = TEXT("preflight");
    OutResult.ErrorCode = RequestFailure.ErrorCode;
    OutResult.ErrorDetail = RequestFailure.ErrorDetail;
    return false;
  }
  ExecutionOptions.bSaveWhenCallerControlled = bSave;
  FApplyWorkspaceResult ApplyResult;
  const bool bApplied = ApplyWorkspace(
      WorkspaceContract, LoadResult.Workspace, Graph,
      UeAgentApplyInputContract::FRegistry::Get(), FProviderRegistry::Get(),
      ExecutionOptions, BackendOptions, ApplyResult);
  OutResult.UnitsChecked = ApplyResult.Preflight.UnitsChecked;
  OutResult.MutationsApplied = ApplyResult.Execution.MutationsApplied;
  OutResult.bRollbackAttempted = ApplyResult.Execution.bRollbackAttempted;
  OutResult.bRollbackSucceeded = ApplyResult.Execution.bRollbackSucceeded;
  if (!bApplied) {
    const bool bPreflight = !ApplyResult.Preflight.IsReady();
    OutResult.Phase =
        bPreflight ? TEXT("preflight") : ApplyResult.Execution.Phase;
    OutResult.ErrorCode = bPreflight ? ApplyResult.Preflight.CauseCode
                                     : ApplyResult.Execution.CauseCode;
    if (OutResult.ErrorCode.IsEmpty()) {
      OutResult.ErrorCode = bPreflight ? ApplyResult.Preflight.ErrorCode
                                       : ApplyResult.Execution.ErrorCode;
    }
    OutResult.ErrorDetail = bPreflight ? ApplyResult.Preflight.ErrorDetail
                                       : ApplyResult.Execution.ErrorDetail;
    return false;
  }

  const EOperation ResolvedOperation =
      RequestedOperation == EOperation::Upsert
          ? (bExistedBeforeApply ? EOperation::Update : EOperation::Create)
          : RequestedOperation;
  OutResult.ResolvedOperation = OperationToString(ResolvedOperation);
  if (const TSharedPtr<FJsonObject> *Metadata =
          ApplyResult.UnitResultMetadata.Find(TEXT("asset"))) {
    if (Metadata->IsValid()) {
      (*Metadata)->TryGetBoolField(TEXT("editor_isolation_performed"),
                                   OutResult.bEditorIsolationPerformed);
      double ClosedEditorCount = 0.0;
      if ((*Metadata)->TryGetNumberField(TEXT("closed_editor_count"),
                                         ClosedEditorCount)) {
        OutResult.ClosedEditorCount =
            FMath::Max(0, FMath::RoundToInt(ClosedEditorCount));
      }
      (*Metadata)->TryGetStringField(TEXT("compile_status"),
                                     OutResult.CompileStatus);
      (*Metadata)->TryGetBoolField(TEXT("compilation_complete"),
                                   OutResult.bCompilationComplete);
      double CompileErrorCount = 0.0;
      if ((*Metadata)->TryGetNumberField(TEXT("compile_error_count"),
                                         CompileErrorCount)) {
        OutResult.CompileErrorCount =
            FMath::Max(0, FMath::RoundToInt(CompileErrorCount));
      }
      double CompileWarningCount = 0.0;
      if ((*Metadata)->TryGetNumberField(TEXT("compile_warning_count"),
                                         CompileWarningCount)) {
        OutResult.CompileWarningCount =
            FMath::Max(0, FMath::RoundToInt(CompileWarningCount));
      }
      double StackIssueCount = 0.0;
      if ((*Metadata)->TryGetNumberField(TEXT("stack_issue_count"),
                                         StackIssueCount)) {
        OutResult.StackIssueCount =
            FMath::Max(0, FMath::RoundToInt(StackIssueCount));
      }
      double StackWarningCount = 0.0;
      if ((*Metadata)->TryGetNumberField(TEXT("stack_warning_count"),
                                         StackWarningCount)) {
        OutResult.StackWarningCount =
            FMath::Max(0, FMath::RoundToInt(StackWarningCount));
      }
      const TArray<TSharedPtr<FJsonValue>> *CompiledTargets = nullptr;
      if ((*Metadata)->TryGetArrayField(TEXT("compiled_targets"),
                                        CompiledTargets) &&
          CompiledTargets) {
        for (const TSharedPtr<FJsonValue> &CompiledTarget :
             *CompiledTargets) {
          FString TargetPath;
          if (CompiledTarget.IsValid() &&
              CompiledTarget->TryGetString(TargetPath) &&
              !TargetPath.IsEmpty()) {
            OutResult.CompiledTargets.AddUnique(TargetPath);
          }
        }
        OutResult.CompiledTargets.Sort();
      }
    }
  }
  OutResult.Asset = ResolvedOperation == EOperation::Remove
                        ? nullptr
                        : LoadAsset(OutResult.AssetPath);
  OutResult.ObjectPath = OutResult.Asset ? OutResult.Asset->GetPathName()
                                         : ToObjectPath(OutResult.AssetPath);
  if (OutResult.Asset)
    OutResult.AssetKind = GetAssetKind(OutResult.Asset);

  const FString PackageFilename =
      FPackageName::LongPackageNameToFilename(
          OutResult.AssetPath, FPackageName::GetAssetPackageExtension());
  OutResult.bSaved =
      ResolvedOperation == EOperation::Remove
          ? !IFileManager::Get().FileExists(*PackageFilename)
          : bSave && IFileManager::Get().FileExists(*PackageFilename);

  if (ResolvedOperation != EOperation::Remove &&
      OutResult.CompileStatus.IsEmpty() && OutResult.Asset) {
    const UeAgentNiagaraCompileValidation::FResult CompileValidation =
        UeAgentNiagaraCompileValidation::InspectAsset(OutResult.Asset);
    OutResult.CompileStatus =
        CompileValidation.bCompilationComplete &&
                !CompileValidation.bHasError
            ? TEXT("complete")
            : (CompileValidation.bCompilationComplete ? TEXT("failed")
                                                      : TEXT("incomplete"));
    OutResult.bCompilationComplete =
        CompileValidation.bCompilationComplete;
    OutResult.CompileErrorCount =
        CompileValidation.ErrorScriptCount;
    OutResult.CompileWarningCount =
        CompileValidation.WarningScriptCount;
    OutResult.CompiledTargets =
        CompileValidation.CompiledTargets;
    const UeAgentNiagaraStackValidation::FResult StackValidation =
        UeAgentNiagaraStackValidation::InspectAsset(OutResult.Asset);
    if (!StackValidation.bInspectionComplete) {
      OutResult.Status = TEXT("failed");
      OutResult.Phase = TEXT("verification");
      OutResult.ErrorCode = TEXT("niagara_stack_inspection_failed");
      OutResult.ErrorDetail = StackValidation.FirstError;
      return false;
    }
    OutResult.StackIssueCount = StackValidation.ErrorCount;
    OutResult.StackWarningCount = StackValidation.WarningCount;
  }

  if (!bSave && ResolvedOperation != EOperation::Remove) {
    OutResult.Status = TEXT("applied_in_memory");
    OutResult.Phase = TEXT("complete");
    return true;
  }
  if (!OutResult.CanReportComplete()) {
    OutResult.Status = TEXT("failed");
    OutResult.Phase = TEXT("verification");
    OutResult.ErrorCode = TEXT("niagara_completion_receipt_invalid");
    OutResult.ErrorDetail = FString::Printf(
        TEXT("compile_status=%s compilation_complete=%s "
             "compile_errors=%d stack_issues=%d compiled_targets=%d saved=%s"),
        *OutResult.CompileStatus,
        OutResult.bCompilationComplete ? TEXT("true") : TEXT("false"),
        OutResult.CompileErrorCount, OutResult.StackIssueCount,
        OutResult.CompiledTargets.Num(),
        OutResult.bSaved ? TEXT("true") : TEXT("false"));
    return false;
  }
  OutResult.Status = TEXT("complete");
  OutResult.Phase = TEXT("complete");
  return true;
}
} // namespace

FString GetApplyFailureMessage(const FAssetApplyResult &Result) {
  return UeAgentAuthoringDiagnostics::GetActionableFailureMessage(
      Result.ErrorCode, Result.ErrorDetail,
      TEXT("niagara_workspace_apply_failed"));
}

bool RegisterAssetApplyContracts(FString &OutError) {
  UeAgentApplyInputContract::ERegistrationResult InputResult;
  if (!UeAgentApplyInputContract::FRegistry::Get().RegisterContract(
          MakeInputContract(), InputResult, OutError)) {
    return false;
  }
  UeAgentApplyWorkspaceContract::ERegistrationResult WorkspaceResult;
  if (!UeAgentApplyWorkspaceContract::FRegistry::Get().RegisterContract(
          MakeWorkspaceContract(), WorkspaceResult, OutError)) {
    return false;
  }

  return true;
}

TSharedRef<IApplyProvider> MakeAssetApplyProvider() {
  static const TSharedRef<IReflectionOperationLifecycle> Lifecycle =
      MakeShared<FAssetLifecycle>();
  UeAgentAuthoringObjectApply::FOptions Options;
  Options.bAllowDomainAuthorizedOwners = true;
  return MakeShared<FReflectionApplyProvider>(ApplyProviderId, Lifecycle,
                                              Options);
}

bool CompileAsset(UObject *Asset, FString &OutError) {
  return CompileAssetObject(Asset, OutError);
}

bool SaveAsset(UObject *Asset, FString &OutError) {
  return SaveAssetObject(Asset, OutError);
}

bool ApplyAsset(const FAssetApplyRequest &Request, FAssetApplyResult &OutResult,
                const FString &RequestId) {
  return ExecuteAssetWorkspaceSource(
      UeAgentAuthoringWorkspaceLoader::FSourceRequest::MakeInlineObject(
          MakeApplyDocument(Request), TEXT("inline://niagara-asset-apply")),
      Request.AssetKind, Request.AssetPath, OutResult, RequestId);
}

bool ApplyAssetWorkspace(const FString &WorkspaceRoot,
                         const FString &ExpectedAssetKind,
                         const FString &ExpectedAssetPath,
                         FAssetApplyResult &OutResult,
                         const FString &RequestId) {
  return ExecuteAssetWorkspaceSource(
      UeAgentAuthoringWorkspaceLoader::FSourceRequest::MakeDirectory(
          WorkspaceRoot),
      ExpectedAssetKind, ExpectedAssetPath, OutResult, RequestId);
}

bool ApplyAssetInlineDocument(
    const TSharedPtr<const FJsonObject> &Document,
    const FString &ExpectedAssetKind, const FString &ExpectedAssetPath,
    FAssetApplyResult &OutResult, const FString &RequestId) {
  return ExecuteAssetWorkspaceSource(
      UeAgentAuthoringWorkspaceLoader::FSourceRequest::MakeInlineObject(
          Document, TEXT("inline://niagara-domain-authoring")),
      ExpectedAssetKind, ExpectedAssetPath, OutResult, RequestId);
}
} // namespace UeAgentNiagaraAuthoring

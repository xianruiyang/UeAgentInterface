// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/Niagara/Authoring/UeAgentNiagaraAuthoringApplyProvider.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringTypeDescriptors.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringJsonFields.h"
#include "Utilities/AuthoringCore/UeAgentNativeNormalizationIntent.h"
#include "Utilities/UeAgentPropertyValueCodec.h"
#include "Core/HjsonWorkspace/DomainAuthoring/UeAgentDomainHjsonAuthoringKit.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"

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
#include "Domains/Niagara/UeAgentNiagaraRendererBindings.h"
#include "Domains/Niagara/UeAgentNiagaraStackValidation.h"
#include "Domains/Niagara/UeAgentNiagaraSystemStructuralRefresh.h"
#include "HAL/FileManager.h"
#include "INiagaraMergeManager.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
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
#include "PackageTools.h"
#include "QueryOwners/Niagara/UeAgentNiagaraModuleCandidateQuery.h"
#include "UObject/SavePackage.h"
#include "Core/AssetDurable/UeAgentPackageSave.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectHash.h"
#include "UObject/UnrealType.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringAssetIdentity.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringIdentity.h"
#include "Utilities/AuthoringCore/UeAgentDetailsAuthoringService.h"
#include "Utilities/AuthoringObjectApply/UeAgentAuthoringObjectApply.h"
#include "Utilities/AuthoringOperationApply/UeAgentAuthoringAssetRemovalStage.h"
#include "Utilities/AuthoringOperationApply/UeAgentAuthoringObjectApplyProvider.h"
#include "Utilities/AuthoringOperationApply/UeAgentAuthoringOperationCoordinator.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentApplyWorkspaceContract.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringAssetWorkspaceApply.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringAssetWorkspaceInput.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringWorkspaceDependencyGraph.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringWorkspaceLoader.h"
#include "Utilities/UeAgentStructuredHjsonFormatter.h"

namespace UeAgentNiagaraAuthoring {
namespace {
using namespace UeAgentApplyInputContract;
using namespace UeAgentAuthoringOperationApply;

UObject *LoadAsset(const FString &Path) {
  const FString PackagePath =
      UeAgentAuthoringAssetIdentity::NormalizePackagePath(Path);
  const FString AssetName = FPackageName::GetLongPackageAssetName(PackagePath);
  if (AssetName.IsEmpty())
    return nullptr;
  const FString ObjectPath =
      UeAgentAuthoringAssetIdentity::ToObjectPath(PackagePath);
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
  // StaticLoadObject creates an empty package while resolving a missing object
  // path. Existence reads must not mutate Create's absent before state.
  return FPackageName::DoesPackageExist(PackagePath)
             ? StaticLoadObject(UObject::StaticClass(), nullptr, *ObjectPath)
             : nullptr;
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

UE::UAI::Contract::FDiagnostic AssetValueFailure(
    const TCHAR* Code, const FString& Reason, const FString& Field,
    const TSharedPtr<FJsonValue>& Actual, const TSharedPtr<FJsonValue>& Expected) {
  auto Error = UE::UAI::Contract::FDiagnostic::Failure(Code, Reason, TEXT("niagara_authoring"));
  Error.PropertyPath = Field;
  Error.ActualValue = Actual;
  Error.ActualValuePresent = Actual.IsValid();
  Error.ExpectedValue = Expected;
  Error.ExpectedValuePresent = Expected.IsValid();
  return Error;
}

UE::UAI::Contract::FDiagnostic NativeAssetFailure(
    const TCHAR* Code, const FString& Reason, const UObject* Asset,
    const FString& Field, const TSharedPtr<FJsonValue>& Actual,
    const TSharedPtr<FJsonValue>& Expected) {
  auto Error = AssetValueFailure(Code, Reason, Field, Actual, Expected);
  Error.SourceKind = TEXT("native_result");
  if (Asset) {
    Error.InternalContext = Asset->GetPathName();
    if (Asset->IsAsset()) Error.TargetPath = Asset->GetPathName();
  }
  return Error;
}

void SetFailure(FOperationFailure& OutFailure, UE::UAI::Contract::FDiagnostic Diagnostic,
    const FString& Subject = FString()) {
  OutFailure.Diagnostic = MoveTemp(Diagnostic);
  if (OutFailure.Diagnostic.SubjectId.IsEmpty()) OutFailure.Diagnostic.SubjectId = Subject;
}






void AppendPreflightError(UE::UAI::Contract::FDiagnostic Error,
    const TSharedPtr<const FJsonObject>& Object, const FString& Field,
    const FString& AssetPath, UE::UAI::Contract::FDiagnostic& Errors) {
  if (Object.IsValid())
    UeAgentAuthoringDiagnostics::LocateFailure(Error, *Object, TEXT("preflight"), AssetPath, Field);
  Errors.Append(Error);
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

bool ValidateAssetForDuplication(const UObject *Asset, UE::UAI::Contract::FDiagnostic& OutError) {
  if (!Asset) {
    OutError = NativeAssetFailure(TEXT("niagara_duplicate_source_missing"),
        TEXT("Niagara asset duplication requires an available source asset."), Asset,
        TEXT("source_asset.available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
    return false;
  }
  if (const UNiagaraSystem *System = Cast<UNiagaraSystem>(Asset)) {
    const UNiagaraScript *SpawnScript = System->GetSystemSpawnScript();
    const UNiagaraScript *UpdateScript = System->GetSystemUpdateScript();
    if (!SpawnScript || !UpdateScript || !SpawnScript->GetLatestSource() ||
        !UpdateScript->GetLatestSource()) {
      OutError = NativeAssetFailure(TEXT("niagara_system_duplicate_source_incomplete"),
          TEXT("The Niagara system cannot be duplicated because a required system script or its source is missing."),
          Asset, !SpawnScript ? TEXT("system_spawn.script_available")
              : !UpdateScript ? TEXT("system_update.script_available")
              : !SpawnScript->GetLatestSource() ? TEXT("system_spawn.source_available")
              : TEXT("system_update.source_available"),
          MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
      return false;
    }
  }
  OutError.Reset();
  return true;
}

bool DeleteAssetObject(UObject *Asset, const bool bForce, const bool bUnchecked,
                       const FString& Target, UE::UAI::Contract::FDiagnostic& OutError) {
  if (!Asset) {
    OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("niagara_delete_target_missing"),
        TEXT("No Niagara asset object was supplied for deletion."), TEXT("niagara_authoring"));
    OutError.SourceKind = TEXT("native_result"); OutError.TargetPath = Target; OutError.PropertyPath = TEXT("asset");
    OutError.ActualValuePresent = true; OutError.ActualValue = MakeShared<FJsonValueNull>();
    OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("Niagara asset object"));
    return false;
  }
  TGuardValue<bool> UnattendedGuard(GIsRunningUnattendedScript, true);
  bool bDeleted = false; int32 DeletedCount = 0;
  if (bForce) {
    TArray<UObject *> Objects = {Asset};
    DeletedCount = bUnchecked ? ObjectTools::DeleteObjectsUnchecked(Objects)
                             : ObjectTools::ForceDeleteObjects(Objects, false);
    bDeleted = DeletedCount > 0;
  } else {
    bDeleted = ObjectTools::DeleteSingleObject(Asset, true);
  }
  if (!bDeleted) {
    OutError = UE::UAI::Contract::FDiagnostic::Failure(bForce ? TEXT("force_delete_asset_failed") : TEXT("niagara_delete_rejected"),
        bForce ? TEXT("The editor did not delete the Niagara asset through the requested force-delete mode.")
               : TEXT("The editor rejected deletion of the Niagara asset."), TEXT("mutation"));
    OutError.SourceKind = TEXT("native_result"); OutError.TargetPath = Target; OutError.ActualValuePresent = true;
    OutError.ActualEvidence = MakeShared<FJsonValueString>(bForce ? (bUnchecked ? TEXT("unchecked deletion") : TEXT("forced deletion")) : TEXT("reference-checked deletion"));
    if (bForce) {
      OutError.PropertyPath = TEXT("deleted_object_count"); OutError.ActualValue = MakeShared<FJsonValueNumber>(DeletedCount);
      OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("positive deleted-object count"));
    } else {
      OutError.PropertyPath = TEXT("deletion_succeeded"); OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
      OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
      OutError.InternalContext = TEXT("DeleteSingleObject returned false with reference checking enabled; its bool API does not distinguish deletion-policy and reference-check rejection.");
    }
  }
  return bDeleted;
}

bool SaveAssetObject(UObject *Asset, UE::UAI::Contract::FDiagnostic& OutError) {
  if (!Asset || !Asset->GetOutermost()) {
    OutError = NativeAssetFailure(TEXT("niagara_save_target_missing"),
        !Asset ? TEXT("Saving requires an available Niagara asset.")
               : TEXT("The Niagara asset has no outermost package to save."),
        Asset, !Asset ? TEXT("asset.available") : TEXT("package.available"),
        MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
    return false;
  }
  UPackage *Package = Asset->GetOutermost();
  const FString PackageName = Package->GetName();
  const FString Filename = FPackageName::LongPackageNameToFilename(
      PackageName, FPackageName::GetAssetPackageExtension());
  FSavePackageArgs Args;
  Args.TopLevelFlags = RF_Public | RF_Standalone;
  UE::UAI::Contract::FDiagnostic SaveDetail;
  if (!UE::UAI::AssetDurable::TrySaveLoadedPackage(*Package, Asset, Filename, Args, SaveDetail)) {
    OutError = SaveDetail;
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
    // Scratch roots are not assets during PostDuplicate. Complete the same
    // native emitter identity synchronization once the published root is one.
    if (UNiagaraEmitter *Emitter = Cast<UNiagaraEmitter>(Published))
      Emitter->SetUniqueEmitterName(Emitter->GetFName().GetPlainNameString());
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
                        UE::UAI::Contract::FDiagnostic& OutError) {
  OutReceipt = FAssetCompileReceipt();
  UeAgentNiagaraCompileValidation::FResult Validation;
  if (UNiagaraSystem *System = Cast<UNiagaraSystem>(Asset)) {
    System->RequestCompile(true);
    System->WaitForCompilationComplete(true, false);
    System->PollForCompilationComplete(true);
    if (System->HasOutstandingCompilationRequests(true)) {
      OutReceipt.Status = TEXT("incomplete");
      OutError = NativeAssetFailure(TEXT("niagara_compile_incomplete"),
          TEXT("The Niagara system still has outstanding compilation requests after waiting for compilation."),
          Asset, TEXT("outstanding_compilation_requests"), MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false));
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
      OutError = NativeAssetFailure(TEXT("niagara_compile_incomplete"),
          TEXT("The Niagara emitter compilation context did not report completed compilation."),
          Asset, TEXT("compilation_complete"), MakeShared<FJsonValueBoolean>(CompileResult.bCompilationComplete),
          MakeShared<FJsonValueBoolean>(true));
      return false;
    }
    Validation = CompileResult.Validation;
  } else if (UNiagaraScript *Script = Cast<UNiagaraScript>(Asset)) {
    Script->RequestCompile(Script->GetExposedVersion().VersionGuid, true);
    Validation = UeAgentNiagaraCompileValidation::InspectAsset(Script);
  } else {
    OutReceipt.Status = TEXT("failed");
    OutError = NativeAssetFailure(TEXT("niagara_compile_target_invalid"),
        TEXT("Compilation requires a Niagara system, emitter, or script asset."), Asset,
        TEXT("asset.class"), Asset ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Asset->GetClass()->GetName()))
            : TSharedPtr<FJsonValue>(MakeShared<FJsonValueNull>()),
        MakeShared<FJsonValueString>(TEXT("NiagaraSystem, NiagaraEmitter, or NiagaraScript")));
    return false;
  }

  OutReceipt.bCompilationComplete = Validation.bCompilationComplete;
  OutReceipt.ErrorCount = Validation.ErrorScriptCount;
  OutReceipt.WarningCount = Validation.WarningScriptCount;
  OutReceipt.CompiledTargets = Validation.CompiledTargets;
  if (!Validation.bCompilationComplete) {
    OutReceipt.Status = TEXT("incomplete");
    OutError = NativeAssetFailure(TEXT("niagara_compile_incomplete"),
        TEXT("Niagara compile validation reports that asset compilation is incomplete."),
        Asset, TEXT("compilation_complete"), MakeShared<FJsonValueBoolean>(Validation.bCompilationComplete),
        MakeShared<FJsonValueBoolean>(true));
    return false;
  }
  if (Validation.bHasError) {
    OutReceipt.Status = TEXT("failed");
    OutError = Validation.GetFailure(Asset->GetPathName());
    return false;
  }

  const UeAgentNiagaraStackValidation::FResult StackResult =
      UeAgentNiagaraStackValidation::InspectAsset(Asset);
  OutReceipt.StackIssueCount = StackResult.ErrorCount;
  OutReceipt.StackWarningCount = StackResult.WarningCount;
  if (!StackResult.bInspectionComplete) {
    OutReceipt.Status = TEXT("stack_inspection_failed");
    OutError = StackResult.GetFailure();
    return false;
  }
  if (OutReceipt.StackIssueCount > 0) {
    OutReceipt.Status = TEXT("stack_issues");
    OutError = StackResult.GetFailure();
    return false;
  }
  OutReceipt.Status = TEXT("complete");
  OutError.Reset();
  return true;
}

bool CompileAssetObject(UObject *Asset, UE::UAI::Contract::FDiagnostic& OutError) {
  FAssetCompileReceipt Receipt;
  return CompileAssetObject(Asset, Receipt, OutError);
}

struct FLiteralInputPatch {
  FString InputName;
  TSharedPtr<const FJsonObject> Payload;
  bool bNeedsMutation = true;
};

struct FDataInterfacePatch {
  TSharedPtr<const FJsonObject> SourceObject;
  FString InputName;
  FString DataInterfaceClass;
  TSharedPtr<FJsonObject> Properties;
};

struct FDynamicInputPatch {
  TSharedPtr<const FJsonObject> SourceOwner;
  TSharedPtr<const FJsonObject> SourceObject;
  FString SourceKey;
  FString InputName;
  bool bRemove = false;
  bool bNeedsMutation = false;
  TStrongObjectPtr<UNiagaraScript> Script;
  TArray<FLiteralInputPatch> Inputs;
  TArray<FDataInterfacePatch> DataInterfaces;
  TArray<TSharedPtr<FDynamicInputPatch>> DynamicInputs;
};

void LocateDynamicInputFailure(UE::UAI::Contract::FDiagnostic& Error,
    const FDynamicInputPatch& Patch, const FString& Field = FString()) {
  if (!Error.IsValid()) return;
  if (Patch.SourceObject.IsValid())
    UeAgentAuthoringDiagnostics::LocateFailure(Error, *Patch.SourceObject, TEXT("niagara_authoring"), FString(), Field);
  else if (Patch.SourceOwner.IsValid())
    UeAgentAuthoringDiagnostics::LocateFailure(Error, *Patch.SourceOwner, TEXT("niagara_authoring"), FString(), Patch.SourceKey);
}

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
  TSharedPtr<FJsonObject> SourceObject;
  FString NativeCandidateName;
  TMap<FString, FString> CandidateSelect;
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
                            FModulePatch &OutPatch, UE::UAI::Contract::FDiagnostic& OutError) {
  OutPatch.bAssignmentTargetsExplicit =
      Object.IsValid() && Object->HasField(TEXT("assignment_targets"));
  if (!OutPatch.bAssignmentTargetsExplicit)
    return true;
  const FJsonObject* ErrorObject = Object.Get();
  FString ErrorField = TEXT("assignment_targets");
  ON_SCOPE_EXIT {
    if (OutError.IsValid() && ErrorObject)
      UeAgentAuthoringDiagnostics::LocateFailure(OutError, *ErrorObject, TEXT("preflight"), FString(), ErrorField);
  };
  const TSharedPtr<FJsonObject> *Targets = nullptr;
  if (!Object->TryGetObjectField(TEXT("assignment_targets"), Targets) ||
      !Targets || !Targets->IsValid()) {
    OutError = AssetValueFailure(TEXT("niagara_workspace_assignment_targets_invalid"),
        TEXT("assignment_targets must be an object keyed by parameter name."),
        TEXT("assignment_targets"), Object->TryGetField(TEXT("assignment_targets")), MakeShared<FJsonValueString>(TEXT("object")));
    return false;
  }
  TArray<FString> TargetNames;
  (*Targets)->Values.GetKeys(TargetNames);
  TargetNames.Sort();
  for (const FString &TargetName : TargetNames) {
    const TSharedPtr<FJsonValue> TargetValue =
        (*Targets)->Values.FindRef(TargetName);
    ErrorObject = Targets->Get();
    ErrorField = TargetName;
    FString TypeText;
    if (TargetValue.IsValid() && TargetValue->Type == EJson::String) {
      TypeText = TargetValue->AsString();
    } else if (TargetValue.IsValid() && TargetValue->Type == EJson::Object) {
      TargetValue->AsObject()->TryGetStringField(TEXT("type"), TypeText);
    } else if (!TargetValue.IsValid() || TargetValue->Type != EJson::Null) {
      OutError = AssetValueFailure(TEXT("niagara_workspace_assignment_target_invalid"),
          TEXT("An assignment target must be a type name, an object with type, or null for native type inference."),
          TargetName, TargetValue, MakeShared<FJsonValueString>(TEXT("string, type object, or null")));
      OutError.CandidateName = TargetName;
      return false;
    }
    FNiagaraVariable Variable;
    if (!UeAgentNiagaraStageModuleAuthoring::ResolveAssignmentTargetVariable(
            TargetName, TypeText, Variable, OutError))
      return false;
    OutPatch.AssignmentTargets.Add(MoveTemp(Variable));
  }
  if (OutPatch.AssignmentTargets.IsEmpty()) {
    OutError = AssetValueFailure(TEXT("niagara_workspace_assignment_targets_missing"),
        TEXT("An explicitly supplied assignment_targets object must contain at least one parameter."),
        TEXT("assignment_targets.count"), MakeShared<FJsonValueNumber>(OutPatch.AssignmentTargets.Num()),
        MakeShared<FJsonValueString>(TEXT("at least one target")));
    return false;
  }
  if (UeAgentNativeNormalizationIntent::IsValueRecording()) {
    // Freeze the resolved native arguments, not the later object readback.
    // The same resolver owns inferred, explicit and object-form type requests.
    const TSharedRef<FJsonObject> ResolvedTargets = MakeShared<FJsonObject>();
    for (const FNiagaraVariable &Variable : OutPatch.AssignmentTargets) {
      ResolvedTargets->SetStringField(Variable.GetName().ToString(),
          UeAgentNiagaraStageModuleAuthoring::GetNiagaraTypeToken(Variable.GetType()));
    }
    UeAgentAuthoringTypeDescriptors::FDescriptor ArgumentType;
    ArgumentType.Kind = TEXT("object");
    return UeAgentNativeNormalizationIntent::RecordValue(
        TEXT("/assignment_targets"), MakeShared<FJsonValueObject>(*Targets),
        MakeShared<FJsonValueObject>(ResolvedTargets), OutError, &ArgumentType, nullptr,
        UeAgentNativeNormalizationIntent::EValueCompletion::ResolvedArgument);
  }
  return true;
}

struct FStagePatch {
  FString SourceKey;
  TSharedPtr<const FJsonObject> SourceObject;
  UeAgentNiagaraStageModuleAuthoring::FStageTarget Target;
  UeAgentNiagaraStageModuleAuthoring::FResolvedStage Stage;
  TArray<FModulePatch> Modules;
  bool bReplaceStage = false;
  bool bNeedsMutation = false;
};

struct FRendererPatch {
  TSharedPtr<const FJsonObject> SourceOwner;
  TSharedPtr<const FJsonObject> SourceObject;
  FString SourceKey;
  FString RendererName;
  FString RendererClass;
  int32 TargetIndex = INDEX_NONE;
  bool bRemove = false;
  TSharedPtr<FJsonObject> Properties;
};

struct FEventHandlerPatch {
  TSharedPtr<const FJsonObject> SourceOwner;
  TSharedPtr<const FJsonObject> SourceObject;
  FString SourceKey;
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
  TSharedPtr<const FJsonObject> SourceObject;
  UeAgentNiagaraStageModuleAuthoring::FEmitterOwnerTarget Target;
  UeAgentNiagaraStageModuleAuthoring::FResolvedEmitterOwner Owner;
  TWeakObjectPtr<UNiagaraSystem> TargetSystem;
  TStrongObjectPtr<UNiagaraEmitter> SourceEmitter;
  FGuid SourceEmitterVersion;
  FGuid AddedHandleId;
  bool bAddEmitterHandle = false;
  bool bAddedEmitterHandle = false;
  bool bRemoveEmitterHandle = false;
  TOptional<FNiagaraEmitterHandle> RemovedHandle;
  int32 RemovedHandleIndex = INDEX_NONE;
  TStrongObjectPtr<UNiagaraEmitter> RemovedEmitterInstance;
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
  bool bCompactValues = false;
  bool bReplaceRenderers = false;
  TArray<FRendererPatch> Renderers;
  TArray<FEventHandlerPatch> EventHandlers;
  bool bNeedsMutation = false;
};

// Only native owner selection is Niagara-specific. Values and comparison stay
// in the common property intent, without retaining emitter data or field pointers.
struct FRendererBindingIntent {
  FName PropertyName;
  UeAgentAuthoringObjectApply::FPropertyIntent Intent;
};

void SupplementEmitterPatchFailure(UE::UAI::Contract::FDiagnostic& Error,
    const FEmitterStructurePatch& Patch, const FJsonObject* SourceObject = nullptr,
    const FString& Field = FString()) {
  if (Error.IsEmpty()) return;
  if (Error.TargetPath.IsEmpty()) Error.TargetPath = Patch.Target.AssetPath;
  if (Error.CandidateName.IsEmpty()) Error.CandidateName = Patch.Target.EmitterName;
  if (SourceObject)
    UeAgentAuthoringDiagnostics::LocateFailure(Error, *SourceObject,
        TEXT("niagara_authoring"), Patch.Target.AssetPath, Field);
}

struct FEmitterPropertyIntent {
  UeAgentNiagaraStageModuleAuthoring::FEmitterOwnerTarget Target;
  FString RendererName;
  TArray<UeAgentAuthoringObjectApply::FPropertyIntent> Properties;
  TArray<FRendererBindingIntent> Bindings;
};

bool VerifyEmitterPropertyIntents(UObject* Root,
    const TArray<FEmitterPropertyIntent>& Intents, UE::UAI::Contract::FDiagnostic& OutError) {
  using namespace UeAgentNiagaraStageModuleAuthoring;
  for (const auto& Entry : Intents) {
    FResolvedEmitterOwner Owner;
    if (!Root) {
      OutError = NativeAssetFailure(TEXT("niagara_property_intent_owner_missing"),
          TEXT("Property verification requires an available Niagara asset root."), Root,
          TEXT("asset.available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
      OutError.TargetPath = Entry.Target.AssetPath;
      return false;
    }
    if (!ResolveEmitterOwner(Entry.Target, Owner, OutError, Root)) return false;
    UNiagaraRendererProperties* Renderer = nullptr;
    if (!Entry.RendererName.IsEmpty()) {
      for (auto* Candidate : Owner.EmitterData->GetRenderers()) {
        if (Candidate && Candidate->GetFName() == FName(*Entry.RendererName)) {
          Renderer = Candidate;
          break;
        }
      }
      if (!Renderer) {
        OutError = NativeAssetFailure(TEXT("niagara_property_intent_renderer_missing"),
            TEXT("The renderer required for property verification is absent from the emitter."), Root,
            TEXT("renderer.present"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
        OutError.CandidateName = Entry.RendererName;
        return false;
      }
    }
    for (const auto& Intent : Entry.Properties) {
      UeAgentAuthoringObjectApply::FApplyResult Result;
      const bool bVerified = Renderer ? Intent.VerifyObject(Renderer, Result)
          : Intent.VerifyStruct(FVersionedNiagaraEmitterData::StaticStruct(),
              Owner.EmitterData, Owner.Emitter, Result);
      if (!bVerified) {
        OutError = Result.Diagnostic;
        return false;
      }
    }
    for (const auto& Binding : Entry.Bindings) {
      auto* Property = Renderer ? FindFProperty<FStructProperty>(Renderer->GetClass(), Binding.PropertyName) : nullptr;
      UeAgentAuthoringObjectApply::FApplyResult Result;
      if (!Property || Property->Struct != FNiagaraVariableAttributeBinding::StaticStruct()) {
        OutError = NativeAssetFailure(TEXT("niagara_binding_intent_target_missing"),
            Property ? TEXT("The renderer binding property has an unexpected structure type.")
                : TEXT("The renderer binding property is unavailable for verification."), Root,
            Binding.PropertyName.ToString(),
            Property && Property->Struct ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Property->Struct->GetName()))
                : TSharedPtr<FJsonValue>(MakeShared<FJsonValueNull>()),
            MakeShared<FJsonValueString>(FNiagaraVariableAttributeBinding::StaticStruct()->GetName()));
        OutError.CandidateName = Entry.RendererName;
        return false;
      }
      if (!Binding.Intent.VerifyStruct(Property->Struct, Property->ContainerPtrToValuePtr<void>(Renderer), Renderer, Result)) {
        Result.Diagnostic.PropertyPath = Binding.PropertyName.ToString() + TEXT(".") + Result.Diagnostic.PropertyPath;
        OutError = Result.Diagnostic;
        return false;
      }
    }
  }
  return true;
}

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
  UeAgentNiagaraStageModuleAuthoring::FEmitterOwnerTarget Target;
  TWeakObjectPtr<UNiagaraEmitter> Emitter;
  FGuid EmitterVersion;
  TSharedPtr<FJsonObject> Properties;
  TArray<FRendererBackup> Renderers;
  TArray<FNiagaraEventScriptProperties> EventHandlers;
};

bool ValidateEmitterStructurePropertyPatches(
    const FEmitterStructurePatch &Patch, UE::UAI::Contract::FDiagnostic& OutError);

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
                        UE::UAI::Contract::FDiagnostic& OutError) {
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
    UE::UAI::Contract::FDiagnostic& OutError) {
  using namespace UeAgentNiagaraStageModuleAuthoring;
  OutTarget = FStageTarget();
  OutTarget.AssetPath = AssetPath;
  if (!Object.IsValid()) {
    OutError = AssetValueFailure(TEXT("niagara_workspace_stage_target_missing"),
        TEXT("A Niagara stage target must be an object."), TEXT("target"),
        MakeShared<FJsonValueNull>(), MakeShared<FJsonValueString>(TEXT("object")));
    return false;
  }
  FString SourceField;
  ON_SCOPE_EXIT {
    if (OutError.IsValid())
      UeAgentAuthoringDiagnostics::LocateFailure(OutError, *Object, TEXT("preflight"), AssetPath, SourceField);
  };
  if (!UeAgentAuthoringJsonFields::ValidateOptionalFields(*Object, {
      {TEXT("emitter_name"), EJson::String},
      {TEXT("emitter_handle_id"), EJson::String}}, OutError)) return false;
  Object->TryGetStringField(TEXT("emitter_name"), OutTarget.EmitterName);
  FString EmitterHandleId;
  if (Object->TryGetStringField(TEXT("emitter_handle_id"), EmitterHandleId) &&
      !EmitterHandleId.IsEmpty()) {
    if (!FGuid::Parse(EmitterHandleId, OutTarget.EmitterHandleId)) {
      SourceField = TEXT("emitter_handle_id");
      OutError = AssetValueFailure(TEXT("niagara_workspace_emitter_handle_id_invalid"),
          TEXT("emitter_handle_id is not a valid GUID."), SourceField,
          MakeShared<FJsonValueString>(EmitterHandleId), MakeShared<FJsonValueString>(TEXT("a GUID")));
      return false;
    }
    OutTarget.bEmitterHandleIdExplicit = true;
  }
  FString Usage;
  if (!Object->TryGetStringField(TEXT("usage"), Usage) ||
      !TryParseScriptUsage(Usage, OutTarget.Usage)) {
    SourceField = TEXT("usage");
    OutError = AssetValueFailure(TEXT("niagara_workspace_stage_usage_invalid"),
        TEXT("The stage usage must name an ENiagaraScriptUsage value."),
        SourceField, Object->TryGetField(SourceField), MakeShared<FJsonValueString>(TEXT("an ENiagaraScriptUsage name")));
    return false;
  }
  OutTarget.bUsageExplicit = true;
  FString UsageId;
  if (Object->TryGetStringField(TEXT("usage_id"), UsageId) &&
      !UsageId.IsEmpty()) {
    if (!FGuid::Parse(UsageId, OutTarget.UsageId)) {
      SourceField = TEXT("usage_id");
      OutError = AssetValueFailure(TEXT("niagara_workspace_stage_usage_id_invalid"),
          TEXT("usage_id is not a valid GUID."), SourceField,
          MakeShared<FJsonValueString>(UsageId), MakeShared<FJsonValueString>(TEXT("a GUID")));
      return false;
    }
    OutTarget.bUsageIdExplicit = true;
  }
  return true;
}

bool ParseLiteralInputPatches(const TSharedPtr<const FJsonObject> &Owner,
                              TArray<FLiteralInputPatch> &OutPatches,
                              UE::UAI::Contract::FDiagnostic& OutError) {
  OutPatches.Reset();
  const TSharedPtr<FJsonObject> *Inputs = nullptr;
  if (!Owner.IsValid() || !Owner->HasField(TEXT("inputs"))) return true;
  if (!Owner->TryGetObjectField(TEXT("inputs"), Inputs) || !Inputs || !Inputs->IsValid()) {
    OutError = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("niagara_workspace_inputs_invalid"),
        TEXT("/inputs"), MakeShared<FJsonValueString>(TEXT("object")), Owner->TryGetField(TEXT("inputs")));
    OutError.PublicMessage = TEXT("inputs must be an object keyed by Niagara input name.");
    UeAgentAuthoringDiagnostics::LocateFailure(OutError, *Owner, TEXT("preflight"), FString(), TEXT("inputs"));
    return false;
  }
  UeAgentAuthoringDiagnostics::FDiagnostic Errors;
  for (const TPair<FString, TSharedPtr<FJsonValue>> &Pair : (*Inputs)->Values) {
    if (Pair.Key.IsEmpty() || !Pair.Value.IsValid() ||
        Pair.Value->Type != EJson::Object) {
      auto Error = UeAgentAuthoringDiagnostics::MakeValueFailure(TEXT("niagara_workspace_literal_input_invalid"),
          TEXT("/inputs/") + Pair.Key.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1")),
          MakeShared<FJsonValueString>(TEXT("named literal object")), Pair.Value);
      Error.PublicMessage = Pair.Key.IsEmpty() ? TEXT("A literal input entry requires a nonempty input name.")
          : TEXT("A literal input entry must be an object.");
      Error.CandidateName = Pair.Key;
      UeAgentAuthoringDiagnostics::LocateFailure(Error, *Owner, TEXT("preflight"), FString(), TEXT("inputs"));
      Errors.Append(Error);
      continue;
    }
    FLiteralInputPatch &Patch = OutPatches.AddDefaulted_GetRef();
    Patch.InputName = Pair.Key;
    Patch.Payload = Pair.Value->AsObject();
  }
  if (!Errors.Code.IsEmpty()) { OutError = Errors; return false; }
  return true;
}

bool ResolveLiteralInputMutationFlags(
    UNiagaraNodeFunctionCall *ExistingModule,
    UNiagaraNode *ExistingStandaloneAssignment,
    TArray<FLiteralInputPatch> &Patches, UE::UAI::Contract::FDiagnostic& OutError) {
  using namespace UeAgentNiagaraStageModuleAuthoring;
  using namespace UeAgentNiagaraStandaloneModuleAuthoring;
  UeAgentAuthoringDiagnostics::FDiagnostic Errors;
  for (FLiteralInputPatch &Patch : Patches) {
    const FString AuthoredName = Patch.InputName;
    auto Check = [&]() -> bool {
    Patch.bNeedsMutation = true;
    bool bMatches = false;
    if (ExistingStandaloneAssignment) {
      FNiagaraVariable TargetVariable;
      UEdGraphPin *TargetPin = FindAssignmentTargetPin(
          *ExistingStandaloneAssignment, Patch.InputName, TargetVariable);
      if (!TargetPin) return true;
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
    return true;
    };
    OutError.Reset();
    if (!Check()) {
      UeAgentAuthoringDiagnostics::PrefixDocumentFailure(OutError,
          TEXT("/inputs/") + AuthoredName.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1")));
      Errors.Append(OutError);
    }
  }
  if (!Errors.Code.IsEmpty()) { OutError = Errors; return false; }
  return true;
}

bool ParseAndResolveLiteralInputs(const TSharedPtr<const FJsonObject>& Owner,
    UNiagaraNodeFunctionCall* Module, UNiagaraNode* Assignment,
    TArray<FLiteralInputPatch>& Patches, UE::UAI::Contract::FDiagnostic& Error) {
  UeAgentAuthoringDiagnostics::FDiagnostic Errors;
  if (!ParseLiteralInputPatches(Owner, Patches, Error))
    Errors.Append(Error);
  Error.Reset();
  if (!ResolveLiteralInputMutationFlags(Module, Assignment, Patches, Error))
    Errors.Append(Error);
  if (!Errors.Code.IsEmpty()) { Error = Errors; return false; }
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
    const TSharedPtr<const FJsonObject>& SourceOwner,
    FUeAgentStructuredResourceTracker &ResourceTracker,
    const FString &RootPointer, FDynamicInputPatch &OutPatch,
    UE::UAI::Contract::FDiagnostic& OutError) {
  TSharedPtr<const FJsonObject> SourceObject;
  FString SourceField;
  const bool bParsed = [&]() {
  using namespace UeAgentNiagaraStageModuleAuthoring;
  struct FParseFrame {
    UNiagaraNodeFunctionCall *ExistingModule = nullptr;
    UEdGraphPin *ExistingInputPin = nullptr;
    FString InputKey;
    TSharedPtr<FJsonValue> Value;
    FDynamicInputPatch *Patch = nullptr;
    UNiagaraNodeFunctionCall *ExistingDynamic = nullptr;
    TSharedPtr<const FJsonObject> Object;
    TSharedPtr<const FJsonObject> SourceOwner;
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
  Root.SourceOwner = SourceOwner;
  Root.Patch = &OutPatch;
  Root.Pointer = RootPointer;
  Stack.Add(MoveTemp(Root));
  TSet<const FJsonObject *> ActiveObjects;

  // Resource accounting failures use the shared resource, limit, consumed and pointer
  // diagnostic contract and occur before graph mutation.
  while (!Stack.IsEmpty()) {
    FParseFrame Frame = Stack.Pop(EAllowShrinking::No);
    const bool bObjectValue = Frame.Value.IsValid() && Frame.Value->Type == EJson::Object;
    SourceObject = bObjectValue ? Frame.Value->AsObject() : Frame.SourceOwner;
    SourceField = bObjectValue ? FString() : Frame.InputKey;
    if (!Frame.Patch) {
      OutError = NativeAssetFailure(TEXT("niagara_workspace_dynamic_input_frame_invalid"),
          TEXT("The dynamic-input parse frame has no destination patch."), nullptr,
          TEXT("parse_frame.patch_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
      return false;
    }
    if (Frame.bFinalize) {
    SourceField = TEXT("inputs");
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
            OutError = AssetValueFailure(TEXT("niagara_workspace_input_source_conflict"),
                TEXT("The same Niagara input cannot be supplied by both a literal and a dynamic input."),
                TEXT("input_sources"), MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{
                    MakeShared<FJsonValueString>(TEXT("inputs")), MakeShared<FJsonValueString>(TEXT("dynamic_inputs"))}),
                MakeShared<FJsonValueString>(TEXT("exactly one input source")));
            OutError.CandidateName = LiteralPatch.InputName;
            if (LiteralPatch.Payload.IsValid()) SourceObject = LiteralPatch.Payload;
            SourceField.Reset();
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
    Patch.SourceOwner = Frame.SourceOwner;
    Patch.SourceKey = Frame.InputKey;
    Patch.InputName = Frame.InputKey;
    if (!Frame.Value.IsValid() || Frame.Value->Type == EJson::Null) {
      Patch.bRemove = true;
    } else if (Frame.Value->Type == EJson::Object) {
      Frame.Object = Frame.Value->AsObject();
      Patch.SourceObject = Frame.Object;
      if (!Frame.Object.IsValid()) {
        SourceObject = Frame.SourceOwner;
        SourceField = Frame.InputKey;
        OutError = AssetValueFailure(TEXT("niagara_workspace_dynamic_input_invalid"),
            TEXT("The dynamic-input value does not contain a valid object."),
            TEXT("dynamic_input"), MakeShared<FJsonValueNull>(), MakeShared<FJsonValueString>(TEXT("object or null")));
        return false;
      }
      if (ActiveObjects.Contains(Frame.Object.Get())) {
        OutError = AssetValueFailure(TEXT("niagara_workspace_dynamic_input_cycle_detected"),
            TEXT("A dynamic-input object refers back to an object already active in the input tree."),
            TEXT("dynamic_input.cycle_detected"), MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false));
        OutError.InternalContext = Frame.Pointer;
        return false;
      }
      ActiveObjects.Add(Frame.Object.Get());
      Frame.Object->TryGetStringField(TEXT("input_name"), Patch.InputName);
      if (Patch.InputName.IsEmpty())
        Patch.InputName = Frame.InputKey;
      Frame.Object->TryGetBoolField(TEXT("remove"), Patch.bRemove);
    } else {
      OutError = AssetValueFailure(TEXT("niagara_workspace_dynamic_input_invalid"),
          TEXT("A dynamic-input entry must be an object, or null to remove the input."),
          TEXT("dynamic_input"), Frame.Value, MakeShared<FJsonValueString>(TEXT("object or null")));
      return false;
    }
    SourceField = Patch.SourceObject.IsValid() ? TEXT("input_name") : Patch.SourceKey;
    if (Patch.InputName.IsEmpty()) {
      OutError = AssetValueFailure(TEXT("niagara_workspace_dynamic_input_name_missing"),
          TEXT("A dynamic input requires a nonempty entry key or input_name."),
          TEXT("input_name"), MakeShared<FJsonValueString>(Patch.InputName), MakeShared<FJsonValueString>(TEXT("a nonempty input name")));
      return false;
    }

    FString ResolvedInputName;
    if (Frame.ExistingInputPin) {
      Frame.ExistingDynamic =
          FindDynamicInputForPin(*Frame.ExistingInputPin);
      ResolvedInputName = Frame.ExistingInputPin->PinName.ToString();
    } else if (Frame.ExistingModule) {
      Frame.ExistingDynamic = FindDynamicInputForModuleInput(*Frame.ExistingModule, Patch.InputName, ResolvedInputName, OutError);
        if (OutError.IsValid()) return false;
      if (!ResolvedInputName.IsEmpty())
        Patch.InputName = ResolvedInputName;
    }

    if (!Patch.bRemove && Frame.Object.IsValid()) {
    SourceField = TEXT("script_asset_path");
      FString ScriptPath;
      if (ReadStringField(
              Frame.Object, TEXT("script_asset_path"), ScriptPath)) {
        if (!ResolvePatchScript(
                ScriptPath, EScriptKind::DynamicInput, Patch.Script,
                OutError))
          return false;
      } else if (!Frame.ExistingDynamic) {
        OutError = AssetValueFailure(TEXT("niagara_workspace_dynamic_input_script_missing"),
            TEXT("A new dynamic input requires script_asset_path because there is no existing dynamic-input node."),
            TEXT("script_asset_path"), Frame.Object->TryGetField(TEXT("script_asset_path")),
            MakeShared<FJsonValueString>(TEXT("an existing Niagara DynamicInput script asset")));
        OutError.CandidateName = Patch.InputName;
        return false;
      }
    }

    // A replacement script owns a different input interface. Keep the old node
    // for replacement detection, but do not compare its inputs with the new tree.
    UNiagaraNodeFunctionCall *InputBaselineDynamic = Frame.ExistingDynamic;
    if (Patch.Script.IsValid() &&
        (!Frame.ExistingDynamic ||
         !GetModuleScriptPath(*Frame.ExistingDynamic).Equals(
             Patch.Script->GetPathName(), ESearchCase::IgnoreCase))) {
      InputBaselineDynamic = nullptr;
    }
    TArray<FParseFrame> ChildFrames;
    SourceField = TEXT("inputs");
    if (Frame.Object.IsValid() &&
        !ParseAndResolveLiteralInputs(Frame.Object, InputBaselineDynamic, nullptr,
                                     Patch.Inputs, OutError))
      return false;
    if (!Patch.bRemove && Frame.Object.IsValid()) {

    SourceField = TEXT("data_interfaces");
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
            SourceObject = *DataInterfaces;
            SourceField = Pair.Key;
            OutError = AssetValueFailure(TEXT("niagara_workspace_data_interface_invalid"),
                Pair.Key.IsEmpty() ? TEXT("A data-interface entry requires a nonempty input name.")
                    : TEXT("A data-interface entry must be an object."),
                TEXT("data_interface"), Pair.Value, MakeShared<FJsonValueString>(TEXT("a named data-interface object")));
            OutError.CandidateName = Pair.Key;
            return false;
          }
          const TSharedPtr<FJsonObject> DataInterfaceObject =
              Pair.Value->AsObject();
          FDataInterfacePatch &DataInterface =
              Patch.DataInterfaces.AddDefaulted_GetRef();
          DataInterface.SourceObject = DataInterfaceObject;
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
            SourceObject = DataInterfaceObject;
            SourceField = DataInterface.InputName.IsEmpty() ? TEXT("input_name") : TEXT("properties");
            OutError = AssetValueFailure(TEXT("niagara_workspace_data_interface_properties_missing"),
                DataInterface.InputName.IsEmpty() ? TEXT("The data-interface input_name must not be empty.")
                    : TEXT("A data-interface patch requires a properties object."),
                SourceField, DataInterfaceObject->TryGetField(SourceField),
                MakeShared<FJsonValueString>(DataInterface.InputName.IsEmpty() ? TEXT("a nonempty input name") : TEXT("object")));
            OutError.CandidateName = Pair.Key;
            return false;
          }
        }
      }

    SourceField = TEXT("dynamic_inputs");
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
          ChildFrame.ExistingModule = InputBaselineDynamic;
          ChildFrame.InputKey = Pair.Key;
          ChildFrame.Value = Pair.Value;
          ChildFrame.SourceOwner = *DynamicInputs;
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
  }();
  if (!bParsed && SourceObject.IsValid())
    UeAgentAuthoringDiagnostics::LocateFailure(OutError, *SourceObject, TEXT("preflight"), FString(), SourceField);
  return bParsed;
}

bool ParseModulePatch(UeAgentNiagaraStageModuleAuthoring::FResolvedStage &Stage,
                      const FString &StableAssetPath, const FString &ModuleKey,
                      const TSharedPtr<FJsonObject> &Object,
                      const bool bForceScriptResolution,
                      FUeAgentStructuredResourceTracker &ResourceTracker,
                      FModulePatch &OutPatch, UE::UAI::Contract::FDiagnostic& OutError) {
  using namespace UeAgentNiagaraStageModuleAuthoring;
  OutPatch = FModulePatch();
  OutPatch.SourceKey = ModuleKey;
  OutPatch.SourceObject = Object;
  FString ModuleGuidText;
  if (!Object.IsValid()) {
    OutError = AssetValueFailure(TEXT("niagara_workspace_module_invalid"),
        TEXT("A module patch must be an object."), TEXT("module"),
        MakeShared<FJsonValueNull>(), MakeShared<FJsonValueString>(TEXT("object")));
    return false;
  }
  FString SourceField;
  const bool bParsed = [&]() {
  if (!UeAgentAuthoringJsonFields::ValidateOptionalFields(*Object, {
      {TEXT("module_guid"), EJson::String}, {TEXT("remove"), EJson::Boolean},
      {TEXT("script_asset_path"), EJson::String}, {TEXT("module_kind"), EJson::String}}, OutError)) return false;
  if (Object->HasField(TEXT("candidate"))) {
    SourceField = TEXT("candidate");
    const TSharedPtr<FJsonObject> *Candidate = nullptr;
    if (!Object->TryGetObjectField(TEXT("candidate"), Candidate) ||
        !Candidate || !Candidate->IsValid() ||
        !(*Candidate)->TryGetStringField(TEXT("name"),
                                         OutPatch.NativeCandidateName) ||
        OutPatch.NativeCandidateName.TrimStartAndEnd().IsEmpty()) {
      const bool bCandidateObject = Candidate && Candidate->IsValid();
      OutError = AssetValueFailure(TEXT("niagara_workspace_native_candidate_name_invalid"),
          bCandidateObject ? TEXT("The native candidate requires a nonempty string name.")
              : TEXT("candidate must be an object with a nonempty name."),
          bCandidateObject ? TEXT("/candidate/name") : TEXT("/candidate"),
          bCandidateObject ? (*Candidate)->TryGetField(TEXT("name")) : Object->TryGetField(TEXT("candidate")),
          MakeShared<FJsonValueString>(bCandidateObject ? TEXT("a nonempty candidate name") : TEXT("object")));
      OutError.Source.JsonPath = OutError.PropertyPath;
      return false;
    }
    OutPatch.NativeCandidateName.TrimStartAndEndInline();
    UE::UAI::HjsonWorkspace::DomainAuthoring::FDomainHjsonCandidateSelector Selector;
    UE::UAI::HjsonWorkspace::FWorkspaceError SelectorError;
    if (!UE::UAI::HjsonWorkspace::DomainAuthoring::TryReadCandidateSelector(
            *Candidate, FString(), Selector, SelectorError)) {
      OutError = SelectorError.GetDiagnostic();
      UeAgentAuthoringDiagnostics::LocateFailure(OutError, **Candidate, TEXT("preflight"), StableAssetPath);
      return false;
    }
    OutPatch.CandidateSelect = MoveTemp(Selector.Select);
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
  SourceField = TEXT("module_index");
  if (!UeAgentAuthoringJsonFields::ReadInt32Field(*Object, TEXT("module_index"), OutPatch.TargetIndex, OutError)) return false;
  if (Object->HasField(TEXT("module_index"))) {
    OutPatch.TargetIndex = FMath::Max(0, OutPatch.TargetIndex);
  }

  const bool bStandaloneModuleStage =
      UeAgentNiagaraStandaloneModuleAuthoring::IsStandaloneModuleStage(Stage);
  UNiagaraNodeFunctionCall *ExistingModule = nullptr;
  UNiagaraNode *ExistingStandaloneAssignment = nullptr;
  int32 ExistingIndex = INDEX_NONE;
  SourceField = TEXT("module_guid");
  if (OutPatch.ModuleGuid.IsValid()) {
    UE::UAI::Contract::FDiagnostic ResolveError;
    if (bStandaloneModuleStage) {
      OutPatch.bExists =
          UeAgentNiagaraStandaloneModuleAuthoring::ResolveAssignmentNode(
              *Stage.OutputNode, OutPatch.ModuleGuid,
              ExistingStandaloneAssignment, ExistingIndex, ResolveError);
      if (!OutPatch.bExists) {
        if (ResolveError.Code != TEXT("module_selector_not_found")) {
          OutError = ResolveError;
          return false;
        }
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
    if (!OutPatch.bExists && ResolveError.Code != TEXT("module_selector_not_found")) {
      OutError = ResolveError;
      return false;
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
  SourceField = TEXT("module_kind");
  FString ModuleKind;
  Object->TryGetStringField(TEXT("module_kind"), ModuleKind);
  ModuleKind.TrimStartAndEndInline();
  ModuleKind.ToLowerInline();
  if (!ModuleKind.IsEmpty() && ModuleKind != TEXT("assignment") &&
      ModuleKind != TEXT("script_module")) {
    OutError = AssetValueFailure(TEXT("niagara_workspace_module_kind_invalid"),
        TEXT("module_kind must be assignment or script_module when supplied."),
        SourceField, MakeShared<FJsonValueString>(ModuleKind), MakeShared<FJsonValueString>(TEXT("assignment or script_module")));
    return false;
  }
  if (bStandaloneModuleStage && ModuleKind == TEXT("script_module")) {
    OutError = AssetValueFailure(TEXT("niagara_standalone_module_script_module_unsupported"),
        TEXT("A standalone Module script stage supports assignment patches, not script_module patches."),
        SourceField, MakeShared<FJsonValueString>(ModuleKind), MakeShared<FJsonValueString>(TEXT("assignment")));
    return false;
  }
  SourceField = TEXT("assignment_targets");
  {
    UeAgentAuthoringIdentity::FCanonicalIdentity ModuleIdentity;
    if (UeAgentNativeNormalizationIntent::IsActive() && Object->HasField(TEXT("assignment_targets"))) {
      auto Selectors = BuildStageIdentitySelectors(Stage, StableAssetPath);
      Selectors.Add(TEXT("module_guid"), OutPatch.ModuleGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
      if (!UeAgentAuthoringIdentity::BuildCanonicalIdentity(TEXT("niagara_module_identity"),
          TEXT("niagara_module"), Selectors, ModuleIdentity, OutError)) return false;
    }
    UeAgentNativeNormalizationIntent::FValueScope Target(ModuleIdentity, ModuleSemanticId);
    if (!ParseAssignmentTargets(Object, OutPatch, OutError)) return false;
  }
  if (bStandaloneModuleStage && !OutPatch.bExists &&
      OutPatch.bAssignmentTargetsExplicit) {
    UNiagaraNodeAssignment *LegacyAssignment = nullptr;
    int32 LegacyIndex = INDEX_NONE;
    UE::UAI::Contract::FDiagnostic LegacyResolveError;
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
    } else if (LegacyResolveError.Code != TEXT("module_selector_not_found")) {
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
  SourceField = TEXT("script_asset_path");
  if (OutPatch.bAssignmentModule) {
    if (!DesiredScriptPath.IsEmpty()) {
      OutError = AssetValueFailure(TEXT("niagara_workspace_assignment_script_conflict"),
          TEXT("An assignment module cannot also specify script_asset_path."),
          SourceField, MakeShared<FJsonValueString>(DesiredScriptPath), MakeShared<FJsonValueString>(TEXT("no script asset path for an assignment")));
      return false;
    }
    if ((!OutPatch.bExists ||
         (!ExistingAssignment && !ExistingStandaloneAssignment)) &&
        !OutPatch.bAssignmentTargetsExplicit) {
      SourceField = TEXT("assignment_targets");
      OutError = AssetValueFailure(TEXT("niagara_workspace_assignment_targets_missing"),
          TEXT("Creating or converting to an assignment module requires explicit assignment_targets."),
          SourceField, Object->TryGetField(SourceField), MakeShared<FJsonValueString>(TEXT("a nonempty assignment_targets object")));
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
    OutError = AssetValueFailure(TEXT("niagara_workspace_module_script_missing"),
        TEXT("Converting an assignment into a script module requires script_asset_path."),
        SourceField, Object->TryGetField(SourceField), MakeShared<FJsonValueString>(TEXT("a Niagara Module script asset path")));
    return false;
  } else if (bForceScriptResolution || !OutPatch.bExists ||
             (!DesiredScriptPath.IsEmpty() &&
              !CurrentScriptPath.Equals(DesiredScriptPath,
                                        ESearchCase::IgnoreCase))) {
    if (DesiredScriptPath.IsEmpty()) {
      OutError = AssetValueFailure(TEXT("niagara_workspace_module_script_missing"),
          TEXT("Creating or replacing a script module requires script_asset_path."),
          SourceField, Object->TryGetField(SourceField), MakeShared<FJsonValueString>(TEXT("a Niagara Module script asset path")));
      return false;
    }
    if (!ResolvePatchScript(DesiredScriptPath, EScriptKind::Module, OutPatch.Script, OutError))
      return false;
    OutPatch.bReplaceScript = OutPatch.bExists;
    OutPatch.bNeedsMutation = true;
  } else if (!OutPatch.NativeCandidateName.IsEmpty()) {
    // Candidate validation still needs the planned script on an in-place
    // update. Retaining that evidence does not request a node replacement.
    if (!ResolvePatchScript(CurrentScriptPath, EScriptKind::Module,
                            OutPatch.Script, OutError))
      return false;
  }
  // Inputs on a new or replaced node must be resolved against that node after
  // native creation, not against the interface which is about to be removed.
  UNiagaraNodeFunctionCall *InputBaselineModule =
      OutPatch.bReplaceScript ? nullptr : ExistingModule;
  UNiagaraNode *InputBaselineAssignment =
      OutPatch.bReplaceScript ? nullptr : ExistingStandaloneAssignment;
  SourceField = TEXT("inputs");
  if (!ParseAndResolveLiteralInputs(Object, InputBaselineModule,
                                   InputBaselineAssignment, OutPatch.Inputs,
                                   OutError))
    return false;
  OutPatch.bNeedsMutation |= OutPatch.Inputs.ContainsByPredicate(
      [](const FLiteralInputPatch &Patch) { return Patch.bNeedsMutation; });
  SourceField = TEXT("dynamic_inputs");
  const TSharedPtr<FJsonObject> *DynamicInputs = nullptr;
  if (Object->TryGetObjectField(TEXT("dynamic_inputs"), DynamicInputs) &&
      DynamicInputs && DynamicInputs->IsValid()) {
    for (const TPair<FString, TSharedPtr<FJsonValue>> &Pair :
         (*DynamicInputs)->Values) {
      FDynamicInputPatch &DynamicPatch =
          OutPatch.DynamicInputs.AddDefaulted_GetRef();
      UEdGraphPin *ExistingTargetPin = nullptr;
      if (InputBaselineAssignment) {
        FNiagaraVariable ExistingTarget;
        ExistingTargetPin =
            UeAgentNiagaraStandaloneModuleAuthoring::FindAssignmentTargetPin(
                *InputBaselineAssignment, Pair.Key, ExistingTarget);
      }
      if (!ParseDynamicInputPatch(
              InputBaselineModule, ExistingTargetPin, Pair.Key, Pair.Value,
              *DynamicInputs, ResourceTracker,
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
        OutError = AssetValueFailure(TEXT("niagara_workspace_input_source_conflict"),
            TEXT("The same module input cannot be supplied by both a literal and a dynamic input."),
            TEXT("input_sources"), MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{
                MakeShared<FJsonValueString>(TEXT("inputs")), MakeShared<FJsonValueString>(TEXT("dynamic_inputs"))}),
            MakeShared<FJsonValueString>(TEXT("exactly one input source")));
        OutError.CandidateName = LiteralPatch.InputName;
        if (LiteralPatch.Payload.IsValid())
          UeAgentAuthoringDiagnostics::LocateFailure(OutError, *LiteralPatch.Payload, TEXT("preflight"), StableAssetPath);
        return false;
      }
    }
  }
  return true;
  }();
  if (!bParsed)
    UeAgentAuthoringDiagnostics::LocateFailure(OutError, *Object, TEXT("preflight"), StableAssetPath, SourceField);
  return bParsed;
}

bool ParseStagePatches(UObject *ProposedAsset, const FString &AssetPath,
                       const TSharedPtr<const FJsonObject> &ProviderExtension,
                       TArray<FStagePatch> &OutPatches, UE::UAI::Contract::FDiagnostic& OutError) {
  using namespace UeAgentNiagaraStageModuleAuthoring;
  FUeAgentStructuredResourceTracker ResourceTracker;
  OutPatches.Reset();
  if (!ProviderExtension.IsValid() ||
      !ProviderExtension->HasField(TEXT("stages")))
    return true;
  if (!ProposedAsset) {
    OutError = NativeAssetFailure(TEXT("niagara_workspace_stage_target_asset_missing"),
        TEXT("Stage patches require an available proposed Niagara asset."), nullptr,
        TEXT("proposed_asset.available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
    OutError.TargetPath = AssetPath;
    return false;
  }
  const TSharedPtr<FJsonObject> *Stages = nullptr;
  if (!ProviderExtension->TryGetObjectField(TEXT("stages"), Stages) ||
      !Stages || !Stages->IsValid()) {
    OutError = AssetValueFailure(TEXT("niagara_workspace_stages_invalid"),
        TEXT("stages must be an object keyed by stage entry name."),
        TEXT("stages"), ProviderExtension->TryGetField(TEXT("stages")), MakeShared<FJsonValueString>(TEXT("object")));
    UeAgentAuthoringDiagnostics::LocateFailure(OutError, *ProviderExtension, TEXT("preflight"), AssetPath, TEXT("stages"));
    return false;
  }
  if (!ResourceTracker.ChargeNodes(
          1, TEXT("/provider_extension/stages"), OutError) ||
      !ResourceTracker.ChargeItems(
          (*Stages)->Values.Num(), TEXT("/provider_extension/stages"),
          OutError)) {
    return false;
  }
  UE::UAI::Contract::FDiagnostic Errors;
  for (const TPair<FString, TSharedPtr<FJsonValue>> &StagePair :
       (*Stages)->Values) {
    TSharedPtr<const FJsonObject> SourceObject = *Stages;
    FString SourceField = StagePair.Key;
    OutError.Reset();
    const bool bParsed = [&]() {
    const FString StagePointer =
        TEXT("/provider_extension/stages/") +
        EscapeWorkspacePointerToken(StagePair.Key);
    const TSharedPtr<FJsonObject> StageObject =
        StagePair.Value.IsValid() && StagePair.Value->Type == EJson::Object
            ? StagePair.Value->AsObject()
            : nullptr;
    if (StageObject) { SourceObject = StageObject; SourceField = TEXT("target"); }
    const TSharedPtr<FJsonObject> *TargetObject = nullptr;
    if (!StageObject.IsValid() ||
        !StageObject->TryGetObjectField(TEXT("target"), TargetObject) ||
        !TargetObject || !TargetObject->IsValid()) {
      OutError = AssetValueFailure(TEXT("niagara_workspace_stage_invalid"),
          StageObject.IsValid() ? TEXT("A stage entry requires a target object.") : TEXT("A stage entry must be an object."),
          StageObject.IsValid() ? TEXT("target") : StagePair.Key,
          StageObject.IsValid() ? StageObject->TryGetField(TEXT("target")) : StagePair.Value,
          MakeShared<FJsonValueString>(TEXT("object")));
      return false;
    }
    if (!ResourceTracker.ChargeNodes(1, StagePointer, OutError) ||
        !ResourceTracker.ChargeItems(
            StageObject->Values.Num(), StagePointer, OutError)) {
      return false;
    }
    FStagePatch &StagePatch = OutPatches.AddDefaulted_GetRef();
    StagePatch.SourceKey = StagePair.Key;
    StagePatch.SourceObject = StageObject;
    SourceField = TEXT("operation");
    FString StageOperation;
    StageObject->TryGetStringField(TEXT("operation"), StageOperation);
    StageOperation.TrimStartAndEndInline();
    StageOperation.ToLowerInline();
    if (StageOperation == TEXT("replace")) {
      StagePatch.bReplaceStage = true;
      StagePatch.bNeedsMutation = true;
    } else if (!StageOperation.IsEmpty() && StageOperation != TEXT("update") &&
               StageOperation != TEXT("upsert")) {
      OutError = AssetValueFailure(TEXT("niagara_workspace_stage_operation_invalid"),
          TEXT("A stage operation must be update, upsert, replace, or omitted."),
          SourceField, MakeShared<FJsonValueString>(StageOperation), MakeShared<FJsonValueString>(TEXT("update, upsert, replace, or empty")));
      return false;
    }
    SourceField = TEXT("target");
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
      OutError = AssetValueFailure(TEXT("niagara_stage_not_found"),
          TEXT("No existing Niagara stage matches the requested target."),
          TEXT("target"), MakeShared<FJsonValueObject>(*TargetObject),
          MakeShared<FJsonValueString>(TEXT("a target matching an existing Niagara stage")));
      return false;
    }

    SourceField = TEXT("modules");
    bool bModuleErrors = false;
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
          OutError = AssetValueFailure(TEXT("niagara_workspace_module_invalid"),
              TEXT("A module entry must be an object."), ModulePair.Key, ModulePair.Value,
              MakeShared<FJsonValueString>(TEXT("object")));
          AppendPreflightError(OutError, *Modules, ModulePair.Key, AssetPath, Errors);
          bModuleErrors = true;
          OutError.Reset();
          continue;
        }
        if (!ResourceTracker.ChargeNodes(1, ModulePointer, OutError) ||
            !ResourceTracker.ChargeItems(
                ModuleObject->Values.Num(), ModulePointer, OutError)) {
          return false;
        }
        FModulePatch &ModulePatch = StagePatch.Modules.AddDefaulted_GetRef();
        if (!ParseModulePatch(StagePatch.Stage, AssetPath, ModulePair.Key,
                              ModuleObject, StagePatch.bReplaceStage,
                              ResourceTracker, ModulePatch, OutError)) {
          AppendPreflightError(OutError, ModuleObject, FString(), AssetPath, Errors);
          bModuleErrors = true;
          OutError.Reset();
        }
      }
    }
    if (bModuleErrors) return true;
    NormalizeModuleTargetIndices(StagePatch);
    if (StagePatch.bReplaceStage &&
        StagePatch.Modules.ContainsByPredicate(
            [](const FModulePatch &Patch) { return Patch.bRemove; })) {
      const FModulePatch* RemovedPatch = StagePatch.Modules.FindByPredicate(
          [](const FModulePatch& Patch) { return Patch.bRemove; });
      OutError = AssetValueFailure(TEXT("niagara_workspace_stage_replace_module_remove_conflict"),
          TEXT("A stage replacement describes the final module list and cannot contain module remove operations."),
          TEXT("remove"), MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false));
      if (RemovedPatch && RemovedPatch->SourceObject.IsValid()) {
        SourceObject = RemovedPatch->SourceObject;
        SourceField = TEXT("remove");
      }
      return false;
    }
    return true;
    }();
    if (!bParsed) AppendPreflightError(OutError, SourceObject, SourceField, AssetPath, Errors);
  }
  if (!Errors.IsEmpty()) {
    OutPatches.Reset();
    OutError = MoveTemp(Errors);
    return false;
  }
  OutError.Reset();
  return true;
}

bool ParseEmitterOwnerTarget(
    const FString &AssetPath, const TSharedPtr<const FJsonObject> &Object,
    UeAgentNiagaraStageModuleAuthoring::FEmitterOwnerTarget &OutTarget,
    UE::UAI::Contract::FDiagnostic& OutError) {
  OutTarget = UeAgentNiagaraStageModuleAuthoring::FEmitterOwnerTarget();
  OutTarget.AssetPath = AssetPath;
  if (!Object.IsValid()) {
    OutError = AssetValueFailure(TEXT("niagara_workspace_emitter_target_missing"),
        TEXT("An emitter target object is required."), TEXT("target"), nullptr,
        MakeShared<FJsonValueString>(TEXT("object")));
    return false;
  }
  ON_SCOPE_EXIT {
    if (OutError.IsValid())
      UeAgentAuthoringDiagnostics::LocateFailure(OutError, *Object,
          TEXT("niagara_authoring"), AssetPath, TEXT("emitter_handle_id"));
  };
  Object->TryGetStringField(TEXT("emitter_name"), OutTarget.EmitterName);
  FString HandleId;
  if (Object->TryGetStringField(TEXT("emitter_handle_id"), HandleId) &&
      !HandleId.IsEmpty()) {
    if (!FGuid::Parse(HandleId, OutTarget.EmitterHandleId)) {
      OutError = AssetValueFailure(TEXT("niagara_workspace_emitter_handle_id_invalid"),
          TEXT("The emitter handle selector must be a valid GUID."), TEXT("emitter_handle_id"),
          MakeShared<FJsonValueString>(HandleId), MakeShared<FJsonValueString>(TEXT("GUID")));
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
  int64 Value = 0;
  if (!UeAgentAuthoringTypeDescriptors::TryResolveEnumValueName(Enum, Text, Value, true))
    return false;
  OutMode = static_cast<EScriptExecutionMode>(Value);
  return true;
}

bool ReadUIntField(const TSharedPtr<const FJsonObject> &Object,
                   const TCHAR *Field, uint32 &OutValue, UE::UAI::Contract::FDiagnostic& OutError) {
  if (!Object.IsValid() || !Object->HasField(Field))
    return true;
  int64 Number;
  if (!UeAgentPropertyValueCodec::TryReadIntegerNumber(Object->TryGetField(Field), 0, MAX_uint32, Number, OutError)) return false;
  OutValue = static_cast<uint32>(Number);
  return true;
}

bool ParseEmitterStructurePatches(
    UObject *ProposedAsset, const FString &AssetPath,
    const TSharedPtr<const FJsonObject> &ProviderExtension,
    bool bCompactValues, TArray<FEmitterStructurePatch> &OutPatches,
    UE::UAI::Contract::FDiagnostic& OutError) {
  using namespace UeAgentNiagaraStageModuleAuthoring;
  OutPatches.Reset();
  if (!ProviderExtension.IsValid() ||
      !ProviderExtension->HasField(TEXT("emitters")))
    return true;
  if (!ProposedAsset) {
    OutError = NativeAssetFailure(TEXT("niagara_workspace_emitter_structure_asset_missing"),
        TEXT("The proposed Niagara asset is unavailable for emitter preflight."), nullptr,
        TEXT("proposed_asset.available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
    OutError.TargetPath = AssetPath;
    return false;
  }
  const TSharedPtr<FJsonObject> *Emitters = nullptr;
  if (!ProviderExtension->TryGetObjectField(TEXT("emitters"), Emitters) ||
      !Emitters || !Emitters->IsValid()) {
    OutError = AssetValueFailure(TEXT("niagara_workspace_emitters_invalid"),
        TEXT("Emitter changes must be an object keyed by emitter selector."), TEXT("emitters"),
        ProviderExtension->TryGetField(TEXT("emitters")), MakeShared<FJsonValueString>(TEXT("object")));
    UeAgentAuthoringDiagnostics::LocateFailure(OutError, *ProviderExtension,
        TEXT("niagara_authoring"), AssetPath, TEXT("emitters"));
    return false;
  }
  UE::UAI::Contract::FDiagnostic Errors;
  for (const TPair<FString, TSharedPtr<FJsonValue>> &OwnerPair :
       (*Emitters)->Values) {
    TSharedPtr<const FJsonObject> SourceObject = *Emitters;
    FString SourceField = OwnerPair.Key;
    OutError.Reset();
    const bool bParsed = [&]() {
    if (!OwnerPair.Value.IsValid() || OwnerPair.Value->Type != EJson::Object) {
      OutError = AssetValueFailure(TEXT("niagara_workspace_emitter_structure_invalid"),
          TEXT("Each emitter change must be an object."), OwnerPair.Key, OwnerPair.Value,
          MakeShared<FJsonValueString>(TEXT("object")));
      return false;
    }
    const TSharedPtr<FJsonObject> OwnerObject = OwnerPair.Value->AsObject();
    SourceObject = OwnerObject;
    SourceField.Reset();
    if (!UeAgentAuthoringJsonFields::ValidateOptionalFields(*OwnerObject, {
        {TEXT("source_asset_path"), EJson::String}, {TEXT("source_sync_mode"), EJson::String},
        {TEXT("emitter_version"), EJson::String}, {TEXT("renderers"), EJson::Object},
        {TEXT("event_handlers"), EJson::Object}}, OutError)) return false;
    SourceField = TEXT("enabled");
    bool bRequestedEnabled = true;
    const bool bHasEnabled = OwnerObject->HasField(TEXT("enabled"));
    if (bHasEnabled &&
        !OwnerObject->TryGetBoolField(TEXT("enabled"), bRequestedEnabled)) {
      OutError = AssetValueFailure(TEXT("niagara_workspace_emitter_enabled_invalid"),
          TEXT("Emitter enabled must be a boolean."), TEXT("enabled"),
          OwnerObject->TryGetField(TEXT("enabled")), MakeShared<FJsonValueString>(TEXT("boolean")));
      return false;
    }
    SourceField = TEXT("target");
    const TSharedPtr<FJsonObject> *TargetObject = nullptr;
    FEmitterStructurePatch &Patch = OutPatches.AddDefaulted_GetRef();
    Patch.SourceKey = OwnerPair.Key;
    Patch.SourceObject = OwnerObject;
    Patch.bCompactValues = bCompactValues;
    if (!OwnerObject->TryGetObjectField(TEXT("target"), TargetObject) ||
        !TargetObject || !TargetObject->IsValid()) {
      OutError = AssetValueFailure(TEXT("niagara_workspace_emitter_target_missing"),
          TEXT("An emitter change requires a target object."), TEXT("target"),
          OwnerObject->TryGetField(TEXT("target")), MakeShared<FJsonValueString>(TEXT("object")));
      return false;
    }
    if (!ParseEmitterOwnerTarget(AssetPath, *TargetObject, Patch.Target, OutError))
      return false;

    SourceField = TEXT("remove");
    bool bRemove = false;
    if (OwnerObject->HasField(TEXT("remove")) &&
        !OwnerObject->TryGetBoolField(TEXT("remove"), bRemove)) {
      OutError = AssetValueFailure(TEXT("niagara_workspace_emitter_remove_invalid"),
          TEXT("Emitter remove must be a boolean."), TEXT("remove"),
          OwnerObject->TryGetField(TEXT("remove")), MakeShared<FJsonValueString>(TEXT("boolean")));
      return false;
    }
    if (bRemove) {
      SourceField = TEXT("target");
      if (!ResolveEmitterOwner(Patch.Target, Patch.Owner, OutError, ProposedAsset))
        return false;
      if (!Patch.Owner.System || !Patch.Owner.EmitterHandleId.IsValid()) {
        OutError = AssetValueFailure(TEXT("niagara_workspace_emitter_remove_requires_system_handle"),
            TEXT("Emitter removal requires an emitter handle owned by a Niagara system."), TEXT("target"),
            MakeShared<FJsonValueObject>(*TargetObject), MakeShared<FJsonValueString>(TEXT("system emitter handle")));
        OutError.CandidateName = Patch.Target.EmitterName;
        return false;
      }
      for (const TCHAR* Field : {TEXT("source_asset_path"), TEXT("source_sync_mode"),
                                TEXT("emitter_version"), TEXT("enabled"), TEXT("properties"),
                                TEXT("renderers"), TEXT("event_handlers")}) {
        if (OwnerObject->HasField(Field)) {
          SourceField = Field;
          OutError = AssetValueFailure(TEXT("niagara_workspace_emitter_remove_conflicting_change"),
              FString::Printf(TEXT("Emitter removal cannot also modify '%s'."), Field), Field,
              OwnerObject->TryGetField(Field), nullptr);
          return false;
        }
      }
      Patch.TargetSystem = Patch.Owner.System;
      Patch.bRemoveEmitterHandle = true;
      Patch.bEnsureSystemIntegration = true;
      Patch.bNeedsMutation = true;
      return true;
    }

    SourceField = TEXT("source_asset_path");
    FString SourceAssetPath;
    OwnerObject->TryGetStringField(TEXT("source_asset_path"), SourceAssetPath);
    EEmitterSourceSyncMode SourceSyncMode = EEmitterSourceSyncMode::Merge;
    FString SourceSyncModeText;
    const bool bHasSourceSyncMode = OwnerObject->TryGetStringField(
        TEXT("source_sync_mode"), SourceSyncModeText);
    SourceField = TEXT("source_sync_mode");
    if (bHasSourceSyncMode) {
      SourceSyncModeText.TrimStartAndEndInline();
      if (SourceSyncModeText.Equals(TEXT("merge"), ESearchCase::IgnoreCase)) {
        SourceSyncMode = EEmitterSourceSyncMode::Merge;
      } else if (SourceSyncModeText.Equals(TEXT("replace"),
                                           ESearchCase::IgnoreCase)) {
        SourceSyncMode = EEmitterSourceSyncMode::Replace;
      } else {
        OutError = AssetValueFailure(TEXT("niagara_workspace_emitter_source_sync_mode_invalid"),
            TEXT("Emitter source synchronization mode must be merge or replace."), TEXT("source_sync_mode"),
            OwnerObject->TryGetField(TEXT("source_sync_mode")), MakeShared<FJsonValueString>(TEXT("merge | replace")));
        return false;
      }
      if (SourceAssetPath.IsEmpty()) {
        SourceField = TEXT("source_asset_path");
        OutError = AssetValueFailure(TEXT("niagara_workspace_emitter_source_sync_mode_requires_source"),
            TEXT("Emitter source synchronization requires a source asset path."), TEXT("source_asset_path"),
            OwnerObject->TryGetField(TEXT("source_asset_path")), MakeShared<FJsonValueString>(TEXT("non-empty Niagara emitter asset path")));
        return false;
      }
    }
    UNiagaraEmitter *SourceEmitter = nullptr;
    if (!SourceAssetPath.IsEmpty()) {
      SourceField = TEXT("source_asset_path");
      Patch.TargetSystem = Cast<UNiagaraSystem>(ProposedAsset);
      if (!Patch.TargetSystem.IsValid()) {
        OutError = AssetValueFailure(TEXT("niagara_workspace_emitter_handle_add_requires_system"),
            TEXT("Adding or synchronizing an emitter handle requires a Niagara system asset."), TEXT("asset.class"),
            MakeShared<FJsonValueString>(ProposedAsset->GetClass()->GetName()),
            MakeShared<FJsonValueString>(UNiagaraSystem::StaticClass()->GetName()));
        return false;
      }
      Patch.bEnsureSystemIntegration = true;
    SourceField = TEXT("source_asset_path");
      SourceEmitter = Cast<UNiagaraEmitter>(LoadAsset(SourceAssetPath));
      if (!SourceEmitter) {
        OutError = AssetValueFailure(TEXT("niagara_workspace_emitter_source_not_found"),
            TEXT("The source asset could not be loaded as a Niagara emitter."), TEXT("source_asset_path"),
            MakeShared<FJsonValueString>(SourceAssetPath), MakeShared<FJsonValueString>(TEXT("loadable Niagara emitter asset")));
        return false;
      }
      Patch.SourceEmitter.Reset(SourceEmitter);
      Patch.SourceEmitterVersion =
          SourceEmitter->GetExposedVersion().VersionGuid;
    SourceField = TEXT("emitter_version");
      FString VersionText;
      if (OwnerObject->TryGetStringField(TEXT("emitter_version"),
                                         VersionText) &&
          !VersionText.IsEmpty() &&
          !FGuid::Parse(VersionText, Patch.SourceEmitterVersion)) {
        OutError = AssetValueFailure(TEXT("niagara_workspace_emitter_version_invalid"),
            TEXT("The emitter version must be a valid GUID."), TEXT("emitter_version"),
            MakeShared<FJsonValueString>(VersionText), MakeShared<FJsonValueString>(TEXT("GUID")));
        return false;
      }
    }
    SourceField = TEXT("target");
    if (!ResolveEmitterOwner(Patch.Target, Patch.Owner, OutError,
                             ProposedAsset)) {
      if (SourceAssetPath.IsEmpty() || OutError.Code != TEXT("niagara_emitter_owner_not_found"))
        return false;
      if (Patch.Target.bEmitterHandleIdExplicit) {
        SourceObject = *TargetObject;
        SourceField = TEXT("emitter_handle_id");
        OutError = AssetValueFailure(TEXT("niagara_workspace_new_emitter_handle_id_not_allowed"),
            TEXT("A new emitter handle cannot use an explicit existing handle identifier."), TEXT("emitter_handle_id"),
            (*TargetObject)->TryGetField(TEXT("emitter_handle_id")), nullptr);
        return false;
      }
      if (Patch.Target.EmitterName.IsEmpty()) {
        SourceObject = *TargetObject;
        SourceField = TEXT("emitter_name");
        OutError = AssetValueFailure(TEXT("niagara_workspace_new_emitter_name_missing"),
            TEXT("A new emitter handle requires a non-empty emitter name."), TEXT("emitter_name"),
            (*TargetObject)->TryGetField(TEXT("emitter_name")), MakeShared<FJsonValueString>(TEXT("non-empty emitter name")));
        return false;
      }
      UNiagaraSystem *System = Cast<UNiagaraSystem>(ProposedAsset);
      if (!System) {
        OutError = AssetValueFailure(TEXT("niagara_workspace_emitter_handle_add_requires_system"),
            TEXT("Adding or synchronizing an emitter handle requires a Niagara system asset."), TEXT("asset.class"),
            MakeShared<FJsonValueString>(ProposedAsset->GetClass()->GetName()),
            MakeShared<FJsonValueString>(UNiagaraSystem::StaticClass()->GetName()));
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
      const FString RequestedPath =
          UeAgentAuthoringAssetIdentity::NormalizePackagePath(SourceAssetPath);
      if (!Parent.Emitter || ParentPath != RequestedPath) {
        SourceField = TEXT("source_asset_path");
        OutError = AssetValueFailure(TEXT("niagara_workspace_emitter_source_conflict"),
            TEXT("The requested source asset does not match the existing emitter's parent asset."), TEXT("source_asset_path"),
            MakeShared<FJsonValueString>(RequestedPath), Parent.Emitter
                ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(ParentPath)) : TSharedPtr<FJsonValue>());
        OutError.CandidateName = Patch.Target.EmitterName;
        return false;
      }
      if (Patch.SourceEmitterVersion.IsValid() && Parent.Version.IsValid() &&
          Patch.SourceEmitterVersion != Parent.Version) {
        SourceField = TEXT("emitter_version");
        OutError = AssetValueFailure(TEXT("niagara_workspace_emitter_version_conflict"),
            TEXT("The requested source version does not match the existing emitter's parent version."), TEXT("emitter_version"),
            MakeShared<FJsonValueString>(Patch.SourceEmitterVersion.ToString()),
            MakeShared<FJsonValueString>(Parent.Version.ToString()));
        OutError.CandidateName = Patch.Target.EmitterName;
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
    SourceField = TEXT("enabled");
    if (bHasEnabled) {
      UNiagaraSystem *System = Patch.TargetSystem.Get();
      if (!System) {
        OutError = AssetValueFailure(TEXT("niagara_workspace_emitter_enabled_requires_system_handle"),
            TEXT("Changing enabled requires an emitter handle owned by a Niagara system."), TEXT("enabled"),
            MakeShared<FJsonValueBoolean>(bRequestedEnabled), MakeShared<FJsonValueString>(TEXT("system emitter handle")));
        OutError.CandidateName = Patch.Target.EmitterName;
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
          OutError = NativeAssetFailure(TEXT("niagara_workspace_emitter_enabled_handle_missing"),
              TEXT("The resolved emitter handle is no longer present in the system during enabled preflight."), System,
              TEXT("emitter_handle.available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
          OutError.CandidateName = Patch.Target.EmitterName;
          return false;
        }
        Patch.bEmitterEnabledNeedsMutation =
            Handle->GetIsEnabled() != bRequestedEnabled;
      }
      Patch.bEnsureSystemIntegration |= Patch.bEmitterEnabledNeedsMutation;
    }
    SourceField = TEXT("properties");
    if (!UeAgentDetailsAuthoringService::ReadPropertyPayload(OwnerObject, TEXT("properties"), Patch.Properties, OutError)) return false;

    SourceField = TEXT("renderers");
    const TSharedPtr<FJsonObject> *Renderers = nullptr;
    if (OwnerObject->TryGetObjectField(TEXT("renderers"), Renderers) &&
        Renderers && Renderers->IsValid()) {
      Patch.bReplaceRenderers = true;
      for (const TPair<FString, TSharedPtr<FJsonValue>> &Pair :
           (*Renderers)->Values) {
        SourceObject = *Renderers;
        SourceField = Pair.Key;
        FRendererPatch &Renderer = Patch.Renderers.AddDefaulted_GetRef();
        Renderer.SourceOwner = *Renderers;
        Renderer.SourceKey = Pair.Key;
        Renderer.RendererName = Pair.Key;
        if (!Pair.Value.IsValid() || Pair.Value->Type == EJson::Null) {
          Renderer.bRemove = true;
          continue;
        }
        if (Pair.Value->Type != EJson::Object) {
          OutError = AssetValueFailure(TEXT("niagara_workspace_renderer_invalid"),
              TEXT("A renderer change must be an object or null for removal."), Pair.Key, Pair.Value,
              MakeShared<FJsonValueString>(TEXT("object | null")));
          OutError.CandidateName = Pair.Key;
          return false;
        }
        const TSharedPtr<FJsonObject> Object = Pair.Value->AsObject();
        Renderer.SourceObject = Object;
        SourceObject = Object;
        SourceField.Reset();
        if (!UeAgentAuthoringJsonFields::ValidateOptionalFields(*Object, {
            {TEXT("renderer_name"), EJson::String}, {TEXT("renderer_class"), EJson::String},
            {TEXT("remove"), EJson::Boolean}}, OutError)) return false;
        Object->TryGetStringField(TEXT("renderer_name"), Renderer.RendererName);
        if (Renderer.RendererName.IsEmpty())
          Renderer.RendererName = Pair.Key;
        Object->TryGetStringField(TEXT("renderer_class"),
                                  Renderer.RendererClass);
        Object->TryGetBoolField(TEXT("remove"), Renderer.bRemove);
        SourceField = TEXT("renderer_index");
        if (!UeAgentAuthoringJsonFields::ReadInt32Field(*Object, TEXT("renderer_index"), Renderer.TargetIndex, OutError)) return false;
        if (Object->HasField(TEXT("renderer_index")))
          Renderer.TargetIndex = FMath::Max(0, Renderer.TargetIndex);
        SourceField = TEXT("properties");
        if (!UeAgentDetailsAuthoringService::ReadPropertyPayload(Object, TEXT("properties"), Renderer.Properties, OutError)) return false;
      }
    }

    SourceField = TEXT("event_handlers");
    SourceObject = OwnerObject;
    const TSharedPtr<FJsonObject> *Events = nullptr;
    if (OwnerObject->TryGetObjectField(TEXT("event_handlers"), Events) &&
        Events && Events->IsValid()) {
      for (const TPair<FString, TSharedPtr<FJsonValue>> &Pair :
           (*Events)->Values) {
        SourceObject = *Events;
        SourceField = Pair.Key;
        FEventHandlerPatch &Event = Patch.EventHandlers.AddDefaulted_GetRef();
        Event.SourceOwner = *Events;
        Event.SourceKey = Pair.Key;
        if (!FGuid::Parse(Pair.Key, Event.UsageId)) {
          OutError = AssetValueFailure(TEXT("niagara_workspace_event_usage_id_invalid"),
              TEXT("An event handler key must be a valid usage GUID."), TEXT("usage_id"),
              MakeShared<FJsonValueString>(Pair.Key), MakeShared<FJsonValueString>(TEXT("GUID")));
          return false;
        }
        if (!Pair.Value.IsValid() || Pair.Value->Type == EJson::Null) {
          Event.bRemove = true;
          continue;
        }
        if (Pair.Value->Type != EJson::Object) {
          OutError = AssetValueFailure(TEXT("niagara_workspace_event_handler_invalid"),
              TEXT("An event handler change must be an object or null for removal."), Pair.Key, Pair.Value,
              MakeShared<FJsonValueString>(TEXT("object | null")));
          return false;
        }
        const TSharedPtr<FJsonObject> Object = Pair.Value->AsObject();
        Event.SourceObject = Object;
        SourceObject = Object;
        SourceField.Reset();
        if (!UeAgentAuthoringJsonFields::ValidateOptionalFields(*Object, {
            {TEXT("remove"), EJson::Boolean}, {TEXT("source_event_name"), EJson::String},
            {TEXT("source_emitter_id"), EJson::String}, {TEXT("execution_mode"), EJson::String},
            {TEXT("random_spawn_number"), EJson::Boolean},
            {TEXT("update_attribute_initial_values"), EJson::Boolean}}, OutError)) return false;
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
        SourceField = TEXT("source_emitter_id");
        if (Object->TryGetStringField(TEXT("source_emitter_id"), Text)) {
          if (Text.IsEmpty()) Event.SourceEmitterId.Invalidate();
          else if (!FGuid::Parse(Text, Event.SourceEmitterId)) {
            OutError = AssetValueFailure(TEXT("niagara_workspace_source_emitter_id_invalid"),
                TEXT("The source emitter identifier must be empty or a valid GUID."), TEXT("source_emitter_id"),
                MakeShared<FJsonValueString>(Text), MakeShared<FJsonValueString>(TEXT("empty string | GUID")));
            return false;
          }
        }
        SourceField = TEXT("execution_mode");
        if (Object->TryGetStringField(TEXT("execution_mode"), Text) &&
            !ParseExecutionMode(Text, Event.ExecutionMode)) {
          OutError = AssetValueFailure(TEXT("niagara_workspace_execution_mode_invalid"),
              TEXT("The event execution mode is not a recognized EScriptExecutionMode name."), TEXT("execution_mode"),
              MakeShared<FJsonValueString>(Text), MakeShared<FJsonValueString>(TEXT("EScriptExecutionMode enum name")));
          return false;
        }
        auto ReadNumber = [&](const TCHAR* Field, uint32& Value) {
          SourceField = Field;
          return ReadUIntField(Object, Field, Value, OutError);
        };
        if (!ReadNumber(TEXT("spawn_number"), Event.SpawnNumber) ||
            !ReadNumber(TEXT("min_spawn_number"), Event.MinSpawnNumber) ||
            !ReadNumber(TEXT("max_events_per_frame"), Event.MaxEventsPerFrame)) {
          return false;
        }
        Object->TryGetBoolField(TEXT("random_spawn_number"),
                                Event.bRandomSpawnNumber);
        Object->TryGetBoolField(TEXT("update_attribute_initial_values"),
                                Event.bUpdateAttributeInitialValues);
      }
    }
    SourceObject = OwnerObject;
    SourceField = TEXT("properties");
    if (!ValidateEmitterStructurePropertyPatches(Patch, OutError))
      return false;
    Patch.bNeedsMutation =
        Patch.bAddEmitterHandle || Patch.bRefreshEmitterFromSource ||
        Patch.bReplaceEmitterFromSource || Patch.bEmitterEnabledNeedsMutation ||
        Patch.Properties.IsValid() || Patch.bReplaceRenderers ||
        !Patch.Renderers.IsEmpty() ||
        !Patch.EventHandlers.IsEmpty();
    return true;
    }();
    if (!bParsed) AppendPreflightError(OutError, SourceObject, SourceField, AssetPath, Errors);
  }
  if (!Errors.IsEmpty()) {
    OutPatches.Reset();
    OutError = MoveTemp(Errors);
    return false;
  }
  OutError.Reset();
  return true;
}

bool ApplyLiteralInputPatches(UNiagaraNodeFunctionCall &Module,
                               const TArray<FLiteralInputPatch> &Patches,
                               UE::UAI::Contract::FDiagnostic& OutError) {
  using namespace UeAgentNiagaraStageModuleAuthoring;
  const bool bAssignmentModule = Cast<UNiagaraNodeAssignment>(&Module) != nullptr;
  for (const FLiteralInputPatch &Patch : Patches) {
    if (!Patch.bNeedsMutation && !bAssignmentModule)
      continue;
    FString ResolvedInputName;
    bool bChanged = false;
    if (!SetInputValue(Module, Patch.InputName, Patch.Payload,
                       ResolvedInputName, bChanged, OutError)) {
      if (Patch.Payload.IsValid())
        UeAgentAuthoringDiagnostics::LocateFailure(OutError, *Patch.Payload, TEXT("niagara_authoring"), FString());
      return false;
    }
  }
  return true;
}

bool ApplyLiteralInputPatches(UNiagaraNode &AssignmentNode,
                              const TArray<FLiteralInputPatch> &Patches,
                              UE::UAI::Contract::FDiagnostic& OutError) {
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
      if (Patch.Payload.IsValid())
        UeAgentAuthoringDiagnostics::LocateFailure(OutError, *Patch.Payload, TEXT("niagara_authoring"), FString());
      return false;
    }
    bool bChanged = false;
    if (!UeAgentNiagaraStageModuleAuthoring::SetInputValueForPin(
            *TargetPin, TargetVariable.GetType(), Patch.Payload, bChanged,
            OutError)) {
      if (Patch.Payload.IsValid())
        UeAgentAuthoringDiagnostics::LocateFailure(OutError, *Patch.Payload, TEXT("niagara_authoring"), FString());
      return false;
    }
  }
  return true;
}

bool ApplyDataInterfacePatches(UNiagaraNodeFunctionCall &AppliedDynamic,
                               const TArray<FDataInterfacePatch> &Patches,
                               UE::UAI::Contract::FDiagnostic& OutError) {
  using namespace UeAgentNiagaraStageModuleAuthoring;
  for (const FDataInterfacePatch &DataInterfacePatch : Patches) {
    FString ErrorField = TEXT("input_name");
    auto Fail = [&]() {
      if (OutError.IsValid() && DataInterfacePatch.SourceObject.IsValid())
        UeAgentAuthoringDiagnostics::LocateFailure(OutError, *DataInterfacePatch.SourceObject,
            TEXT("niagara_authoring"), FString(), ErrorField);
      return false;
    };
    UNiagaraNodeInput *InputNode = nullptr;
    UNiagaraDataInterface *DataInterface = nullptr;
    FString ResolvedDataInterfaceInputName;
    if (!ResolveDynamicInputDataInterface(
            AppliedDynamic, DataInterfacePatch.InputName, InputNode,
            DataInterface, ResolvedDataInterfaceInputName, OutError))
      return Fail();
    if (!DataInterfacePatch.DataInterfaceClass.IsEmpty() &&
        !DataInterface->GetClass()->GetPathName().Equals(
            DataInterfacePatch.DataInterfaceClass, ESearchCase::IgnoreCase)) {
      ErrorField = TEXT("data_interface_class");
      OutError = AssetValueFailure(TEXT("niagara_workspace_data_interface_class_mismatch"),
          TEXT("The requested data-interface class differs from the class of the resolved input."),
          ErrorField, MakeShared<FJsonValueString>(DataInterfacePatch.DataInterfaceClass),
          MakeShared<FJsonValueString>(DataInterface->GetClass()->GetPathName()));
      OutError.CandidateName = ResolvedDataInterfaceInputName;
      return Fail();
    }
    UeAgentAuthoringObjectApply::FOptions Options;
    Options.bAllowDomainAuthorizedOwners = true;
    Options.bMarkPackageDirty = false;
    UeAgentAuthoringDirectApply::FResult Result;
    if (!UeAgentDetailsAuthoringService::ApplyObjectProperties(DataInterface, DataInterfacePatch.Properties, Options, Result)) {
      OutError = Result.Diagnostic;
      ErrorField = TEXT("properties");
      return Fail();
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
    FUeAgentStructuredResourceTracker &ResourceTracker, UE::UAI::Contract::FDiagnostic& OutError) {
  using namespace UeAgentNiagaraStageModuleAuthoring;
  while (!Stack.IsEmpty()) {
    FDynamicInputApplyFrame Frame = Stack.Pop(EAllowShrinking::No);
    if (!Frame.Owner || !Frame.Patch) {
      OutError = NativeAssetFailure(TEXT("niagara_workspace_dynamic_input_apply_frame_invalid"),
          !Frame.Owner ? TEXT("The dynamic-input apply frame has no owner node.")
              : TEXT("The dynamic-input apply frame has no patch."), Frame.Owner,
          !Frame.Owner ? TEXT("apply_frame.owner_available") : TEXT("apply_frame.patch_available"),
          MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
      return false;
    }
    const FDynamicInputPatch &Patch = *Frame.Patch;
    if (!Patch.bNeedsMutation)
      continue;
    FString ErrorField;
    auto Fail = [&]() { LocateDynamicInputFailure(OutError, Patch, ErrorField); return false; };
    const FString WorkspacePointer =
        Frame.ParentPointer + TEXT("/dynamic_inputs/") +
        EscapeWorkspacePointerToken(Patch.InputName);
    if (!ResourceTracker.ChargeNodes(1, WorkspacePointer, OutError) ||
        !ResourceTracker.ChargeItems(1, WorkspacePointer, OutError) ||
        !ResourceTracker.ChargeWorkUnits(1, WorkspacePointer, OutError) ||
        !ResourceTracker.ChargeEstimatedMemoryBytes(
            sizeof(FDynamicInputApplyFrame), WorkspacePointer, OutError)) {
      return Fail();
    }

    UNiagaraNodeFunctionCall &OwnerNode = *Frame.Owner;
    FString ResolvedInputName;
    UNiagaraNodeFunctionCall *Existing = FindDynamicInputForModuleInput(OwnerNode, Patch.InputName, ResolvedInputName, OutError);
    if (OutError.IsValid()) { ErrorField = TEXT("input_name"); return Fail(); }
    if (Patch.bRemove) {
      if (!Existing || ClearDynamicInput(OwnerNode, Patch.InputName,
                                         ResolvedInputName, OutError))
        continue;
      return Fail();
    }
    const FString ExistingPath =
        Existing ? GetModuleScriptPath(*Existing) : FString();
    const FString DesiredPath =
        Patch.Script.IsValid() ? Patch.Script->GetPathName() : ExistingPath;
    UNiagaraNodeFunctionCall *AppliedDynamic = Existing;
    if (!ExistingPath.Equals(DesiredPath, ESearchCase::IgnoreCase)) {
      ErrorField = TEXT("script_asset_path");
      if (!Patch.Script.IsValid()) {
        OutError = NativeAssetFailure(TEXT("niagara_workspace_dynamic_input_apply_failed"),
            TEXT("The replacement dynamic-input script is no longer available."), &OwnerNode,
            TEXT("dynamic_input.script_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
        OutError.CandidateName = Patch.InputName;
        return Fail();
      }
      if (!SetDynamicInput(OwnerNode, Patch.InputName, *Patch.Script, FString(),
                          AppliedDynamic, ResolvedInputName, OutError)) return Fail();
      ErrorField.Reset();
    }
    if (!AppliedDynamic) {
      OutError = NativeAssetFailure(TEXT("niagara_workspace_dynamic_input_not_found_after_apply"),
          TEXT("No dynamic-input node is available after applying the requested input."), &OwnerNode,
          TEXT("dynamic_input.node_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
      OutError.CandidateName = Patch.InputName;
      return Fail();
    }
    if (!InitializeDynamicInputDataInterfaces(
            *AppliedDynamic, OutError)) {
      return Fail();
    }
    if (!ApplyLiteralInputPatches(*AppliedDynamic, Patch.Inputs, OutError)) {
      return Fail();
    }
    if (!ApplyDataInterfacePatches(*AppliedDynamic, Patch.DataInterfaces,
                                   OutError)) {
      return Fail();
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
                              UE::UAI::Contract::FDiagnostic& OutError) {
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
                              UE::UAI::Contract::FDiagnostic& OutError) {
  using namespace UeAgentNiagaraStageModuleAuthoring;
  using namespace UeAgentNiagaraStandaloneModuleAuthoring;
  for (const FDynamicInputPatch &Patch : Patches) {
    if (!Patch.bNeedsMutation)
      continue;
    FString ErrorField;
    auto Fail = [&]() { LocateDynamicInputFailure(OutError, Patch, ErrorField); return false; };
    const FString WorkspacePointer =
        TEXT("/dynamic_inputs/") +
        EscapeWorkspacePointerToken(Patch.InputName);
    if (!ResourceTracker.ChargeNodes(1, WorkspacePointer, OutError) ||
        !ResourceTracker.ChargeItems(1, WorkspacePointer, OutError) ||
        !ResourceTracker.ChargeWorkUnits(1, WorkspacePointer, OutError) ||
        !ResourceTracker.ChargeEstimatedMemoryBytes(
            sizeof(FDynamicInputApplyFrame), WorkspacePointer, OutError)) {
      return Fail();
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
      ErrorField = TEXT("input_name");
      return Fail();
    }

    UNiagaraNodeFunctionCall *Existing = FindDynamicInputForPin(*TargetPin);
    if (Patch.bRemove) {
      if (Existing && !ClearInputValueNodesForPin(*TargetPin, OutError))
        return Fail();
      continue;
    }
    const FString ExistingPath =
        Existing ? GetModuleScriptPath(*Existing) : FString();
    const FString DesiredPath =
        Patch.Script.IsValid() ? Patch.Script->GetPathName() : ExistingPath;
    UNiagaraNodeFunctionCall *AppliedDynamic = Existing;
    if (!ExistingPath.Equals(DesiredPath, ESearchCase::IgnoreCase)) {
      ErrorField = TEXT("script_asset_path");
      if (!Patch.Script.IsValid()) {
        OutError = NativeAssetFailure(TEXT("niagara_workspace_dynamic_input_apply_failed"),
            TEXT("The replacement dynamic-input script is no longer available."), &AssignmentNode,
            TEXT("dynamic_input.script_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
        OutError.CandidateName = Patch.InputName;
        return Fail();
      }
      if (!SetDynamicInputForPin(*TargetPin, *Patch.Script, FString(), AppliedDynamic, OutError))
        return Fail();
      ErrorField.Reset();
    }
    if (!AppliedDynamic) {
      OutError = NativeAssetFailure(TEXT("niagara_workspace_dynamic_input_not_found_after_apply"),
          TEXT("No dynamic-input node is available after applying the assignment input."), &AssignmentNode,
          TEXT("dynamic_input.node_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
      OutError.CandidateName = Patch.InputName;
      return Fail();
    }
    if (!InitializeDynamicInputDataInterfaces(
            *AppliedDynamic, OutError) ||
        !ApplyLiteralInputPatches(*AppliedDynamic, Patch.Inputs, OutError) ||
        !ApplyDataInterfacePatches(*AppliedDynamic, Patch.DataInterfaces,
                                   OutError)) {
      return Fail();
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
      return Fail();
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

bool ReadProjectedPropertyDescriptors(
    const TSharedPtr<FJsonObject> &Properties, const TCHAR *MissingArrayError,
    TArray<TSharedPtr<FJsonValue>> &OutDescriptors, UE::UAI::Contract::FDiagnostic& OutError) {
  OutDescriptors.Reset();
  if (!Properties.IsValid())
    return true;
  if (!UeAgentDetailsAuthoringService::DecodePropertiesDocument(
          Properties, OutDescriptors, OutError)) {
    if (OutError.Code == TEXT("missing_properties_array"))
      OutError.Code = MissingArrayError;
    return false;
  }
  return true;
}


bool ResolveEmitterValidationContext(
    const FEmitterStructurePatch &Patch, UNiagaraEmitter *&OutEmitter,
    FVersionedNiagaraEmitterData *&OutEmitterData) {
  const bool bUsesSourceState =
      Patch.bAddEmitterHandle || Patch.bReplaceEmitterFromSource;
  OutEmitter = bUsesSourceState ? Patch.SourceEmitter.Get() : Patch.Owner.Emitter;
  OutEmitterData = bUsesSourceState
                       ? (OutEmitter && Patch.SourceEmitterVersion.IsValid()
                              ? OutEmitter->GetEmitterData(Patch.SourceEmitterVersion)
                              : nullptr)
                       : Patch.Owner.EmitterData;
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
                               const bool bCompactValues,
                               UE::UAI::Contract::FDiagnostic& OutError) {
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
  Options.bCompactValues = bCompactValues;
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
      OutError = NativeAssetFailure(TEXT("niagara_emitter_properties_preflight_clone_failed"),
          TEXT("A transient emitter and its version data could not be created for property preflight."), &Emitter,
          ValidationEmitter ? TEXT("validation_emitter.version_data.available") : TEXT("validation_emitter.available"),
          MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
      return false;
    }
  }
  UeAgentAuthoringObjectApply::FPreparedApply Prepared;
  UeAgentAuthoringObjectApply::FFailure Failure;
  if (!UeAgentAuthoringObjectApply::PrepareStruct(
          FVersionedNiagaraEmitterData::StaticStruct(), ValidationData,
          ValidationEmitter, Descriptors, Options, Prepared, Failure)) {
    OutError = Failure.Diagnostic;
    return false;
  }
  return true;
}

bool ReadRendererEnabledDescriptor(const TSharedPtr<FJsonObject> &Descriptor,
                                   bool &OutEnabled, UE::UAI::Contract::FDiagnostic& OutError) {
  ON_SCOPE_EXIT {
    if (OutError.IsValid() && Descriptor.IsValid())
      UeAgentAuthoringDiagnostics::LocatePropertyValueSource(OutError, *Descriptor);
  };
  const TSharedPtr<FJsonObject> Projected =
      UeAgentAuthoringObjectApply::ProjectDescriptorForApply(Descriptor);
  if (!Projected.IsValid()) {
    OutError = AssetValueFailure(TEXT("niagara_renderer_enabled_descriptor_invalid"),
        TEXT("The renderer enabled property requires an object descriptor."), TEXT("bIsEnabled"),
        Descriptor.IsValid() ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueObject>(Descriptor)) : TSharedPtr<FJsonValue>(),
        MakeShared<FJsonValueString>(TEXT("property descriptor object")));
    return false;
  }
  for (const TPair<FString, TSharedPtr<FJsonValue>> &Field :
       Projected->Values) {
    if (Field.Key != TEXT("property_path") && Field.Key != TEXT("kind") &&
        Field.Key != TEXT("value") && Field.Key != TEXT("authoring_source")) {
      OutError = AssetValueFailure(TEXT("niagara_renderer_enabled_descriptor_field_invalid"),
          FString::Printf(TEXT("Renderer enabled does not support descriptor field '%s'."), *Field.Key),
          TEXT("bIsEnabled.") + Field.Key, Field.Value, nullptr);
      return false;
    }
  }
  FString ValueKind;
  if (Projected->TryGetStringField(TEXT("kind"), ValueKind) &&
      ValueKind != TEXT("bool")) {
    OutError = AssetValueFailure(TEXT("niagara_renderer_enabled_value_kind_invalid"),
        TEXT("The renderer enabled descriptor kind must be bool."), TEXT("bIsEnabled.kind"),
        MakeShared<FJsonValueString>(ValueKind), MakeShared<FJsonValueString>(TEXT("bool")));
    return false;
  }
  const TSharedPtr<FJsonObject> *Value = nullptr;
  if (!Projected->TryGetObjectField(TEXT("value"), Value) || !Value ||
      !Value->IsValid() || (*Value)->Values.Num() != 1 ||
      !(*Value)->TryGetBoolField(TEXT("literal"), OutEnabled)) {
    OutError = AssetValueFailure(TEXT("niagara_renderer_enabled_literal_invalid"),
        TEXT("The renderer enabled value must contain exactly one boolean literal."), TEXT("bIsEnabled.value"),
        Projected->TryGetField(TEXT("value")), MakeShared<FJsonValueString>(TEXT("{ literal: boolean }")));
    return false;
  }
  return true;
}

bool ValidateRendererProperties(UNiagaraRendererProperties &Renderer,
                                const TSharedPtr<FJsonObject> &Properties,
                                UE::UAI::Contract::FDiagnostic& OutError) {
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
        OutError = AssetValueFailure(TEXT("niagara_renderer_enabled_descriptor_duplicate"),
            TEXT("The renderer enabled property occurs more than once in the property changes."), TEXT("bIsEnabled.descriptor_count"),
            MakeShared<FJsonValueNumber>(2), MakeShared<FJsonValueNumber>(1));
        UeAgentAuthoringDiagnostics::LocatePropertyValueSource(OutError, *Descriptor);
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
    FName BindingName;
    UeAgentAuthoringObjectApply::FPropertyIntent Intent;
    UeAgentAuthoringObjectApply::FFailure Failure;
    if (!UeAgentNiagaraRendererBindings::PrepareAuthoringBinding(*StructProperty, Descriptor,
        Renderer, FVersionedNiagaraEmitter(), BindingName, Intent, Failure)) {
      OutError = Failure.Diagnostic;
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
    OutError = Failure.Diagnostic;
    return false;
  }
  return true;
}

bool ValidateEmitterStructurePropertyPatches(
    const FEmitterStructurePatch &Patch, UE::UAI::Contract::FDiagnostic& OutError) {
  UNiagaraEmitter *ValidationEmitter = nullptr;
  FVersionedNiagaraEmitterData *ValidationData = nullptr;
  if (!ResolveEmitterValidationContext(Patch, ValidationEmitter,
                                       ValidationData)) {
    if (!Patch.Properties.IsValid() && Patch.Renderers.IsEmpty())
      return true;
    OutError = NativeAssetFailure(TEXT("niagara_workspace_emitter_property_preflight_context_missing"),
        TEXT("Emitter property preflight requires an emitter instance and its selected version data."), ValidationEmitter,
        ValidationEmitter ? TEXT("emitter.version_data.available") : TEXT("emitter.available"),
        MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
    OutError.TargetPath = Patch.Target.AssetPath;
    OutError.CandidateName = Patch.Target.EmitterName;
    return false;
  }
  if (!ValidateEmitterProperties(*ValidationData, *ValidationEmitter,
                                 Patch.Properties, Patch.bCompactValues, OutError))
    return false;

  FVersionedNiagaraEmitterData *SourceData =
      Patch.SourceEmitter.IsValid() && Patch.SourceEmitterVersion.IsValid()
          ? Patch.SourceEmitter->GetEmitterData(Patch.SourceEmitterVersion)
          : nullptr;
  for (const FRendererPatch &RendererPatch : Patch.Renderers) {
    if (RendererPatch.bRemove)
      continue;
    FString FailureField = TEXT("renderer_class");
    ON_SCOPE_EXIT {
      if (OutError.IsValid()) {
        if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = RendererPatch.RendererName;
        if (RendererPatch.SourceObject.IsValid())
          UeAgentAuthoringDiagnostics::LocateFailure(OutError, *RendererPatch.SourceObject,
              TEXT("niagara_authoring"), Patch.Target.AssetPath, FailureField);
      }
    };
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
      OutError = AssetValueFailure(TEXT("niagara_workspace_renderer_class_invalid"),
          TEXT("The renderer class could not be resolved, and no existing renderer supplies its class."),
          TEXT("renderer_class"), RendererPatch.SourceObject.IsValid()
              ? RendererPatch.SourceObject->TryGetField(TEXT("renderer_class")) : TSharedPtr<FJsonValue>(),
          MakeShared<FJsonValueString>(TEXT("supported Niagara renderer class or existing renderer")));
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
      OutError = NativeAssetFailure(TEXT("niagara_workspace_renderer_preflight_create_failed"),
          TEXT("A transient renderer could not be created for property preflight."), nullptr,
          TEXT("validation_renderer.available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
      return false;
    }
    FailureField = TEXT("properties");
    if (!ValidateRendererProperties(*Probe, RendererPatch.Properties, OutError))
      return false;
  }
  return true;
}

bool ApplyRendererProperties(UNiagaraRendererProperties &Renderer,
                             UNiagaraEmitter &Emitter,
                             const FGuid &EmitterVersion,
                             const TSharedPtr<FJsonObject> &Properties,
                             TArray<UeAgentAuthoringObjectApply::FPropertyIntent>& OutIntents,
                             TArray<FRendererBindingIntent>& OutBindings,
                             UE::UAI::Contract::FDiagnostic& OutError) {
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
  TArray<TSharedPtr<FJsonObject>> BindingDescriptors;
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
        OutError = AssetValueFailure(TEXT("niagara_renderer_enabled_descriptor_duplicate"),
            TEXT("The renderer enabled property occurs more than once in the property changes."), TEXT("bIsEnabled.descriptor_count"),
            MakeShared<FJsonValueNumber>(2), MakeShared<FJsonValueNumber>(1));
        UeAgentAuthoringDiagnostics::LocatePropertyValueSource(OutError, *Descriptor);
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

    BindingDescriptors.Add(Descriptor);
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
    if (!UeAgentDetailsAuthoringService::ApplyObjectProperties(&Renderer, GenericProperties, Options, Result)) {
      OutError = Result.Diagnostic;
      return false;
    }
    OutIntents.Append(MoveTemp(Result.PropertyIntents));
  }
  // SourceMode and other native prerequisites must settle before SetValue.
  for (const auto& Descriptor : BindingDescriptors) {
    const FString Path = Descriptor->GetStringField(TEXT("property_path"));
    auto* Property = FindFProperty<FStructProperty>(Renderer.GetClass(), *Path);
    FRendererBindingIntent Intent;
    Intent.PropertyName = Property->GetFName();
    FName Name;
    UeAgentAuthoringObjectApply::FFailure Failure;
    if (!UeAgentNiagaraRendererBindings::PrepareAuthoringBinding(*Property, Descriptor,
        Renderer, VersionedEmitter, Name, Intent.Intent, Failure)) {
      OutError = Failure.Diagnostic; return false;
    }
    Renderer.PreEditChange(Property);
    Property->ContainerPtrToValuePtr<FNiagaraVariableAttributeBinding>(&Renderer)->SetValue(Name, VersionedEmitter, Renderer.GetCurrentSourceMode());
    FPropertyChangedEvent Event(Property, EPropertyChangeType::ValueSet);
    Renderer.PostEditChangeProperty(Event);
    OutBindings.Add(MoveTemp(Intent));
  }
  if (Enabled.IsSet())
    Renderer.SetIsEnabled(Enabled.GetValue());
  return true;
}

bool ApplyEmitterProperties(FVersionedNiagaraEmitterData &EmitterData,
                            UNiagaraEmitter &Emitter,
                            const TSharedPtr<FJsonObject> &Properties,
                            const bool bCompactValues,
                            TArray<UeAgentAuthoringObjectApply::FPropertyIntent>& OutIntents,
                            UE::UAI::Contract::FDiagnostic& OutError) {
  if (!Properties.IsValid())
    return true;
  UeAgentAuthoringObjectApply::FOptions Options;
  Options.bAllowDomainAuthorizedOwners = true;
  Options.bMarkPackageDirty = false;
  Options.bCompactValues = bCompactValues;
  UeAgentAuthoringDirectApply::FResult Result;
  if (!UeAgentDetailsAuthoringService::ApplyStructProperties(FVersionedNiagaraEmitterData::StaticStruct(), &EmitterData, &Emitter, Properties, Options, Result)) {
      OutError = Result.Diagnostic;
    return false;
  }
  OutIntents.Append(MoveTemp(Result.PropertyIntents));
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
                              UE::UAI::Contract::FDiagnostic& OutError) {
  OutProperties.Reset();
  ON_SCOPE_EXIT {
    SupplementEmitterPatchFailure(OutError, Patch, Patch.SourceObject.Get(), TEXT("properties"));
  };
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
      OutError = AssetValueFailure(TEXT("property_descriptor_invalid"),
          TEXT("Capturing emitter properties requires an object descriptor with a property path."), TEXT("property_path"),
          Descriptor.IsValid() ? Descriptor->TryGetField(TEXT("property_path")) : DescriptorValue,
          MakeShared<FJsonValueString>(TEXT("property path string")));
      if (Descriptor.IsValid()) UeAgentAuthoringDiagnostics::LocatePropertyValueSource(OutError, *Descriptor);
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

bool IsRendererUsedByEmitter(UNiagaraEmitter& Emitter, const UNiagaraRendererProperties* Renderer) {
  for (const FNiagaraAssetVersion& Version : Emitter.GetAllAvailableVersions()) {
    const auto* Data = Emitter.GetEmitterData(Version.VersionGuid);
    if (Data && Data->GetRenderers().Contains(Renderer)) return true;
  }
  return false;
}

bool RetireDetachedRenderer(UNiagaraRendererProperties& Renderer, UE::UAI::Contract::FDiagnostic& OutError) {
  const FName RetiredName = MakeUniqueObjectName(GetTransientPackage(), Renderer.GetClass(),
      FName(*(Renderer.GetName() + TEXT("_Retired"))));
  if (!Renderer.Rename(*RetiredName.ToString(), GetTransientPackage(),
      REN_DontCreateRedirectors | REN_NonTransactional | REN_ForceNoResetLoaders)) {
    OutError = NativeAssetFailure(TEXT("niagara_renderer_retire_failed"),
        TEXT("The detached renderer could not be moved to the transient package."), &Renderer,
        TEXT("renderer.retired"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
    OutError.CandidateName = Renderer.GetName();
    return false;
  }
  return true;
}

bool RemoveRendererForEdit(UNiagaraEmitter& Emitter, const FGuid& Version,
                          UNiagaraRendererProperties& Renderer, UE::UAI::Contract::FDiagnostic& OutError) {
  Emitter.RemoveRenderer(&Renderer, Version);
  // Native versions normally duplicate their renderers. Still preserve a renderer
  // if another version references it rather than moving that version's object.
  return IsRendererUsedByEmitter(Emitter, &Renderer) || RetireDetachedRenderer(Renderer, OutError);
}

bool PrepareRendererObjectName(UNiagaraEmitter& Emitter, FName Name, UE::UAI::Contract::FDiagnostic& OutError) {
  UObject* ExistingObject = FindObjectFast<UObject>(&Emitter, Name);
  if (!ExistingObject) return true;
  auto* Renderer = Cast<UNiagaraRendererProperties>(ExistingObject);
  if (!Renderer || IsRendererUsedByEmitter(Emitter, Renderer)) {
    OutError = NativeAssetFailure(TEXT("niagara_renderer_name_in_use"),
        TEXT("The requested renderer name is occupied by another object or a renderer still used by an emitter version."),
        ExistingObject, TEXT("renderer_name.available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
    OutError.CandidateName = Name.ToString();
    return false;
  }
  return RetireDetachedRenderer(*Renderer, OutError);
}

bool ApplyEmitterStructurePatches(TArray<FEmitterStructurePatch> &Patches,
                                  TArray<FEmitterPropertyIntent>& OutIntents,
                                  UE::UAI::Contract::FDiagnostic& OutError) {
  using namespace UeAgentNiagaraStageModuleAuthoring;
  for (FEmitterStructurePatch &Patch : Patches) {
    bool bStepSucceeded = false;
    ON_SCOPE_EXIT {
      if (!bStepSucceeded)
        SupplementEmitterPatchFailure(OutError, Patch, Patch.SourceObject.Get(), TEXT("target"));
    };
    if (Patch.bRemoveEmitterHandle) {
      UNiagaraSystem* System = Patch.TargetSystem.Get();
      const int32 Index = System ? System->GetEmitterHandles().IndexOfByPredicate(
          [&Patch](const FNiagaraEmitterHandle& Handle) {
            return Handle.GetId() == Patch.Owner.EmitterHandleId;
          }) : INDEX_NONE;
      if (!System || Index == INDEX_NONE) {
        OutError = NativeAssetFailure(TEXT("niagara_workspace_emitter_remove_handle_missing"),
            TEXT("The system or selected emitter handle is unavailable during removal."), System,
            System ? TEXT("emitter_handle.available") : TEXT("system.available"),
            MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
        return false;
      }
      System->Modify();
      Patch.RemovedHandle = System->GetEmitterHandles()[Index];
      Patch.RemovedHandleIndex = Index;
      Patch.RemovedEmitterInstance.Reset(Patch.Owner.Emitter);
      UeAgentNiagaraOps::RemoveEmitterHandlesForStructuralEdit(
          *System, {Patch.Owner.EmitterHandleId});
      bStepSucceeded = true;
      continue;
    }
    if (!Patch.bAddEmitterHandle) {
      bStepSucceeded = true;
      continue;
    }
    UNiagaraSystem *System = Patch.TargetSystem.Get();
    UNiagaraEmitter *SourceEmitter = Patch.SourceEmitter.Get();
    if (!System || !SourceEmitter || !Patch.SourceEmitterVersion.IsValid()) {
      OutError = NativeAssetFailure(TEXT("niagara_workspace_emitter_handle_add_state_invalid"),
          TEXT("Adding an emitter handle requires the system, source emitter, and a valid source version."), System,
          !System ? TEXT("system.available") : !SourceEmitter ? TEXT("source_emitter.available") : TEXT("source_version.valid"),
          MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
      return false;
    }
    // Keep the requested handle name while sharing the editor's instance
    // teardown before changing the system's emitter storage.
    UeAgentNiagaraOps::ReleaseSystemInstancesForEmitterHandleEdit(*System);
    System->Modify();
    UeAgentNiagaraRendererBindings::Refresh(
        FVersionedNiagaraEmitter(SourceEmitter, Patch.SourceEmitterVersion));
    const FNiagaraEmitterHandle Added = System->AddEmitterHandle(
        *SourceEmitter, *Patch.Target.EmitterName, Patch.SourceEmitterVersion);
    if (!Added.GetId().IsValid()) {
      OutError = NativeAssetFailure(TEXT("niagara_workspace_emitter_handle_add_failed"),
          TEXT("Unreal did not return a valid handle after adding the emitter."), System,
          TEXT("new_emitter_handle.valid"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
      return false;
    }
    Patch.AddedHandleId = Added.GetId();
    Patch.bAddedEmitterHandle = true;
    Patch.Target.EmitterHandleId = Added.GetId();
    Patch.Target.bEmitterHandleIdExplicit = true;
    if (!ResolveEmitterOwner(Patch.Target, Patch.Owner, OutError, System))
      return false;
    bStepSucceeded = true;
  }
  for (FEmitterStructurePatch &Patch : Patches) {
    if (Patch.bRemoveEmitterHandle) continue;
    if (!Patch.bNeedsMutation || !Patch.Owner.Emitter ||
        !Patch.Owner.EmitterData)
      continue;
    const FJsonObject* FailureObject = Patch.SourceObject.Get();
    FString FailureField = TEXT("source_asset_path");
    bool bPatchSucceeded = false;
    ON_SCOPE_EXIT {
      if (!bPatchSucceeded) SupplementEmitterPatchFailure(OutError, Patch, FailureObject, FailureField);
    };
    if (Patch.bReplaceEmitterFromSource) {
      UNiagaraSystem *System = Patch.TargetSystem.Get();
      UNiagaraEmitter *SourceEmitter = Patch.SourceEmitter.Get();
      if (!System || !SourceEmitter || !Patch.SourceEmitterVersion.IsValid()) {
        OutError = NativeAssetFailure(TEXT("niagara_workspace_emitter_source_replace_state_invalid"),
            TEXT("Replacing an emitter from its source requires the system, source emitter, and valid source version."), System,
            !System ? TEXT("system.available") : !SourceEmitter ? TEXT("source_emitter.available") : TEXT("source_version.valid"),
            MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
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
        OutError = NativeAssetFailure(TEXT("niagara_workspace_emitter_source_replace_handle_missing"),
            TEXT("The emitter handle to replace is no longer present in the system."), System,
            TEXT("emitter_handle.available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
        return false;
      }

      System->Modify();
      const FName ReplacementObjectName = MakeUniqueObjectName(
          System, UNiagaraEmitter::StaticClass(),
          FName(*FString::Printf(TEXT("%s_SourceInstance"),
                                 *Patch.Target.EmitterName)));
      UeAgentNiagaraRendererBindings::Refresh(
          FVersionedNiagaraEmitter(SourceEmitter, Patch.SourceEmitterVersion));
      UNiagaraEmitter *Replacement = UNiagaraEmitter::CreateWithParentAndOwner(
          FVersionedNiagaraEmitter(SourceEmitter, Patch.SourceEmitterVersion),
          System, ReplacementObjectName, ~(RF_Public | RF_Standalone));
      if (!Replacement) {
        OutError = NativeAssetFailure(TEXT("niagara_workspace_emitter_source_replace_failed"),
            TEXT("Unreal could not create the replacement emitter instance from its source."), System,
            TEXT("replacement_emitter.available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
        return false;
      }
      Replacement->SetUniqueEmitterName(Patch.Target.EmitterName);
      UeAgentNiagaraOps::ReleaseSystemInstancesForEmitterHandleEdit(*System);
      Handle->GetInstance() =
          FVersionedNiagaraEmitter(Replacement, Patch.SourceEmitterVersion);
      Patch.bReplacedEmitterFromSource = true;
      if (!ResolveEmitterOwner(Patch.Target, Patch.Owner, OutError, System))
        return false;
    }
    if (Patch.Enabled.IsSet()) {
      FailureField = TEXT("enabled");
      UNiagaraSystem *System = Patch.TargetSystem.Get();
      FNiagaraEmitterHandle *Handle =
          System
              ? System->GetEmitterHandles().FindByPredicate(
                    [&Patch](const FNiagaraEmitterHandle &Candidate) {
                      return Candidate.GetId() == Patch.Owner.EmitterHandleId;
                    })
              : nullptr;
      if (!System || !Handle) {
        OutError = NativeAssetFailure(TEXT("niagara_workspace_emitter_enabled_apply_handle_missing"),
            TEXT("The system or emitter handle is unavailable while applying enabled."), System,
            System ? TEXT("emitter_handle.available") : TEXT("system.available"),
            MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
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
      FailureField = TEXT("source_asset_path");
      UeAgentNiagaraRendererBindings::PrepareForMerge(Emitter);
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
        FString MergeStatus;
        switch (Result.MergeResult) {
        case INiagaraMergeManager::EMergeEmitterResult::FailedToDiff: MergeStatus = TEXT("FailedToDiff"); break;
        case INiagaraMergeManager::EMergeEmitterResult::FailedToMerge: MergeStatus = TEXT("FailedToMerge"); break;
        case INiagaraMergeManager::EMergeEmitterResult::None: MergeStatus = TEXT("None"); break;
        default: MergeStatus = FString::Printf(TEXT("Unknown (%d)"), static_cast<int32>(Result.MergeResult)); break;
        }
        OutError = NativeAssetFailure(TEXT("niagara_workspace_emitter_source_merge_failed"),
            TEXT("Unreal could not merge changes from the emitter's parent asset."), &Emitter,
            TEXT("source_merge.status"), MakeShared<FJsonValueString>(MergeStatus),
            MakeShared<FJsonValueString>(TEXT("SucceededNoDifferences | SucceededDifferencesApplied")));
        const FString NativeReason = Result.GetErrorMessagesString();
        if (!NativeReason.IsEmpty()) {
          auto Cause = UE::UAI::Contract::FDiagnostic::Failure(TEXT("niagara_native_emitter_merge_failed"),
              NativeReason, TEXT("niagara_authoring"));
          Cause.SourceKind = TEXT("native_result");
          OutError.PublicCause = MakeShared<UE::UAI::Contract::FDiagnostic>(MoveTemp(Cause));
        }
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
        OutError = NativeAssetFailure(TEXT("niagara_workspace_emitter_source_merge_data_missing"),
            TEXT("The selected emitter version data is unavailable after merging the parent changes."), &Emitter,
            TEXT("emitter.version_data.available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
        return false;
      }
    }
    FailureField = TEXT("properties");
    FEmitterPropertyIntent EmitterIntent;
    EmitterIntent.Target = Patch.Target;
    if (!ApplyEmitterProperties(*Patch.Owner.EmitterData, Emitter,
                                Patch.Properties, Patch.bCompactValues,
                                EmitterIntent.Properties, OutError))
      return false;
    if (!EmitterIntent.Properties.IsEmpty()) OutIntents.Add(MoveTemp(EmitterIntent));

    if (Patch.bReplaceRenderers) {
      FailureField = TEXT("renderers");
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
          if (!RemoveRendererForEdit(Emitter, Patch.Owner.EmitterVersion, *CurrentRenderer, OutError)) return false;
        }
      }
      Patch.Owner.EmitterData =
          Emitter.GetEmitterData(Patch.Owner.EmitterVersion);
      if (!Patch.Owner.EmitterData) {
        OutError = NativeAssetFailure(TEXT("niagara_workspace_renderer_collection_data_missing"),
            TEXT("The selected emitter version data is unavailable while replacing its renderer collection."), &Emitter,
            TEXT("emitter.version_data.available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
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
        TArray<TSharedPtr<FJsonValue>> RemainingNames;
        for (const auto& Name : UnexpectedRendererNames) RemainingNames.Add(MakeShared<FJsonValueString>(Name));
        OutError = NativeAssetFailure(TEXT("niagara_workspace_renderer_collection_replace_failed"),
            TEXT("Renderers outside the requested final collection remain after removal."), &Emitter,
            TEXT("renderers.unexpected_names"), MakeShared<FJsonValueArray>(RemainingNames),
            MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>()));
        return false;
      }
    }

    for (const FRendererPatch &RendererPatch : Patch.Renderers) {
      FailureObject = RendererPatch.SourceObject.IsValid() ? RendererPatch.SourceObject.Get() : RendererPatch.SourceOwner.Get();
      FailureField = RendererPatch.SourceObject.IsValid() ? TEXT("renderer_class") : RendererPatch.SourceKey;
      ON_SCOPE_EXIT {
        if (OutError.IsValid() && OutError.CandidateName.IsEmpty())
          OutError.CandidateName = RendererPatch.RendererName;
      };
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
        if (Existing && !RemoveRendererForEdit(Emitter, Patch.Owner.EmitterVersion, *Existing, OutError)) return false;
        continue;
      }
      UClass *DesiredClass =
          RendererPatch.RendererClass.IsEmpty()
              ? (Existing ? Existing->GetClass() : nullptr)
              : ResolveRendererClass(RendererPatch.RendererClass);
      if (!DesiredClass) {
        OutError = AssetValueFailure(TEXT("niagara_workspace_renderer_class_invalid"),
            TEXT("The renderer class could not be resolved, and no existing renderer supplies its class."),
            TEXT("renderer_class"), RendererPatch.SourceObject.IsValid()
                ? RendererPatch.SourceObject->TryGetField(TEXT("renderer_class")) : TSharedPtr<FJsonValue>(),
            MakeShared<FJsonValueString>(TEXT("supported Niagara renderer class or existing renderer")));
        return false;
      }
      if (Existing && Existing->GetClass() != DesiredClass) {
        if (!RemoveRendererForEdit(Emitter, Patch.Owner.EmitterVersion, *Existing, OutError)) return false;
        Existing = nullptr;
      }
      if (!Existing) {
        if (!PrepareRendererObjectName(Emitter, FName(*RendererPatch.RendererName), OutError)) return false;
        Existing = NewObject<UNiagaraRendererProperties>(
            &Emitter, DesiredClass, FName(*RendererPatch.RendererName),
            RF_Transactional);
        if (!Existing) {
          OutError = NativeAssetFailure(TEXT("niagara_workspace_renderer_create_failed"),
              TEXT("Unreal could not create the requested renderer instance."), &Emitter,
              TEXT("renderer.available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
          return false;
        }
        Emitter.AddRenderer(Existing, Patch.Owner.EmitterVersion);
      }
      Existing->Modify();
      FEmitterPropertyIntent RendererIntent;
      RendererIntent.Target = Patch.Target;
      RendererIntent.RendererName = Existing->GetName();
      FailureField = TEXT("properties");
      if (!ApplyRendererProperties(*Existing, Emitter,
                                   Patch.Owner.EmitterVersion,
                                   RendererPatch.Properties, RendererIntent.Properties, RendererIntent.Bindings, OutError))
        return false;
      if (!RendererIntent.Properties.IsEmpty() || !RendererIntent.Bindings.IsEmpty()) OutIntents.Add(MoveTemp(RendererIntent));
      if (RendererPatch.TargetIndex != INDEX_NONE) {
        Emitter.MoveRenderer(Existing, RendererPatch.TargetIndex,
                             Patch.Owner.EmitterVersion);
      }
    }
    FailureObject = Patch.SourceObject.Get();
    FailureField = TEXT("renderers");
    UeAgentNiagaraRendererBindings::Refresh(
        FVersionedNiagaraEmitter(&Emitter, Patch.Owner.EmitterVersion));
    if (Patch.bReplaceRenderers) {
      Patch.Owner.EmitterData =
          Emitter.GetEmitterData(Patch.Owner.EmitterVersion);
      if (!Patch.Owner.EmitterData) {
        OutError = NativeAssetFailure(TEXT("niagara_workspace_renderer_collection_data_missing"),
            TEXT("The selected emitter version data is unavailable while replacing its renderer collection."), &Emitter,
            TEXT("emitter.version_data.available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
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
        OutError = NativeAssetFailure(TEXT("niagara_workspace_renderer_collection_replace_postcondition_failed"),
            TEXT("The resulting renderer count does not match the requested final collection."), &Emitter,
            TEXT("renderers.count"), MakeShared<FJsonValueNumber>(AppliedRenderers.Num()),
            MakeShared<FJsonValueNumber>(ExpectedRendererNames.Num()));
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
          TArray<TSharedPtr<FJsonValue>> ExpectedNames;
          for (const auto& Name : ExpectedRendererNames) ExpectedNames.Add(MakeShared<FJsonValueString>(Name));
          OutError = NativeAssetFailure(TEXT("niagara_workspace_renderer_collection_replace_postcondition_failed"),
              TEXT("A resulting renderer is missing or is not a member of the requested final collection."), &Emitter,
              FString::Printf(TEXT("renderers[%d].renderer_name"), Index),
              AppliedRenderer ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(AppliedRenderer->GetName())) : TSharedPtr<FJsonValue>(),
              MakeShared<FJsonValueArray>(ExpectedNames));
          OutError.OperationIndex = Index;
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
          const auto* ActualRenderer = AppliedRenderers.IsValidIndex(RendererPatch.TargetIndex)
              ? AppliedRenderers[RendererPatch.TargetIndex] : nullptr;
          OutError = NativeAssetFailure(TEXT("niagara_workspace_renderer_collection_replace_postcondition_failed"),
              TEXT("The renderer at the requested index does not match the requested renderer name."), &Emitter,
              FString::Printf(TEXT("renderers[%d].renderer_name"), RendererPatch.TargetIndex),
              ActualRenderer ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(ActualRenderer->GetName())) : TSharedPtr<FJsonValue>(),
              MakeShared<FJsonValueString>(RendererPatch.RendererName));
          OutError.OperationIndex = RendererPatch.TargetIndex;
          OutError.CandidateName = RendererPatch.RendererName;
          return false;
        }
      }
    }

    for (const FEventHandlerPatch &EventPatch : Patch.EventHandlers) {
      FailureObject = EventPatch.SourceObject.IsValid() ? EventPatch.SourceObject.Get() : EventPatch.SourceOwner.Get();
      FailureField = EventPatch.SourceObject.IsValid() ? FString() : EventPatch.SourceKey;
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
            UE::UAI::Contract::FDiagnostic RemoveError;
            if (!RemoveStageGraph(*Patch.Owner.Graph,
                                  ENiagaraScriptUsage::ParticleEventScript,
                                  EventPatch.UsageId, RemoveError)
                && RemoveError.Code != TEXT("niagara_stage_not_found")) {
              OutError = MoveTemp(RemoveError);
              return false;
            }
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
          OutError = NativeAssetFailure(TEXT("niagara_workspace_event_script_create_failed"),
              TEXT("Unreal could not create the Niagara event handler script."), &Emitter,
              TEXT("event_handler.script.available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
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
        if (!Patch.Owner.Graph) {
          OutError = NativeAssetFailure(TEXT("niagara_workspace_event_graph_missing"),
              TEXT("The emitter graph is unavailable while creating the event stage."), &Emitter,
              TEXT("event_handler.graph.available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
          return false;
        }
        if (!EnsureStageBridge(*Patch.Owner.Graph,
                               ENiagaraScriptUsage::ParticleEventScript,
                               EventPatch.UsageId, OutError))
          return false;
      }
      if (!Existing) {
        OutError = NativeAssetFailure(TEXT("niagara_workspace_event_handler_resolve_failed"),
            TEXT("The event handler is unavailable after its script was added to the emitter."), &Emitter,
            TEXT("event_handler.available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
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
    bPatchSucceeded = true;
  }
  return true;
}

bool RollbackAddedEmitterHandles(TArray<FEmitterStructurePatch> &Patches,
                                 UE::UAI::Contract::FDiagnostic& OutError) {
  OutError.Reset();
  for (int32 PatchIndex = Patches.Num() - 1; PatchIndex >= 0; --PatchIndex) {
    FEmitterStructurePatch &Patch = Patches[PatchIndex];
    if (!Patch.bAddedEmitterHandle || !Patch.AddedHandleId.IsValid())
      continue;
    UNiagaraSystem *System = Patch.TargetSystem.Get();
    if (!System) {
      OutError = NativeAssetFailure(TEXT("niagara_workspace_emitter_handle_rollback_system_missing"),
          TEXT("The system is unavailable while rolling back the added emitter handle."), System,
          TEXT("system.available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
      SupplementEmitterPatchFailure(OutError, Patch);
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
      UeAgentNiagaraOps::RemoveEmitterHandlesForStructuralEdit(
          *System, {Patch.AddedHandleId});
    Patch.bAddedEmitterHandle = false;
    Patch.AddedHandleId.Invalidate();
  }
  return true;
}

bool RollbackReplacedEmitterHandles(TArray<FEmitterStructurePatch> &Patches,
                                    UE::UAI::Contract::FDiagnostic& OutError) {
  OutError.Reset();
  for (int32 PatchIndex = Patches.Num() - 1; PatchIndex >= 0; --PatchIndex) {
    FEmitterStructurePatch &Patch = Patches[PatchIndex];
    if (!Patch.bReplacedEmitterFromSource)
      continue;
    UNiagaraSystem *System = Patch.TargetSystem.Get();
    UNiagaraEmitter *PreviousEmitter = Patch.PreviousEmitterInstance.Get();
    if (!System || !PreviousEmitter ||
        !Patch.PreviousEmitterVersion.IsValid()) {
      OutError = NativeAssetFailure(TEXT("niagara_workspace_emitter_source_replace_rollback_state_invalid"),
          TEXT("Emitter replacement rollback requires the system, captured emitter instance, and its valid version."), System,
          !System ? TEXT("system.available") : !PreviousEmitter ? TEXT("previous_emitter.available") : TEXT("previous_version.valid"),
          MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
      SupplementEmitterPatchFailure(OutError, Patch);
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
      OutError = NativeAssetFailure(TEXT("niagara_workspace_emitter_source_replace_rollback_handle_missing"),
          TEXT("The emitter handle is unavailable while restoring its captured instance."), System,
          TEXT("emitter_handle.available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
      SupplementEmitterPatchFailure(OutError, Patch);
      return false;
    }
    UeAgentNiagaraOps::ReleaseSystemInstancesForEmitterHandleEdit(*System);
    System->Modify();
    Handle->GetInstance() =
        FVersionedNiagaraEmitter(PreviousEmitter, Patch.PreviousEmitterVersion);
    Patch.bReplacedEmitterFromSource = false;
  }
  return true;
}

bool RollbackRemovedEmitterHandles(TArray<FEmitterStructurePatch>& Patches,
                                  UE::UAI::Contract::FDiagnostic& OutError) {
  for (int32 Index = Patches.Num() - 1; Index >= 0; --Index) {
    FEmitterStructurePatch& Patch = Patches[Index];
    if (!Patch.RemovedHandle.IsSet()) continue;
    UNiagaraSystem* System = Patch.TargetSystem.Get();
    if (!System || !Patch.RemovedEmitterInstance.IsValid()) {
      OutError = NativeAssetFailure(TEXT("niagara_workspace_emitter_remove_rollback_state_invalid"),
          TEXT("Emitter removal rollback requires the system and captured emitter instance."), System,
          System ? TEXT("removed_emitter.available") : TEXT("system.available"),
          MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
      SupplementEmitterPatchFailure(OutError, Patch);
      return false;
    }
    UeAgentNiagaraOps::ReleaseSystemInstancesForEmitterHandleEdit(*System);
    System->Modify();
    System->GetEmitterHandles().Insert(Patch.RemovedHandle.GetValue(),
        FMath::Clamp(Patch.RemovedHandleIndex, 0, System->GetEmitterHandles().Num()));
    System->RefreshSystemParametersFromEmitter(Patch.RemovedHandle.GetValue());
    System->InvalidateCachedData();
    Patch.RemovedHandle.Reset();
    Patch.RemovedEmitterInstance.Reset();
  }
  return true;
}

bool RollbackEmitterEnabledStates(TArray<FEmitterStructurePatch> &Patches,
                                  UE::UAI::Contract::FDiagnostic& OutError) {
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
      OutError = NativeAssetFailure(TEXT("niagara_workspace_emitter_enabled_rollback_handle_missing"),
          TEXT("The system or emitter handle is unavailable while restoring the enabled state."), System,
          System ? TEXT("emitter_handle.available") : TEXT("system.available"),
          MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
      SupplementEmitterPatchFailure(OutError, Patch);
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
    TArray<FEmitterStructureBackup> &OutBackups, UE::UAI::Contract::FDiagnostic& OutError) {
  OutBackups.Reset();
  for (const FEmitterStructurePatch &Patch : Patches) {
    if (!Patch.bNeedsMutation || !Patch.Owner.Emitter ||
        !Patch.Owner.EmitterData)
      continue;
    ON_SCOPE_EXIT {
      SupplementEmitterPatchFailure(OutError, Patch, Patch.SourceObject.Get(), TEXT("properties"));
    };
    FEmitterStructureBackup &Backup = OutBackups.AddDefaulted_GetRef();
    Backup.Target = Patch.Target;
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
        OutError = NativeAssetFailure(TEXT("niagara_renderer_rollback_capture_failed"),
            TEXT("A renderer snapshot could not be captured before the emitter change."), Renderer,
            Renderer ? TEXT("renderer.snapshot.available") : TEXT("renderer.available"),
            MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
        OutError.OperationIndex = Backup.Renderers.Num();
        if (Renderer) OutError.CandidateName = Renderer->GetName();
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
                                    UE::UAI::Contract::FDiagnostic& OutError) {
  for (FEmitterStructureBackup &Backup : Backups) {
    ON_SCOPE_EXIT {
      if (OutError.IsValid()) {
        if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Backup.Target.AssetPath;
        if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = Backup.Target.EmitterName;
      }
    };
    UNiagaraEmitter *Emitter = Backup.Emitter.Get();
    FVersionedNiagaraEmitterData *Data =
        Emitter ? Emitter->GetEmitterData(Backup.EmitterVersion) : nullptr;
    if (!Emitter || !Data) {
      OutError = NativeAssetFailure(TEXT("niagara_emitter_rollback_target_missing"),
          TEXT("Emitter rollback requires the emitter instance and its selected version data."), Emitter,
          Emitter ? TEXT("emitter.version_data.available") : TEXT("emitter.available"),
          MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
      return false;
    }
    TArray<UeAgentAuthoringObjectApply::FPropertyIntent> RestoredIntents;
    if (!ApplyEmitterProperties(*Data, *Emitter, Backup.Properties, false, RestoredIntents, OutError))
      return false;
    const TArray<UNiagaraRendererProperties *> CurrentRenderers =
        Data->GetRenderers();
    for (UNiagaraRendererProperties *Renderer : CurrentRenderers) {
      if (Renderer && !RemoveRendererForEdit(*Emitter, Backup.EmitterVersion, *Renderer, OutError)) return false;
    }

    for (const FRendererBackup &RendererBackup : Backup.Renderers) {
      if (!PrepareRendererObjectName(*Emitter, RendererBackup.OriginalName, OutError)) return false;
      UNiagaraRendererProperties *Restored =
          RendererBackup.Snapshot.IsValid()
              ? DuplicateObject<UNiagaraRendererProperties>(
                    RendererBackup.Snapshot.Get(), Emitter,
                    RendererBackup.OriginalName)
              : nullptr;
      if (!Restored) {
        OutError = NativeAssetFailure(TEXT("niagara_renderer_rollback_restore_failed"),
            TEXT("The captured renderer could not be restored to the emitter."), Emitter,
            RendererBackup.Snapshot.IsValid() ? TEXT("restored_renderer.available") : TEXT("renderer.snapshot.available"),
            MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
        OutError.CandidateName = RendererBackup.OriginalName.ToString();
        return false;
      }
      Emitter->AddRenderer(Restored, Backup.EmitterVersion);
    }
    UeAgentNiagaraRendererBindings::Refresh(
        FVersionedNiagaraEmitter(Emitter, Backup.EmitterVersion));
    Data->EventHandlerScriptProps = Backup.EventHandlers;
    Emitter->MarkPackageDirty();
  }
  return true;
}

UE::UAI::Contract::FDiagnostic AssignmentPostconditionFailure(
    const TArray<FNiagaraVariable>& ExpectedTargets, const TArray<FNiagaraVariable>& ActualTargets,
    const int32 ModuleIndex, const bool bNormalizeTargetNames) {
  auto ProjectTargets = [bNormalizeTargetNames](const TArray<FNiagaraVariable>& Targets) -> TSharedPtr<FJsonValue> {
    struct FProjectedTarget {
      FString Name;
      FString TypePath;
      uint32 TypeFlags = 0;
    };
    TArray<FProjectedTarget> Values;
    for (const FNiagaraVariable& Target : Targets) {
      auto& Value = Values.AddDefaulted_GetRef();
      Value.Name = Target.GetName().ToString();
      if (bNormalizeTargetNames)
        Value.Name = UeAgentNiagaraStageModuleAuthoring::NormalizeAssignmentTargetName(Value.Name);
      Value.Name.ToLowerInline();
      const FNiagaraTypeDefinition& Type = Target.GetType();
      const UObject* TypeObject = Type.IsEnum() ? static_cast<const UObject*>(Type.GetEnum()) : Type.GetStruct();
      Value.TypePath = TypeObject ? TypeObject->GetPathName() : TEXT("Invalid");
      Value.TypeFlags = Type.GetFlags();
    }
    Values.Sort([](const FProjectedTarget& Left, const FProjectedTarget& Right) {
      if (Left.Name != Right.Name) return Left.Name < Right.Name;
      if (Left.TypePath != Right.TypePath) return Left.TypePath < Right.TypePath;
      return Left.TypeFlags < Right.TypeFlags;
    });
    TArray<TSharedPtr<FJsonValue>> Result;
    for (const auto& Value : Values) {
      auto Entry = MakeShared<FJsonObject>();
      Entry->SetStringField(TEXT("name"), Value.Name);
      auto Type = MakeShared<FJsonObject>();
      Type->SetStringField(TEXT("path"), Value.TypePath);
      Type->SetNumberField(TEXT("flags"), Value.TypeFlags);
      Entry->SetObjectField(TEXT("type"), Type);
      Result.Add(MakeShared<FJsonValueObject>(Entry));
    }
    return MakeShared<FJsonValueArray>(Result);
  };
  auto Error = NativeAssetFailure(TEXT("niagara_workspace_stage_replace_postcondition_failed"),
      TEXT("The resulting assignment parameters do not match the requested parameter names and types."),
      nullptr, TEXT("assignment_targets.native_equivalent"),
      MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
  Error.OperationIndex = ModuleIndex;
  const auto Difference = UE::UAI::StateSemantic::CanonicalJson::CompareJsonValues(
      ProjectTargets(ExpectedTargets), ProjectTargets(ActualTargets), TEXT("/assignment_targets"));
  if (!Difference.bEquivalent) {
    Difference.ApplyTo(Error);
    Error.PropertyPath = Difference.Path;
  }
  return Error;
}

bool ApplyStagePatches(TArray<FStagePatch> &StagePatches, UE::UAI::Contract::FDiagnostic& OutError) {
  using namespace UeAgentNiagaraStageModuleAuthoring;
  FUeAgentStructuredResourceTracker DynamicInputResourceTracker;
  for (FStagePatch &StagePatch : StagePatches) {
    if (!StagePatch.bNeedsMutation)
      continue;
    FResolvedStage &Stage = StagePatch.Stage;
    const FJsonObject* FailureObject = StagePatch.SourceObject.Get();
    FString FailureField;
    bool bStageSucceeded = false;
    ON_SCOPE_EXIT {
      if (!bStageSucceeded && OutError.IsValid()) {
        if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = StagePatch.Target.AssetPath;
        if (FailureObject)
          UeAgentAuthoringDiagnostics::LocateFailure(OutError, *FailureObject,
              TEXT("mutation"), StagePatch.Target.AssetPath, FailureField);
      }
    };
    if (!Stage.Asset || !Stage.ScriptSource || !Stage.Graph ||
        !Stage.OutputNode) {
      OutError = NativeAssetFailure(TEXT("niagara_workspace_stage_context_invalid"),
          TEXT("The resolved Niagara stage no longer has all objects required for mutation."),
          Stage.Asset, !Stage.Asset ? TEXT("asset.available") : !Stage.ScriptSource ? TEXT("script_source.available")
              : !Stage.Graph ? TEXT("graph.available") : TEXT("output_node.available"),
          MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
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
      if (!GetModules(*Stage.OutputNode, CurrentLegacyModules, OutError)) return false;
      TArray<UNiagaraNode *> CurrentNativeAssignments;
      if (!UeAgentNiagaraStandaloneModuleAuthoring::GetAssignmentNodes(
              *Stage.OutputNode, CurrentNativeAssignments, OutError)) return false;
      TArray<FModulePatch *> LegacyMigrations =
          Upserts.FilterByPredicate([](const FModulePatch *Patch) {
            return Patch && Patch->bLegacyStandaloneNode &&
                   Patch->LegacyNodeGuid.IsValid();
          });
      if (!LegacyMigrations.IsEmpty()) {
        TSet<FGuid> CoveredLegacyGuids;
        for (const FModulePatch &Patch : StagePatch.Modules) {
          if (Patch.bRemove)
            continue;
          if (Patch.LegacyNodeGuid.IsValid())
            CoveredLegacyGuids.Add(Patch.LegacyNodeGuid);
        }
        bool bCoversEveryLegacyModule =
            CurrentLegacyModules.Num() == CoveredLegacyGuids.Num();
        for (const UNiagaraNodeFunctionCall *CurrentModule :
             CurrentLegacyModules) {
          bCoversEveryLegacyModule &=
              CurrentModule &&
              CoveredLegacyGuids.Contains(CurrentModule->NodeGuid);
        }
        const bool bLegacyOnlyRebuild = !LegacyMigrations.IsEmpty() &&
                                        CurrentNativeAssignments.IsEmpty() &&
                                        bCoversEveryLegacyModule;
        if (!Removals.IsEmpty() || !bLegacyOnlyRebuild) {
          auto Actual = MakeShared<FJsonObject>();
          Actual->SetNumberField(TEXT("remove_count"), Removals.Num());
          Actual->SetNumberField(TEXT("native_assignment_count"), CurrentNativeAssignments.Num());
          Actual->SetBoolField(TEXT("covers_every_legacy_module"), bCoversEveryLegacyModule);
          auto Expected = MakeShared<FJsonObject>();
          Expected->SetNumberField(TEXT("remove_count"), 0);
          Expected->SetNumberField(TEXT("native_assignment_count"), 0);
          Expected->SetBoolField(TEXT("covers_every_legacy_module"), true);
          OutError = AssetValueFailure(TEXT("niagara_standalone_structural_rebuild_requires_complete_stage"),
              TEXT("Migrating legacy standalone assignments requires the complete legacy-only stage and no simultaneous removals."),
              TEXT("stage_rebuild"), MakeShared<FJsonValueObject>(Actual), MakeShared<FJsonValueObject>(Expected));
          FailureField = TEXT("modules");
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

    TMap<const FModulePatch*, int32> RemovalIndices;
    for (const FModulePatch* Patch : Removals) {
      FModuleSelector Selector;
      Selector.NodeGuid = Patch->ModuleGuid;
      UNiagaraNodeFunctionCall* Node = nullptr;
      int32 Index = INDEX_NONE;
      UE::UAI::Contract::FDiagnostic Error;
      if (!ResolveModule(*Stage.OutputNode, Selector, Node, Index, Error)
          && Error.Code != TEXT("module_selector_not_found")) {
        OutError = MoveTemp(Error);
        FailureObject = Patch->SourceObject.Get();
        FailureField = TEXT("module_guid");
        return false;
      }
      RemovalIndices.Add(Patch, Index);
    }
    Removals.Sort([&RemovalIndices](const FModulePatch& Left, const FModulePatch& Right) {
      return RemovalIndices.FindChecked(&Left) > RemovalIndices.FindChecked(&Right);
    });
    for (FModulePatch *Patch : Removals) {
      FailureObject = Patch->SourceObject.Get();
      FailureField = TEXT("module_guid");
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
        if (OutError.Code != TEXT("module_selector_not_found")) return false;
        OutError.Reset();
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
      FailureObject = Patch->SourceObject.Get();
      FailureField.Reset();
      if (Patch->bStandaloneAssignment) {
        UNiagaraNode *AssignmentNode = nullptr;
        int32 CurrentIndex = INDEX_NONE;
        bool bExists =
            Patch->ModuleGuid.IsValid() &&
            UeAgentNiagaraStandaloneModuleAuthoring::ResolveAssignmentNode(
                *Stage.OutputNode, Patch->ModuleGuid, AssignmentNode,
                CurrentIndex, OutError);
        if (!bExists && Patch->ModuleGuid.IsValid()) {
          if (OutError.Code != TEXT("module_selector_not_found")) return false;
          OutError.Reset();
        }
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
          } else {
            if (OutError.Code != TEXT("module_selector_not_found")) return false;
            OutError.Reset();
          }
        } else if (bExists && Patch->bReplaceScript) {
          AssignmentNode = UeAgentNiagaraStandaloneModuleAuthoring::ReplaceAssignmentNode(
              *Stage.OutputNode, *AssignmentNode, Patch->AssignmentTargets,
              CurrentIndex, OutError);
          if (!AssignmentNode) return false;
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
          if (!UeAgentNiagaraStandaloneModuleAuthoring::GetAssignmentNodes(
                  *Stage.OutputNode, AssignmentNodes, OutError)) return false;
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
      auto LocateCandidateFailure = [&]() {
        if (Patch->SourceObject.IsValid())
          UeAgentAuthoringDiagnostics::LocateFailure(
              OutError, *Patch->SourceObject, TEXT("mutation"),
              StagePatch.Target.AssetPath, TEXT("candidate"));
      };
      if (!Patch->NativeCandidateName.IsEmpty()) {
        if (!Stage.Asset) {
          OutError = NativeAssetFailure(TEXT("niagara_native_candidate_target_missing"),
              TEXT("Native candidate execution requires an available stage asset."), nullptr,
              TEXT("asset.available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
          OutError.CandidateName = Patch->NativeCandidateName;
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
                    *Stage.Asset, Patch->NativeCandidateName,
                    CandidateContext, ResolvedCandidate, ResolvedContext,
                    CandidateError, nullptr, Patch->CandidateSelect)) {
          OutError = UeAgentAuthoringDiagnostics::CandidateFailure(CandidateError.Code.IsEmpty()
                  ? TEXT("niagara_native_candidate_unresolved")
                  : CandidateError.Code, Patch->NativeCandidateName, StagePatch.Target.AssetPath, ScriptUsageToString(Stage.Usage), TEXT("niagara.module.candidates"), CandidateError);
          LocateCandidateFailure();
          return false;
        }
        if (!Patch->Script.IsValid() ||
            !ResolvedCandidate.ScriptAssetPath.Equals(
                Patch->Script->GetPathName(), ESearchCase::CaseSensitive) ||
            ResolvedContext.StageKey != Stage.StageKey) {
          UeAgentAuthoringDiagnostics::FDiagnostic Failure;
          Failure.Code = TEXT("niagara_native_candidate_context_changed");
          Failure.Stage = TEXT("mutation");
          Failure.CandidateName = Patch->NativeCandidateName;
          Failure.CauseCode = !Patch->Script.IsValid()
                                  ? TEXT("planned_module_script_missing")
                              : ResolvedContext.StageKey != Stage.StageKey
                                  ? TEXT("candidate_stage_changed")
                                  : TEXT("candidate_script_changed");
          Failure.PublicMessage = !Patch->Script.IsValid()
              ? TEXT("The module update plan did not retain its script for candidate validation.")
              : TEXT("The resolved candidate script or stack differs from the module update plan.");
          Failure.SourceKind = TEXT("native_result");
          if (!Patch->Script.IsValid()) {
            Failure.PropertyPath = TEXT("planned_script.available");
            Failure.ActualValue = MakeShared<FJsonValueBoolean>(false);
            Failure.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
          } else if (ResolvedContext.StageKey != Stage.StageKey) {
            Failure.PropertyPath = TEXT("candidate.stage_matches_plan");
            Failure.ActualValue = MakeShared<FJsonValueBoolean>(false);
            Failure.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
            Failure.ActualEvidence = MakeShared<FJsonValueString>(ResolvedContext.StageKey);
            Failure.ExpectedEvidence = MakeShared<FJsonValueString>(Stage.StageKey);
          } else {
            Failure.PropertyPath = TEXT("candidate.script_asset_path");
            Failure.ActualValue = MakeShared<FJsonValueString>(ResolvedCandidate.ScriptAssetPath);
            Failure.ExpectedValue = MakeShared<FJsonValueString>(Patch->Script->GetPathName());
          }
          Failure.ActualValuePresent = true;
          Failure.ExpectedValuePresent = true;
          OutError = Failure;
          LocateCandidateFailure();
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
          OutError = NativeAssetFailure(TEXT("niagara_workspace_module_script_unresolved"),
              TEXT("The planned module script is no longer available for node creation."), Stage.Asset,
              TEXT("module_script.available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
          return nullptr;
        }
        if (Patch->NativeCandidateName.IsEmpty()) {
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
                    *Stage.Asset, Patch->NativeCandidateName,
                    CandidateContext, Patch->TargetIndex, PerformedCandidate,
                    PerformedContext, CreatedNode, CandidateError, Patch->CandidateSelect)) {
          OutError = UeAgentAuthoringDiagnostics::CandidateFailure(CandidateError.Code.IsEmpty()
                  ? TEXT("niagara_native_candidate_execute_failed")
                  : CandidateError.Code, Patch->NativeCandidateName, StagePatch.Target.AssetPath, ScriptUsageToString(Stage.Usage), TEXT("niagara.module.candidates"), CandidateError);
          LocateCandidateFailure();
          return nullptr;
        }
        return CreatedNode;
      };
      FModuleSelector Selector;
      Selector.NodeGuid = Patch->ModuleGuid;
      UNiagaraNodeFunctionCall *Module = nullptr;
      int32 CurrentIndex = INDEX_NONE;
      UE::UAI::Contract::FDiagnostic ExistingResolveError;
      const bool bExists = Patch->ModuleGuid.IsValid() &&
                           ResolveModule(*Stage.OutputNode, Selector, Module,
                                         CurrentIndex, ExistingResolveError);
      if (!bExists && Patch->ModuleGuid.IsValid()
          && ExistingResolveError.Code != TEXT("module_selector_not_found")) {
        OutError = ExistingResolveError;
        return false;
      }
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
        OutError = NativeAssetFailure(TEXT("niagara_workspace_module_apply_failed"),
            TEXT("Module application did not produce an available module node."), Stage.Asset,
            TEXT("module_node.available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
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
        UE::UAI::Contract::FDiagnostic AppliedResolveError;
        if (!ResolveModule(*Stage.OutputNode, AppliedSelector, AppliedModule,
                           AppliedIndex, AppliedResolveError)) {
          OutError = AppliedResolveError;
          return false;
        }
        if (AppliedIndex != Patch->TargetIndex &&
            !MoveModule(*Stage.OutputNode, *AppliedModule, Patch->TargetIndex,
                        AppliedIndex, OutError))
          return false;
      }
    }
    FailureObject = StagePatch.SourceObject.Get();
    FailureField = TEXT("modules");
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
        if (!UeAgentNiagaraStandaloneModuleAuthoring::GetAssignmentNodes(
                *Stage.OutputNode, AppliedAssignments, OutError)) return false;
        if (AppliedAssignments.Num() != ExpectedModules.Num()) {
          OutError = NativeAssetFailure(TEXT("niagara_workspace_stage_replace_postcondition_failed"),
              TEXT("The replaced standalone stage has a different assignment count from the requested final list."),
              Stage.Asset, TEXT("assignment_count"), MakeShared<FJsonValueNumber>(AppliedAssignments.Num()),
              MakeShared<FJsonValueNumber>(ExpectedModules.Num()));
          return false;
        }
        for (int32 Index = 0; Index < ExpectedModules.Num(); ++Index) {
          if (!AppliedAssignments[Index] ||
              !UeAgentNiagaraStandaloneModuleAuthoring::
                  AssignmentTargetsMatch(
                      *AppliedAssignments[Index],
                      ExpectedModules[Index]->AssignmentTargets)) {
            if (!AppliedAssignments[Index]) {
              OutError = NativeAssetFailure(TEXT("niagara_workspace_stage_replace_postcondition_failed"),
                  TEXT("The replaced stage has no assignment node at the requested index."), Stage.Asset,
                  TEXT("assignment_node.available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
              OutError.OperationIndex = Index;
            } else {
              TArray<FNiagaraVariable> ActualTargets;
              UeAgentNiagaraStandaloneModuleAuthoring::GetAssignmentTargets(*AppliedAssignments[Index], ActualTargets);
              OutError = AssignmentPostconditionFailure(ExpectedModules[Index]->AssignmentTargets, ActualTargets, Index, true);
            }
            return false;
          }
        }
      } else {
        TArray<UNiagaraNodeFunctionCall *> AppliedModules;
        if (!GetModules(*Stage.OutputNode, AppliedModules, OutError)) return false;
        if (AppliedModules.Num() != ExpectedModules.Num()) {
          OutError = NativeAssetFailure(TEXT("niagara_workspace_stage_replace_postcondition_failed"),
              TEXT("The replaced stage has a different module count from the requested final list."),
              Stage.Asset, TEXT("module_count"), MakeShared<FJsonValueNumber>(AppliedModules.Num()),
              MakeShared<FJsonValueNumber>(ExpectedModules.Num()));
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
            if (!Applied) {
              OutError = NativeAssetFailure(TEXT("niagara_workspace_stage_replace_postcondition_failed"),
                  TEXT("The replaced stage has no module node at the requested index."), Stage.Asset,
                  TEXT("module_node.available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
            } else if (Expected.bAssignmentModule) {
              if (const auto* Assignment = Cast<UNiagaraNodeAssignment>(Applied))
                OutError = AssignmentPostconditionFailure(Expected.AssignmentTargets, Assignment->GetAssignmentTargets(), Index, false);
              else
                OutError = NativeAssetFailure(TEXT("niagara_workspace_stage_replace_postcondition_failed"),
                    TEXT("The resulting node is not an assignment module."), Stage.Asset,
                    TEXT("module_node.class"), MakeShared<FJsonValueString>(Applied->GetClass()->GetName()),
                    MakeShared<FJsonValueString>(UNiagaraNodeAssignment::StaticClass()->GetName()));
            } else if (!Expected.Script.IsValid()) {
              OutError = NativeAssetFailure(TEXT("niagara_workspace_stage_replace_postcondition_failed"),
                  TEXT("The expected module script is unavailable during postcondition verification."), Stage.Asset,
                  TEXT("expected_script.available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
            } else {
              OutError = NativeAssetFailure(TEXT("niagara_workspace_stage_replace_postcondition_failed"),
                  TEXT("The resulting module uses a different script asset from the requested module."), Stage.Asset,
                  TEXT("script_asset_path"), MakeShared<FJsonValueString>(GetModuleScriptPath(*Applied)),
                  MakeShared<FJsonValueString>(Expected.Script->GetPathName()));
            }
            OutError.OperationIndex = Index;
            return false;
          }
        }
      }
    }
    if (!ValidateGraphInputDependenciesAcyclic(*Stage.Graph, OutError))
      return false;
    Stage.Graph->NotifyGraphChanged();
    Stage.Asset->MarkPackageDirty();
    bStageSucceeded = true;
  }
  return true;
}

bool RestoreGraphBackups(TArray<FGraphBackup> &Backups, UE::UAI::Contract::FDiagnostic& OutError) {
  for (FGraphBackup &Backup : Backups) {
    UNiagaraScriptSource *Source = Backup.Source.Get();
    UNiagaraGraph *Current = Source ? Source->NodeGraph : nullptr;
    if (!Source || !Current || !Backup.Graph.IsValid()) {
      OutError = NativeAssetFailure(TEXT("niagara_workspace_graph_rollback_state_invalid"),
          TEXT("Graph rollback requires the script source, current graph, and captured graph backup."),
          Source, !Source ? TEXT("script_source.available") : !Current ? TEXT("current_graph.available") : TEXT("graph_backup.available"),
          MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
      return false;
    }
    const FString RetiredName = FString::Printf(
        TEXT("%s_UAIRetired_%s"), *Backup.OriginalName.ToString(),
        *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    if (!Current->Rename(*RetiredName, GetTransientPackage(),
                         REN_DontCreateRedirectors | REN_NonTransactional)) {
      OutError = NativeAssetFailure(TEXT("niagara_workspace_graph_rollback_retire_failed"),
          TEXT("The editor could not retire the current Niagara graph before restoring its backup."),
          Current, TEXT("current_graph.retired"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
      return false;
    }
    UNiagaraGraph *Restored = DuplicateObject<UNiagaraGraph>(
        Backup.Graph.Get(), Source, Backup.OriginalName);
    if (!Restored) {
      OutError = NativeAssetFailure(TEXT("niagara_workspace_graph_rollback_restore_failed"),
          TEXT("The editor could not duplicate the captured Niagara graph back into its script source."),
          Source, TEXT("graph.restored"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
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
        bUncheckedDelete(bInUncheckedDelete) {
    if (ProviderExtension.IsValid())
      ProviderSourceLocations = UeAgentAuthoringDiagnostics::FScopedSourceLocations::CaptureProjection(*ProviderExtension);
    bReadFullSnapshot = !StagePatches.IsEmpty() || !EmitterStructurePatches.IsEmpty()
        || !UserParameterPatches.IsEmpty();
  }

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

  virtual bool PreparePropertyApplication(FOperationFailure &OutFailure) override {
    OutFailure = FOperationFailure();
    if (Operation == EOperation::Create || Operation == EOperation::Clone) {
      if (!CreatePackage(*PackagePath)) {
        SetFailure(OutFailure, NativeAssetFailure(TEXT("niagara_target_package_create_failed"),
            TEXT("The editor could not create the destination Niagara asset package."), nullptr,
            TEXT("package.created"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)));
        return false;
      }
    }
    return true;
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
        UE::UAI::Contract::FDiagnostic IsolationError;
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
          SetFailure(OutFailure, NativeAssetFailure(TEXT("niagara_workspace_graph_rollback_capture_failed"),
              TEXT("The editor could not duplicate the Niagara graph for rollback."), Patch.Stage.Graph,
              TEXT("graph.backup_created"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)));
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
          SetFailure(OutFailure, NativeAssetFailure(TEXT("niagara_workspace_graph_rollback_capture_failed"),
              TEXT("The editor could not duplicate the Niagara graph for rollback."), Patch.Owner.Graph,
              TEXT("graph.backup_created"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)));
          return false;
        }
        SeenGraphs.Add(Patch.Owner.Graph);
      }
      UE::UAI::Contract::FDiagnostic BackupError;
      if (!CaptureEmitterStructureBackups(
              EmitterStructurePatches, EmitterStructureBackups, BackupError)) {
        SetFailure(OutFailure, BackupError);
        return false;
      }
      if (!UserParameterPatches.IsEmpty()) {
        UNiagaraSystem *System = Cast<UNiagaraSystem>(Existing);
        if (!System) {
          SetFailure(OutFailure, NativeAssetFailure(TEXT("niagara_workspace_user_parameters_require_system"),
              TEXT("Capturing user-parameter rollback state requires a Niagara system."), Existing,
              TEXT("asset.class"), Existing ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Existing->GetClass()->GetName()))
                  : TSharedPtr<FJsonValue>(MakeShared<FJsonValueNull>()),
              MakeShared<FJsonValueString>(TEXT("NiagaraSystem"))));
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
    UE::UAI::Contract::FDiagnostic Error;
    if (!RemovalStage.Capture(Existing, Error)) {
      SetFailure(OutFailure, Error);
      return false;
    }
    return true;
  }

  virtual bool ApplyProposed(FOperationFailure &OutFailure) override {
    UeAgentAuthoringDiagnostics::FScopedSourceLocations Sources(ProviderExtension.Get(), [this](const FString& Pointer) {
      return UeAgentAuthoringDiagnostics::FScopedSourceLocations::ResolveProjection(ProviderSourceLocations, Pointer);
    });
    FUeAgentInterfaceLogger::Log(
        TEXT("NiagaraWorkspaceApply phase=apply_proposed_begin asset=%s"),
        *PackagePath);
    OutFailure = FOperationFailure();
    PropertyIntents.Reset();
    bPropertyIntentsApplied = false;
    UE::UAI::Contract::FDiagnostic Error;
    if (!UserParameterPatches.IsEmpty()) {
      FUeAgentInterfaceLogger::Log(
          TEXT("NiagaraWorkspaceApply phase=user_parameters_begin asset=%s "
               "count=%d"),
          *PackagePath, UserParameterPatches.Num());
      UNiagaraSystem *System = Cast<UNiagaraSystem>(Proposed.Get());
      if (!System) {
        SetFailure(OutFailure, NativeAssetFailure(TEXT("niagara_workspace_user_parameter_apply_failed"),
            TEXT("Applying user parameters requires a Niagara system proposed asset."), Proposed.Get(),
            TEXT("asset.class"), Proposed.IsValid() ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Proposed->GetClass()->GetName()))
                : TSharedPtr<FJsonValue>(MakeShared<FJsonValueNull>()),
            MakeShared<FJsonValueString>(TEXT("NiagaraSystem"))));
        return false;
      }
      if (!UeAgentNiagaraUserParameterAuthoring::ApplyPatches(*System, UserParameterPatches, Error)) {
        SetFailure(OutFailure, Error);
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
    if (!ApplyEmitterStructurePatches(EmitterStructurePatches, PropertyIntents, Error)) {
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
    bPropertyIntentsApplied = true;
    return VerifyPropertyIntents(Proposed.Get(), OutFailure);
  }

  virtual bool ApplyLifecycle(FOperationFailure &OutFailure) override {
    OutFailure = FOperationFailure();
    if (Operation == EOperation::Remove) {
      UE::UAI::Contract::FDiagnostic Error;
      if (!RemovalStage.Stage(Error)) {
        SetFailure(OutFailure, Error);
        return false;
      }
      Actual = nullptr;
      return true;
    }

    if (!Proposed.IsValid()) {
      SetFailure(OutFailure, NativeAssetFailure(TEXT("niagara_proposed_asset_missing"),
          TEXT("The proposed Niagara asset is no longer available for publication."), nullptr,
          TEXT("proposed_asset.available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)));
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
        SetFailure(OutFailure, NativeAssetFailure(TEXT("niagara_asset_publish_failed"),
            !TargetPackage ? TEXT("The destination package could not be created for publishing the Niagara asset.")
                : AssetName.IsEmpty() ? TEXT("The destination package does not provide a nonempty asset name.")
                : TEXT("The editor could not duplicate the proposed Niagara asset into the destination package."),
            Proposed.Get(), !TargetPackage ? TEXT("package.available") : AssetName.IsEmpty() ? TEXT("asset_name") : TEXT("published_asset.created"),
            AssetName.IsEmpty() && TargetPackage ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(AssetName))
                : TSharedPtr<FJsonValue>(MakeShared<FJsonValueBoolean>(false)),
            AssetName.IsEmpty() && TargetPackage ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(TEXT("a nonempty asset name")))
                : TSharedPtr<FJsonValue>(MakeShared<FJsonValueBoolean>(true))));
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
      auto Error = AssetValueFailure(TEXT("niagara_compile_required"),
          TEXT("This Niagara apply lifecycle requires compilation to verify the resulting asset."),
          TEXT("compile"), MakeShared<FJsonValueBoolean>(bCompile), MakeShared<FJsonValueBoolean>(true));
      UeAgentAuthoringDiagnostics::FScopedSourceLocations Sources(ProviderExtension.Get(), [this](const FString& Pointer) {
        return UeAgentAuthoringDiagnostics::FScopedSourceLocations::ResolveProjection(ProviderSourceLocations, Pointer);
      });
      if (ProviderExtension.IsValid())
        UeAgentAuthoringDiagnostics::LocateFailure(Error, *ProviderExtension, TEXT("authoring_apply"), PackagePath, TEXT("compile"));
      SetFailure(OutFailure, MoveTemp(Error));
      return false;
    }
    FUeAgentInterfaceLogger::Log(
        TEXT("NiagaraWorkspaceApply phase=compile_begin asset=%s"),
        *PackagePath);
    UE::UAI::Contract::FDiagnostic Error;
    if (!CompileAssetObject(Actual ? Actual : Existing, CompileReceipt,
                            Error)) {
      SetFailure(OutFailure, Error);
      return false;
    }
    FUeAgentInterfaceLogger::Log(
        TEXT("NiagaraWorkspaceApply phase=compile_end asset=%s"), *PackagePath);
    return VerifyPropertyIntents(Actual ? Actual : Existing, OutFailure);
  }

  virtual bool Save(FOperationFailure &OutFailure) override {
    OutFailure = FOperationFailure();
    if (!bSave || Operation == EOperation::Remove)
      return true;
    FUeAgentInterfaceLogger::Log(
        TEXT("NiagaraWorkspaceApply phase=save_begin asset=%s"), *PackagePath);
    UE::UAI::Contract::FDiagnostic Error;
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
    if (!BuildPackageDurablePlan(PackagePath, PackagePath,
                                   Operation == EOperation::Remove
                                       ? Existing
                                       : (Existing ? Existing : Proposed.Get()),
                                   Operation == EOperation::Remove
                                       ? EPackageDurableIntent::Delete
                                       : EPackageDurableIntent::Save,
                                   OutPlans, OutFailure,
                                   Operation != EOperation::Remove ||
                                       (!bForceDelete && !bUncheckedDelete),
                                   Operation == EOperation::Create || Operation == EOperation::Clone
                                       ? EPackageBeforePresence::Absent : EPackageBeforePresence::Present)) return false;
    // Synchronous standalone Script compilation roots native compilation copies.
    // Their shallow graph references can keep the original asset reachable; use
    // native package replacement to rebind those references to persisted data.
    if (Operation != EOperation::Remove
        && Cast<UNiagaraScript>(Existing ? Existing : Proposed.Get())) {
      OutPlans.Last().ReadbackPolicy = EPackageReadbackPolicy::ReplaceResidentPackage;
    }
    return true;
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
    if (Operation != EOperation::Remove && !VerifyPropertyIntents(Proposed.Get(), OutFailure)) return false;
    return BuildReadRequest(
        Operation == EOperation::Remove ? nullptr : Proposed.Get(),
        Operation == EOperation::Remove, OutRequests, OutFailure);
  }

  virtual bool BuildDomainActualReadRequests(
      TArray<UeAgentAuthoringState::FReadRequest> &OutRequests,
      FOperationFailure &OutFailure) const override {
    if (Operation != EOperation::Remove && !VerifyPropertyIntents(Actual ? Actual : Existing, OutFailure)) return false;
    return BuildReadRequest(
        Operation == EOperation::Remove ? nullptr
                                        : (Actual ? Actual : Existing),
        Operation == EOperation::Remove || ((Operation == EOperation::Create || Operation == EOperation::Clone) && !Actual && !Existing), OutRequests, OutFailure);
  }

  virtual bool RequiresCheckpointRollback() const override {
    return bSave && (Operation == EOperation::Create || Operation == EOperation::Clone);
  }

  virtual bool SupportsPersistentReadback() const override {
    return Operation == EOperation::Update || Operation == EOperation::Create || Operation == EOperation::Clone;
  }

  virtual bool ReleaseForPersistentReadback(FOperationFailure &OutFailure) override {
    OutFailure = FOperationFailure();
    if (Existing) {
      UE::UAI::Contract::FDiagnostic Error;
      if (!UeAgentNiagaraOps::PrepareAssetForStructuralNiagaraEdit(
              *Existing, ClosedEditorCount, Error)) {
        SetFailure(OutFailure, Error);
        return false;
      }
    }
    StagePatches.Reset();
    EmitterStructurePatches.Reset();
    UserParameterPatches.Reset();
    GraphBackups.Reset();
    EmitterStructureBackups.Reset();
    UserParameterSnapshot.Reset();
    // Update deliberately edits Existing in place. Only release the strong
    // reference; the public asset must not be retired as a proposed scratch.
    Proposed.Reset();
    Existing = nullptr;
    Actual = nullptr;
    return true;
  }

  virtual bool RebindAfterPersistentReadback(FOperationFailure &OutFailure) override {
    Existing = LoadAsset(PackagePath);
    Actual = Existing;
    if (!Existing) {
      SetFailure(OutFailure, NativeAssetFailure(TEXT("niagara_persistent_reload_failed"),
          TEXT("The saved Niagara asset could not be loaded for persistent readback."), nullptr,
          TEXT("asset.loaded"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)));
      return false;
    }
    return true;
  }

  virtual bool BuildDomainBeforeReadRequests(
      TArray<UeAgentAuthoringState::FReadRequest> &OutRequests,
      FOperationFailure &OutFailure) const override {
    UObject* Root = Actual ? Actual : Existing;
    if ((Operation == EOperation::Create || Operation == EOperation::Clone) && !Root)
      return BuildReadRequest(nullptr, true, OutRequests, OutFailure);
    UE::UAI::Contract::FDiagnostic Error;
    if (!Root) {
      SetFailure(OutFailure, NativeAssetFailure(TEXT("niagara_before_snapshot_failed"),
          TEXT("Capturing the existing Niagara asset snapshot requires an available asset root."), nullptr,
          TEXT("asset.available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)));
      return false;
    }
    if (!BuildAssetSnapshotReadRequests(*Root, true, OutRequests, Error, PackagePath)) {
      SetFailure(OutFailure, Error);
      return false;
    }
    return true;
  }

  virtual bool PrepareCheckpointRestore(FOperationFailure &OutFailure) override {
    bPropertyIntentsApplied = false;
    if (Operation != EOperation::Create && Operation != EOperation::Clone)
      return ReleaseForPersistentReadback(OutFailure);
    UObject* Created = Actual ? Actual : Existing;
    if (!ReleaseForPersistentReadback(OutFailure)) return false;
    if (Created) {
      const int32 DeletedCount = ObjectTools::DeleteObjects({Created}, false);
      if (DeletedCount != 1) {
        SetFailure(OutFailure, NativeAssetFailure(TEXT("niagara_created_restore_delete_failed"),
            TEXT("Checkpoint restoration did not delete exactly the newly created Niagara asset."), nullptr,
            TEXT("deleted_object_count"), MakeShared<FJsonValueNumber>(DeletedCount), MakeShared<FJsonValueNumber>(1)));
        return false;
      }
    }
    if (UPackage* Package = FindPackage(nullptr, *PackagePath)) {
      Package->SetDirtyFlag(false);
      FText Error;
      if (!UPackageTools::UnloadPackages({Package}, Error, true)) {
        SetFailure(OutFailure, NativeAssetFailure(TEXT("niagara_created_restore_unload_failed"),
            Error.IsEmpty() ? TEXT("The editor could not unload the Niagara package during checkpoint restoration.")
                : TEXT("The editor could not unload the Niagara package during checkpoint restoration: ") + Error.ToString(),
            nullptr, TEXT("package.unloaded"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)));
        return false;
      }
    }
    return true;
  }

  virtual bool RebindAfterCheckpointRestore(FOperationFailure &OutFailure) override {
    if (Operation != EOperation::Create && Operation != EOperation::Clone)
      return RebindAfterPersistentReadback(OutFailure);
    OutFailure = FOperationFailure();
    const FString ObjectPath = PackagePath + TEXT(".") + FPackageName::GetLongPackageAssetName(PackagePath);
    if (FindObject<UObject>(nullptr, *ObjectPath)) {
      SetFailure(OutFailure, NativeAssetFailure(TEXT("niagara_absent_restore_unconfirmed"),
          TEXT("The created Niagara asset is still present after restoring the previously absent state."), nullptr,
          TEXT("asset.present"), MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false)));
      return false;
    }
    return true;
  }

  virtual bool Commit(FOperationFailure &OutFailure) override {
    OutFailure = FOperationFailure();
    if (Operation == EOperation::Remove && RemovalStage.IsStaged()) {
      UE::UAI::Contract::FDiagnostic Error;
      if (!DeleteAssetObject(RemovalStage.GetAsset(), bForceDelete,
                             bUncheckedDelete, PackagePath, Error)) {
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
    bPropertyIntentsApplied = false;
    if (Operation == EOperation::Update) {
      UE::UAI::Contract::FDiagnostic Error;
      if (!UserParameterPatches.IsEmpty()) {
        UNiagaraSystem *System = Cast<UNiagaraSystem>(Existing);
        if (!System) {
          SetFailure(OutFailure, NativeAssetFailure(TEXT("niagara_user_parameter_snapshot_restore_failed"),
              TEXT("Restoring user parameters requires an existing Niagara system asset."), Existing,
              TEXT("asset.class"), Existing ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Existing->GetClass()->GetName()))
                  : TSharedPtr<FJsonValue>(MakeShared<FJsonValueNull>()),
              MakeShared<FJsonValueString>(TEXT("NiagaraSystem"))));
          return false;
        }
        if (!UeAgentNiagaraUserParameterAuthoring::RestoreSnapshot(*System, UserParameterSnapshot, Error)) {
          SetFailure(OutFailure, Error);
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
      if (!RollbackRemovedEmitterHandles(EmitterStructurePatches, Error)) {
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
      TSet<UNiagaraSystem*> RestoredSystems;
      for (const FEmitterStructurePatch& Patch : EmitterStructurePatches) {
        UNiagaraSystem* System = Patch.TargetSystem.Get();
        if (Patch.bEnsureSystemIntegration && System && !RestoredSystems.Contains(System)) {
          UeAgentNiagaraOps::RefreshNiagaraSystemAfterStructuralEdit(*System, true, true);
          RestoredSystems.Add(System);
        }
      }
      return true;
    }
    UE::UAI::Contract::FDiagnostic Error;
    if (Operation != EOperation::Remove) {
      if (Actual && !DeleteAssetObject(Actual, true, true, PackagePath, Error)) {
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
  void SetFailure(FOperationFailure& OutFailure, UE::UAI::Contract::FDiagnostic Diagnostic,
      const FString& Subject = FString()) const {
    UeAgentNiagaraAuthoring::SetFailure(OutFailure, MoveTemp(Diagnostic), Subject);
    if (OutFailure.Diagnostic.TargetPath.IsEmpty()) OutFailure.Diagnostic.TargetPath = PackagePath;
  }

  bool VerifyPropertyIntents(UObject* Root, FOperationFailure& OutFailure) const {
    if (!bPropertyIntentsApplied || PropertyIntents.IsEmpty()) return true;
    UeAgentAuthoringDiagnostics::FScopedSourceLocations Sources(ProviderExtension.Get(), [this](const FString& Pointer) {
      return UeAgentAuthoringDiagnostics::FScopedSourceLocations::ResolveProjection(ProviderSourceLocations, Pointer);
    });
    UE::UAI::Contract::FDiagnostic Error;
    if (VerifyEmitterPropertyIntents(Root, PropertyIntents, Error)) return true;
    SetFailure(OutFailure, Error);
    UeAgentAuthoringDiagnostics::ReportConsistencyWarning(OutFailure.Diagnostic.Code, OutFailure.Diagnostic, TEXT("niagara_property_observation"));
    OutFailure = FOperationFailure();
    return true;
  }

  bool
  BuildReadRequest(const UObject *Root, const bool bExpectedAbsent,
                   TArray<UeAgentAuthoringState::FReadRequest> &OutRequests,
                   FOperationFailure &OutFailure) const {
    OutFailure = FOperationFailure();
    UeAgentAuthoringIdentity::FCanonicalIdentity Identity;
    UE::UAI::Contract::FDiagnostic IdentityError;
    if (!UeAgentAuthoringIdentity::BuildCanonicalIdentity(
            IdentityModelForKind(Kind), TargetKindForKind(Kind),
            {{TEXT("asset_path"), PackagePath}}, Identity, IdentityError)) {
      SetFailure(OutFailure, IdentityError);
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
        bReadFullSnapshot) {
      UE::UAI::Contract::FDiagnostic Error;
      if (!BuildAssetSnapshotReadRequests(*const_cast<UObject *>(Root), true,
                                          OutRequests, Error, PackagePath)) {
        SetFailure(OutFailure, Error);
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
  TArray<FEmitterPropertyIntent> PropertyIntents;
  bool bPropertyIntentsApplied = false;
  TArray<UeAgentNiagaraUserParameterAuthoring::FPatch> UserParameterPatches;
  TSharedPtr<const FJsonObject> ProviderExtension;
  TMap<FString, UE::UAI::Contract::FSourceLocation> ProviderSourceLocations;
  TArray<FGraphBackup> GraphBackups;
  TArray<FEmitterStructureBackup> EmitterStructureBackups;
  TArray<UeAgentNiagaraUserParameterAuthoring::FSnapshotEntry>
      UserParameterSnapshot;
  FAssetCompileReceipt CompileReceipt;
  bool bCompile = false;
  bool bReadFullSnapshot = false;
  bool bSave = true;
  bool bForceDelete = false;
  bool bUncheckedDelete = false;
  bool bEditorIsolationPerformed = false;
  int32 ClosedEditorCount = 0;
};

class FAssetLifecycle final : public IReflectionOperationLifecycle {
public:
  explicit FAssetLifecycle(bool bInCompactValues = false)
      : bCompactValues(bInCompactValues) {}

  virtual bool
  PrepareLifecycle(const FPrepareRequest &Request,
                   TUniquePtr<IReflectionOperationLifecycleState> &OutState,
                   FOperationFailure &OutFailure) const override {
    OutState.Reset();
    OutFailure = FOperationFailure();
    if (!Request.IsValid() || !Request.Input->Target.IsValid()) {
      SetFailure(OutFailure, AssetValueFailure(TEXT("niagara_asset_apply_request_invalid"),
          TEXT("Niagara lifecycle preparation requires a valid operation request and target object."),
          TEXT("request.valid"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)));
      return false;
    }

    FString PackagePath;
    const FJsonObject* SourceObject = Request.Input->Target.Get();
    FString SourceField;
    ON_SCOPE_EXIT {
      if (OutFailure.Diagnostic.IsValid()) {
        if (OutFailure.Diagnostic.TargetPath.IsEmpty()) OutFailure.Diagnostic.TargetPath = PackagePath;
        if (SourceObject)
          UeAgentAuthoringDiagnostics::LocateFailure(OutFailure.Diagnostic, *SourceObject,
              TEXT("authoring_apply"), PackagePath, SourceField);
      }
    };
    Request.Input->Target->TryGetStringField(TEXT("asset_path"), PackagePath);
    PackagePath =
        UeAgentAuthoringAssetIdentity::NormalizePackagePath(PackagePath);
    if (!FPackageName::IsValidLongPackageName(PackagePath) ||
        FPackageName::GetLongPackageAssetName(PackagePath).IsEmpty()) {
      SourceField = TEXT("asset_path");
      SetFailure(OutFailure, AssetValueFailure(TEXT("invalid_asset_path"),
          TEXT("asset_path must be a valid long package name with a nonempty asset name."),
          SourceField, Request.Input->Target->TryGetField(SourceField),
          MakeShared<FJsonValueString>(TEXT("a long package name such as /Game/Effects/MySystem"))));
      return false;
    }

    UObject *Existing = LoadAsset(PackagePath);
    EOperation Operation = Request.Input->RequestedOperation;
    if (Operation == EOperation::Upsert) {
      Operation = Existing ? EOperation::Update : EOperation::Create;
    }
    if ((Operation == EOperation::Create || Operation == EOperation::Clone) &&
        Existing) {
      SourceField = TEXT("asset_path");
      SetFailure(OutFailure, AssetValueFailure(TEXT("asset_already_exists"),
          TEXT("Create and clone operations require a destination asset that does not already exist."),
          SourceField, MakeShared<FJsonValueString>(PackagePath), MakeShared<FJsonValueString>(TEXT("an absent destination asset"))));
      return false;
    }
    if ((Operation == EOperation::Update || Operation == EOperation::Remove) &&
        !Existing) {
      SourceField = TEXT("asset_path");
      SetFailure(OutFailure, AssetValueFailure(TEXT("asset_not_found"),
          TEXT("Update and remove operations require an existing Niagara asset."),
          SourceField, MakeShared<FJsonValueString>(PackagePath), MakeShared<FJsonValueString>(TEXT("an existing Niagara asset"))));
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
        SourceField = TEXT("source_asset_path");
        SetFailure(OutFailure, AssetValueFailure(TEXT("source_asset_not_found"),
            TEXT("The Niagara clone source asset could not be found."),
            SourceField, Request.Input->Target->TryGetField(SourceField),
            MakeShared<FJsonValueString>(TEXT("an existing clone source asset"))));
        return false;
      }
    }
    const FString InferredKind = GetAssetKind(Existing ? Existing : Source);
    if (Kind.IsEmpty())
      Kind = InferredKind;
    if (!InferredKind.IsEmpty() && Kind != InferredKind) {
      SourceField = TEXT("asset_kind");
      SetFailure(OutFailure, AssetValueFailure(TEXT("niagara_asset_kind_conflict"),
          TEXT("The requested asset kind differs from the existing or clone-source Niagara asset kind."),
          SourceField, MakeShared<FJsonValueString>(Kind), MakeShared<FJsonValueString>(InferredKind)));
      return false;
    }
    if (Kind != TEXT("system") && Kind != TEXT("emitter") &&
        Kind != TEXT("script")) {
      SourceField = TEXT("asset_kind");
      SetFailure(OutFailure, AssetValueFailure(TEXT("niagara_asset_kind_invalid"),
          TEXT("A Niagara asset kind must resolve to system, emitter, or script."),
          SourceField, MakeShared<FJsonValueString>(Kind), MakeShared<FJsonValueString>(TEXT("system, emitter, or script"))));
      return false;
    }
    if (Kind != TEXT("script") && Request.Input->ProviderExtension.IsValid() &&
        Request.Input->ProviderExtension->HasField(TEXT("script_usage"))) {
      SourceObject = Request.Input->ProviderExtension.Get();
      SourceField = TEXT("script_usage");
      SetFailure(OutFailure, AssetValueFailure(TEXT("niagara_workspace_script_usage_asset_kind_invalid"),
          TEXT("script_usage is only supported for Niagara script assets."),
          TEXT("asset_kind"), MakeShared<FJsonValueString>(Kind), MakeShared<FJsonValueString>(TEXT("script"))));
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
        SetFailure(OutFailure, NativeAssetFailure(TEXT("niagara_proposed_package_create_failed"),
            TEXT("The editor could not allocate a scratch package for the proposed Niagara asset."),
            nullptr, TEXT("scratch_package.created"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)));
        return false;
      }
      if (Operation == EOperation::Update) {
        Proposed = Existing;
      } else if (Operation == EOperation::Create) {
        UClass *Class = Kind == TEXT("system") ? UNiagaraSystem::StaticClass()
                        : Kind == TEXT("emitter")
                            ? UNiagaraEmitter::StaticClass()
                            : UNiagaraScript::StaticClass();
        UE::UAI::Contract::FDiagnostic Error;
        Proposed = CreateFactorySeed(
            Class, ScratchPackage, Kind,
            ReadBool(Request.Input->ProviderExtension,
                     TEXT("create_default_nodes"), true),
            ReadBool(Request.Input->ProviderExtension,
                     TEXT("add_default_modules_and_renderers"), true),
            Error, RequestedScriptUsage);
        if (!Proposed) {
          SourceObject = Request.Input->ProviderExtension.Get();
          SetFailure(OutFailure, Error);
          return false;
        }
      } else {
        UE::UAI::Contract::FDiagnostic ValidationError;
        if (!ValidateAssetForDuplication(Source, ValidationError)) {
          SetFailure(OutFailure, ValidationError);
          return false;
        }
        Proposed = StaticDuplicateObject(Source, ScratchPackage, NAME_None,
                                         RF_Transactional);
        if (!Proposed) {
          SetFailure(OutFailure, NativeAssetFailure(TEXT("niagara_proposed_asset_create_failed"),
              TEXT("The editor could not duplicate the Niagara source asset into the proposed scratch package."),
              Source, TEXT("proposed_asset.created"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)));
          return false;
        }
      }
    }
    if (UNiagaraScript *Script = Cast<UNiagaraScript>(Proposed)) {
      if (!RequestedScriptUsage.IsEmpty()) {
        ENiagaraScriptUsage RequestedUsage = ENiagaraScriptUsage::Module;
        if (!UeAgentNiagaraStageModuleAuthoring::TryParseScriptUsage(
                RequestedScriptUsage, RequestedUsage)) {
          SourceObject = Request.Input->ProviderExtension.Get();
          SourceField = TEXT("script_usage");
          SetFailure(OutFailure, AssetValueFailure(TEXT("niagara_workspace_script_usage_invalid"),
              TEXT("script_usage is not a recognized ENiagaraScriptUsage value."),
              SourceField, MakeShared<FJsonValueString>(RequestedScriptUsage),
              MakeShared<FJsonValueString>(TEXT("an ENiagaraScriptUsage name, optionally prefixed with ENiagaraScriptUsage::"))));
          return false;
        }
        if (Script->GetUsage() != RequestedUsage) {
          SourceObject = Request.Input->ProviderExtension.Get();
          SourceField = TEXT("script_usage");
          SetFailure(OutFailure, AssetValueFailure(TEXT("niagara_workspace_script_usage_conflict"),
              TEXT("The requested script usage differs from the Niagara script's existing usage."),
              SourceField, MakeShared<FJsonValueString>(UeAgentNiagaraStageModuleAuthoring::ScriptUsageToString(RequestedUsage)),
              MakeShared<FJsonValueString>(UeAgentNiagaraStageModuleAuthoring::ScriptUsageToString(Script->GetUsage()))));
          return false;
        }
      }
      if (Request.Input->ProviderExtension.IsValid() &&
          Request.Input->ProviderExtension->HasField(TEXT("stages")) &&
          Script->GetUsage() != ENiagaraScriptUsage::Module) {
        SourceObject = Request.Input->ProviderExtension.Get();
        SourceField = TEXT("stages");
        SetFailure(OutFailure, AssetValueFailure(TEXT("niagara_workspace_script_stage_usage_unsupported"),
            TEXT("Standalone stage authoring requires a Niagara script with Module usage."),
            TEXT("script_usage"), MakeShared<FJsonValueString>(UeAgentNiagaraStageModuleAuthoring::ScriptUsageToString(Script->GetUsage())),
            MakeShared<FJsonValueString>(TEXT("Module"))));
        return false;
      }
    }
    TArray<FStagePatch> StagePatches;
    if (Operation == EOperation::Remove &&
        Request.Input->ProviderExtension.IsValid() &&
        (Request.Input->ProviderExtension->HasField(TEXT("stages")) ||
         Request.Input->ProviderExtension->HasField(TEXT("emitters")) ||
         Request.Input->ProviderExtension->HasField(TEXT("user_parameters")))) {
      SourceObject = Request.Input->ProviderExtension.Get();
      SourceField = SourceObject->HasField(TEXT("stages")) ? TEXT("stages")
          : SourceObject->HasField(TEXT("emitters")) ? TEXT("emitters") : TEXT("user_parameters");
      TArray<TSharedPtr<FJsonValue>> SuppliedFields;
      for (const TCHAR* Field : {TEXT("stages"), TEXT("emitters"), TEXT("user_parameters")})
        if (SourceObject->HasField(Field)) SuppliedFields.Add(MakeShared<FJsonValueString>(Field));
      SetFailure(OutFailure, AssetValueFailure(TEXT("niagara_workspace_remove_extensions_not_allowed"),
          TEXT("An asset removal cannot also contain stage, emitter, or user-parameter edits."),
          TEXT("provider_extension.fields"), MakeShared<FJsonValueArray>(SuppliedFields),
          MakeShared<FJsonValueString>(TEXT("no stages, emitters, or user_parameters fields for remove"))));
      return false;
    }
    TArray<FEmitterStructurePatch> EmitterStructurePatches;
    UE::UAI::Contract::FDiagnostic StructurePatchError;
    FUeAgentInterfaceLogger::Log(
        TEXT("NiagaraWorkspaceApply phase=parse_emitters_begin asset=%s"),
        *PackagePath);
    if (!ParseEmitterStructurePatches(
            Proposed, PackagePath, Request.Input->ProviderExtension,
            bCompactValues, EmitterStructurePatches, StructurePatchError)) {
      SourceObject = Request.Input->ProviderExtension.Get();
      SourceField = TEXT("emitters");
      SetFailure(OutFailure, StructurePatchError);
      return false;
    }
    FUeAgentInterfaceLogger::Log(
        TEXT(
            "NiagaraWorkspaceApply phase=parse_emitters_end asset=%s count=%d"),
        *PackagePath, EmitterStructurePatches.Num());
    TArray<UeAgentNiagaraUserParameterAuthoring::FPatch> UserParameterPatches;
    UE::UAI::Contract::FDiagnostic UserParameterPatchError;
    FUeAgentInterfaceLogger::Log(
        TEXT(
            "NiagaraWorkspaceApply phase=parse_user_parameters_begin asset=%s"),
        *PackagePath);
    if (!UeAgentNiagaraUserParameterAuthoring::ParsePatches(
            Cast<UNiagaraSystem>(Proposed), Request.Input->ProviderExtension,
            UserParameterPatches, UserParameterPatchError)) {
      SourceObject = Request.Input->ProviderExtension.Get();
      SourceField = TEXT("user_parameters");
      SetFailure(OutFailure, UserParameterPatchError);
      return false;
    }
    FUeAgentInterfaceLogger::Log(
        TEXT("NiagaraWorkspaceApply phase=parse_user_parameters_end asset=%s "
             "count=%d"),
        *PackagePath, UserParameterPatches.Num());
    UE::UAI::Contract::FDiagnostic StagePatchError;
    FUeAgentInterfaceLogger::Log(
        TEXT("NiagaraWorkspaceApply phase=parse_stages_begin asset=%s"),
        *PackagePath);
    if (!ParseStagePatches(Proposed, PackagePath,
                           Request.Input->ProviderExtension, StagePatches,
                           StagePatchError)) {
      const bool bDeferredEventStage =
          !EmitterStructurePatches.IsEmpty() &&
          StagePatchError.Code == TEXT("niagara_stage_not_found");
      if (!bDeferredEventStage) {
        SourceObject = Request.Input->ProviderExtension.Get();
        SourceField = TEXT("stages");
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

private:
  const bool bCompactValues;
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
  Result.PropertyPayload.Mode = TEXT("reflection");
  Result.PropertyPayload.PropertyPathField = TEXT("property_path");
  Result.PropertyPayload.ValueField = TEXT("value");
  Result.PropertyPayload.OptionalAssertionFields = {
      TEXT("kind"),         TEXT("struct_path"),  TEXT("enum_path"),
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
  if (Request.Stages.IsValid())
    Extension->SetObjectField(TEXT("stages"), Request.Stages);
  if (Request.Emitters.IsValid())
    Extension->SetObjectField(TEXT("emitters"), Request.Emitters);
  if (Request.UserParameters.IsValid())
    Extension->SetObjectField(TEXT("user_parameters"), Request.UserParameters);
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
    FAssetApplyResult &OutResult, const FString &RequestId,
    TFunction<bool(FOperationFailure&)> FinalReadback = {},
    const TSharedPtr<IDurablePackageStager> &PackageStager = nullptr,
    const bool bCompactValues = false) {
  OutResult = FAssetApplyResult();
  OutResult.Status = TEXT("failed");
  UeAgentAuthoringAssetWorkspaceInput::FRequest ResolveRequest;
  ResolveRequest.WorkspaceContractId = AssetWorkspaceContractId;
  ResolveRequest.Source = Source;
  ResolveRequest.ExpectedAssetPath = ExpectedAssetPath;
  ResolveRequest.Errors.ContractMissing = TEXT("niagara_workspace_contract_missing");
  ResolveRequest.Errors.EntryInvalid = TEXT("niagara_workspace_entry_invalid");
  ResolveRequest.Errors.AssetKindConflict =
      TEXT("niagara_workspace_asset_kind_conflict");
  ResolveRequest.Errors.AssetPathConflict =
      TEXT("niagara_workspace_asset_path_conflict");
  UeAgentAuthoringAssetWorkspaceInput::FResult ResolvedInput;
  if (!UeAgentAuthoringAssetWorkspaceInput::Resolve(
          ResolveRequest, ResolvedInput)) {
    OutResult.Phase = ResolvedInput.Phase;
    OutResult.Diagnostic = ResolvedInput.Diagnostic;
    return false;
  }
  const UeAgentApplyWorkspaceContract::FContract &WorkspaceContract =
      ResolvedInput.WorkspaceContract;
  const UeAgentAuthoringWorkspaceLoader::FLoadResult &LoadResult =
      ResolvedInput.Load;
  const UeAgentApplyInputContract::FOperationInput &OperationInput =
      ResolvedInput.Input;
  const EOperation RequestedOperation = OperationInput.RequestedOperation;
  OutResult.AssetPath = ResolvedInput.AssetPath;
  OutResult.AssetKind = ResolvedInput.AssetKind;
  FString NormalizedExpectedKind = ExpectedAssetKind.TrimStartAndEnd();
  NormalizedExpectedKind.ToLowerInline();
  UObject *ExistingBeforeApply = LoadAsset(OutResult.AssetPath);
  const bool bExistedBeforeApply = ExistingBeforeApply != nullptr;
  if (OutResult.AssetKind.IsEmpty())
    OutResult.AssetKind = GetAssetKind(ExistingBeforeApply);
  if (!NormalizedExpectedKind.IsEmpty() &&
      !OutResult.AssetKind.Equals(NormalizedExpectedKind,
                                  ESearchCase::IgnoreCase)) {
    OutResult.Phase = TEXT("preflight");
    OutResult.Diagnostic = AssetValueFailure(TEXT("niagara_workspace_asset_kind_conflict"),
        TEXT("The workspace asset kind does not match the kind required by this command."),
        TEXT("asset_kind"), MakeShared<FJsonValueString>(OutResult.AssetKind),
        MakeShared<FJsonValueString>(NormalizedExpectedKind));
    OutResult.Diagnostic.Stage = OutResult.Phase;
    OutResult.Diagnostic.TargetPath = OutResult.AssetPath;
    for (const auto& Unit : LoadResult.Workspace.Units) {
      if (Unit.CanonicalUnitKey != ResolvedInput.CanonicalUnitKey || !Unit.Document) continue;
      UeAgentAuthoringDiagnostics::FScopedSourceLocations Sources(Unit.Document.Get(),
          [&Unit](const FString& Pointer) { return Unit.ResolveDiagnosticSource(Pointer); });
      if (OperationInput.Target.IsValid())
        UeAgentAuthoringDiagnostics::LocateAuthoringSource(OutResult.Diagnostic, *OperationInput.Target, TEXT("asset_kind"));
      break;
    }
    return false;
  }
  if (RequestedOperation == EOperation::Clone) {
    const FString &SourceAssetPath = ResolvedInput.SourceAssetPath;
    if (const UNiagaraSystem *SourceSystem =
            Cast<UNiagaraSystem>(LoadAsset(SourceAssetPath))) {
      OutResult.RepairedEmitterVersionCount =
          CountInvalidEmitterVersions(*SourceSystem);
    }
  }

  bool bSave = false;
  if (OperationInput.ProviderExtension.IsValid())
    OperationInput.ProviderExtension->TryGetBoolField(TEXT("save"), bSave);
  FBackendOptions BackendOptions;
  BackendOptions.FinalReadback = MoveTemp(FinalReadback);
  if (PackageStager.IsValid())
    BackendOptions.PackageStager = PackageStager;
  BackendOptions.bUseEditorTransaction =
      RequestedOperation != EOperation::Remove;
  BackendOptions.TransactionDescription =
      NSLOCTEXT("UeAgentInterface", "NiagaraAssetWorkspaceApply",
                "Apply Niagara asset workspace");
  FApplyWorkspaceResult ApplyResult;
  UeAgentAuthoringAssetWorkspaceApply::FFailure ApplyFailure;
  FProviderRegistry LocalProviders;
  const FProviderRegistry *Providers = &FProviderRegistry::Get();
  if (bCompactValues) {
    UeAgentAuthoringOperationApply::ERegistrationResult Registered;
    UE::UAI::Contract::FDiagnostic RegistrationError;
    if (!LocalProviders.RegisterProvider(MakeAssetApplyProvider(true), Registered, RegistrationError)) {
      OutResult.Phase = TEXT("preflight");
      OutResult.Diagnostic = RegistrationError;
      return false;
    }
    Providers = &LocalProviders;
  }
  const bool bApplied = UeAgentAuthoringAssetWorkspaceApply::Execute(
      WorkspaceContract, LoadResult.Workspace, *Providers,
      RequestId, bSave, BackendOptions, ApplyResult, ApplyFailure);
  OutResult.UnitsChecked = ApplyResult.Preflight.UnitsChecked;
  OutResult.MutationsApplied = ApplyResult.Execution.MutationsApplied;
  OutResult.bRollbackAttempted = ApplyResult.Execution.bRollbackAttempted;
  OutResult.bRollbackSucceeded = ApplyResult.Execution.bRollbackSucceeded;
  if (!bApplied) {
    OutResult.Status = ApplyResult.Execution.Status;
    OutResult.Phase = ApplyFailure.Phase;
    OutResult.Diagnostic = ApplyFailure.Diagnostic;
    if (OutResult.bRollbackAttempted && !OutResult.bRollbackSucceeded)
      {
        const auto Cause = OutResult.Diagnostic;
        OutResult.Diagnostic.Code = TEXT("niagara_import_state_unknown");
        OutResult.Diagnostic.PublicMessage = TEXT("Rollback did not restore a confirmed asset state.");
        OutResult.Diagnostic.PublicCause = MakeShared<UE::UAI::Contract::FDiagnostic>(Cause);
      }
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
  OutResult.ObjectPath =
      OutResult.Asset
          ? OutResult.Asset->GetPathName()
          : UeAgentAuthoringAssetIdentity::ToObjectPath(OutResult.AssetPath);
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
      OutResult.Diagnostic = StackValidation.GetFailure();
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
    OutResult.Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(TEXT("niagara_completion_receipt_invalid"),
        TEXT("The Niagara result does not satisfy the required completion conditions."), OutResult.Phase);
    OutResult.Diagnostic.SourceKind = TEXT("native_result");
    OutResult.Diagnostic.TargetPath = OutResult.AssetPath;
    auto AddRequirementFailure = [&](const TCHAR* Field, const FString& Reason,
        const TSharedPtr<FJsonValue>& Actual, const TSharedPtr<FJsonValue>& Expected) {
      auto Error = NativeAssetFailure(TEXT("niagara_completion_requirement_unmet"), Reason, nullptr, Field, Actual, Expected);
      Error.Stage = OutResult.Phase;
      OutResult.Diagnostic.Diagnostics.Add(MoveTemp(Error));
    };
    if (!OutResult.bSaved)
      AddRequirementFailure(TEXT("saved"), TEXT("The requested persisted asset state was not confirmed."),
          MakeShared<FJsonValueBoolean>(OutResult.bSaved), MakeShared<FJsonValueBoolean>(true));
    if (ResolvedOperation != EOperation::Remove) {
      if (OutResult.CompileStatus != TEXT("complete"))
        AddRequirementFailure(TEXT("compile_status"), TEXT("The compile status is not complete."),
            MakeShared<FJsonValueString>(OutResult.CompileStatus), MakeShared<FJsonValueString>(TEXT("complete")));
      if (!OutResult.bCompilationComplete)
        AddRequirementFailure(TEXT("compilation_complete"), TEXT("Asset compilation has not completed."),
            MakeShared<FJsonValueBoolean>(OutResult.bCompilationComplete), MakeShared<FJsonValueBoolean>(true));
      if (OutResult.CompileErrorCount != 0)
        AddRequirementFailure(TEXT("compile_error_count"), TEXT("The compile result does not report zero errors."),
            MakeShared<FJsonValueNumber>(OutResult.CompileErrorCount), MakeShared<FJsonValueNumber>(0));
      if (OutResult.StackIssueCount != 0)
        AddRequirementFailure(TEXT("stack_issue_count"), TEXT("The Niagara stack does not report zero issues."),
            MakeShared<FJsonValueNumber>(OutResult.StackIssueCount), MakeShared<FJsonValueNumber>(0));
      if (OutResult.CompiledTargets.IsEmpty())
        AddRequirementFailure(TEXT("compiled_target_count"), TEXT("No compiled target was recorded."),
            MakeShared<FJsonValueNumber>(0), MakeShared<FJsonValueString>(TEXT("at least one compiled target")));
    }
    return false;
  }
  OutResult.Status = TEXT("complete");
  OutResult.Phase = TEXT("complete");
  return true;
}
} // namespace

FString GetApplyFailureMessage(const FAssetApplyResult& Result) {
  return Result.Diagnostic.PublicMessage;
}

bool RegisterAssetApplyContracts(UE::UAI::Contract::FDiagnostic& OutError) {
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

TSharedRef<IApplyProvider> MakeAssetApplyProvider(bool bCompactValues) {
  static const TSharedRef<IReflectionOperationLifecycle> StrictLifecycle =
      MakeShared<FAssetLifecycle>(false);
  static const TSharedRef<IReflectionOperationLifecycle> CompactLifecycle =
      MakeShared<FAssetLifecycle>(true);
  const TSharedRef<IReflectionOperationLifecycle> &Lifecycle =
      bCompactValues ? CompactLifecycle : StrictLifecycle;
  UeAgentAuthoringObjectApply::FOptions Options;
  Options.bAllowDomainAuthorizedOwners = true;
  Options.bCompactValues = bCompactValues;
  return MakeShared<FReflectionApplyProvider>(ApplyProviderId, Lifecycle,
                                              Options);
}

bool CompileAsset(UObject *Asset, UE::UAI::Contract::FDiagnostic& OutError) {
  return CompileAssetObject(Asset, OutError);
}

bool SaveAsset(UObject *Asset, UE::UAI::Contract::FDiagnostic& OutError) {
  return SaveAssetObject(Asset, OutError);
}



bool ApplyAsset(const FAssetApplyRequest &Request, FAssetApplyResult &OutResult,
                const FString &RequestId) {
  return ExecuteAssetWorkspaceSource(
      UeAgentAuthoringWorkspaceLoader::FSourceRequest::MakeInlineObject(
          MakeApplyDocument(Request), TEXT("inline://niagara-asset-apply")),
      Request.AssetKind, Request.AssetPath, OutResult, RequestId,
      Request.FinalReadback, Request.PackageStager, Request.bCompactValues);
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
    FAssetApplyResult &OutResult, const FString &RequestId,
    bool bCompactValues, TFunction<bool(FOperationFailure&)> FinalReadback,
    const TSharedPtr<IDurablePackageStager>& PackageStager) {
  return ExecuteAssetWorkspaceSource(
      UeAgentAuthoringWorkspaceLoader::FSourceRequest::MakeInlineObject(
          Document, TEXT("inline://niagara-domain-authoring")),
      ExpectedAssetKind, ExpectedAssetPath, OutResult, RequestId,
      MoveTemp(FinalReadback), PackageStager, bCompactValues);
}
} // namespace UeAgentNiagaraAuthoring

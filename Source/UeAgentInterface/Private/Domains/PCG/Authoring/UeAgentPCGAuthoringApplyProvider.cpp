// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/PCG/Authoring/UeAgentPCGAuthoringApplyProvider.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Domains/PCG/Authoring/UeAgentPCGAuthoringDescriptors.h"
#include "Domains/PCG/Authoring/UeAgentPCGAuthoringState.h"
#include "Domains/PCG/Authoring/UeAgentPCGGraphAuthoring.h"
#include "Domains/PCG/UeAgentPCGDomainBackend.h"
#include "Engine/Engine.h"
#include "Misc/PackageName.h"
#include "ObjectTools.h"
#include "PCGGraph.h"
#include "PCGNode.h"
#include "PCGSettings.h"
#include "UObject/StrongObjectPtr.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringState.h"
#include "Utilities/AuthoringOperationApply/UeAgentAuthoringAssetRemovalStage.h"
#include "Utilities/AuthoringOperationApply/UeAgentAuthoringObjectApplyProvider.h"
#include "Utilities/AuthoringOperationApply/UeAgentAuthoringOperationCoordinator.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentApplyWorkspaceContract.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringWorkspaceDependencyGraph.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringWorkspaceLoader.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringWorkspaceTransaction.h"

namespace UeAgentPCGAuthoring {
namespace {
using namespace UeAgentApplyInputContract;
using namespace UeAgentAuthoringOperationApply;

void SetFailure(FOperationFailure &OutFailure, const FString &Code,
                const FString &Detail = FString()) {
  OutFailure = FOperationFailure();
  OutFailure.ErrorCode = Code;
  OutFailure.ErrorDetail = Detail.IsEmpty() ? Code : Detail;
}

FString NormalizePackagePath(const FString &InPath) {
  FString Path = InPath.TrimStartAndEnd();
  if (Path.Contains(TEXT(".")))
    Path = FPackageName::ObjectPathToPackageName(Path);
  return Path;
}

FString ToObjectPath(const FString &PackagePath) {
  const FString Name = FPackageName::GetLongPackageAssetName(PackagePath);
  return Name.IsEmpty() ? PackagePath
                        : FString::Printf(TEXT("%s.%s"), *PackagePath, *Name);
}

UPCGGraph *LoadGraph(const FString &InPath) {
  return UeAgentPCG::LoadAsset<UPCGGraph>(NormalizePackagePath(InPath));
}

UPCGGraph *DuplicatePublishedGraph(UPCGGraph *Source, UPackage *Package,
                                   const FString &Name) {
  if (!Source || !Package || Name.IsEmpty())
    return nullptr;
  UPCGGraph *Published = Cast<UPCGGraph>(
      StaticDuplicateObject(Source, Package, *Name, RF_AllFlags));
  if (Published) {
    Published->ClearFlags(RF_Transient);
    Published->SetFlags(RF_Public | RF_Standalone | RF_Transactional);
  }
  return Published;
}

bool DeleteGraph(UPCGGraph *Graph, FString &OutError) {
  OutError.Reset();
  if (!Graph) {
    OutError = TEXT("pcg_delete_target_missing");
    return false;
  }
  TGuardValue<bool> UnattendedGuard(GIsRunningUnattendedScript, true);
  if (!ObjectTools::DeleteSingleObject(Graph, true)) {
    OutError = TEXT("pcg_delete_blocked_by_references");
    return false;
  }
  return true;
}

bool CompileGraph(UPCGGraph *Graph, FString &OutError) {
  OutError.Reset();
  if (!Graph) {
    OutError = TEXT("pcg_compile_target_missing");
    return false;
  }
  Graph->Recompile();
  Graph->PrimeGraphCompilationCache();
  return true;
}

bool ReadBool(const TSharedPtr<const FJsonObject> &Object, const TCHAR *Field,
              const bool DefaultValue) {
  bool Value = DefaultValue;
  if (Object.IsValid())
    Object->TryGetBoolField(Field, Value);
  return Value;
}

bool ValidateExtensionFields(const TSharedPtr<const FJsonObject> &Extension,
                             FString &OutError) {
  OutError.Reset();
  if (!Extension.IsValid())
    return true;
  for (const TPair<FString, TSharedPtr<FJsonValue>> &Field :
       Extension->Values) {
    if (Field.Key != TEXT("graph") && Field.Key != TEXT("replace_nodes") &&
        Field.Key != TEXT("compile") && Field.Key != TEXT("save")) {
      OutError = TEXT("pcg_provider_extension_unknown_field:") + Field.Key;
      return false;
    }
  }
  return true;
}

bool GraphDocumentHasNodeProperties(
    const TSharedPtr<const FJsonObject> &Document) {
  const TSharedPtr<FJsonObject> *Nodes = nullptr;
  if (!Document.IsValid() ||
      !Document->TryGetObjectField(TEXT("nodes"), Nodes) || !Nodes ||
      !Nodes->IsValid())
    return false;
  for (const TPair<FString, TSharedPtr<FJsonValue>> &Pair : (*Nodes)->Values) {
    const TSharedPtr<FJsonObject> Node =
        Pair.Value.IsValid() ? Pair.Value->AsObject() : nullptr;
    const TArray<TSharedPtr<FJsonValue>> *Properties = nullptr;
    if (Node.IsValid() &&
        Node->TryGetArrayField(TEXT("properties"), Properties) && Properties &&
        !Properties->IsEmpty())
      return true;
  }
  return false;
}

bool RestoreGraph(UPCGGraph &Target, UPCGGraph &RollbackSource,
                  FString &OutError) {
  TSharedPtr<FJsonObject> Document;
  if (!BuildGraphDocument(RollbackSource, Document, OutError))
    return false;
  FGraphApplyResult ApplyResult;
  if (!ApplyGraphDocument(Target, Document, true, ApplyResult, OutError))
    return false;
  const TMap<FString, UPCGNode *> SourceNodes =
      UeAgentPCG::BuildExportNodeMapById(&RollbackSource);
  const TMap<FString, UPCGNode *> TargetNodes =
      UeAgentPCG::BuildExportNodeMapById(&Target);
  for (const TPair<FString, UPCGNode *> &Pair : SourceNodes) {
    UPCGNode *const *TargetNode = TargetNodes.Find(Pair.Key);
    UPCGSettings *SourceSettings =
        Pair.Value ? Pair.Value->GetSettings() : nullptr;
    UPCGSettings *TargetSettings =
        TargetNode && *TargetNode ? (*TargetNode)->GetSettings() : nullptr;
    if (SourceSettings && TargetSettings) {
      UEngine::CopyPropertiesForUnrelatedObjects(SourceSettings,
                                                 TargetSettings);
      (*TargetNode)->UpdateAfterSettingsChangeDuringCreation();
    }
  }
  Target.Recompile();
  Target.MarkPackageDirty();
  return true;
}

class FAssetLifecycleState final : public IReflectionOperationLifecycleState {
public:
  FAssetLifecycleState(FString InPackagePath, EOperation InOperation,
                       UPCGGraph *InExisting, UPCGGraph *InProposed,
                       TSharedPtr<const FJsonObject> InGraphDocument,
                       FGraphApplyResult InGraphResult, bool bInReplaceNodes,
                       bool bInCompile, bool bInSave, bool bInGraphNoOp)
      : PackagePath(MoveTemp(InPackagePath)), Operation(InOperation),
        Existing(InExisting), Proposed(InProposed),
        GraphDocument(MoveTemp(InGraphDocument)), GraphResult(InGraphResult),
        bReplaceNodes(bInReplaceNodes), bCompile(bInCompile), bSave(bInSave),
        bGraphNoOp(bInGraphNoOp) {}

  virtual FString GetTargetKey() const override { return PackagePath; }
  virtual EOperation GetResolvedOperation() const override { return Operation; }
  virtual bool IsNoOp() const override {
    return Operation == EOperation::Update && bGraphNoOp && !bCompile && !bSave;
  }
  virtual FReflectionRoot GetProposedRoot() const override {
    return FReflectionRoot::MakeObject(Proposed.Get());
  }
  virtual FReflectionRoot GetActualRoot() const override {
    return FReflectionRoot::MakeObject(Actual ? Actual : Existing);
  }
  virtual bool ShouldApplyPropertiesToActualRoot() const override {
    return Operation == EOperation::Update;
  }

  virtual bool CaptureRollback(FOperationFailure &OutFailure) override {
    OutFailure = FOperationFailure();
    if (Operation == EOperation::Update && Existing) {
      RollbackCopy.Reset(Cast<UPCGGraph>(StaticDuplicateObject(
          Existing, GetTransientPackage(), NAME_None, RF_Transient)));
      if (!RollbackCopy.IsValid()) {
        SetFailure(OutFailure, TEXT("pcg_rollback_capture_failed"));
        return false;
      }
      return true;
    }
    if (Operation == EOperation::Remove && Existing) {
      FString Error;
      if (!RemovalStage.Capture(Existing, Error)) {
        SetFailure(OutFailure, Error);
        return false;
      }
    }
    return true;
  }

  virtual bool ApplyProposed(FOperationFailure &OutFailure) override {
    OutFailure = FOperationFailure();
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
    if (Operation == EOperation::Update) {
      if (GraphDocument.IsValid()) {
        FGraphApplyResult Applied;
        FString Error;
        if (!ApplyGraphDocument(*Existing, GraphDocument, bReplaceNodes,
                                Applied, Error)) {
          SetFailure(OutFailure, TEXT("pcg_graph_apply_failed"), Error);
          return false;
        }
      }
      Actual = Existing;
      return true;
    }
    if (!Proposed.IsValid()) {
      SetFailure(OutFailure, TEXT("pcg_proposed_asset_missing"));
      return false;
    }
    UPackage *TargetPackage = CreatePackage(*PackagePath);
    Actual = DuplicatePublishedGraph(
        Proposed.Get(), TargetPackage,
        FPackageName::GetLongPackageAssetName(PackagePath));
    if (!Actual) {
      SetFailure(OutFailure, TEXT("pcg_asset_publish_failed"));
      return false;
    }
    FAssetRegistryModule::AssetCreated(Actual);
    Actual->MarkPackageDirty();
    return true;
  }

  virtual bool Compile(FOperationFailure &OutFailure) override {
    OutFailure = FOperationFailure();
    if (!bCompile || Operation == EOperation::Remove)
      return true;
    FString Error;
    if (!CompileGraph(Actual ? Actual : Existing, Error)) {
      SetFailure(OutFailure, Error);
      return false;
    }
    return true;
  }

  virtual bool Save(FOperationFailure &OutFailure) override {
    OutFailure = FOperationFailure();
    if (!bSave || Operation == EOperation::Remove)
      return true;
    FString Error;
    if (!UeAgentPCG::SavePackagesForObjects({Actual ? Actual : Existing},
                                            Error)) {
      SetFailure(OutFailure, Error);
      return false;
    }
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
                                   OutPlans, OutFailure);
  }

  virtual bool BuildDomainExpectedReadRequests(
      TArray<UeAgentAuthoringState::FReadRequest> &OutRequests,
      FOperationFailure &OutFailure) const override {
    return BuildReadRequests(
        Operation == EOperation::Remove ? nullptr : Proposed.Get(),
        Operation == EOperation::Remove, OutRequests, OutFailure);
  }

  virtual bool BuildDomainActualReadRequests(
      TArray<UeAgentAuthoringState::FReadRequest> &OutRequests,
      FOperationFailure &OutFailure) const override {
    return BuildReadRequests(
        Operation == EOperation::Remove ? nullptr
                                        : (Actual ? Actual : Existing),
        Operation == EOperation::Remove, OutRequests, OutFailure);
  }

  virtual TSharedPtr<FJsonObject> BuildResultMetadata() const override {
    TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
    Result->SetNumberField(TEXT("nodes_applied"), GraphResult.NodeCount);
    Result->SetNumberField(TEXT("edges_applied"), GraphResult.EdgeCount);
    Result->SetNumberField(TEXT("node_properties_applied"),
                           GraphResult.NodePropertiesApplied);
    return Result;
  }

  virtual bool Commit(FOperationFailure &OutFailure) override {
    OutFailure = FOperationFailure();
    if (Operation == EOperation::Remove && RemovalStage.IsStaged()) {
      FString Error;
      if (!DeleteGraph(Cast<UPCGGraph>(RemovalStage.GetAsset()), Error)) {
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
    FString Error;
    if (Operation == EOperation::Update && Existing && RollbackCopy.IsValid()) {
      if (!RestoreGraph(*Existing, *RollbackCopy, Error)) {
        SetFailure(OutFailure, TEXT("pcg_rollback_restore_failed"), Error);
        return false;
      }
      Actual = Existing;
      return true;
    }
    if ((Operation == EOperation::Create || Operation == EOperation::Clone) &&
        Actual) {
      if (!DeleteGraph(Actual, Error)) {
        TArray<UObject *> Objects = {Actual};
        if (ObjectTools::DeleteObjectsUnchecked(Objects) <= 0) {
          SetFailure(OutFailure, Error);
          return false;
        }
      }
      Actual = nullptr;
      return true;
    }
    if (Operation == EOperation::Remove && RemovalStage.IsCaptured()) {
      if (!RemovalStage.Rollback(Error)) {
        SetFailure(OutFailure, Error);
        return false;
      }
      Actual = Existing;
    }
    return true;
  }

private:
  bool
  BuildReadRequests(UPCGGraph *Graph, const bool bExpectedAbsent,
                    TArray<UeAgentAuthoringState::FReadRequest> &OutRequests,
                    FOperationFailure &OutFailure) const {
    OutFailure = FOperationFailure();
    FString Error;
    if (!BuildAssetSnapshotReadRequests(Graph, PackagePath, bExpectedAbsent,
                                        OutRequests, Error)) {
      SetFailure(OutFailure, TEXT("pcg_snapshot_request_failed"), Error);
      return false;
    }
    return true;
  }

  FString PackagePath;
  EOperation Operation;
  UPCGGraph *Existing = nullptr;
  TStrongObjectPtr<UPCGGraph> Proposed;
  TStrongObjectPtr<UPCGGraph> RollbackCopy;
  FAssetRemovalStage RemovalStage;
  UPCGGraph *Actual = nullptr;
  TSharedPtr<const FJsonObject> GraphDocument;
  FGraphApplyResult GraphResult;
  bool bReplaceNodes = true;
  bool bCompile = true;
  bool bSave = false;
  bool bGraphNoOp = false;
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
      SetFailure(OutFailure, TEXT("pcg_apply_request_invalid"));
      return false;
    }

    FString PackagePath;
    Request.Input->Target->TryGetStringField(TEXT("asset_path"), PackagePath);
    PackagePath = NormalizePackagePath(PackagePath);
    if (!FPackageName::IsValidLongPackageName(PackagePath) ||
        FPackageName::GetLongPackageAssetName(PackagePath).IsEmpty() ||
        (!PackagePath.Equals(TEXT("/Game")) &&
         !PackagePath.StartsWith(TEXT("/Game/")))) {
      SetFailure(OutFailure, TEXT("invalid_asset_path"), PackagePath);
      return false;
    }

    UPCGGraph *Existing = LoadGraph(PackagePath);
    EOperation Operation = Request.Input->RequestedOperation;
    if (Operation == EOperation::Upsert)
      Operation = Existing ? EOperation::Update : EOperation::Create;
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

    FString Error;
    if (!ValidateExtensionFields(Request.Input->ProviderExtension, Error)) {
      SetFailure(OutFailure, Error);
      return false;
    }
    UPCGGraph *Source = nullptr;
    if (Operation == EOperation::Clone) {
      FString SourcePath;
      Request.Input->Target->TryGetStringField(TEXT("source_asset_path"),
                                               SourcePath);
      Source = LoadGraph(SourcePath);
      if (!Source) {
        SetFailure(OutFailure, TEXT("source_asset_not_found"), SourcePath);
        return false;
      }
    }

    UPCGGraph *Proposed = nullptr;
    TSharedPtr<const FJsonObject> GraphDocument;
    FGraphApplyResult GraphResult;
    const bool bReplaceNodes =
        ReadBool(Request.Input->ProviderExtension, TEXT("replace_nodes"), true);
    if (Operation != EOperation::Remove) {
      UPackage *Scratch = CreatePackage(
          *FString::Printf(TEXT("/Temp/UAI/PCG/Proposed_%s"),
                           *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
      if (Operation == EOperation::Create) {
        Proposed = CreateFactorySeed(Scratch, NAME_None,
                                     RF_Public | RF_Transactional, Error);
      } else {
        Proposed = Cast<UPCGGraph>(StaticDuplicateObject(
            Operation == EOperation::Clone ? Source : Existing, Scratch,
            NAME_None, RF_AllFlags));
        if (Proposed) {
          Proposed->ClearFlags(RF_Transient | RF_Standalone);
          Proposed->SetFlags(RF_Public | RF_Transactional);
        }
      }
      if (!Proposed) {
        SetFailure(OutFailure, Error.IsEmpty()
                                   ? TEXT("pcg_proposed_asset_create_failed")
                                   : Error);
        return false;
      }
      const TSharedPtr<FJsonObject> *GraphObject = nullptr;
      if (Request.Input->ProviderExtension.IsValid() &&
          Request.Input->ProviderExtension->TryGetObjectField(TEXT("graph"),
                                                              GraphObject)) {
        if (!GraphObject || !GraphObject->IsValid()) {
          SetFailure(OutFailure, TEXT("pcg_graph_extension_invalid"));
          return false;
        }
        GraphDocument = *GraphObject;
        if (!ApplyGraphDocument(*Proposed, GraphDocument, bReplaceNodes,
                                GraphResult, Error)) {
          SetFailure(OutFailure, TEXT("pcg_graph_preflight_failed"), Error);
          return false;
        }
      }
    }

    bool bGraphNoOp = !GraphDocument.IsValid();
    if (Operation == EOperation::Update && GraphDocument.IsValid() &&
        !GraphDocumentHasNodeProperties(GraphDocument)) {
      TSharedPtr<FJsonObject> Current;
      TSharedPtr<FJsonObject> ProposedDocument;
      if (BuildGraphDocument(*Existing, Current, Error) &&
          BuildGraphDocument(*Proposed, ProposedDocument, Error)) {
        bGraphNoOp = UeAgentAuthoringState::MakeStableCanonicalValueKey(
                         MakeShared<FJsonValueObject>(Current)) ==
                     UeAgentAuthoringState::MakeStableCanonicalValueKey(
                         MakeShared<FJsonValueObject>(ProposedDocument));
      }
    }

    OutState = MakeUnique<FAssetLifecycleState>(
        PackagePath, Operation, Existing, Proposed, GraphDocument, GraphResult,
        bReplaceNodes,
        ReadBool(Request.Input->ProviderExtension, TEXT("compile"), true),
        ReadBool(Request.Input->ProviderExtension, TEXT("save"), false),
        bGraphNoOp);
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
  if (Operation != EOperation::Remove)
    Result.AcceptedSections.Add(TEXT("properties"));
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
  Result.ProfileId = TEXT("pcg.graph");
  Result.TargetKind = TEXT("pcg_graph");
  Result.SemanticDescriptorRefs = {
      GraphSemanticId,     ExportNodeSemanticId, ExportNodePropertySemanticId,
      ExportPinSemanticId, ExportEdgeSemanticId, ExportParametersSemanticId};
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
      Field(TEXT("asset_kind"), TEXT("type_assertion"), TEXT("pcg_graph_kind")),
      Field(TEXT("source_asset_path"), TEXT("selector"), TEXT("ue_asset_path"),
            {EOperation::Clone}),
      Field(TEXT("operation"), TEXT("operation_control"),
            TEXT("apply_operation"), All),
      Field(TEXT("properties"), TEXT("intent"),
            TEXT("property_descriptor_list")),
      Field(TEXT("graph"), TEXT("provider_extension"), TEXT("pcg_graph")),
      Field(TEXT("replace_nodes"), TEXT("provider_extension"), TEXT("bool")),
      Field(TEXT("compile"), TEXT("provider_extension"), TEXT("bool")),
      Field(TEXT("save"), TEXT("provider_extension"), TEXT("bool")),
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
  Result.ProfileId = TEXT("pcg.graph");
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
  Unit.RouteId = TEXT("pcg.graph");
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
  Result.Execution.Compile = TEXT("provider_hook");
  Result.Execution.Save = TEXT("caller_controlled");
  Result.Execution.Verification = TEXT("shared_snapshot_required");
  Result.UnknownFilePolicy = TEXT("reject");
  Result.DuplicateTargetPolicy = TEXT("reject");
  return Result;
}

TSharedRef<FJsonObject> MakeApplyDocument(const FAssetApplyRequest &Request) {
  TSharedRef<FJsonObject> Target = MakeShared<FJsonObject>();
  Target->SetStringField(TEXT("asset_path"), Request.AssetPath);
  Target->SetStringField(TEXT("asset_kind"), TEXT("pcg_graph"));
  if (!Request.SourceAssetPath.IsEmpty())
    Target->SetStringField(TEXT("source_asset_path"), Request.SourceAssetPath);
  TSharedRef<FJsonObject> Extension = MakeShared<FJsonObject>();
  Extension->SetBoolField(TEXT("replace_nodes"), Request.bReplaceNodes);
  Extension->SetBoolField(TEXT("compile"), Request.bCompile);
  Extension->SetBoolField(TEXT("save"), Request.bSave);
  if (Request.Graph.IsValid())
    Extension->SetObjectField(TEXT("graph"), Request.Graph);
  TSharedRef<FJsonObject> Document = MakeShared<FJsonObject>();
  Document->SetStringField(TEXT("operation"),
                           OperationToString(Request.Operation));
  Document->SetObjectField(TEXT("target"), Target);
  Document->SetObjectField(TEXT("provider_extension"), Extension);
  if (!Request.Properties.IsEmpty())
    Document->SetArrayField(TEXT("properties"), Request.Properties);
  return Document;
}

bool InspectWorkspaceSource(
    const UeAgentAuthoringWorkspaceLoader::FSourceRequest &Source,
    const FString &ExpectedAssetPath, FWorkspaceInspectResult &OutResult) {
  OutResult = FWorkspaceInspectResult();
  OutResult.Status = TEXT("failed");
  UeAgentApplyWorkspaceContract::FContract WorkspaceContract;
  if (!UeAgentApplyWorkspaceContract::FRegistry::Get().FindContract(
          AssetWorkspaceContractId, WorkspaceContract)) {
    OutResult.ErrorCode = TEXT("pcg_workspace_contract_missing");
    return false;
  }
  UeAgentAuthoringWorkspaceLoader::FLoadResult LoadResult;
  if (!UeAgentAuthoringWorkspaceLoader::LoadWorkspace(WorkspaceContract, Source,
                                                      LoadResult)) {
    OutResult.ErrorCode = LoadResult.ErrorCode;
    OutResult.ErrorDetail = LoadResult.ErrorDetail;
    return false;
  }
  const UeAgentAuthoringWorkspaceRouter::FWorkspaceUnit *Unit =
      LoadResult.Workspace.FindSingleUnit(TEXT("asset"));
  const TSharedPtr<const FJsonObject> Document =
      Unit ? Unit->Document : nullptr;
  const TSharedPtr<FJsonObject> *Target = nullptr;
  FString OperationText;
  EOperation Operation;
  if (!Document.IsValid() ||
      !Document->TryGetStringField(TEXT("operation"), OperationText) ||
      !ParseOperation(OperationText, Operation) ||
      !Document->TryGetObjectField(TEXT("target"), Target) || !Target ||
      !Target->IsValid() ||
      !(*Target)->TryGetStringField(TEXT("asset_path"), OutResult.AssetPath)) {
    OutResult.ErrorCode = TEXT("pcg_workspace_entry_invalid");
    return false;
  }
  OutResult.AssetPath = NormalizePackagePath(OutResult.AssetPath);
  OutResult.RequestedOperation = OperationToString(Operation);
  const FString Expected = NormalizePackagePath(ExpectedAssetPath);
  if (!Expected.IsEmpty() &&
      !OutResult.AssetPath.Equals(Expected, ESearchCase::IgnoreCase)) {
    OutResult.ErrorCode = TEXT("pcg_workspace_asset_path_conflict");
    OutResult.ErrorDetail = OutResult.AssetPath + TEXT(" != ") + Expected;
    return false;
  }
  OutResult.bAssetExists = LoadGraph(OutResult.AssetPath) != nullptr;
  OutResult.bWouldCreate =
      Operation == EOperation::Create ||
      (Operation == EOperation::Upsert && !OutResult.bAssetExists);

  UeAgentAuthoringWorkspaceDependencyGraph::FDependencyGraph DependencyGraph;
  UeAgentAuthoringWorkspaceDependencyGraph::FGraphFailure GraphFailure;
  if (!UeAgentAuthoringWorkspaceDependencyGraph::BuildDependencyGraph(
          WorkspaceContract, LoadResult.Workspace,
          UeAgentAuthoringWorkspaceDependencyGraph::FDependencyInputs(),
          DependencyGraph, GraphFailure)) {
    OutResult.ErrorCode = GraphFailure.ErrorCode;
    OutResult.ErrorDetail = GraphFailure.ErrorDetail;
    return false;
  }
  FWorkspaceOperationPreflightProvider Provider(
      UeAgentApplyInputContract::FRegistry::Get(), FProviderRegistry::Get());
  UeAgentAuthoringWorkspaceTransaction::FPreflightResult Preflight;
  if (!UeAgentAuthoringWorkspaceTransaction::PreflightWorkspace(
          WorkspaceContract, LoadResult.Workspace, DependencyGraph, Provider,
          Preflight)) {
    OutResult.UnitsChecked = Preflight.UnitsChecked;
    OutResult.ErrorCode = Preflight.CauseCode.IsEmpty() ? Preflight.ErrorCode
                                                        : Preflight.CauseCode;
    OutResult.ErrorDetail = Preflight.ErrorDetail;
    return false;
  }
  OutResult.UnitsChecked = Preflight.UnitsChecked;
  if (Preflight.Units.Num() == 1 && Preflight.Units[0].Mutation.IsValid()) {
    OutResult.bNoOp = Preflight.Units[0].Mutation->IsNoOp();
    const TSharedPtr<FJsonObject> Metadata =
        Preflight.Units[0].Mutation->BuildResultMetadata();
    if (Metadata.IsValid()) {
      OutResult.NodesPlanned = Metadata->GetIntegerField(TEXT("nodes_applied"));
      OutResult.EdgesPlanned = Metadata->GetIntegerField(TEXT("edges_applied"));
      OutResult.NodePropertiesPlanned =
          Metadata->GetIntegerField(TEXT("node_properties_applied"));
    }
  }
  OutResult.Status = TEXT("ready");
  return true;
}

bool ExecuteWorkspaceSource(
    const UeAgentAuthoringWorkspaceLoader::FSourceRequest &Source,
    const FString &ExpectedAssetPath, FAssetApplyResult &OutResult,
    const FString &RequestId) {
  OutResult = FAssetApplyResult();
  OutResult.Status = TEXT("failed");
  UeAgentApplyWorkspaceContract::FContract WorkspaceContract;
  if (!UeAgentApplyWorkspaceContract::FRegistry::Get().FindContract(
          AssetWorkspaceContractId, WorkspaceContract)) {
    OutResult.Phase = TEXT("registration");
    OutResult.ErrorCode = TEXT("pcg_workspace_contract_missing");
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

  const UeAgentAuthoringWorkspaceRouter::FWorkspaceUnit *Unit =
      LoadResult.Workspace.FindSingleUnit(TEXT("asset"));
  const TSharedPtr<const FJsonObject> Document =
      Unit ? Unit->Document : nullptr;
  const TSharedPtr<FJsonObject> *Target = nullptr;
  FString OperationText;
  EOperation RequestedOperation;
  FString AssetPath;
  if (!Document.IsValid() ||
      !Document->TryGetStringField(TEXT("operation"), OperationText) ||
      !ParseOperation(OperationText, RequestedOperation) ||
      !Document->TryGetObjectField(TEXT("target"), Target) || !Target ||
      !Target->IsValid() ||
      !(*Target)->TryGetStringField(TEXT("asset_path"), AssetPath)) {
    OutResult.Phase = TEXT("load");
    OutResult.ErrorCode = TEXT("pcg_workspace_entry_invalid");
    return false;
  }
  AssetPath = NormalizePackagePath(AssetPath);
  const FString Expected = NormalizePackagePath(ExpectedAssetPath);
  if (!Expected.IsEmpty() &&
      !AssetPath.Equals(Expected, ESearchCase::IgnoreCase)) {
    OutResult.Phase = TEXT("preflight");
    OutResult.ErrorCode = TEXT("pcg_workspace_asset_path_conflict");
    OutResult.ErrorDetail = AssetPath + TEXT(" != ") + Expected;
    return false;
  }

  UeAgentAuthoringWorkspaceDependencyGraph::FDependencyGraph DependencyGraph;
  UeAgentAuthoringWorkspaceDependencyGraph::FGraphFailure GraphFailure;
  if (!UeAgentAuthoringWorkspaceDependencyGraph::BuildDependencyGraph(
          WorkspaceContract, LoadResult.Workspace,
          UeAgentAuthoringWorkspaceDependencyGraph::FDependencyInputs(),
          DependencyGraph, GraphFailure)) {
    OutResult.Phase = TEXT("preflight");
    OutResult.ErrorCode = GraphFailure.ErrorCode;
    OutResult.ErrorDetail = GraphFailure.ErrorDetail;
    return false;
  }

  const bool bExisted = LoadGraph(AssetPath) != nullptr;
  bool bSave = false;
  const TSharedPtr<FJsonObject> *Extension = nullptr;
  if (Document->TryGetObjectField(TEXT("provider_extension"), Extension) &&
      Extension && Extension->IsValid())
    (*Extension)->TryGetBoolField(TEXT("save"), bSave);
  FBackendOptions BackendOptions;
  BackendOptions.bUseEditorTransaction =
      RequestedOperation != EOperation::Remove;
  BackendOptions.TransactionDescription =
      NSLOCTEXT("UeAgentInterface", "PCGGraphWorkspaceApply",
                "Apply PCG graph workspace");
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
      WorkspaceContract, LoadResult.Workspace, DependencyGraph,
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
    if (OutResult.ErrorCode.IsEmpty())
      OutResult.ErrorCode = bPreflight ? ApplyResult.Preflight.ErrorCode
                                       : ApplyResult.Execution.ErrorCode;
    OutResult.ErrorDetail = bPreflight ? ApplyResult.Preflight.ErrorDetail
                                       : ApplyResult.Execution.ErrorDetail;
    return false;
  }

  const EOperation Resolved =
      RequestedOperation == EOperation::Upsert
          ? (bExisted ? EOperation::Update : EOperation::Create)
          : RequestedOperation;
  OutResult.Status = TEXT("applied");
  OutResult.Phase = TEXT("complete");
  OutResult.ResolvedOperation = OperationToString(Resolved);
  OutResult.AssetPath = AssetPath;
  OutResult.Asset =
      Resolved == EOperation::Remove ? nullptr : LoadGraph(AssetPath);
  OutResult.ObjectPath = OutResult.Asset ? OutResult.Asset->GetPathName()
                                         : ToObjectPath(AssetPath);
  if (const TSharedPtr<FJsonObject> *Metadata =
          ApplyResult.UnitResultMetadata.Find(TEXT("asset"));
      Metadata && Metadata->IsValid()) {
    OutResult.NodesApplied =
        (*Metadata)->GetIntegerField(TEXT("nodes_applied"));
    OutResult.EdgesApplied =
        (*Metadata)->GetIntegerField(TEXT("edges_applied"));
    OutResult.NodePropertiesApplied =
        (*Metadata)->GetIntegerField(TEXT("node_properties_applied"));
  }
  return true;
}
} // namespace

bool RegisterApplyContracts(FString &OutError) {
  UeAgentApplyInputContract::ERegistrationResult InputResult;
  if (!UeAgentApplyInputContract::FRegistry::Get().RegisterContract(
          MakeInputContract(), InputResult, OutError))
    return false;
  UeAgentApplyWorkspaceContract::ERegistrationResult WorkspaceResult;
  return UeAgentApplyWorkspaceContract::FRegistry::Get().RegisterContract(
      MakeWorkspaceContract(), WorkspaceResult, OutError);
}

TSharedRef<IApplyProvider> MakeApplyProvider() {
  static const TSharedRef<IReflectionOperationLifecycle> Lifecycle =
      MakeShared<FAssetLifecycle>();
  UeAgentAuthoringObjectApply::FOptions Options;
  Options.bAllowDomainAuthorizedOwners = true;
  return MakeShared<FReflectionApplyProvider>(ApplyProviderId, Lifecycle,
                                              Options);
}

bool ApplyAsset(const FAssetApplyRequest &Request, FAssetApplyResult &OutResult,
                const FString &RequestId) {
  return ExecuteWorkspaceSource(
      UeAgentAuthoringWorkspaceLoader::FSourceRequest::MakeInlineObject(
          MakeApplyDocument(Request), TEXT("inline://pcg-graph-apply")),
      Request.AssetPath, OutResult, RequestId);
}

bool ApplyAssetWorkspace(const FString &WorkspaceRoot,
                         const FString &ExpectedAssetPath,
                         FAssetApplyResult &OutResult,
                         const FString &RequestId) {
  return ExecuteWorkspaceSource(
      UeAgentAuthoringWorkspaceLoader::FSourceRequest::MakeDirectory(
          WorkspaceRoot),
      ExpectedAssetPath, OutResult, RequestId);
}

bool InspectAssetWorkspace(const FString &WorkspaceRoot,
                           const FString &ExpectedAssetPath,
                           FWorkspaceInspectResult &OutResult) {
  return InspectWorkspaceSource(
      UeAgentAuthoringWorkspaceLoader::FSourceRequest::MakeDirectory(
          WorkspaceRoot),
      ExpectedAssetPath, OutResult);
}
} // namespace UeAgentPCGAuthoring

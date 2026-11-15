// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/PCG/Authoring/UeAgentPCGAuthoringApplyProvider.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Domains/PCG/Authoring/UeAgentPCGAuthoringDescriptors.h"
#include "Domains/PCG/Authoring/UeAgentPCGAuthoringState.h"
#include "Domains/PCG/Authoring/UeAgentPCGGraphAuthoring.h"
#include "Domains/PCG/UeAgentPCGDomainBackend.h"
#include "Domains/PCG/UeAgentPcgGraphCompilation.h"
#include "Engine/Engine.h"
#include "Misc/PackageName.h"
#include "ObjectTools.h"
#include "PCGGraph.h"
#include "PCGNode.h"
#include "PCGSettings.h"
#include "UObject/StrongObjectPtr.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringAssetIdentity.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringJsonFields.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringState.h"
#include "Utilities/AuthoringOperationApply/UeAgentAuthoringAssetRemovalStage.h"
#include "Utilities/AuthoringOperationApply/UeAgentAuthoringObjectApplyProvider.h"
#include "Utilities/AuthoringOperationApply/UeAgentAuthoringOperationCoordinator.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentApplyWorkspaceContract.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringAssetWorkspaceApply.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringAssetWorkspaceInput.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringWorkspaceDependencyGraph.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringWorkspaceLoader.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringWorkspaceTransaction.h"

namespace UeAgentPCGAuthoring {
namespace {
using namespace UeAgentApplyInputContract;
using namespace UeAgentAuthoringOperationApply;

void SetFailure(FOperationFailure& OutFailure, UE::UAI::Contract::FDiagnostic Diagnostic,
    const FString& Subject = FString()) {
  OutFailure.Diagnostic = MoveTemp(Diagnostic);
  if (OutFailure.Diagnostic.SubjectId.IsEmpty()) OutFailure.Diagnostic.SubjectId = Subject;
  if (OutFailure.Diagnostic.TargetPath.IsEmpty()) OutFailure.Diagnostic.TargetPath = Subject;
}

UE::UAI::Contract::FDiagnostic NativeFailure(const TCHAR* Code, const TCHAR* Reason,
    const TCHAR* Field, TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected) {
  auto Error = UE::UAI::Contract::FDiagnostic::Failure(Code, Reason, TEXT("authoring_apply"));
  Error.SourceKind = TEXT("native_result");
  Error.PropertyPath = Field;
  Error.ActualValue = MoveTemp(Actual);
  Error.ExpectedValue = MoveTemp(Expected);
  Error.ActualValuePresent = Error.ActualValue.IsValid();
  Error.ExpectedValuePresent = Error.ExpectedValue.IsValid();
  return Error;
}

UE::UAI::Contract::FDiagnostic InputFailure(const TCHAR* Code, const TCHAR* Reason,
    const FJsonObject& Input, const FString& Field, const TCHAR* Expected) {
  auto Error = UE::UAI::Contract::FDiagnostic::Failure(Code, Reason, TEXT("authoring_apply"));
  Error.PropertyPath = Field;
  Error.ActualValue = Input.TryGetField(Field);
  Error.ActualValuePresent = Error.ActualValue.IsValid();
  Error.ExpectedValue = MakeShared<FJsonValueString>(Expected);
  Error.ExpectedValuePresent = true;
  UeAgentAuthoringDiagnostics::LocateAuthoringSource(Error, Input, Field);
  return Error;
}







UPCGGraph *LoadGraph(const FString &InPath) {
  return UeAgentPCG::LoadAsset<UPCGGraph>(
      UeAgentAuthoringAssetIdentity::NormalizePackagePath(InPath));
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

bool DeleteGraph(UPCGGraph* Graph, const FString& Target, UE::UAI::Contract::FDiagnostic& OutError)
{
  OutError.Reset();
  if (!Graph) {
    OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("pcg_delete_target_missing"),
      TEXT("No PCGGraph object was supplied for deletion."), TEXT("authoring_apply"));
    OutError.SourceKind = TEXT("native_result"); OutError.TargetPath = Target; OutError.PropertyPath = TEXT("asset");
    OutError.ActualValuePresent = true; OutError.ActualValue = MakeShared<FJsonValueNull>();
    OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("PCGGraph object"));
    return false;
  }
  TGuardValue<bool> UnattendedGuard(GIsRunningUnattendedScript, true);
  if (!ObjectTools::DeleteSingleObject(Graph, true)) {
    OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("pcg_delete_rejected"),
      TEXT("The editor rejected deletion of the PCGGraph object."), TEXT("authoring_apply"),
      TEXT("DeleteSingleObject returned false with reference checking enabled; its bool API does not distinguish deletion-policy and reference-check rejection."));
    OutError.SourceKind = TEXT("native_result"); OutError.TargetPath = Target; OutError.PropertyPath = TEXT("deletion_succeeded");
    OutError.ActualValuePresent = true; OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
    OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
    return false;
  }
  return true;
}

bool CompileGraph(UPCGGraph *Graph, UE::UAI::Contract::FDiagnostic& OutError) {
  OutError.Reset();
  if (!Graph) {
    OutError = NativeFailure(TEXT("pcg_compile_target_missing"), TEXT("No PCG graph is available for compilation."),
        TEXT("graph_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
    return false;
  }
  return UE::UAI::PcgCompilation::Compile(*Graph, OutError);
}

bool ReadBool(const TSharedPtr<const FJsonObject> &Object, const TCHAR *Field,
              const bool DefaultValue) {
  bool Value = DefaultValue;
  if (Object.IsValid())
    Object->TryGetBoolField(Field, Value);
  return Value;
}

bool ValidateExtensionFields(const TSharedPtr<const FJsonObject> &Extension,
                             UE::UAI::Contract::FDiagnostic& OutError) {
  OutError.Reset();
  if (!Extension.IsValid())
    return true;
  for (const TPair<FString, TSharedPtr<FJsonValue>> &Field :
       Extension->Values) {
    if (Field.Key != TEXT("graph") && Field.Key != TEXT("replace_nodes") &&
        Field.Key != TEXT("compile") && Field.Key != TEXT("save") &&
        Field.Key != TEXT("preserve_existing_nodes")) {
      OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("pcg_provider_extension_unknown_field"),
          TEXT("The PCG provider extension contains an unsupported field."), TEXT("authoring_apply"));
      OutError.PropertyPath = Field.Key;
      OutError.ActualValue = MakeShared<FJsonValueString>(Field.Key);
      OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("graph, replace_nodes, preserve_existing_nodes, compile or save"));
      UeAgentAuthoringDiagnostics::LocateAuthoringSource(OutError, *Extension, Field.Key);
      return false;
    }
  }
  return UeAgentAuthoringJsonFields::ValidateOptionalFields(*Extension,
      {{TEXT("graph"), EJson::Object}, {TEXT("replace_nodes"), EJson::Boolean},
       {TEXT("preserve_existing_nodes"), EJson::Boolean}, {TEXT("compile"), EJson::Boolean}, {TEXT("save"), EJson::Boolean}}, OutError);
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
                  UE::UAI::Contract::FDiagnostic& OutError) {
  TSharedPtr<FJsonObject> Document;
  if (!BuildGraphDocument(RollbackSource, Document, OutError, Target.GetPathName()))
    return false;
  FGraphApplyResult ApplyResult;
  if (!ApplyGraphDocument(Target, Document, true, ApplyResult, OutError, false, nullptr, Target.GetPathName()))
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
                       bool bInCompile, bool bInSave, bool bInGraphNoOp,
                       bool bInPreserveExistingNodes,
                       TSharedPtr<const UE::UAI::HjsonWorkspace::FGraphAuthoringConnectionContext> InConnections)
      : PackagePath(MoveTemp(InPackagePath)), Operation(InOperation),
        Existing(InExisting), Proposed(InProposed),
        GraphDocument(MoveTemp(InGraphDocument)), GraphResult(InGraphResult),
        bReplaceNodes(bInReplaceNodes), bPreserveExistingNodes(bInPreserveExistingNodes),
        bCompile(bInCompile), bSave(bInSave), bGraphNoOp(bInGraphNoOp),
        Connections(MoveTemp(InConnections)) {
    if (GraphDocument) GraphSources = UeAgentAuthoringDiagnostics::FScopedSourceLocations::CaptureProjection(*GraphDocument);
  }

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
        SetFailure(OutFailure, NativeFailure(TEXT("pcg_rollback_capture_failed"), TEXT("Unreal could not duplicate the graph to capture rollback state."),
            TEXT("rollback_copy_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)), PackagePath);
        return false;
      }
      return true;
    }
    if (Operation == EOperation::Remove && Existing) {
      UE::UAI::Contract::FDiagnostic Error;
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
      UE::UAI::Contract::FDiagnostic Error;
      if (!RemovalStage.Stage(Error)) {
        SetFailure(OutFailure, Error);
        return false;
      }
      Actual = nullptr;
      return true;
    }
    if (Operation == EOperation::Update) {
      if (GraphDocument.IsValid()) {
        UeAgentAuthoringDiagnostics::FScopedSourceLocations SourceScope(GraphDocument.Get(),
            [this](const FString& Pointer) { return UeAgentAuthoringDiagnostics::FScopedSourceLocations::ResolveProjection(GraphSources, Pointer); });
        UE::UAI::Contract::FDiagnostic Error;
        if (!ApplyGraphDocument(*Existing, GraphDocument, bReplaceNodes,
                                ActualGraphResult, Error, bPreserveExistingNodes, Connections.Get(), PackagePath)) {
          SetFailure(OutFailure, MoveTemp(Error));
          return false;
        }
      }
      Actual = Existing;
      return true;
    }
    if (!Proposed.IsValid()) {
      SetFailure(OutFailure, NativeFailure(TEXT("pcg_proposed_asset_missing"), TEXT("The prepared PCG graph is no longer available for publication."),
          TEXT("proposed_graph_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)), PackagePath);
      return false;
    }
    UPackage *TargetPackage = CreatePackage(*PackagePath);
    Actual = DuplicatePublishedGraph(
        Proposed.Get(), TargetPackage,
        FPackageName::GetLongPackageAssetName(PackagePath));
    if (!Actual) {
      SetFailure(OutFailure, NativeFailure(TEXT("pcg_asset_publish_failed"), !TargetPackage
          ? TEXT("Unreal could not create the target package for the PCG graph.") : TEXT("Unreal could not duplicate the prepared graph into its target package."),
          !TargetPackage ? TEXT("package_available") : TEXT("published_graph_available"),
          MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)), PackagePath);
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
    UE::UAI::Contract::FDiagnostic Error;
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
    UE::UAI::Contract::FDiagnostic Error;
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
    UE::UAI::Contract::FDiagnostic Error;
    if (Operation != EOperation::Remove && Proposed.IsValid() &&
        !VerifyGraphPropertyIntent(*Proposed, GraphResult, Error)) {
      SetFailure(OutFailure, Error);
      return false;
    }
    return BuildReadRequests(
        Operation == EOperation::Remove ? nullptr : Proposed.Get(),
        Operation == EOperation::Remove, OutRequests, OutFailure);
  }

  virtual bool SupportsPersistentReadback() const override {
    return Operation == EOperation::Update && bSave;
  }
  virtual bool ReleaseForPersistentReadback(FOperationFailure &OutFailure) override {
    OutFailure = FOperationFailure();
    Existing = nullptr;
    Actual = nullptr;
    Proposed.Reset();
    RollbackCopy.Reset();
    return true;
  }
  virtual bool RebindAfterPersistentReadback(FOperationFailure &OutFailure) override {
    OutFailure = FOperationFailure();
    Existing = LoadGraph(PackagePath);
    Actual = Existing;
    if (!Actual) {
      SetFailure(OutFailure, NativeFailure(TEXT("pcg_persistent_rebind_failed"), TEXT("The saved PCG graph could not be reloaded for persistent readback."),
          TEXT("graph_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)), PackagePath);
      return false;
    }
    return true;
  }

  virtual bool BuildDomainActualReadRequests(
      TArray<UeAgentAuthoringState::FReadRequest> &OutRequests,
      FOperationFailure &OutFailure) const override {
    UPCGGraph* Current = Actual ? Actual : Existing;
    UE::UAI::Contract::FDiagnostic Error;
    if (Operation != EOperation::Remove && Current &&
        !VerifyGraphPropertyIntent(*Current,
            Operation == EOperation::Update ? ActualGraphResult : GraphResult, Error)) {
      SetFailure(OutFailure, Error);
      return false;
    }
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
      UE::UAI::Contract::FDiagnostic Error;
      if (!DeleteGraph(Cast<UPCGGraph>(RemovalStage.GetAsset()), PackagePath, Error)) {
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
    UE::UAI::Contract::FDiagnostic Error;
    if (Operation == EOperation::Update && Existing && RollbackCopy.IsValid()) {
      if (!RestoreGraph(*Existing, *RollbackCopy, Error)) {
        SetFailure(OutFailure, MoveTemp(Error), PackagePath);
        return false;
      }
      Actual = Existing;
      return true;
    }
    if ((Operation == EOperation::Create || Operation == EOperation::Clone) &&
        Actual) {
      if (!DeleteGraph(Actual, PackagePath, Error)) {
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
  void SetFailure(FOperationFailure& OutFailure, UE::UAI::Contract::FDiagnostic Error,
      const FString& Target = FString()) const {
    UeAgentPCGAuthoring::SetFailure(OutFailure, MoveTemp(Error), Target.IsEmpty() ? PackagePath : Target);
  }

  bool
  BuildReadRequests(UPCGGraph *Graph, const bool bExpectedAbsent,
                    TArray<UeAgentAuthoringState::FReadRequest> &OutRequests,
                    FOperationFailure &OutFailure) const {
    OutFailure = FOperationFailure();
    UE::UAI::Contract::FDiagnostic Error;
    const int32 First = OutRequests.Num();
    if (!BuildAssetSnapshotReadRequests(Graph, PackagePath, bExpectedAbsent,
                                        OutRequests, Error)) {
      SetFailure(OutFailure, MoveTemp(Error), PackagePath);
      return false;
    }
    if (Operation == EOperation::Update && GraphDocument.IsValid() && !bExpectedAbsent) {
      for (int32 Index = First; Index < OutRequests.Num(); ++Index)
        if (OutRequests[Index].SemanticId == GraphSemanticId)
          OutRequests[Index].Context.Add(TEXT("observed_graph_body"), TEXT("true"));
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
  TMap<FString, UE::UAI::Contract::FSourceLocation> GraphSources;
  FGraphApplyResult GraphResult;
  FGraphApplyResult ActualGraphResult;
  bool bReplaceNodes = true;
  bool bPreserveExistingNodes = false;
  bool bCompile = true;
  bool bSave = false;
  bool bGraphNoOp = false;
  TSharedPtr<const UE::UAI::HjsonWorkspace::FGraphAuthoringConnectionContext> Connections;
};

class FAssetLifecycle final : public IReflectionOperationLifecycle {
public:
  explicit FAssetLifecycle(TSharedPtr<const UE::UAI::HjsonWorkspace::FGraphAuthoringConnectionContext> InConnections)
      : Connections(MoveTemp(InConnections)) {}
  virtual bool
  PrepareLifecycle(const FPrepareRequest &Request,
                   TUniquePtr<IReflectionOperationLifecycleState> &OutState,
                   FOperationFailure &OutFailure) const override {
    OutState.Reset();
    OutFailure = FOperationFailure();
    if (!Request.IsValid() || !Request.Input->Target.IsValid()) {
      const TCHAR* Field = !Request.Contract ? TEXT("contract") : !Request.Unit ? TEXT("unit")
          : !Request.Input ? TEXT("input") : TEXT("target");
      SetFailure(OutFailure, NativeFailure(TEXT("pcg_apply_request_invalid"),
          *FString::Printf(TEXT("The PCG prepare request is missing its required %s object."), Field),
          Field, nullptr, MakeShared<FJsonValueString>(TEXT("a valid preparation object"))));
      return false;
    }

    FString PackagePath;
    Request.Input->Target->TryGetStringField(TEXT("asset_path"), PackagePath);
    PackagePath =
        UeAgentAuthoringAssetIdentity::NormalizePackagePath(PackagePath);
    if (!FPackageName::IsValidLongPackageName(PackagePath) ||
        FPackageName::GetLongPackageAssetName(PackagePath).IsEmpty() ||
        (!PackagePath.Equals(TEXT("/Game")) &&
         !PackagePath.StartsWith(TEXT("/Game/")))) {
      SetFailure(OutFailure, InputFailure(TEXT("invalid_asset_path"), TEXT("The PCG asset path must identify a valid named package under /Game."),
          *Request.Input->Target, TEXT("asset_path"), TEXT("a valid /Game package path with an asset name")), PackagePath);
      return false;
    }

    UPCGGraph *Existing = LoadGraph(PackagePath);
    EOperation Operation = Request.Input->RequestedOperation;
    if (Operation == EOperation::Upsert)
      Operation = Existing ? EOperation::Update : EOperation::Create;
    if ((Operation == EOperation::Create || Operation == EOperation::Clone) &&
        Existing) {
      SetFailure(OutFailure, InputFailure(TEXT("asset_already_exists"), TEXT("Create or clone requires a target path without an existing PCG graph."),
          *Request.Input->Target, TEXT("asset_path"), TEXT("an unused PCG graph asset path")), PackagePath);
      return false;
    }
    if ((Operation == EOperation::Update || Operation == EOperation::Remove) &&
        !Existing) {
      SetFailure(OutFailure, InputFailure(TEXT("asset_not_found"), TEXT("Update or remove requires an existing PCG graph at the target path."),
          *Request.Input->Target, TEXT("asset_path"), TEXT("an existing PCG graph asset path")), PackagePath);
      return false;
    }

    UE::UAI::Contract::FDiagnostic Error;
    if (!ValidateExtensionFields(Request.Input->ProviderExtension, Error)) {
      SetFailure(OutFailure, MoveTemp(Error), PackagePath);
      return false;
    }
    UPCGGraph *Source = nullptr;
    if (Operation == EOperation::Clone) {
      FString SourcePath;
      Request.Input->Target->TryGetStringField(TEXT("source_asset_path"),
                                               SourcePath);
      Source = LoadGraph(SourcePath);
      if (!Source) {
        SetFailure(OutFailure, InputFailure(TEXT("source_asset_not_found"), TEXT("The clone source path did not resolve to a PCG graph."),
            *Request.Input->Target, TEXT("source_asset_path"), TEXT("an existing PCG graph asset path")), PackagePath);
        return false;
      }
    }

    UPCGGraph *Proposed = nullptr;
    TSharedPtr<const FJsonObject> GraphDocument;
    FGraphApplyResult GraphResult;
    const bool bReplaceNodes =
        ReadBool(Request.Input->ProviderExtension, TEXT("replace_nodes"), true);
    const bool bPreserveExistingNodes =
        ReadBool(Request.Input->ProviderExtension, TEXT("preserve_existing_nodes"), false);
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
        } else {
          Error = NativeFailure(TEXT("pcg_proposed_asset_create_failed"), TEXT("Unreal could not duplicate the source PCG graph into the preparation package."),
              TEXT("proposed_graph_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
        }
      }
      if (!Proposed) {
        SetFailure(OutFailure, MoveTemp(Error), PackagePath);
        return false;
      }
      const TSharedPtr<FJsonObject> *GraphObject = nullptr;
      if (Request.Input->ProviderExtension.IsValid() &&
          Request.Input->ProviderExtension->TryGetObjectField(TEXT("graph"),
                                                              GraphObject)) {
        if (!GraphObject || !GraphObject->IsValid()) {
          SetFailure(OutFailure, InputFailure(TEXT("pcg_graph_extension_invalid"), TEXT("The graph extension must contain a graph document object."),
              *Request.Input->ProviderExtension, TEXT("graph"), TEXT("a PCG graph document object")), PackagePath);
          return false;
        }
        GraphDocument = *GraphObject;
        if (!ApplyGraphDocument(*Proposed, GraphDocument, bReplaceNodes,
                                GraphResult, Error, bPreserveExistingNodes, Connections.Get(), PackagePath)) {
          SetFailure(OutFailure, MoveTemp(Error), PackagePath);
          return false;
        }
      }
    }

    bool bGraphNoOp = !GraphDocument.IsValid();
    if (Operation == EOperation::Update && GraphDocument.IsValid() &&
        !GraphDocumentHasNodeProperties(GraphDocument)) {
      TSharedPtr<FJsonObject> Current;
      TSharedPtr<FJsonObject> ProposedDocument;
      if (BuildGraphDocument(*Existing, Current, Error, PackagePath) &&
          BuildGraphDocument(*Proposed, ProposedDocument, Error, PackagePath)) {
        bGraphNoOp = UeAgentAuthoringState::MakeStableCanonicalValueKey(
                         MakeShared<FJsonValueObject>(Current)) ==
                     UeAgentAuthoringState::MakeStableCanonicalValueKey(
                         MakeShared<FJsonValueObject>(ProposedDocument));
      } else {
        // Failure to prove a no-op retains the existing apply path.
        if (Error.TargetPath.IsEmpty()) Error.TargetPath = PackagePath;
        UeAgentAuthoringDiagnostics::ReportConsistencyWarning(Error);
      }
    }

    OutState = MakeUnique<FAssetLifecycleState>(
        PackagePath, Operation, Existing, Proposed, GraphDocument, GraphResult,
        bReplaceNodes,
        ReadBool(Request.Input->ProviderExtension, TEXT("compile"), true),
        ReadBool(Request.Input->ProviderExtension, TEXT("save"), false),
        bGraphNoOp, bPreserveExistingNodes, Connections);
    return true;
  }
private:
  TSharedPtr<const UE::UAI::HjsonWorkspace::FGraphAuthoringConnectionContext> Connections;
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
      Field(TEXT("asset_kind"), TEXT("type_assertion"), TEXT("pcg_graph_kind")),
      Field(TEXT("source_asset_path"), TEXT("selector"), TEXT("ue_asset_path"),
            {EOperation::Clone}),
      Field(TEXT("operation"), TEXT("operation_control"),
            TEXT("apply_operation"), All),
      Field(TEXT("properties"), TEXT("intent"),
            TEXT("property_descriptor_list")),
      Field(TEXT("graph"), TEXT("provider_extension"), TEXT("pcg_graph")),
      Field(TEXT("replace_nodes"), TEXT("provider_extension"), TEXT("bool")),
      Field(TEXT("preserve_existing_nodes"), TEXT("provider_extension"), TEXT("bool")),
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
  Extension->SetBoolField(TEXT("preserve_existing_nodes"), Request.bPreserveExistingNodes);
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
    OutResult.Diagnostic = NativeFailure(TEXT("pcg_workspace_contract_missing"), TEXT("The PCG workspace contract is not registered."),
        TEXT("contract_id"), MakeShared<FJsonValueString>(AssetWorkspaceContractId), MakeShared<FJsonValueString>(TEXT("a registered workspace contract")));
    OutResult.Diagnostic.Stage = TEXT("workspace_inspect");
    return false;
  }
  UeAgentAuthoringWorkspaceLoader::FLoadResult LoadResult;
  if (!UeAgentAuthoringWorkspaceLoader::LoadWorkspace(WorkspaceContract, Source,
                                                      LoadResult)) {
    OutResult.Diagnostic = LoadResult.Diagnostic;
    return false;
  }
  UeAgentAuthoringAssetWorkspaceInput::FTargetRequest TargetRequest;
  TargetRequest.ExpectedAssetPath = ExpectedAssetPath;
  TargetRequest.Errors.EntryInvalid = TEXT("pcg_workspace_entry_invalid");
  TargetRequest.Errors.AssetPathConflict =
      TEXT("pcg_workspace_asset_path_conflict");
  UeAgentAuthoringAssetWorkspaceInput::FTargetResult Target;
  if (!UeAgentAuthoringAssetWorkspaceInput::ResolveTarget(
          LoadResult.Workspace, TargetRequest, Target)) {
    OutResult.Diagnostic = Target.Diagnostic;
    return false;
  }
  const UeAgentApplyInputContract::FOperationInput &OperationInput = Target.Input;
  OutResult.AssetPath = Target.AssetPath;
  OutResult.RequestedOperation =
      OperationToString(OperationInput.RequestedOperation);
  OutResult.bAssetExists = LoadGraph(OutResult.AssetPath) != nullptr;
  OutResult.bWouldCreate =
      OperationInput.RequestedOperation == EOperation::Create ||
      (OperationInput.RequestedOperation == EOperation::Upsert &&
       !OutResult.bAssetExists);

  UeAgentAuthoringWorkspaceDependencyGraph::FDependencyGraph DependencyGraph;
  UeAgentAuthoringWorkspaceDependencyGraph::FGraphFailure GraphFailure;
  if (!UeAgentAuthoringWorkspaceDependencyGraph::BuildDependencyGraph(
          WorkspaceContract, LoadResult.Workspace,
          UeAgentAuthoringWorkspaceDependencyGraph::FDependencyInputs(),
          DependencyGraph, GraphFailure)) {
    OutResult.Diagnostic = GraphFailure.Diagnostic;
    return false;
  }
  FWorkspaceOperationPreflightProvider Provider(
      UeAgentApplyInputContract::FRegistry::Get(), FProviderRegistry::Get());
  UeAgentAuthoringWorkspaceTransaction::FPreflightResult Preflight;
  if (!UeAgentAuthoringWorkspaceTransaction::PreflightWorkspace(
          WorkspaceContract, LoadResult.Workspace, DependencyGraph, Provider,
          Preflight)) {
    const FApplyFailureProjection Failure = ProjectApplyFailure(Preflight);
    OutResult.UnitsChecked = Preflight.UnitsChecked;
    OutResult.Diagnostic = Failure.Diagnostic;
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
    const FString &RequestId,
    const TFunction<bool(FOperationFailure&)>& FinalReadback = {},
    TSharedPtr<const UE::UAI::HjsonWorkspace::FGraphAuthoringConnectionContext> Connections = nullptr) {
  OutResult = FAssetApplyResult();
  OutResult.Status = TEXT("failed");
  UeAgentAuthoringAssetWorkspaceInput::FRequest ResolveRequest;
  ResolveRequest.WorkspaceContractId = AssetWorkspaceContractId;
  ResolveRequest.Source = Source;
  ResolveRequest.ExpectedAssetPath = ExpectedAssetPath;
  ResolveRequest.Errors.ContractMissing = TEXT("pcg_workspace_contract_missing");
  ResolveRequest.Errors.EntryInvalid = TEXT("pcg_workspace_entry_invalid");
  ResolveRequest.Errors.AssetPathConflict =
      TEXT("pcg_workspace_asset_path_conflict");
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
  const FString &AssetPath = ResolvedInput.AssetPath;

  const bool bExisted = LoadGraph(AssetPath) != nullptr;
  bool bSave = false;
  if (OperationInput.ProviderExtension.IsValid())
    OperationInput.ProviderExtension->TryGetBoolField(TEXT("save"), bSave);
  FBackendOptions BackendOptions;
  BackendOptions.FinalReadback = FinalReadback;
  BackendOptions.bUseEditorTransaction =
      OperationInput.RequestedOperation != EOperation::Remove;
  BackendOptions.TransactionDescription =
      NSLOCTEXT("UeAgentInterface", "PCGGraphWorkspaceApply",
                "Apply PCG graph workspace");
  FApplyWorkspaceResult ApplyResult;
  UeAgentAuthoringAssetWorkspaceApply::FFailure ApplyFailure;
  FProviderRegistry LocalProviders;
  const FProviderRegistry* Providers = &FProviderRegistry::Get();
  if (Connections)
  {
    UeAgentAuthoringOperationApply::ERegistrationResult Registered;
    UE::UAI::Contract::FDiagnostic RegistrationError;
    if (!LocalProviders.RegisterProvider(MakeApplyProvider(Connections), Registered, RegistrationError))
    { OutResult.Phase = TEXT("preflight"); OutResult.Diagnostic = RegistrationError; if (OutResult.Diagnostic.Stage.IsEmpty()) OutResult.Diagnostic.Stage = OutResult.Phase; return false; }
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
    OutResult.Phase = ApplyFailure.Phase;
    OutResult.Diagnostic = ApplyFailure.Diagnostic;
    return false;
  }

  const EOperation Resolved =
      OperationInput.RequestedOperation == EOperation::Upsert
          ? (bExisted ? EOperation::Update : EOperation::Create)
          : OperationInput.RequestedOperation;
  OutResult.Status = TEXT("applied");
  OutResult.Phase = TEXT("complete");
  OutResult.ResolvedOperation = OperationToString(Resolved);
  OutResult.AssetPath = AssetPath;
  OutResult.Asset =
      Resolved == EOperation::Remove ? nullptr : LoadGraph(AssetPath);
  OutResult.ObjectPath = OutResult.Asset ? OutResult.Asset->GetPathName()
                                         : UeAgentAuthoringAssetIdentity::ToObjectPath(
                                               AssetPath);
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

bool RegisterApplyContracts(UE::UAI::Contract::FDiagnostic& OutError) {
  UeAgentApplyInputContract::ERegistrationResult InputResult;
  if (!UeAgentApplyInputContract::FRegistry::Get().RegisterContract(
          MakeInputContract(), InputResult, OutError))
    return false;
  UeAgentApplyWorkspaceContract::ERegistrationResult WorkspaceResult;
  return UeAgentApplyWorkspaceContract::FRegistry::Get().RegisterContract(
      MakeWorkspaceContract(), WorkspaceResult, OutError);
}

TSharedRef<IApplyProvider> MakeApplyProvider(
    TSharedPtr<const UE::UAI::HjsonWorkspace::FGraphAuthoringConnectionContext> Connections) {
  const TSharedRef<IReflectionOperationLifecycle> Lifecycle =
      MakeShared<FAssetLifecycle>(MoveTemp(Connections));
  UeAgentAuthoringObjectApply::FOptions Options;
  Options.bAllowDomainAuthorizedOwners = true;
  Options.bCompactValues = true;
  return MakeShared<FReflectionApplyProvider>(ApplyProviderId, Lifecycle,
                                              Options);
}

bool ApplyAsset(const FAssetApplyRequest &Request, FAssetApplyResult &OutResult,
                const FString &RequestId) {
  return ExecuteWorkspaceSource(
      UeAgentAuthoringWorkspaceLoader::FSourceRequest::MakeInlineObject(
          MakeApplyDocument(Request), TEXT("inline://pcg-graph-apply")),
      Request.AssetPath, OutResult, RequestId, Request.FinalReadback, Request.Connections);
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

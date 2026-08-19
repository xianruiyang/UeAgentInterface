// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringApplyProvider.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringAssets.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringDescriptors.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringProfileState.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringState.h"
#include "Misc/PackageName.h"
#include "ObjectTools.h"
#include "UObject/StrongObjectPtr.h"
#include "Utilities/AuthoringOperationApply/UeAgentAuthoringAssetRemovalStage.h"
#include "Utilities/AuthoringOperationApply/UeAgentAuthoringObjectApplyProvider.h"
#include "Utilities/AuthoringOperationApply/UeAgentAuthoringOperationCoordinator.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentApplyWorkspaceContract.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringWorkspaceDependencyGraph.h"
#include "Utilities/AuthoringWorkspaceApply/UeAgentAuthoringWorkspaceTransaction.h"

namespace UeAgentReflectionAuthoring {
namespace {
using namespace UeAgentApplyInputContract;
using namespace UeAgentAuthoringOperationApply;

void SetFailure(FOperationFailure &OutFailure, const FString &Code,
                const FString &Detail = FString()) {
  OutFailure = FOperationFailure();
  OutFailure.ErrorCode = Code;
  OutFailure.ErrorDetail = Detail.IsEmpty() ? Code : Detail;
}

bool ReadBool(const TSharedPtr<const FJsonObject> &Object, const TCHAR *Field,
              const bool DefaultValue) {
  bool Value = DefaultValue;
  if (Object.IsValid())
    Object->TryGetBoolField(Field, Value);
  return Value;
}

void RetireTransientAsset(TStrongObjectPtr<UObject> &Asset,
                          UObject *PreservedAsset = nullptr) {
  if (!Asset.IsValid())
    return;
  if (Asset.Get() != PreservedAsset) {
    Asset->ClearFlags(RF_Public | RF_Standalone);
    Asset->MarkAsGarbage();
  }
  Asset.Reset();
}

class FAssetLifecycleState final : public IReflectionOperationLifecycleState {
public:
  FAssetLifecycleState(const FProfile &InProfile, FString InPackagePath,
                       const EOperation InOperation, UObject *InExisting,
                       UObject *InProposed,
                       TSharedPtr<FJsonValue> InProfileStatePatch,
                       const bool bInHasProfileStatePatch, const bool bInSave)
      : Profile(InProfile), PackagePath(MoveTemp(InPackagePath)),
        Operation(InOperation), Existing(InExisting), Proposed(InProposed),
        ProfileStatePatch(MoveTemp(InProfileStatePatch)),
        bHasProfileStatePatch(bInHasProfileStatePatch), bSave(bInSave) {}

  virtual ~FAssetLifecycleState() override {
    RetireTransientAsset(Proposed, Actual);
  }

  virtual FString GetTargetKey() const override { return PackagePath; }
  virtual EOperation GetResolvedOperation() const override { return Operation; }
  virtual bool IsStateNoOp() const override {
    return Operation == EOperation::Update && !bHasProfileStatePatch;
  }
  virtual bool IsNoOp() const override { return IsStateNoOp() && !bSave; }
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
    if (!bHasProfileStatePatch || Operation == EOperation::Remove)
      return true;
    if (!Proposed.IsValid()) {
      SetFailure(OutFailure, TEXT("reflection_profile_state_proposed_missing"),
                 PackagePath);
      return false;
    }
    FString Error;
    if (!ProfileState::Apply(Profile, *Proposed.Get(), ProfileStatePatch,
                             Error)) {
      SetFailure(OutFailure, Error);
      return false;
    }
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
      Actual = Existing;
      if (bHasProfileStatePatch) {
        FString Error;
        if (!ProfileState::Apply(Profile, *Actual, ProfileStatePatch, Error)) {
          SetFailure(OutFailure, Error);
          return false;
        }
      }
      return true;
    }
    FString Error;
    Actual = PublishAsset(Proposed.Get(), PackagePath, Error);
    if (!Actual) {
      SetFailure(OutFailure, Error);
      return false;
    }
    Actual->PostEditChange();
    return true;
  }

  virtual bool Compile(FOperationFailure &OutFailure) override {
    OutFailure = FOperationFailure();
    if (!bHasProfileStatePatch || Operation == EOperation::Remove)
      return true;
    UObject *Asset = Actual ? Actual : (Existing ? Existing : Proposed.Get());
    if (!Asset) {
      SetFailure(OutFailure,
                 TEXT("reflection_profile_state_compile_target_missing"),
                 PackagePath);
      return false;
    }
    FString Error;
    if (!ProfileState::CompileIfNeeded(Profile, *Asset, Error)) {
      SetFailure(OutFailure, Error);
      return false;
    }
    return true;
  }

  virtual bool Save(FOperationFailure &OutFailure) override {
    OutFailure = FOperationFailure();
    if (!bSave || Operation == EOperation::Remove)
      return true;
    UObject *Asset = Actual ? Actual : Existing;
    FString Error;
    if (!Asset || !SaveAsset(*Asset, Error)) {
      SetFailure(OutFailure, Error.IsEmpty()
                                 ? TEXT("reflection_asset_save_target_missing")
                                 : Error);
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
    Result->SetStringField(TEXT("profile"), Profile.TypeId);
    Result->SetStringField(TEXT("class_path"), Profile.AssetClassPath);
    return Result;
  }

  virtual bool Commit(FOperationFailure &OutFailure) override {
    OutFailure = FOperationFailure();
    if (Operation == EOperation::Remove && RemovalStage.IsStaged()) {
      FString Error;
      if (!DeleteAsset(RemovalStage.GetAsset(), Error)) {
        SetFailure(OutFailure, Error);
        return false;
      }
      RemovalStage.Reset();
      Existing = nullptr;
    }
    RetireTransientAsset(Proposed, Actual);
    return true;
  }

  virtual bool Rollback(FOperationFailure &OutFailure) override {
    OutFailure = FOperationFailure();
    FString Error;
    if ((Operation == EOperation::Create || Operation == EOperation::Clone) &&
        Actual) {
      if (!DeleteAsset(Actual, Error)) {
        TArray<UObject *> Objects = {Actual};
        if (ObjectTools::DeleteObjectsUnchecked(Objects) <= 0) {
          SetFailure(OutFailure, Error);
          return false;
        }
      }
      Actual = nullptr;
    }
    if (Operation == EOperation::Remove && RemovalStage.IsCaptured()) {
      if (!RemovalStage.Rollback(Error)) {
        SetFailure(OutFailure, Error);
        return false;
      }
      Actual = Existing;
    }
    RetireTransientAsset(Proposed, Actual);
    return true;
  }

private:
  bool
  BuildReadRequests(UObject *Asset, const bool bExpectedAbsent,
                    TArray<UeAgentAuthoringState::FReadRequest> &OutRequests,
                    FOperationFailure &OutFailure) const {
    FString Error;
    if (!BuildAssetSnapshotReadRequests(Profile, Asset, PackagePath,
                                        bExpectedAbsent, OutRequests, Error)) {
      SetFailure(OutFailure, TEXT("reflection_snapshot_request_failed"), Error);
      return false;
    }
    return true;
  }

  FProfile Profile;
  FString PackagePath;
  EOperation Operation;
  UObject *Existing = nullptr;
  TStrongObjectPtr<UObject> Proposed;
  TSharedPtr<FJsonValue> ProfileStatePatch;
  FAssetRemovalStage RemovalStage;
  UObject *Actual = nullptr;
  bool bHasProfileStatePatch = false;
  bool bSave = false;
};

class FAssetLifecycle final : public IReflectionOperationLifecycle {
public:
  virtual bool
  PrepareLifecycle(const FPrepareRequest &Request,
                   TUniquePtr<IReflectionOperationLifecycleState> &OutState,
                   FOperationFailure &OutFailure) const override {
    OutState.Reset();
    OutFailure = FOperationFailure();
    if (!Request.IsValid() || !Request.Input->Target.IsValid() ||
        !Request.Contract) {
      SetFailure(OutFailure, TEXT("reflection_apply_request_invalid"));
      return false;
    }
    const FProfile *Profile =
        FindProfileByTargetKind(Request.Contract->TargetKind);
    if (!Profile) {
      SetFailure(OutFailure, TEXT("reflection_apply_profile_missing"),
                 Request.Contract->TargetKind);
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
    FString AssetKind;
    if (Request.Input->Target->TryGetStringField(TEXT("asset_kind"),
                                                 AssetKind) &&
        !AssetKind.Equals(Profile->TypeId, ESearchCase::IgnoreCase)) {
      SetFailure(OutFailure, TEXT("reflection_asset_kind_conflict"), AssetKind);
      return false;
    }
    FString RequestedClassPath;
    UClass *RequestedClass = nullptr;
    if (Request.Input->Target->TryGetStringField(TEXT("class_path"),
                                                 RequestedClassPath) &&
        !RequestedClassPath.TrimStartAndEnd().IsEmpty()) {
      RequestedClass =
          FSoftClassPath(RequestedClassPath).TryLoadClass<UObject>();
      FString ClassError;
      UClass *AcceptedClass = ResolveAssetClass(*Profile, false, ClassError);
      if (!RequestedClass || !AcceptedClass ||
          !RequestedClass->IsChildOf(AcceptedClass)) {
        SetFailure(OutFailure, TEXT("reflection_asset_class_not_allowed"),
                   RequestedClassPath);
        return false;
      }
    }

    UObject *Existing = LoadAsset(*Profile, PackagePath);
    EOperation Operation = Request.Input->RequestedOperation;
    if (Operation == EOperation::Upsert)
      Operation = Existing ? EOperation::Update : EOperation::Create;
    TSharedPtr<FJsonValue> ProfileStatePatch;
    bool bHasProfileStatePatch = false;
    FString ProfileStateError;
    if (!ProfileState::PreparePatch(*Profile, Request.Input->ProviderExtension,
                                    ProfileStatePatch, bHasProfileStatePatch,
                                    ProfileStateError)) {
      SetFailure(OutFailure, ProfileStateError);
      return false;
    }
    if (Operation == EOperation::Remove && bHasProfileStatePatch) {
      SetFailure(OutFailure,
                 TEXT("reflection_profile_state_remove_not_allowed"),
                 Profile->TypeId);
      return false;
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
    if (Existing && RequestedClass && Existing->GetClass() != RequestedClass) {
      SetFailure(OutFailure, TEXT("reflection_asset_class_conflict"),
                 Existing->GetClass()->GetPathName() + TEXT(" != ") +
                     RequestedClassPath);
      return false;
    }

    UObject *Source = nullptr;
    if (Operation == EOperation::Clone) {
      FString SourcePath;
      Request.Input->Target->TryGetStringField(TEXT("source_asset_path"),
                                               SourcePath);
      Source = LoadAsset(*Profile, SourcePath);
      if (!Source) {
        SetFailure(OutFailure, TEXT("source_asset_not_found"), SourcePath);
        return false;
      }
      if (RequestedClass && Source->GetClass() != RequestedClass) {
        SetFailure(OutFailure, TEXT("reflection_asset_class_conflict"),
                   Source->GetClass()->GetPathName() + TEXT(" != ") +
                       RequestedClassPath);
        return false;
      }
    }

    UObject *Proposed = nullptr;
    if (Operation != EOperation::Remove) {
      UPackage *Scratch = CreatePackage(
          *FString::Printf(TEXT("/Temp/UAI/Reflection/Proposed_%s"),
                           *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
      FString Error;
      Proposed =
          Operation == EOperation::Create
              ? CreateFactorySeed(*Profile, Scratch, NAME_None,
                                  RF_Public | RF_Standalone | RF_Transactional,
                                  Error, RequestedClass)
              : DuplicateAsset(
                    Operation == EOperation::Clone ? Source : Existing, Scratch,
                    NAME_None, RF_Public | RF_Standalone | RF_Transactional);
      if (!Proposed) {
        SetFailure(OutFailure,
                   Error.IsEmpty()
                       ? TEXT("reflection_proposed_asset_create_failed")
                       : Error);
        return false;
      }
    }

    OutState = MakeUnique<FAssetLifecycleState>(
        *Profile, PackagePath, Operation, Existing, Proposed,
        MoveTemp(ProfileStatePatch), bHasProfileStatePatch,
        ReadBool(Request.Input->ProviderExtension, TEXT("save"), false));
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

FContract MakeInputContract(const FProfile &Profile) {
  FContract Result;
  Result.Schema = TEXT("uai.apply_input_contract.v1");
  Result.ContractId = Profile.InputContractId;
  Result.ContractVersion = TEXT("1.0.0");
  Result.ProfileId = Profile.TypeId;
  Result.TargetKind = Profile.TargetKind;
  Result.SemanticDescriptorRefs = {Profile.SeedSemanticId,
                                   Profile.ExportTargetSemanticId};
  if (!Profile.ProfileStateSemanticId.IsEmpty()) {
    Result.SemanticDescriptorRefs.Add(Profile.ProfileStateSemanticId);
  }
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
  TArray<EOperation> Operations = {EOperation::Update, EOperation::Remove,
                                   EOperation::Clone};
  if (Profile.bAllowCreate) {
    Operations.Insert(EOperation::Create, 0);
    Operations.Add(EOperation::Upsert);
  }
  Result.InputFields = {
      Field(TEXT("asset_path"), TEXT("identity"), TEXT("ue_asset_path"),
            Operations),
      Field(TEXT("asset_kind"), TEXT("type_assertion"), *Profile.TypeId),
      Field(TEXT("class_path"), TEXT("type_assertion"), TEXT("ue_class_path")),
      Field(TEXT("source_asset_path"), TEXT("selector"), TEXT("ue_asset_path"),
            {EOperation::Clone}),
      Field(TEXT("operation"), TEXT("operation_control"),
            TEXT("apply_operation"), Operations),
      Field(TEXT("properties"), TEXT("intent"),
            TEXT("property_descriptor_list")),
      Field(TEXT("save"), TEXT("provider_extension"), TEXT("bool")),
  };
  if (!Profile.ProfileStateField.IsEmpty()) {
    Result.InputFields.Add(Field(*Profile.ProfileStateField,
                                 TEXT("provider_extension"),
                                 *Profile.ProfileStateInputType));
  }
  for (const EOperation Item : Operations)
    Result.Operations.Add(OperationDescriptor(Item));
  Result.ProviderId = ApplyProviderId;
  Result.UnknownInputFieldPolicy = TEXT("reject");
  Result.Verification.StateSource = TEXT("shared_state_snapshot");
  Result.Verification.CompareSource = TEXT("shared_semantic_compare");
  Result.Verification.bRequired = true;
  return Result;
}

UeAgentApplyWorkspaceContract::FContract
MakeWorkspaceContract(const FProfile &Profile) {
  using namespace UeAgentApplyWorkspaceContract;
  UeAgentApplyWorkspaceContract::FContract Result;
  Result.Schema = TEXT("uai.apply_workspace_contract.v1");
  Result.ContractId = Profile.WorkspaceContractId;
  Result.ContractVersion = TEXT("1.0.0");
  Result.ProfileId = Profile.TypeId;
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
  Unit.RouteId = Profile.TypeId;
  Unit.ApplyContractRef = Profile.InputContractId;
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

bool ReadWorkspaceIdentity(
    const UeAgentAuthoringWorkspaceRouter::FWorkspace &Workspace,
    FString &OutAssetPath, EOperation &OutOperation,
    TSharedPtr<const FJsonObject> &OutDocument) {
  const UeAgentAuthoringWorkspaceRouter::FWorkspaceUnit *Unit =
      Workspace.FindSingleUnit(TEXT("asset"));
  OutDocument = Unit ? Unit->Document : nullptr;
  const TSharedPtr<FJsonObject> *Target = nullptr;
  FString OperationText;
  return OutDocument.IsValid() &&
         OutDocument->TryGetStringField(TEXT("operation"), OperationText) &&
         ParseOperation(OperationText, OutOperation) &&
         OutDocument->TryGetObjectField(TEXT("target"), Target) && Target &&
         Target->IsValid() &&
         (*Target)->TryGetStringField(TEXT("asset_path"), OutAssetPath);
}

bool ValidateExpectedPath(const FString &Actual, const FString &Expected,
                          FString &OutErrorCode, FString &OutErrorDetail) {
  if (Expected.IsEmpty() || Actual.Equals(Expected, ESearchCase::IgnoreCase))
    return true;
  OutErrorCode = TEXT("reflection_workspace_asset_path_conflict");
  OutErrorDetail = Actual + TEXT(" != ") + Expected;
  return false;
}

bool BuildDependencyGraph(
    const UeAgentApplyWorkspaceContract::FContract &Contract,
    const UeAgentAuthoringWorkspaceRouter::FWorkspace &Workspace,
    UeAgentAuthoringWorkspaceDependencyGraph::FDependencyGraph &OutGraph,
    FString &OutErrorCode, FString &OutErrorDetail) {
  UeAgentAuthoringWorkspaceDependencyGraph::FGraphFailure Failure;
  if (UeAgentAuthoringWorkspaceDependencyGraph::BuildDependencyGraph(
          Contract, Workspace,
          UeAgentAuthoringWorkspaceDependencyGraph::FDependencyInputs(),
          OutGraph, Failure))
    return true;
  OutErrorCode = Failure.ErrorCode;
  OutErrorDetail = Failure.ErrorDetail;
  return false;
}

bool LoadWorkspace(
    const FProfile &Profile,
    const UeAgentAuthoringWorkspaceLoader::FSourceRequest &Source,
    UeAgentApplyWorkspaceContract::FContract &OutContract,
    UeAgentAuthoringWorkspaceLoader::FLoadResult &OutLoad,
    FString &OutErrorCode, FString &OutErrorDetail) {
  if (!UeAgentApplyWorkspaceContract::FRegistry::Get().FindContract(
          Profile.WorkspaceContractId, OutContract)) {
    OutErrorCode = TEXT("reflection_workspace_contract_missing");
    return false;
  }
  if (!UeAgentAuthoringWorkspaceLoader::LoadWorkspace(OutContract, Source,
                                                      OutLoad)) {
    OutErrorCode = OutLoad.ErrorCode;
    OutErrorDetail = OutLoad.ErrorDetail;
    return false;
  }
  return true;
}
} // namespace

TSharedRef<FJsonObject> FAssetApplyResult::ToJsonObject() const {
  TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
  Result->SetStringField(TEXT("status"), Status);
  Result->SetStringField(TEXT("phase"), Phase);
  Result->SetStringField(TEXT("profile"), ProfileId);
  Result->SetStringField(TEXT("asset_path"), AssetPath);
  Result->SetStringField(TEXT("resolved_operation"), ResolvedOperation);
  Result->SetNumberField(TEXT("units_checked"), UnitsChecked);
  Result->SetNumberField(TEXT("mutations_applied"), MutationsApplied);
  Result->SetBoolField(TEXT("created"), bCreated);
  Result->SetBoolField(TEXT("removed"), bRemoved);
  Result->SetBoolField(TEXT("no_op"), bNoOp);
  Result->SetBoolField(TEXT("rollback_attempted"), bRollbackAttempted);
  Result->SetBoolField(TEXT("rollback_succeeded"), bRollbackSucceeded);
  if (!ErrorCode.IsEmpty())
    Result->SetStringField(TEXT("error_code"), ErrorCode);
  if (!ErrorDetail.IsEmpty())
    Result->SetStringField(TEXT("error_detail"), ErrorDetail);
  return Result;
}

TSharedRef<FJsonObject> FWorkspaceInspectResult::ToJsonObject() const {
  TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
  Result->SetStringField(TEXT("status"), Status);
  Result->SetStringField(TEXT("profile"), ProfileId);
  Result->SetStringField(TEXT("asset_path"), AssetPath);
  Result->SetStringField(TEXT("requested_operation"), RequestedOperation);
  Result->SetNumberField(TEXT("units_checked"), UnitsChecked);
  Result->SetBoolField(TEXT("asset_exists"), bAssetExists);
  Result->SetBoolField(TEXT("would_create"), bWouldCreate);
  Result->SetBoolField(TEXT("no_op"), bNoOp);
  if (!ErrorCode.IsEmpty())
    Result->SetStringField(TEXT("error_code"), ErrorCode);
  if (!ErrorDetail.IsEmpty())
    Result->SetStringField(TEXT("error_detail"), ErrorDetail);
  return Result;
}

bool RegisterApplyContracts(FString &OutError) {
  for (const FProfile &Profile : GetProfiles()) {
    UeAgentApplyInputContract::ERegistrationResult InputResult;
    if (!UeAgentApplyInputContract::FRegistry::Get().RegisterContract(
            MakeInputContract(Profile), InputResult, OutError))
      return false;
    UeAgentApplyWorkspaceContract::ERegistrationResult WorkspaceResult;
    if (!UeAgentApplyWorkspaceContract::FRegistry::Get().RegisterContract(
            MakeWorkspaceContract(Profile), WorkspaceResult, OutError))
      return false;
  }
  return true;
}

TSharedRef<IApplyProvider> MakeApplyProvider() {
  static const TSharedRef<IReflectionOperationLifecycle> Lifecycle =
      MakeShared<FAssetLifecycle>();
  UeAgentAuthoringObjectApply::FOptions Options;
  Options.bAllowDomainAuthorizedOwners = true;
  Options.bEmitEditorNotifications = false;
  return MakeShared<FReflectionApplyProvider>(ApplyProviderId, Lifecycle,
                                              Options);
}

bool InspectAssetSource(
    const FProfile &Profile,
    const UeAgentAuthoringWorkspaceLoader::FSourceRequest &Source,
    const FString &ExpectedAssetPath, FWorkspaceInspectResult &OutResult) {
  OutResult = FWorkspaceInspectResult();
  OutResult.Status = TEXT("failed");
  OutResult.ProfileId = Profile.TypeId;
  UeAgentApplyWorkspaceContract::FContract Contract;
  UeAgentAuthoringWorkspaceLoader::FLoadResult Load;
  if (!LoadWorkspace(Profile, Source, Contract, Load, OutResult.ErrorCode,
                     OutResult.ErrorDetail))
    return false;
  TSharedPtr<const FJsonObject> Document;
  EOperation Operation;
  if (!ReadWorkspaceIdentity(Load.Workspace, OutResult.AssetPath, Operation,
                             Document)) {
    OutResult.ErrorCode = TEXT("reflection_workspace_entry_invalid");
    return false;
  }
  OutResult.AssetPath = NormalizePackagePath(OutResult.AssetPath);
  OutResult.RequestedOperation = OperationToString(Operation);
  if (!ValidateExpectedPath(OutResult.AssetPath,
                            NormalizePackagePath(ExpectedAssetPath),
                            OutResult.ErrorCode, OutResult.ErrorDetail))
    return false;
  OutResult.bAssetExists = LoadAsset(Profile, OutResult.AssetPath) != nullptr;
  OutResult.bWouldCreate =
      Operation == EOperation::Create ||
      (Operation == EOperation::Upsert && !OutResult.bAssetExists);
  UeAgentAuthoringWorkspaceDependencyGraph::FDependencyGraph Graph;
  if (!BuildDependencyGraph(Contract, Load.Workspace, Graph,
                            OutResult.ErrorCode, OutResult.ErrorDetail))
    return false;
  FWorkspaceOperationPreflightProvider Provider(
      UeAgentApplyInputContract::FRegistry::Get(), FProviderRegistry::Get());
  UeAgentAuthoringWorkspaceTransaction::FPreflightResult Preflight;
  if (!UeAgentAuthoringWorkspaceTransaction::PreflightWorkspace(
          Contract, Load.Workspace, Graph, Provider, Preflight)) {
    OutResult.UnitsChecked = Preflight.UnitsChecked;
    OutResult.ErrorCode = Preflight.CauseCode.IsEmpty() ? Preflight.ErrorCode
                                                        : Preflight.CauseCode;
    OutResult.ErrorDetail = Preflight.ErrorDetail;
    return false;
  }
  OutResult.UnitsChecked = Preflight.UnitsChecked;
  OutResult.bNoOp = Preflight.Units.Num() == 1 &&
                    Preflight.Units[0].Mutation.IsValid() &&
                    Preflight.Units[0].Mutation->IsStateNoOp();
  OutResult.Status = TEXT("ready");
  return true;
}

bool ApplyAssetSource(
    const FProfile &Profile,
    const UeAgentAuthoringWorkspaceLoader::FSourceRequest &Source,
    const FString &ExpectedAssetPath, FAssetApplyResult &OutResult,
    const TOptional<bool> SaveOverride, const FString &RequestId) {
  OutResult = FAssetApplyResult();
  OutResult.Status = TEXT("failed");
  OutResult.ProfileId = Profile.TypeId;
  UeAgentApplyWorkspaceContract::FContract Contract;
  UeAgentAuthoringWorkspaceLoader::FLoadResult Load;
  if (!LoadWorkspace(Profile, Source, Contract, Load, OutResult.ErrorCode,
                     OutResult.ErrorDetail)) {
    OutResult.Phase = TEXT("load");
    return false;
  }
  TSharedPtr<const FJsonObject> Document;
  EOperation RequestedOperation;
  if (!ReadWorkspaceIdentity(Load.Workspace, OutResult.AssetPath,
                             RequestedOperation, Document)) {
    OutResult.Phase = TEXT("load");
    OutResult.ErrorCode = TEXT("reflection_workspace_entry_invalid");
    return false;
  }
  OutResult.AssetPath = NormalizePackagePath(OutResult.AssetPath);
  if (!ValidateExpectedPath(OutResult.AssetPath,
                            NormalizePackagePath(ExpectedAssetPath),
                            OutResult.ErrorCode, OutResult.ErrorDetail)) {
    OutResult.Phase = TEXT("preflight");
    return false;
  }
  UeAgentAuthoringWorkspaceDependencyGraph::FDependencyGraph Graph;
  if (!BuildDependencyGraph(Contract, Load.Workspace, Graph,
                            OutResult.ErrorCode, OutResult.ErrorDetail)) {
    OutResult.Phase = TEXT("preflight");
    return false;
  }
  const bool bExisted = LoadAsset(Profile, OutResult.AssetPath) != nullptr;
  bool bSave = false;
  const TSharedPtr<FJsonObject> *Extension = nullptr;
  if (Document->TryGetObjectField(TEXT("provider_extension"), Extension) &&
      Extension && Extension->IsValid())
    (*Extension)->TryGetBoolField(TEXT("save"), bSave);
  if (SaveOverride.IsSet())
    bSave = SaveOverride.GetValue();
  FBackendOptions BackendOptions;
  BackendOptions.bUseEditorTransaction =
      RequestedOperation != EOperation::Remove;
  BackendOptions.TransactionDescription =
      NSLOCTEXT("UeAgentInterface", "ReflectionAssetWorkspaceApply",
                "Apply reflection asset workspace");
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
      Contract, Load.Workspace, Graph,
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
  OutResult.Status = TEXT("ok");
  OutResult.Phase = TEXT("complete");
  OutResult.ResolvedOperation = OperationToString(Resolved);
  OutResult.bCreated =
      Resolved == EOperation::Create || Resolved == EOperation::Clone;
  OutResult.bRemoved = Resolved == EOperation::Remove;
  OutResult.bNoOp = ApplyResult.Execution.MutationsApplied == 0;
  return true;
}

bool ApplyAsset(const FAssetApplyRequest &Request, FAssetApplyResult &OutResult,
                const FString &RequestId) {
  const FProfile *Profile = FindProfileByTypeId(Request.TypeId);
  if (!Profile) {
    OutResult = FAssetApplyResult();
    OutResult.ErrorCode = TEXT("reflection_apply_profile_missing");
    return false;
  }
  TSharedRef<FJsonObject> Target = MakeShared<FJsonObject>();
  Target->SetStringField(TEXT("asset_path"),
                         NormalizePackagePath(Request.AssetPath));
  Target->SetStringField(TEXT("asset_kind"), Profile->TypeId);
  if (!Request.ClassPath.IsEmpty())
    Target->SetStringField(TEXT("class_path"), Request.ClassPath);
  if (!Request.SourceAssetPath.IsEmpty())
    Target->SetStringField(TEXT("source_asset_path"),
                           NormalizePackagePath(Request.SourceAssetPath));
  TSharedRef<FJsonObject> Extension = MakeShared<FJsonObject>();
  if (Request.ProviderExtension.IsValid()) {
    for (const TPair<FString, TSharedPtr<FJsonValue>> &Pair :
         Request.ProviderExtension->Values) {
      Extension->SetField(Pair.Key, Pair.Value);
    }
  }
  Extension->SetBoolField(TEXT("save"), Request.bSave);
  TSharedRef<FJsonObject> Document = MakeShared<FJsonObject>();
  Document->SetStringField(TEXT("operation"),
                           OperationToString(Request.Operation));
  Document->SetObjectField(TEXT("target"), Target);
  Document->SetObjectField(TEXT("provider_extension"), Extension);
  if (!Request.Properties.IsEmpty())
    Document->SetArrayField(TEXT("properties"), Request.Properties);
  return ApplyAssetSource(
      *Profile,
      UeAgentAuthoringWorkspaceLoader::FSourceRequest::MakeInlineObject(
          Document, TEXT("inline://reflection-asset-apply")),
      Request.AssetPath, OutResult, TOptional<bool>(), RequestId);
}
} // namespace UeAgentReflectionAuthoring

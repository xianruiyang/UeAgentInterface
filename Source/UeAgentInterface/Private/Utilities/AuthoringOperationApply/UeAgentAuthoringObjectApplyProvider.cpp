// Copyright Epic Games, Inc. All Rights Reserved.

#include "Utilities/AuthoringOperationApply/UeAgentAuthoringObjectApplyProvider.h"

#include "Dom/JsonObject.h"
#include "UObject/Class.h"
#include "UObject/Object.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringIdentity.h"

namespace {
using namespace UeAgentAuthoringOperationApply;

void SetFailure(FOperationFailure &OutFailure, const FString &ErrorCode,
                const FString &ErrorDetail = FString()) {
  OutFailure = FOperationFailure();
  OutFailure.ErrorCode = ErrorCode;
  OutFailure.ErrorDetail = ErrorDetail.IsEmpty() ? ErrorCode : ErrorDetail;
}

void SetObjectApplyFailure(
    FOperationFailure &OutFailure,
    const UeAgentAuthoringObjectApply::FFailure &Failure) {
  SetFailure(OutFailure,
             Failure.ErrorCode.IsEmpty()
                 ? TEXT("object_operation_prepare_failed")
                 : Failure.ErrorCode,
             Failure.ErrorDetail);
  OutFailure.Diagnostic = Failure.Diagnostic;
}

void SetObjectApplyFailure(
    FOperationFailure &OutFailure,
    const UeAgentAuthoringObjectApply::FApplyResult &Result) {
  SetFailure(OutFailure,
             Result.ErrorCode.IsEmpty() ? TEXT("object_operation_apply_failed")
                                        : Result.ErrorCode,
             Result.ErrorDetail);
}

bool BuildSnapshotIdentity(
    const UeAgentAuthoringIdentity::FCanonicalIdentity &NativeIdentity,
    const FString &TargetKey, const FString &PropertyPath,
    UeAgentAuthoringIdentity::FCanonicalIdentity &OutIdentity,
    FString &OutError) {
  return UeAgentAuthoringIdentity::BuildCanonicalIdentity(
      NativeIdentity.ModelId, NativeIdentity.TargetKind,
      {
          {TEXT("owner_path"), TargetKey},
          {TEXT("property_path"), PropertyPath},
      },
      OutIdentity, OutError);
}

bool AppendPropertyReadRequestsInternal(
    const FReflectionRoot &Root, const FString &TargetKey,
    const TArray<UeAgentAuthoringObjectApply::FResolvedNode> &Nodes,
    TArray<UeAgentAuthoringState::FReadRequest> &OutRequests,
    FOperationFailure &OutFailure) {
  if (!Root.IsValid()) {
    SetFailure(OutFailure, TEXT("reflection_operation_readback_root_missing"),
               TargetKey);
    return false;
  }
  for (const UeAgentAuthoringObjectApply::FResolvedNode &Node : Nodes) {
    UeAgentAuthoringState::FReadRequest ReadRequest;
    UeAgentAuthoringTargetLocator::FResolveResult ResolveResult;
    const bool bReadRequestBuilt =
        Root.Kind == EReflectionRootKind::Object
            ? UeAgentAuthoringState::BuildPropertyReadRequest(
                  Root.Object, Node.PropertyPath, TEXT("core.property"),
                  ReadRequest, ResolveResult)
            : UeAgentAuthoringState::BuildStructPropertyReadRequest(
                  Root.StructType, Root.StructData, Root.OwnerPath,
                  Root.NotificationOwner, Node.PropertyPath,
                  TEXT("core.property"), ReadRequest, ResolveResult);
    if (!bReadRequestBuilt) {
      SetFailure(OutFailure,
                 ResolveResult.ErrorCode.IsEmpty()
                     ? TEXT("object_operation_readback_property_unresolved")
                     : ResolveResult.ErrorCode,
                 Node.PropertyPath);
      return false;
    }
    UeAgentAuthoringIdentity::FCanonicalIdentity SnapshotIdentity;
    FString IdentityError;
    if (!BuildSnapshotIdentity(ReadRequest.TargetIdentity, TargetKey,
                               Node.PropertyPath, SnapshotIdentity,
                               IdentityError)) {
      SetFailure(OutFailure,
                 IdentityError.IsEmpty()
                     ? TEXT("reflection_operation_readback_identity_invalid")
                     : IdentityError,
                 Node.PropertyPath);
      return false;
    }
    ReadRequest.SnapshotIdentityOverride = MoveTemp(SnapshotIdentity);
    OutRequests.Add(MoveTemp(ReadRequest));
  }
  return true;
}

class FReflectionPreparedOperationState final : public IPreparedOperationState {
public:
  FReflectionPreparedOperationState(
      TUniquePtr<IReflectionOperationLifecycleState> InLifecycleState,
      UeAgentAuthoringObjectApply::FPreparedApply InPropertyApply,
      bool bInHasPropertyApply,
      TArray<UeAgentAuthoringObjectApply::FResolvedNode> InResolvedNodes,
      TArray<TSharedPtr<FJsonValue>> InPropertyDescriptors,
      UeAgentAuthoringObjectApply::FOptions InObjectApplyOptions)
      : LifecycleState(MoveTemp(InLifecycleState)),
        PropertyApply(MoveTemp(InPropertyApply)),
        bHasPropertyApply(bInHasPropertyApply),
        ResolvedNodes(MoveTemp(InResolvedNodes)),
        PropertyDescriptors(MoveTemp(InPropertyDescriptors)),
        ObjectApplyOptions(MoveTemp(InObjectApplyOptions)) {}

  virtual FString GetTargetKey() const override {
    return LifecycleState->GetTargetKey();
  }

  virtual UeAgentApplyInputContract::EOperation
  GetResolvedOperation() const override {
    return LifecycleState->GetResolvedOperation();
  }

  virtual bool IsNoOp() const override {
    return !bHasPropertyApply && LifecycleState->IsNoOp();
  }

  virtual bool IsStateNoOp() const override {
    return !bHasPropertyApply && LifecycleState->IsStateNoOp();
  }

  virtual void
  CollectRollbackObjects(TArray<UObject *> &OutObjects) const override {
    auto AddRoot = [&OutObjects](const FReflectionRoot &Root) {
      UObject *Object = Root.Kind == EReflectionRootKind::Object
                            ? Root.Object
                            : Root.NotificationOwner;
      if (Object)
        OutObjects.AddUnique(Object);
    };
    AddRoot(LifecycleState->GetProposedRoot());
    AddRoot(LifecycleState->GetActualRoot());
  }

  virtual bool CaptureRollback(FOperationFailure &OutFailure) override {
    return LifecycleState->CaptureRollback(OutFailure);
  }

  virtual bool ApplyProposed(FOperationFailure &OutFailure) override {
    if (bHasPropertyApply) {
      UeAgentAuthoringObjectApply::FApplyResult Result;
      if (!UeAgentAuthoringObjectApply::ApplyPrepared(PropertyApply, Result) ||
          !UeAgentAuthoringObjectApply::VerifyPrepared(PropertyApply, Result)) {
        SetObjectApplyFailure(OutFailure, Result);
        return false;
      }
    }
    if (!LifecycleState->ApplyProposed(OutFailure))
      return false;
    if (bHasPropertyApply && !ObjectApplyOptions.bEmitEditorNotifications) {
      const FReflectionRoot ProposedRoot = LifecycleState->GetProposedRoot();
      UObject *NotificationOwner =
          ProposedRoot.Kind == EReflectionRootKind::Object
              ? ProposedRoot.Object
              : ProposedRoot.NotificationOwner;
      if (NotificationOwner)
        NotificationOwner->PostEditChange();
    }
    return true;
  }

  virtual bool BuildExpectedReadRequests(
      TArray<UeAgentAuthoringState::FReadRequest> &OutRequests,
      FOperationFailure &OutFailure) const override {
    OutRequests.Reset();
    if (!LifecycleState->BuildDomainExpectedReadRequests(OutRequests,
                                                         OutFailure))
      return false;
    if (!bHasPropertyApply)
      return true;
    return UeAgentAuthoringOperationApply::AppendPropertyReadRequests(
        LifecycleState->GetProposedRoot(), LifecycleState->GetTargetKey(),
        ResolvedNodes, OutRequests, OutFailure);
  }

  virtual bool ApplyLifecycle(FOperationFailure &OutFailure) override {
    if (!LifecycleState->ApplyLifecycle(OutFailure))
      return false;
    if (!bHasPropertyApply ||
        !LifecycleState->ShouldApplyPropertiesToActualRoot())
      return true;

    const FReflectionRoot ActualRoot = LifecycleState->GetActualRoot();
    if (!ActualRoot.IsValid()) {
      SetFailure(OutFailure, TEXT("reflection_operation_actual_root_missing"),
                 LifecycleState->GetTargetKey());
      return false;
    }
    UeAgentAuthoringObjectApply::FFailure PrepareFailure;
    const bool bPrepared =
        ActualRoot.Kind == EReflectionRootKind::Object
            ? UeAgentAuthoringObjectApply::PrepareObject(
                  ActualRoot.Object, PropertyDescriptors, ObjectApplyOptions,
                  ActualPropertyApply, PrepareFailure)
            : UeAgentAuthoringObjectApply::PrepareStruct(
                  ActualRoot.StructType, ActualRoot.StructData,
                  ActualRoot.NotificationOwner, PropertyDescriptors,
                  ObjectApplyOptions, ActualPropertyApply, PrepareFailure);
    if (!bPrepared) {
      SetObjectApplyFailure(OutFailure, PrepareFailure);
      return false;
    }
    UeAgentAuthoringObjectApply::FApplyResult ApplyResult;
    if (!UeAgentAuthoringObjectApply::ApplyPrepared(ActualPropertyApply,
                                                    ApplyResult)) {
      SetObjectApplyFailure(OutFailure, ApplyResult);
      return false;
    }
    if (!UeAgentAuthoringObjectApply::VerifyPrepared(ActualPropertyApply,
                                                     ApplyResult)) {
      SetObjectApplyFailure(OutFailure, ApplyResult);
      return false;
    }
    if (!ObjectApplyOptions.bEmitEditorNotifications) {
      UObject *NotificationOwner =
          ActualRoot.Kind == EReflectionRootKind::Object
              ? ActualRoot.Object
              : ActualRoot.NotificationOwner;
      if (NotificationOwner)
        NotificationOwner->PostEditChange();
    }
    return true;
  }

  virtual bool Compile(FOperationFailure &OutFailure) override {
    return LifecycleState->Compile(OutFailure);
  }

  virtual bool Save(FOperationFailure &OutFailure) override {
    return LifecycleState->Save(OutFailure);
  }

  virtual bool
  BuildDurablePackagePlans(TArray<FPackageDurablePlan> &OutPlans,
                           FOperationFailure &OutFailure) const override {
    return LifecycleState->BuildDurablePackagePlans(OutPlans, OutFailure);
  }

  virtual bool BuildActualReadRequests(
      TArray<UeAgentAuthoringState::FReadRequest> &OutRequests,
      FOperationFailure &OutFailure) const override {
    OutRequests.Reset();
    if (!LifecycleState->BuildDomainActualReadRequests(OutRequests, OutFailure))
      return false;
    if (!bHasPropertyApply)
      return true;
    return UeAgentAuthoringOperationApply::AppendPropertyReadRequests(
        LifecycleState->GetActualRoot(), LifecycleState->GetTargetKey(),
        ResolvedNodes, OutRequests, OutFailure);
  }

  virtual TSharedPtr<FJsonObject> BuildResultMetadata() const override {
    return LifecycleState->BuildResultMetadata();
  }

  virtual bool Commit(FOperationFailure &OutFailure) override {
    return LifecycleState->Commit(OutFailure);
  }

  virtual bool Rollback(FOperationFailure &OutFailure) override {
    bool bActualPropertiesRolledBack = true;
    UeAgentAuthoringObjectApply::FApplyResult ActualPropertyResult;
    if (ActualPropertyApply.HasBeenApplied()) {
      bActualPropertiesRolledBack =
          UeAgentAuthoringObjectApply::RollbackPrepared(ActualPropertyApply,
                                                        ActualPropertyResult);
    }
    FOperationFailure LifecycleFailure;
    const bool bLifecycleRolledBack =
        LifecycleState->Rollback(LifecycleFailure);
    bool bProposedPropertiesRolledBack = true;
    UeAgentAuthoringObjectApply::FApplyResult ProposedPropertyResult;
    if (bHasPropertyApply && PropertyApply.HasBeenApplied()) {
      bProposedPropertiesRolledBack =
          UeAgentAuthoringObjectApply::RollbackPrepared(PropertyApply,
                                                        ProposedPropertyResult);
    }
    if (!bActualPropertiesRolledBack) {
      SetObjectApplyFailure(OutFailure, ActualPropertyResult);
      return false;
    }
    if (!bLifecycleRolledBack) {
      OutFailure = LifecycleFailure;
      return false;
    }
    if (!bProposedPropertiesRolledBack) {
      SetObjectApplyFailure(OutFailure, ProposedPropertyResult);
      return false;
    }
    return true;
  }

private:
  TUniquePtr<IReflectionOperationLifecycleState> LifecycleState;
  UeAgentAuthoringObjectApply::FPreparedApply PropertyApply;
  UeAgentAuthoringObjectApply::FPreparedApply ActualPropertyApply;
  bool bHasPropertyApply = false;
  TArray<UeAgentAuthoringObjectApply::FResolvedNode> ResolvedNodes;
  TArray<TSharedPtr<FJsonValue>> PropertyDescriptors;
  UeAgentAuthoringObjectApply::FOptions ObjectApplyOptions;
};
} // namespace

namespace UeAgentAuthoringOperationApply {
bool AppendPropertyReadRequests(
    const FReflectionRoot &Root, const FString &TargetKey,
    const TArray<UeAgentAuthoringObjectApply::FResolvedNode> &Nodes,
    TArray<UeAgentAuthoringState::FReadRequest> &OutRequests,
    FOperationFailure &OutFailure) {
  return AppendPropertyReadRequestsInternal(Root, TargetKey, Nodes, OutRequests,
                                            OutFailure);
}

FReflectionRoot FReflectionRoot::MakeObject(UObject *InObject) {
  FReflectionRoot Result;
  Result.Kind = EReflectionRootKind::Object;
  Result.Object = InObject;
  Result.NotificationOwner = InObject;
  Result.OwnerPath = InObject ? InObject->GetPathName() : FString();
  return Result;
}

FReflectionRoot FReflectionRoot::MakeStruct(UStruct *InStructType,
                                            void *InStructData,
                                            UObject *InNotificationOwner,
                                            FString InOwnerPath) {
  FReflectionRoot Result;
  Result.Kind = EReflectionRootKind::Struct;
  Result.StructType = InStructType;
  Result.StructData = InStructData;
  Result.NotificationOwner = InNotificationOwner;
  Result.OwnerPath = MoveTemp(InOwnerPath);
  if (Result.OwnerPath.IsEmpty()) {
    Result.OwnerPath =
        InNotificationOwner
            ? InNotificationOwner->GetPathName()
            : (InStructType ? InStructType->GetPathName() : FString());
  }
  return Result;
}

bool FReflectionRoot::IsValid() const {
  return Kind == EReflectionRootKind::Object
             ? Object != nullptr
             : StructType != nullptr && StructData != nullptr &&
                   NotificationOwner != nullptr;
}

FReflectionApplyProvider::FReflectionApplyProvider(
    FString InProviderId, TSharedRef<IReflectionOperationLifecycle> InLifecycle,
    UeAgentAuthoringObjectApply::FOptions InObjectApplyOptions)
    : ProviderId(MoveTemp(InProviderId)), Lifecycle(MoveTemp(InLifecycle)),
      ObjectApplyOptions(MoveTemp(InObjectApplyOptions)) {
  ProviderId.TrimStartAndEndInline();
  ProviderId.ToLowerInline();
}

FString FReflectionApplyProvider::GetProviderId() const { return ProviderId; }

bool FReflectionApplyProvider::PrepareOperation(
    const FPrepareRequest &Request,
    TUniquePtr<IPreparedOperationState> &OutState,
    FOperationFailure &OutFailure) const {
  OutState.Reset();
  OutFailure = FOperationFailure();
  if (!Request.IsValid() || Request.Contract->ProviderId != ProviderId) {
    SetFailure(OutFailure, TEXT("object_operation_prepare_request_invalid"),
               ProviderId);
    return false;
  }

  TUniquePtr<IReflectionOperationLifecycleState> LifecycleState;
  if (!Lifecycle->PrepareLifecycle(Request, LifecycleState, OutFailure) ||
      !LifecycleState.IsValid()) {
    if (OutFailure.ErrorCode.IsEmpty()) {
      SetFailure(OutFailure, TEXT("object_operation_lifecycle_prepare_failed"),
                 ProviderId);
    }
    return false;
  }

  UeAgentAuthoringObjectApply::FPreparedApply PropertyApply;
  bool bHasPropertyApply = Request.Input->HasProperties();
  TArray<UeAgentAuthoringObjectApply::FResolvedNode> ResolvedNodes;
  TArray<TSharedPtr<FJsonValue>> EffectivePropertyDescriptors;
  if (bHasPropertyApply) {
    const FReflectionRoot ProposedRoot = LifecycleState->GetProposedRoot();
    if (!ProposedRoot.IsValid()) {
      SetFailure(OutFailure, TEXT("reflection_operation_proposed_root_missing"),
                 LifecycleState->GetTargetKey());
      return false;
    }
    TArray<TSharedPtr<FJsonValue>> ProjectedPropertyDescriptors;
    ProjectedPropertyDescriptors.Reserve(Request.Input->Properties.Num());
    for (const TSharedPtr<FJsonValue> &DescriptorValue :
         Request.Input->Properties) {
      const TSharedPtr<FJsonObject> Descriptor =
          DescriptorValue.IsValid() && DescriptorValue->Type == EJson::Object
              ? DescriptorValue->AsObject()
              : nullptr;
      const TSharedPtr<FJsonObject> Projected =
          UeAgentAuthoringObjectApply::ProjectDescriptorForApply(Descriptor);
      ProjectedPropertyDescriptors.Add(
          Projected.IsValid() ? MakeShared<FJsonValueObject>(Projected)
                              : DescriptorValue);
    }
    auto PrepareRoot =
        [this](const FReflectionRoot &Root,
               const TArray<TSharedPtr<FJsonValue>> &Descriptors,
               UeAgentAuthoringObjectApply::FPreparedApply &OutApply,
               UeAgentAuthoringObjectApply::FFailure &OutPrepareFailure) {
          return Root.Kind == EReflectionRootKind::Object
                     ? UeAgentAuthoringObjectApply::PrepareObject(
                           Root.Object, Descriptors, ObjectApplyOptions,
                           OutApply, OutPrepareFailure)
                     : UeAgentAuthoringObjectApply::PrepareStruct(
                           Root.StructType, Root.StructData,
                           Root.NotificationOwner, Descriptors,
                           ObjectApplyOptions, OutApply, OutPrepareFailure);
        };
    const FReflectionRoot ActualBaselineRoot = LifecycleState->GetActualRoot();
    const bool bUseActualBaseline =
        LifecycleState->ShouldApplyPropertiesToActualRoot() &&
        ActualBaselineRoot.IsValid();
    const FReflectionRoot BaselineRoot =
        bUseActualBaseline ? ActualBaselineRoot : ProposedRoot;
    if (!BaselineRoot.IsValid()) {
      SetFailure(OutFailure, TEXT("reflection_operation_delta_root_missing"),
                 LifecycleState->GetTargetKey());
      return false;
    }
    UeAgentAuthoringObjectApply::FPreparedApply DeltaApply;
    UeAgentAuthoringObjectApply::FFailure PrepareFailure;
    if (!PrepareRoot(BaselineRoot, ProjectedPropertyDescriptors, DeltaApply,
                     PrepareFailure)) {
      SetObjectApplyFailure(OutFailure, PrepareFailure);
      return false;
    }
    TSet<FString> ChangedPaths;
    for (const UeAgentAuthoringObjectApply::FResolvedNode &Node :
         DeltaApply.GetResolvedNodes()) {
      ChangedPaths.Add(Node.PropertyPath);
      ResolvedNodes.Add(Node);
    }
    for (const TSharedPtr<FJsonValue> &DescriptorValue :
         ProjectedPropertyDescriptors) {
      const TSharedPtr<FJsonObject> Descriptor =
          DescriptorValue.IsValid() && DescriptorValue->Type == EJson::Object
              ? DescriptorValue->AsObject()
              : nullptr;
      FString PropertyPath;
      if (Descriptor.IsValid() &&
          Descriptor->TryGetStringField(TEXT("property_path"), PropertyPath) &&
          ChangedPaths.Contains(PropertyPath)) {
        EffectivePropertyDescriptors.Add(DescriptorValue);
      }
    }
    bHasPropertyApply = !EffectivePropertyDescriptors.IsEmpty();
    if (bHasPropertyApply) {
      if (bUseActualBaseline) {
        PrepareFailure = UeAgentAuthoringObjectApply::FFailure();
        if (!PrepareRoot(ProposedRoot, EffectivePropertyDescriptors,
                         PropertyApply, PrepareFailure)) {
          SetObjectApplyFailure(OutFailure, PrepareFailure);
          return false;
        }
      } else {
        PropertyApply = MoveTemp(DeltaApply);
      }
    }
  }

  OutState = MakeUnique<FReflectionPreparedOperationState>(
      MoveTemp(LifecycleState), MoveTemp(PropertyApply), bHasPropertyApply,
      MoveTemp(ResolvedNodes), MoveTemp(EffectivePropertyDescriptors),
      ObjectApplyOptions);
  return true;
}
} // namespace UeAgentAuthoringOperationApply

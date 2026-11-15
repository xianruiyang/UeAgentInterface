// Copyright Epic Games, Inc. All Rights Reserved.

#include "Utilities/AuthoringOperationApply/UeAgentAuthoringObjectApplyProvider.h"

#include "Core/Contract/UeAgentDiagnostic.h"

#include "Dom/JsonObject.h"
#include "UObject/Class.h"
#include "UObject/Object.h"
#include "UObject/Package.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringIdentity.h"
#include "Utilities/AuthoringCore/UeAgentNativeNormalizationIntent.h"
#include "Utilities/AuthoringObjectApply/UeAgentPrimitiveComponentNormalization.h"

namespace {
using namespace UeAgentAuthoringOperationApply;

using FDiagnostic = UeAgentAuthoringDiagnostics::FDiagnostic;

FDiagnostic NativeFailure(const FString& Code, const FString& Reason, const FString& InternalContext) {
  auto Diagnostic = FDiagnostic::Failure(Code, Reason, TEXT("reflection_operation"), InternalContext);
  Diagnostic.SourceKind = TEXT("native_result");
  return Diagnostic;
}

void SetFailure(FOperationFailure& OutFailure, FDiagnostic Diagnostic) {
  if (Diagnostic.IsEmpty()) Diagnostic = NativeFailure(TEXT("reflection_operation_diagnostic_missing"),
      TEXT("The native property operation failed without reporting its diagnostic."), FString());
  OutFailure.Diagnostic = MoveTemp(Diagnostic);
}

void SetObjectApplyFailure(
    FOperationFailure& OutFailure,
    const UeAgentAuthoringObjectApply::FFailure& Failure) {
  OutFailure.Diagnostic = Failure.Diagnostic;
}

void SetObjectApplyFailure(
    FOperationFailure& OutFailure,
    const UeAgentAuthoringObjectApply::FApplyResult& Result) {
  OutFailure.Diagnostic = Result.Diagnostic;
}

void AttachPropertyFailureSource(FOperationFailure& Failure, const FReflectionRoot& Root,
    const TArray<TSharedPtr<FJsonValue>>& Descriptors) {
  const UObject* Owner = Root.Kind == EReflectionRootKind::Object ? Root.Object : Root.NotificationOwner;
  int32 MatchedLength = 0;
  const FJsonObject* MatchedDescriptor = nullptr;
  for (const auto& Value : Descriptors) {
    if (!Value.IsValid() || Value->Type != EJson::Object) continue;
    const auto Descriptor = Value->AsObject();
    FString Path;
    const TSharedPtr<FJsonObject>* Source = nullptr;
    if (!Descriptor->TryGetStringField(TEXT("property_path"), Path) || Path.IsEmpty()
        || Path.Len() < MatchedLength || !Descriptor->TryGetObjectField(TEXT("authoring_source"), Source)) continue;
    const FString& FailedPath = Failure.Diagnostic.PropertyPath;
    if (FailedPath != Path && !FailedPath.StartsWith(Path + TEXT(".")) && !FailedPath.StartsWith(Path + TEXT("["))) continue;
    MatchedLength = Path.Len();
    MatchedDescriptor = Descriptor.Get();
  }
  if (MatchedDescriptor) UeAgentAuthoringDiagnostics::LocatePropertyValueSource(Failure.Diagnostic, *MatchedDescriptor);
  if (Owner && Failure.Diagnostic.TargetPath.IsEmpty()) Failure.Diagnostic.TargetPath = Owner->GetPathName();
}

bool BuildSnapshotIdentity(
    const UeAgentAuthoringIdentity::FCanonicalIdentity &NativeIdentity,
    const FString &TargetKey, const FString &PropertyPath,
    UeAgentAuthoringIdentity::FCanonicalIdentity &OutIdentity,
    UE::UAI::Contract::FDiagnostic& OutError) {
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
    FOperationFailure &OutFailure,
    const UeAgentAuthoringObjectApply::FOptions* Options = nullptr) {
  if (!Root.IsValid()) {
    SetFailure(OutFailure, NativeFailure(TEXT("reflection_operation_readback_root_missing"),
        TEXT("The reflection root required for property readback is unavailable."), TargetKey));
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
      SetFailure(OutFailure, ResolveResult.Diagnostic);
      OutFailure.Diagnostic.PropertyPath = Node.PropertyPath;
      return false;
    }
    UeAgentAuthoringIdentity::FCanonicalIdentity SnapshotIdentity;
    UE::UAI::Contract::FDiagnostic IdentityError;
    if (!BuildSnapshotIdentity(ReadRequest.TargetIdentity, TargetKey,
                               Node.PropertyPath, SnapshotIdentity,
                               IdentityError)) {
      SetFailure(OutFailure, IdentityError);
      OutFailure.Diagnostic.PropertyPath = Node.PropertyPath;
      return false;
    }
    ReadRequest.SnapshotIdentityOverride = MoveTemp(SnapshotIdentity);
    if (Options) {
      ReadRequest.Limits.AuthorizeProperty = Options->AuthorizeProperty;
      if (!Node.bExplicit && !Options->NativeStages.IsEmpty()) {
        const FName Name(*Node.PropertyPath);
        for (const auto& Stage : Options->NativeStages) {
          if (Stage.VerificationOnlyReadbackProperties.Contains(Name)) {
            ReadRequest.MarkVerificationOnly();
            break;
          }
        }
      }
    }
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
      TArray<TSharedPtr<FJsonValue>> InSourceDescriptors,
      UeAgentAuthoringObjectApply::FOptions InObjectApplyOptions,
      TOptional<UeAgentAuthoringObjectApply::FPropertyIntent> InUnchangedIntent)
      : LifecycleState(MoveTemp(InLifecycleState)),
        PropertyApply(MoveTemp(InPropertyApply)),
        UnchangedPropertyIntent(MoveTemp(InUnchangedIntent)),
        bHasPropertyApply(bInHasPropertyApply),
        ResolvedNodes(MoveTemp(InResolvedNodes)),
        PropertyDescriptors(MoveTemp(InPropertyDescriptors)),
        SourceDescriptors(MoveTemp(InSourceDescriptors)),
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
    LifecycleState->CollectRollbackObjects(OutObjects);
  }

  virtual bool RequiresCheckpointRollback() const override {
    return LifecycleState->RequiresCheckpointRollback();
  }

  virtual bool CaptureRollback(FOperationFailure &OutFailure) override {
    return LifecycleState->CaptureRollback(OutFailure);
  }

  virtual bool ApplyProposed(FOperationFailure &OutFailure) override {
    if (!LifecycleState->PreparePropertyApplication(OutFailure)) {
      AttachPropertyFailureSource(OutFailure, LifecycleState->GetActualRoot(), SourceDescriptors);
      return false;
    }
    if (!LifecycleState->UsesProposedPropertyPreview())
      return LifecycleState->ApplyProposed(OutFailure);
    if (bHasPropertyApply) {
      if (PropertyApply.HasTextSourceEdits()) {
        const FReflectionRoot ActualRoot = LifecycleState->GetActualRoot();
        UObject* ActualOwner = ActualRoot.Kind == EReflectionRootKind::Object ? ActualRoot.Object : ActualRoot.NotificationOwner;
        UPackage* IdentityPackage = ActualOwner ? ActualOwner->GetPackage() : nullptr;
        if (!IdentityPackage) {
          TArray<FPackageDurablePlan> Plans;
          if (!LifecycleState->BuildDurablePackagePlans(Plans, OutFailure)) return false;
          if (Plans.Num() == 1) IdentityPackage = FindPackage(nullptr, *Plans[0].PackageName);
        }
        if (!IdentityPackage) {
          SetFailure(OutFailure, NativeFailure(TEXT("text_source_identity_package_unresolved"),
              TEXT("The package required to assign the edited text identity could not be resolved."), LifecycleState->GetTargetKey()));
          return false;
        }
        PropertyApply.SetTextIdentityPackage(IdentityPackage);
      }
      UeAgentAuthoringObjectApply::FApplyResult Result;
      if (!UeAgentAuthoringObjectApply::ApplyPrepared(PropertyApply, Result)) {
        SetObjectApplyFailure(OutFailure, Result);
        AttachPropertyFailureSource(OutFailure, LifecycleState->GetActualRoot(), SourceDescriptors);
        return false;
      }
      PropertyDescriptors = PropertyApply.GetResolvedPropertyDescriptors();
      if (!FreezeIntent(PropertyApply, ProposedPropertyIntent, OutFailure)) return false;
    }
    if (!LifecycleState->ApplyProposed(OutFailure)) {
      AttachPropertyFailureSource(OutFailure, LifecycleState->GetActualRoot(), SourceDescriptors);
      return false;
    }
    if (bHasPropertyApply && LifecycleState->ShouldNotifyProposedProperties()
        && !ObjectApplyOptions.bEmitEditorNotifications) {
      const FReflectionRoot ProposedRoot = LifecycleState->GetProposedRoot();
      UObject *NotificationOwner =
          ProposedRoot.Kind == EReflectionRootKind::Object
              ? ProposedRoot.Object
              : ProposedRoot.NotificationOwner;
      if (NotificationOwner) {
        UeAgentAuthoringObjectApply::FFailure Failure;
        if (!UeAgentAuthoringObjectApply::NotifyObject(*NotificationOwner, ObjectApplyOptions, Failure)) {
          SetObjectApplyFailure(OutFailure, Failure);
          AttachPropertyFailureSource(OutFailure, LifecycleState->GetActualRoot(), SourceDescriptors);
          return false;
        }
      }
    }
    // Domain preparation and whole-object notification run after the first
    // property check. Recheck the prepared intent before the coordinator can
    // capture their effects as the expected snapshot.
    if (bHasPropertyApply) {
      if (!VerifyIntent(ProposedPropertyIntent.GetValue(), LifecycleState->GetProposedRoot(), OutFailure)) return false;
    }
    return !UnchangedPropertyIntent.IsSet()
        || VerifyIntent(UnchangedPropertyIntent.GetValue(), LifecycleState->GetProposedRoot(), OutFailure);
  }

  virtual bool CaptureExpectedAfterLifecycle() const override {
    return !LifecycleState->UsesProposedPropertyPreview();
  }

  virtual bool BuildExpectedReadRequests(
      TArray<UeAgentAuthoringState::FReadRequest> &OutRequests,
      FOperationFailure &OutFailure) const override {
    OutRequests.Reset();
    if (!LifecycleState->BuildDomainExpectedReadRequests(OutRequests,
                                                         OutFailure))
      return false;
    if (ResolvedNodes.IsEmpty())
      return true;
    return AppendPropertyReadRequestsInternal(
        LifecycleState->UsesProposedPropertyPreview()
            ? LifecycleState->GetProposedRoot() : LifecycleState->GetActualRoot(),
        LifecycleState->GetTargetKey(),
        ResolvedNodes, OutRequests, OutFailure, &ObjectApplyOptions);
  }

  virtual bool ApplyLifecycle(FOperationFailure &OutFailure) override {
    if (!LifecycleState->ApplyLifecycle(OutFailure)) {
      AttachPropertyFailureSource(OutFailure, LifecycleState->GetActualRoot(), SourceDescriptors);
      return false;
    }
    if (!bHasPropertyApply ||
        !LifecycleState->ShouldApplyPropertiesToActualRoot())
      return true;

    const FReflectionRoot ActualRoot = LifecycleState->GetActualRoot();
    if (!ActualRoot.IsValid()) {
      SetFailure(OutFailure, NativeFailure(TEXT("reflection_operation_actual_root_missing"),
          TEXT("The live reflection root is unavailable after the lifecycle operation."), LifecycleState->GetTargetKey()));
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
      AttachPropertyFailureSource(OutFailure, ActualRoot, SourceDescriptors);
      return false;
    }
    UeAgentAuthoringObjectApply::FApplyResult ApplyResult;
    if (!UeAgentAuthoringObjectApply::ApplyPrepared(ActualPropertyApply,
                                                    ApplyResult)) {
      SetObjectApplyFailure(OutFailure, ApplyResult);
      AttachPropertyFailureSource(OutFailure, ActualRoot, SourceDescriptors);
      return false;
    }
    if (!FreezeIntent(ActualPropertyApply, ActualPropertyIntent, OutFailure)) return false;
    if (!ObjectApplyOptions.bEmitEditorNotifications) {
      UObject *NotificationOwner =
          ActualRoot.Kind == EReflectionRootKind::Object
              ? ActualRoot.Object
              : ActualRoot.NotificationOwner;
      if (NotificationOwner) {
        UeAgentAuthoringObjectApply::FFailure Failure;
        if (!UeAgentAuthoringObjectApply::NotifyObject(*NotificationOwner, ObjectApplyOptions, Failure)) {
          SetObjectApplyFailure(OutFailure, Failure);
          AttachPropertyFailureSource(OutFailure, ActualRoot, SourceDescriptors);
          return false;
        }
      }
    }
    const FReflectionRoot SettledRoot = LifecycleState->GetActualRoot();
    if (!SettledRoot.IsValid()) {
      SetFailure(OutFailure, NativeFailure(TEXT("reflection_operation_actual_root_missing"),
          TEXT("The live reflection root is unavailable for rollback or readback."), LifecycleState->GetTargetKey()));
      return false;
    }
    // Rebind the frozen decoded intent even when native notifications replaced
    // the root. A new snapshot is an observation, never a new authored intent.
    return VerifyIntent(ActualPropertyIntent.GetValue(), SettledRoot, OutFailure);
  }

  virtual bool Compile(FOperationFailure &OutFailure) override {
    return LifecycleState->Compile(OutFailure) && VerifyActualIntent(OutFailure);
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
    if ((LifecycleState->GetResolvedOperation() == UeAgentApplyInputContract::EOperation::Create || LifecycleState->GetResolvedOperation() == UeAgentApplyInputContract::EOperation::Clone) &&
        !LifecycleState->GetActualRoot().IsValid()) return true;
    if (!VerifyActualIntent(OutFailure)) return false;
    if (ResolvedNodes.IsEmpty()) return true;
    return AppendPropertyReadRequestsInternal(
        LifecycleState->GetActualRoot(), LifecycleState->GetTargetKey(),
        ResolvedNodes, OutRequests, OutFailure, &ObjectApplyOptions);
  }

  virtual TSharedPtr<FJsonObject> BuildResultMetadata() const override {
    return LifecycleState->BuildResultMetadata();
  }

  virtual bool BuildBeforeReadRequests(
      TArray<UeAgentAuthoringState::FReadRequest> &OutRequests,
      FOperationFailure &OutFailure) const override {
    OutRequests.Reset();
    if (!LifecycleState->BuildDomainBeforeReadRequests(OutRequests, OutFailure))
      return false;
    if (ResolvedNodes.IsEmpty()) return true;
    if ((LifecycleState->GetResolvedOperation() == UeAgentApplyInputContract::EOperation::Create || LifecycleState->GetResolvedOperation() == UeAgentApplyInputContract::EOperation::Clone) &&
        !LifecycleState->GetActualRoot().IsValid()) return true;
    return AppendPropertyReadRequestsInternal(
        LifecycleState->GetActualRoot(), LifecycleState->GetTargetKey(),
        ResolvedNodes, OutRequests, OutFailure, &ObjectApplyOptions);
  }

  virtual bool Commit(FOperationFailure &OutFailure) override {
    if (!LifecycleState->Commit(OutFailure)) return false;
    const auto& Intent = ActualPropertyIntent.IsSet() ? ActualPropertyIntent : ProposedPropertyIntent;
    if (Intent.IsSet()) LifecycleState->RetainPropertyIntent(Intent.GetValue());
    return true;
  }

  virtual bool SupportsPersistentReadback() const override {
    return LifecycleState->SupportsPersistentReadback();
  }
  virtual bool ReleaseForPersistentReadback(FOperationFailure &OutFailure) override {
    // These native pointers and values cannot survive package unload. Late
    // recovery belongs to the transaction's complete before-package checkpoint.
    PropertyApply = UeAgentAuthoringObjectApply::FPreparedApply();
    ActualPropertyApply = UeAgentAuthoringObjectApply::FPreparedApply();
    return LifecycleState->ReleaseForPersistentReadback(OutFailure);
  }
  virtual bool RebindAfterPersistentReadback(FOperationFailure &OutFailure) override {
    return LifecycleState->RebindAfterPersistentReadback(OutFailure);
  }
  virtual bool PrepareCheckpointRestore(FOperationFailure &OutFailure) override {
    PropertyApply = UeAgentAuthoringObjectApply::FPreparedApply();
    ActualPropertyApply = UeAgentAuthoringObjectApply::FPreparedApply();
    return LifecycleState->PrepareCheckpointRestore(OutFailure);
  }
  virtual bool RebindAfterCheckpointRestore(FOperationFailure &OutFailure) override {
    return LifecycleState->RebindAfterCheckpointRestore(OutFailure);
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
  bool FreezeIntent(const UeAgentAuthoringObjectApply::FPreparedApply& Plan,
      TOptional<UeAgentAuthoringObjectApply::FPropertyIntent>& OutIntent,
      FOperationFailure& OutFailure) {
    UeAgentAuthoringObjectApply::FPropertyIntent Intent;
    UeAgentAuthoringObjectApply::FFailure Failure;
    if (!Plan.CaptureIntent(Intent, Failure, false)) {
      SetObjectApplyFailure(OutFailure, Failure);
      AttachPropertyFailureSource(OutFailure, LifecycleState->GetActualRoot(), SourceDescriptors);
      UeAgentAuthoringDiagnostics::ReportConsistencyWarning(OutFailure.Diagnostic.Code,
          OutFailure.Diagnostic, TEXT("property_intent_capture"));
      OutFailure = FOperationFailure();
    }
    OutIntent = MoveTemp(Intent);
    return true;
  }

  bool VerifyIntent(const UeAgentAuthoringObjectApply::FPropertyIntent& Intent,
      const FReflectionRoot& Root, FOperationFailure& OutFailure) const {
    if (Intent.IsEmpty()) return true;
    UeAgentAuthoringObjectApply::FApplyResult Result;
    const bool bVerified = Root.Kind == EReflectionRootKind::Object
        ? Intent.VerifyObject(Root.Object, Result)
        : Intent.VerifyStruct(Root.StructType, Root.StructData, Root.NotificationOwner, Result);
    if (!bVerified) {
      SetObjectApplyFailure(OutFailure, Result);
      AttachPropertyFailureSource(OutFailure, Root, SourceDescriptors);
      UeAgentAuthoringDiagnostics::ReportConsistencyWarning(OutFailure.Diagnostic.Code,
          OutFailure.Diagnostic, TEXT("property_intent_observation"));
      OutFailure = FOperationFailure();
    }
    return true;
  }

  bool VerifyActualIntent(FOperationFailure& OutFailure) const {
    const auto& Intent = ActualPropertyIntent.IsSet() ? ActualPropertyIntent : ProposedPropertyIntent;
    return (!Intent.IsSet() || VerifyIntent(Intent.GetValue(), LifecycleState->GetActualRoot(), OutFailure))
        && (!UnchangedPropertyIntent.IsSet() || VerifyIntent(UnchangedPropertyIntent.GetValue(), LifecycleState->GetActualRoot(), OutFailure));
  }

  TUniquePtr<IReflectionOperationLifecycleState> LifecycleState;
  UeAgentAuthoringObjectApply::FPreparedApply PropertyApply;
  UeAgentAuthoringObjectApply::FPreparedApply ActualPropertyApply;
  TOptional<UeAgentAuthoringObjectApply::FPropertyIntent> ProposedPropertyIntent;
  TOptional<UeAgentAuthoringObjectApply::FPropertyIntent> ActualPropertyIntent;
  TOptional<UeAgentAuthoringObjectApply::FPropertyIntent> UnchangedPropertyIntent;
  bool bHasPropertyApply = false;
  TArray<UeAgentAuthoringObjectApply::FResolvedNode> ResolvedNodes;
  TArray<TSharedPtr<FJsonValue>> PropertyDescriptors;
  TArray<TSharedPtr<FJsonValue>> SourceDescriptors;
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
    SetFailure(OutFailure, NativeFailure(TEXT("object_operation_prepare_request_invalid"),
        TEXT("Property preparation requires a valid reflection operation request and lifecycle provider."), ProviderId));
    return false;
  }

  TUniquePtr<IReflectionOperationLifecycleState> LifecycleState;
  if (!Lifecycle->PrepareLifecycle(Request, LifecycleState, OutFailure) ||
      !LifecycleState.IsValid()) {
    if (OutFailure.Diagnostic.Code.IsEmpty()) {
      SetFailure(OutFailure, NativeFailure(TEXT("object_operation_lifecycle_prepare_failed"),
          TEXT("The lifecycle provider failed to prepare the property target without reporting its diagnostic."), ProviderId));
    }
    return false;
  }

  UeAgentAuthoringObjectApply::FPreparedApply PropertyApply;
  TOptional<UeAgentAuthoringObjectApply::FPropertyIntent> UnchangedIntent;
  TArray<TSharedPtr<FJsonValue>> InputPropertyDescriptors = Request.Input->Properties;
  if (!LifecycleState->CollectPropertyDescriptors(InputPropertyDescriptors, OutFailure)) return false;
  bool bHasPropertyApply = !InputPropertyDescriptors.IsEmpty();
  TArray<UeAgentAuthoringObjectApply::FResolvedNode> ResolvedNodes;
  TArray<TSharedPtr<FJsonValue>> EffectivePropertyDescriptors;
  const auto AddReadbackProperty = [&ResolvedNodes](FName Property) {
    const FString Path = Property.ToString();
    if (!ResolvedNodes.ContainsByPredicate([&](const auto& Node) { return Node.PropertyPath == Path; })) {
      auto& Node = ResolvedNodes.AddDefaulted_GetRef();
      Node.PropertyPath = Path;
      Node.bExplicit = false;
    }
  };
  if (bHasPropertyApply) {
    const FReflectionRoot ProposedRoot = LifecycleState->GetProposedRoot();
    if (!ProposedRoot.IsValid()) {
      SetFailure(OutFailure, NativeFailure(TEXT("reflection_operation_proposed_root_missing"),
          TEXT("The lifecycle provider did not supply a reflection root for the proposed property values."), LifecycleState->GetTargetKey()));
      return false;
    }
    TArray<TSharedPtr<FJsonValue>> ProjectedPropertyDescriptors;
    ProjectedPropertyDescriptors.Reserve(InputPropertyDescriptors.Num());
    for (const TSharedPtr<FJsonValue> &DescriptorValue :
         InputPropertyDescriptors) {
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
        [this, &LifecycleState, &ProposedRoot](const FReflectionRoot &Root,
               const TArray<TSharedPtr<FJsonValue>> &Descriptors,
               UeAgentAuthoringObjectApply::FPreparedApply &OutApply,
               UeAgentAuthoringObjectApply::FFailure &OutPrepareFailure) {
          auto Options = ObjectApplyOptions;
          LifecycleState->ConfigurePropertyOptions(Options);
          if (Root.Kind == ProposedRoot.Kind && Root.Object == ProposedRoot.Object
              && Root.StructData == ProposedRoot.StructData && !LifecycleState->ShouldNotifyProposedProperties())
            Options.bEmitEditorNotifications = false;
          return Root.Kind == EReflectionRootKind::Object
                     ? UeAgentAuthoringObjectApply::PrepareObject(
                           Root.Object, Descriptors, Options,
                           OutApply, OutPrepareFailure)
                     : UeAgentAuthoringObjectApply::PrepareStruct(
                           Root.StructType, Root.StructData,
                           Root.NotificationOwner, Descriptors,
                           Options, OutApply, OutPrepareFailure);
        };
    auto ResolvedOptions = ObjectApplyOptions;
    LifecycleState->ConfigurePropertyOptions(ResolvedOptions);
    if (!ResolvedOptions.NativeStages.IsEmpty() && !LifecycleState->RequiresCheckpointRollback()) {
      SetFailure(OutFailure, NativeFailure(TEXT("native_stages_require_checkpoint"),
          TEXT("Native staged property edits require a lifecycle checkpoint for rollback."), LifecycleState->GetTargetKey())); return false;
    }
    const FReflectionRoot ActualBaselineRoot = LifecycleState->GetActualRoot();
    UeAgentNativeNormalizationIntent::FTargetScope NormalizationTarget(
        ActualBaselineRoot.IsValid() ? ActualBaselineRoot.OwnerPath : LifecycleState->GetTargetKey());
    if (!LifecycleState->NormalizeProposedPropertyDescriptors(
            ProjectedPropertyDescriptors, ResolvedOptions, OutFailure)) return false;
    auto SetPreparedPropertyFailure = [&](const UeAgentAuthoringObjectApply::FFailure& Failure) {
      SetObjectApplyFailure(OutFailure, Failure);
      AttachPropertyFailureSource(OutFailure, ActualBaselineRoot, InputPropertyDescriptors);
    };
    const bool bUseActualBaseline =
        LifecycleState->ShouldApplyPropertiesToActualRoot() &&
        ActualBaselineRoot.IsValid();
    const FReflectionRoot BaselineRoot =
        bUseActualBaseline ? ActualBaselineRoot : ProposedRoot;
    if (!BaselineRoot.IsValid()) {
      SetFailure(OutFailure, NativeFailure(TEXT("reflection_operation_delta_root_missing"),
          TEXT("The reflection root required to compute the property delta is unavailable."), LifecycleState->GetTargetKey()));
      return false;
    }
    UeAgentAuthoringObjectApply::FPreparedApply DeltaApply;
    UeAgentAuthoringObjectApply::FFailure PrepareFailure;
    if (ProposedRoot.Kind == EReflectionRootKind::Object
        && !UeAgentAuthoringObjectApply::NormalizePrimitiveComponentProperties(
            ProposedRoot.Object, BaselineRoot.Object, ProjectedPropertyDescriptors,
            ResolvedOptions, PrepareFailure)) {
      SetPreparedPropertyFailure(PrepareFailure);
      return false;
    }
    if (!PrepareRoot(BaselineRoot, ProjectedPropertyDescriptors, DeltaApply,
                     PrepareFailure)) {
      SetPreparedPropertyFailure(PrepareFailure);
      return false;
    }
    UeAgentAuthoringObjectApply::FPropertyIntent Preserved;
    if (!DeltaApply.CaptureUnchangedIntent(Preserved, PrepareFailure)) {
      SetPreparedPropertyFailure(PrepareFailure);
      UeAgentAuthoringDiagnostics::ReportConsistencyWarning(OutFailure.Diagnostic.Code,
          OutFailure.Diagnostic, TEXT("unchanged_intent_capture"));
      OutFailure = FOperationFailure();
    }
    if (!Preserved.IsEmpty()) {
      UnchangedIntent = Preserved;
      LifecycleState->RetainPropertyIntent(Preserved);
    }
    TSet<FString> ChangedPaths;
    for (const UeAgentAuthoringObjectApply::FResolvedNode &Node :
         DeltaApply.GetResolvedNodes()) {
      ChangedPaths.Add(Node.PropertyPath);
      ResolvedNodes.Add(Node);
    }
    for (const auto& Stage : ResolvedOptions.NativeStages) {
      for (const FName Property : Stage.ReadbackProperties) AddReadbackProperty(Property);
      for (const FName Property : Stage.VerificationOnlyReadbackProperties) AddReadbackProperty(Property);
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
      if (bUseActualBaseline && LifecycleState->UsesProposedPropertyPreview()) {
        PrepareFailure = UeAgentAuthoringObjectApply::FFailure();
        if (!PrepareRoot(ProposedRoot, EffectivePropertyDescriptors,
                         PropertyApply, PrepareFailure)) {
          SetPreparedPropertyFailure(PrepareFailure);
          return false;
        }
      } else if (!bUseActualBaseline) {
        PropertyApply = MoveTemp(DeltaApply);
      }
    }
  }

  if (!bHasPropertyApply) ResolvedNodes.Reset();
  for (const FName Property : LifecycleState->GetLifecycleReadbackProperties()) AddReadbackProperty(Property);
  auto ResolvedOptions = ObjectApplyOptions;
  LifecycleState->ConfigurePropertyOptions(ResolvedOptions);
  if (!bHasPropertyApply && LifecycleState->IsNoOp())
    LifecycleState->DiscardNoOpPreparation();
  OutState = MakeUnique<FReflectionPreparedOperationState>(
      MoveTemp(LifecycleState), MoveTemp(PropertyApply), bHasPropertyApply,
      MoveTemp(ResolvedNodes), MoveTemp(EffectivePropertyDescriptors),
      Request.Input->Properties,
      ResolvedOptions, MoveTemp(UnchangedIntent));
  return true;
}
} // namespace UeAgentAuthoringOperationApply

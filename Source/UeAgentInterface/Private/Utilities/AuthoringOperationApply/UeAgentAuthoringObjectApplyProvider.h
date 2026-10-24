// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Utilities/AuthoringObjectApply/UeAgentAuthoringObjectApply.h"
#include "Utilities/AuthoringOperationApply/UeAgentAuthoringOperationCoordinator.h"

class UObject;
class UStruct;

namespace UeAgentAuthoringOperationApply {
enum class EReflectionRootKind : uint8 {
  Object,
  Struct,
};

struct UEAGENTINTERFACE_API FReflectionRoot {
  EReflectionRootKind Kind = EReflectionRootKind::Object;
  UObject *Object = nullptr;
  UStruct *StructType = nullptr;
  void *StructData = nullptr;
  UObject *NotificationOwner = nullptr;
  FString OwnerPath;

  static FReflectionRoot MakeObject(UObject *InObject);
  static FReflectionRoot MakeStruct(UStruct *InStructType, void *InStructData,
                                    UObject *InNotificationOwner,
                                    FString InOwnerPath = FString());
  bool IsValid() const;
};

UEAGENTINTERFACE_API bool AppendPropertyReadRequests(
    const FReflectionRoot &Root, const FString &TargetKey,
    const TArray<UeAgentAuthoringObjectApply::FResolvedNode> &Nodes,
    TArray<UeAgentAuthoringState::FReadRequest> &OutRequests,
    FOperationFailure &OutFailure);

class UEAGENTINTERFACE_API IReflectionOperationLifecycleState {
public:
  virtual ~IReflectionOperationLifecycleState() = default;

  virtual FString GetTargetKey() const = 0;
  virtual UeAgentApplyInputContract::EOperation
  GetResolvedOperation() const = 0;
  virtual bool IsNoOp() const { return false; }
  virtual bool IsStateNoOp() const { return IsNoOp(); }
  virtual FReflectionRoot GetProposedRoot() const = 0;
  virtual FReflectionRoot GetActualRoot() const = 0;
  virtual bool ShouldApplyPropertiesToActualRoot() const { return false; }
  // Default previews retain native normalization. A domain may suppress all
  // preview notifications only when they are proven external side effects;
  // actual notifications and settle still follow the operation contract.
  virtual bool ShouldNotifyProposedProperties() const { return true; }
  // Domain input aliases become ordinary descriptors before shared preparation,
  // intent capture and rollback; they must not retain a second native writer.
  virtual bool CollectPropertyDescriptors(TArray<TSharedPtr<FJsonValue>>& Descriptors,
      FOperationFailure& OutFailure) const { return true; }
  virtual void ConfigurePropertyOptions(UeAgentAuthoringObjectApply::FOptions& Options) const {}
  // Opt-in native normalization on an isolated proposed root. Preserve complete
  // descriptors; actual application remains subject to strict value verification.
  virtual bool NormalizeProposedPropertyDescriptors(
      TArray<TSharedPtr<FJsonValue>>& Descriptors,
      const UeAgentAuthoringObjectApply::FOptions& Options,
      FOperationFailure& OutFailure) { return true; }
  virtual void CollectRollbackObjects(TArray<UObject*>& OutObjects) const {}
  virtual bool CaptureRollback(FOperationFailure &OutFailure) = 0;
  // Runs within the rollback boundary, after before-state/checkpoint capture
  // and before proposed property application or expected-state capture.
  virtual bool PreparePropertyApplication(FOperationFailure &OutFailure) {
    OutFailure = FOperationFailure();
    return true;
  }
  virtual bool ApplyProposed(FOperationFailure &OutFailure) {
    OutFailure = FOperationFailure();
    return true;
  }
  virtual bool ApplyLifecycle(FOperationFailure &OutFailure) = 0;
  virtual bool Compile(FOperationFailure &OutFailure) = 0;
  virtual bool Save(FOperationFailure &OutFailure) = 0;
  virtual bool BuildDurablePackagePlans(
      TArray<FPackageDurablePlan> &OutPlans,
      FOperationFailure &OutFailure) const {
    OutPlans.Reset();
    OutFailure = FOperationFailure();
    OutFailure.Diagnostic = UeAgentAuthoringDiagnostics::FDiagnostic::Failure(
        TEXT("durable_package_plan_not_supported"), TEXT("This operation does not support a durable package plan."));
    OutFailure.Diagnostic.InternalContext = GetTargetKey();
    return false;
  }
  virtual bool BuildDomainExpectedReadRequests(
      TArray<UeAgentAuthoringState::FReadRequest> &OutRequests,
      FOperationFailure &OutFailure) const = 0;
  virtual bool BuildDomainActualReadRequests(
      TArray<UeAgentAuthoringState::FReadRequest> &OutRequests,
      FOperationFailure &OutFailure) const = 0;
  virtual bool BuildDomainBeforeReadRequests(
      TArray<UeAgentAuthoringState::FReadRequest> &OutRequests,
      FOperationFailure &OutFailure) const {
    return BuildDomainActualReadRequests(OutRequests, OutFailure);
  }
  virtual TSharedPtr<FJsonObject> BuildResultMetadata() const {
    return nullptr;
  }
  virtual bool Commit(FOperationFailure &OutFailure) {
    OutFailure = FOperationFailure();
    return true;
  }
  // Retain equal-value intent at prepare (no write required), and changed-value
  // intent after commit, without retaining native property storage.
  virtual void RetainPropertyIntent(
      const UeAgentAuthoringObjectApply::FPropertyIntent& Intent) {}
  virtual bool Rollback(FOperationFailure &OutFailure) = 0;
  virtual bool RequiresCheckpointRollback() const { return false; }
  virtual bool SupportsPersistentReadback() const { return false; }
  virtual bool ReleaseForPersistentReadback(FOperationFailure &OutFailure) { return false; }
  virtual bool RebindAfterPersistentReadback(FOperationFailure &OutFailure) { return false; }
  virtual bool PrepareCheckpointRestore(FOperationFailure &OutFailure) {
    return ReleaseForPersistentReadback(OutFailure);
  }
  virtual bool RebindAfterCheckpointRestore(FOperationFailure &OutFailure) {
    return RebindAfterPersistentReadback(OutFailure);
  }
};

class UEAGENTINTERFACE_API IReflectionOperationLifecycle {
public:
  virtual ~IReflectionOperationLifecycle() = default;
  virtual bool
  PrepareLifecycle(const FPrepareRequest &Request,
                   TUniquePtr<IReflectionOperationLifecycleState> &OutState,
                   FOperationFailure &OutFailure) const = 0;
};

class UEAGENTINTERFACE_API FReflectionApplyProvider final
    : public IApplyProvider {
public:
  FReflectionApplyProvider(
      FString InProviderId,
      TSharedRef<IReflectionOperationLifecycle> InLifecycle,
      UeAgentAuthoringObjectApply::FOptions InObjectApplyOptions =
          UeAgentAuthoringObjectApply::FOptions());

  virtual FString GetProviderId() const override;
  virtual bool PrepareOperation(const FPrepareRequest &Request,
                                TUniquePtr<IPreparedOperationState> &OutState,
                                FOperationFailure &OutFailure) const override;

private:
  FString ProviderId;
  TSharedRef<IReflectionOperationLifecycle> Lifecycle;
  UeAgentAuthoringObjectApply::FOptions ObjectApplyOptions;
};
} // namespace UeAgentAuthoringOperationApply

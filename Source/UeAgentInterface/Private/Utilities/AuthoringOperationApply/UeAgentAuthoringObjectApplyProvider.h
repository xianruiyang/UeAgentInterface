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
  virtual bool CaptureRollback(FOperationFailure &OutFailure) = 0;
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
    OutFailure.ErrorCode = TEXT("durable_package_plan_not_supported");
    OutFailure.ErrorDetail = GetTargetKey();
    return false;
  }
  virtual bool BuildDomainExpectedReadRequests(
      TArray<UeAgentAuthoringState::FReadRequest> &OutRequests,
      FOperationFailure &OutFailure) const = 0;
  virtual bool BuildDomainActualReadRequests(
      TArray<UeAgentAuthoringState::FReadRequest> &OutRequests,
      FOperationFailure &OutFailure) const = 0;
  virtual TSharedPtr<FJsonObject> BuildResultMetadata() const {
    return nullptr;
  }
  virtual bool Commit(FOperationFailure &OutFailure) {
    OutFailure = FOperationFailure();
    return true;
  }
  virtual bool Rollback(FOperationFailure &OutFailure) = 0;
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

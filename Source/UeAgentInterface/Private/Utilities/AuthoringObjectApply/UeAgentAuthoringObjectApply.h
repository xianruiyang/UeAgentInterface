// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"

class FJsonObject;
class FJsonValue;
class UObject;
class UStruct;

namespace UeAgentAuthoringObjectApply {
using FDomainOwnerAuthorizationHandle = uint64;

UEAGENTINTERFACE_API FDomainOwnerAuthorizationHandle
RegisterDomainAuthorizedOwnerPackagePrefix(const FString &PackagePrefix);
UEAGENTINTERFACE_API void UnregisterDomainAuthorizedOwnerPackagePrefix(
    FDomainOwnerAuthorizationHandle Handle);

struct UEAGENTINTERFACE_API FOptions {
  bool bAllowReadOnly = false;
  bool bAllowTemplateOwners = false;
  bool bAllowDomainAuthorizedOwners = false;
  bool bEmitEditorNotifications = true;
  bool bMarkPackageDirty = true;
  int32 MaxContainerItems = 256;
  int32 MaxStringChars = 4096;
};

struct UEAGENTINTERFACE_API FFailure {
  FString ErrorCode;
  FString ErrorDetail;
  FString PropertyPath;
  UeAgentAuthoringDiagnostics::FDiagnostic Diagnostic;
};

struct UEAGENTINTERFACE_API FResolvedNode {
  FString PropertyPath;
  FString ValueKind;
  FString TypePath;
  bool bExplicit = true;
  TArray<FResolvedNode> Children;

  TSharedRef<FJsonObject> ToJsonObject() const;
};

struct UEAGENTINTERFACE_API FApplyResult {
  FString Status;
  FString ErrorCode;
  FString ErrorDetail;
  FString PropertyPath;
  int32 PropertiesPrepared = 0;
  int32 PropertiesAttempted = 0;
  int32 PropertiesApplied = 0;
  int32 PropertiesRolledBack = 0;
  bool bRollbackAttempted = false;
  bool bRollbackSucceeded = false;

  bool IsApplied() const;
  TSharedRef<FJsonObject> ToJsonObject() const;
};

class FPreparedApply;
struct FPreparedApplyAccess;

UEAGENTINTERFACE_API TSharedPtr<FJsonObject>
ProjectDescriptorForApply(const TSharedPtr<FJsonObject> &ExportDescriptor);

UEAGENTINTERFACE_API bool
PrepareObject(UObject *RootObject,
              const TArray<TSharedPtr<FJsonValue>> &PropertyDescriptors,
              const FOptions &Options, FPreparedApply &OutPrepared,
              FFailure &OutFailure);
UEAGENTINTERFACE_API bool
PrepareStruct(UStruct *RootStruct, void *RootData, UObject *NotificationOwner,
              const TArray<TSharedPtr<FJsonValue>> &PropertyDescriptors,
              const FOptions &Options, FPreparedApply &OutPrepared,
              FFailure &OutFailure);
UEAGENTINTERFACE_API bool ApplyPrepared(FPreparedApply &Prepared,
                                        FApplyResult &OutResult);
UEAGENTINTERFACE_API bool VerifyPrepared(FPreparedApply &Prepared,
                                         FApplyResult &OutResult);
UEAGENTINTERFACE_API bool RollbackPrepared(FPreparedApply &Prepared,
                                           FApplyResult &OutResult);

class UEAGENTINTERFACE_API FPreparedApply {
public:
  struct FImpl;

  FPreparedApply();
  ~FPreparedApply();

  FPreparedApply(FPreparedApply &&) noexcept;
  FPreparedApply &operator=(FPreparedApply &&) noexcept;
  FPreparedApply(const FPreparedApply &) = delete;
  FPreparedApply &operator=(const FPreparedApply &) = delete;

  bool IsReady() const;
  bool HasBeenApplied() const;
  int32 NumProperties() const;
  const TArray<FResolvedNode> &GetResolvedNodes() const;

private:
  TUniquePtr<FImpl> Impl;

  friend struct FPreparedApplyAccess;
  friend UEAGENTINTERFACE_API bool
  PrepareObject(UObject *RootObject,
                const TArray<TSharedPtr<FJsonValue>> &PropertyDescriptors,
                const FOptions &Options, FPreparedApply &OutPrepared,
                FFailure &OutFailure);
  friend UEAGENTINTERFACE_API bool
  PrepareStruct(UStruct *RootStruct, void *RootData, UObject *NotificationOwner,
                const TArray<TSharedPtr<FJsonValue>> &PropertyDescriptors,
                const FOptions &Options, FPreparedApply &OutPrepared,
                FFailure &OutFailure);
  friend UEAGENTINTERFACE_API bool ApplyPrepared(FPreparedApply &Prepared,
                                                 FApplyResult &OutResult);
  friend UEAGENTINTERFACE_API bool VerifyPrepared(FPreparedApply &Prepared,
                                                  FApplyResult &OutResult);
  friend UEAGENTINTERFACE_API bool RollbackPrepared(FPreparedApply &Prepared,
                                                    FApplyResult &OutResult);
};
} // namespace UeAgentAuthoringObjectApply

// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Containers/ArrayView.h"
#include "UObject/UnrealType.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"

class FJsonObject;
class FJsonValue;
class UObject;
class UPackage;
class UStruct;
class UScriptStruct;

namespace UeAgentAuthoringObjectApply {
using UE::UAI::Contract::FDiagnostic;
using FDomainOwnerAuthorizationHandle = uint64;

/** Overlay decoded, explicit struct changes onto current native storage. Uses
 * the same semantic field boundaries as property notifications: ordinary struct
 * members recurse, native atomic values and containers remain whole values.
 * Before and Edited have the same type; unchanged fields preserve native effects.
 * The caller retains ownership of native setters, notifications and validation. */
UEAGENTINTERFACE_API void ApplyStructChanges(UScriptStruct* Struct,
    const void* Before, const void* Edited, void* Current, UObject* Owner = nullptr);

struct FStructChange {
  const void* Before = nullptr;
  const void* Edited = nullptr;
};
/** Merge independent source edits after native identities converge. Conflicting
 * explicit values report their field path before modifying Current. */
UEAGENTINTERFACE_API bool TryApplyStructChanges(UScriptStruct* Struct,
    TConstArrayView<FStructChange> Edits, void* Current, UObject* Owner, FDiagnostic& Error);

UEAGENTINTERFACE_API FDomainOwnerAuthorizationHandle
RegisterDomainAuthorizedOwnerPackagePrefix(const FString &PackagePrefix);
UEAGENTINTERFACE_API void UnregisterDomainAuthorizedOwnerPackagePrefix(
    FDomainOwnerAuthorizationHandle Handle);

struct FFailure;
struct FResolvedNode;

struct UEAGENTINTERFACE_API FOptions {
  struct FNativeStage {
    TSet<FName> RootProperties;
    // The native owner explicitly supports a whole-object event for this group.
    // Otherwise preserve every original property notification chain.
    bool bWholeObjectNotification = false;
    // Native derived state that must independently agree with the preview,
    // including when no explicit edit was supplied for that property.
    TSet<FName> ReadbackProperties;
    // Read and compare these native effects at every checkpoint without
    // publishing them as properties of the edited asset.
    TSet<FName> VerificationOnlyReadbackProperties;
  };
  bool bAllowReadOnly = false;
  bool bAllowTemplateOwners = false;
  bool bAllowDomainAuthorizedOwners = false;
  bool bEmitEditorNotifications = true;
  /** Native owner reconstructs itself on post-edit. Write all fields before
   * one whole-object notification; the lifecycle must rebind and checkpoint it. */
  bool bWholeObjectNotification = false;
  bool bMarkPackageDirty = true;
  /** False for detached struct storage whose UObject is only a resolution or
   * notification context. Prepared value rollback remains available. */
  bool bRecordOwnerTransaction = true;
  /** Ordinary authoring values may omit the shared single-field codec envelopes. */
  bool bCompactValues = false;
  /** Matches a native Details view that explicitly shows EditDefaultsOnly fields. */
  bool bShowDefaultsOnly = false;
  /** Use the native instance field policy independently of preview/archetype flags. */
  bool bUseInstanceEditability = false;
  TFunction<bool(const FProperty*)> AuthorizeProperty;
  /** Native owner may permit resizing a specific EditFixedSize array. Does not grant field write permission. */
  TFunction<bool(const FProperty*)> AuthorizeArrayResize;
  /** Narrows domain field scope even for otherwise editable properties. Never grants write permission. */
  TFunction<bool(const FProperty*)> IncludeProperty;
  /** Narrows only the first declaration of each path in the authoring root.
   * Owned descendants use their own editability; nested plans do not inherit this filter. */
  TFunction<bool(const FProperty*)> IncludeRootProperty;
  EPropertyChangeType::Type NotificationChangeType = EPropertyChangeType::ValueSet;
  /** Native domain notification adapter; used for apply and rollback alike.
   * Return true when handled; false or unset preserves the original native notification. */
  TFunction<bool(UObject&, FPropertyChangedEvent&)> PostEditNotification;
  /** A checked native notification owns the event and reports lifecycle failure.
   * Unlike PostEditNotification, false means failure, never native fallback. */
  TFunction<bool(UObject&, FPropertyChangedEvent&, FFailure&)> CheckedPostEditNotification;
  /** Native relationships outside reflected value storage, before its write and
   * before PostEditChange. Rollback restores those relationships from the same
   * request-local owner. Array origins have already been resolved by this owner. */
  TFunction<void(UObject&, FProperty&, const FResolvedNode&, bool bRestoring)> NativePropertyTransition;
  /** Relationship-only edits still need a write/notification when values compare equal. */
  TFunction<bool(const UObject&, const FProperty&, const FResolvedNode&)> NeedsNativePropertyTransition;
  // Lifecycle policy, never an authored value. Dependent stages are prepared
  // against the preceding native settle, under the operation's full checkpoint.
  TArray<FNativeStage> NativeStages;
  // Only isolated normalization previews may capture instead of verifying each
  // native stage. Actual operations retain the default strict comparison.
  bool bVerifyNativeStages = true;
  /** Domain-owned scoped storage can notify its containing UObject member.
   * This is a whole-member ValueSet, never an inferred array operation.
   * Also supplies the registered native owned factory's host-member context. */
  FString StructNotificationOwnerPropertyPath;
  /** Caller-owned logical target for diagnostics; native object resolution keeps its own owner path. */
  FString DiagnosticTargetPath;
	/** Supplement source positions from the actual descriptor occurrence being processed.
   * Root identity prevents nested plans from borrowing an unrelated caller map.
   * This callback must retain the diagnostic reason, facts and causes. */
	TFunction<void(UE::UAI::Contract::FDiagnostic&, const FJsonObject&, const UStruct*, const void*, int32)> SupplementDiagnosticSource;
};

struct UEAGENTINTERFACE_API FFailure {
  UeAgentAuthoringDiagnostics::FDiagnostic Diagnostic;
};

struct UEAGENTINTERFACE_API FResolvedNode {
  FString PropertyPath;
  FString ValueKind;
  FString TypePath;
  bool bExplicit = true;
  TArray<FResolvedNode> Children;
  // Internal ordered-array provenance; not an authored field or diagnostic payload.
  int32 CapturedArrayCount = INDEX_NONE;
  TArray<int32> CapturedArrayIndices;

  TSharedRef<FJsonObject> ToJsonObject() const;
};

struct UEAGENTINTERFACE_API FApplyResult {
  FString Status;
  UeAgentAuthoringDiagnostics::FDiagnostic Diagnostic;
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

/** Decoded authoring intent without native field/storage pointers. The domain
 * supplies the current root after reconstruction; conversion and comparison
 * remain owned by the common property codec. This is not a rollback checkpoint. */
class UEAGENTINTERFACE_API FPropertyIntent {
public:
  struct FImpl;
  bool IsEmpty() const;
  bool VerifyObject(UObject* RootObject, FApplyResult& OutResult) const;
  bool VerifyStruct(UStruct* RootStruct, void* RootData, UObject* Owner,
                    FApplyResult& OutResult) const;
private:
  TSharedPtr<const FImpl> Impl;
  friend class FPreparedApply;
};

/** Notify real UObject members around a domain-owned structural mutation.
 * Paths name whole members; the domain supplies only members it actually changes. */
UEAGENTINTERFACE_API bool NotifyObjectMembers(UObject& Owner,
    const TArray<FName>& Members, bool bPre, FFailure& OutFailure);
/** Whole-object lifecycle notification. Without a checked adapter, preserves
 * UObject::PostEditChange; a checked adapter must finish or report failure. */
UEAGENTINTERFACE_API bool NotifyObject(UObject& Owner,
    const FOptions& Options, FFailure& OutFailure);

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
/** Production observation; never changes native state or execution success. */
UEAGENTINTERFACE_API void ObservePrepared(FPreparedApply& Prepared);
/** Verify the original decoded intent after native struct storage was copied or
 * replaced. The struct type and owned references retain their prepared identity;
 * this does not apply values, rebuild expected values, or change the rollback root. */
UEAGENTINTERFACE_API bool VerifyPreparedStruct(FPreparedApply &Prepared,
    UStruct* RootStruct, void* RootData, UObject* NotificationOwner,
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
  bool HasTextSourceEdits() const;
  void SetTextIdentityPackage(UPackage* Package);
  /** Freeze only resolved text leaves; keep other authoring descriptors intact. */
  TArray<TSharedPtr<FJsonValue>> GetResolvedPropertyDescriptors() const;
  /** Freeze original decoded values after native creation/text identity have
   * been resolved. Never substitute a capture of the written native values. */
  bool CaptureIntent(FPropertyIntent& OutIntent, FFailure& OutFailure, bool bIncludeUnchanged = true) const;
  /** Requested equal values remain intent even though they need no write. */
  bool CaptureUnchangedIntent(FPropertyIntent& OutIntent, FFailure& OutFailure) const;
  /** Execute only on an isolated native normalization preview. Changed values
   * become proposal evidence through CaptureNativeNormalizedDescriptors; they
   * are not observations of a completed authored write. */
  bool ApplyNativeNormalizationPreview(FApplyResult& OutResult);
  /** Capture native normalized values on an explicitly isolated preview only.
   * Unchanged descriptors (including owned identities) remain intact. */
  bool CaptureNativeNormalizedDescriptors(TArray<TSharedPtr<FJsonValue>>& OutDescriptors,
      FFailure& OutFailure) const;

private:
  bool CaptureIntentInternal(FPropertyIntent& OutIntent, FFailure& OutFailure,
      bool bUnchangedOnly, bool bIncludeUnchanged) const;
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
  friend UEAGENTINTERFACE_API bool VerifyPreparedStruct(FPreparedApply &Prepared,
      UStruct* RootStruct, void* RootData, UObject* NotificationOwner,
      FApplyResult &OutResult);
  friend UEAGENTINTERFACE_API bool RollbackPrepared(FPreparedApply &Prepared,
                                                    FApplyResult &OutResult);
};
} // namespace UeAgentAuthoringObjectApply

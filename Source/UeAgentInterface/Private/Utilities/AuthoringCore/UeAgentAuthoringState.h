// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringIdentity.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringPropertyValues.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringSemanticRegistry.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringTargetLocator.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringTypeDescriptors.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringReflectionDescriptors.h"

class FJsonObject;
class FJsonValue;
class FProperty;
class UObject;
class UScriptStruct;
class UStruct;

namespace UeAgentAuthoringState
{
	using UE::UAI::Contract::FDiagnostic;
	enum class ERegistrationResult : uint8
	{
		Registered,
		AlreadyRegistered
	};

	enum class EReadOutcome : uint8
	{
		Unavailable,
		Resolved,
		Failed
	};

	struct UEAGENTINTERFACE_API FReadLimits
	{
		TFunction<bool(const UScriptStruct* Struct, const FProperty* Field)> StructFieldFilter;
		TFunction<bool(const FProperty*)> AuthorizeProperty;
		bool bShowDefaultsOnly = false;
		/** Native editor treats this surface as an instance even when a preview/CDO supplies values. */
		bool bUseInstanceEditability = false;
	};

	struct UEAGENTINTERFACE_API FReadRequest
	{
		FString SemanticId;
		UeAgentAuthoringIdentity::FCanonicalIdentity TargetIdentity;
		UeAgentAuthoringTargetLocator::FTargetQuery TargetQuery;
		const UObject* RootObject = nullptr;
		UStruct* RootStruct = nullptr;
		const void* RootData = nullptr;
		FString RootOwnerPath;
		const UObject* NotificationOwner = nullptr;
		FString PropertyPath;
		FReadLimits Limits;
		TMap<FString, FString> Context;
		TOptional<UeAgentAuthoringIdentity::FCanonicalIdentity> SnapshotIdentityOverride;
	};

	struct UEAGENTINTERFACE_API FEditablePropertyReadOptions
	{
		FString SemanticId = TEXT("core.property");
		/** Optional immutable identity-builder scope, supplied by its domain owner.
		 * Equal scope guarantees equal snapshot identity for the same property path.
		 * Selection/support and current limits are always evaluated afresh. */
		FString RequestIdentityScope;
		FReadLimits Limits;
		TSet<FName> ExcludedProperties;
		TFunction<bool(const FProperty* Property, const void* ValuePtr, const UObject* Owner)> IncludeProperty;
		TFunction<bool(
			const FString& PropertyPath,
			UeAgentAuthoringIdentity::FCanonicalIdentity& OutIdentity,
			FDiagnostic& OutError)> BuildSnapshotIdentity;
	};

	struct UEAGENTINTERFACE_API FStateValue
	{
		UeAgentAuthoringIdentity::FCanonicalIdentity TargetIdentity;
		UeAgentAuthoringTypeDescriptors::FDescriptor Type;
		TSharedPtr<FJsonValue> Value;
		TSharedPtr<FJsonValue> ValueDefinition;
		TSharedPtr<UeAgentAuthoringPropertyValues::FPropertyValueStorage> NativeValue;
		bool bAvailable = true;
		// Unset inherits the registered semantic write capability. A native
		// accessor can still explicitly report read-only for this instance.
		TOptional<bool> bEditable;
		bool bAuthoringVisible = true;
		FString AvailabilityReason;
		TMap<FString, FString> Constraints;
		FString DisplayName;
		FString Tooltip;
		TArray<FString> CategoryPath;
		TArray<FString> PropertyFlags;
		TSharedPtr<FJsonValue> CandidatesHeader;
		TSharedPtr<FJsonValue> Candidates;
		FString ValueText;
		FString Provenance;
	};

	using FStateAccessor = TFunction<EReadOutcome(
		const FReadRequest& Request,
		FStateValue& OutValue,
		FDiagnostic& OutError)>;

	struct UEAGENTINTERFACE_API FStateAccessorDescriptor
	{
		FString AccessorId;
		TArray<FString> TargetKinds;
		TArray<FString> CapabilityTags;

		bool SupportsTargetKind(const FString& TargetKind) const;
		bool operator==(const FStateAccessorDescriptor& Other) const;
		bool operator!=(const FStateAccessorDescriptor& Other) const { return !(*this == Other); }
	};

	class UEAGENTINTERFACE_API FStateAccessorRegistry
	{
	public:
		FStateAccessorRegistry() = default;
		FStateAccessorRegistry(const FStateAccessorRegistry&) = delete;
		FStateAccessorRegistry& operator=(const FStateAccessorRegistry&) = delete;

		static FStateAccessorRegistry& Get();

		bool RegisterAccessor(
			const FStateAccessorDescriptor& Descriptor,
			FStateAccessor Accessor,
			ERegistrationResult& OutResult,
			UE::UAI::Contract::FDiagnostic& OutError);
		bool FindAccessor(const FString& AccessorId, FStateAccessorDescriptor& OutDescriptor) const;
		bool CopyAccessor(const FString& AccessorId, FStateAccessor& OutAccessor) const;
		TArray<FStateAccessorDescriptor> ListAccessors() const;
		int32 GetAccessorCount() const;

	private:
		struct FEntry
		{
			FStateAccessorDescriptor Descriptor;
			FStateAccessor Accessor;
		};

		mutable FRWLock RegistryLock;
		TMap<FString, FEntry> Accessors;
	};

	struct UEAGENTINTERFACE_API FStateEntry
	{
		// Identity of an immutable, actually captured JSON/metadata observation.
		// Writable copies drop it; it never substitutes for a current native read.
		TSharedPtr<const uint8> Observation;
		FString SemanticId;
		UeAgentAuthoringIdentity::FCanonicalIdentity TargetIdentity;
		UeAgentAuthoringTypeDescriptors::FDescriptor Type;
		TSharedPtr<FJsonValue> Value;
		TSharedPtr<FJsonValue> ValueDefinition;
		// Native codec provenance for comparison only; not an editable schema or revision input.
		TSharedPtr<FJsonValue> ComparisonValueDefinition;
		TSharedPtr<const UeAgentAuthoringPropertyValues::FPropertyValueStorage> NativeValue;
		// Preserve the reflection identity when a domain supplies a snapshot identity.
		// Runtime provenance only, like NativeValue; never part of the public schema.
		UeAgentAuthoringIdentity::FCanonicalIdentity NativeTargetIdentity;
		// The scope that produced this native capture travels with it through copies.
		// Runtime-only, like NativeValue; never serialized or inferred from visible fields.
		TSharedPtr<const FReadLimits> NativeReadLimits;
		bool bAvailable = true;
		bool bEditable = false;
		bool bAuthoringVisible = true;
		FString AvailabilityReason;
		TMap<FString, FString> Constraints;
		FString DisplayName;
		FString Tooltip;
		TArray<FString> CategoryPath;
		TArray<FString> PropertyFlags;
		TSharedPtr<FJsonValue> CandidatesHeader;
		TSharedPtr<FJsonValue> Candidates;
		FString AccessorId;
		FString ValueText;
		FString Provenance;

		bool IsValid() const;
		FDiagnostic GetValidationDiagnostic() const;
		TSharedRef<FJsonObject> ToJsonObject() const;
		TSharedRef<FJsonObject> ToDiagnosticJsonObject() const;
	};

	class UEAGENTINTERFACE_API FStateSnapshot
	{
	public:
		int32 Num() const { return Entries.Num(); }
		bool IsEmpty() const { return Entries.IsEmpty(); }
		bool CopyEntry(int32 Index, FStateEntry& OutEntry) const;
		bool FindEntry(
			const FString& SemanticId,
			const FString& TargetCanonicalKey,
			FStateEntry& OutEntry) const;
		TArray<FStateEntry> CopyEntries() const;
		/** Immutable observation view; callers must not mutate referenced JSON. */
		const TArray<FStateEntry>& GetEntries() const { return Entries; }
		/** Replace already captured identities, retaining the complete baseline. */
		bool TryOverlay(const FStateSnapshot& ActualEntries, FStateSnapshot& OutSnapshot,
			FDiagnostic& OutError, bool bReuseIdenticalObservations = false) const;
		/** Release native property storage while retaining the canonical snapshot and metadata. */
		void ReleaseNativeValues();
		/** Domain lifecycle may retain only storage whose field/type lifetime it owns. */
		void ReleaseNativeValuesIf(TFunctionRef<bool(const FStateEntry&)> ShouldRelease);
		TSharedRef<FJsonObject> ToJsonObject() const;

	private:
		bool TryOverlayInPlace(const FStateSnapshot& ActualEntries, FDiagnostic& OutError,
			bool bReuseIdenticalObservations);
		TArray<FStateEntry> Entries;
		friend class FSnapshotBuilder;
	};

	struct UEAGENTINTERFACE_API FCaptureResult
	{
		FString Status;
		FDiagnostic Diagnostic;
		FString SemanticId;
		FString AccessorId;
		UeAgentAuthoringIdentity::FCanonicalIdentity TargetIdentity;
		FStateSnapshot Snapshot;

		bool IsCaptured() const;
		TSharedRef<FJsonObject> ToJsonObject() const;
	};

	UEAGENTINTERFACE_API TSharedPtr<FJsonValue> CloneCanonicalValue(const TSharedPtr<FJsonValue>& Value);
	UEAGENTINTERFACE_API FString MakeStableCanonicalValueKey(const TSharedPtr<FJsonValue>& Value);

	UEAGENTINTERFACE_API bool RegisterBuiltInStateAccessors(
		FStateAccessorRegistry& Registry,
		UE::UAI::Contract::FDiagnostic& OutError);

	// Snapshot reads can retain the logical owner while reading temporary object storage.
	UEAGENTINTERFACE_API bool BuildPropertyReadRequest(
		const UObject* RootObject,
		const FString& PropertyPath,
		const FString& SemanticId,
		FReadRequest& OutRequest,
		UeAgentAuthoringTargetLocator::FResolveResult& OutTargetResult,
		const FString& CanonicalRootOwnerPath = FString());

	UEAGENTINTERFACE_API bool BuildStructPropertyReadRequest(
		UStruct* RootStruct,
		const void* RootData,
		const FString& RootOwnerPath,
		const UObject* NotificationOwner,
		const FString& PropertyPath,
		const FString& SemanticId,
		FReadRequest& OutRequest,
		UeAgentAuthoringTargetLocator::FResolveResult& OutTargetResult);

	UEAGENTINTERFACE_API bool AppendEditableObjectPropertyReadRequests(
		const UObject* RootObject,
		const FEditablePropertyReadOptions& Options,
		TArray<FReadRequest>& OutRequests,
		FDiagnostic& OutError);

	UEAGENTINTERFACE_API bool AppendEditableStructPropertyReadRequests(
		UStruct* RootStruct,
		const void* RootData,
		const FString& RootOwnerPath,
		const UObject* NotificationOwner,
		const FEditablePropertyReadOptions& Options,
		TArray<FReadRequest>& OutRequests,
		FDiagnostic& OutError);

	UEAGENTINTERFACE_API UObject* ResolveRequestRootObject(
		const FReadRequest& Request,
		FDiagnostic& OutError);

	/** The accessor and typed codec are shared. Only authoring presentation may omit defaults. */
	enum class ECaptureValueMode : uint8 { Authoring, Native };

	UEAGENTINTERFACE_API bool CaptureSnapshot(
		const TArray<FReadRequest>& Requests,
		FCaptureResult& OutResult,
		const UeAgentAuthoringSemanticRegistry::FRegistry* SemanticRegistry = nullptr,
		const FStateAccessorRegistry* AccessorRegistry = nullptr,
		ECaptureValueMode ValueMode = ECaptureValueMode::Authoring);
}

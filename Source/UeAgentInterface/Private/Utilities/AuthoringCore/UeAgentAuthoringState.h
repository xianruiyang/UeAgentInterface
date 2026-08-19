// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringIdentity.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringPropertyValues.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringSemanticRegistry.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringTargetLocator.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringTypeDescriptors.h"

class FJsonObject;
class FJsonValue;
class FProperty;
class UObject;
class UScriptStruct;
class UStruct;

namespace UeAgentAuthoringState
{
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
		int32 MaxContainerItems = 256;
		int32 MaxStringChars = 4096;
		TFunction<bool(const UScriptStruct* Struct, const FProperty* Field)> StructFieldFilter;
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
		FReadLimits Limits;
		TSet<FName> ExcludedProperties;
		TFunction<bool(const FProperty* Property, const void* ValuePtr, const UObject* Owner)> IncludeProperty;
		TFunction<bool(
			const FString& PropertyPath,
			UeAgentAuthoringIdentity::FCanonicalIdentity& OutIdentity,
			FString& OutError)> BuildSnapshotIdentity;
	};

	struct UEAGENTINTERFACE_API FStateValue
	{
		UeAgentAuthoringIdentity::FCanonicalIdentity TargetIdentity;
		UeAgentAuthoringTypeDescriptors::FDescriptor Type;
		TSharedPtr<FJsonValue> Value;
		TSharedPtr<UeAgentAuthoringPropertyValues::FPropertyValueStorage> NativeValue;
		bool bAvailable = true;
		bool bEditable = false;
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
		FString& OutError)>;

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
			FString& OutError);
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
		FString SemanticId;
		UeAgentAuthoringIdentity::FCanonicalIdentity TargetIdentity;
		UeAgentAuthoringTypeDescriptors::FDescriptor Type;
		TSharedPtr<FJsonValue> Value;
		TSharedPtr<const UeAgentAuthoringPropertyValues::FPropertyValueStorage> NativeValue;
		bool bAvailable = true;
		bool bEditable = false;
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
		TSharedRef<FJsonObject> ToJsonObject() const;

	private:
		TArray<FStateEntry> Entries;
		friend class FSnapshotBuilder;
	};

	struct UEAGENTINTERFACE_API FCaptureResult
	{
		FString Status;
		FString ErrorCode;
		FString ErrorDetail;
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
		FString& OutError);

	UEAGENTINTERFACE_API bool BuildPropertyReadRequest(
		const UObject* RootObject,
		const FString& PropertyPath,
		const FString& SemanticId,
		FReadRequest& OutRequest,
		UeAgentAuthoringTargetLocator::FResolveResult& OutTargetResult);

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
		FString& OutError);

	UEAGENTINTERFACE_API bool AppendEditableStructPropertyReadRequests(
		UStruct* RootStruct,
		const void* RootData,
		const FString& RootOwnerPath,
		const UObject* NotificationOwner,
		const FEditablePropertyReadOptions& Options,
		TArray<FReadRequest>& OutRequests,
		FString& OutError);

	UEAGENTINTERFACE_API UObject* ResolveRequestRootObject(
		const FReadRequest& Request,
		FString& OutError);

	UEAGENTINTERFACE_API bool CaptureSnapshot(
		const TArray<FReadRequest>& Requests,
		FCaptureResult& OutResult,
		const UeAgentAuthoringSemanticRegistry::FRegistry* SemanticRegistry = nullptr,
		const FStateAccessorRegistry* AccessorRegistry = nullptr);
}

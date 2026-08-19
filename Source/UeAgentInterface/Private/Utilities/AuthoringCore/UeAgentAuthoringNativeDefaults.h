// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringIdentity.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringPropertyValues.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringSemanticRegistry.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringTargetLocator.h"

class FJsonObject;
class FJsonValue;
class FProperty;
class UClass;
class UObject;
class UPackage;
class UScriptStruct;

namespace UeAgentAuthoringNativeDefaults
{
	enum class EOperation : uint8
	{
		Update,
		Patch,
		NestedPatch,
		Create,
		Replace,
		OwnedObjectCreate,
		GeneratedAfterApply,
		PostCreateReadback,
		InspectDefault,
		Reset
	};

	enum class EProbeOutcome : uint8
	{
		Unavailable,
		Resolved,
		Failed
	};

	enum class ERegistrationResult : uint8
	{
		Registered,
		AlreadyRegistered
	};

	UEAGENTINTERFACE_API FString OperationToString(EOperation Operation);
	UEAGENTINTERFACE_API FString SourceKindToString(UeAgentAuthoringSemanticRegistry::EDefaultSourceKind SourceKind);
	UEAGENTINTERFACE_API int32 GetSourcePriority(UeAgentAuthoringSemanticRegistry::EDefaultSourceKind SourceKind);

	using FPropertyValueStorage = UeAgentAuthoringPropertyValues::FPropertyValueStorage;

	class UEAGENTINTERFACE_API FScratchScope
	{
	public:
		FScratchScope();
		~FScratchScope();

		FScratchScope(const FScratchScope&) = delete;
		FScratchScope& operator=(const FScratchScope&) = delete;

		UPackage* CreateScratchPackage(FString& OutError);
		UObject* CreateObject(
			UClass* ObjectClass,
			UObject* Outer,
			UObject* ArchetypeOrTemplate,
			FString& OutError);
		bool AdoptObjectFromScratchPackage(UObject* Object, FString& OutError);
		bool Contains(const UObject* Object) const;
		int32 NumTrackedObjects() const;
		void Cleanup();

		static int32 GetActiveScopeCount();
		static int32 GetTrackedObjectCount();

	private:
		struct FImpl;
		TUniquePtr<FImpl> Impl;
	};

	struct UEAGENTINTERFACE_API FRequest
	{
		EOperation Operation = EOperation::Create;
		UeAgentAuthoringIdentity::FCanonicalIdentity TargetIdentity;
		UeAgentAuthoringTargetLocator::FTargetQuery TargetQuery;
		FProperty* Property = nullptr;
		FString PropertyPath;
		UObject* ExistingOwner = nullptr;
		const void* ExistingValuePtr = nullptr;
		UClass* ObjectClass = nullptr;
		UObject* ProposedOuter = nullptr;
		UObject* ArchetypeOrTemplate = nullptr;
		UScriptStruct* StructType = nullptr;
		bool bConstructScratchObject = false;
		bool bRequired = false;
		TArray<FString> ProviderIds;
		TMap<FString, FString> Context;
	};

	struct UEAGENTINTERFACE_API FProbeValue
	{
		UeAgentAuthoringIdentity::FCanonicalIdentity SourceIdentity;
		TSharedPtr<FJsonValue> Value;
		TSharedPtr<FPropertyValueStorage> PropertyValue;
		FString ValueText;
		FString Provenance;
	};

	using FProbe = TFunction<EProbeOutcome(
		const FRequest& Request,
		FScratchScope& Scratch,
		FProbeValue& OutValue,
		FString& OutError)>;

	struct UEAGENTINTERFACE_API FProviderDescriptor
	{
		FString ProviderId;
		UeAgentAuthoringSemanticRegistry::EDefaultSourceKind SourceKind = UeAgentAuthoringSemanticRegistry::EDefaultSourceKind::None;
		TArray<FString> TargetKinds;
		TArray<FString> CapabilityTags;
		bool bRequiresScratch = false;

		bool operator==(const FProviderDescriptor& Other) const;
		bool operator!=(const FProviderDescriptor& Other) const { return !(*this == Other); }
	};

	class UEAGENTINTERFACE_API FProviderRegistry
	{
	public:
		FProviderRegistry() = default;
		FProviderRegistry(const FProviderRegistry&) = delete;
		FProviderRegistry& operator=(const FProviderRegistry&) = delete;

		static FProviderRegistry& Get();

		bool RegisterProvider(
			const FProviderDescriptor& Descriptor,
			FProbe Probe,
			ERegistrationResult& OutResult,
			FString& OutError);
		bool FindProvider(const FString& ProviderId, FProviderDescriptor& OutDescriptor) const;
		TArray<FProviderDescriptor> ListProviders() const;
		int32 GetProviderCount() const;
		bool CopyProbe(const FString& ProviderId, FProbe& OutProbe) const;

	private:
		struct FEntry
		{
			FProviderDescriptor Descriptor;
			FProbe Probe;
		};

		mutable FRWLock RegistryLock;
		TMap<FString, FEntry> Providers;
	};

	struct UEAGENTINTERFACE_API FAttempt
	{
		FString ProviderId;
		UeAgentAuthoringSemanticRegistry::EDefaultSourceKind SourceKind = UeAgentAuthoringSemanticRegistry::EDefaultSourceKind::None;
		FString Outcome;
		FString ErrorCode;
	};

	struct UEAGENTINTERFACE_API FDiagnosticCandidate
	{
		FString ProviderId;
		UeAgentAuthoringSemanticRegistry::EDefaultSourceKind SourceKind = UeAgentAuthoringSemanticRegistry::EDefaultSourceKind::None;
		UeAgentAuthoringIdentity::FCanonicalIdentity SourceIdentity;
		FString Provenance;
	};

	struct UEAGENTINTERFACE_API FResult
	{
		FString Status;
		FString ErrorCode;
		FString ErrorDetail;
		EOperation Operation = EOperation::Create;
		bool bPreserveExisting = false;
		bool bDeferredUntilPostCreate = false;
		FString ProviderId;
		UeAgentAuthoringSemanticRegistry::EDefaultSourceKind SourceKind = UeAgentAuthoringSemanticRegistry::EDefaultSourceKind::None;
		UeAgentAuthoringIdentity::FCanonicalIdentity TargetIdentity;
		UeAgentAuthoringIdentity::FCanonicalIdentity SourceIdentity;
		TSharedPtr<FJsonValue> Value;
		TSharedPtr<FPropertyValueStorage> PropertyValue;
		FString ValueText;
		FString Provenance;
		TArray<FAttempt> Attempts;
		TArray<FDiagnosticCandidate> Candidates;

		bool IsResolved() const;
		TSharedRef<FJsonObject> ToJsonObject(bool bIncludeValue = false) const;
	};

	UEAGENTINTERFACE_API bool RegisterBuiltInProviders(FProviderRegistry& Registry, FString& OutError);

	UEAGENTINTERFACE_API bool BuildPropertyRequest(
		UObject* Object,
		const FString& PropertyPath,
		EOperation Operation,
		bool bRequired,
		FRequest& OutRequest,
		UeAgentAuthoringTargetLocator::FResolveResult& OutTargetResult);

	UEAGENTINTERFACE_API bool Resolve(
		const FRequest& Request,
		FResult& OutResult,
		const FProviderRegistry* Registry = nullptr);
}

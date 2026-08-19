// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringState.h"

class FJsonObject;
class FJsonValue;
class FProperty;
class UObject;

namespace UeAgentAuthoringSemanticCompare
{
	enum class ERegistrationResult : uint8
	{
		Registered,
		AlreadyRegistered
	};

	struct UEAGENTINTERFACE_API FComparatorDescriptor
	{
		FString ComparatorId;
		TArray<FString> ValueKinds;
		TArray<FString> CapabilityTags;

		bool SupportsValueKind(const FString& ValueKind) const;
		bool operator==(const FComparatorDescriptor& Other) const;
		bool operator!=(const FComparatorDescriptor& Other) const { return !(*this == Other); }
	};

	using FComparator = TFunction<bool(
		const UeAgentAuthoringState::FStateEntry& Left,
		const UeAgentAuthoringState::FStateEntry& Right,
		bool& bOutEquivalent,
		FString& OutError)>;

	class UEAGENTINTERFACE_API FComparatorRegistry
	{
	public:
		FComparatorRegistry() = default;
		FComparatorRegistry(const FComparatorRegistry&) = delete;
		FComparatorRegistry& operator=(const FComparatorRegistry&) = delete;

		static FComparatorRegistry& Get();

		bool RegisterComparator(
			const FComparatorDescriptor& Descriptor,
			FComparator Comparator,
			ERegistrationResult& OutResult,
			FString& OutError);
		bool FindComparator(const FString& ComparatorId, FComparatorDescriptor& OutDescriptor) const;
		bool CopyComparator(const FString& ComparatorId, FComparator& OutComparator) const;
		TArray<FComparatorDescriptor> ListComparators() const;
		int32 GetComparatorCount() const;

	private:
		struct FEntry
		{
			FComparatorDescriptor Descriptor;
			FComparator Comparator;
		};

		mutable FRWLock RegistryLock;
		TMap<FString, FEntry> Comparators;
	};

	struct UEAGENTINTERFACE_API FCompareOptions
	{
		bool bCompareAvailability = true;
		bool bCompareValue = true;
	};

	struct UEAGENTINTERFACE_API FEntryComparison
	{
		bool bEquivalent = false;
		FString Status;
		FString ErrorCode;
		FString ErrorDetail;
		FString SemanticId;
		FString ComparatorId;
		UeAgentAuthoringIdentity::FCanonicalIdentity TargetIdentity;

		TSharedRef<FJsonObject> ToJsonObject() const;
	};

	struct UEAGENTINTERFACE_API FComparisonReport
	{
		bool bEquivalent = false;
		FString Status;
		FString ErrorCode;
		FString ErrorDetail;
		TArray<FEntryComparison> Entries;

		TSharedRef<FJsonObject> ToJsonObject() const;
	};

	struct UEAGENTINTERFACE_API FBindingIssue
	{
		FString SemanticId;
		FString ErrorCode;
		FString ErrorDetail;

		TSharedRef<FJsonObject> ToJsonObject() const;
	};

	struct UEAGENTINTERFACE_API FBindingReport
	{
		bool bValid = false;
		FString Status;
		FString ErrorCode;
		TArray<FBindingIssue> Issues;

		TSharedRef<FJsonObject> ToJsonObject() const;
	};

	UEAGENTINTERFACE_API bool RegisterBuiltInComparators(
		FComparatorRegistry& Registry,
		FString& OutError);

	UEAGENTINTERFACE_API bool BuildExpectedPropertyEntry(
		const FString& SemanticId,
		const UeAgentAuthoringIdentity::FCanonicalIdentity& TargetIdentity,
		FProperty* Property,
		const TSharedPtr<FJsonValue>& CanonicalValue,
		UObject* Owner,
		UeAgentAuthoringState::FStateEntry& OutEntry,
		FString& OutError);

	UEAGENTINTERFACE_API bool ArePropertyValuesEquivalent(
		FProperty* Property,
		const void* LeftValuePtr,
		const void* RightValuePtr,
		UObject* Owner,
		bool& bOutEquivalent,
		FString& OutError);

	UEAGENTINTERFACE_API bool CompareEntries(
		const UeAgentAuthoringState::FStateEntry& Left,
		const UeAgentAuthoringState::FStateEntry& Right,
		const FCompareOptions& Options,
		FEntryComparison& OutResult,
		const UeAgentAuthoringSemanticRegistry::FRegistry* SemanticRegistry = nullptr,
		const FComparatorRegistry* ComparatorRegistry = nullptr);

	UEAGENTINTERFACE_API bool CompareSnapshots(
		const UeAgentAuthoringState::FStateSnapshot& Left,
		const UeAgentAuthoringState::FStateSnapshot& Right,
		const FCompareOptions& Options,
		FComparisonReport& OutReport,
		const UeAgentAuthoringSemanticRegistry::FRegistry* SemanticRegistry = nullptr,
		const FComparatorRegistry* ComparatorRegistry = nullptr);

	UEAGENTINTERFACE_API bool ValidateSharedCoreBindings(
		FBindingReport& OutReport,
		const UeAgentAuthoringSemanticRegistry::FRegistry* SemanticRegistry = nullptr,
		const UeAgentAuthoringIdentity::FRegistry* IdentityRegistry = nullptr,
		const UeAgentAuthoringState::FStateAccessorRegistry* AccessorRegistry = nullptr,
		const FComparatorRegistry* ComparatorRegistry = nullptr);
}

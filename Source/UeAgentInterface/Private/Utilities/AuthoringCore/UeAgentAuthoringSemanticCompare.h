// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"

#include "CoreMinimal.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringState.h"

class FJsonObject;
class FJsonValue;
class FProperty;
class UObject;

namespace UeAgentAuthoringSemanticCompare
{
	// Shared graph-layout policies selected by the native semantic owner.
	inline constexpr const TCHAR* GraphInt32PositionComparator = TEXT("uai.compare.graph_int32_position");
	inline constexpr const TCHAR* GraphRoundedInt32PositionComparator = TEXT("uai.compare.graph_rounded_int32_position");
	inline constexpr const TCHAR* GraphVectorPositionComparator = TEXT("uai.compare.graph_vector_position");
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
		UE::UAI::Contract::FDiagnostic& OutError)>;

	// Projects only documented native identity/lifecycle normalization before
	// the shared original-delta comparison. The entry is a comparison copy: a
	// value-shape projection must update its comparison declaration as well.
	// It never applies an asset mutation.
	using FOriginalIntentProjection = TFunction<bool(
		UeAgentAuthoringState::FStateEntry& NativeEntry,
		TSharedPtr<FJsonValue>& Before, TSharedPtr<FJsonValue>& Desired,
		TSharedPtr<FJsonValue>& Actual, TMap<FString, FString>& SourcePaths,
		UE::UAI::Contract::FDiagnostic& OutError)>;

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
			UE::UAI::Contract::FDiagnostic& OutError,
			FOriginalIntentProjection OriginalIntentProjection = {});
		bool FindComparator(const FString& ComparatorId, FComparatorDescriptor& OutDescriptor) const;
		bool CopyComparator(const FString& ComparatorId, FComparator& OutComparator) const;
		bool ProjectOriginalIntent(const FString& ComparatorId,
			UeAgentAuthoringState::FStateEntry& NativeEntry,
			TSharedPtr<FJsonValue>& Before, TSharedPtr<FJsonValue>& Desired,
			TSharedPtr<FJsonValue>& Actual, TMap<FString, FString>& SourcePaths,
			UE::UAI::Contract::FDiagnostic& OutError) const;
		TArray<FComparatorDescriptor> ListComparators() const;
		int32 GetComparatorCount() const;

	private:
		struct FEntry
		{
			FComparatorDescriptor Descriptor;
			FComparator Comparator;
			FOriginalIntentProjection OriginalIntentProjection;
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
		UE::UAI::Contract::FDiagnostic Diagnostic;
		FString SemanticId;
		FString ComparatorId;
		UeAgentAuthoringIdentity::FCanonicalIdentity TargetIdentity;

		TSharedRef<FJsonObject> ToJsonObject() const;
		void AppendDifferenceValues(FJsonObject& Object) const;
	};

	struct UEAGENTINTERFACE_API FComparisonReport
	{
		bool bEquivalent = false;
		FString Status;
		UE::UAI::Contract::FDiagnostic Diagnostic;
		TArray<FEntryComparison> Entries;

		TSharedRef<FJsonObject> ToJsonObject() const;
	};

	struct UEAGENTINTERFACE_API FBindingIssue
	{
		UE::UAI::Contract::FDiagnostic Diagnostic;

		TSharedRef<FJsonObject> ToJsonObject() const;
	};

	struct UEAGENTINTERFACE_API FBindingReport
	{
		bool bValid = false;
		FString Status;
		UE::UAI::Contract::FDiagnostic Diagnostic;
		TArray<FBindingIssue> Issues;

		TSharedRef<FJsonObject> ToJsonObject() const;
	};

	UEAGENTINTERFACE_API bool RegisterBuiltInComparators(
		FComparatorRegistry& Registry,
		UE::UAI::Contract::FDiagnostic& OutError);

	/** Uses declared codec rules and retains the source difference; undeclared members remain exact.
	 * This does not validate or apply an input. */
	UEAGENTINTERFACE_API UE::UAI::StateSemantic::CanonicalJson::FValueComparison CompareDeclaredValues(
		const TSharedPtr<FJsonObject>& Type, const TSharedPtr<FJsonObject>& Definition,
		const TSharedPtr<FJsonValue>& Left, const TSharedPtr<FJsonValue>& Right);
	UEAGENTINTERFACE_API bool AreDeclaredValuesEquivalent(
		const TSharedPtr<FJsonObject>& Type, const TSharedPtr<FJsonObject>& Definition,
		const TSharedPtr<FJsonValue>& Left, const TSharedPtr<FJsonValue>& Right);

	/** Bind an explicitly projected keyed field; ordinary authoring arrays are unchanged. */
	UEAGENTINTERFACE_API void BindComparisonObjectRows(
		UeAgentAuthoringState::FStateEntry& Entry, const FString& Field,
		const TSharedPtr<FJsonObject>& RowTypes);
	/** Remap native array item declarations to the projector's explicit source identities. */
	UEAGENTINTERFACE_API bool BindComparisonCollectionKeys(
		UeAgentAuthoringState::FStateEntry& Entry, const FString& Field,
		const TMap<FString, int32>& SourceToNativeIndex, UE::UAI::Contract::FDiagnostic& OutError);

	/** Apply only original edits to the native result. Declared values use the
	 * same codec comparison as final verification; undeclared documents stay exact. */
	UEAGENTINTERFACE_API TSharedPtr<FJsonValue> OverlayValueDelta(
		const TSharedPtr<FJsonValue>& Before, const TSharedPtr<FJsonValue>& Desired,
		const TSharedPtr<FJsonValue>& Actual,
		const UeAgentAuthoringState::FStateEntry* Declaration = nullptr);

	/** Resolve an RFC JSON pointer using only the native declaration's key semantics.
	 * The resolved pointer retains the spelling present in Value, including when a
	 * later path segment is missing, so diagnostics can locate the authored source. */
	UEAGENTINTERFACE_API TSharedPtr<FJsonValue> FindDeclaredValuePath(
		const UeAgentAuthoringState::FStateEntry& Declaration,
		const TSharedPtr<FJsonValue>& Value, const FString& Path, FString* OutResolvedPath = nullptr);

	UEAGENTINTERFACE_API bool BuildExpectedPropertyEntry(
		const FString& SemanticId,
		const UeAgentAuthoringIdentity::FCanonicalIdentity& TargetIdentity,
		FProperty* Property,
		const TSharedPtr<FJsonValue>& CanonicalValue,
		UObject* Owner,
		UeAgentAuthoringState::FStateEntry& OutEntry,
		UE::UAI::Contract::FDiagnostic& OutError,
		const UeAgentAuthoringState::FReadLimits* CaptureLimits = nullptr);

	/** Normalize against the exact native schema and field scope of a fresh capture. */
	UEAGENTINTERFACE_API bool BuildExpectedPropertyEntry(
		const UeAgentAuthoringState::FStateEntry& Capture,
		const TSharedPtr<FJsonValue>& CanonicalValue,
		UObject* Owner,
		UeAgentAuthoringState::FStateEntry& OutEntry,
		UE::UAI::Contract::FDiagnostic& OutError);

	UEAGENTINTERFACE_API bool ArePropertyValuesEquivalent(
		FProperty* Property,
		const void* LeftValuePtr,
		const void* RightValuePtr,
		UObject* Owner,
		bool& bOutEquivalent,
		UE::UAI::Contract::FDiagnostic& OutError, bool bStoredValues = false);

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

// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"
#include "Containers/StringView.h"
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
namespace UE::UAI::StateSemantic { class FUeAgentCanonicalSnapshot; class FUeAgentCanonicalValue; }

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
		/** Built-in policy, separate from caller callbacks so detached bindings do not retain a closure. */
		bool bApplyAuthoringStructFieldPolicy = false;
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
		/** Selected by the domain's native export-target builder. Does not affect accessor validation. */
		void MarkExportTarget() { Context.Add(TEXT("export_target_request"), TEXT("true")); }
		bool IsExportTarget() const { return Context.Contains(TEXT("export_target_request")); }
	/** This native verification read keeps its value and identity, but the raw
	 * entry does not describe a public authoring field. */
	void MarkVerificationOnly() { Context.Add(TEXT("verification_only"), TEXT("true")); }
	bool IsVerificationOnly() const
	{
		if (Context.IsEmpty()) return false;
		static const FString Key(TEXT("verification_only"));
		const FString* Role = Context.Find(Key);
		return Role && *Role == TEXT("true");
	}
		TOptional<UeAgentAuthoringIdentity::FCanonicalIdentity> SnapshotIdentityOverride;
		/** Requested only for real before/final consumers, not expected/pre-save checks. */
		bool bCaptureAuthoringProjection = false;
		/** Same synchronous capture's top-level field selection. Never retained in
		 * a request template/durable snapshot; a changed binding is an error. */
		FProperty* SelectedRootProperty = nullptr;
		const UStruct* SelectedRootClass = nullptr;
		const FString& GetSemanticId() const { return SemanticId; }
		const FString& GetPropertyPath() const { return PropertyPath; }
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

	/** Immutable request identity plus this capture's native root and read policy.
	 * A binding lives only for a synchronous capture; cached templates hold no roots. */
	struct FCompactReadRequestTemplate;
	class UEAGENTINTERFACE_API FReadRequestBinding
	{
	public:
		FReadRequestBinding(FReadRequest&& Request);
		FReadRequestBinding(TSharedPtr<const FReadRequest> InTemplate, const UObject* InRoot,
			TSharedPtr<const FReadLimits> InLimits);
		FReadRequestBinding(TSharedPtr<const FCompactReadRequestTemplate> InTemplate, const UObject* InRoot,
			TSharedPtr<const FReadLimits> InLimits);
		const FReadRequest& GetTemplate() const;
		const FString& GetSemanticId() const;
		const FString& GetPropertyPath() const;
		bool MatchesSnapshotIdentityKey(const FString& Key) const;
		FString GetSnapshotIdentityKey() const;
		const UObject* GetRootObject() const { return RootObject; }
		const FReadLimits& GetReadLimits() const;
		FReadRequest Materialize() const;
		void BindSelectedRootProperty(FProperty* Property, const UStruct* RootClass);
	private:
		mutable TSharedPtr<const FReadRequest> Template;
		TSharedPtr<const FCompactReadRequestTemplate> CompactTemplate;
		const UObject* RootObject = nullptr;
		TSharedPtr<const FReadLimits> Limits;
		FProperty* SelectedRootProperty = nullptr;
		const UStruct* SelectedRootClass = nullptr;
	};

	struct UEAGENTINTERFACE_API FStateValue
	{
		UeAgentAuthoringIdentity::FCanonicalIdentity TargetIdentity;
		UeAgentAuthoringTypeDescriptors::FDescriptor Type;
		TSharedPtr<FJsonObject> FieldDeclaration;
		/** Explicit export-only pure declaration; ordinary/default DTOs own Type. */
		TSharedPtr<const UeAgentAuthoringReflection::FSharedFieldDeclaration> SharedDeclaration;
		TSharedPtr<FJsonValue> Value;
		/** Same-codec immutable scalar/closed builtin for the selected export consumer. */
		TSharedPtr<const UE::UAI::StateSemantic::FUeAgentCanonicalValue> DirectAuthoringValue;
		TSharedPtr<FJsonValue> ValueDefinition;
		/** Pure semantic facts already captured by a domain's native reader. */
		TSharedPtr<const UE::UAI::StateSemantic::FUeAgentCanonicalSnapshot> AuthoringProjection;
		TSharedPtr<UeAgentAuthoringPropertyValues::FPropertyValueStorage> NativeValue;
		// Borrowed only until this accessor result is consumed synchronously.
		// Authoring export needs normalization, not a retained native value copy.
		FProperty* ValueProperty = nullptr;
		// The shared property accessor transfers its freshly normalized, detached
		// Value/ValueDefinition trees. Custom accessors retain the cloning default.
		// This says nothing about whether native observation may be skipped.
		bool bOwnsNormalizedPropertyData = false;
		// Issued only by an accessor's current-observation path. Capture verifies
		// this token against its sealed baseline before sharing authoring content.
		TSharedPtr<const uint8> ReusedAuthoringObservation;
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
		TSharedPtr<const TArray<FString>> SharedCategoryPath;
		TArray<FString> PropertyFlags;
		TSharedPtr<FJsonValue> CandidatesHeader;
		TSharedPtr<FJsonValue> Candidates;
		FString ValueText;
		FString Provenance;
		/** Builtin remapped export only: the actual native key was compared by
		 * its original writer. This synchronous accessor view ends in ReadOne;
		 * no pointer is transferred into a snapshot, binding or durable record. */
		const UeAgentAuthoringIdentity::FCanonicalIdentity* ScopedTargetIdentity = nullptr;
		const UeAgentAuthoringIdentity::FCanonicalIdentity& GetTargetIdentity() const {
			return ScopedTargetIdentity ? *ScopedTargetIdentity : TargetIdentity;
		}
		const UeAgentAuthoringTypeDescriptors::FDescriptor& GetType() const { return SharedDeclaration ? SharedDeclaration->Type : Type; }
		const TMap<FString, FString>& GetConstraints() const { return SharedDeclaration ? SharedDeclaration->Constraints : Constraints; }
		const TArray<FString>& GetCategoryPath() const { return SharedCategoryPath ? *SharedCategoryPath : CategoryPath; }
	};

	using FStateAccessor = TFunction<EReadOutcome(
		const FReadRequest& Request,
		FStateValue& OutValue,
		FDiagnostic& OutError)>;

	struct FStateEntry;
	struct FStateNativeBinding;
	using FStateAccessorWithBaseline = TFunction<EReadOutcome(
		const FReadRequest&, const FStateEntry&, const FStateNativeBinding&,
		FStateValue&, FDiagnostic&)>;

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
		bool RegisterAccessor(const FStateAccessorDescriptor& Descriptor, FStateAccessor Accessor,
			FStateAccessorWithBaseline WithBaseline, ERegistrationResult& OutResult, FDiagnostic& OutError);
		bool FindAccessor(const FString& AccessorId, FStateAccessorDescriptor& OutDescriptor) const;
		bool CopyAccessor(const FString& AccessorId, FStateAccessor& OutAccessor) const;
		bool CopyAccessor(const FString& AccessorId, FStateAccessor& OutAccessor,
			FStateAccessorWithBaseline& OutWithBaseline) const;
		TArray<FStateAccessorDescriptor> ListAccessors() const;
		int32 GetAccessorCount() const;

	private:
		struct FEntry
		{
			FStateAccessorDescriptor Descriptor;
			FStateAccessor Accessor;
			FStateAccessorWithBaseline WithBaseline;
		};

		mutable FRWLock RegistryLock;
		TMap<FString, FEntry> Accessors;
	};

	/** Current native provenance is independent of immutable authoring content. */
	struct UEAGENTINTERFACE_API FStateNativeBinding
	{
		TSharedPtr<const UeAgentAuthoringPropertyValues::FPropertyValueStorage> NativeValue;
		UeAgentAuthoringIdentity::FCanonicalIdentity NativeTargetIdentity;
		TSharedPtr<const FReadLimits> NativeReadLimits;
	};

	struct UEAGENTINTERFACE_API FStateEntry : FStateNativeBinding
	{
		// Identity of an immutable, actually captured JSON/metadata observation.
		// Writable copies drop it; it never substitutes for a current native read.
		TSharedPtr<const uint8> Observation;
		FString SemanticId;
		UeAgentAuthoringIdentity::FCanonicalIdentity TargetIdentity;
		UeAgentAuthoringTypeDescriptors::FDescriptor Type;
		// Immutable same-capture property declaration from the reflection owner.
		TSharedPtr<FJsonObject> FieldDeclaration;
		TSharedPtr<const UeAgentAuthoringReflection::FSharedFieldDeclaration> SharedDeclaration;
		TSharedPtr<FJsonValue> Value;
		/** Pure current value; ordinary/default snapshots retain their JSON Value. */
		TSharedPtr<const UE::UAI::StateSemantic::FUeAgentCanonicalValue> DirectAuthoringValue;
		TSharedPtr<FJsonValue> ValueDefinition;
		TSharedPtr<const UE::UAI::StateSemantic::FUeAgentCanonicalSnapshot> AuthoringProjection;
		// Native codec provenance for comparison only; not an editable schema or revision input.
		TSharedPtr<FJsonValue> ComparisonValueDefinition;
		bool bAvailable = true;
		bool bEditable = false;
		bool bAuthoringVisible = true;
		FString AvailabilityReason;
		TMap<FString, FString> Constraints;
		FString DisplayName;
		FString Tooltip;
		TArray<FString> CategoryPath;
		TSharedPtr<const TArray<FString>> SharedCategoryPath;
		TArray<FString> PropertyFlags;
		TSharedPtr<FJsonValue> CandidatesHeader;
		TSharedPtr<FJsonValue> Candidates;
		FString AccessorId;
		FString ValueText;
		FString Provenance;
		const UeAgentAuthoringTypeDescriptors::FDescriptor& GetType() const { return SharedDeclaration ? SharedDeclaration->Type : Type; }
		const TMap<FString, FString>& GetConstraints() const { return SharedDeclaration ? SharedDeclaration->Constraints : Constraints; }
		const TArray<FString>& GetCategoryPath() const { return SharedCategoryPath ? *SharedCategoryPath : CategoryPath; }
		/** Owning DTO for an actual legacy/writable consumer, not the export path. */
		void MaterializeDeclaration() {
			if (SharedDeclaration) { Type = SharedDeclaration->Type; Constraints = SharedDeclaration->Constraints; SharedDeclaration.Reset(); }
			if (SharedCategoryPath) { CategoryPath = *SharedCategoryPath; SharedCategoryPath.Reset(); }
		}

		bool IsValid() const;
		FDiagnostic GetValidationDiagnostic() const;
		TSharedRef<FJsonObject> ToJsonObject() const;
		TSharedRef<FJsonObject> ToDiagnosticJsonObject() const;
	};

	// A sealed range in the existing semantic/identity order. Its token proves
	// immutable authoring content, never that current native observation may skip.
	struct FStateObservationBlock
	{
		int32 FirstIndex = 0;
		int32 NumEntries = 0;
		TSharedPtr<const uint8> Observation;
	};

	/** Streams the exact existing stable-key grammar, including malformed-value
	 * markers and original key ordering; not JSON/HJSON serialization. */
	void VisitStableCanonicalValueMaterial(const TSharedPtr<FJsonValue>& Value, TFunctionRef<void(const FString&)> Append);
	/** Same material, borrowing each fragment only for the synchronous callback. */
	void VisitStableCanonicalValueMaterialView(const TSharedPtr<FJsonValue>& Value, TFunctionRef<void(FStringView)> Append);
	/** Same revision grammar for the captured current value, without materializing JSON. */
	void VisitStableCanonicalValueMaterialView(const FStateEntry& Entry, TFunctionRef<void(FStringView)> Append);
	class UEAGENTINTERFACE_API FStateSnapshot
	{
	public:
		/** Borrowed ordered view; no full-entry copy or ancestor snapshot retention. */
		class FEntryView
		{
		public:
			explicit FEntryView(const FStateSnapshot& InSnapshot, bool bInAuthoringOnly = false)
				: Snapshot(&InSnapshot), bAuthoringOnly(bInAuthoringOnly) {}
			int32 Num() const { return Snapshot->Num(); }
			bool IsEmpty() const { return Num() == 0; }
			bool IsValidIndex(int32 Index) const { return Index >= 0 && Index < Num(); }
			const FStateEntry& operator[](int32 Index) const { return bAuthoringOnly ? Snapshot->ReadAuthoringEntry(Index) : Snapshot->ReadEntry(Index); }
			struct FIterator
			{
				const FStateSnapshot* Snapshot;
				int32 Index;
				bool bAuthoringOnly;
				const FStateEntry& operator*() const { return bAuthoringOnly ? Snapshot->ReadAuthoringEntry(Index) : Snapshot->ReadEntry(Index); }
				FIterator& operator++() { ++Index; return *this; }
				bool operator!=(const FIterator& Other) const { return Index != Other.Index; }
			};
			FIterator begin() const { return {Snapshot, 0, bAuthoringOnly}; }
			FIterator end() const { return {Snapshot, Num(), bAuthoringOnly}; }
		private:
			const FStateSnapshot* Snapshot;
			bool bAuthoringOnly;
		};
		int32 Num() const { return Storage->Entries.Num(); }
		bool IsEmpty() const { return Storage->Entries.IsEmpty(); }
		FEntryView ReadEntries() const { return FEntryView(*this); }
		const FStateEntry& ReadEntry(int32 Index) const;
		/** Authoring-only consumers never materialize a runtime-bound entry.
		 * Export records borrow pure declarations: read GetType/GetConstraints/GetCategoryPath.
		 * ReadEntry/GetEntries/CopyEntry retain complete owning DTO fields. */
		FEntryView ReadAuthoringEntries() const { return FEntryView(*this, true); }
		const FStateEntry& ReadAuthoringEntry(int32 Index) const { return *Storage->Entries[Index]; }
		const FStateNativeBinding& ReadNativeBinding(int32 Index) const;
		bool CopyEntry(int32 Index, FStateEntry& OutEntry) const;
		bool FindEntry(
			const FString& SemanticId,
			const FString& TargetCanonicalKey,
			FStateEntry& OutEntry) const;
		TArray<FStateEntry> CopyEntries() const;
		/** Immutable observation view; callers must not mutate referenced JSON. */
		const TArray<FStateEntry>& GetEntries() const;
		/** Exact same sealed payload and native bindings, not merely membership. */
		bool SharesStorageWith(const FStateSnapshot& Other) const { return Storage == Other.Storage; }
		/** Index into the sealed immutable observation view; exact identity lookup. */
		int32 FindEntryIndex(const FString& SemanticId, const FString& TargetCanonicalKey) const;
		const TSharedPtr<const uint8>& GetObservationMembership() const { return Storage->ObservationMembership; }
		bool HasOrderedAuthoringEntries() const { return Storage->bOrderedAuthoringEntries; }
		const TArray<FStateObservationBlock>& GetObservationBlocks() const { return Storage->ObservationBlocks; }
		/** Actual complete observations only. False means membership/identity changed;
		 * callers must use their structural path, never interpret it as no changes. */
		bool TryFindAuthoringChanges(const FStateSnapshot& After, TArray<int32>& OutIndices) const;
		/** Borrow selected immutable entries and their current native bindings. */
		bool TrySelectEntries(const TArray<int32>& Indices, FStateSnapshot& OutSnapshot) const;
		/** Replace already captured identities, retaining the complete baseline. */
		bool TryOverlay(const FStateSnapshot& ActualEntries, FStateSnapshot& OutSnapshot,
			FDiagnostic& OutError, bool bReuseIdenticalObservations = false) const;
		/** Also report baseline indices whose immutable observation actually changed. */
		bool TryOverlay(const FStateSnapshot& ActualEntries, FStateSnapshot& OutSnapshot,
			FDiagnostic& OutError, bool bReuseIdenticalObservations, TArray<int32>& OutChangedIndices) const;
		/** Release native property storage while retaining the canonical snapshot and metadata. */
		void ReleaseNativeValues();
		/** Share sealed authoring observations without retaining native storage,
		 * callbacks or current runtime identities. Not a native-read baseline. */
		FStateSnapshot MakeAuthoringOnlySnapshot() const;
		/** Select an already captured identity before retiring native provenance.
		 * Unchanged entries remain shared; no field is read or re-encoded. */
		FStateSnapshot MakeAuthoringObservation(TFunctionRef<const UeAgentAuthoringIdentity::FCanonicalIdentity*(
			const FStateEntry&, const FStateNativeBinding&)> SelectIdentity) const;
		/** Union already observed authoring entries by exact identity. No native
		 * bindings or new reads; choose first-before or last-after on overlap. */
		FStateSnapshot MergeAuthoringObservations(const FStateSnapshot& Additional, bool bPreferAdditional) const;
		/** Union already captured entries and their native bindings. The caller
		 * supplies the current selection; this does not make old values current. */
		FStateSnapshot MergeCapturedObservations(const FStateSnapshot& Additional, bool bPreferAdditional) const;
		/** Domain lifecycle may retain only storage whose field/type lifetime it owns. */
		void ReleaseNativeValuesIf(TFunctionRef<bool(const FStateEntry&)> ShouldRelease);
		void ReleaseNativeValuesIf(TFunctionRef<bool(const FStateEntry&, const FStateNativeBinding&)> ShouldRelease);
		TSharedRef<FJsonObject> ToJsonObject() const;

	private:
		FStateSnapshot MergeObservations(const FStateSnapshot& Additional, bool bPreferAdditional, bool bKeepNativeBindings) const;
		bool TryOverlayInPlace(const FStateSnapshot& ActualEntries, FDiagnostic& OutError,
			bool bReuseIdenticalObservations, TArray<int32>* OutChangedIndices);
		void SealObservationBlocks();
		class FNativeBindings
		{
			static constexpr int32 PageSize = 256;
			struct FPage { TOptional<FStateNativeBinding> Values[PageSize]; };
			TArray<TSharedPtr<FPage>> Pages;
			int32 Count = 0;
			FPage& WritePage(int32 Index) {
				const int32 Page = Index / PageSize;
				if (Page >= Pages.Num()) Pages.SetNum(Page + 1);
				if (!Pages[Page]) Pages[Page] = MakeShared<FPage>();
				else if (!Pages[Page].IsUnique()) Pages[Page] = MakeShared<FPage>(*Pages[Page]);
				return *Pages[Page];
			}
		public:
			const FStateNativeBinding* Find(int32 Index) const {
				const int32 Page = Index / PageSize;
				if (!Pages.IsValidIndex(Page) || !Pages[Page]) return nullptr;
				const auto& Value = Pages[Page]->Values[Index % PageSize];
				return Value.IsSet() ? &Value.GetValue() : nullptr;
			}
			FStateNativeBinding* FindMutable(int32 Index) {
				return Find(Index) ? &WritePage(Index).Values[Index % PageSize].GetValue() : nullptr;
			}
			bool Contains(int32 Index) const { return Find(Index) != nullptr; }
			int32 Num() const { return Count; }
			void Add(int32 Index, FStateNativeBinding Value) {
				if (!Contains(Index)) ++Count;
				WritePage(Index).Values[Index % PageSize] = MoveTemp(Value);
			}
			void Remove(int32 Index) {
				if (Contains(Index)) { WritePage(Index).Values[Index % PageSize].Reset(); --Count; }
			}
		};
		struct FStorage
		{
			FStorage() = default;
			FStorage(const FStorage& Other)
				: Entries(Other.Entries), NativeBindings(Other.NativeBindings), ObservationMembership(Other.ObservationMembership),
				  ObservationBlocks(Other.ObservationBlocks), bOrderedAuthoringEntries(Other.bOrderedAuthoringEntries) {}
			TArray<TSharedRef<const FStateEntry>> Entries;
			// Current result storage, shared in independent COW pages. It is not
			// an optional cache and never folds native data into authoring entries.
			FNativeBindings NativeBindings;
			mutable TMap<int32, TSharedRef<const FStateEntry>> BoundEntries;
			// Compatibility only. Hot readers consume ReadEntries; mutations drop
			// this view so it cannot keep obsolete native bindings alive.
			mutable TSharedPtr<const TArray<FStateEntry>> MaterializedEntries;
			mutable FRWLock MaterializationLock;
			TSharedPtr<const uint8> ObservationMembership;
			TArray<FStateObservationBlock> ObservationBlocks;
			bool bOrderedAuthoringEntries = false;
		};
		// Copies share sealed data. Every mutating operation detaches first;
		// releasing native storage in one snapshot must not modify another.
		TSharedRef<FStorage> Storage = MakeShared<FStorage>();
		void DetachStorage();
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
	/** Exact stable-key equality without materializing whole-tree key strings. */
	UEAGENTINTERFACE_API bool AreStableCanonicalValuesEqual(const TSharedPtr<FJsonValue>& Left, const TSharedPtr<FJsonValue>& Right);
	bool HaveSameStateEntryDeclaration(const FStateEntry& Left, const FStateEntry& Right);

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

	UEAGENTINTERFACE_API bool AppendEditableObjectPropertyReadRequests(
		const UObject* RootObject, const FEditablePropertyReadOptions& Options,
		TArray<FReadRequestBinding>& OutRequests, FDiagnostic& OutError);

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
	enum class ECaptureValueMode : uint8 { Authoring, Native, AuthoringExport, AuthoringExportLiterals };

	UEAGENTINTERFACE_API bool CaptureSnapshot(
		const TArray<FReadRequest>& Requests,
		FCaptureResult& OutResult,
		const UeAgentAuthoringSemanticRegistry::FRegistry* SemanticRegistry = nullptr,
		const FStateAccessorRegistry* AccessorRegistry = nullptr,
		ECaptureValueMode ValueMode = ECaptureValueMode::Authoring);
	UEAGENTINTERFACE_API bool CaptureSnapshot(
		const TArray<FReadRequestBinding>& Requests, FCaptureResult& OutResult,
		const UeAgentAuthoringSemanticRegistry::FRegistry* SemanticRegistry,
		const FStateAccessorRegistry* AccessorRegistry, ECaptureValueMode ValueMode);
	/** Same synchronous reader and final seal. A producer may finish its input
	 * validation after Consume returns false; no further native reads occur.
	 * Producer failure retains priority over a recorded accessor failure. */
	using FReadRequestConsumer = TFunctionRef<bool(const FReadRequest&)>;
	using FReadRequestProducer = TFunctionRef<bool(FReadRequestConsumer, FDiagnostic&)>;
	UEAGENTINTERFACE_API bool CaptureSnapshot(FReadRequestProducer Producer,
		FCaptureResult& OutResult, ECaptureValueMode ValueMode,
		double* OutReadSeconds = nullptr, int32* OutRequestCount = nullptr);
	/** Same reader/validation/seal. The producer transfers each request at its
	 * last consumer, so the sealed entry may take its logical identity/semantic.
	 * The producer may finish later input validation after a read failure. */
	using FOwnedReadRequestConsumer = TFunctionRef<bool(FReadRequest&&)>;
	using FOwnedReadRequestProducer = TFunctionRef<bool(FOwnedReadRequestConsumer, FDiagnostic&)>;
	UEAGENTINTERFACE_API bool CaptureSnapshotOwned(FOwnedReadRequestProducer Producer,
		FCaptureResult& OutResult, ECaptureValueMode ValueMode,
		double* OutReadSeconds = nullptr, int32* OutRequestCount = nullptr);

	/** Same capture contract, with an optional previously sealed authoring result.
	 * An accessor must observe and prove its current output before any reuse. */
	UEAGENTINTERFACE_API bool CaptureSnapshotWithBaseline(
		const TArray<FReadRequest>& Requests, const FStateSnapshot& Baseline,
		FCaptureResult& OutResult);
}

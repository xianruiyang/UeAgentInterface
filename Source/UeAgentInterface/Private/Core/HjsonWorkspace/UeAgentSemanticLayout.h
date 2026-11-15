#pragma once

#include "CoreMinimal.h"
#include "Core/ContextResource/UeAgentContextResource.h"
#include "Core/HjsonWorkspace/UeAgentHjsonWorkspace.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "Templates/SharedPointer.h"
#include "Templates/Function.h"

class FJsonObject;
struct FUeAgentStructuredHjsonObjectView;

namespace UE::UAI::HjsonWorkspace
{
	/** Borrowed captured collection naming fields, not a typed value model. */
	using FKeyedMemberNameVisitor = TFunctionRef<void(const FString& Alias, const FString& UnitId,
		const FString& ActorGuid, const FString& ManagedId, const FString& ComponentName)>;
	using FKeyedMemberNameLookup = TFunction<bool(const FString& UnitId, const FString& RootField,
		FKeyedMemberNameVisitor Visitor, FWorkspaceError& Error)>;
	struct FCommittedWorkspaceDelta;
	struct FSemanticReferenceNamespace;
	class FResolvedWorkspaceUnitBindings;
	using UE::UAI::ContextResource::FUeAgentResourceTracker;
	using UE::UAI::StateSemantic::FUeAgentCanonicalValueRef;

	enum class ESemanticUnitKind : uint8
	{
		WorkspaceIndex,
		TargetState,
		EntityState,
		PropertySet,
		StructureState,
		ItemState,
		RelationSet,
		OrderingState,
		LayoutState,
		CollectionState,
		DependencyState,
		ExternalPayloadState,
		OpaqueExtension,
	};

	enum class EWorkspaceSourceMode : uint8
	{
		Directory,
		SingleFile,
		Inline,
	};

	class UEAGENTINTERFACE_API FSemanticUnit final
	{
		struct FStorage;
		struct FDeclarationRecord;
	public:
		class FDeclarationProof;
		/** The unit's existing immutable declaration, without its value storage.
		 * Directory/name/edge consumers may keep this view after values retire. */
		class FDeclarationView
		{
		public:
			const FString& GetUnitId() const;
			const TSharedRef<const FString, ESPMode::ThreadSafe>& GetSharedUnitId() const;
			ESemanticUnitKind GetUnitKind() const;
			const FString& GetOwnerRef() const;
			const FString& GetSemanticTypeRef() const;
			const FString& GetRole() const;
			const FString& GetFieldSetRef() const;
			const TArray<FString>& GetItemRefs() const;
			const TArray<FString>& GetRelationRefs() const;
			const FString& GetOrderingRef() const;
			const FString& GetLayoutRef() const;
			const TArray<FString>& GetDependencyRefs() const;
			const TArray<FString>& GetProfileVisibility() const;
			const FString& GetSourceProvenance() const;
			const FString& GetTargetIdentityKey() const;
			const TArray<FString>& GetCategoryPath() const;
			int32 GetValueCount() const { return ValueCount; }
			bool HasGraphBody() const { return bGraphBody; }
			bool HasOwnTypeDescriptor() const { return bOwnTypeDescriptor; }
			bool HasLayoutNameFields() const { return bLayoutNameFields; }
			bool HasDirectAssetPathField() const { return bDirectAssetPathField; }
			bool HasKeyedRoot(const FString& Root) const;
			FDeclarationProof GetDeclarationProof() const;
		private:
			FDeclarationView(TSharedRef<const FDeclarationRecord, ESPMode::ThreadSafe> InDeclaration, bool bInValidated,
				int32 InValueCount, bool bInGraphBody, uint64 InKeyedRoots, bool bInOwnTypeDescriptor, bool bInLayoutNameFields, bool bInDirectAssetPathField)
				: Declaration(MoveTemp(InDeclaration)), bValidated(bInValidated), ValueCount(InValueCount),
				bGraphBody(bInGraphBody), KeyedRoots(InKeyedRoots), bOwnTypeDescriptor(bInOwnTypeDescriptor), bLayoutNameFields(bInLayoutNameFields),
				bDirectAssetPathField(bInDirectAssetPathField) {}
			TSharedRef<const FDeclarationRecord, ESPMode::ThreadSafe> Declaration;
			bool bValidated;
			int32 ValueCount;
			bool bGraphBody;
			uint64 KeyedRoots;
			bool bOwnTypeDescriptor;
			bool bLayoutNameFields;
			bool bDirectAssetPathField;
			friend class FSemanticUnit;
		};
		FDeclarationView GetDeclaration() const;
		/** Metadata reader only: supply the captured value's shallow shape, not
		 * fabricated values. The declaration still comes from the normal validator. */
		FDeclarationView GetDeclarationForCapturedShape(const TArray<FString>& ValueKeys, bool bOwnTypeDescriptor) const;
		static FSemanticUnit FromDeclaration(FDeclarationView Declaration, TMap<FString, FUeAgentCanonicalValueRef> Values);
		// A proof may remember identity without retaining the unit's object graph.
		using FStorageIdentity = TWeakPtr<const FStorage, ESPMode::ThreadSafe>;
		class FDeclarationProof
		{
		public:
			FDeclarationProof() = default;
		private:
			TWeakPtr<const void, ESPMode::ThreadSafe> Identity;
			friend class FSemanticUnit;
			friend class FDeclarationView;
		};
		FDeclarationProof GetDeclarationProof() const;
		bool MatchesCompiledDeclaration(const FDeclarationProof& Proof) const;
		FStorageIdentity GetStorageIdentity() const { return Storage; }
		bool SharesStorageWith(const FSemanticUnit& Other) const { return &Storage.Get() == &Other.Storage.Get(); }
		bool SharesStorageIdentity(const FStorageIdentity& Identity) const
		{
			return Identity.Pin().Get() == &Storage.Get();
		}
		FSemanticUnit(
			FString InUnitId,
			ESemanticUnitKind InUnitKind,
			FString InOwnerRef,
			FString InSemanticTypeRef,
			FString InRole,
			FString InFieldSetRef,
			TMap<FString, FUeAgentCanonicalValueRef> InValues,
			TArray<FString> InItemRefs,
			TArray<FString> InRelationRefs,
			FString InOrderingRef,
			FString InLayoutRef,
			TArray<FString> InDependencyRefs,
			TArray<FString> InProfileVisibility,
			FString InSourceProvenance,
			FString InTargetIdentityKey = FString(),
			TArray<FString> InCategoryPath = {});
		/** Share the exact inventory identity backing; normalization and semantic
		 * validation remain owned by the unit constructor. */
		FSemanticUnit(
			TSharedRef<const FString, ESPMode::ThreadSafe> InUnitId,
			ESemanticUnitKind InUnitKind,
			FString InOwnerRef, FString InSemanticTypeRef, FString InRole, FString InFieldSetRef,
			TMap<FString, FUeAgentCanonicalValueRef> InValues,
			TArray<FString> InItemRefs, TArray<FString> InRelationRefs,
			FString InOrderingRef, FString InLayoutRef,
			TArray<FString> InDependencyRefs, TArray<FString> InProfileVisibility,
			FString InSourceProvenance, FString InTargetIdentityKey = FString(),
			TArray<FString> InCategoryPath = {});

		/** The producer already has one value/literal/selected field. No value map,
		 * alternate identity or retained producer is required by that fact. */
		struct FSinglePropertyValue {
			enum class EField : uint8 { Value, Literal, Selected };
			static const FString& GetFieldName(EField Field);
			FUeAgentCanonicalValueRef Value;
			EField Field = EField::Value;
		};
		FSemanticUnit(FString InUnitId, ESemanticUnitKind InUnitKind, FString InOwnerRef,
			FString InSemanticTypeRef, FString InRole, FString InFieldSetRef, FSinglePropertyValue InValue,
			TArray<FString> InItemRefs, TArray<FString> InRelationRefs, FString InOrderingRef, FString InLayoutRef,
			TArray<FString> InDependencyRefs, TArray<FString> InProfileVisibility, FString InSourceProvenance,
			FString InTargetIdentityKey = FString(), TArray<FString> InCategoryPath = {});

		const FString& GetUnitId() const;
		const TSharedRef<const FString, ESPMode::ThreadSafe>& GetSharedUnitId() const;
		ESemanticUnitKind GetUnitKind() const;
		const FString& GetOwnerRef() const;
		const FString& GetSemanticTypeRef() const;
		const FString& GetRole() const;
		const FString& GetFieldSetRef() const;
		const TMap<FString, FUeAgentCanonicalValueRef>& GetValues() const;
		/** Field consumers do not request the concrete legacy TMap above. Returned
		 * references belong to this unit's immutable storage, not a temporary. */
		const FUeAgentCanonicalValueRef* FindValue(const FString& Key, const FString** MatchedKey = nullptr) const;
		const FUeAgentCanonicalValueRef* FindValue(const TCHAR* Key, const FString** MatchedKey = nullptr) const;
		bool HasValue(const FString& Key) const { return FindValue(Key) != nullptr; }
		bool HasValue(const TCHAR* Key) const { return FindValue(Key) != nullptr; }
		int32 GetValueCount() const;
		bool VisitValues(TFunctionRef<bool(const FString&, const FUeAgentCanonicalValueRef&)> Visitor) const;
		bool HasInlinePropertyValue() const;
		bool HasExpandedInlineValueMap() const;
		FSemanticUnit WithValues(TMap<FString, FUeAgentCanonicalValueRef> NewValues) const;
		const TArray<FString>& GetItemRefs() const;
		const TArray<FString>& GetRelationRefs() const;
		const FString& GetOrderingRef() const;
		const FString& GetLayoutRef() const;
		const TArray<FString>& GetDependencyRefs() const;
		const TArray<FString>& GetProfileVisibility() const;
		const FString& GetSourceProvenance() const;
		const FString& GetTargetIdentityKey() const;
		const TArray<FString>& GetCategoryPath() const;
		/** The formatter's exact context-free semantic row SHA, when produced.
		 * This is neither edit_hash nor a layout/source/native proof. */
		const FString& GetPublicRowHash() const;
		/** Private allocation; shared declaration, canonical values, strings and
		 * containers are accounted through the corresponding visitors. */
		int64 GetStorageAllocatedSize() const;
		void VisitSharedStorageStrings(TFunctionRef<void(const FString&)> Visit) const;
		void VisitSharedStorageContainers(TFunctionRef<void(const void*, int64)> Visit) const;
		/** The admission owner may reject an already-accounted immutable backing
		 * before its allocation sizes are traversed. The legacy visitor visits all. */
		void VisitSharedStorageContainers(TFunctionRef<bool(const void*)> ShouldVisit,
			TFunctionRef<void(const void*, int64)> Visit) const;
		bool IsValid() const;
		const Contract::FDiagnostic& GetValidationDiagnostic() const;

	private:
		friend struct FSemanticUnitContentAccess;
		explicit FSemanticUnit(TSharedRef<const FStorage, ESPMode::ThreadSafe> InStorage) : Storage(MoveTemp(InStorage)) {}
		// Construction owns and validates the input once. Copies share only this
		// immutable storage; WithValues validates new values and shares the proven
		// declaration, without retaining a parent value/snapshot generation.
		TSharedRef<const FStorage, ESPMode::ThreadSafe> Storage;
	};

	struct FCompiledAuthoringStructure;
	/** Normalize only the declared property-definition schema, never raw source
	 * hashes. Called after source admission and at model/durable decode ingress. */
	UEAGENTINTERFACE_API bool TryNormalizeAuthoringUnit(
		FSemanticUnit& Unit, FUeAgentResourceTracker& Resources, FWorkspaceError& OutError);
	class FAuthoringModel;
	class FSemanticProjectionBindings;
	class FAcceptedWorkspaceInventory;
	int32 FindCompiledUnitIndex(const FCompiledAuthoringStructure& Structure, const FString& UnitId);
	using FAuthoringModelPtr = TSharedPtr<const FAuthoringModel>;
	using FAuthoringModelRef = TSharedRef<const FAuthoringModel>;

	// Naming candidates only. The current compiled definition must prove the
	// complete semantic content before a previous projection name is retained.
	struct FDefinitionProjectionBinding
	{
		FString ContentKey;
		FString DefinitionName;
		FString ReadableAlias;
		FString PublicRef;
	};
	struct FDefinitionProjectionHints
	{
		FString ContractId;
		int32 ContractVersion = 0;
		FString ProfileId;
		TArray<FString> TargetUnitIds;
		TMap<FString, FDefinitionProjectionBinding> Definitions;
	};

	/** Only a successful compiler model can populate this proof. It owns no
	 * unit values, source trees, native objects or compiled index graph. */
	class UEAGENTINTERFACE_API FCompiledAuthoringReuseProof final
	{
	public:
		FCompiledAuthoringReuseProof() = default;
		int64 GetAllocatedSize() const;
	private:
		TWeakPtr<const FCompiledAuthoringStructure> Structure;
		TArray<FSemanticUnit::FDeclarationProof> Declarations;
		TArray<FString> TargetUnitIds;
		friend class FAuthoringModel;
		friend bool TryCompileAuthoringModelRaw(
			FString, int32, FString, FString, TArray<FSemanticUnit>, TArray<FString>,
			FUeAgentResourceTracker&, FAuthoringModelPtr&, FWorkspaceError&,
			const FDefinitionProjectionHints*, const FAuthoringModel*, const FCompiledAuthoringReuseProof*, bool);
	};
	/** Recover naming candidates only, using this layout owner's allocation
	 * grammar. Current model compilation still validates content and conflicts. */
	bool TryReadPersistedDefinitionProjectionBinding(const FSemanticUnit& Unit,
		const FString& UnitPointer, const FString& PublicRef, FDefinitionProjectionBinding& OutBinding);
	/** Original allocator identity test, including the business-name contribution. */
	bool HaveSameSemanticProjectionIdentity(const FSemanticUnit& Unit, const FSemanticUnit& Before);

	class UEAGENTINTERFACE_API FAuthoringModel final
	{
	public:
		const FString& GetContractId() const;
		int32 GetContractVersion() const;
		const FString& GetProfileId() const;
		const FString& GetBaseRevision() const;
		const TArray<FSemanticUnit>& GetUnits() const;
		/** Pure identity/index owner; retains no unit values or native objects. */
		TSharedRef<const FCompiledAuthoringStructure> GetSharedUnitIndex() const;
		/** Consume current rows without materializing a complete source overlay. */
		bool VisitUnits(TFunctionRef<bool(const FSemanticUnit&)> Visitor) const;
		/** Enumerate the same unit directory without requesting value rows. */
		bool VisitDeclarations(TFunctionRef<bool(const FSemanticUnit::FDeclarationView&, int32)> Visitor) const;
		const TArray<FSemanticUnit::FDeclarationView>& GetDeclarations() const;
		TSharedRef<const TArray<FSemanticUnit::FDeclarationView>> GetSharedDeclarations() const;
		FString GetRowHashAt(int32 Index) const;
		bool CheckValueReadResult(FWorkspaceError& OutError) const;
		TOptional<FSemanticUnit::FDeclarationView> FindDeclaration(const FString& UnitId) const;
		int32 GetUnitCount() const;
		int32 FindUnitIndex(const FString& UnitId) const;
		const TArray<FString>& GetTargetUnitIds() const;
		const FSemanticUnit* FindUnit(const FString& UnitId) const;
		TArray<const FSemanticUnit*> FindUnitsForTargetIdentity(const FString& TargetIdentity) const;
		TArray<const FSemanticUnit*> FindUnitsForSemanticType(const FString& SemanticType) const;
		const FString* FindReadablePropertyDefinitionName(const FString& UnitId) const;
		const TMap<FString, FDefinitionProjectionBinding>& GetPreferredDefinitionBindings() const;
		const FString& GetSemanticFingerprint() const;
		const TSet<FString>& GetSemanticTypePrefixes() const;
		TSharedRef<const FCompiledAuthoringReuseProof> CreateCompilationReuseProof() const;
		bool IsCompiled() const { return bCompiled; }
		/** Indexed, admitted source rows for value-only imports. This is not a
		 * compiled graph, export fingerprint or compilation reuse proof. */
		static FAuthoringModelRef MakePropertySourceView(FString ContractId, int32 ContractVersion,
			FString ProfileId, FString BaseRevision, TArray<FSemanticUnit> Units,
			TArray<FString> TargetUnitIds, FString SourceStamp);
		static FAuthoringModelRef MakePropertySourceView(FAuthoringModelRef PublishedRows,
			FString BaseRevision, TMap<FString, FSemanticUnit> Replacements, FString SourceStamp);
		static FAuthoringModelRef RebasePropertySourceView(FAuthoringModelRef Source,
			FString BaseRevision, TMap<FString, FSemanticUnit> Replacements);
		/** Published directory owns declarations/index/names, never the producer
		 * Model or its values. Required values come from the original row reader. */
		static FAuthoringModelRef MakePublicationDirectory(const FAuthoringModel& Producer,
			TSharedRef<const FResolvedWorkspaceUnitBindings> Values,
			TSharedPtr<const TArray<FSemanticUnit::FDeclarationView>> CapturedDeclarations = {});
		/** Request-local view of a destination publication plus its folded actual
		 * changes. Only declaration membership and changed value handles are
		 * built; this is not a complete-model compilation or a retained chain. */
		static bool TryMakeCommittedPublicationView(FAuthoringModelRef Published,
			const FCommittedWorkspaceDelta& Changes, FAuthoringModelPtr& OutModel, FWorkspaceError& OutError);
		/** Formatter enumeration of the addressed rows and their naming context.
		 * Lookups remain backed by the same actual/publication facts. This view is
		 * not a complete workspace and must not supply a publication's total count. */
		static bool TryMakeOutputScope(FAuthoringModelRef Context, const TArray<FString>& UnitIds,
			FAuthoringModelPtr& OutModel, FWorkspaceError& OutError);
		bool IsOutputScope() const { return bScopedOutputEnumeration; }
		/** The real definition catalog, without visiting every property row. */
		bool VisitDefinitionDeclarations(TFunctionRef<bool(const FSemanticUnit::FDeclarationView&)> Visitor) const;
		/** Rebind only workspace names; compiled declarations, values and digest
		 * remain identical. Eligible hints still require the exact canonical row. */
		bool TryRebindDefinitionProjection(const FDefinitionProjectionHints* ProjectionHints,
			FAuthoringModelPtr& OutModel, FWorkspaceError& OutError) const;

	private:
		FAuthoringModel(
			FString InContractId,
			int32 InContractVersion,
			FString InProfileId,
			FString InBaseRevision,
			TArray<FSemanticUnit> InUnits,
			TArray<FString> InTargetUnitIds,
			FString InSemanticFingerprint,
			TSharedRef<const FCompiledAuthoringStructure> InStructure,
			const FDefinitionProjectionHints* ProjectionHints, bool bInCompiled = true);
		void InitializeDefinitionProjection(const FDefinitionProjectionHints* ProjectionHints,
			const TMap<FString, FDefinitionProjectionBinding>* AcceptedBindings = nullptr);
		FString ReadRowHashByIdentity(const FString& UnitId) const;

		FString ContractId;
		int32 ContractVersion = 0;
		FString ProfileId;
		FString BaseRevision;
		mutable TArray<FSemanticUnit> Units;
		// A publication directory can outlive value rows without retaining a
		// previous Model. Full capture models continue to use their own Units.
		mutable TSharedPtr<const TArray<FSemanticUnit::FDeclarationView>> DeclarationDirectory;
		TSharedPtr<const FResolvedWorkspaceUnitBindings> PublicationValues;
		TSharedPtr<const FAuthoringModel> SourceRows;
		TMap<FString, FSemanticUnit> RebasedValues;
		// A request-local membership overlay on the destination publication.
		// Full arrays/indexes are built only by their explicit complete consumer.
		TOptional<int32> DeltaUnitCount;
		TArray<FString> AddedUnitIds;
		TSet<FString> RemovedUnitIds;
		bool bScopedOutputEnumeration = false;
		TArray<FString> TargetUnitIds;
		mutable TSharedRef<const FCompiledAuthoringStructure> Structure;
		mutable bool bPublicationIndexMaterialized = false;
		TMap<FString, FString> ReadablePropertyDefinitionNames;
		TMap<FString, FDefinitionProjectionBinding> PreferredDefinitionBindings;
		mutable TMap<FString, TSharedPtr<const FDefinitionProjectionBinding>> PublishedDefinitionBindings;
		mutable TSharedPtr<const FDefinitionProjectionHints> PublishedDefinitionHints;
		FString DefinitionProjectionCollision;
		FString SemanticFingerprint;
		bool bCompiled = true;

		friend UEAGENTINTERFACE_API bool TryCompileAuthoringModel(
			FString,
			int32,
			FString,
			FString,
			TArray<FSemanticUnit>,
			TArray<FString>,
			FUeAgentResourceTracker&,
			FAuthoringModelPtr&,
			FWorkspaceError&);
		friend UEAGENTINTERFACE_API bool TryCompileAuthoringModel(
			FString, int32, FString, FString, TArray<FSemanticUnit>, TArray<FString>,
			FUeAgentResourceTracker&, FAuthoringModelPtr&, FWorkspaceError&,
			const FDefinitionProjectionHints*);
		friend bool TryCompileAuthoringModelRaw(
			FString, int32, FString, FString, TArray<FSemanticUnit>, TArray<FString>,
			FUeAgentResourceTracker&, FAuthoringModelPtr&, FWorkspaceError&,
			const FDefinitionProjectionHints*, const FAuthoringModel*, const FCompiledAuthoringReuseProof*, bool);
	};

	struct UEAGENTINTERFACE_API FWorkspaceUnitInventoryEntry
	{
		const FString& UnitId;
		// The digest reader may expand a compact hash by value. This row owns
		// that result through the synchronous inventory visitor.
		FString ContentHash;
	};

	/** Read the actual immutable unit/hash owner; no duplicate identity/hash
	 * array and no per-row wrapper allocation. */
	class FWorkspaceUnitInventoryView
	{
	public:
		FWorkspaceUnitInventoryView() = default;
		explicit FWorkspaceUnitInventoryView(FAuthoringModelRef InModel) : Model(MoveTemp(InModel)) {}
		int32 Num() const { return Model ? Model->GetUnitCount() : 0; }
		bool IsValidIndex(int32 Index) const { return Index >= 0 && Index < Num(); }
		FWorkspaceUnitInventoryEntry operator[](int32 Index) const {
			const auto& Unit = Model->GetDeclarations()[Index];
			return {Unit.GetUnitId(), Model->GetRowHashAt(Index)};
		}
	private:
		FAuthoringModelPtr Model;
	};

	/** The document is the immutable allocation owner for its placement rows.
	 * Identities use the semantic unit's existing shared declaration storage. */
	struct FWorkspacePlacementRow
	{
		TSharedRef<const FString, ESPMode::ThreadSafe> UnitId;
		FString LogicalPath;
		FString PhysicalOverride;
		bool bPhysicalOverride = false;
		FString PublicRef;
	};
	struct FWorkspacePlacementDocument
	{
		TSharedRef<const FString> PhysicalPath;
		TArray<FWorkspacePlacementRow> Rows;
		explicit FWorkspacePlacementDocument(TSharedRef<const FString> InPath) : PhysicalPath(MoveTemp(InPath)) {}
		const FString& PhysicalAt(int32 Index) const { return Rows[Index].bPhysicalOverride ? Rows[Index].PhysicalOverride : PhysicalPath.Get(); }
	};
	class UEAGENTINTERFACE_API FWorkspaceFileMembers
	{
	public:
		FWorkspaceFileMembers() = default;
		explicit FWorkspaceFileMembers(TSharedRef<const FWorkspacePlacementDocument> InDocument) : Document(MoveTemp(InDocument)) {}
		int32 Num() const { return Document ? Document->Rows.Num() : 0; }
		bool IsEmpty() const { return Num() == 0; }
		const FString& operator[](int32 Index) const { return Document->Rows[Index].UnitId.Get(); }
		bool Contains(const FString& Id) const { for (const FString& Value : *this) if (Value == Id) return true; return false; }
		bool operator==(const FWorkspaceFileMembers& Other) const;
		const TSharedPtr<const FWorkspacePlacementDocument>& GetDocument() const { return Document; }
		struct FConstIterator
		{
			const FWorkspaceFileMembers* Owner;
			int32 Index;
			const FString& operator*() const { return (*Owner)[Index]; }
			FConstIterator& operator++() { ++Index; return *this; }
			bool operator!=(const FConstIterator& Other) const { return Owner != Other.Owner || Index != Other.Index; }
		};
		FConstIterator begin() const { return {this, 0}; }
		FConstIterator end() const { return {this, Num()}; }
	private:
		TSharedPtr<const FWorkspacePlacementDocument> Document;
	};
	using FWorkspacePhysicalFiles = TMap<FString, FWorkspaceFileMembers>;
	struct FWorkspacePlacementStorage
	{
		// Borrowed string keys remain used by request-local path sets. They no
		// longer back a second complete unit identity index.
		struct FKey
		{
			const FString* Id;
			friend uint32 GetTypeHash(const FKey& Key) { return GetTypeHash(*Key.Id); }
			bool operator==(const FKey& Other) const { return *Id == *Other.Id; }
		};
		struct FLocation { const FWorkspacePlacementDocument* Document; int32 Index; };
		// Reuse the compiler's actual identity index, not a second complete map.
		// File members own the rows; this index owner has no model/value graph.
		FWorkspacePhysicalFiles Files;
		TSharedPtr<const FCompiledAuthoringStructure> UnitIndex;
		TArray<FLocation> Locations;
		// Names referenced by a scoped formatter, not members/placements. The
		// same immutable owner retains them until the reference writer finishes.
		TMap<FString, FString> ReferencedPublicRefs;
		const FLocation* Find(const FString& Id) const;
	};
	/** A read view, with no second map or per-unit allocation. */
	class UEAGENTINTERFACE_API FWorkspacePlacementView
	{
	public:
		enum class EColumn { Logical, Physical, PublicRef };
		FWorkspacePlacementView() : Storage(MakeShared<FWorkspacePlacementStorage>()), Column(EColumn::PublicRef) {}
		FWorkspacePlacementView(TSharedRef<const FWorkspacePlacementStorage> InStorage, bool bInPhysical)
			: Storage(MoveTemp(InStorage)), Column(bInPhysical ? EColumn::Physical : EColumn::Logical) {}
		FWorkspacePlacementView(TSharedRef<const FWorkspacePlacementStorage> InStorage, EColumn InColumn)
			: Storage(MoveTemp(InStorage)), Column(InColumn) {}
		int32 Num() const { return Storage->Locations.Num(); }
		bool IsEmpty() const { return Num() == 0; }
		const FString* Find(const FString& Id) const;
		const FString* Find(const FSemanticUnit& Unit) const { return Find(Unit.GetUnitId()); }
		/** Resolve an emitted reference without treating its owner as a member.
		 * Num/iteration continue to describe actual placement rows only. */
		const FString* FindReference(const FString& Id) const;
		const FString& FindChecked(const FString& Id) const { const auto* Value = Find(Id); check(Value); return *Value; }
		const FString& FindChecked(const FSemanticUnit& Unit) const { return FindChecked(Unit.GetUnitId()); }
		FString FindRef(const FString& Id) const { const auto* Value = Find(Id); return Value ? *Value : FString(); }
		bool Contains(const FString& Id) const { return Find(Id) != nullptr; }
		const FString* FindKey(const FString& Value) const { for (const auto& Pair : *this) if (Pair.Value == Value) return &Pair.Key; return nullptr; }
		void GetKeys(TArray<FString>& Out) const { Out.Reset(Num()); for (const auto& Pair : *this) Out.Add(Pair.Key); }
		bool OrderIndependentCompareEqual(const FWorkspacePlacementView& Other) const;
		struct FReadPair { const FString& Key; const FString& Value; };
		class FConstIterator
		{
		public:
			using FIndexIterator = FWorkspacePhysicalFiles::TConstIterator;
			FConstIterator(const FWorkspacePlacementView& InOwner, bool bEnd);
			FReadPair operator*() const;
			FConstIterator& operator++();
			bool operator!=(const FConstIterator& Other) const;
		private:
			const FWorkspacePlacementView* Owner;
			FIndexIterator Inner;
			int32 RowIndex = 0;
			bool bEnd;
			void Seek();
		};
		FConstIterator begin() const { return FConstIterator(*this, false); }
		FConstIterator end() const { return FConstIterator(*this, true); }
		const TSharedRef<const FWorkspacePlacementStorage>& GetStorage() const { return Storage; }
	private:
		const FString& ValueAt(const FWorkspacePlacementStorage::FLocation& Row) const;
		TSharedRef<const FWorkspacePlacementStorage> Storage;
		EColumn Column;
	};

	struct UEAGENTINTERFACE_API FMachineWorkspaceManifest
	{
		FString WorkspaceSchema;
		FString ContractId;
		int32 ContractVersion = 0;
		int32 LayoutVersion = 0;
		FString ProfileId;
		FString SemanticFingerprint;
		FWorkspaceUnitInventoryView UnitInventory;
		// Internal identity -> readable ref, owned by the same immutable row as placement.
		FWorkspacePlacementView PublicRefMapping;
		FString BaseRevision;
	};

	class FWorkspaceLayoutPlan;
	using FWorkspaceLayoutPlanPtr =
		TSharedPtr<const FWorkspaceLayoutPlan>;

	class UEAGENTINTERFACE_API FWorkspaceLayoutPlan final
	{
	public:
		const FAuthoringModelRef& GetModel() const;
		int32 GetLayoutVersion() const;
		EWorkspaceSourceMode GetSourceMode() const;
		const FString& GetSemanticFingerprint() const;
		const FWorkspacePlacementView& GetLogicalPlacements() const;
		const FWorkspacePlacementView& GetPhysicalPlacements() const;
		const FWorkspacePhysicalFiles& GetPhysicalFiles() const;
		const FMachineWorkspaceManifest& GetMachineManifest() const;
		const TMap<FString, int64>& GetTelemetry() const;
		const FDefinitionProjectionHints& GetDefinitionProjectionHints() const;
		TWeakPtr<const uint8> GetProjectionIdentity() const { return Projection->Identity; }
		// Immutable projection backing, independent of the model and its values.
		// Consumers may retain it without copying all placement/member maps.
		struct FProjection
		{
			FProjection(TSharedRef<const FWorkspacePlacementStorage> InPlacements,
				FDefinitionProjectionHints InHints, TSharedPtr<const FSemanticReferenceNamespace> InNamespace);
			TSharedRef<const FWorkspacePlacementStorage> Placements;
			FWorkspacePlacementView LogicalPlacements;
			FWorkspacePlacementView PhysicalPlacements;
			const FWorkspacePhysicalFiles& PhysicalFiles;
			FDefinitionProjectionHints DefinitionHints;
			TSharedPtr<const FSemanticReferenceNamespace> ReferenceNamespace;
			TSharedPtr<const uint8> Identity = MakeShared<const uint8>(0);
		};
		TSharedRef<const FProjection> GetSharedProjection() const { return Projection; }
		/** Transfer the same completed projection to a value-free publication
		 * directory. This retires the request's native/Source value Model. */
		TSharedRef<const FWorkspaceLayoutPlan> WithPublicationDirectory(FAuthoringModelRef Directory) const;
		void RetireOptionalRetention() const;
		static bool TryRestorePublicationDirectory(FAuthoringModelRef Published,
			const FAcceptedWorkspaceInventory& Inventory, const FDefinitionProjectionHints* Hints,
			TSharedPtr<const FWorkspaceLayoutPlan>& OutLayout, FWorkspaceError& OutError);

	private:
		FWorkspaceLayoutPlan(
			FAuthoringModelRef InModel,
			int32 InLayoutVersion,
			EWorkspaceSourceMode InSourceMode,
			TSharedRef<const FWorkspacePlacementStorage> InPlacements,
			FMachineWorkspaceManifest InMachineManifest,
			TMap<FString, int64> InTelemetry,
			FDefinitionProjectionHints InDefinitionProjectionHints,
			TSharedPtr<const FSemanticReferenceNamespace> InReferenceNamespace);
		FWorkspaceLayoutPlan(FAuthoringModelRef InModel, FMachineWorkspaceManifest InManifest,
			TMap<FString, int64> InTelemetry, const FWorkspaceLayoutPlan& ProvenProjection);

		FAuthoringModelRef Model;
		int32 LayoutVersion = 0;
		EWorkspaceSourceMode SourceMode =
			EWorkspaceSourceMode::Directory;
		TSharedRef<const FProjection> Projection;
		FMachineWorkspaceManifest MachineManifest;
		TMap<FString, int64> Telemetry;

		friend UEAGENTINTERFACE_API bool TryPlanWorkspaceLayout(
			FAuthoringModelRef,
			int32,
			EWorkspaceSourceMode,
			TMap<FString, int64>,
			FUeAgentResourceTracker&,
			FWorkspaceLayoutPlanPtr&,
			FWorkspaceError&, const FSemanticProjectionBindings*, const FCommittedWorkspaceDelta*, const FWorkspaceLayoutPlan*);
	};

	UEAGENTINTERFACE_API FString GetSemanticWorkspaceSchemaId();

	UEAGENTINTERFACE_API FString LexToString(
		ESemanticUnitKind UnitKind);

	// Format a business name; embedded hex runs and non-ASCII letters are valid.
	UEAGENTINTERFACE_API FString MakeReadableSemanticAlias(
		const FString& Value);

	// Address hint from the original placement producer. A reader still checks
	// the actual inventory UnitId; this suffix is not an identity proof.
	FString GetStableProjectionIdentitySuffix(const FString& UnitId);

	// For fallback identity/source fields only, not authoritative business names.
	UEAGENTINTERFACE_API bool IsOpaqueSemanticIdentity(const FString& Value);

	// File navigation follows business ownership, independently of public reference handles.
	bool TryBuildAuthoringContentDirectories(const FAuthoringModel& Model,
		const FWorkspacePlacementView& Placements, TMap<FString, FString>& OutDirectories, FWorkspaceError& OutError,
		const FWorkspacePlacementView* ProvenPlacements = nullptr, const TMap<FString, FString>* ReadableAliases = nullptr,
		bool bOnlyUnprovenUnits = false);

	UEAGENTINTERFACE_API bool TryHashSemanticUnitContent(
		const FSemanticUnit& Unit, FString& OutHash, FWorkspaceError& OutError);
	/** Exact strings/keys and container membership; only number/bool leaves may differ. */
	bool HaveOnlyNumericOrBooleanValueChanges(const StateSemantic::FUeAgentCanonicalValue& A,
		const StateSemantic::FUeAgentCanonicalValue& B);

	UEAGENTINTERFACE_API bool TrySerializeSemanticHjsonObject(
		const UE::UAI::StateSemantic::FUeAgentCanonicalValue& Object,
		TArray<uint8>& OutCanonicalUtf8, FWorkspaceError& OutError,
		const FString& SourceUnit = FString());
	/** Same serializer, borrowing fields until this synchronous call returns. */
	bool TrySerializeSemanticHjsonObject(const FUeAgentStructuredHjsonObjectView& Object,
		TArray<uint8>& OutCanonicalUtf8, FWorkspaceError& OutError, const FString& SourceUnit);
	/** Generated machine-row positions from the same formatter call. */
	bool TrySerializeSemanticHjsonObject(
		const UE::UAI::StateSemantic::FUeAgentCanonicalValue& Object,
		TArray<uint8>& OutCanonicalUtf8, FWorkspaceError& OutError, const FString& SourceUnit,
		const FString& RowArrayField, TMap<FString, FIntPoint>& OutByteSpans);

	UEAGENTINTERFACE_API bool TrySerializeSemanticHjsonObject(
		const TSharedRef<FJsonObject>& Object,
		TArray<uint8>& OutCanonicalUtf8, FWorkspaceError& OutError,
		const FString& SourceUnit = FString());

	UEAGENTINTERFACE_API bool TrySerializeSemanticUnitRow(
		const FSemanticUnit& Unit,
		TArray<uint8>& OutCanonicalUtf8, FWorkspaceError& OutError,
		const FString& SourceFile = FString());
	/** Hash of the canonical row used by directory workspace inventory. */
	UEAGENTINTERFACE_API bool TryHashSemanticUnitRow(
		const FSemanticUnit& Unit, FString& OutHash, FWorkspaceError& OutError,
		const FString& SourceFile = FString());
	/** Prepare the same owned row digest, without copying its spelling to an
	 * intermediate string. Read it from Unit only after this call succeeds. */
	UEAGENTINTERFACE_API bool TryPrepareSemanticUnitRowHash(
		const FSemanticUnit& Unit, FWorkspaceError& OutError,
		const FString& SourceFile = FString());

	/** Builds a fresh editable row using the same projection as serialization. */
	UEAGENTINTERFACE_API bool TryBuildSemanticModelUnitRow(
		const FSemanticUnit& Unit, TSharedPtr<FJsonObject>& OutRow,
		FWorkspaceError& OutError,
		const TMap<FString, FString>* PublicRefs = nullptr,
		const FString& SourceFile = FString());
	UEAGENTINTERFACE_API bool TryBuildSemanticModelUnitRow(
		const FSemanticUnit& Unit, TSharedPtr<FJsonObject>& OutRow,
		FWorkspaceError& OutError, const FWorkspacePlacementView& PublicRefs,
		const FString& SourceFile = FString());

	UEAGENTINTERFACE_API bool TrySerializeSemanticModelUnitRow(
		const FSemanticUnit& Unit,
		TArray<uint8>& OutCanonicalUtf8, FWorkspaceError& OutError,
		const FString& SourceFile = FString());

	UEAGENTINTERFACE_API bool TrySerializeSemanticModelUnitRow(
		const FSemanticUnit& Unit,
		const TMap<FString, FString>& PublicRefs,
		TArray<uint8>& OutCanonicalUtf8, FWorkspaceError& OutError,
		const FString& SourceFile = FString());
	UEAGENTINTERFACE_API bool TrySerializeSemanticModelUnitRow(
		const FSemanticUnit& Unit, const FWorkspacePlacementView& PublicRefs,
		TArray<uint8>& OutCanonicalUtf8, FWorkspaceError& OutError,
		const FString& SourceFile = FString());

	UEAGENTINTERFACE_API bool TrySerializeSemanticUnitDocument(
		const FSemanticUnit& Unit,
		TArray<uint8>& OutCanonicalUtf8, FWorkspaceError& OutError,
		const FString& SourceFile = FString());

	UEAGENTINTERFACE_API bool TrySerializeSemanticUnitDocument(
		const FSemanticUnit& Unit,
		const TMap<FString, FString>& PublicRefs,
		TArray<uint8>& OutCanonicalUtf8, FWorkspaceError& OutError,
		const FString& SourceFile = FString());
	UEAGENTINTERFACE_API bool TrySerializeSemanticUnitDocument(
		const FSemanticUnit& Unit, const FWorkspacePlacementView& PublicRefs,
		TArray<uint8>& OutCanonicalUtf8, FWorkspaceError& OutError,
		const FString& SourceFile = FString());

	UEAGENTINTERFACE_API bool TryCompileAuthoringModel(
		FString ContractId,
		int32 ContractVersion,
		FString ProfileId,
		FString BaseRevision,
		TArray<FSemanticUnit> Units,
		TArray<FString> TargetUnitIds,
		FUeAgentResourceTracker& Resources,
		FAuthoringModelPtr& OutModel,
		FWorkspaceError& OutError);

	// Internal projection continuity; the existing compiler entry remains valid.
	UEAGENTINTERFACE_API bool TryCompileAuthoringModel(
		FString ContractId, int32 ContractVersion, FString ProfileId, FString BaseRevision,
		TArray<FSemanticUnit> Units, TArray<FString> TargetUnitIds,
		FUeAgentResourceTracker& Resources, FAuthoringModelPtr& OutModel,
		FWorkspaceError& OutError, const FDefinitionProjectionHints* ProjectionHints);

	// Owner-supplied successful candidates supplement the existing weak fallback;
	// they never replace current validation, resource charges or linear SHA.
	UEAGENTINTERFACE_API bool TryCompileAuthoringModel(
		FString ContractId, int32 ContractVersion, FString ProfileId, FString BaseRevision,
		TArray<FSemanticUnit> Units, TArray<FString> TargetUnitIds,
		FUeAgentResourceTracker& Resources, FAuthoringModelPtr& OutModel,
		FWorkspaceError& OutError, const FDefinitionProjectionHints* ProjectionHints,
		const FAuthoringModel* ModelCandidate, const FCompiledAuthoringReuseProof* StructureCandidate,
		bool bBuildLegacyFingerprint = true);

	// Compiler-owned successful candidates; called by the existing server lifecycle.
	/** Source rows have already passed their original inventory hashes. When
	 * supplied, verify the original manifest SHA before projecting schema values.
	 * Old workspaces retain their original integrity grammar and diagnostics. */
	UEAGENTINTERFACE_API bool TryCompileAuthoringSourceModel(
		FString ContractId, int32 ContractVersion, FString ProfileId, FString BaseRevision,
		TArray<FSemanticUnit> Units, TArray<FString> TargetUnitIds,
		FUeAgentResourceTracker& Resources, FAuthoringModelPtr& OutModel,
		FWorkspaceError& OutError, const FDefinitionProjectionHints* ProjectionHints,
		const FAuthoringModel* ModelCandidate, const FCompiledAuthoringReuseProof* StructureCandidate,
		const FString* ExpectedSourceFingerprint, bool bBuildLegacyFingerprint = true);

	UEAGENTINTERFACE_API void ResetCompiledAuthoringReuse();

	UEAGENTINTERFACE_API bool TryPlanWorkspaceLayout(
		FAuthoringModelRef Model,
		int32 LayoutVersion,
		EWorkspaceSourceMode SourceMode,
		TMap<FString, int64> Telemetry,
		FUeAgentResourceTracker& Resources,
		FWorkspaceLayoutPlanPtr& OutPlan,
		FWorkspaceError& OutError, const FSemanticProjectionBindings* Bindings = nullptr,
		const FCommittedWorkspaceDelta* ActualChanges = nullptr, const FWorkspaceLayoutPlan* AcceptedLayout = nullptr);

	UEAGENTINTERFACE_API bool IsModelVisibleSemanticWorkspacePath(
		const FString& RelativePath);
}

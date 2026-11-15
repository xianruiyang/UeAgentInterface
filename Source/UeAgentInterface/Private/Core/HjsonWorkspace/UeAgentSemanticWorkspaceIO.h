#pragma once

#include "CoreMinimal.h"
#include "Core/Contract/UeAgentDiagnostic.h"
#include "Core/Contract/UeAgentDiagnostic.h"
#include "Core/HjsonWorkspace/UeAgentSemanticLayout.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "Templates/SharedPointer.h"

class FJsonObject;
class FJsonValue;

namespace UE::UAI::HjsonWorkspace
{
	struct FDocumentValueSourceBinding;
	class FSemanticProjectionBindings;
	void VisitCapturedKeyedMemberNames(const StateSemantic::FUeAgentCanonicalValue& Items, FKeyedMemberNameVisitor Visitor);
	using UE::UAI::StateSemantic::FUeAgentStateRevision;
	/** Attach the same parser provenance used by semantic workspaces to an
	 * inline/initialization apply payload. Does not modify the source document. */
	UEAGENTINTERFACE_API void AttachInlinePropertySources(
		const FHjsonDocument& Document, const FString& ObjectPointer,
		FJsonObject& Object, const FString& PreferredField = TEXT("candidate"));
	/** Carry parser locations through a native descriptor refresh by property path. */
	UEAGENTINTERFACE_API void AttachProjectedPropertySources(
		const TArray<TSharedPtr<FJsonValue>>& Projected, FJsonObject& Properties);

	enum class ESemanticWorkspaceMode : uint8
	{
		State,
		Seed,
		EditBundle,
	};

	UEAGENTINTERFACE_API FString LexToString(
		ESemanticWorkspaceMode Mode);

	enum class EStructuralEditKind : uint8
	{
		Add,
		Remove,
		Replace,
		Rename,
		AddRelation,
		RemoveRelation,
		SetOrder,
	};

	UEAGENTINTERFACE_API FString LexToString(
		EStructuralEditKind Kind);

	class UEAGENTINTERFACE_API FStructuralEdit final
	{
	public:
		FStructuralEdit(
			FString InEditId,
			EStructuralEditKind InKind,
			FString InOwnerUnitId,
			FString InSubjectUnitId = FString(),
			FString InReplacementUnitId = FString(),
			FString InRelationId = FString(),
			TArray<FString> InOrderedUnitIds = {},
			TOptional<FSourceProvenance> InSource = {});

		const FString& GetEditId() const;
		EStructuralEditKind GetKind() const;
		const FString& GetOwnerUnitId() const;
		const FString& GetSubjectUnitId() const;
		const FString& GetReplacementUnitId() const;
		const FString& GetRelationId() const;
		const TArray<FString>& GetOrderedUnitIds() const;
		/** Original authored row; diagnostic metadata only. */
		const TOptional<FSourceProvenance>& GetSource() const { return Source; }
		bool IsValid() const;
		const Contract::FDiagnostic& GetValidationDiagnostic() const { return ValidationDiagnostic; }

	private:
		FString EditId;
		EStructuralEditKind Kind = EStructuralEditKind::Add;
		FString OwnerUnitId;
		FString SubjectUnitId;
		FString ReplacementUnitId;
		FString RelationId;
		TArray<FString> OrderedUnitIds;
		TOptional<FSourceProvenance> Source;
		Contract::FDiagnostic ValidationDiagnostic;
	};

	class FResolvedSemanticWorkspace;
	using FResolvedSemanticWorkspacePtr =
		TSharedPtr<const FResolvedSemanticWorkspace>;
	using FResolvedSemanticWorkspaceRef =
		TSharedRef<const FResolvedSemanticWorkspace>;

	/** Current bindings are accepted only by the resolver after source,
	 * membership and dependency proof. Generated bindings may be supplied as
	 * candidates; their actual file hashes must first match the current read. */
	class FResolvedWorkspaceUnitBindings
	{
	public:
		virtual ~FResolvedWorkspaceUnitBindings() = default;
		virtual const FSourceProvenance* FindSource(const FString& UnitId) const = 0;
		virtual const FSemanticUnit* FindBaseline(const FString& UnitId) const = 0;
		/** Query captured value strings without materializing a typed unit when
		 * the authoritative backing is a published envelope. Unset means this
		 * identity has no before-value, not an empty or null reference. */
		virtual bool TryBaselineContainsAnyString(const FString& UnitId, TFunctionRef<bool(const FString&)> Matches,
			TOptional<bool>& Out, FWorkspaceError& Error) const;
		/** A before-value consumer requests the addressed members explicitly.
		 * This does not replace the complete published baseline or its identity. */
		virtual bool TryReadBaselineMembers(const FString& UnitId, const FString& RootField, const TSet<FString>& Keys,
			ContextResource::FUeAgentResourceTracker& Resources, TOptional<FSemanticUnit>& Out, FWorkspaceError& Error) const;
		virtual bool VisitBaselineMemberNames(const FString& UnitId, const FString& RootField, FKeyedMemberNameVisitor Visitor,
			ContextResource::FUeAgentResourceTracker& Resources, FWorkspaceError& Error) const;
		/** Directory-backed readers expose the already accepted row digest
		 * without requesting that row's value payload. Absence is not a request
		 * to scan or materialize the complete workspace. */
		virtual FString ReadBaselineHash(const FString&) const { return FString(); }
		virtual const FWorkspaceError* GetReadFailure() const { return nullptr; }
		virtual FHjsonDocumentPtr FindSourceDocument(const FString& UnitId) const = 0;
		virtual void CopySources(TMap<FString, FSourceProvenance>& Out) const = 0;
		virtual void CopyBaselines(TMap<FString, FSemanticUnit>& Out) const = 0;
		/** Pure generated recipe, not a source AST. The resolver binds its exact
		 * file hash before exposing it to an accepted-source consumer. */
		virtual StateSemantic::FUeAgentCanonicalValuePtr FindGeneratedDocument(const FString&) const { return nullptr; }
		/** Borrow the actual producer's naming/hash view after the resolver has
		 * accepted that generation. Legacy/remapped inventory keeps its reader. */
		virtual const FSemanticProjectionBindings* GetGeneratedProjection() const { return nullptr; }
		virtual const class FAcceptedWorkspaceInventory* GetGeneratedInventory() const { return nullptr; }
		/** A publication's original directory reader can answer one identity
		 * without manufacturing its complete declaration/index representation.
		 * The count is exact; absence retains the ordinary concrete model. */
		virtual TOptional<int32> GetPublishedUnitCount() const { return {}; }
		virtual TSharedPtr<const FDefinitionProjectionBinding> FindPublishedDefinitionBinding(const FString&) const { return nullptr; }
		virtual TSharedPtr<const FDefinitionProjectionHints> ReadPublishedDefinitionBindings() const { return nullptr; }
		virtual TOptional<FSemanticUnit::FDeclarationView> FindPublishedDeclaration(const FString&) const { return {}; }
		virtual bool VisitPublishedDeclarations(TFunctionRef<bool(const FSemanticUnit::FDeclarationView&, int32)> Visitor) const { return false; }
		virtual bool VisitPublishedDefinitionDeclarations(TFunctionRef<bool(const FSemanticUnit::FDeclarationView&)> Visitor) const {
			return VisitPublishedDeclarations([&](const FSemanticUnit::FDeclarationView& Declaration, int32) {
				return Declaration.GetRole() != TEXT("property_definition") || Visitor(Declaration);
			});
		}
		virtual bool VisitPublishedTargetIdentity(const FString& Target,
			TFunctionRef<void(const FSemanticUnit::FDeclarationView&)> Visitor) const {
			return VisitPublishedDeclarations([&](const FSemanticUnit::FDeclarationView& Declaration, int32) {
				if (Declaration.GetTargetIdentityKey() == Target) Visitor(Declaration);
				return true;
			});
		}
	};

	/** Exact accepted inventory hashes. Implementations retain compact values,
	 * not parser trees; the legacy complete map is expanded only on request. */
	class FResolvedWorkspaceBaselineHashes
	{
	public:
		virtual ~FResolvedWorkspaceBaselineHashes() = default;
		virtual void CopyTo(TMap<FString, FString>& Out) const = 0;
	};

	class UEAGENTINTERFACE_API FResolvedSemanticWorkspace final
	{
	public:
		FResolvedSemanticWorkspace(
			EWorkspaceSourceKind InSourceKind,
			ESemanticWorkspaceMode InMode,
			FAuthoringModelRef InModel,
			FUeAgentStateRevision InBaseStateRevision,
			TArray<FStructuralEdit> InStructuralEdits,
			TArray<FString> InManagedUnitPaths,
			FString InSelectionFingerprint,
			FString InClosureFingerprint,
			TArray<FString> InClosureUnitIds,
			FString InWorkspaceFingerprint,
			TOptional<TArray<FString>> InEditedUnitIds = {},
			TMap<FString, TSet<FString>> InResolvedKeyedOperationRoots = {},
			TMap<FString, FSourceProvenance> InUnitSources = {},
			TMap<FString, FString> InBaselineUnitHashes = {},
			TMap<FString, FHjsonDocumentRef> InUnitSourceDocuments = {},
			TMap<FString, FSemanticUnit> InBaselineUnits = {},
			TOptional<FSourceProvenance> InStructuralEditsSource = {},
			TSharedPtr<const FResolvedWorkspaceUnitBindings> InUnitBindings = {});
		FResolvedSemanticWorkspace(
			EWorkspaceSourceKind InSourceKind, ESemanticWorkspaceMode InMode,
			FAuthoringModelRef InModel, FUeAgentStateRevision InBaseStateRevision,
			TArray<FStructuralEdit> InStructuralEdits, TArray<FString> InManagedUnitPaths,
			FString InSelectionFingerprint, FString InClosureFingerprint,
			TArray<FString> InClosureUnitIds, FString InWorkspaceFingerprint,
			TOptional<TArray<FString>> InEditedUnitIds,
			TMap<FString, TSet<FString>> InResolvedKeyedOperationRoots,
			TMap<FString, FSourceProvenance> InUnitSources,
			TMap<FString, FString> InBaselineUnitHashes,
			TMap<FString, FHjsonDocumentRef> InUnitSourceDocuments,
			TMap<FString, FSemanticUnit> InBaselineUnits,
			TOptional<FSourceProvenance> InStructuralEditsSource,
			TSharedPtr<const FResolvedWorkspaceUnitBindings> InUnitBindings,
			TSharedPtr<const FResolvedWorkspaceBaselineHashes> InBaselineHashView,
			FString InPublishedSemanticFingerprint = {});

		EWorkspaceSourceKind GetSourceKind() const;
		ESemanticWorkspaceMode GetMode() const;
		const FAuthoringModelRef& GetModel() const;
		const FUeAgentStateRevision& GetBaseStateRevision() const;
		const TArray<FStructuralEdit>& GetStructuralEdits() const;
		/** Original manifest array; never part of semantic hashes or serialization. */
		const TOptional<FSourceProvenance>& GetStructuralEditsSource() const { return StructuralEditsSource; }
		const TArray<FString>& GetManagedUnitPaths() const;
		const FString& GetSelectionFingerprint() const;
		const FString& GetClosureFingerprint() const;
		const TArray<FString>& GetClosureUnitIds() const;
		const FString& GetWorkspaceFingerprint() const;
		/** Manifest identity of the published capture. Not the edited/reparsed model's SHA. */
		const FString& GetPublishedSemanticFingerprint() const { return PublishedSemanticFingerprint; }
		const TOptional<TArray<FString>>& GetEditedUnitIds() const;
		/** Request-local accepted/native handoff; never changes the published
		 * inventory or the globally accepted raw Source. */
		TSharedRef<const FResolvedSemanticWorkspace> RebindUnconsumedAuthoring(
			FAuthoringModelRef NewModel, TArray<FString> NewEdits,
			TMap<FString, FSemanticUnit> ActualBaselines, TMap<FString, FString> ActualHashes,
			TArray<FStructuralEdit> RemainingStructuralEdits) const;
		bool HasResolvedAcceptedAuthoring() const { return bResolvedAcceptedAuthoring; }
		/** In-memory provenance from actual keyed resolution; never read from HJSON or serialized. */
		const TMap<FString, TSet<FString>>& GetResolvedKeyedOperationRoots() const { return ResolvedKeyedOperationRoots; }
		const TMap<FString, FSourceProvenance>& GetUnitSources() const;
		const FSourceProvenance* FindUnitSource(const FString& UnitId) const;
		/** Source of an authored value, before projection into a native apply payload. */
		const FHjsonNode* FindValueNode(const FString& UnitId, const FString& JsonPointer) const;
		const FSourceProvenance* FindValueSource(const FString& UnitId, const FString& JsonPointer) const;
		/** Carry a value subtree's parser provenance into a generated document.
		 * Call at the transformation that knows the actual output address. */
		void ProjectValueSources(const FString& UnitId, const FString& ValuePointer,
			const FString& DocumentPointer, TMap<FString, Contract::FSourceLocation>& OutSources) const;
		const FSourceProvenance* FindDocumentValueSource(const FString& DocumentIdentity,
			const FString& DocumentPointer, const TArray<FDocumentValueSourceBinding>& Bindings) const;
		/** Locate a semantic diagnostic without treating inferred values as authored leaves. */
		void LocateDocumentDiagnostic(const TArray<FString>& DocumentIdentities,
			const TArray<FDocumentValueSourceBinding>& Bindings, Contract::FDiagnostic& Diagnostic,
			const FString& GraphUnitId = FString()) const;
		/** Annotate the object/candidate and inline properties in a projected apply payload only;
		 * authored/canonical values stay unchanged. */
		void AttachInlinePropertySources(const FString& UnitId, const FString& ObjectPointer,
			FJsonObject& Object, const FString& PreferredField = TEXT("candidate")) const;
		/** Map a recomposed document object back through composition's fragment
		 * binding before using the existing parser provenance. */
		void AttachDocumentPropertySources(const FString& DocumentIdentity,
			const FString& DocumentPointer, const TArray<FDocumentValueSourceBinding>& Bindings,
			FJsonObject& Object, const FString& PreferredField = TEXT("candidate")) const;
		const TMap<FString, FString>& GetBaselineUnitHashes() const;
		/** Read the accepted row digest without expanding the complete hash map or values. */
		FString ReadBaselineUnitHash(const FString& UnitId) const;
		const TMap<FString, FSemanticUnit>& GetBaselineUnits() const;
		const FSemanticUnit* FindBaselineUnit(const FString& UnitId) const;
		bool TryBaselineContainsAnyString(const FString& UnitId, TFunctionRef<bool(const FString&)> Matches,
			TOptional<bool>& Out, FWorkspaceError& Error) const;
		bool TryReadBaselineMembers(const FString& UnitId, const FString& RootField, const TSet<FString>& Keys,
			ContextResource::FUeAgentResourceTracker& Resources, TOptional<FSemanticUnit>& Out, FWorkspaceError& Error) const;
		bool VisitBaselineMemberNames(const FString& UnitId, const FString& RootField, FKeyedMemberNameVisitor Visitor,
			ContextResource::FUeAgentResourceTracker& Resources, FWorkspaceError& Error) const;
		/** In-memory rebind after conflict analysis; never rewrites the user's files. */
		TSharedRef<const FResolvedSemanticWorkspace> Rebind(
			FAuthoringModelRef NewModel, const FUeAgentStateRevision& Revision) const;
		bool IsValid() const;
		const Contract::FDiagnostic& GetValidationDiagnostic() const { return ValidationDiagnostic; }

	private:
		EWorkspaceSourceKind SourceKind =
			EWorkspaceSourceKind::Inline;
		ESemanticWorkspaceMode Mode =
			ESemanticWorkspaceMode::State;
		FAuthoringModelRef Model;
		FUeAgentStateRevision BaseStateRevision;
		TArray<FStructuralEdit> StructuralEdits;
		TOptional<FSourceProvenance> StructuralEditsSource;
		TArray<FString> ManagedUnitPaths;
		FString SelectionFingerprint;
		FString ClosureFingerprint;
		TArray<FString> ClosureUnitIds;
		FString WorkspaceFingerprint;
		FString PublishedSemanticFingerprint;
		TOptional<TArray<FString>> EditedUnitIds;
		bool bResolvedAcceptedAuthoring = false;
		TMap<FString, TSet<FString>> ResolvedKeyedOperationRoots;
		TMap<FString, FSourceProvenance> UnitSources;
		TMap<FString, FString> BaselineUnitHashes;
		TSharedPtr<const FResolvedWorkspaceBaselineHashes> BaselineHashView;
		mutable TSharedPtr<const TMap<FString, FString>> MaterializedBaselineHashes;
		TMap<FString, FHjsonDocumentRef> UnitSourceDocuments;
		TMap<FString, FSemanticUnit> BaselineUnits;
		TSharedPtr<const FResolvedWorkspaceUnitBindings> UnitBindings;
		mutable TSharedPtr<const TMap<FString, FSourceProvenance>> MaterializedUnitSources;
		mutable TSharedPtr<const TMap<FString, FSemanticUnit>> MaterializedBaselineUnits;
		void Validate();
		Contract::FDiagnostic ValidationDiagnostic;
	};

	UEAGENTINTERFACE_API bool TryValidateSemanticWorkspaceBinding(
		const FResolvedSemanticWorkspaceRef& Workspace,
		ESemanticWorkspaceMode ExpectedMode,
		const FAuthoringModelRef& CurrentModel,
		const FUeAgentStateRevision& CurrentRevision,
		const FString& ExpectedSelectionFingerprint,
		const FString& ExpectedClosureFingerprint,
		FWorkspaceError& OutError,
		bool bAllowVerifiedRevisionDrift = false);
}

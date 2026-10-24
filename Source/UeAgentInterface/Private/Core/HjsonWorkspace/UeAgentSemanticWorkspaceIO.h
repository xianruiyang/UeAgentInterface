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
			TOptional<FSourceProvenance> InStructuralEditsSource = {});

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
		const TOptional<TArray<FString>>& GetEditedUnitIds() const;
		/** In-memory provenance from actual keyed resolution; never read from HJSON or serialized. */
		const TMap<FString, TSet<FString>>& GetResolvedKeyedOperationRoots() const { return ResolvedKeyedOperationRoots; }
		const TMap<FString, FSourceProvenance>& GetUnitSources() const { return UnitSources; }
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
		const TMap<FString, FString>& GetBaselineUnitHashes() const { return BaselineUnitHashes; }
		const TMap<FString, FSemanticUnit>& GetBaselineUnits() const { return BaselineUnits; }
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
		TOptional<TArray<FString>> EditedUnitIds;
		TMap<FString, TSet<FString>> ResolvedKeyedOperationRoots;
		TMap<FString, FSourceProvenance> UnitSources;
		TMap<FString, FString> BaselineUnitHashes;
		TMap<FString, FHjsonDocumentRef> UnitSourceDocuments;
		TMap<FString, FSemanticUnit> BaselineUnits;
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

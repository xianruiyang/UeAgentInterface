#pragma once

#include "CoreMinimal.h"
#include "Core/HjsonWorkspace/UeAgentSemanticLayout.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "Templates/SharedPointer.h"

namespace UE::UAI::HjsonWorkspace
{
	using UE::UAI::StateSemantic::FUeAgentStateRevision;

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
			TArray<FString> InOrderedUnitIds = {});

		const FString& GetEditId() const;
		EStructuralEditKind GetKind() const;
		const FString& GetOwnerUnitId() const;
		const FString& GetSubjectUnitId() const;
		const FString& GetReplacementUnitId() const;
		const FString& GetRelationId() const;
		const TArray<FString>& GetOrderedUnitIds() const;
		bool IsValid() const;

	private:
		FString EditId;
		EStructuralEditKind Kind = EStructuralEditKind::Add;
		FString OwnerUnitId;
		FString SubjectUnitId;
		FString ReplacementUnitId;
		FString RelationId;
		TArray<FString> OrderedUnitIds;
		bool bValid = false;
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
			FString InWorkspaceFingerprint);

		EWorkspaceSourceKind GetSourceKind() const;
		ESemanticWorkspaceMode GetMode() const;
		const FAuthoringModelRef& GetModel() const;
		const FUeAgentStateRevision& GetBaseStateRevision() const;
		const TArray<FStructuralEdit>& GetStructuralEdits() const;
		const TArray<FString>& GetManagedUnitPaths() const;
		const FString& GetSelectionFingerprint() const;
		const FString& GetClosureFingerprint() const;
		const TArray<FString>& GetClosureUnitIds() const;
		const FString& GetWorkspaceFingerprint() const;
		bool IsValid() const;

	private:
		EWorkspaceSourceKind SourceKind =
			EWorkspaceSourceKind::Inline;
		ESemanticWorkspaceMode Mode =
			ESemanticWorkspaceMode::State;
		FAuthoringModelRef Model;
		FUeAgentStateRevision BaseStateRevision;
		TArray<FStructuralEdit> StructuralEdits;
		TArray<FString> ManagedUnitPaths;
		FString SelectionFingerprint;
		FString ClosureFingerprint;
		TArray<FString> ClosureUnitIds;
		FString WorkspaceFingerprint;
		bool bValid = false;
	};

	UEAGENTINTERFACE_API bool TryValidateSemanticWorkspaceBinding(
		const FResolvedSemanticWorkspaceRef& Workspace,
		ESemanticWorkspaceMode ExpectedMode,
		const FAuthoringModelRef& CurrentModel,
		const FUeAgentStateRevision& CurrentRevision,
		const FString& ExpectedSelectionFingerprint,
		const FString& ExpectedClosureFingerprint,
		FWorkspaceError& OutError);
}

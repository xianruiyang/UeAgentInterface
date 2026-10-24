#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"
#include "Core/Discovery/UeAgentDiscovery.h"
#include "Core/HjsonWorkspace/UeAgentExportFieldContract.h"
#include "Core/HjsonWorkspace/UeAgentSemanticLayout.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"

namespace UE::UAI::HjsonWorkspace
{
	using UE::UAI::Discovery::FUeAgentCandidateBinding;
	using UE::UAI::StateSemantic::FUeAgentStateEntry;

	enum class EWorkspaceSelectionKind : uint8
	{
		Property,
		GraphItem,
		Connection,
		StructuralAdd,
		TreeItem,
		TimelineItem,
		SpatialLayout,
	};

	UEAGENTINTERFACE_API FString LexToString(
		EWorkspaceSelectionKind Kind);

	class UEAGENTINTERFACE_API FWorkspaceSelection final
	{
	public:
		FWorkspaceSelection(
			FString InSelectionId,
			EWorkspaceSelectionKind InSelectionKind,
			TArray<FString> InSelectedUnitIds,
			TArray<FString> InSelectedFieldIds,
			TArray<FString> InSelectedRelationIds,
			FString InCandidateBindingId);

		const FString& GetSelectionId() const;
		EWorkspaceSelectionKind GetSelectionKind() const;
		const TArray<FString>& GetSelectedUnitIds() const;
		const TArray<FString>& GetSelectedFieldIds() const;
		const TArray<FString>& GetSelectedRelationIds() const;
		const FString& GetCandidateBindingId() const;
		bool IsValid() const;

	private:
		FString SelectionId;
		EWorkspaceSelectionKind SelectionKind =
			EWorkspaceSelectionKind::Property;
		TArray<FString> SelectedUnitIds;
		TArray<FString> SelectedFieldIds;
		TArray<FString> SelectedRelationIds;
		FString CandidateBindingId;
		bool bValid = false;
	};

	class UEAGENTINTERFACE_API FSelectionPolicy final
	{
	public:
		FSelectionPolicy(
			FString InContractId,
			int32 InContractVersion,
			FString InPolicyId,
			TMap<FString, TArray<FString>> InControllingFieldIds,
			TMap<FString, TArray<FString>> InDynamicConstraintUnitIds,
			TArray<FString> InInterfaceRoles,
			TArray<ESemanticUnitKind> InPlacementUnitKinds);

		const FString& GetContractId() const;
		int32 GetContractVersion() const;
		const FString& GetPolicyId() const;
		const TMap<FString, TArray<FString>>&
			GetControllingFieldIds() const;
		const TMap<FString, TArray<FString>>&
			GetDynamicConstraintUnitIds() const;
		const TArray<FString>& GetInterfaceRoles() const;
		const TArray<ESemanticUnitKind>& GetPlacementUnitKinds() const;
		const FString& GetPolicyFingerprint() const;
		bool IsValid() const;

	private:
		FString ContractId;
		int32 ContractVersion = 0;
		FString PolicyId;
		TMap<FString, TArray<FString>> ControllingFieldIds;
		TMap<FString, TArray<FString>> DynamicConstraintUnitIds;
		TArray<FString> InterfaceRoles;
		TArray<ESemanticUnitKind> PlacementUnitKinds;
		FString PolicyFingerprint;
		bool bValid = false;
	};

	class UEAGENTINTERFACE_API FSelectionClosure final
	{
	public:
		FSelectionClosure(
			FWorkspaceSelection InSelection,
			TArray<FString> InUnitIds,
			TArray<FString> InFieldIds,
			TArray<FString> InRelationIds,
			FString InSelectionFingerprint,
			FString InClosureFingerprint);

		const FWorkspaceSelection& GetSelection() const;
		const TArray<FString>& GetUnitIds() const;
		const TArray<FString>& GetFieldIds() const;
		const TArray<FString>& GetRelationIds() const;
		const FString& GetSelectionFingerprint() const;
		const FString& GetClosureFingerprint() const;
		bool IsValid() const;

	private:
		FWorkspaceSelection Selection;
		TArray<FString> UnitIds;
		TArray<FString> FieldIds;
		TArray<FString> RelationIds;
		FString SelectionFingerprint;
		FString ClosureFingerprint;
		bool bValid = false;
	};

	class FScopedEditBundle;
	using FScopedEditBundlePtr =
		TSharedPtr<const FScopedEditBundle>;
	using FScopedEditBundleRef =
		TSharedRef<const FScopedEditBundle>;

	class UEAGENTINTERFACE_API FScopedEditBundle final
	{
	public:
		FScopedEditBundle(
			FString InWorkspaceSchema,
			FString InProfileId,
			FString InContractId,
			int32 InContractVersion,
			FString InBaseRevision,
			FString InBaseSemanticFingerprint,
			FString InSelectionFingerprint,
			FString InClosureFingerprint,
			FString InSourceSnapshotId,
			TMap<FString, FSemanticUnit> InUnitsById,
			TArray<FUeAgentStateEntry> InFieldStates,
			TMap<FString, FExportFieldDescription>
				InFieldDescriptionsById,
			TMap<FString, FString> InLogicalPlacements,
			TOptional<FUeAgentCandidateBinding> InCandidateBinding,
			FString InBundleFingerprint);

		const FString& GetWorkspaceSchema() const;
		const FString& GetProfileId() const;
		const FString& GetContractId() const;
		int32 GetContractVersion() const;
		const FString& GetBaseRevision() const;
		const FString& GetBaseSemanticFingerprint() const;
		const FString& GetSelectionFingerprint() const;
		const FString& GetClosureFingerprint() const;
		const FString& GetSourceSnapshotId() const;
		const TMap<FString, FSemanticUnit>& GetUnitsById() const;
		const TArray<FUeAgentStateEntry>& GetFieldStates() const;
		const TMap<FString, FExportFieldDescription>&
			GetFieldDescriptionsById() const;
		const TMap<FString, FString>& GetLogicalPlacements() const;
		const TOptional<FUeAgentCandidateBinding>&
			GetCandidateBinding() const;
		const FString& GetBundleFingerprint() const;
		bool IsValid() const;

	private:
		FString WorkspaceSchema;
		FString ProfileId;
		FString ContractId;
		int32 ContractVersion = 0;
		FString BaseRevision;
		FString BaseSemanticFingerprint;
		FString SelectionFingerprint;
		FString ClosureFingerprint;
		FString SourceSnapshotId;
		TMap<FString, FSemanticUnit> UnitsById;
		TArray<FUeAgentStateEntry> FieldStates;
		TMap<FString, FExportFieldDescription> FieldDescriptionsById;
		TMap<FString, FString> LogicalPlacements;
		TOptional<FUeAgentCandidateBinding> CandidateBinding;
		FString BundleFingerprint;
		bool bValid = false;
	};

	using FEditBundleSelectionError = UE::UAI::Contract::FDiagnostic;

	UEAGENTINTERFACE_API bool TryResolveSelectionClosure(
		const FAuthoringModelRef& Model,
		const FWorkspaceSelection& Selection,
		const FSelectionPolicy& Policy,
		TOptional<FSelectionClosure>& OutClosure,
		FEditBundleSelectionError& OutError);

	UEAGENTINTERFACE_API bool TryBuildScopedEditBundle(
		const FAuthoringModelRef& Model,
		const FWorkspaceLayoutPlan& Layout,
		const FSelectionClosure& Closure,
		const FString& SourceSnapshotId,
		const TArray<FUeAgentStateEntry>& FieldStates,
		const TMap<FString, FExportFieldDescription>&
			FieldDescriptionsById,
		TOptional<FUeAgentCandidateBinding> CandidateBinding,
		FScopedEditBundlePtr& OutBundle,
		FEditBundleSelectionError& OutError);

	UEAGENTINTERFACE_API FString GetEditBundleWorkspaceSchemaId();
	UEAGENTINTERFACE_API FString GetEditBundleProfileId();
}

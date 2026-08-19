#pragma once

#include "CoreMinimal.h"
#include "Core/HjsonWorkspace/UeAgentSemanticLayout.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringState.h"
#include "Utilities/AuthoringExport/UeAgentExportWorkspaceWriter.h"

namespace UE::UAI::HjsonWorkspace
{
	enum class EAuthoringSurfaceKind : uint8
	{
		Properties,
		Collection,
		Structure,
		Items,
		Relations,
		Timeline,
		Hierarchy,
		ExternalPayload,
	};

	struct UEAGENTINTERFACE_API FCompositionSurfaceContract
	{
		FString SurfaceId;
		EAuthoringSurfaceKind SurfaceKind =
			EAuthoringSurfaceKind::Properties;
		ESemanticUnitKind UnitKind =
			ESemanticUnitKind::PropertySet;
	};

	enum class ECompositionSemanticRouteMatch : uint8
	{
		Exact,
		Suffix,
		Contains,
		Default,
	};

	enum class ECompositionSemanticRouteRole : uint8
	{
		Undeclared,
		TargetValues,
		Graph,
		Item,
		Property,
		Relation,
		Document,
	};

	struct UEAGENTINTERFACE_API FCompositionSemanticRouteContract
	{
		ECompositionSemanticRouteMatch Match =
			ECompositionSemanticRouteMatch::Exact;
		FString Pattern;
		FString SurfaceId;
		ESemanticUnitKind UnitKind =
			ESemanticUnitKind::PropertySet;
		ECompositionSemanticRouteRole Role =
			ECompositionSemanticRouteRole::Undeclared;
		int32 Priority = 0;
	};

	struct UEAGENTINTERFACE_API FTargetCompositionContract
	{
		FString SchemaId;
		FString TargetKind;
		TArray<FCompositionSurfaceContract> Surfaces;
		TArray<FCompositionSemanticRouteContract> SemanticRoutes;
	};

	class UEAGENTINTERFACE_API FTargetCompositionRegistry final
	{
	public:
		static FTargetCompositionRegistry& Get();

		bool RegisterContract(
			FTargetCompositionContract Contract,
			FString& OutError);
		bool RegisterContracts(
			TArray<FTargetCompositionContract> InContracts,
			FString& OutError);
		bool FindContract(
			const FString& TargetKind,
			FTargetCompositionContract& OutContract) const;
		TArray<FTargetCompositionContract> ListContracts() const;

	private:
		mutable FRWLock RegistryLock;
		TMap<FString, FTargetCompositionContract> Contracts;
	};

	struct UEAGENTINTERFACE_API FCompositionRequest
	{
		FString TargetKind;
		FString ProfileId;
		FString BaseRevision;
		FString SourceProvenance;
		TArray<UeAgentAuthoringState::FStateEntry> Entries;
	};

	struct UEAGENTINTERFACE_API FCanonicalCompositionRequest
	{
		FString TargetKind;
		FString ProfileId;
		FString BaseRevision;
		FString SourceProvenance;
		TArray<UE::UAI::StateSemantic::FUeAgentStateEntry> Entries;
	};

	struct UEAGENTINTERFACE_API FCompositionSnapshotBinding
	{
		FString SnapshotId;
		UE::UAI::StateSemantic::FUeAgentStateRevision Revision;
		FString EvidenceId;

		bool IsValid() const;
	};

	UEAGENTINTERFACE_API FString
	GetTargetCompositionContractSchemaId();

	UEAGENTINTERFACE_API bool ValidateTargetCompositionContract(
		const FTargetCompositionContract& Contract,
		FString& OutError);

	UEAGENTINTERFACE_API bool
	RegisterGraphTreeTargetCompositionContracts(FString& OutError);

	UEAGENTINTERFACE_API bool
	RegisterOtherTargetCompositionContracts(FString& OutError);

	UEAGENTINTERFACE_API bool TryResolveTargetCompositionSemanticRoute(
		const FString& TargetKind,
		const FString& SemanticId,
		FCompositionSemanticRouteContract& OutRoute);

	UEAGENTINTERFACE_API FString ResolveTargetCompositionKind(
		const FString& SourceTargetKind,
		const FString& SourceTypeId);

	UEAGENTINTERFACE_API bool TryComposeAuthoringModel(
		const FCompositionRequest& Request,
		ContextResource::FUeAgentResourceTracker& Resources,
		FAuthoringModelPtr& OutModel,
		FWorkspaceError& OutError);

	UEAGENTINTERFACE_API bool TryComposeCanonicalAuthoringModel(
		const FCanonicalCompositionRequest& Request,
		ContextResource::FUeAgentResourceTracker& Resources,
		FAuthoringModelPtr& OutModel,
		FWorkspaceError& OutError);

	UEAGENTINTERFACE_API bool TryBuildCompositionExportPlan(
		FAuthoringModelRef Model,
		TMap<FString, int64> Telemetry,
		ContextResource::FUeAgentResourceTracker& Resources,
		FExportWorkspacePlanPtr& OutPlan,
		FWorkspaceError& OutError);

	UEAGENTINTERFACE_API bool TryBuildCompositionExportPlan(
		FAuthoringModelRef Model,
		const FCompositionSnapshotBinding& SnapshotBinding,
		TMap<FString, int64> Telemetry,
		ContextResource::FUeAgentResourceTracker& Resources,
		FExportWorkspacePlanPtr& OutPlan,
		FWorkspaceError& OutError);

	UEAGENTINTERFACE_API bool TryExportCompositionWorkspace(
		const FString& TargetKind,
		const FString& ProfileId,
		const UeAgentAuthoringState::FStateSnapshot& Snapshot,
		const FString& WorkspaceRoot,
		UeAgentExportWorkspaceWriter::FWorkspaceResult& OutResult,
		FString& OutError);
}

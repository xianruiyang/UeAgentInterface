#include "Core/HjsonWorkspace/UeAgentSnapshotComposition.h"

namespace UE::UAI::HjsonWorkspace
{
	using namespace UE::UAI::StateSemantic;

	namespace
	{
		FString Normalize(FString Value)
		{
			Value.TrimStartAndEndInline();
			return Value;
		}
	}

	FSnapshotCompositionRequest::FSnapshotCompositionRequest(
		FString InTargetKind,
		FString InProfileId,
		FUeAgentCanonicalSnapshotRef InSnapshot)
		: TargetKind(Normalize(MoveTemp(InTargetKind)))
		, ProfileId(Normalize(MoveTemp(InProfileId)))
		, Snapshot(MoveTemp(InSnapshot))
	{
		bValid = !TargetKind.IsEmpty()
			&& !ProfileId.IsEmpty()
			&& Snapshot->GetRevision().IsValid()
			&& Snapshot->GetProvenance().IsValid()
			&& !Snapshot->GetSnapshotId().IsEmpty();
	}

	const FString& FSnapshotCompositionRequest::GetTargetKind() const
	{
		return TargetKind;
	}

	const FString& FSnapshotCompositionRequest::GetProfileId() const
	{
		return ProfileId;
	}

	const FUeAgentCanonicalSnapshotRef&
	FSnapshotCompositionRequest::GetSnapshot() const
	{
		return Snapshot;
	}

	bool FSnapshotCompositionRequest::IsValid() const
	{
		return bValid;
	}

	bool TryComposeSnapshotAuthoringModel(
		const FSnapshotCompositionRequest& Request,
		ContextResource::FUeAgentResourceTracker& Resources,
		FAuthoringModelPtr& OutModel,
		FWorkspaceError& OutError)
	{
		OutModel.Reset();
		OutError.Reset();
		if (!Request.IsValid())
		{
			OutError.Set(TEXT("snapshot_composition_request_invalid"));
			return false;
		}
		FString ContractError;
		if (!RegisterGraphTreeTargetCompositionContracts(ContractError)
			|| !RegisterOtherTargetCompositionContracts(ContractError))
		{
			OutError.Set(
				TEXT("target_composition_contract_registration_failed"),
				ContractError);
			return false;
		}
		FCanonicalCompositionRequest Composition;
		Composition.TargetKind = Request.GetTargetKind();
		Composition.ProfileId = Request.GetProfileId();
		Composition.BaseRevision =
			Request.GetSnapshot()->GetRevision().GetGeneration();
		Composition.SourceProvenance =
			Request.GetSnapshot()->GetProvenance().GetSourceId()
			+ TEXT(":")
			+ Request.GetSnapshot()->GetProvenance().GetEvidenceId();
		Composition.Entries = Request.GetSnapshot()->GetEntries();
		return TryComposeCanonicalAuthoringModel(
			Composition,
			Resources,
			OutModel,
			OutError);
	}

	bool TryBuildSnapshotExportPlan(
		const FSnapshotCompositionRequest& Request,
		TMap<FString, int64> Telemetry,
		ContextResource::FUeAgentResourceTracker& Resources,
		FExportWorkspacePlanPtr& OutPlan,
		FWorkspaceError& OutError)
	{
		OutPlan.Reset();
		FAuthoringModelPtr Model;
		if (!TryComposeSnapshotAuthoringModel(
			Request,
			Resources,
			Model,
			OutError)
			|| !Model.IsValid())
		{
			return false;
		}
		return TryBuildCompositionExportPlan(
			Model.ToSharedRef(),
			{
				Request.GetSnapshot()->GetSnapshotId(),
				Request.GetSnapshot()->GetRevision(),
				Request.GetSnapshot()->GetProvenance()
					.GetEvidenceId(),
			},
			MoveTemp(Telemetry),
			Resources,
			OutPlan,
			OutError);
	}
}

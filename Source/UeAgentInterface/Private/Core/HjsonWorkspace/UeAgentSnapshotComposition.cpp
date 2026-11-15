#include "Core/HjsonWorkspace/UeAgentSnapshotComposition.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "Dom/JsonValue.h"

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
		if (TargetKind.IsEmpty() || ProfileId.IsEmpty())
		{
			ValidationDiagnostic = Contract::FDiagnostic::Failure(TEXT("snapshot_composition_request_invalid"),
				TargetKind.IsEmpty() ? TEXT("The snapshot composition request requires a target kind.") : TEXT("The snapshot composition request requires a profile identifier."), TEXT("snapshot_composition"));
			ValidationDiagnostic.PropertyPath = TargetKind.IsEmpty() ? TEXT("/target_kind") : TEXT("/profile_id");
			ValidationDiagnostic.ActualValue = MakeShared<FJsonValueString>(FString());
			ValidationDiagnostic.SourceKind = TEXT("native_result");
		}
		else if (!Snapshot->GetRevision().IsValid())
			ValidationDiagnostic = Contract::FDiagnostic::WrapFailure(TEXT("snapshot_composition_request_invalid"), Snapshot->GetRevision().GetValidationDiagnostic());
		else if (!Snapshot->GetProvenance().IsValid())
			ValidationDiagnostic = Contract::FDiagnostic::WrapFailure(TEXT("snapshot_composition_request_invalid"), Snapshot->GetProvenance().GetValidationDiagnostic());
		else if (Snapshot->GetSnapshotId().IsEmpty())
		{
			ValidationDiagnostic = Contract::FDiagnostic::Failure(TEXT("snapshot_composition_request_invalid"),
				TEXT("The snapshot composition request requires a source snapshot identifier."), TEXT("snapshot_composition"));
			ValidationDiagnostic.PropertyPath = TEXT("/snapshot/snapshot_id");
			ValidationDiagnostic.ActualValue = MakeShared<FJsonValueString>(FString());
			ValidationDiagnostic.SourceKind = TEXT("native_result");
		}
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
		return ValidationDiagnostic.IsEmpty();
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
			OutError.SetFailure(Request.GetValidationDiagnostic());
			return false;
		}
		UE::UAI::Contract::FDiagnostic ContractError;
		if (!RegisterGraphTreeTargetCompositionContracts(ContractError)
			|| !RegisterOtherTargetCompositionContracts(ContractError))
		{
			OutError.SetFailure(MoveTemp(ContractError));
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
		FWorkspaceError& OutError, FPreparedWorkspaceExport* ExportSettings)
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
		const FCompositionSnapshotBinding Binding{Request.GetSnapshot()->GetSnapshotId(),
			Request.GetSnapshot()->GetRevision(), Request.GetSnapshot()->GetProvenance().GetEvidenceId()};
		if (!TryPrepareCompositionExportState(Request.GetTargetKind(), *Model, Binding, Resources, ExportSettings, OutError)) return false;
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
			OutError, Request.GetTargetKind(), ExportSettings);
	}
}

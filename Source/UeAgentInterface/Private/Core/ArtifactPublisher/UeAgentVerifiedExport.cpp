#include "Core/ArtifactPublisher/UeAgentVerifiedExport.h"
#include "Core/HjsonWorkspace/UeAgentWorkspaceRefresh.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace UE::UAI::ArtifactPublisher
{
	namespace
	{
		FArtifactPublishError VerificationFailure(const TCHAR* Code, const FString& Reason,
			const FString& Target, const FString& Detail = FString())
		{
			auto Error = FArtifactPublishError::Failure(Code, Reason, TEXT("artifact_publish"), Detail);
			Error.SourceKind = TEXT("native_result");
			Error.TargetPath = Target;
			return Error;
		}

		void SetFallbackError(
			FArtifactPublishError& Error,
			const TCHAR* Code,
			const FString& Reason,
			const FString& Target,
			const FString& Detail = FString())
		{
			if (Error.IsEmpty())
			{
				Error = VerificationFailure(Code, Reason, Target, Detail);
			}
			else if (Error.TargetPath.IsEmpty()) Error.TargetPath = Target;
		}

		TSharedPtr<FJsonValue> RevisionEvidence(const FUeAgentStateRevision& Revision)
		{
			const auto Object = MakeShared<FJsonObject>();
			Object->SetStringField(TEXT("source_id"), Revision.GetSourceId());
			Object->SetStringField(TEXT("generation"), Revision.GetGeneration());
			Object->SetStringField(TEXT("sequence"), FString::Printf(TEXT("%lld"), Revision.GetSequence()));
			return MakeShared<FJsonValueObject>(Object);
		}

		FArtifactPublishError InvalidRevision(const FUeAgentStateRevision& Revision,
			const FString& Reason, const FString& Target)
		{
			auto Error = FArtifactPublishError::WrapFailure(TEXT("artifact_target_revision_invalid"),
				Revision.GetValidationDiagnostic(), TEXT("artifact_publish"), Reason);
			Error.SourceKind = TEXT("native_result");
			Error.TargetPath = Target;
			return Error;
		}

		FArtifactPublishError RevisionMismatch(const TCHAR* Code, const FString& Reason,
			const FUeAgentStateRevision& Actual, const FUeAgentStateRevision& Expected, const FString& Target)
		{
			auto Error = VerificationFailure(Code, Reason, Target);
			Error.ActualEvidence = RevisionEvidence(Actual);
			Error.ExpectedEvidence = RevisionEvidence(Expected);
			return Error;
		}

		class FVerifiedPublishVerification final
			: public IArtifactPublishVerification
		{
		public:
			FVerifiedPublishVerification(
				TSharedRef<IVerifiedExportWorkspaceSource> InSource,
				FUeAgentStateRevision InFrozenRevision,
				FString InTargetPath)
				: Source(MoveTemp(InSource))
				, FrozenRevision(MoveTemp(InFrozenRevision))
				, TargetPath(MoveTemp(InTargetPath))
			{
			}

			virtual bool TryRecheckTargetVersion(
				FArtifactPublishError& OutError) override
			{
				FUeAgentStateRevision Current(TEXT(""), TEXT(""), -1);
				if (!Source->TryReadCurrentTargetRevision(Current, OutError))
				{
					SetFallbackError(
						OutError,
						TEXT("artifact_target_version_recheck_failed"),
						TEXT("The target revision could not be read again before publishing the workspace."), TargetPath);
					return false;
				}
				if (!Current.IsValid())
				{
					OutError = InvalidRevision(Current,
						TEXT("The target revision read before publication is invalid."), TargetPath);
					return false;
				}
				if (Current != FrozenRevision)
				{
					OutError = RevisionMismatch(TEXT("artifact_target_version_conflict"),
						TEXT("The target changed after workspace capture; the captured export cannot be published as current."),
						Current, FrozenRevision, TargetPath);
					return false;
				}
				return true;
			}

		private:
			TSharedRef<IVerifiedExportWorkspaceSource> Source;
			FUeAgentStateRevision FrozenRevision;
			FString TargetPath;
		};
	}

	FVerifiedExportRequest::FVerifiedExportRequest(
		TSharedRef<IVerifiedExportWorkspaceSource> InSource,
		FString InFinalPath,
		FString InPublisherOwnerId)
		: Source(MoveTemp(InSource))
		, FinalPath(MoveTemp(InFinalPath))
		, PublisherOwnerId(MoveTemp(InPublisherOwnerId))
	{
		FinalPath.TrimStartAndEndInline();
		PublisherOwnerId.TrimStartAndEndInline();
		if (FinalPath.IsEmpty() || PublisherOwnerId.IsEmpty())
		{
			ValidationDiagnostic = VerificationFailure(TEXT("artifact_verified_export_request_invalid"),
				FinalPath.IsEmpty() ? TEXT("The verified export destination is empty.")
					: TEXT("The verified export has no publisher owner identity."), FinalPath);
			if (FinalPath.IsEmpty())
			{
				ValidationDiagnostic.ActualValue = MakeShared<FJsonValueString>(FinalPath);
				ValidationDiagnostic.ExpectedValue = MakeShared<FJsonValueString>(TEXT("non-empty workspace destination"));
			}
			else ValidationDiagnostic.ActualEvidence = MakeShared<FJsonValueString>(PublisherOwnerId);
		}
	}

	const TSharedRef<IVerifiedExportWorkspaceSource>&
	FVerifiedExportRequest::GetSource() const
	{
		return Source;
	}

	const FString& FVerifiedExportRequest::GetFinalPath() const
	{
		return FinalPath;
	}

	const FString& FVerifiedExportRequest::GetPublisherOwnerId() const
	{
		return PublisherOwnerId;
	}

	bool FVerifiedExportRequest::IsValid() const
	{
		return ValidationDiagnostic.IsEmpty();
	}

	const FArtifactPublishError& FVerifiedExportRequest::GetValidationDiagnostic() const
	{
		return ValidationDiagnostic;
	}

	bool TryExportVerifiedWorkspace(
		const FVerifiedExportRequest& Request,
		FUeAgentResourceTracker& Resources,
		FArtifactPublishReceipt& OutReceipt,
		FArtifactPublishError& OutError)
	{
		OutReceipt = {};
		OutError = {};
		if (!Request.IsValid())
		{
			OutError = Request.GetValidationDiagnostic();
			return false;
		}

		FUeAgentStateRevision Frozen(TEXT(""), TEXT(""), -1);
		if (!Request.GetSource()->TryFreezeTarget(Frozen, OutError))
		{
			SetFallbackError(OutError, TEXT("artifact_target_freeze_failed"),
				TEXT("The export source could not capture a stable target revision."), Request.GetFinalPath());
			return false;
		}
		if (!Frozen.IsValid())
		{
			OutError = InvalidRevision(Frozen,
				TEXT("The export source returned an invalid revision when capturing the target."), Request.GetFinalPath());
			return false;
		}

		FExportWorkspacePlanPtr Plan;
		if (!Request.GetSource()->TryCaptureWorkspace(
				Frozen,
				Plan,
				OutError))
		{
			SetFallbackError(OutError, TEXT("artifact_capture_failed"),
				TEXT("The export source could not capture the workspace for the selected target revision."), Request.GetFinalPath());
			return false;
		}
		if (!Plan.IsValid())
		{
			OutError = VerificationFailure(TEXT("artifact_capture_invalid"),
				TEXT("Workspace capture reported success without producing an export plan."), Request.GetFinalPath());
			return false;
		}
		if (Plan->GetSnapshotRevision() != Frozen)
		{
			OutError = RevisionMismatch(TEXT("artifact_capture_revision_mismatch"),
				TEXT("The captured workspace belongs to a different target revision than the one selected for this export."),
				Plan->GetSnapshotRevision(), Frozen, Request.GetFinalPath());
			return false;
		}

		const TSharedRef<FVerifiedPublishVerification> Verification =
			MakeShared<FVerifiedPublishVerification>(
				Request.GetSource(),
				Frozen,
				Request.GetFinalPath());
		if (auto* Prepared = Request.GetWorkspacePreparation(); Prepared && Prepared->bPrepareOnly) {
			Prepared->Plan = Plan;
			Prepared->PublisherOwnerId = Request.GetPublisherOwnerId();
			Prepared->Verification = Verification;
			return true;
		}
		FArtifactPublishRequest PublishRequest(Plan.ToSharedRef(), Request.GetFinalPath(), Request.GetPublisherOwnerId(), Verification);
		HjsonWorkspace::ConfigureWorkspacePublication(Request.GetWorkspacePreparation(), PublishRequest);
		// Preserve the actual target-version check across the publication handoff.
		// Generated syntax belongs to its formatter, not a repeated staged rebuild.
		PublishRequest.SetVerification(Verification);
		return TryPublishExportPlan(
			PublishRequest,
			Resources,
			OutReceipt,
			OutError);
	}
}

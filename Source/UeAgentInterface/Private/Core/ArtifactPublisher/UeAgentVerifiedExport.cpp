#include "Core/ArtifactPublisher/UeAgentVerifiedExport.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace UE::UAI::ArtifactPublisher
{
	namespace
	{
		bool IsFingerprint(const FString& Value)
		{
			if (!Value.StartsWith(TEXT("sha256:")) || Value.Len() != 71)
			{
				return false;
			}
			for (int32 Index = 7; Index < Value.Len(); ++Index)
			{
				if (!FChar::IsHexDigit(Value[Index]))
				{
					return false;
				}
			}
			return true;
		}

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

		TSharedPtr<FJsonValue> SignatureEvidence(const FExportSemanticSignature& Signature)
		{
			const auto Object = MakeShared<FJsonObject>();
			for (const auto& Pair : Signature.ToReceiptFields()) Object->SetStringField(Pair.Key, Pair.Value);
			return MakeShared<FJsonValueObject>(Object);
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

		FArtifactPublishError SignatureMismatch(const FExportSemanticSignature& Actual,
			const FExportSemanticSignature& Expected, const FString& Target, const FString& StagedPath)
		{
			auto Error = VerificationFailure(TEXT("artifact_semantic_mismatch"),
				TEXT("The staged workspace differs from the captured export in the reported verification dimensions."),
				Target, StagedPath);
			TArray<TSharedPtr<FJsonValue>> Dimensions;
			if (Actual.GetInventory() != Expected.GetInventory()) Dimensions.Add(MakeShared<FJsonValueString>(TEXT("inventory")));
			if (Actual.GetReferenceClosure() != Expected.GetReferenceClosure()) Dimensions.Add(MakeShared<FJsonValueString>(TEXT("reference_closure")));
			if (Actual.GetContentDigests() != Expected.GetContentDigests()) Dimensions.Add(MakeShared<FJsonValueString>(TEXT("content_digests")));
			if (Actual.GetContractSemantic() != Expected.GetContractSemantic()) Dimensions.Add(MakeShared<FJsonValueString>(TEXT("contract_semantic")));
			if (Actual.GetBusinessSemantic() != Expected.GetBusinessSemantic()) Dimensions.Add(MakeShared<FJsonValueString>(TEXT("business_semantic")));
			const auto Facts = MakeShared<FJsonObject>();
			Facts->SetArrayField(TEXT("mismatched_dimensions"), MoveTemp(Dimensions));
			Error.ActualValue = MakeShared<FJsonValueObject>(Facts);
			Error.ActualEvidence = SignatureEvidence(Actual);
			Error.ExpectedEvidence = SignatureEvidence(Expected);
			return Error;
		}

		class FVerifiedPublishVerification final
			: public IArtifactPublishVerification
		{
		public:
			FVerifiedPublishVerification(
				TSharedRef<IVerifiedExportWorkspaceSource> InSource,
				FUeAgentStateRevision InFrozenRevision,
				FExportSemanticSignature InExpectedSemantic,
				FString InTargetPath)
				: Source(MoveTemp(InSource))
				, FrozenRevision(MoveTemp(InFrozenRevision))
				, ExpectedSemantic(MoveTemp(InExpectedSemantic))
				, TargetPath(MoveTemp(InTargetPath))
			{
			}

			virtual bool TryVerifyStagedWorkspace(
				const FString& StagedWorkspacePath,
				const EWorkspaceKind WorkspaceKind,
				const FString& EntryFile,
				FUeAgentResourceTracker& Resources,
				FArtifactPublishError& OutError) override
			{
				FExportSemanticSignature Rebuilt;
				if (!Source->TryRebuildStagedWorkspace(
						StagedWorkspacePath,
						WorkspaceKind,
						EntryFile,
						Resources,
						Rebuilt,
						OutError))
				{
					SetFallbackError(
						OutError,
						TEXT("artifact_semantic_rebuild_failed"),
						TEXT("The staged workspace could not be rebuilt for semantic verification."), TargetPath,
						StagedWorkspacePath);
					return false;
				}
				if (!Rebuilt.IsValid())
				{
					OutError = FArtifactPublishError::WrapFailure(TEXT("artifact_semantic_rebuild_invalid"),
						Rebuilt.GetValidationDiagnostic(), TEXT("artifact_publish"),
						TEXT("The staged workspace did not produce a valid semantic verification signature."));
					OutError.SourceKind = TEXT("native_result");
					OutError.TargetPath = TargetPath;
					OutError.InternalContext = StagedWorkspacePath;
					return false;
				}
				if (Rebuilt != ExpectedSemantic)
				{
					OutError = SignatureMismatch(Rebuilt, ExpectedSemantic, TargetPath, StagedWorkspacePath);
					return false;
				}
				bSemanticVerified = true;
				return true;
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
				bVersionRechecked = true;
				return true;
			}

			virtual TMap<FString, FString> GetSemanticVerificationFields()
				const override
			{
				return bSemanticVerified && bVersionRechecked
					? ExpectedSemantic.ToReceiptFields()
					: TMap<FString, FString>();
			}

		private:
			TSharedRef<IVerifiedExportWorkspaceSource> Source;
			FUeAgentStateRevision FrozenRevision;
			FExportSemanticSignature ExpectedSemantic;
			FString TargetPath;
			bool bSemanticVerified = false;
			bool bVersionRechecked = false;
		};
	}

	FExportSemanticSignature::FExportSemanticSignature()
		: FExportSemanticSignature(FString(), FString(), FString(), FString(), FString())
	{
	}

	FExportSemanticSignature::FExportSemanticSignature(
		FString InInventory,
		FString InReferenceClosure,
		FString InContentDigests,
		FString InContractSemantic,
		FString InBusinessSemantic)
		: Inventory(MoveTemp(InInventory))
		, ReferenceClosure(MoveTemp(InReferenceClosure))
		, ContentDigests(MoveTemp(InContentDigests))
		, ContractSemantic(MoveTemp(InContractSemantic))
		, BusinessSemantic(MoveTemp(InBusinessSemantic))
	{
		struct FCheck { const TCHAR* Name; const TCHAR* Label; const FString* Value; };
		const FCheck Checks[] = {
			{TEXT("inventory"), TEXT("inventory"), &Inventory},
			{TEXT("reference_closure"), TEXT("reference closure"), &ReferenceClosure},
			{TEXT("content_digests"), TEXT("content digest"), &ContentDigests},
			{TEXT("contract_semantic"), TEXT("contract semantic"), &ContractSemantic},
			{TEXT("business_semantic"), TEXT("business semantic"), &BusinessSemantic},
		};
		for (const FCheck& Check : Checks)
		{
			if (IsFingerprint(*Check.Value)) continue;
			ValidationDiagnostic = VerificationFailure(TEXT("artifact_semantic_signature_invalid"),
				FString::Printf(TEXT("The export %s fingerprint %s."), Check.Label,
					Check.Value->IsEmpty() ? TEXT("is missing") : TEXT("has an invalid format")), FString());
			ValidationDiagnostic.Source.InternalJsonPath = FString(TEXT("/semantic_signature/")) + Check.Name;
			ValidationDiagnostic.ActualEvidence = MakeShared<FJsonValueString>(*Check.Value);
			ValidationDiagnostic.ExpectedEvidence = MakeShared<FJsonValueString>(TEXT("sha256: followed by 64 hexadecimal characters"));
			break;
		}
	}

	const FString& FExportSemanticSignature::GetInventory() const
	{
		return Inventory;
	}

	const FString& FExportSemanticSignature::GetReferenceClosure() const
	{
		return ReferenceClosure;
	}

	const FString& FExportSemanticSignature::GetContentDigests() const
	{
		return ContentDigests;
	}

	const FString& FExportSemanticSignature::GetContractSemantic() const
	{
		return ContractSemantic;
	}

	const FString& FExportSemanticSignature::GetBusinessSemantic() const
	{
		return BusinessSemantic;
	}

	bool FExportSemanticSignature::IsValid() const
	{
		return ValidationDiagnostic.IsEmpty();
	}

	const FArtifactPublishError& FExportSemanticSignature::GetValidationDiagnostic() const
	{
		return ValidationDiagnostic;
	}

	bool FExportSemanticSignature::operator==(
		const FExportSemanticSignature& Other) const
	{
		return Inventory == Other.Inventory
			&& ReferenceClosure == Other.ReferenceClosure
			&& ContentDigests == Other.ContentDigests
			&& ContractSemantic == Other.ContractSemantic
			&& BusinessSemantic == Other.BusinessSemantic;
	}

	bool FExportSemanticSignature::operator!=(
		const FExportSemanticSignature& Other) const
	{
		return !(*this == Other);
	}

	TMap<FString, FString> FExportSemanticSignature::ToReceiptFields() const
	{
		return {
			{TEXT("inventory"), Inventory},
			{TEXT("reference_closure"), ReferenceClosure},
			{TEXT("content_digests"), ContentDigests},
			{TEXT("contract_semantic"), ContractSemantic},
			{TEXT("business_semantic"), BusinessSemantic},
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
		FExportSemanticSignature ExpectedSemantic;
		if (!Request.GetSource()->TryCaptureWorkspace(
				Frozen,
				Plan,
				ExpectedSemantic,
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
		if (!ExpectedSemantic.IsValid())
		{
			OutError = FArtifactPublishError::WrapFailure(TEXT("artifact_capture_invalid"),
				ExpectedSemantic.GetValidationDiagnostic(), TEXT("artifact_publish"),
				TEXT("Workspace capture did not produce a valid semantic verification signature."));
			OutError.SourceKind = TEXT("native_result");
			OutError.TargetPath = Request.GetFinalPath();
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
				ExpectedSemantic,
				Request.GetFinalPath());
		return TryPublishExportPlan(
			FArtifactPublishRequest(
				Plan.ToSharedRef(),
				Request.GetFinalPath(),
				Request.GetPublisherOwnerId(),
				Verification),
			Resources,
			OutReceipt,
			OutError);
	}
}

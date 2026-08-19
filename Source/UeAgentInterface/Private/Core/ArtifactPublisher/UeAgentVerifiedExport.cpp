#include "Core/ArtifactPublisher/UeAgentVerifiedExport.h"

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

		void SetFallbackError(
			FArtifactPublishError& Error,
			const TCHAR* Code,
			const FString& Detail = FString())
		{
			if (Error.GetCode().IsEmpty())
			{
				Error.Set(Code, FString(), Detail);
			}
		}

		class FVerifiedPublishVerification final
			: public IArtifactPublishVerification
		{
		public:
			FVerifiedPublishVerification(
				TSharedRef<IVerifiedExportWorkspaceSource> InSource,
				FUeAgentStateRevision InFrozenRevision,
				FExportSemanticSignature InExpectedSemantic)
				: Source(MoveTemp(InSource))
				, FrozenRevision(MoveTemp(InFrozenRevision))
				, ExpectedSemantic(MoveTemp(InExpectedSemantic))
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
						StagedWorkspacePath);
					return false;
				}
				if (!Rebuilt.IsValid())
				{
					OutError.Set(
						TEXT("artifact_semantic_rebuild_invalid"),
						StagedWorkspacePath);
					return false;
				}
				if (Rebuilt != ExpectedSemantic)
				{
					OutError.Set(
						TEXT("artifact_semantic_mismatch"),
						StagedWorkspacePath,
						TEXT("staged workspace differs from the captured semantic model"));
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
						TEXT("artifact_target_version_recheck_failed"));
					return false;
				}
				if (!Current.IsValid())
				{
					OutError.Set(TEXT("artifact_target_revision_invalid"));
					return false;
				}
				if (Current != FrozenRevision)
				{
					OutError.Set(
						TEXT("artifact_target_version_conflict"),
						FString(),
						TEXT("target revision changed after capture"));
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
			bool bSemanticVerified = false;
			bool bVersionRechecked = false;
		};
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
		return IsFingerprint(Inventory)
			&& IsFingerprint(ReferenceClosure)
			&& IsFingerprint(ContentDigests)
			&& IsFingerprint(ContractSemantic)
			&& IsFingerprint(BusinessSemantic);
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
		bValid = !FinalPath.IsEmpty() && !PublisherOwnerId.IsEmpty();
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
		return bValid;
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
			OutError.Set(
				TEXT("artifact_verified_export_request_invalid"),
				Request.GetFinalPath());
			return false;
		}

		FUeAgentStateRevision Frozen(TEXT(""), TEXT(""), -1);
		if (!Request.GetSource()->TryFreezeTarget(Frozen, OutError))
		{
			SetFallbackError(OutError, TEXT("artifact_target_freeze_failed"));
			return false;
		}
		if (!Frozen.IsValid())
		{
			OutError.Set(TEXT("artifact_target_revision_invalid"));
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
			SetFallbackError(OutError, TEXT("artifact_capture_failed"));
			return false;
		}
		if (!Plan.IsValid() || !ExpectedSemantic.IsValid())
		{
			OutError.Set(TEXT("artifact_capture_invalid"));
			return false;
		}
		if (Plan->GetSnapshotRevision() != Frozen)
		{
			OutError.Set(
				TEXT("artifact_capture_revision_mismatch"),
				FString(),
				TEXT("captured workspace is not bound to the frozen target revision"));
			return false;
		}

		const TSharedRef<FVerifiedPublishVerification> Verification =
			MakeShared<FVerifiedPublishVerification>(
				Request.GetSource(),
				Frozen,
				ExpectedSemantic);
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

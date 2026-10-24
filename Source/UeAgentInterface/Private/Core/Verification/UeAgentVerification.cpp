#include "Core/Verification/UeAgentVerification.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "Core/StateSemantic/UeAgentStateSemanticEvidence.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include <openssl/sha.h>

namespace UE::UAI::Verification
{
	using UE::UAI::AssetDurable::EDurableStatus;
	using UE::UAI::AssetDurable::ERecoveryAction;
	using UE::UAI::AssetDurable::FAssetDurableError;
	using UE::UAI::ContextResource::FUeAgentCancellationRequested;
	using UE::UAI::ContextResource::FUeAgentOperationDeadlineExceeded;
	using UE::UAI::Discovery::FUeAgentDiscoveryError;
	using UE::UAI::Discovery::FUeAgentStateReadRequest;
	using UE::UAI::StateSemantic::EUeAgentStatePlane;
	using UE::UAI::StateSemantic::FUeAgentCanonicalSnapshot;
	using UE::UAI::StateSemantic::FUeAgentStateSemanticError;
	using UE::UAI::StateSemantic::TryCompareSnapshots;

	namespace
	{
		FVerificationError VerificationFailure(const FString& Code, const FString& Reason, const FString& Path)
		{
			auto Error = FVerificationError::Failure(Code, Reason, TEXT("verification"));
			Error.PropertyPath = Path;
			return Error;
		}

		TSharedPtr<const FVerificationError> StoreValidationFailure(FVerificationError Error, const FString& Prefix = FString())
		{
			if (!Prefix.IsEmpty()) Error.PropertyPath = Prefix + (Error.PropertyPath.IsEmpty() ? FString()
				: (Error.PropertyPath.StartsWith(TEXT("/")) ? Error.PropertyPath : TEXT(".") + Error.PropertyPath));
			return MakeShared<FVerificationError>(MoveTemp(Error));
		}

		FVerificationError IdentityMismatch(const FString& Code, const FString& Reason, const FString& Path,
			const FUeAgentCanonicalIdentity& Actual, const FUeAgentCanonicalIdentity& Expected)
		{
			auto Error = VerificationFailure(Code, Reason, Path);
			Error.ActualEvidence = MakeShared<FJsonValueString>(Actual.GetCanonicalKey());
			Error.ExpectedEvidence = MakeShared<FJsonValueString>(Expected.GetCanonicalKey());
			return Error;
		}

		FVerificationError TargetFailure(const FString& Code, const FString& Reason, const FString& Path, const FVerificationTarget& Target)
		{
			auto Error = VerificationFailure(Code, Reason, Path);
			Error.TargetPath = Target.GetBinding().GetPublicRef();
			return Error;
		}

		void AppendTargetDiagnostic(FVerificationError& Aggregate, FVerificationError Diagnostic, const FVerificationTarget& Target)
		{
			if (Diagnostic.TargetPath.IsEmpty()) Diagnostic.TargetPath = Target.GetBinding().GetPublicRef();
			if (Aggregate.IsEmpty()) Aggregate = MoveTemp(Diagnostic);
			else Aggregate.Append(Diagnostic);
		}

		FString NormalizedText(FString Value)
		{
			Value.TrimStartAndEndInline();
			return Value;
		}

		void NormalizeTextArray(TArray<FString>& Values)
		{
			for (int32 Index = Values.Num() - 1; Index >= 0; --Index)
			{
				Values[Index] = NormalizedText(MoveTemp(Values[Index]));
				if (Values[Index].IsEmpty())
				{
					Values.RemoveAt(Index);
				}
			}
			Values.Sort();
			for (int32 Index = Values.Num() - 1; Index > 0; --Index)
			{
				if (Values[Index] == Values[Index - 1])
				{
					Values.RemoveAt(Index);
				}
			}
		}

		FString Sha256(const FString& Text)
		{
			const FTCHARToUTF8 Utf8(*Text);
			uint8 Digest[SHA256_DIGEST_LENGTH];
			SHA256(
				reinterpret_cast<const uint8*>(Utf8.Get()),
				Utf8.Length(),
				Digest);
			FString Hex;
			Hex.Reserve(SHA256_DIGEST_LENGTH * 2);
			for (const uint8 Byte : Digest)
			{
				Hex += FString::Printf(TEXT("%02x"), Byte);
			}
			return TEXT("verification:sha256:") + Hex;
		}

		FString BuildVerificationId(
			const FVerificationRequest& Request)
		{
			FString TargetKeys;
			for (const FVerificationTarget& Target : Request.GetTargets())
			{
				if (!TargetKeys.IsEmpty())
				{
					TargetKeys += TEXT("\n");
				}
				TargetKeys += Target.GetIdentity().GetCanonicalKey();
			}
			const FUeAgentDescriptorIdentity& Descriptor =
				Request.GetDescriptorIdentity();
			const FDurableReceipt& Receipt =
				Request.GetDurableReceipt();
			return Sha256(
				Request.GetOperationId()
				+ TEXT("\n")
				+ Descriptor.GetDescriptorId()
				+ TEXT("\n")
				+ FString::FromInt(Descriptor.GetVersion())
				+ TEXT("\n")
				+ Descriptor.GetDescriptorHash()
				+ TEXT("\n")
				+ Receipt.GetJournalId()
				+ TEXT("\n")
				+ Receipt.GetPlanFingerprint()
				+ TEXT("\n")
				+ Request.GetInventory().GetInventoryHash()
				+ TEXT("\n")
				+ Request.GetJournalOwnerId()
				+ TEXT("\n")
				+ FString::Printf(
					TEXT("%lld"),
					Request.GetFencingToken())
				+ TEXT("\n")
				+ TargetKeys);
		}

		TArray<FDurableArtifact> ArtifactsFor(
			const FVerificationRequest& Request,
			const FVerificationTarget& Target)
		{
			TSet<FString> ArtifactIds;
			for (const FString& ArtifactId : Target.GetArtifactIds())
			{
				ArtifactIds.Add(ArtifactId);
			}
			TArray<FDurableArtifact> Result;
			for (
				const FDurableArtifact& Artifact
				: Request.GetInventory().GetArtifacts())
			{
				if (ArtifactIds.Contains(Artifact.GetArtifactId()))
				{
					Result.Add(Artifact);
				}
			}
			return Result;
		}

		FVerificationOutcome MakeOutcome(
			const FString& VerificationId,
			const EVerificationStatus Status,
			const EEffectOutcome EffectOutcome,
			FVerificationError Diagnostic,
			TArray<FTargetVerification> Targets = {},
			TArray<FString> EvidenceIds = {},
			TOptional<FVerificationAcknowledgement> Acknowledgement = {},
			FVerificationError Cause = {})
		{
			NormalizeTextArray(EvidenceIds);
			if (!Cause.IsEmpty())
			{
				if (Diagnostic.Code == Cause.Code) Diagnostic = MoveTemp(Cause);
				else
				{
					Diagnostic.CauseCode = Cause.Code;
					Diagnostic.PublicCause = MakeShared<FVerificationError>(MoveTemp(Cause));
				}
			}
			Diagnostic.Schema = TEXT("uai.verification.diagnostic/1");
			Diagnostic.VerificationStatus = LexToString(Status);
			Diagnostic.EffectOutcome = LexToString(EffectOutcome);
			if (Status == EVerificationStatus::Verified) Diagnostic.Severity = Contract::ESeverity::Info;
			return FVerificationOutcome(VerificationId, Status, EffectOutcome, MoveTemp(Diagnostic), false,
				MoveTemp(Targets), MoveTemp(EvidenceIds), MoveTemp(Acknowledgement));
		}

		FVerificationError OracleDiagnostic(const FString& Stage, FVerificationError Cause)
		{
			auto Diagnostic = FVerificationError::Failure(TEXT("verification_oracle_unavailable"),
				TEXT("Independent readback is unavailable."), Stage);
			Diagnostic.Schema = TEXT("uai.verification.diagnostic/1");
			if (!Cause.IsEmpty())
			{
				Diagnostic.CauseCode = Cause.Code;
				Diagnostic.PublicCause = MakeShared<FVerificationError>(MoveTemp(Cause));
			}
			return Diagnostic;
		}



		FVerificationError ComparisonDiagnostic(const FUeAgentSemanticComparison& Comparison)
		{
			return StateSemantic::MakeComparisonDiagnostic(Comparison, TEXT("verification_semantic_mismatch"),
				TEXT("Expected and observed values differ."), TEXT("verification_compare"), TEXT("uai.verification.diagnostic/1"));
		}

		bool CanContinue(
			const FUeAgentOperationDeadline& Deadline,
			const FUeAgentCancellationToken& Cancellation,
			FVerificationError& OutDiagnostic)
		{
			FUeAgentCancellationRequested CancellationError;
			if (!Cancellation.TryCheckpoint(CancellationError))
			{
				OutDiagnostic = FVerificationError::Failure(TEXT("verification_cancelled"), TEXT("Verification was cancelled."), TEXT("verification"));
				OutDiagnostic.PublicCause = MakeShared<FVerificationError>(CancellationError);
				return false;
			}
			return true;
		}
	}









	FString LexToString(const EVerificationOracleKind Value)
	{
		switch (Value)
		{
		case EVerificationOracleKind::Persisted:
			return TEXT("persisted");
		case EVerificationOracleKind::Absence:
			return TEXT("absence");
		case EVerificationOracleKind::Reference:
			return TEXT("reference");
		default:
			return TEXT("unknown");
		}
	}

	FString LexToString(const EVerificationStatus Value)
	{
		switch (Value)
		{
		case EVerificationStatus::Verified:
			return TEXT("verified");
		case EVerificationStatus::Mismatch:
			return TEXT("mismatch");
		case EVerificationStatus::Unavailable:
			return TEXT("unavailable");
		default:
			return TEXT("unknown");
		}
	}

	FString LexToString(const EEffectOutcome Value)
	{
		switch (Value)
		{
		case EEffectOutcome::Complete:
			return TEXT("complete");
		case EEffectOutcome::RolledBack:
			return TEXT("rolled_back");
		case EEffectOutcome::Partial:
			return TEXT("partial");
		case EEffectOutcome::StateUnknown:
			return TEXT("state_unknown");
		default:
			return TEXT("unknown");
		}
	}

	FVerificationTarget::FVerificationTarget(
		FUeAgentCanonicalIdentity InIdentity,
		FUeAgentCandidateBinding InBinding,
		const EVerificationOracleKind InOracleKind,
		FUeAgentCanonicalSnapshotPtr InExpectedSnapshot,
		TArray<FString> InArtifactIds,
		TArray<FString> InExpectedReferenceKeys)
		: Identity(MoveTemp(InIdentity))
		, Binding(MoveTemp(InBinding))
		, OracleKind(InOracleKind)
		, ExpectedSnapshot(MoveTemp(InExpectedSnapshot))
		, ArtifactIds(MoveTemp(InArtifactIds))
		, ExpectedReferenceKeys(MoveTemp(InExpectedReferenceKeys))
	{
		NormalizeTextArray(ArtifactIds);
		NormalizeTextArray(ExpectedReferenceKeys);
		auto Reject = [this](const FString& Reason, const FString& Path)
		{
			ValidationDiagnostic = StoreValidationFailure(VerificationFailure(
				TEXT("verification_target_invalid"), Reason, Path));
		};
		if (!Identity.IsValid())
		{
			ValidationDiagnostic = StoreValidationFailure(Identity.GetValidationDiagnostic(), TEXT("identity"));
			return;
		}
		if (!Binding.IsValid())
		{
			ValidationDiagnostic = StoreValidationFailure(*Binding.GetValidationDiagnostic(), TEXT("binding"));
			return;
		}
		if (!(Binding.GetSummary().GetIdentity() == Identity))
		{
			ValidationDiagnostic = StoreValidationFailure(IdentityMismatch(TEXT("verification_target_invalid"),
				TEXT("The verification binding identifies a different target."), TEXT("binding.identity"),
				Binding.GetSummary().GetIdentity(), Identity));
			return;
		}
		if (ArtifactIds.IsEmpty())
		{
			Reject(TEXT("The verification target has no durable artifacts to observe."), TEXT("artifact_ids"));
			return;
		}
		if (OracleKind == EVerificationOracleKind::Persisted)
		{
			if (!ExpectedSnapshot.IsValid())
			{
				Reject(TEXT("Persisted verification requires an expected snapshot."), TEXT("expected_snapshot"));
				return;
			}
			if (ExpectedSnapshot->GetStatePlane() != EUeAgentStatePlane::Persisted)
			{
				auto Error = VerificationFailure(TEXT("verification_target_invalid"),
					TEXT("The expected verification snapshot must describe persisted state."), TEXT("expected_snapshot.state_plane"));
				Error.ActualValue = MakeShared<FJsonValueString>(Contract::LexToString(ExpectedSnapshot->GetStatePlane()));
				Error.ExpectedValue = MakeShared<FJsonValueString>(TEXT("persisted"));
				ValidationDiagnostic = StoreValidationFailure(MoveTemp(Error));
				return;
			}
			if (ExpectedSnapshot->GetProvenance().GetSourceKind() != TEXT("operation_expected_state"))
			{
				auto Error = VerificationFailure(TEXT("verification_target_invalid"),
					TEXT("The expected verification snapshot must originate from the operation's expected state."),
					TEXT("expected_snapshot.provenance.source_kind"));
				Error.ActualValue = MakeShared<FJsonValueString>(ExpectedSnapshot->GetProvenance().GetSourceKind());
				Error.ExpectedValue = MakeShared<FJsonValueString>(TEXT("operation_expected_state"));
				ValidationDiagnostic = StoreValidationFailure(MoveTemp(Error));
				return;
			}
			if (!ExpectedReferenceKeys.IsEmpty())
			{
				Reject(TEXT("Persisted verification cannot also specify expected reference keys."), TEXT("expected_reference_keys"));
				return;
			}
			bool bContainsIdentity = false;
			for (const StateSemantic::FUeAgentStateEntry& Entry : ExpectedSnapshot->GetEntries())
			{
				bContainsIdentity = bContainsIdentity || Entry.GetIdentity() == Identity;
			}
			if (!bContainsIdentity) Reject(TEXT("The expected snapshot contains no state for the verification target."), TEXT("expected_snapshot.entries"));
		}
		else
		{
			if (ExpectedSnapshot.IsValid()) Reject(TEXT("Absence and reference verification do not accept an expected snapshot."), TEXT("expected_snapshot"));
			else if (OracleKind == EVerificationOracleKind::Absence && !ExpectedReferenceKeys.IsEmpty())
				Reject(TEXT("Absence verification cannot specify expected reference keys."), TEXT("expected_reference_keys"));
		}
	}

	const FUeAgentCanonicalIdentity&
	FVerificationTarget::GetIdentity() const
	{
		return Identity;
	}

	const FUeAgentCandidateBinding&
	FVerificationTarget::GetBinding() const
	{
		return Binding;
	}

	EVerificationOracleKind
	FVerificationTarget::GetOracleKind() const
	{
		return OracleKind;
	}

	const FUeAgentCanonicalSnapshotPtr&
	FVerificationTarget::GetExpectedSnapshot() const
	{
		return ExpectedSnapshot;
	}

	const TArray<FString>&
	FVerificationTarget::GetArtifactIds() const
	{
		return ArtifactIds;
	}

	const TArray<FString>&
	FVerificationTarget::GetExpectedReferenceKeys() const
	{
		return ExpectedReferenceKeys;
	}

	bool FVerificationTarget::IsValid() const
	{
		return !ValidationDiagnostic.IsValid();
	}

	const FVerificationError* FVerificationTarget::GetValidationDiagnostic() const
	{
		return ValidationDiagnostic.Get();
	}

	FVerificationRequest::FVerificationRequest(
		FString InOperationId,
		FUeAgentDescriptorIdentity InDescriptorIdentity,
		FDurableReceipt InDurableReceipt,
		FDurableInventory InInventory,
		FString InJournalOwnerId,
		const int64 InFencingToken,
		TArray<FVerificationTarget> InTargets)
		: OperationId(NormalizedText(MoveTemp(InOperationId)))
		, DescriptorIdentity(MoveTemp(InDescriptorIdentity))
		, DurableReceipt(MoveTemp(InDurableReceipt))
		, Inventory(MoveTemp(InInventory))
		, JournalOwnerId(
			NormalizedText(MoveTemp(InJournalOwnerId)))
		, FencingToken(InFencingToken)
		, Targets(MoveTemp(InTargets))
	{
		Targets.Sort(
			[](const FVerificationTarget& Left,
				const FVerificationTarget& Right)
			{
				return Left.GetIdentity().GetCanonicalKey()
					< Right.GetIdentity().GetCanonicalKey();
			});
		auto Reject = [this](const FString& Reason, const FString& Path)
		{
			ValidationDiagnostic = StoreValidationFailure(VerificationFailure(TEXT("verification_request_invalid"), Reason, Path));
		};
		if (OperationId.IsEmpty()) { Reject(TEXT("The verification request has no operation identifier."), TEXT("operation_id")); return; }
		if (!DescriptorIdentity.IsValid())
		{
			ValidationDiagnostic = StoreValidationFailure(DescriptorIdentity.GetValidationDiagnostic(), TEXT("descriptor"));
			return;
		}
		if (!Inventory.IsValid())
		{
			ValidationDiagnostic = StoreValidationFailure(*Inventory.GetValidationDiagnostic(), TEXT("inventory"));
			return;
		}
		if (JournalOwnerId.IsEmpty()) { Reject(TEXT("The verification request has no durable journal owner."), TEXT("journal_owner_id")); return; }
		if (FencingToken < 0)
		{
			auto Error = VerificationFailure(TEXT("verification_request_invalid"),
				TEXT("The verification fencing token must be non-negative."), TEXT("fencing_token"));
			Error.ActualEvidence = MakeShared<FJsonValueString>(FString::Printf(TEXT("%lld"), FencingToken));
			ValidationDiagnostic = StoreValidationFailure(MoveTemp(Error));
			return;
		}
		if (Targets.IsEmpty()) { Reject(TEXT("The verification request has no targets."), TEXT("targets")); return; }
		if (DurableReceipt.GetInventoryHash() != Inventory.GetInventoryHash())
		{
			auto Error = VerificationFailure(TEXT("verification_request_invalid"),
				TEXT("The durable receipt belongs to a different artifact inventory."), TEXT("durable_receipt.inventory_hash"));
			Error.ActualEvidence = MakeShared<FJsonValueString>(DurableReceipt.GetInventoryHash());
			Error.ExpectedEvidence = MakeShared<FJsonValueString>(Inventory.GetInventoryHash());
			ValidationDiagnostic = StoreValidationFailure(MoveTemp(Error));
			return;
		}
		TSet<FString> TargetKeys;
		TSet<FString> InventoryArtifactIds;
		for (const FDurableArtifact& Artifact : Inventory.GetArtifacts())
		{
			InventoryArtifactIds.Add(Artifact.GetArtifactId());
		}
		for (int32 TargetIndex = 0; TargetIndex < Targets.Num(); ++TargetIndex)
		{
			const FVerificationTarget& Target = Targets[TargetIndex];
			const FString TargetPath = FString::Printf(TEXT("targets[%d]"), TargetIndex);
			const FString& Key = Target.GetIdentity().GetCanonicalKey();
			if (!Target.IsValid())
			{
				auto Error = *Target.GetValidationDiagnostic();
				if (Error.TargetPath.IsEmpty()) Error.TargetPath = Target.GetBinding().GetPublicRef();
				ValidationDiagnostic = StoreValidationFailure(MoveTemp(Error), TargetPath);
				return;
			}
			if (TargetKeys.Contains(Key))
			{
				auto Error = VerificationFailure(TEXT("verification_request_invalid"),
					TEXT("The verification request contains the same target more than once."), TargetPath + TEXT(".identity"));
				Error.ActualEvidence = MakeShared<FJsonValueString>(Key);
				ValidationDiagnostic = StoreValidationFailure(MoveTemp(Error));
				return;
			}
			TargetKeys.Add(Key);
			for (int32 ArtifactIndex = 0; ArtifactIndex < Target.GetArtifactIds().Num(); ++ArtifactIndex)
			{
				const FString& ArtifactId = Target.GetArtifactIds()[ArtifactIndex];
				if (!InventoryArtifactIds.Contains(ArtifactId))
				{
					auto Error = VerificationFailure(TEXT("verification_request_invalid"),
						TEXT("The verification target refers to an artifact that is absent from the durable inventory."),
						TargetPath + FString::Printf(TEXT(".artifact_ids[%d]"), ArtifactIndex));
					Error.ActualEvidence = MakeShared<FJsonValueString>(ArtifactId);
					ValidationDiagnostic = StoreValidationFailure(MoveTemp(Error));
					return;
				}
			}
		}
	}

	const FString& FVerificationRequest::GetOperationId() const
	{
		return OperationId;
	}

	const FUeAgentDescriptorIdentity&
	FVerificationRequest::GetDescriptorIdentity() const
	{
		return DescriptorIdentity;
	}

	const FDurableReceipt&
	FVerificationRequest::GetDurableReceipt() const
	{
		return DurableReceipt;
	}

	const FDurableInventory&
	FVerificationRequest::GetInventory() const
	{
		return Inventory;
	}

	const FString& FVerificationRequest::GetJournalOwnerId() const
	{
		return JournalOwnerId;
	}

	int64 FVerificationRequest::GetFencingToken() const
	{
		return FencingToken;
	}

	const TArray<FVerificationTarget>&
	FVerificationRequest::GetTargets() const
	{
		return Targets;
	}

	bool FVerificationRequest::IsValid() const
	{
		return !ValidationDiagnostic.IsValid();
	}

	const FVerificationError* FVerificationRequest::GetValidationDiagnostic() const
	{
		return ValidationDiagnostic.Get();
	}

	FAbsenceObservation::FAbsenceObservation()
		: FAbsenceObservation(FUeAgentCanonicalIdentity(TEXT(""), TEXT(""), TEXT(""), TEXT(""), TEXT(""), TEXT("")), false, true, FString())
	{
	}

	FAbsenceObservation::FAbsenceObservation(
		FUeAgentCanonicalIdentity InIdentity,
		const bool bInAbsent,
		const bool bInUnavailable,
		FString InEvidenceId)
		: Identity(MoveTemp(InIdentity))
		, bAbsent(bInAbsent)
		, bUnavailable(bInUnavailable)
		, EvidenceId(NormalizedText(MoveTemp(InEvidenceId)))
	{
		if (!Identity.IsValid()) ValidationDiagnostic = StoreValidationFailure(Identity.GetValidationDiagnostic(), TEXT("observation.identity"));
		else if (bAbsent && bUnavailable)
		{
			auto Error = VerificationFailure(TEXT("verification_absence_observation_invalid"),
				TEXT("An absence observation cannot report both absent and unavailable."), TEXT("observation.absent"));
			Error.ActualValue = MakeShared<FJsonValueBoolean>(true);
			Error.ExpectedValue = MakeShared<FJsonValueBoolean>(false);
			ValidationDiagnostic = StoreValidationFailure(MoveTemp(Error));
		}
		else if (EvidenceId.IsEmpty()) ValidationDiagnostic = StoreValidationFailure(VerificationFailure(
			TEXT("verification_absence_observation_invalid"), TEXT("The absence observation has no evidence identifier."), TEXT("observation.evidence_id")));
	}

	const FUeAgentCanonicalIdentity&
	FAbsenceObservation::GetIdentity() const
	{
		return Identity;
	}

	bool FAbsenceObservation::IsAbsent() const
	{
		return bAbsent;
	}

	bool FAbsenceObservation::IsUnavailable() const
	{
		return bUnavailable;
	}

	const FString& FAbsenceObservation::GetEvidenceId() const
	{
		return EvidenceId;
	}

	bool FAbsenceObservation::IsValid() const
	{
		return !ValidationDiagnostic.IsValid();
	}

	const FVerificationError* FAbsenceObservation::GetValidationDiagnostic() const
	{
		return ValidationDiagnostic.Get();
	}

	FReferenceObservation::FReferenceObservation()
		: FReferenceObservation(FUeAgentCanonicalIdentity(TEXT(""), TEXT(""), TEXT(""), TEXT(""), TEXT(""), TEXT("")), {}, true, FString())
	{
	}

	FReferenceObservation::FReferenceObservation(
		FUeAgentCanonicalIdentity InIdentity,
		TArray<FString> InReferenceKeys,
		const bool bInUnavailable,
		FString InEvidenceId)
		: Identity(MoveTemp(InIdentity))
		, ReferenceKeys(MoveTemp(InReferenceKeys))
		, bUnavailable(bInUnavailable)
		, EvidenceId(NormalizedText(MoveTemp(InEvidenceId)))
	{
		NormalizeTextArray(ReferenceKeys);
		if (!Identity.IsValid()) ValidationDiagnostic = StoreValidationFailure(Identity.GetValidationDiagnostic(), TEXT("observation.identity"));
		else if (EvidenceId.IsEmpty()) ValidationDiagnostic = StoreValidationFailure(VerificationFailure(
			TEXT("verification_reference_observation_invalid"), TEXT("The reference observation has no evidence identifier."), TEXT("observation.evidence_id")));
	}

	const FUeAgentCanonicalIdentity&
	FReferenceObservation::GetIdentity() const
	{
		return Identity;
	}

	const TArray<FString>&
	FReferenceObservation::GetReferenceKeys() const
	{
		return ReferenceKeys;
	}

	bool FReferenceObservation::IsUnavailable() const
	{
		return bUnavailable;
	}

	const FString& FReferenceObservation::GetEvidenceId() const
	{
		return EvidenceId;
	}

	bool FReferenceObservation::IsValid() const
	{
		return !ValidationDiagnostic.IsValid();
	}

	const FVerificationError* FReferenceObservation::GetValidationDiagnostic() const
	{
		return ValidationDiagnostic.Get();
	}

	FTargetVerification::FTargetVerification(
		FUeAgentCanonicalIdentity InIdentity,
		const EVerificationOracleKind InOracleKind,
		const EVerificationStatus InStatus,
		FString InExpectedSnapshotId,
		FString InActualSnapshotId,
		FUeAgentSemanticComparison InComparison,
		const bool bInHasComparison,
		TArray<FString> InEvidenceIds)
		: Identity(MoveTemp(InIdentity))
		, OracleKind(InOracleKind)
		, Status(InStatus)
		, ExpectedSnapshotId(
			NormalizedText(MoveTemp(InExpectedSnapshotId)))
		, ActualSnapshotId(
			NormalizedText(MoveTemp(InActualSnapshotId)))
		, Comparison(MoveTemp(InComparison))
		, bHasComparison(bInHasComparison)
		, EvidenceIds(MoveTemp(InEvidenceIds))
	{
		NormalizeTextArray(EvidenceIds);
	}

	const FUeAgentCanonicalIdentity&
	FTargetVerification::GetIdentity() const
	{
		return Identity;
	}

	EVerificationOracleKind
	FTargetVerification::GetOracleKind() const
	{
		return OracleKind;
	}

	EVerificationStatus FTargetVerification::GetStatus() const
	{
		return Status;
	}

	const FString&
	FTargetVerification::GetExpectedSnapshotId() const
	{
		return ExpectedSnapshotId;
	}

	const FString&
	FTargetVerification::GetActualSnapshotId() const
	{
		return ActualSnapshotId;
	}

	const FUeAgentSemanticComparison&
	FTargetVerification::GetComparison() const
	{
		return Comparison;
	}

	bool FTargetVerification::HasComparison() const
	{
		return bHasComparison;
	}

	const TArray<FString>&
	FTargetVerification::GetEvidenceIds() const
	{
		return EvidenceIds;
	}

	FVerificationOutcome::FVerificationOutcome(
		FString InVerificationId,
		const EVerificationStatus InStatus,
		const EEffectOutcome InEffectOutcome,
		FVerificationError InDiagnostic,
		const bool bInRetrySafe,
		TArray<FTargetVerification> InTargets,
		TArray<FString> InEvidenceIds,
		TOptional<FVerificationAcknowledgement> InAcknowledgement,
		FUeAgentCanonicalValuePtr InPublicResult)
		: VerificationId(
			NormalizedText(MoveTemp(InVerificationId)))
		, Status(InStatus)
		, EffectOutcome(InEffectOutcome)
		, Diagnostic(MoveTemp(InDiagnostic))
		, bRetrySafe(bInRetrySafe)
		, Targets(MoveTemp(InTargets))
		, EvidenceIds(MoveTemp(InEvidenceIds))
		, Acknowledgement(MoveTemp(InAcknowledgement))
		, PublicResult(MoveTemp(InPublicResult))
	{
		NormalizeTextArray(EvidenceIds);
		if (Status != EVerificationStatus::Verified
			|| EffectOutcome != EEffectOutcome::Complete)
		{
			PublicResult.Reset();
		}
	}

	const FString& FVerificationOutcome::GetVerificationId() const
	{
		return VerificationId;
	}

	EVerificationStatus FVerificationOutcome::GetStatus() const
	{
		return Status;
	}

	EEffectOutcome FVerificationOutcome::GetEffectOutcome() const
	{
		return EffectOutcome;
	}

	const FString& FVerificationOutcome::GetDiagnosticCode() const
	{
		return Diagnostic.Code;
	}

	const FVerificationError& FVerificationOutcome::GetDiagnostic() const
	{
		return Diagnostic;
	}

	bool FVerificationOutcome::IsRetrySafe() const
	{
		return bRetrySafe;
	}

	const TArray<FTargetVerification>&
	FVerificationOutcome::GetTargets() const
	{
		return Targets;
	}

	const TArray<FString>&
	FVerificationOutcome::GetEvidenceIds() const
	{
		return EvidenceIds;
	}

	const FVerificationAcknowledgement*
	FVerificationOutcome::GetAcknowledgement() const
	{
		return Acknowledgement.IsSet()
			? &Acknowledgement.GetValue()
			: nullptr;
	}

	const FUeAgentCanonicalValuePtr&
	FVerificationOutcome::GetPublicResult() const
	{
		return PublicResult;
	}

	FStateReadPortPersistedStateOracle::
	FStateReadPortPersistedStateOracle(
		IUeAgentStateReadPort& InStateReadPort)
		: StateReadPort(&InStateReadPort)
	{
	}

	FStateReadPortPersistedStateOracle::
	FStateReadPortPersistedStateOracle(
		TSharedRef<
			IUeAgentStateReadPort,
			ESPMode::ThreadSafe> InStateReadPort)
		: OwnedStateReadPort(MoveTemp(InStateReadPort))
		, StateReadPort(OwnedStateReadPort.Get())
	{
	}

	const FString& FStateReadPortPersistedStateOracle::
	GetReadPortId() const
	{
		check(StateReadPort);
		return StateReadPort->GetPortId();
	}

	bool FStateReadPortPersistedStateOracle::TryObserve(
		const FVerificationTarget& Target,
		const TArray<FDurableArtifact>& Artifacts,
		FUeAgentResourceTracker& Resources,
		const FUeAgentOperationDeadline& Deadline,
		const FUeAgentCancellationToken& Cancellation,
		FUeAgentCanonicalSnapshotPtr& OutSnapshot,
		FVerificationError& OutError)
	{
		OutSnapshot.Reset();
		OutError = FVerificationError();
		if (!StateReadPort)
		{
			OutError = TargetFailure(TEXT("persisted_state_oracle_request_invalid"),
				TEXT("The persisted-state oracle has no state read port."), TEXT("read_port"), Target);
			return false;
		}
		if (!Target.IsValid())
		{
			OutError = *Target.GetValidationDiagnostic();
			if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Target.GetBinding().GetPublicRef();
			return false;
		}
		if (Artifacts.IsEmpty())
		{
			OutError = TargetFailure(TEXT("persisted_state_oracle_request_invalid"),
				TEXT("Persisted-state readback requires at least one durable artifact for the target."), TEXT("artifacts"), Target);
			return false;
		}
		if (StateReadPort->GetPortId() != Target.GetBinding().GetReadPortId())
		{
			OutError = TargetFailure(TEXT("persisted_state_oracle_request_invalid"),
				TEXT("The persisted-state oracle's read port differs from the target binding's read port."), TEXT("read_port_id"), Target);
			OutError.ActualEvidence = MakeShared<FJsonValueString>(StateReadPort->GetPortId());
			OutError.ExpectedEvidence = MakeShared<FJsonValueString>(Target.GetBinding().GetReadPortId());
			return false;
		}
		FUeAgentDiscoveryError ReadError;
		if (!StateReadPort->TryRead(
				FUeAgentStateReadRequest(
					Target.GetBinding(),
					EUeAgentStatePlane::Persisted),
				Resources,
				Deadline,
				Cancellation,
				OutSnapshot,
				ReadError))
		{
			OutError = ReadError.IsEmpty() ? TargetFailure(TEXT("persisted_state_oracle_read_failed"),
				TEXT("The persisted-state read port failed without reporting a diagnostic."), TEXT("observation"), Target) : ReadError;
			if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Target.GetBinding().GetPublicRef();
			return false;
		}
		return true;
	}

	FVerificationService::FVerificationService(
		IPersistedStateOracle& InPersistedStateOracle,
		IAbsenceOracle& InAbsenceOracle,
		IReferenceOracle& InReferenceOracle,
		IDurableVerificationAcknowledger& InAcknowledger)
		: PersistedStateOracle(&InPersistedStateOracle)
		, AbsenceOracle(&InAbsenceOracle)
		, ReferenceOracle(&InReferenceOracle)
		, Acknowledger(&InAcknowledger)
	{
	}

	bool FVerificationService::TryVerify(
		const FVerificationRequest& Request,
		FUeAgentResourceTracker& Resources,
		const FUeAgentOperationDeadline& Deadline,
		const FUeAgentCancellationToken& Cancellation,
		FVerificationOutcome& OutOutcome,
		FVerificationError& OutError,
		bool bObservationOnly) const
	{
		OutError = FVerificationError();
		if (!Request.IsValid())
		{
			OutError = *Request.GetValidationDiagnostic();
			return false;
		}
		if (PersistedStateOracle == nullptr
			|| AbsenceOracle == nullptr
			|| ReferenceOracle == nullptr
			|| Acknowledger == nullptr)
		{
			const FString Missing = PersistedStateOracle == nullptr ? TEXT("persisted_state_oracle")
				: AbsenceOracle == nullptr ? TEXT("absence_oracle") : ReferenceOracle == nullptr ? TEXT("reference_oracle") : TEXT("acknowledger");
			OutError = VerificationFailure(TEXT("verification_request_invalid"),
				TEXT("The verification service is missing its required ") + Missing + TEXT("."), Missing);
			return false;
		}

		const FString VerificationId = BuildVerificationId(Request);
		const FDurableReceipt& Receipt = Request.GetDurableReceipt();
		const bool bVerifiable =
			(Receipt.GetStatus()
				== EDurableStatus::PersistedUnverified
				&& Receipt.GetRecoveryAction()
					== ERecoveryAction::None)
			|| (Receipt.GetStatus()
				== EDurableStatus::RecoveryRequired
				&& Receipt.GetRecoveryAction()
					== ERecoveryAction::VerifyPersisted);
		if (!bVerifiable)
		{
			const bool bRolledBack = Receipt.GetStatus() == EDurableStatus::RolledBack;
			auto Diagnostic = VerificationFailure(!bObservationOnly && bRolledBack
				? TEXT("verification_durable_rolled_back") : TEXT("verification_durable_status_not_verifiable"),
				bRolledBack ? TEXT("The durable operation was rolled back, so its proposed persisted result cannot be verified.")
					: TEXT("Verification requires persisted_unverified with no recovery action, or recovery_required with verify_persisted."),
				TEXT("durable_receipt"));
			auto State = MakeShared<FJsonObject>();
			State->SetStringField(TEXT("status"), AssetDurable::LexToString(Receipt.GetStatus()));
			State->SetStringField(TEXT("recovery_action"), AssetDurable::LexToString(Receipt.GetRecoveryAction()));
			Diagnostic.ActualValue = MakeShared<FJsonValueObject>(State);
			if (bObservationOnly) {
				OutError = MoveTemp(Diagnostic);
				return false;
			}
			OutOutcome = MakeOutcome(VerificationId,
				EVerificationStatus::Unavailable,
				bRolledBack
					? EEffectOutcome::RolledBack
					: EEffectOutcome::StateUnknown,
				MoveTemp(Diagnostic));
			return true;
		}
		if (!Receipt.RequiresIndependentVerification())
		{
			OutOutcome = MakeOutcome(VerificationId,
				EVerificationStatus::Unavailable,
				EEffectOutcome::StateUnknown,
				VerificationFailure(TEXT("verification_independent_readback_not_required"),
					TEXT("The durable receipt does not request independent verification."), TEXT("durable_receipt.requires_independent_verification")));
			return true;
		}

		TArray<FTargetVerification> TargetResults;
		TArray<FString> EvidenceIds;
		FVerificationError UnavailableDiagnostic;
		FVerificationError MismatchDiagnostic;
		bool bUnavailable = false;
		bool bMismatch = false;
		auto AppendObservedDiagnostics = [&](FVerificationError& Diagnostic)
		{
			if (!UnavailableDiagnostic.IsEmpty()) Diagnostic.Append(UnavailableDiagnostic);
			if (!MismatchDiagnostic.IsEmpty()) Diagnostic.Append(MismatchDiagnostic);
		};
		for (const FVerificationTarget& Target : Request.GetTargets())
		{
			FVerificationError CheckpointDiagnostic;
			if (!CanContinue(
				Deadline,
				Cancellation,
				CheckpointDiagnostic))
			{
				AppendObservedDiagnostics(CheckpointDiagnostic);
				if (bObservationOnly) {
					OutError = CheckpointDiagnostic;
					return false;
				}
				OutOutcome = MakeOutcome(VerificationId,
				EVerificationStatus::Unavailable,
				EEffectOutcome::StateUnknown,
				CheckpointDiagnostic,
				MoveTemp(TargetResults),
				MoveTemp(EvidenceIds));
				return true;
			}

			if (Target.GetOracleKind()
				== EVerificationOracleKind::Persisted)
			{
				FUeAgentCanonicalSnapshotPtr Actual;
				FVerificationError OracleError;
				if (PersistedStateOracle->GetReadPortId()
					!= Target.GetBinding().GetReadPortId())
				{
					auto Error = TargetFailure(TEXT("verification_oracle_read_port_mismatch"),
						TEXT("The persisted-state oracle's read port differs from the target binding's read port."), TEXT("read_port_id"), Target);
					Error.ActualEvidence = MakeShared<FJsonValueString>(PersistedStateOracle->GetReadPortId());
					Error.ExpectedEvidence = MakeShared<FJsonValueString>(Target.GetBinding().GetReadPortId());
					AppendTargetDiagnostic(UnavailableDiagnostic, OracleDiagnostic(TEXT("persisted_read_port"), MoveTemp(Error)), Target);
					bUnavailable = true;
					break;
				}
				if (!PersistedStateOracle->TryObserve(
						Target,
						ArtifactsFor(Request, Target),
						Resources,
						Deadline,
						Cancellation,
						Actual,
						OracleError))
				{
					if (OracleError.IsEmpty()) OracleError = TargetFailure(TEXT("persisted_state_oracle_read_failed"),
						TEXT("The persisted-state oracle failed without reporting a diagnostic."), TEXT("observation"), Target);
					if (bObservationOnly) {
						OutError = OracleError;
						if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Target.GetBinding().GetPublicRef();
						AppendObservedDiagnostics(OutError);
						return false;
					}
					AppendTargetDiagnostic(UnavailableDiagnostic, OracleDiagnostic(TEXT("persisted_observe"), MoveTemp(OracleError)), Target);
					bUnavailable = true;
					break;
				}
				if (!Actual.IsValid()
					|| Actual->GetProvenance().GetSourceKind()
						!= TEXT("independent_readback"))
				{
					auto Error = TargetFailure(TEXT("verification_oracle_snapshot_invalid"),
						!Actual.IsValid() ? TEXT("The persisted-state oracle returned no snapshot.")
							: TEXT("The observed snapshot does not identify its source as independent readback."),
						!Actual.IsValid() ? TEXT("actual_snapshot") : TEXT("actual_snapshot.provenance.source_kind"), Target);
					if (Actual.IsValid())
					{
						Error.ActualValue = MakeShared<FJsonValueString>(Actual->GetProvenance().GetSourceKind());
						Error.ExpectedValue = MakeShared<FJsonValueString>(TEXT("independent_readback"));
					}
					AppendTargetDiagnostic(UnavailableDiagnostic, OracleDiagnostic(TEXT("persisted_snapshot"), MoveTemp(Error)), Target);
					bUnavailable = true;
					break;
				}
				const FUeAgentCanonicalSnapshotPtr& Expected =
					Target.GetExpectedSnapshot();
				if (!Expected.IsValid()
					|| Actual->GetSnapshotId()
						== Expected->GetSnapshotId()
					|| Actual->GetProvenance().GetEvidenceId()
						== Expected->GetProvenance().GetEvidenceId())
				{
					const bool bSameSnapshot = Expected.IsValid() && Actual->GetSnapshotId() == Expected->GetSnapshotId();
					auto Error = TargetFailure(TEXT("verification_oracle_evidence_not_independent"),
						!Expected.IsValid() ? TEXT("Independent verification has no expected snapshot to compare against.")
							: bSameSnapshot ? TEXT("Observed and expected states reuse the same snapshot identifier.")
							: TEXT("Observed and expected states reuse the same evidence identifier."),
						!Expected.IsValid() ? TEXT("expected_snapshot") : bSameSnapshot
							? TEXT("actual_snapshot.snapshot_id") : TEXT("actual_snapshot.provenance.evidence_id"), Target);
					if (Expected.IsValid())
					{
						Error.ActualEvidence = MakeShared<FJsonValueString>(bSameSnapshot ? Actual->GetSnapshotId() : Actual->GetProvenance().GetEvidenceId());
						Error.ExpectedEvidence = MakeShared<FJsonValueString>(bSameSnapshot ? Expected->GetSnapshotId() : Expected->GetProvenance().GetEvidenceId());
					}
					AppendTargetDiagnostic(UnavailableDiagnostic, OracleDiagnostic(TEXT("persisted_independence"), MoveTemp(Error)), Target);
					bUnavailable = true;
					break;
				}
				FUeAgentSemanticComparison Comparison;
				FUeAgentStateSemanticError CompareError;
				if (!TryCompareSnapshots(
					Expected.ToSharedRef(),
					Actual.ToSharedRef(),
					Resources,
					Comparison,
					CompareError))
				{
					AppendTargetDiagnostic(UnavailableDiagnostic, OracleDiagnostic(TEXT("persisted_compare"), MoveTemp(CompareError)), Target);
					bUnavailable = true;
					break;
				}
				const EVerificationStatus TargetStatus =
					Comparison.IsEquivalent()
						? EVerificationStatus::Verified
						: EVerificationStatus::Mismatch;
				if (TargetStatus == EVerificationStatus::Mismatch)
				{
					AppendTargetDiagnostic(MismatchDiagnostic, ComparisonDiagnostic(Comparison), Target);
				}
				bMismatch = bMismatch
					|| TargetStatus == EVerificationStatus::Mismatch;
				const FString EvidenceId =
					Actual->GetProvenance().GetEvidenceId();
				EvidenceIds.Add(EvidenceId);
				TargetResults.Emplace(
					Target.GetIdentity(),
					Target.GetOracleKind(),
					TargetStatus,
					Expected->GetSnapshotId(),
					Actual->GetSnapshotId(),
					MoveTemp(Comparison),
					true,
					TArray<FString>{EvidenceId});
				continue;
			}

			if (Target.GetOracleKind()
				== EVerificationOracleKind::Absence)
			{
				FAbsenceObservation Observation;
				FVerificationError OracleError;
				const bool bObserved = AbsenceOracle->TryObserve(
					Target.GetIdentity(),
					ArtifactsFor(Request, Target),
					Resources,
					Deadline,
					Cancellation,
					Observation,
					OracleError);
				if (!bObserved && OracleError.IsEmpty()) OracleError = TargetFailure(TEXT("verification_absence_read_failed"),
					TEXT("The absence oracle failed without reporting a diagnostic."), TEXT("observation"), Target);
				if (!bObserved && bObservationOnly) {
					OutError = OracleError;
					if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Target.GetBinding().GetPublicRef();
					AppendObservedDiagnostics(OutError);
					return false;
				}
				if (!bObserved || !Observation.IsValid()
					|| !(Observation.GetIdentity()
						== Target.GetIdentity()))
				{
					if (OracleError.IsEmpty()) OracleError = !Observation.IsValid() ? *Observation.GetValidationDiagnostic()
						: IdentityMismatch(TEXT("verification_absence_observation_invalid"),
							TEXT("The absence oracle returned an observation for a different target."), TEXT("observation.identity"),
							Observation.GetIdentity(), Target.GetIdentity());
					AppendTargetDiagnostic(UnavailableDiagnostic, OracleDiagnostic(TEXT("absence_observe"), MoveTemp(OracleError)), Target);
					bUnavailable = true;
					break;
				}
				const EVerificationStatus TargetStatus =
					Observation.IsUnavailable()
						? EVerificationStatus::Unavailable
						: (Observation.IsAbsent()
							? EVerificationStatus::Verified
							: EVerificationStatus::Mismatch);
				if (TargetStatus == EVerificationStatus::Unavailable)
				{
					auto Error = TargetFailure(TEXT("verification_absence_unavailable"),
						TEXT("The absence oracle could not determine whether the target still exists."), TEXT("observation.unavailable"), Target);
					Error.ActualValue = MakeShared<FJsonValueBoolean>(true);
					Error.ExpectedValue = MakeShared<FJsonValueBoolean>(false);
					AppendTargetDiagnostic(UnavailableDiagnostic, MoveTemp(Error), Target);
				}
				else if (TargetStatus == EVerificationStatus::Mismatch)
				{
					auto Error = TargetFailure(TEXT("verification_absence_mismatch"),
						TEXT("The target is still present, but the operation expected it to be absent."), TEXT("observation.absent"), Target);
					Error.ActualValue = MakeShared<FJsonValueBoolean>(false);
					Error.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
					AppendTargetDiagnostic(MismatchDiagnostic, MoveTemp(Error), Target);
				}
				bUnavailable = bUnavailable
					|| TargetStatus == EVerificationStatus::Unavailable;
				bMismatch = bMismatch
					|| TargetStatus == EVerificationStatus::Mismatch;
				EvidenceIds.Add(Observation.GetEvidenceId());
				TargetResults.Emplace(
					Target.GetIdentity(),
					Target.GetOracleKind(),
					TargetStatus,
					FString(),
					FString(),
					FUeAgentSemanticComparison(),
					false,
					TArray<FString>{Observation.GetEvidenceId()});
				continue;
			}

			FReferenceObservation Observation;
			FVerificationError OracleError;
			const bool bObserved = ReferenceOracle->TryObserve(
				Target.GetIdentity(),
				Resources,
				Deadline,
				Cancellation,
				Observation,
				OracleError);
			if (!bObserved && OracleError.IsEmpty()) OracleError = TargetFailure(TEXT("verification_reference_read_failed"),
				TEXT("The reference oracle failed without reporting a diagnostic."), TEXT("observation"), Target);
			if (!bObserved && bObservationOnly) {
				OutError = OracleError;
				if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Target.GetBinding().GetPublicRef();
				AppendObservedDiagnostics(OutError);
				return false;
			}
			if (!bObserved || !Observation.IsValid()
				|| !(Observation.GetIdentity()
					== Target.GetIdentity()))
			{
				if (OracleError.IsEmpty()) OracleError = !Observation.IsValid() ? *Observation.GetValidationDiagnostic()
					: IdentityMismatch(TEXT("verification_reference_observation_invalid"),
						TEXT("The reference oracle returned an observation for a different target."), TEXT("observation.identity"),
						Observation.GetIdentity(), Target.GetIdentity());
				AppendTargetDiagnostic(UnavailableDiagnostic, OracleDiagnostic(TEXT("reference_observe"), MoveTemp(OracleError)), Target);
				bUnavailable = true;
				break;
			}
			const EVerificationStatus TargetStatus =
				Observation.IsUnavailable()
					? EVerificationStatus::Unavailable
					: (Observation.GetReferenceKeys()
						== Target.GetExpectedReferenceKeys()
						? EVerificationStatus::Verified
						: EVerificationStatus::Mismatch);
			if (TargetStatus == EVerificationStatus::Unavailable)
			{
				auto Error = TargetFailure(TEXT("verification_reference_unavailable"),
					TEXT("The reference oracle could not determine the target's reference set."), TEXT("observation.unavailable"), Target);
				Error.ActualValue = MakeShared<FJsonValueBoolean>(true);
				Error.ExpectedValue = MakeShared<FJsonValueBoolean>(false);
				AppendTargetDiagnostic(UnavailableDiagnostic, MoveTemp(Error), Target);
			}
			else if (TargetStatus == EVerificationStatus::Mismatch)
			{
				auto Error = TargetFailure(TEXT("verification_reference_mismatch"),
					TEXT("The observed reference set differs from the operation's expected reference set."), TEXT("observation.reference_keys"), Target);
				TArray<TSharedPtr<FJsonValue>> ActualKeys, ExpectedKeys;
				for (const FString& Key : Observation.GetReferenceKeys()) ActualKeys.Add(MakeShared<FJsonValueString>(Key));
				for (const FString& Key : Target.GetExpectedReferenceKeys()) ExpectedKeys.Add(MakeShared<FJsonValueString>(Key));
				Error.ActualEvidence = MakeShared<FJsonValueArray>(MoveTemp(ActualKeys));
				Error.ExpectedEvidence = MakeShared<FJsonValueArray>(MoveTemp(ExpectedKeys));
				AppendTargetDiagnostic(MismatchDiagnostic, MoveTemp(Error), Target);
			}
			bUnavailable = bUnavailable
				|| TargetStatus == EVerificationStatus::Unavailable;
			bMismatch = bMismatch
				|| TargetStatus == EVerificationStatus::Mismatch;
			EvidenceIds.Add(Observation.GetEvidenceId());
			TargetResults.Emplace(
				Target.GetIdentity(),
				Target.GetOracleKind(),
				TargetStatus,
				FString(),
				FString(),
				FUeAgentSemanticComparison(),
				false,
				TArray<FString>{Observation.GetEvidenceId()});
		}

		if (bUnavailable && !bObservationOnly)
		{
			if (!MismatchDiagnostic.IsEmpty()) UnavailableDiagnostic.Append(MismatchDiagnostic);
			OutOutcome = MakeOutcome(VerificationId,
				EVerificationStatus::Unavailable,
				EEffectOutcome::StateUnknown,
				VerificationFailure(TEXT("verification_oracle_unavailable"), TEXT("Independent verification could not observe every required target."), FString()),
				MoveTemp(TargetResults),
				MoveTemp(EvidenceIds),
				TOptional<FVerificationAcknowledgement>(),
				MoveTemp(UnavailableDiagnostic));
			return true;
		}
		if (bMismatch && !bObservationOnly)
		{
			OutOutcome = MakeOutcome(VerificationId,
				EVerificationStatus::Mismatch,
				EEffectOutcome::Partial,
				VerificationFailure(TEXT("verification_semantic_mismatch"), TEXT("Independent readback differs from the operation's expected result."), FString()),
				MoveTemp(TargetResults),
				MoveTemp(EvidenceIds),
				TOptional<FVerificationAcknowledgement>(),
				MoveTemp(MismatchDiagnostic));
			return true;
		}

		FVerificationError CheckpointDiagnostic;
		if (!CanContinue(
			Deadline,
			Cancellation,
			CheckpointDiagnostic))
		{
			AppendObservedDiagnostics(CheckpointDiagnostic);
			if (bObservationOnly) {
				OutError = CheckpointDiagnostic;
				return false;
			}
			OutOutcome = MakeOutcome(VerificationId,
				EVerificationStatus::Unavailable,
				EEffectOutcome::StateUnknown,
				CheckpointDiagnostic,
				MoveTemp(TargetResults),
				MoveTemp(EvidenceIds));
			return true;
		}
		const FVerificationAcknowledgement Acknowledgement(
			Receipt.GetJournalId(),
			Receipt.GetPlanFingerprint(),
			Receipt.GetInventoryHash(),
			VerificationId,
			Request.GetJournalOwnerId(),
			Request.GetFencingToken(), !bMismatch && !bUnavailable);
		FAssetDurableError AcknowledgeError;
		bool bAcknowledged = false;
		if (!Acknowledgement.IsValid()) AcknowledgeError = *Acknowledgement.GetValidationDiagnostic();
		else bAcknowledged = Acknowledger->TryAcknowledge(
				Acknowledgement,
				Resources,
				AcknowledgeError);
		if (!bAcknowledged)
		{
			if (AcknowledgeError.IsEmpty()) AcknowledgeError = VerificationFailure(TEXT("verification_acknowledgement_rejected"),
				TEXT("The durable verification acknowledgement was rejected without a diagnostic."), TEXT("acknowledgement"));
			AppendObservedDiagnostics(AcknowledgeError);
			if (bObservationOnly) {
				OutError = AcknowledgeError;
				return false;
			}
			OutOutcome = MakeOutcome(VerificationId,
				EVerificationStatus::Unavailable,
				EEffectOutcome::StateUnknown,
				VerificationFailure(TEXT("verification_acknowledgement_rejected"),
					TEXT("The durable operation did not accept the verification acknowledgement."), TEXT("acknowledgement")),
				MoveTemp(TargetResults),
				MoveTemp(EvidenceIds), {}, AcknowledgeError);
			return true;
		}
		if (bUnavailable && !MismatchDiagnostic.IsEmpty()) UnavailableDiagnostic.Append(MismatchDiagnostic);
		OutOutcome = MakeOutcome(VerificationId,
				bUnavailable ? EVerificationStatus::Unavailable : bMismatch ? EVerificationStatus::Mismatch : EVerificationStatus::Verified,
				bUnavailable ? EEffectOutcome::StateUnknown : bMismatch ? EEffectOutcome::Partial : EEffectOutcome::Complete,
				VerificationFailure(bUnavailable ? TEXT("verification_oracle_unavailable") : bMismatch ? TEXT("verification_semantic_mismatch") : TEXT("verification_complete"),
					bUnavailable ? TEXT("Independent verification could not observe every required target.")
						: bMismatch ? TEXT("Independent readback differs from the operation's expected result.")
						: TEXT("Independent readback matches the operation's expected result."), FString()),
				MoveTemp(TargetResults),
				MoveTemp(EvidenceIds),
				TOptional<FVerificationAcknowledgement>(
				Acknowledgement),
				bUnavailable ? MoveTemp(UnavailableDiagnostic) : MoveTemp(MismatchDiagnostic));
		return true;
	}
}

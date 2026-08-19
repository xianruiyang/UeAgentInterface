#include "Core/Verification/UeAgentVerification.h"

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
			const FString& DiagnosticCode,
			TArray<FTargetVerification> Targets = {},
			TArray<FString> EvidenceIds = {},
			TOptional<FVerificationAcknowledgement> Acknowledgement =
				TOptional<FVerificationAcknowledgement>())
		{
			NormalizeTextArray(EvidenceIds);
			return FVerificationOutcome(
				VerificationId,
				Status,
				EffectOutcome,
				DiagnosticCode,
				false,
				MoveTemp(Targets),
				MoveTemp(EvidenceIds),
				MoveTemp(Acknowledgement));
		}

		bool CanContinue(
			const FUeAgentOperationDeadline& Deadline,
			const FUeAgentCancellationToken& Cancellation,
			FString& OutDiagnostic)
		{
			FUeAgentCancellationRequested CancellationError;
			if (!Cancellation.TryCheckpoint(CancellationError))
			{
				OutDiagnostic = TEXT("verification_cancelled");
				return false;
			}
			FUeAgentOperationDeadlineExceeded DeadlineError;
			if (!Deadline.TryCheckpoint(
				TEXT("C9.Verification"),
				DeadlineError))
			{
				OutDiagnostic = TEXT("verification_deadline_exceeded");
				return false;
			}
			return true;
		}
	}

	FVerificationError::FVerificationError(
		FString InCode,
		FString InDetail)
		: Code(NormalizedText(MoveTemp(InCode)))
		, Detail(NormalizedText(MoveTemp(InDetail)))
	{
	}

	const FString& FVerificationError::GetCode() const
	{
		return Code;
	}

	const FString& FVerificationError::GetDetail() const
	{
		return Detail;
	}

	bool FVerificationError::IsPresent() const
	{
		return !Code.IsEmpty();
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
		bValid = Identity.IsValid()
			&& Binding.IsValid()
			&& Binding.GetSummary().GetIdentity() == Identity
			&& !ArtifactIds.IsEmpty();
		if (OracleKind == EVerificationOracleKind::Persisted)
		{
			bValid = bValid
				&& ExpectedSnapshot.IsValid()
				&& ExpectedSnapshot->GetStatePlane()
					== EUeAgentStatePlane::Persisted
				&& ExpectedSnapshot->GetProvenance()
					.GetSourceKind()
					== TEXT("operation_expected_state")
				&& ExpectedReferenceKeys.IsEmpty();
			bool bContainsIdentity = false;
			if (ExpectedSnapshot.IsValid())
			{
				for (
					const UE::UAI::StateSemantic::FUeAgentStateEntry& Entry
					: ExpectedSnapshot->GetEntries())
				{
					bContainsIdentity = bContainsIdentity
						|| Entry.GetIdentity() == Identity;
				}
			}
			bValid = bValid && bContainsIdentity;
		}
		else
		{
			bValid = bValid && !ExpectedSnapshot.IsValid();
			if (OracleKind == EVerificationOracleKind::Absence)
			{
				bValid = bValid && ExpectedReferenceKeys.IsEmpty();
			}
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
		return bValid;
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
		bValid = !OperationId.IsEmpty()
			&& DescriptorIdentity.IsValid()
			&& Inventory.IsValid()
			&& !JournalOwnerId.IsEmpty()
			&& FencingToken >= 0
			&& !Targets.IsEmpty()
			&& DurableReceipt.GetInventoryHash()
				== Inventory.GetInventoryHash();
		TSet<FString> TargetKeys;
		TSet<FString> InventoryArtifactIds;
		for (const FDurableArtifact& Artifact : Inventory.GetArtifacts())
		{
			InventoryArtifactIds.Add(Artifact.GetArtifactId());
		}
		for (const FVerificationTarget& Target : Targets)
		{
			const FString& Key = Target.GetIdentity().GetCanonicalKey();
			bValid = bValid
				&& Target.IsValid()
				&& !TargetKeys.Contains(Key);
			TargetKeys.Add(Key);
			for (const FString& ArtifactId : Target.GetArtifactIds())
			{
				bValid = bValid
					&& InventoryArtifactIds.Contains(ArtifactId);
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
		return bValid;
	}

	FAbsenceObservation::FAbsenceObservation()
		: Identity(
			TEXT(""),
			TEXT(""),
			TEXT(""),
			TEXT(""),
			TEXT(""),
			TEXT(""))
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
		, bValid(
			Identity.IsValid()
			&& !(bAbsent && bUnavailable)
			&& !EvidenceId.IsEmpty())
	{
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
		return bValid;
	}

	FReferenceObservation::FReferenceObservation()
		: Identity(
			TEXT(""),
			TEXT(""),
			TEXT(""),
			TEXT(""),
			TEXT(""),
			TEXT(""))
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
		bValid = Identity.IsValid() && !EvidenceId.IsEmpty();
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
		return bValid;
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
		FString InDiagnosticCode,
		const bool bInRetrySafe,
		TArray<FTargetVerification> InTargets,
		TArray<FString> InEvidenceIds,
		TOptional<FVerificationAcknowledgement> InAcknowledgement)
		: VerificationId(
			NormalizedText(MoveTemp(InVerificationId)))
		, Status(InStatus)
		, EffectOutcome(InEffectOutcome)
		, DiagnosticCode(
			NormalizedText(MoveTemp(InDiagnosticCode)))
		, bRetrySafe(bInRetrySafe)
		, Targets(MoveTemp(InTargets))
		, EvidenceIds(MoveTemp(InEvidenceIds))
		, Acknowledgement(MoveTemp(InAcknowledgement))
	{
		NormalizeTextArray(EvidenceIds);
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
		return DiagnosticCode;
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

	FVerificationService::FVerificationService(
		IUeAgentStateReadPort& InStateReadPort,
		IAbsenceOracle& InAbsenceOracle,
		IReferenceOracle& InReferenceOracle,
		IDurableVerificationAcknowledger& InAcknowledger)
		: StateReadPort(&InStateReadPort)
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
		FVerificationError& OutError) const
	{
		OutError = FVerificationError();
		if (!Request.IsValid()
			|| StateReadPort == nullptr
			|| AbsenceOracle == nullptr
			|| ReferenceOracle == nullptr
			|| Acknowledger == nullptr)
		{
			OutError = FVerificationError(
				TEXT("verification_request_invalid"));
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
			const bool bRolledBack = Receipt.GetStatus()
				== EDurableStatus::RolledBack;
			OutOutcome = MakeOutcome(
				VerificationId,
				EVerificationStatus::Unavailable,
				bRolledBack
					? EEffectOutcome::RolledBack
					: EEffectOutcome::StateUnknown,
				bRolledBack
					? TEXT("verification_durable_rolled_back")
					: TEXT(
						"verification_durable_status_not_verifiable"));
			return true;
		}
		if (!Receipt.RequiresIndependentVerification())
		{
			OutOutcome = MakeOutcome(
				VerificationId,
				EVerificationStatus::Unavailable,
				EEffectOutcome::StateUnknown,
				TEXT(
					"verification_independent_readback_not_required"));
			return true;
		}

		TArray<FTargetVerification> TargetResults;
		TArray<FString> EvidenceIds;
		bool bUnavailable = false;
		bool bMismatch = false;
		for (const FVerificationTarget& Target : Request.GetTargets())
		{
			FString CheckpointDiagnostic;
			if (!CanContinue(
				Deadline,
				Cancellation,
				CheckpointDiagnostic))
			{
				OutOutcome = MakeOutcome(
					VerificationId,
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
				FUeAgentDiscoveryError ReadError;
				const FUeAgentStateReadRequest ReadRequest(
					Target.GetBinding(),
					EUeAgentStatePlane::Persisted);
				if (StateReadPort->GetPortId()
						!= Target.GetBinding().GetReadPortId()
					|| !StateReadPort->TryRead(
						ReadRequest,
						Resources,
						Deadline,
						Cancellation,
						Actual,
						ReadError)
					|| !Actual.IsValid()
					|| Actual->GetProvenance().GetSourceKind()
						!= TEXT("independent_readback"))
				{
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
					bUnavailable = true;
					break;
				}
				const EVerificationStatus TargetStatus =
					Comparison.IsEquivalent()
						? EVerificationStatus::Verified
						: EVerificationStatus::Mismatch;
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
				if (!AbsenceOracle->TryObserve(
					Target.GetIdentity(),
					ArtifactsFor(Request, Target),
					Resources,
					Deadline,
					Cancellation,
					Observation,
					OracleError)
					|| !Observation.IsValid()
					|| !(Observation.GetIdentity()
						== Target.GetIdentity()))
				{
					bUnavailable = true;
					break;
				}
				const EVerificationStatus TargetStatus =
					Observation.IsUnavailable()
						? EVerificationStatus::Unavailable
						: (Observation.IsAbsent()
							? EVerificationStatus::Verified
							: EVerificationStatus::Mismatch);
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
			if (!ReferenceOracle->TryObserve(
				Target.GetIdentity(),
				Resources,
				Deadline,
				Cancellation,
				Observation,
				OracleError)
				|| !Observation.IsValid()
				|| !(Observation.GetIdentity()
					== Target.GetIdentity()))
			{
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

		if (bUnavailable)
		{
			OutOutcome = MakeOutcome(
				VerificationId,
				EVerificationStatus::Unavailable,
				EEffectOutcome::StateUnknown,
				TEXT("verification_oracle_unavailable"),
				MoveTemp(TargetResults),
				MoveTemp(EvidenceIds));
			return true;
		}
		if (bMismatch)
		{
			OutOutcome = MakeOutcome(
				VerificationId,
				EVerificationStatus::Mismatch,
				EEffectOutcome::Partial,
				TEXT("verification_semantic_mismatch"),
				MoveTemp(TargetResults),
				MoveTemp(EvidenceIds));
			return true;
		}

		FString CheckpointDiagnostic;
		if (!CanContinue(
			Deadline,
			Cancellation,
			CheckpointDiagnostic))
		{
			OutOutcome = MakeOutcome(
				VerificationId,
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
			Request.GetFencingToken());
		FAssetDurableError AcknowledgeError;
		if (!Acknowledgement.IsValid()
			|| !Acknowledger->TryAcknowledge(
				Acknowledgement,
				Resources,
				AcknowledgeError))
		{
			OutOutcome = MakeOutcome(
				VerificationId,
				EVerificationStatus::Unavailable,
				EEffectOutcome::StateUnknown,
				TEXT(
					"verification_acknowledgement_rejected"),
				MoveTemp(TargetResults),
				MoveTemp(EvidenceIds));
			return true;
		}
		OutOutcome = MakeOutcome(
			VerificationId,
			EVerificationStatus::Verified,
			EEffectOutcome::Complete,
			TEXT("verification_complete"),
			MoveTemp(TargetResults),
			MoveTemp(EvidenceIds),
			TOptional<FVerificationAcknowledgement>(
				Acknowledgement));
		return true;
	}
}

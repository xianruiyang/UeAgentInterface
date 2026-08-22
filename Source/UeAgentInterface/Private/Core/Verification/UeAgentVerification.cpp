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
				TOptional<FVerificationAcknowledgement>(),
			FString DiagnosticDetail = FString())
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
				MoveTemp(Acknowledgement),
				MoveTemp(DiagnosticDetail));
		}

		FString OracleDiagnosticDetail(
			const FString& Stage,
			const FString& Code,
			const FString& Detail = FString())
		{
			FString Result = TEXT("stage=") + Stage;
			if (!Code.IsEmpty())
			{
				Result += TEXT("; oracle_code=") + Code;
			}
			if (!Detail.IsEmpty())
			{
				Result += TEXT("; oracle_detail=") + Detail;
			}
			return Result;
		}

		FString CanonicalValueDiagnostic(
			const StateSemantic::FUeAgentCanonicalValuePtr& Value)
		{
			if (!Value.IsValid())
			{
				return TEXT("<missing>");
			}
			switch (Value->GetKind())
			{
			case StateSemantic::EUeAgentValueKind::Null:
				return TEXT("null");
			case StateSemantic::EUeAgentValueKind::Boolean:
				return Value->GetBoolean() ? TEXT("true") : TEXT("false");
			case StateSemantic::EUeAgentValueKind::String:
				return Value->GetText().Left(160);
			case StateSemantic::EUeAgentValueKind::Integer:
			case StateSemantic::EUeAgentValueKind::Decimal:
				return Value->GetLexeme();
			case StateSemantic::EUeAgentValueKind::Array:
				return FString::Printf(
					TEXT("array[%d]"),
					Value->GetArrayItems().Num());
			case StateSemantic::EUeAgentValueKind::Object:
				return FString::Printf(
					TEXT("object[%d]"),
					Value->GetObjectItems().Num());
			case StateSemantic::EUeAgentValueKind::Temporal:
				return TEXT("temporal");
			case StateSemantic::EUeAgentValueKind::Spatial:
				return TEXT("spatial");
			default:
				return TEXT("unknown");
			}
		}

		FString ComparisonDiagnosticDetail(
			const FUeAgentSemanticComparison& Comparison)
		{
			const TArray<StateSemantic::FUeAgentSemanticDifference>&
				Differences =
				Comparison.GetDifferences();
			FString Result = FString::Printf(
				TEXT("difference_count=%d"),
				Differences.Num());
			const int32 Limit =
				FMath::Min<int32>(Differences.Num(), 8);
			for (int32 Index = 0; Index < Limit; ++Index)
			{
				const StateSemantic::FUeAgentSemanticDifference&
					Difference =
					Differences[Index];
				Result += FString::Printf(
					TEXT("; difference[%d].path=%s; left=%s; right=%s"),
					Index,
					*Difference.GetPath(),
					*CanonicalValueDiagnostic(Difference.GetLeft()),
					*CanonicalValueDiagnostic(Difference.GetRight()));
			}
			return Result;
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
		TOptional<FVerificationAcknowledgement> InAcknowledgement,
		FString InDiagnosticDetail)
		: VerificationId(
			NormalizedText(MoveTemp(InVerificationId)))
		, Status(InStatus)
		, EffectOutcome(InEffectOutcome)
		, DiagnosticCode(
			NormalizedText(MoveTemp(InDiagnosticCode)))
		, DiagnosticDetail(
			NormalizedText(MoveTemp(InDiagnosticDetail)))
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

	const FString& FVerificationOutcome::GetDiagnosticDetail() const
	{
		return DiagnosticDetail;
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
		if (!StateReadPort
			|| !Target.IsValid()
			|| Artifacts.IsEmpty()
			|| StateReadPort->GetPortId()
				!= Target.GetBinding().GetReadPortId())
		{
			OutError = FVerificationError(
				TEXT("persisted_state_oracle_request_invalid"));
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
			OutError = FVerificationError(
				ReadError.Code.IsEmpty()
					? TEXT("persisted_state_oracle_read_failed")
					: ReadError.Code,
				ReadError.Detail);
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
		FVerificationError& OutError) const
	{
		OutError = FVerificationError();
		if (!Request.IsValid()
			|| PersistedStateOracle == nullptr
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
		FString UnavailableDetail;
		FString MismatchDetail;
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
				FVerificationError OracleError;
				if (PersistedStateOracle->GetReadPortId()
					!= Target.GetBinding().GetReadPortId())
				{
					UnavailableDetail = OracleDiagnosticDetail(
						TEXT("persisted_read_port"),
						TEXT("verification_oracle_read_port_mismatch"),
						PersistedStateOracle->GetReadPortId()
							+ TEXT(" != ")
							+ Target.GetBinding().GetReadPortId());
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
					UnavailableDetail = OracleDiagnosticDetail(
						TEXT("persisted_observe"),
						OracleError.GetCode(),
						OracleError.GetDetail());
					bUnavailable = true;
					break;
				}
				if (!Actual.IsValid()
					|| Actual->GetProvenance().GetSourceKind()
						!= TEXT("independent_readback"))
				{
					UnavailableDetail = OracleDiagnosticDetail(
						TEXT("persisted_snapshot"),
						TEXT("verification_oracle_snapshot_invalid"));
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
					UnavailableDetail = OracleDiagnosticDetail(
						TEXT("persisted_independence"),
						TEXT("verification_oracle_evidence_not_independent"));
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
					UnavailableDetail = OracleDiagnosticDetail(
						TEXT("persisted_compare"),
						CompareError.Code,
						CompareError.Detail);
					bUnavailable = true;
					break;
				}
				const EVerificationStatus TargetStatus =
					Comparison.IsEquivalent()
						? EVerificationStatus::Verified
						: EVerificationStatus::Mismatch;
				if (TargetStatus == EVerificationStatus::Mismatch
					&& MismatchDetail.IsEmpty())
				{
					MismatchDetail =
						ComparisonDiagnosticDetail(Comparison);
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
					UnavailableDetail = OracleDiagnosticDetail(
						TEXT("absence_observe"),
						OracleError.IsPresent()
							? OracleError.GetCode()
							: TEXT("verification_absence_observation_invalid"),
						OracleError.GetDetail());
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
				UnavailableDetail = OracleDiagnosticDetail(
					TEXT("reference_observe"),
					OracleError.IsPresent()
						? OracleError.GetCode()
						: TEXT("verification_reference_observation_invalid"),
					OracleError.GetDetail());
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
				MoveTemp(EvidenceIds),
				TOptional<FVerificationAcknowledgement>(),
				MoveTemp(UnavailableDetail));
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
				MoveTemp(EvidenceIds),
				TOptional<FVerificationAcknowledgement>(),
				MoveTemp(MismatchDetail));
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

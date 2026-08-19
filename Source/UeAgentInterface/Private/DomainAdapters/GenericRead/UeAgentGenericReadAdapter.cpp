#include "DomainAdapters/GenericRead/UeAgentGenericReadAdapter.h"

namespace UE::UAI::DomainAdapters::GenericRead
{
	namespace
	{
		using UE::UAI::ContextResource::FUeAgentCancellationRequested;
		using UE::UAI::ContextResource::FUeAgentOperationDeadlineExceeded;
		using UE::UAI::StateSemantic::FUeAgentStateEntry;
		using UE::UAI::StateSemantic::FUeAgentStateProvenance;
		using UE::UAI::StateSemantic::FUeAgentStateSemanticError;

		bool IsNonEmpty(const FString& Value)
		{
			return !Value.TrimStartAndEnd().IsEmpty();
		}

		bool Fail(
			FUeAgentDiscoveryError& OutError,
			FString Code,
			FString SubjectId = FString(),
			FString Detail = FString())
		{
			OutError.Code = MoveTemp(Code);
			OutError.SubjectId = MoveTemp(SubjectId);
			OutError.Detail = MoveTemp(Detail);
			return false;
		}

		bool Checkpoint(
			const FUeAgentOperationDeadline& Deadline,
			const FUeAgentCancellationToken& Cancellation,
			FUeAgentDiscoveryError& OutError)
		{
			const FString Stage = TEXT("domain_adapter.generic_read");
			FUeAgentCancellationRequested Cancelled;
			if (!Cancellation.TryCheckpoint(Cancelled))
			{
				return Fail(
					OutError,
					Cancelled.Code,
					Stage,
					Cancelled.Reason);
			}

			FUeAgentOperationDeadlineExceeded Expired;
			if (!Deadline.TryCheckpoint(Stage, Expired))
			{
				return Fail(
					OutError,
					Expired.Code,
					Stage,
					Expired.Stage);
			}
			return true;
		}

		void MapStateError(
			const FUeAgentStateSemanticError& Source,
			FUeAgentDiscoveryError& OutError)
		{
			OutError.Code = Source.Code;
			OutError.SubjectId = Source.Path;
			OutError.Detail = Source.Detail;
			OutError.ResourceDimension = Source.ResourceDimension;
			OutError.ResourceLimit = Source.ResourceLimit;
			OutError.ResourceObserved = Source.ResourceObserved;
		}
	}

	FUeAgentUeTypeBinding::FUeAgentUeTypeBinding(
		FString InSourceTypeId,
		FUeAgentCanonicalType InCanonicalType,
		FString InBindingEvidence)
		: SourceTypeId(MoveTemp(InSourceTypeId))
		, CanonicalType(MoveTemp(InCanonicalType))
		, BindingEvidence(MoveTemp(InBindingEvidence))
	{
		bValid = IsNonEmpty(SourceTypeId)
			&& CanonicalType.IsValid()
			&& IsNonEmpty(BindingEvidence);
	}

	const FString& FUeAgentUeTypeBinding::GetSourceTypeId() const
	{
		return SourceTypeId;
	}

	const FUeAgentCanonicalType& FUeAgentUeTypeBinding::GetCanonicalType() const
	{
		return CanonicalType;
	}

	const FString& FUeAgentUeTypeBinding::GetBindingEvidence() const
	{
		return BindingEvidence;
	}

	bool FUeAgentUeTypeBinding::IsValid() const
	{
		return bValid;
	}

	FUeAgentUeReadEntry::FUeAgentUeReadEntry(
		FUeAgentCanonicalIdentity InIdentity,
		FUeAgentUeTypeBinding InTypeBinding,
		FUeAgentCanonicalValueRef InValue,
		FUeAgentDefaultValue InDefault,
		FUeAgentStateDetails InDetails,
		FUeAgentStateRevision InObservedRevision)
		: Identity(MoveTemp(InIdentity))
		, TypeBinding(MoveTemp(InTypeBinding))
		, Value(MoveTemp(InValue))
		, Default(MoveTemp(InDefault))
		, Details(MoveTemp(InDetails))
		, ObservedRevision(MoveTemp(InObservedRevision))
	{
		const FUeAgentCanonicalType& Type = TypeBinding.GetCanonicalType();
		const auto MatchesType =
			[&Type](const FUeAgentCanonicalValueRef& Candidate)
			{
				return Candidate->GetKind() == Type.GetValueKind()
					|| (Candidate->GetKind()
							== StateSemantic::EUeAgentValueKind::Null
						&& Type.IsNullable());
			};
		bValid = Identity.IsValid()
			&& TypeBinding.IsValid()
			&& Default.IsValid()
			&& Details.IsValid()
			&& ObservedRevision.IsValid()
			&& MatchesType(Value)
			&& MatchesType(Default.GetValue());
	}

	const FUeAgentCanonicalIdentity& FUeAgentUeReadEntry::GetIdentity() const
	{
		return Identity;
	}

	const FUeAgentUeTypeBinding& FUeAgentUeReadEntry::GetTypeBinding() const
	{
		return TypeBinding;
	}

	const FUeAgentCanonicalValueRef& FUeAgentUeReadEntry::GetValue() const
	{
		return Value;
	}

	const FUeAgentDefaultValue& FUeAgentUeReadEntry::GetDefault() const
	{
		return Default;
	}

	const FUeAgentStateDetails& FUeAgentUeReadEntry::GetDetails() const
	{
		return Details;
	}

	const FUeAgentStateRevision& FUeAgentUeReadEntry::GetObservedRevision() const
	{
		return ObservedRevision;
	}

	bool FUeAgentUeReadEntry::IsValid() const
	{
		return bValid;
	}

	FUeAgentUeReadCapture::FUeAgentUeReadCapture(
		FString InSnapshotId,
		EUeAgentStatePlane InStatePlane,
		FUeAgentStateRevision InRevision,
		FUeAgentTemporalPoint InCapturedAt,
		FString InEvidenceId,
		bool bInSettled,
		TArray<FUeAgentUeReadEntry> InEntries)
		: SnapshotId(MoveTemp(InSnapshotId))
		, StatePlane(InStatePlane)
		, Revision(MoveTemp(InRevision))
		, CapturedAt(MoveTemp(InCapturedAt))
		, EvidenceId(MoveTemp(InEvidenceId))
		, bSettled(bInSettled)
		, Entries(MoveTemp(InEntries))
	{
		bValid = IsNonEmpty(SnapshotId)
			&& Revision.IsValid()
			&& CapturedAt.IsValid()
			&& IsNonEmpty(EvidenceId);
		for (const FUeAgentUeReadEntry& Entry : Entries)
		{
			if (!Entry.IsValid()
				|| Entry.GetObservedRevision() != Revision)
			{
				bValid = false;
				break;
			}
		}
	}

	const FString& FUeAgentUeReadCapture::GetSnapshotId() const
	{
		return SnapshotId;
	}

	EUeAgentStatePlane FUeAgentUeReadCapture::GetStatePlane() const
	{
		return StatePlane;
	}

	const FUeAgentStateRevision& FUeAgentUeReadCapture::GetRevision() const
	{
		return Revision;
	}

	const FUeAgentTemporalPoint& FUeAgentUeReadCapture::GetCapturedAt() const
	{
		return CapturedAt;
	}

	const FString& FUeAgentUeReadCapture::GetEvidenceId() const
	{
		return EvidenceId;
	}

	bool FUeAgentUeReadCapture::IsSettled() const
	{
		return bSettled;
	}

	const TArray<FUeAgentUeReadEntry>& FUeAgentUeReadCapture::GetEntries() const
	{
		return Entries;
	}

	bool FUeAgentUeReadCapture::IsValid() const
	{
		return bValid;
	}

	FUeAgentGenericReadAdapter::FUeAgentGenericReadAdapter(
		FString InPortId,
		TSharedRef<IUeAgentUeReadHook> InHook)
		: PortId(MoveTemp(InPortId))
		, Hook(MoveTemp(InHook))
	{
		PortId.TrimStartAndEndInline();
	}

	const FString& FUeAgentGenericReadAdapter::GetPortId() const
	{
		return PortId;
	}

	bool FUeAgentGenericReadAdapter::TryRead(
		const FUeAgentStateReadRequest& Request,
		FUeAgentResourceTracker& Resources,
		const FUeAgentOperationDeadline& Deadline,
		const FUeAgentCancellationToken& Cancellation,
		TSharedPtr<const FUeAgentCanonicalSnapshot>& OutSnapshot,
		FUeAgentDiscoveryError& OutError)
	{
		OutSnapshot.Reset();
		OutError = {};
		if (!Request.IsValid())
		{
			return Fail(
				OutError,
				TEXT("ue_read_request_invalid"),
				Request.GetBinding().GetBindingId());
		}
		if (!IsNonEmpty(PortId)
			|| Request.GetBinding().GetReadPortId() != PortId)
		{
			return Fail(
				OutError,
				TEXT("ue_read_port_mismatch"),
				Request.GetBinding().GetReadPortId(),
				PortId);
		}
		if (!Checkpoint(Deadline, Cancellation, OutError))
		{
			return false;
		}

		TSharedPtr<const FUeAgentUeReadCapture> Capture;
		FUeAgentGenericReadError CaptureError;
		if (!Hook->TryCapture(
				Request,
				Resources,
				Deadline,
				Cancellation,
				Capture,
				CaptureError))
		{
			if (CaptureError.Code.IsEmpty())
			{
				return Fail(
					OutError,
					TEXT("ue_capture_failed"),
					Request.GetBinding().GetBindingId());
			}
			return Fail(
				OutError,
				CaptureError.Code,
				CaptureError.SubjectId,
				CaptureError.Detail);
		}
		if (!Capture.IsValid())
		{
			return Fail(
				OutError,
				TEXT("ue_read_capture_invalid"),
				Request.GetBinding().GetBindingId());
		}
		if (!Capture->IsSettled())
		{
			return Fail(
				OutError,
				TEXT("ue_read_not_settled"),
				Request.GetBinding().GetBindingId());
		}
		if (Capture->GetStatePlane() != Request.GetStatePlane())
		{
			return Fail(
				OutError,
				TEXT("ue_read_state_plane_mismatch"),
				Request.GetBinding().GetBindingId());
		}
		if (Capture->GetRevision().GetSourceId() != PortId)
		{
			return Fail(
				OutError,
				TEXT("ue_read_revision_source_mismatch"),
				Capture->GetRevision().GetSourceId(),
				PortId);
		}
		if (!Capture->IsValid())
		{
			return Fail(
				OutError,
				TEXT("ue_read_capture_invalid"),
				Request.GetBinding().GetBindingId());
		}

		TArray<FUeAgentStateEntry> Entries;
		Entries.Reserve(Capture->GetEntries().Num());
		for (const FUeAgentUeReadEntry& Entry : Capture->GetEntries())
		{
			Entries.Emplace(
				Entry.GetIdentity(),
				Entry.GetTypeBinding().GetCanonicalType(),
				Entry.GetValue(),
				Entry.GetDefault(),
				Entry.GetDetails(),
				Entry.GetObservedRevision());
		}

		FUeAgentStateSemanticError StateError;
		if (!FUeAgentCanonicalSnapshot::TryCreate(
				Capture->GetSnapshotId(),
				Capture->GetStatePlane(),
				Capture->GetRevision(),
				Capture->GetCapturedAt(),
				FUeAgentStateProvenance(
					TEXT("independent_readback"),
					PortId,
					Capture->GetEvidenceId()),
				Entries,
				Resources,
				OutSnapshot,
				StateError))
		{
			MapStateError(StateError, OutError);
			return false;
		}
		return true;
	}
}

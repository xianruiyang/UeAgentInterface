#include "DomainAdapters/GenericRead/UeAgentGenericReadAdapter.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

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

		FUeAgentGenericReadError NativeValueFailure(
			FString Code, FString Reason, FString Field,
			TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected)
		{
			auto Error = FUeAgentGenericReadError::Failure(MoveTemp(Code), MoveTemp(Reason), TEXT("generic_read"));
			Error.SourceKind = TEXT("native_result");
			Error.PropertyPath = MoveTemp(Field);
			Error.ActualValue = MoveTemp(Actual);
			Error.ExpectedValue = MoveTemp(Expected);
			Error.ActualValuePresent = Error.ActualValue.IsValid();
			Error.ExpectedValuePresent = Error.ExpectedValue.IsValid();
			return Error;
		}

		FUeAgentGenericReadError NativeConditionFailure(
			FString Code, FString Reason, FString Field, bool bActual, bool bExpected)
		{
			return NativeValueFailure(MoveTemp(Code), MoveTemp(Reason), MoveTemp(Field),
				MakeShared<FJsonValueBoolean>(bActual), MakeShared<FJsonValueBoolean>(bExpected));
		}

		FUeAgentGenericReadError WrapValidation(FString Code, FUeAgentGenericReadError Cause)
		{
			if (Cause.SourceKind.IsEmpty()) Cause.SourceKind = TEXT("native_result");
			if (Cause.PropertyPath.IsEmpty()) Cause.PropertyPath = Cause.SemanticPointer;
			return FUeAgentGenericReadError::WrapFailure(MoveTemp(Code), Cause, TEXT("generic_read"));
		}

		const FUeAgentGenericReadError& ValidationOrEmpty(
			const TSharedPtr<const FUeAgentGenericReadError>& Diagnostic)
		{
			static const FUeAgentGenericReadError Empty;
			return Diagnostic ? *Diagnostic : Empty;
		}

		FUeAgentGenericReadError AtBinding(FUeAgentGenericReadError Error,
			const Discovery::FUeAgentCandidateBinding& Binding)
		{
			if (Error.TargetPath.IsEmpty()) Error.TargetPath = Binding.GetPublicRef();
			if (Error.SubjectId.IsEmpty()) Error.SubjectId = Binding.GetBindingId();
			return Error;
		}

		FUeAgentGenericReadError AtCapture(FUeAgentGenericReadError Error,
			const FUeAgentUeReadCapture& Capture, const Discovery::FUeAgentCandidateBinding& Binding)
		{
			Error.Append(Capture.GetValidationDiagnostic());
			return AtBinding(MoveTemp(Error), Binding);
		}

		FString ValueKindName(StateSemantic::EUeAgentValueKind Kind)
		{
			switch (Kind)
			{
			case StateSemantic::EUeAgentValueKind::Null: return TEXT("null");
			case StateSemantic::EUeAgentValueKind::Boolean: return TEXT("boolean");
			case StateSemantic::EUeAgentValueKind::String: return TEXT("string");
			case StateSemantic::EUeAgentValueKind::Integer: return TEXT("integer");
			case StateSemantic::EUeAgentValueKind::Decimal: return TEXT("decimal");
			case StateSemantic::EUeAgentValueKind::Array: return TEXT("array");
			case StateSemantic::EUeAgentValueKind::Object: return TEXT("object");
			case StateSemantic::EUeAgentValueKind::Temporal: return TEXT("temporal");
			case StateSemantic::EUeAgentValueKind::Spatial: return TEXT("spatial");
			default: return FString::FromInt(static_cast<int32>(Kind));
			}
		}

		TSharedPtr<FJsonValue> RevisionEvidence(const FUeAgentStateRevision& Revision)
		{
			auto Value = MakeShared<FJsonObject>();
			Value->SetStringField(TEXT("source_id"), Revision.GetSourceId());
			Value->SetStringField(TEXT("generation"), Revision.GetGeneration());
			Value->SetStringField(TEXT("sequence"), LexToString(Revision.GetSequence()));
			return MakeShared<FJsonValueObject>(Value);
		}

		bool Fail(FUeAgentDiscoveryError& OutError, UE::UAI::Contract::FDiagnostic Diagnostic)
		{
			OutError = MoveTemp(Diagnostic);
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
				return Fail(OutError, UE::UAI::Contract::FDiagnostic::WrapFailure(Cancelled.Code, Cancelled, Stage));
			}

			return true;
		}

		void MapStateError(
			const FUeAgentStateSemanticError& Source,
			FUeAgentDiscoveryError& OutError)
		{
			OutError = Source;
			if (OutError.SourceKind.IsEmpty()) OutError.SourceKind = TEXT("native_result");
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
		if (!bValid)
		{
			FUeAgentGenericReadError Error;
			if (!IsNonEmpty(SourceTypeId))
				Error = NativeValueFailure(TEXT("ue_type_binding_invalid"), TEXT("The native source type is empty."),
					TEXT("/type_binding/source_type_id"), MakeShared<FJsonValueString>(SourceTypeId), MakeShared<FJsonValueString>(TEXT("non-empty type")));
			else if (!CanonicalType.IsValid())
				Error = WrapValidation(TEXT("ue_type_binding_invalid"), CanonicalType.GetValidationDiagnostic());
			else
				Error = NativeConditionFailure(TEXT("ue_type_binding_invalid"), TEXT("The native-to-canonical type binding has no evidence."),
					TEXT("/type_binding/binding_evidence_present"), false, true);
			ValidationDiagnostic = MakeShared<const FUeAgentGenericReadError>(MoveTemp(Error));
		}
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

	const FUeAgentGenericReadError& FUeAgentUeTypeBinding::GetValidationDiagnostic() const
	{
		return ValidationOrEmpty(ValidationDiagnostic);
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
		if (!bValid)
		{
			FUeAgentGenericReadError Error;
			if (!Identity.IsValid()) Error = WrapValidation(TEXT("ue_read_entry_invalid"), Identity.GetValidationDiagnostic());
			else if (!TypeBinding.IsValid()) Error = WrapValidation(TEXT("ue_read_entry_invalid"), TypeBinding.GetValidationDiagnostic());
			else if (!Default.IsValid()) Error = WrapValidation(TEXT("ue_read_entry_invalid"), Default.GetValidationDiagnostic());
			else if (!Details.IsValid()) Error = WrapValidation(TEXT("ue_read_entry_invalid"), Details.GetValidationDiagnostic());
			else if (!ObservedRevision.IsValid()) Error = WrapValidation(TEXT("ue_read_entry_invalid"), ObservedRevision.GetValidationDiagnostic());
			else
			{
				const bool bValueMismatch = !MatchesType(Value);
				FString Expected = ValueKindName(Type.GetValueKind());
				if (Type.IsNullable()) Expected += TEXT(" or null");
				Error = NativeValueFailure(TEXT("ue_read_entry_invalid"),
					bValueMismatch ? TEXT("The observed value kind does not match its canonical type.")
						: TEXT("The default value kind does not match its canonical type."),
					bValueMismatch ? TEXT("/value/kind") : TEXT("/default/kind"),
					MakeShared<FJsonValueString>(ValueKindName((bValueMismatch ? Value : Default.GetValue())->GetKind())),
					MakeShared<FJsonValueString>(Expected));
			}
			if (Error.SubjectId.IsEmpty()) Error.SubjectId = Identity.GetCanonicalKey();
			ValidationDiagnostic = MakeShared<const FUeAgentGenericReadError>(MoveTemp(Error));
		}
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

	const FUeAgentGenericReadError& FUeAgentUeReadEntry::GetValidationDiagnostic() const
	{
		return ValidationOrEmpty(ValidationDiagnostic);
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
		FUeAgentGenericReadError Error;
		if (!IsNonEmpty(SnapshotId))
			Error = NativeConditionFailure(TEXT("ue_read_capture_invalid"), TEXT("The native read capture has no snapshot identity."),
				TEXT("/snapshot_id_present"), false, true);
		else if (!Revision.IsValid()) Error = WrapValidation(TEXT("ue_read_capture_invalid"), Revision.GetValidationDiagnostic());
		else if (!CapturedAt.IsValid()) Error = WrapValidation(TEXT("ue_read_capture_invalid"), CapturedAt.GetValidationDiagnostic());
		else if (!IsNonEmpty(EvidenceId))
			Error = NativeConditionFailure(TEXT("ue_read_capture_invalid"), TEXT("The native read capture has no observation evidence."),
				TEXT("/evidence_id_present"), false, true);
		for (int32 Index = 0; Index < Entries.Num(); ++Index)
		{
			const FUeAgentUeReadEntry& Entry = Entries[Index];
			if (!Entry.IsValid()
				|| Entry.GetObservedRevision() != Revision)
			{
				bValid = false;
				if (Error.IsEmpty())
				{
					if (!Entry.IsValid()) Error = WrapValidation(TEXT("ue_read_capture_invalid"), Entry.GetValidationDiagnostic());
					else
					{
						Error = NativeConditionFailure(TEXT("ue_read_capture_invalid"), TEXT("The entry revision differs from its capture revision."),
							TEXT("/observed_revision_matches_capture"), false, true);
						Error.ActualEvidence = RevisionEvidence(Entry.GetObservedRevision());
						Error.ExpectedEvidence = RevisionEvidence(Revision);
					}
					Error.PropertyPath = FString::Printf(TEXT("/entries/%d"), Index) + Error.PropertyPath;
				}
				break;
			}
		}
		if (!Error.IsEmpty()) ValidationDiagnostic = MakeShared<const FUeAgentGenericReadError>(MoveTemp(Error));
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

	const FUeAgentGenericReadError& FUeAgentUeReadCapture::GetValidationDiagnostic() const
	{
		return ValidationOrEmpty(ValidationDiagnostic);
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
			const auto& Validation = Request.GetBinding().GetValidationDiagnostic();
			auto Error = Validation
				? WrapValidation(TEXT("ue_read_request_invalid"), *Validation)
				: NativeConditionFailure(TEXT("ue_read_request_invalid"),
					TEXT("The read binding is invalid but supplied no validation diagnostic."), TEXT("/binding_valid"), false, true);
			return Fail(OutError, AtBinding(MoveTemp(Error), Request.GetBinding()));
		}
		if (!IsNonEmpty(PortId)
			|| Request.GetBinding().GetReadPortId() != PortId)
		{
			auto Error = NativeConditionFailure(TEXT("ue_read_port_mismatch"),
				!IsNonEmpty(PortId) ? TEXT("The native read adapter has no configured read port.")
					: TEXT("The binding requests a different native read port."),
				TEXT("/read_port_matches_binding"), false, true);
			Error.ActualEvidence = MakeShared<FJsonValueString>(PortId);
			Error.ExpectedEvidence = MakeShared<FJsonValueString>(Request.GetBinding().GetReadPortId());
			return Fail(OutError, AtBinding(MoveTemp(Error), Request.GetBinding()));
		}
		if (!Checkpoint(Deadline, Cancellation, OutError))
		{
			OutError = AtBinding(MoveTemp(OutError), Request.GetBinding());
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
			OutError = CaptureError.IsEmpty()
				? NativeConditionFailure(TEXT("ue_capture_failed"), TEXT("The native capture hook failed without supplying a diagnostic."),
					TEXT("/native_capture_succeeded"), false, true)
				: FUeAgentDiscoveryError::WrapFailure(CaptureError.Code, CaptureError, TEXT("generic_read"));
			OutError = AtBinding(MoveTemp(OutError), Request.GetBinding());
			return false;
		}
		if (!Capture.IsValid())
		{
			return Fail(OutError, AtBinding(NativeConditionFailure(TEXT("ue_read_capture_invalid"),
				TEXT("The native capture hook returned no read capture."), TEXT("/capture_present"), false, true), Request.GetBinding()));
		}
		if (!Capture->IsSettled())
		{
			return Fail(OutError, AtCapture(NativeConditionFailure(TEXT("ue_read_not_settled"),
				TEXT("The native read capture has not settled."), TEXT("/settled"), false, true), *Capture, Request.GetBinding()));
		}
		if (Capture->GetStatePlane() != Request.GetStatePlane())
		{
			return Fail(OutError, AtCapture(NativeValueFailure(TEXT("ue_read_state_plane_mismatch"),
				TEXT("The native capture state plane differs from the requested state plane."), TEXT("/state_plane"),
				MakeShared<FJsonValueString>(UE::UAI::Contract::LexToString(Capture->GetStatePlane())),
				MakeShared<FJsonValueString>(UE::UAI::Contract::LexToString(Request.GetStatePlane()))), *Capture, Request.GetBinding()));
		}
		if (Capture->GetRevision().GetSourceId() != PortId)
		{
			auto Error = NativeConditionFailure(TEXT("ue_read_revision_source_mismatch"),
				TEXT("The capture revision was produced by a different native read source."), TEXT("/revision/source_matches_port"), false, true);
			Error.ActualEvidence = MakeShared<FJsonValueString>(Capture->GetRevision().GetSourceId());
			Error.ExpectedEvidence = MakeShared<FJsonValueString>(PortId);
			return Fail(OutError, AtCapture(MoveTemp(Error), *Capture, Request.GetBinding()));
		}
		if (!Capture->IsValid())
		{
			return Fail(OutError, AtBinding(WrapValidation(TEXT("ue_read_capture_invalid"),
				Capture->GetValidationDiagnostic()), Request.GetBinding()));
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
			OutError = AtBinding(MoveTemp(OutError), Request.GetBinding());
			return false;
		}
		return true;
	}
}

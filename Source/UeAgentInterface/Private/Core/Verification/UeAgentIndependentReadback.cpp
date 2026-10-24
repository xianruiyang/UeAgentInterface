#include "Core/Verification/UeAgentIndependentReadback.h"
#include "Core/StateSemantic/UeAgentStateSemanticEvidence.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Dom/JsonValue.h"

namespace UE::UAI::Verification
{
	using namespace ArtifactPublisher;
	using namespace ContextResource;
	using namespace StateSemantic;

	namespace
	{
		FString NormalizeText(FString Value)
		{
			Value.TrimStartAndEndInline();
			return Value;
		}

		bool IsOracleId(const FString& Value)
		{
			return !Value.IsEmpty()
				&& Value == NormalizeText(Value)
				&& Value.StartsWith(TEXT("uai.readback."));
		}

		FVerificationError ReadbackFailure(const FString& Code, const FString& Reason, const FString& Path)
		{
			auto Error = FVerificationError::Failure(Code, Reason, TEXT("independent_readback"));
			Error.PropertyPath = Path;
			return Error;
		}
	}

	FIndependentReadbackRequest::FIndependentReadbackRequest(
		FString InOperationId,
		FString InOracleId,
		FString InSubjectRef,
		FUeAgentCanonicalValueRef InExpectedValue,
		TOptional<FFileArtifactReference> InArtifact)
		: OperationId(NormalizeText(MoveTemp(InOperationId)))
		, OracleId(NormalizeText(MoveTemp(InOracleId)))
		, SubjectRef(NormalizeText(MoveTemp(InSubjectRef)))
		, ExpectedValue(MoveTemp(InExpectedValue))
		, Artifact(MoveTemp(InArtifact))
	{
		FVerificationError Error;
		if (OperationId.IsEmpty()) Error = ReadbackFailure(TEXT("independent_readback_request_invalid"),
			TEXT("The independent readback request has no operation identifier."), TEXT("operation_id"));
		else if (!IsOracleId(OracleId))
		{
			Error = ReadbackFailure(TEXT("independent_readback_request_invalid"),
				TEXT("The readback oracle identifier must start with 'uai.readback.' and have no surrounding whitespace."), TEXT("oracle_id"));
			Error.ActualEvidence = MakeShared<FJsonValueString>(OracleId);
		}
		else if (SubjectRef.IsEmpty()) Error = ReadbackFailure(TEXT("independent_readback_request_invalid"),
			TEXT("The independent readback request has no target reference."), TEXT("subject_ref"));
		else if (Artifact.IsSet() && !Artifact->IsValid())
		{
			Error = *Artifact->GetValidationDiagnostic();
			Error.PropertyPath = TEXT("artifact.") + Error.PropertyPath;
		}
		if (!Error.IsEmpty())
		{
			if (Error.TargetPath.IsEmpty()) Error.TargetPath = SubjectRef;
			ValidationDiagnostic = MakeShared<FVerificationError>(MoveTemp(Error));
		}
	}

	const FString& FIndependentReadbackRequest::GetOperationId() const
	{
		return OperationId;
	}

	const FString& FIndependentReadbackRequest::GetOracleId() const
	{
		return OracleId;
	}

	const FString& FIndependentReadbackRequest::GetSubjectRef() const
	{
		return SubjectRef;
	}

	const FUeAgentCanonicalValueRef&
	FIndependentReadbackRequest::GetExpectedValue() const
	{
		return ExpectedValue;
	}

	const TOptional<FFileArtifactReference>&
	FIndependentReadbackRequest::GetArtifact() const
	{
		return Artifact;
	}

	bool FIndependentReadbackRequest::IsValid() const
	{
		return !ValidationDiagnostic.IsValid();
	}

	const FVerificationError* FIndependentReadbackRequest::GetValidationDiagnostic() const
	{
		return ValidationDiagnostic.Get();
	}

	FIndependentReadbackObservation::FIndependentReadbackObservation()
		: ValidationDiagnostic(MakeShared<FVerificationError>(ReadbackFailure(
			TEXT("independent_readback_observation_invalid"),
			TEXT("The readback oracle did not supply an observation."), TEXT("observation"))))
	{
	}

	FIndependentReadbackObservation
	FIndependentReadbackObservation::Available(
		FUeAgentCanonicalValueRef InActualValue,
		FString InEvidenceId)
	{
		FIndependentReadbackObservation Result;
		Result.ActualValue = MoveTemp(InActualValue);
		Result.EvidenceId = NormalizeText(MoveTemp(InEvidenceId));
		Result.bAvailable = true;
		Result.ValidationDiagnostic.Reset();
		if (!Result.ActualValue.IsValid() || Result.EvidenceId.IsEmpty())
		{
			Result.ValidationDiagnostic = MakeShared<FVerificationError>(ReadbackFailure(
				TEXT("independent_readback_observation_invalid"),
				!Result.ActualValue.IsValid()
					? TEXT("The available readback observation has no observed value.")
					: TEXT("The available readback observation has no evidence identifier."),
				!Result.ActualValue.IsValid() ? TEXT("observation.actual_value") : TEXT("observation.evidence_id")));
		}
		return Result;
	}

	FIndependentReadbackObservation
	FIndependentReadbackObservation::Unavailable(
		FVerificationError InDiagnostic,
		FString InEvidenceId)
	{
		FIndependentReadbackObservation Result;
		Result.Diagnostic = MoveTemp(InDiagnostic);
		Result.EvidenceId = NormalizeText(MoveTemp(InEvidenceId));
		Result.bAvailable = false;
		Result.ValidationDiagnostic.Reset();
		if (Result.Diagnostic.IsEmpty())
		{
			Result.ValidationDiagnostic = MakeShared<FVerificationError>(ReadbackFailure(
				TEXT("independent_readback_observation_invalid"),
				TEXT("The unavailable readback observation has no diagnostic explaining why it is unavailable."),
				TEXT("observation.diagnostic")));
		}
		return Result;
	}

	bool FIndependentReadbackObservation::IsValid() const
	{
		return !ValidationDiagnostic.IsValid();
	}

	const FVerificationError* FIndependentReadbackObservation::GetValidationDiagnostic() const
	{
		return ValidationDiagnostic.Get();
	}

	bool FIndependentReadbackObservation::IsAvailable() const
	{
		return bAvailable;
	}

	const FUeAgentCanonicalValuePtr&
	FIndependentReadbackObservation::GetActualValue() const
	{
		return ActualValue;
	}

	const FString& FIndependentReadbackObservation::GetEvidenceId() const
	{
		return EvidenceId;
	}

	const FString&
	FIndependentReadbackObservation::GetDiagnosticCode() const
	{
		return Diagnostic.Code;
	}


	const FVerificationError& FIndependentReadbackObservation::GetDiagnostic() const
	{
		return Diagnostic;
	}

	FIndependentReadbackOutcome::FIndependentReadbackOutcome(
		const EVerificationStatus InStatus,
		const EEffectOutcome InEffectOutcome,
		FVerificationError InDiagnostic,
		FString InEvidenceId,
		FUeAgentCanonicalValuePtr InActualValue,
		FUeAgentSemanticComparison InComparison)
		: Status(InStatus)
		, EffectOutcome(InEffectOutcome)
		, Diagnostic(MoveTemp(InDiagnostic))
		, EvidenceId(NormalizeText(MoveTemp(InEvidenceId)))
		, ActualValue(MoveTemp(InActualValue))
		, Comparison(MoveTemp(InComparison))
	{
	}

	EVerificationStatus FIndependentReadbackOutcome::GetStatus() const
	{
		return Status;
	}

	EEffectOutcome FIndependentReadbackOutcome::GetEffectOutcome() const
	{
		return EffectOutcome;
	}

	const FString& FIndependentReadbackOutcome::GetDiagnosticCode() const
	{
		return Diagnostic.Code;
	}


	const FVerificationError& FIndependentReadbackOutcome::GetDiagnostic() const
	{
		return Diagnostic;
	}

	const FString& FIndependentReadbackOutcome::GetEvidenceId() const
	{
		return EvidenceId;
	}



	FVerificationError FIndependentReadbackOutcome::GetFailureDiagnostic(const FVerificationError& Error) const
	{
		FVerificationError Result = Error.IsEmpty() ? Diagnostic : Error;
		Result.Schema = TEXT("uai.verification.diagnostic/1");
		Result.VerificationStatus = LexToString(Status);
		Result.EffectOutcome = LexToString(EffectOutcome);
		return Result;
	}

	const FUeAgentCanonicalValuePtr&
	FIndependentReadbackOutcome::GetActualValue() const
	{
		return ActualValue;
	}

	const FUeAgentSemanticComparison&
	FIndependentReadbackOutcome::GetComparison() const
	{
		return Comparison;
	}

	bool FIndependentReadbackService::TryCreate(
		const TArray<TSharedRef<
			IIndependentReadbackOracle,
			ESPMode::ThreadSafe>>& InOracles,
		TSharedPtr<FIndependentReadbackService>& OutService,
		FVerificationError& OutError)
	{
		OutService.Reset();
		OutError = FVerificationError();
		if (InOracles.IsEmpty())
		{
			OutError = ReadbackFailure(TEXT("independent_readback_oracle_catalog_empty"),
				TEXT("At least one independent readback oracle must be registered."), TEXT("oracles"));
			return false;
		}
		TSet<FString> Ids;
		for (int32 Index = 0; Index < InOracles.Num(); ++Index)
		{
			const FString& Id = InOracles[Index]->GetOracleId();
			if (!IsOracleId(Id))
			{
				OutError = ReadbackFailure(TEXT("independent_readback_oracle_invalid"),
					TEXT("A registered readback oracle identifier must start with 'uai.readback.' and have no surrounding whitespace."),
					FString::Printf(TEXT("oracles[%d].oracle_id"), Index));
				OutError.ActualEvidence = MakeShared<FJsonValueString>(Id);
				return false;
			}
			if (Ids.Contains(Id))
			{
				OutError = ReadbackFailure(TEXT("independent_readback_oracle_duplicate"),
					TEXT("The same independent readback oracle identifier is registered more than once."),
					FString::Printf(TEXT("oracles[%d].oracle_id"), Index));
				OutError.ActualEvidence = MakeShared<FJsonValueString>(Id);
				return false;
			}
			Ids.Add(Id);
		}
		OutService = MakeShareable(
			new FIndependentReadbackService(InOracles));
		return true;
	}

	FIndependentReadbackService::FIndependentReadbackService(
		TArray<TSharedRef<
			IIndependentReadbackOracle,
			ESPMode::ThreadSafe>> InOracles)
		: Oracles(MoveTemp(InOracles))
	{
	}

	bool FIndependentReadbackService::TryVerify(
		const FIndependentReadbackRequest& Request,
		FUeAgentResourceTracker& Resources,
		FIndependentReadbackOutcome& OutOutcome,
		FVerificationError& OutError) const
	{
		return TryReadAndCompare(Request, Resources, OutOutcome, OutError, false);
	}

	bool FIndependentReadbackService::TryObserve(
		const FIndependentReadbackRequest& Request, FUeAgentResourceTracker& Resources,
		FIndependentReadbackOutcome& OutOutcome, FVerificationError& OutError) const
	{
		if (!TryReadAndCompare(Request, Resources, OutOutcome, OutError, true)) return false;
		if (OutOutcome.GetStatus() != EVerificationStatus::Verified)
		{
			auto Warning = OutOutcome.GetFailureDiagnostic(OutError);
			if (Warning.Stage.IsEmpty()) Warning.Stage = TEXT("independent_readback");
			if (Warning.TargetPath.IsEmpty()) Warning.TargetPath = Request.GetSubjectRef();
			UeAgentAuthoringDiagnostics::ReportConsistencyWarning(Warning);
			// This identifies the observation, not proof of equivalence. Keep the
			// actual status; durability acknowledgement must not fabricate Verified.
			if (OutOutcome.GetEvidenceId().IsEmpty())
				OutOutcome = FIndependentReadbackOutcome(OutOutcome.GetStatus(), OutOutcome.GetEffectOutcome(),
					OutOutcome.GetDiagnostic(), TEXT("observation:") + Request.GetOperationId(),
					OutOutcome.GetActualValue(), OutOutcome.GetComparison());
		}
		OutError = FVerificationError();
		return true;
	}

	bool FIndependentReadbackService::TryReadAndCompare(
		const FIndependentReadbackRequest& Request, FUeAgentResourceTracker& Resources,
		FIndependentReadbackOutcome& OutOutcome, FVerificationError& OutError,
		bool bObservationOnly) const
	{
		OutOutcome = FIndependentReadbackOutcome();
		OutError = FVerificationError();
		if (!Request.IsValid())
		{
			OutError = *Request.GetValidationDiagnostic();
			return false;
		}
		const TSharedRef<IIndependentReadbackOracle, ESPMode::ThreadSafe>*
			Oracle = Oracles.FindByPredicate(
				[&Request](
					const TSharedRef<
						IIndependentReadbackOracle,
						ESPMode::ThreadSafe>& Candidate)
				{
					return Candidate->GetOracleId() == Request.GetOracleId();
				});
		if (Oracle == nullptr)
		{
			OutError = ReadbackFailure(TEXT("independent_readback_oracle_not_registered"),
				TEXT("No independent readback oracle is registered for the requested oracle identifier."), TEXT("oracle_id"));
			OutError.ActualEvidence = MakeShared<FJsonValueString>(Request.GetOracleId());
			OutError.TargetPath = Request.GetSubjectRef();
			return false;
		}

		FIndependentReadbackObservation Observation;
		if (!(*Oracle)->TryRead(Request, Resources, Observation, OutError))
		{
			if (OutError.IsEmpty()) OutError = ReadbackFailure(TEXT("independent_readback_read_failed"),
				TEXT("The independent readback oracle failed without reporting a diagnostic."), TEXT("observation"));
			if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Request.GetSubjectRef();
			return false;
		}
		if (!Observation.IsValid())
		{
			OutError = *Observation.GetValidationDiagnostic();
			if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Request.GetSubjectRef();
			return false;
		}
		if (!Observation.IsAvailable())
		{
			auto Diagnostic = Observation.GetDiagnostic();
			if (Diagnostic.TargetPath.IsEmpty()) Diagnostic.TargetPath = Request.GetSubjectRef();
			OutOutcome = FIndependentReadbackOutcome(
				EVerificationStatus::Unavailable,
				EEffectOutcome::StateUnknown,
				MoveTemp(Diagnostic),
				Observation.GetEvidenceId(),
				nullptr,
				FUeAgentSemanticComparison());
			return true;
		}

		FUeAgentSemanticComparison Comparison;
		FUeAgentStateSemanticError CompareError;
		if (!TryCompareValues(
			Request.GetExpectedValue(),
			Observation.GetActualValue().ToSharedRef(),
			TEXT("/"),
			Resources,
			Comparison,
			CompareError))
		{
			OutError = CompareError;
			if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Request.GetSubjectRef();
			if (bObservationOnly)
			{
				OutOutcome = FIndependentReadbackOutcome(EVerificationStatus::Unavailable,
					EEffectOutcome::StateUnknown, OutError, Observation.GetEvidenceId(),
					Observation.GetActualValue(), MoveTemp(Comparison));
				return true;
			}
			return false;
		}
		const bool bEquivalent = Comparison.IsEquivalent();
		FVerificationError Diagnostic = bEquivalent
			? ReadbackFailure(TEXT("independent_readback_complete"),
				TEXT("Independent readback matches the expected value."), FString())
			: StateSemantic::MakeComparisonDiagnostic(Comparison, TEXT("independent_readback_semantic_mismatch"),
				TEXT("Independent readback differs from the expected value."), TEXT("independent_readback"),
				TEXT("uai.verification.diagnostic/1"));
		if (Diagnostic.TargetPath.IsEmpty()) Diagnostic.TargetPath = Request.GetSubjectRef();
		if (bEquivalent) Diagnostic.Severity = Contract::ESeverity::Info;
		OutOutcome = FIndependentReadbackOutcome(
			bEquivalent
				? EVerificationStatus::Verified
				: EVerificationStatus::Mismatch,
			bEquivalent ? EEffectOutcome::Complete : EEffectOutcome::Partial,
			MoveTemp(Diagnostic),
			Observation.GetEvidenceId(),
			Observation.GetActualValue(),
			MoveTemp(Comparison));
		return true;
	}
}

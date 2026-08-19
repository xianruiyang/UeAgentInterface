#include "Core/Verification/UeAgentIndependentReadback.h"

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
		bValid = !OperationId.IsEmpty()
			&& IsOracleId(OracleId)
			&& !SubjectRef.IsEmpty()
			&& (!Artifact.IsSet() || Artifact->IsValid());
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
		return bValid;
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
		Result.bValid = Result.ActualValue.IsValid()
			&& !Result.EvidenceId.IsEmpty();
		return Result;
	}

	FIndependentReadbackObservation
	FIndependentReadbackObservation::Unavailable(
		FString InDiagnosticCode,
		FString InEvidenceId)
	{
		FIndependentReadbackObservation Result;
		Result.DiagnosticCode = NormalizeText(MoveTemp(InDiagnosticCode));
		Result.EvidenceId = NormalizeText(MoveTemp(InEvidenceId));
		Result.bAvailable = false;
		Result.bValid = !Result.DiagnosticCode.IsEmpty();
		return Result;
	}

	bool FIndependentReadbackObservation::IsValid() const
	{
		return bValid;
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
		return DiagnosticCode;
	}

	FIndependentReadbackOutcome::FIndependentReadbackOutcome(
		const EVerificationStatus InStatus,
		const EEffectOutcome InEffectOutcome,
		FString InDiagnosticCode,
		FString InEvidenceId,
		FUeAgentCanonicalValuePtr InActualValue,
		FUeAgentSemanticComparison InComparison)
		: Status(InStatus)
		, EffectOutcome(InEffectOutcome)
		, DiagnosticCode(NormalizeText(MoveTemp(InDiagnosticCode)))
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
		return DiagnosticCode;
	}

	const FString& FIndependentReadbackOutcome::GetEvidenceId() const
	{
		return EvidenceId;
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
			OutError = FVerificationError(
				TEXT("independent_readback_oracle_catalog_empty"));
			return false;
		}
		TSet<FString> Ids;
		for (const TSharedRef<
			IIndependentReadbackOracle,
			ESPMode::ThreadSafe>& Oracle : InOracles)
		{
			const FString& Id = Oracle->GetOracleId();
			if (!IsOracleId(Id))
			{
				OutError = FVerificationError(
					TEXT("independent_readback_oracle_invalid"), Id);
				return false;
			}
			if (Ids.Contains(Id))
			{
				OutError = FVerificationError(
					TEXT("independent_readback_oracle_duplicate"), Id);
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
		OutOutcome = FIndependentReadbackOutcome();
		OutError = FVerificationError();
		if (!Request.IsValid())
		{
			OutError = FVerificationError(
				TEXT("independent_readback_request_invalid"));
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
			OutError = FVerificationError(
				TEXT("independent_readback_oracle_not_registered"),
				Request.GetOracleId());
			return false;
		}

		FIndependentReadbackObservation Observation;
		if (!(*Oracle)->TryRead(Request, Resources, Observation, OutError))
		{
			return false;
		}
		if (!Observation.IsValid())
		{
			OutError = FVerificationError(
				TEXT("independent_readback_observation_invalid"));
			return false;
		}
		if (!Observation.IsAvailable())
		{
			OutOutcome = FIndependentReadbackOutcome(
				EVerificationStatus::Unavailable,
				EEffectOutcome::StateUnknown,
				Observation.GetDiagnosticCode(),
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
			OutError = FVerificationError(
				CompareError.Code, CompareError.Path);
			return false;
		}
		const bool bEquivalent = Comparison.IsEquivalent();
		OutOutcome = FIndependentReadbackOutcome(
			bEquivalent
				? EVerificationStatus::Verified
				: EVerificationStatus::Mismatch,
			bEquivalent ? EEffectOutcome::Complete : EEffectOutcome::Partial,
			bEquivalent
				? TEXT("independent_readback_complete")
				: TEXT("independent_readback_semantic_mismatch"),
			Observation.GetEvidenceId(),
			Observation.GetActualValue(),
			MoveTemp(Comparison));
		return true;
	}
}

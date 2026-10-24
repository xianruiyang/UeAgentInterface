#pragma once

#include "CoreMinimal.h"
#include "Core/ArtifactPublisher/UeAgentFileArtifactPublisher.h"
#include "Core/ContextResource/UeAgentContextResource.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "Core/Verification/UeAgentVerification.h"

#define UAI_INDEPENDENT_READBACK_V1 1

namespace UE::UAI::Verification
{
	class UEAGENTINTERFACE_API FIndependentReadbackRequest final
	{
	public:
		FIndependentReadbackRequest(
			FString InOperationId,
			FString InOracleId,
			FString InSubjectRef,
			StateSemantic::FUeAgentCanonicalValueRef InExpectedValue,
			TOptional<ArtifactPublisher::FFileArtifactReference>
				InArtifact = {});

		const FString& GetOperationId() const;
		const FString& GetOracleId() const;
		const FString& GetSubjectRef() const;
		const StateSemantic::FUeAgentCanonicalValueRef&
			GetExpectedValue() const;
		const TOptional<ArtifactPublisher::FFileArtifactReference>&
			GetArtifact() const;
		bool IsValid() const;
		const FVerificationError* GetValidationDiagnostic() const;

	private:
		FString OperationId;
		FString OracleId;
		FString SubjectRef;
		StateSemantic::FUeAgentCanonicalValueRef ExpectedValue;
		TOptional<ArtifactPublisher::FFileArtifactReference> Artifact;
		TSharedPtr<const FVerificationError> ValidationDiagnostic;
	};

	class UEAGENTINTERFACE_API FIndependentReadbackObservation final
	{
	public:
		FIndependentReadbackObservation();

		static FIndependentReadbackObservation Available(
			StateSemantic::FUeAgentCanonicalValueRef InActualValue,
			FString InEvidenceId);
		static FIndependentReadbackObservation Unavailable(
			FVerificationError InDiagnostic,
			FString InEvidenceId = FString());

		bool IsValid() const;
		const FVerificationError* GetValidationDiagnostic() const;
		bool IsAvailable() const;
		const StateSemantic::FUeAgentCanonicalValuePtr&
			GetActualValue() const;
		const FString& GetEvidenceId() const;
		const FString& GetDiagnosticCode() const;
		const FVerificationError& GetDiagnostic() const;

	private:
		StateSemantic::FUeAgentCanonicalValuePtr ActualValue;
		FString EvidenceId;
		FVerificationError Diagnostic;
		bool bAvailable = false;
		TSharedPtr<const FVerificationError> ValidationDiagnostic;
	};

	class UEAGENTINTERFACE_API IIndependentReadbackOracle
	{
	public:
		virtual ~IIndependentReadbackOracle() = default;

		virtual const FString& GetOracleId() const = 0;
		virtual bool TryRead(
			const FIndependentReadbackRequest& Request,
			ContextResource::FUeAgentResourceTracker& Resources,
			FIndependentReadbackObservation& OutObservation,
			FVerificationError& OutError) = 0;
	};

	class UEAGENTINTERFACE_API FIndependentReadbackOutcome final
	{
	public:
		FIndependentReadbackOutcome() = default;
		FIndependentReadbackOutcome(
			EVerificationStatus InStatus,
			EEffectOutcome InEffectOutcome,
			FVerificationError InDiagnostic,
			FString InEvidenceId,
			StateSemantic::FUeAgentCanonicalValuePtr InActualValue,
			StateSemantic::FUeAgentSemanticComparison InComparison);

		EVerificationStatus GetStatus() const;
		EEffectOutcome GetEffectOutcome() const;
		const FString& GetDiagnosticCode() const;
		const FVerificationError& GetDiagnostic() const;
		const FString& GetEvidenceId() const;
		FVerificationError GetFailureDiagnostic(const FVerificationError& Error = FVerificationError()) const;
		const StateSemantic::FUeAgentCanonicalValuePtr&
			GetActualValue() const;
		const StateSemantic::FUeAgentSemanticComparison&
			GetComparison() const;

	private:
		EVerificationStatus Status = EVerificationStatus::Unavailable;
		EEffectOutcome EffectOutcome = EEffectOutcome::StateUnknown;
		FVerificationError Diagnostic;
		FString EvidenceId;
		StateSemantic::FUeAgentCanonicalValuePtr ActualValue;
		StateSemantic::FUeAgentSemanticComparison Comparison;
	};

	class UEAGENTINTERFACE_API FIndependentReadbackService final
	{
	public:
		static bool TryCreate(
			const TArray<TSharedRef<
				IIndependentReadbackOracle,
				ESPMode::ThreadSafe>>& InOracles,
			TSharedPtr<FIndependentReadbackService>& OutService,
			FVerificationError& OutError);

		bool TryVerify(
			const FIndependentReadbackRequest& Request,
			ContextResource::FUeAgentResourceTracker& Resources,
			FIndependentReadbackOutcome& OutOutcome,
			FVerificationError& OutError) const;
		// Production observation preserves the native result. Semantic mismatch
		// or unavailable comparison is a warning; actual oracle read failures
		// remain errors. TryVerify retains the pure development-test contract.
		bool TryObserve(
			const FIndependentReadbackRequest& Request,
			ContextResource::FUeAgentResourceTracker& Resources,
			FIndependentReadbackOutcome& OutOutcome,
			FVerificationError& OutError) const;

	private:
		bool TryReadAndCompare(const FIndependentReadbackRequest& Request,
			ContextResource::FUeAgentResourceTracker& Resources,
			FIndependentReadbackOutcome& OutOutcome, FVerificationError& OutError,
			bool bObservationOnly) const;
		explicit FIndependentReadbackService(
			TArray<TSharedRef<
				IIndependentReadbackOracle,
				ESPMode::ThreadSafe>> InOracles);

		TArray<TSharedRef<IIndependentReadbackOracle, ESPMode::ThreadSafe>>
			Oracles;
	};
}


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

	private:
		FString OperationId;
		FString OracleId;
		FString SubjectRef;
		StateSemantic::FUeAgentCanonicalValueRef ExpectedValue;
		TOptional<ArtifactPublisher::FFileArtifactReference> Artifact;
		bool bValid = false;
	};

	class UEAGENTINTERFACE_API FIndependentReadbackObservation final
	{
	public:
		FIndependentReadbackObservation() = default;

		static FIndependentReadbackObservation Available(
			StateSemantic::FUeAgentCanonicalValueRef InActualValue,
			FString InEvidenceId);
		static FIndependentReadbackObservation Unavailable(
			FString InDiagnosticCode,
			FString InEvidenceId = FString());

		bool IsValid() const;
		bool IsAvailable() const;
		const StateSemantic::FUeAgentCanonicalValuePtr&
			GetActualValue() const;
		const FString& GetEvidenceId() const;
		const FString& GetDiagnosticCode() const;

	private:
		StateSemantic::FUeAgentCanonicalValuePtr ActualValue;
		FString EvidenceId;
		FString DiagnosticCode;
		bool bAvailable = false;
		bool bValid = false;
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
			FString InDiagnosticCode,
			FString InEvidenceId,
			StateSemantic::FUeAgentCanonicalValuePtr InActualValue,
			StateSemantic::FUeAgentSemanticComparison InComparison);

		EVerificationStatus GetStatus() const;
		EEffectOutcome GetEffectOutcome() const;
		const FString& GetDiagnosticCode() const;
		const FString& GetEvidenceId() const;
		const StateSemantic::FUeAgentCanonicalValuePtr&
			GetActualValue() const;
		const StateSemantic::FUeAgentSemanticComparison&
			GetComparison() const;

	private:
		EVerificationStatus Status = EVerificationStatus::Unavailable;
		EEffectOutcome EffectOutcome = EEffectOutcome::StateUnknown;
		FString DiagnosticCode;
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

	private:
		explicit FIndependentReadbackService(
			TArray<TSharedRef<
				IIndependentReadbackOracle,
				ESPMode::ThreadSafe>> InOracles);

		TArray<TSharedRef<IIndependentReadbackOracle, ESPMode::ThreadSafe>>
			Oracles;
	};
}


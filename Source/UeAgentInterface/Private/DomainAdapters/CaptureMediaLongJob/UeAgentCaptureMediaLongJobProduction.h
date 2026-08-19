#pragma once

#include "CoreMinimal.h"
#include "Core/ArtifactPublisher/UeAgentFileArtifactPublisher.h"
#include "DomainAdapters/CaptureMediaLongJob/UeAgentCaptureMediaLongJobAdapter.h"

#define UAI_CAPTURE_MEDIA_LONG_JOB_PRODUCTION_V1 1

namespace UE::UAI::DomainAdapters::CaptureMediaLongJob
{
	class UEAGENTINTERFACE_API
	FUeAgentCaptureMediaLongJobPolicyDescriptor final
	{
	public:
		FUeAgentCaptureMediaLongJobPolicyDescriptor(
			EUeAgentCaptureMediaLongJobFamily InFamily,
			FString InPolicyId,
			FString InNativeMechanismId,
			FString InReadbackOracleId,
			Contract::EUeAgentStatePlane InRequiredStatePlane,
			TArray<ContextResource::EUeAgentExecutionWorldType>
				InAllowedWorldTypes);

		EUeAgentCaptureMediaLongJobFamily GetFamily() const;
		const FString& GetPolicyId() const;
		const FString& GetNativeMechanismId() const;
		const FString& GetReadbackOracleId() const;
		Contract::EUeAgentStatePlane GetRequiredStatePlane() const;
		const TArray<ContextResource::EUeAgentExecutionWorldType>&
			GetAllowedWorldTypes() const;
		bool IsValid() const;

	private:
		EUeAgentCaptureMediaLongJobFamily Family;
		FString PolicyId;
		FString NativeMechanismId;
		FString ReadbackOracleId;
		Contract::EUeAgentStatePlane RequiredStatePlane;
		TArray<ContextResource::EUeAgentExecutionWorldType>
			AllowedWorldTypes;
	};

	class UEAGENTINTERFACE_API IUeAgentCaptureMediaLongJobPortBuilder
	{
	public:
		virtual ~IUeAgentCaptureMediaLongJobPortBuilder() = default;

		virtual bool TryCreatePort(
			const FUeAgentCaptureMediaLongJobTypedRequest& Request,
			const FUeAgentCaptureMediaLongJobPolicyDescriptor& Policy,
			TSharedPtr<
				ExecutionMutation::ILongRunningOperationPort,
				ESPMode::ThreadSafe>& OutPort,
			ExecutionMutation::FExecutionMutationError& OutError) = 0;
	};

	class UEAGENTINTERFACE_API
	FUeAgentCaptureMediaLongJobPolicyRegistration final
	{
	public:
		FUeAgentCaptureMediaLongJobPolicyRegistration(
			FUeAgentCaptureMediaLongJobPolicyDescriptor InDescriptor,
			TSharedRef<
				IUeAgentCaptureMediaLongJobPortBuilder,
				ESPMode::ThreadSafe> InBuilder);

		const FUeAgentCaptureMediaLongJobPolicyDescriptor&
			GetDescriptor() const;
		IUeAgentCaptureMediaLongJobPortBuilder& GetBuilder() const;

	private:
		FUeAgentCaptureMediaLongJobPolicyDescriptor Descriptor;
		TSharedRef<IUeAgentCaptureMediaLongJobPortBuilder,
			ESPMode::ThreadSafe> Builder;
	};

	class UEAGENTINTERFACE_API
	FUeAgentCaptureMediaLongJobProductionPortFactory final
		: public IUeAgentCaptureMediaLongJobPortFactory
	{
	public:
		static bool TryCreate(
			const TArray<FUeAgentCaptureMediaLongJobPolicyRegistration>&
				Registrations,
			TSharedPtr<
				FUeAgentCaptureMediaLongJobProductionPortFactory>& OutFactory,
			ExecutionMutation::FExecutionMutationError& OutError);

		const TArray<FUeAgentCaptureMediaLongJobPolicyDescriptor>&
			GetPolicyDescriptors() const;

		bool TryCreateOperationPort(
			const FUeAgentCaptureMediaLongJobTypedRequest& Request,
			TSharedPtr<
				ExecutionMutation::ILongRunningOperationPort,
				ESPMode::ThreadSafe>& OutPort,
			ExecutionMutation::FExecutionMutationError& OutError) override;

	private:
		explicit FUeAgentCaptureMediaLongJobProductionPortFactory(
			TArray<FUeAgentCaptureMediaLongJobPolicyRegistration>
				InRegistrations);

		TArray<FUeAgentCaptureMediaLongJobPolicyRegistration> Registrations;
		TArray<FUeAgentCaptureMediaLongJobPolicyDescriptor> Descriptors;
	};

	class UEAGENTINTERFACE_API
	FUeAgentCaptureMediaNativeObservation final
	{
	public:
		FUeAgentCaptureMediaNativeObservation() = default;
		FUeAgentCaptureMediaNativeObservation(
			ExecutionMutation::ELongRunningWorkState InState,
			FString InPhase,
			FString InErrorCode = FString(),
			TOptional<ArtifactPublisher::FFileArtifactReference>
				InArtifact = {});

		ExecutionMutation::ELongRunningWorkState GetState() const;
		const FString& GetPhase() const;
		const FString& GetErrorCode() const;
		const TOptional<ArtifactPublisher::FFileArtifactReference>&
			GetArtifact() const;
		bool IsValid() const;

	private:
		ExecutionMutation::ELongRunningWorkState State =
			ExecutionMutation::ELongRunningWorkState::Invalid;
		FString Phase;
		FString ErrorCode;
		TOptional<ArtifactPublisher::FFileArtifactReference> Artifact;
	};

	class UEAGENTINTERFACE_API IUeAgentCaptureMediaNativeSession
	{
	public:
		virtual ~IUeAgentCaptureMediaNativeSession() = default;

		virtual bool Start(
			const ExecutionMutation::FLongRunningOperationRequest& Request,
			const ContextResource::FUeAgentCancellationToken& Cancellation,
			FString& OutNativeResourceId,
			ExecutionMutation::FExecutionMutationError& OutError) = 0;
		virtual bool Observe(
			FUeAgentCaptureMediaNativeObservation& OutObservation,
			ExecutionMutation::FExecutionMutationError& OutError) = 0;
		virtual bool RequestStop(
			const FString& Reason,
			ExecutionMutation::FExecutionMutationError& OutError) = 0;
		virtual bool Join(
			ExecutionMutation::FExecutionMutationError& OutError) = 0;
		virtual bool Release(
			ExecutionMutation::FExecutionMutationError& OutError) = 0;
	};

	UEAGENTINTERFACE_API bool TryCreateCaptureMediaNativeResourcePort(
		const FUeAgentCaptureMediaLongJobTypedRequest& Request,
		const FUeAgentCaptureMediaLongJobPolicyDescriptor& Policy,
		TSharedRef<IUeAgentCaptureMediaNativeSession, ESPMode::ThreadSafe>
			NativeSession,
		TSharedPtr<
			ExecutionMutation::ILongRunningOperationPort,
			ESPMode::ThreadSafe>& OutPort,
		ExecutionMutation::FExecutionMutationError& OutError);
}


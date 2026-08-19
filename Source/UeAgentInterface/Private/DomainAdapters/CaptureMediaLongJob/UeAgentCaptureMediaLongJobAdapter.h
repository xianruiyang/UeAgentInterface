#pragma once

#include "CoreMinimal.h"
#include "Core/ExecutionMutation/UeAgentLongRunningOperation.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "DomainAdapters/CaptureMediaLongJob/UeAgentCaptureMediaLongJobContract.h"

#define UAI_CAPTURE_MEDIA_LONG_JOB_ADAPTER_V1 1

namespace UE::UAI::DomainAdapters::CaptureMediaLongJob
{
	class UEAGENTINTERFACE_API
	FUeAgentCaptureMediaLongJobTypedRequest final
	{
	public:
		FUeAgentCaptureMediaLongJobTypedRequest(
			EUeAgentCaptureMediaLongJobFamily InFamily,
			FString InSubjectRef,
			FString InPolicyId,
			StateSemantic::FUeAgentCanonicalValueRef InParameters);

		EUeAgentCaptureMediaLongJobFamily GetFamily() const;
		const FString& GetSubjectRef() const;
		const FString& GetPolicyId() const;
		const StateSemantic::FUeAgentCanonicalValueRef&
			GetParameters() const;
		bool IsValid() const;

	private:
		EUeAgentCaptureMediaLongJobFamily Family;
		FString SubjectRef;
		FString PolicyId;
		StateSemantic::FUeAgentCanonicalValueRef Parameters;
	};

	class UEAGENTINTERFACE_API
	IUeAgentCaptureMediaLongJobPortFactory
	{
	public:
		virtual ~IUeAgentCaptureMediaLongJobPortFactory() = default;

		virtual bool TryCreateOperationPort(
			const FUeAgentCaptureMediaLongJobTypedRequest& Request,
			TSharedPtr<
				ExecutionMutation::ILongRunningOperationPort,
				ESPMode::ThreadSafe>& OutPort,
			ExecutionMutation::FExecutionMutationError& OutError) = 0;
	};

	UEAGENTINTERFACE_API bool TryAcceptCaptureMediaLongJob(
		const FUeAgentCaptureMediaLongJobTypedRequest& TypedRequest,
		const ExecutionMutation::FLongRunningOperationRequest&
			OperationRequest,
		IUeAgentCaptureMediaLongJobPortFactory& PortFactory,
		ExecutionMutation::FLongRunningOperationOwner& Owner,
		ExecutionMutation::FLongRunningOperationSnapshot& OutSnapshot,
		ExecutionMutation::FExecutionMutationError& OutError);
}

#pragma once

#include "CoreMinimal.h"
#include "Core/ExecutionMutation/UeAgentLongRunningOperation.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "DomainAdapters/CompileBuildJob/UeAgentCompileBuildJobContract.h"

#define UAI_COMPILE_BUILD_JOB_ADAPTER_V1 1

namespace UE::UAI::DomainAdapters::CompileBuildJob
{
	class UEAGENTINTERFACE_API
	FUeAgentCompileBuildJobTypedRequest final
	{
	public:
		FUeAgentCompileBuildJobTypedRequest(
			EUeAgentCompileBuildJobFamily InFamily,
			FString InSubjectRef,
			FString InPolicyId,
			StateSemantic::FUeAgentCanonicalValueRef InParameters);

		EUeAgentCompileBuildJobFamily GetFamily() const;
		const FString& GetSubjectRef() const;
		const FString& GetPolicyId() const;
		const StateSemantic::FUeAgentCanonicalValueRef&
			GetParameters() const;
		bool IsValid() const;
		const ExecutionMutation::FExecutionMutationError& GetValidationDiagnostic() const;

	private:
		EUeAgentCompileBuildJobFamily Family;
		FString SubjectRef;
		FString PolicyId;
		StateSemantic::FUeAgentCanonicalValueRef Parameters;
		ExecutionMutation::FExecutionMutationError ValidationDiagnostic;
	};

	class UEAGENTINTERFACE_API IUeAgentCompileBuildJobPortFactory
	{
	public:
		virtual ~IUeAgentCompileBuildJobPortFactory() = default;

		virtual bool TryCreateOperationPort(
			const FUeAgentCompileBuildJobTypedRequest& Request,
			TSharedPtr<
				ExecutionMutation::ILongRunningOperationPort,
				ESPMode::ThreadSafe>& OutPort,
			ExecutionMutation::FExecutionMutationError& OutError) = 0;
	};

	UEAGENTINTERFACE_API bool TryAcceptCompileBuildJob(
		const FUeAgentCompileBuildJobTypedRequest& TypedRequest,
		const ExecutionMutation::FLongRunningOperationRequest&
			OperationRequest,
		IUeAgentCompileBuildJobPortFactory& PortFactory,
		ExecutionMutation::FLongRunningOperationOwner& Owner,
		ExecutionMutation::FLongRunningOperationSnapshot& OutSnapshot,
		ExecutionMutation::FExecutionMutationError& OutError);
}

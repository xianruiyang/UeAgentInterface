#pragma once

#include "CoreMinimal.h"
#include "Core/ExecutionMutation/UeAgentLongRunningOperation.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "DomainAdapters/EditorSessionOperation/UeAgentEditorSessionOperationContract.h"

#define UAI_EDITOR_SESSION_OPERATION_ADAPTER_V1 1

namespace UE::UAI::DomainAdapters::EditorSessionOperation
{
	class UEAGENTINTERFACE_API
	FUeAgentEditorSessionOperationTypedRequest final
	{
	public:
		FUeAgentEditorSessionOperationTypedRequest(
			EUeAgentEditorSessionOperationFamily InFamily,
			FString InSubjectRef,
			FString InPolicyId,
			StateSemantic::FUeAgentCanonicalValueRef InParameters);

		EUeAgentEditorSessionOperationFamily GetFamily() const;
		const FString& GetSubjectRef() const;
		const FString& GetPolicyId() const;
		const StateSemantic::FUeAgentCanonicalValueRef&
			GetParameters() const;
		bool IsValid() const;
		const ExecutionMutation::FExecutionMutationError& GetValidationDiagnostic() const;

	private:
		EUeAgentEditorSessionOperationFamily Family;
		FString SubjectRef;
		FString PolicyId;
		StateSemantic::FUeAgentCanonicalValueRef Parameters;
		ExecutionMutation::FExecutionMutationError ValidationDiagnostic;
	};

	class UEAGENTINTERFACE_API
	IUeAgentEditorSessionOperationPortFactory
	{
	public:
		virtual ~IUeAgentEditorSessionOperationPortFactory() = default;

		virtual bool TryCreateOperationPort(
			const FUeAgentEditorSessionOperationTypedRequest& Request,
			const ExecutionMutation::FLongRunningOperationRequest&
				OperationRequest,
			TSharedPtr<
				ExecutionMutation::ILongRunningOperationPort,
				ESPMode::ThreadSafe>& OutPort,
			ExecutionMutation::FExecutionMutationError& OutError) = 0;
	};

	UEAGENTINTERFACE_API bool TryAcceptEditorSessionOperation(
		const FUeAgentEditorSessionOperationTypedRequest& TypedRequest,
		const ExecutionMutation::FLongRunningOperationRequest&
			OperationRequest,
		IUeAgentEditorSessionOperationPortFactory& PortFactory,
		ExecutionMutation::FLongRunningOperationOwner& Owner,
		ExecutionMutation::FLongRunningOperationSnapshot& OutSnapshot,
		ExecutionMutation::FExecutionMutationError& OutError);
}

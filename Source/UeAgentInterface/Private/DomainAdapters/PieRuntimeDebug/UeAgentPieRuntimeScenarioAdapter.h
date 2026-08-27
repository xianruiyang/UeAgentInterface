#pragma once

#include "CoreMinimal.h"
#include "Core/ExecutionMutation/UeAgentLongRunningOperation.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"

namespace UE::UAI::DomainAdapters::PieRuntimeDebug
{
	class UEAGENTINTERFACE_API FUeAgentPieRuntimeScenarioTypedRequest final
	{
	public:
		FUeAgentPieRuntimeScenarioTypedRequest(
			FString InSessionRef,
			FString InPolicyId,
			StateSemantic::FUeAgentCanonicalValueRef InParameters);

		const FString& GetSessionRef() const;
		const FString& GetPolicyId() const;
		const StateSemantic::FUeAgentCanonicalValueRef& GetParameters() const;
		bool IsValid() const;

	private:
		FString SessionRef;
		FString PolicyId;
		StateSemantic::FUeAgentCanonicalValueRef Parameters;
	};

	class UEAGENTINTERFACE_API IUeAgentPieRuntimeScenarioPortFactory
	{
	public:
		virtual ~IUeAgentPieRuntimeScenarioPortFactory() = default;

		virtual bool TryCreateOperationPort(
			const FUeAgentPieRuntimeScenarioTypedRequest& Request,
			const ExecutionMutation::FLongRunningOperationRequest&
				OperationRequest,
			TSharedPtr<ExecutionMutation::ILongRunningOperationPort,
				ESPMode::ThreadSafe>& OutPort,
			ExecutionMutation::FExecutionMutationError& OutError) = 0;
	};

	UEAGENTINTERFACE_API bool TryAcceptPieRuntimeScenario(
		const FUeAgentPieRuntimeScenarioTypedRequest& TypedRequest,
		const ExecutionMutation::FLongRunningOperationRequest& OperationRequest,
		IUeAgentPieRuntimeScenarioPortFactory& PortFactory,
		ExecutionMutation::FLongRunningOperationOwner& Owner,
		ExecutionMutation::FLongRunningOperationSnapshot& OutSnapshot,
		ExecutionMutation::FExecutionMutationError& OutError);
}

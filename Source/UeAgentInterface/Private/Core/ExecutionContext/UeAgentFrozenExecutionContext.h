#pragma once

#include "CoreMinimal.h"
#include "Core/Contract/UeAgentExecutionContextPolicy.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"

namespace UE::UAI::ExecutionContext
{
	struct UEAGENTINTERFACE_API FUeAgentExecutionContextFact
	{
		Contract::EUeAgentExecutionContextFacet Facet =
			Contract::EUeAgentExecutionContextFacet::Invalid;
		FString StableIdentity;
	};

	struct UEAGENTINTERFACE_API FUeAgentExecutionContextError
	{
		FString Code;
		FString Detail;
	};

	class UEAGENTINTERFACE_API FUeAgentFrozenExecutionContext final
	{
	public:
		static bool TryFreeze(
			FString RequestId,
			FString ProjectId,
			Contract::FUeAgentExecutionContextPolicy Policy,
			Contract::EUeAgentStatePlane StatePlane,
			TArray<FUeAgentExecutionContextFact> Facts,
			StateSemantic::FUeAgentTemporalPoint FreshnessObservedAt,
			TSharedPtr<const FUeAgentFrozenExecutionContext>& OutContext,
			FUeAgentExecutionContextError& OutError);

		const FString& GetRequestId() const;
		const FString& GetProjectId() const;
		const Contract::FUeAgentExecutionContextPolicy& GetPolicy() const;
		Contract::EUeAgentStatePlane GetStatePlane() const;
		const TArray<FUeAgentExecutionContextFact>& GetFacts() const;
		FString FindFacet(Contract::EUeAgentExecutionContextFacet Facet) const;
		const StateSemantic::FUeAgentTemporalPoint&
			GetFreshnessObservedAt() const;
		bool IsValid() const;

	private:
		FUeAgentFrozenExecutionContext(
			FString InRequestId,
			FString InProjectId,
			Contract::FUeAgentExecutionContextPolicy InPolicy,
			Contract::EUeAgentStatePlane InStatePlane,
			TArray<FUeAgentExecutionContextFact> InFacts,
			StateSemantic::FUeAgentTemporalPoint InFreshnessObservedAt);

		FString RequestId;
		FString ProjectId;
		Contract::FUeAgentExecutionContextPolicy Policy;
		Contract::EUeAgentStatePlane StatePlane =
			Contract::EUeAgentStatePlane::Invalid;
		TArray<FUeAgentExecutionContextFact> Facts;
		StateSemantic::FUeAgentTemporalPoint FreshnessObservedAt;
		bool bValid = false;
	};
}

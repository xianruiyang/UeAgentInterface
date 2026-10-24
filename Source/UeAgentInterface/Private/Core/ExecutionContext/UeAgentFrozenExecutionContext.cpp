#include "Core/ExecutionContext/UeAgentFrozenExecutionContext.h"

namespace UE::UAI::ExecutionContext
{
	namespace
	{
		void Fail(
			FUeAgentExecutionContextError& OutError,
			const TCHAR* Code,
			FString Detail)
		{
			OutError.Code = Code;
			OutError.InternalContext = MoveTemp(Detail);
		}

		bool IsMeaningful(const FString& Value)
		{
			return !Value.TrimStartAndEnd().IsEmpty();
		}
	}

	FUeAgentFrozenExecutionContext::FUeAgentFrozenExecutionContext(
		FString InRequestId,
		FString InProjectId,
		Contract::FUeAgentExecutionContextPolicy InPolicy,
		const Contract::EUeAgentStatePlane InStatePlane,
		TArray<FUeAgentExecutionContextFact> InFacts,
		StateSemantic::FUeAgentTemporalPoint InFreshnessObservedAt)
		: RequestId(MoveTemp(InRequestId))
		, ProjectId(MoveTemp(InProjectId))
		, Policy(MoveTemp(InPolicy))
		, StatePlane(InStatePlane)
		, Facts(MoveTemp(InFacts))
		, FreshnessObservedAt(MoveTemp(InFreshnessObservedAt))
		, bValid(true)
	{
	}

	bool FUeAgentFrozenExecutionContext::TryFreeze(
		FString RequestId,
		FString ProjectId,
		Contract::FUeAgentExecutionContextPolicy Policy,
		const Contract::EUeAgentStatePlane StatePlane,
		TArray<FUeAgentExecutionContextFact> Facts,
		StateSemantic::FUeAgentTemporalPoint FreshnessObservedAt,
		TSharedPtr<const FUeAgentFrozenExecutionContext>& OutContext,
		FUeAgentExecutionContextError& OutError)
	{
		OutContext.Reset();
		OutError = {};
		if (!IsMeaningful(RequestId) || !IsMeaningful(ProjectId))
		{
			Fail(
				OutError,
				TEXT("execution_context_identity_missing"),
				TEXT("request_id and project_id must be explicit"));
			return false;
		}
		if (!Policy.IsValid())
		{
			Fail(
				OutError,
				TEXT("execution_context_policy_invalid"),
				TEXT("execution context policy is invalid"));
			return false;
		}
		if (!Policy.GetAllowedStatePlanes().Contains(StatePlane))
		{
			Fail(
				OutError,
				TEXT("execution_context_state_plane_not_allowed"),
				FString::Printf(
					TEXT("state plane '%s' is not allowed"),
					Contract::LexToString(StatePlane)));
			return false;
		}
		if (!FreshnessObservedAt.IsValid())
		{
			Fail(
				OutError,
				TEXT("execution_context_freshness_invalid"),
				TEXT("freshness observation must be explicit and valid"));
			return false;
		}

		TSet<Contract::EUeAgentExecutionContextFacet> SeenFacets;
		for (const FUeAgentExecutionContextFact& Fact : Facts)
		{
			if (FString(Contract::LexToString(Fact.Facet)).IsEmpty()
				|| !IsMeaningful(Fact.StableIdentity))
			{
				Fail(
					OutError,
					TEXT("execution_context_fact_invalid"),
					TEXT("every resolved facet requires a stable identity"));
				return false;
			}
			if (SeenFacets.Contains(Fact.Facet))
			{
				Fail(
					OutError,
					TEXT("execution_context_facet_duplicate"),
					FString::Printf(
						TEXT("facet '%s' is declared more than once"),
						Contract::LexToString(Fact.Facet)));
				return false;
			}
			SeenFacets.Add(Fact.Facet);
		}
		for (const Contract::EUeAgentExecutionContextFacet Required :
			Policy.GetRequiredFacets())
		{
			if (!SeenFacets.Contains(Required))
			{
				Fail(
					OutError,
					TEXT("execution_context_required_facet_missing"),
					FString::Printf(
						TEXT("required facet '%s' was not resolved before planning"),
						Contract::LexToString(Required)));
				return false;
			}
		}

		Facts.Sort(
			[](const FUeAgentExecutionContextFact& Left,
			   const FUeAgentExecutionContextFact& Right)
			{
				return FCString::Strcmp(
					Contract::LexToString(Left.Facet),
					Contract::LexToString(Right.Facet)) < 0;
			});
		OutContext = MakeShareable(new FUeAgentFrozenExecutionContext(
			MoveTemp(RequestId),
			MoveTemp(ProjectId),
			MoveTemp(Policy),
			StatePlane,
			MoveTemp(Facts),
			MoveTemp(FreshnessObservedAt)));
		return true;
	}

	const FString& FUeAgentFrozenExecutionContext::GetRequestId() const
	{
		return RequestId;
	}

	const FString& FUeAgentFrozenExecutionContext::GetProjectId() const
	{
		return ProjectId;
	}

	const Contract::FUeAgentExecutionContextPolicy&
	FUeAgentFrozenExecutionContext::GetPolicy() const
	{
		return Policy;
	}

	Contract::EUeAgentStatePlane
	FUeAgentFrozenExecutionContext::GetStatePlane() const
	{
		return StatePlane;
	}

	const TArray<FUeAgentExecutionContextFact>&
	FUeAgentFrozenExecutionContext::GetFacts() const
	{
		return Facts;
	}

	FString FUeAgentFrozenExecutionContext::FindFacet(
		const Contract::EUeAgentExecutionContextFacet Facet) const
	{
		for (const FUeAgentExecutionContextFact& Fact : Facts)
		{
			if (Fact.Facet == Facet)
			{
				return Fact.StableIdentity;
			}
		}
		return {};
	}

	const StateSemantic::FUeAgentTemporalPoint&
	FUeAgentFrozenExecutionContext::GetFreshnessObservedAt() const
	{
		return FreshnessObservedAt;
	}

	bool FUeAgentFrozenExecutionContext::IsValid() const
	{
		return bValid;
	}
}

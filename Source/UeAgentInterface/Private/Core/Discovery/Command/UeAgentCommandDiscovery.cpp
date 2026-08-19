#include "Core/Discovery/Command/UeAgentCommandDiscovery.h"

namespace UE::UAI::Discovery::Command
{
	using namespace CommandContract;
	using namespace CommandRegistry;

	namespace
	{
		struct FScoredCandidate final
		{
			FCommandCandidate Candidate;
			int32 Score = 0;
			TArray<FString> Reasons;
		};

		TArray<FString> IntentTokens(FString Intent)
		{
			Intent.TrimStartAndEndInline();
			Intent.ToLowerInline();
			for (TCHAR& Character : Intent)
			{
				if (!(FChar::IsAlnum(Character) || Character == TEXT('_')))
				{
					Character = TEXT(' ');
				}
			}
			TArray<FString> Tokens;
			Intent.ParseIntoArrayWS(Tokens);
			Tokens.RemoveAll([](const FString& Value)
			{
				return Value.Len() < 2;
			});
			Tokens.Sort();
			for (int32 Index = Tokens.Num() - 1; Index > 0; --Index)
			{
				if (Tokens[Index] == Tokens[Index - 1])
				{
					Tokens.RemoveAt(Index);
				}
			}
			return Tokens;
		}

		void ScoreText(
			const FString& Label,
			FString Text,
			const TArray<FString>& Tokens,
			const int32 Weight,
			FScoredCandidate& Scored)
		{
			Text.ToLowerInline();
			for (const FString& Token : Tokens)
			{
				if (Text.Contains(Token))
				{
					Scored.Score += Weight;
					Scored.Reasons.AddUnique(Label + TEXT(":") + Token);
				}
			}
		}

		FString JoinValues(const TArray<FString>& Values)
		{
			return FString::Join(Values, TEXT(" "));
		}
	}

	bool FCommandDiscoveryService::TryCreate(
		TSharedRef<const FUeAgentImmutableCommandRegistry> Registry,
		TSharedPtr<const FCommandDiscoveryService>& OutService,
		FCommandDiscoveryError& OutError)
	{
		OutService.Reset();
		OutError = {};
		if (Registry->GetContracts().IsEmpty())
		{
			OutError.Code = TEXT("command_registry_empty");
			OutError.Detail = TEXT("discovery requires an immutable command registry");
			return false;
		}
		OutService = MakeShareable(new FCommandDiscoveryService(Registry));
		return true;
	}

	bool FCommandDiscoveryService::Search(
		const FCommandSearchRequest& Request,
		FCommandSearchPage& OutPage,
		FCommandDiscoveryError& OutError) const
	{
		OutPage = {};
		OutError = {};
		const TArray<FString> Tokens = IntentTokens(Request.Intent);
		if (Tokens.IsEmpty())
		{
			OutError.Code = TEXT("command_intent_missing");
			OutError.Detail = TEXT("intent must contain at least one searchable token");
			return false;
		}
		if (Request.Limit < 1 || Request.Limit > 8 || Request.Cursor < 0)
		{
			OutError.Code = TEXT("command_search_page_invalid");
			OutError.Detail = TEXT("limit must be 1..8 and cursor must be non-negative");
			return false;
		}

		TArray<FScoredCandidate> Matches;
		for (const FUeAgentFrozenCommandContract& Contract : Registry->GetContracts())
		{
			if (Contract.GetAvailability() != EUeAgentCommandAvailability::Enabled)
			{
				continue;
			}
			FScoredCandidate Scored;
			Scored.Candidate.CommandId = Contract.GetCommandId();
			Scored.Candidate.ContractHash = Contract.GetContractHash();
			Scored.Candidate.Summary = Contract.GetPurpose().Summary;
			Scored.Candidate.Target = Contract.GetPurpose().Target;
			Scored.Candidate.bHasSideEffects =
				Contract.GetSideEffect() != EUeAgentCommandSideEffect::None;
			ScoreText(TEXT("id"), Contract.GetCommandId(), Tokens, 8, Scored);
			ScoreText(TEXT("keyword"), JoinValues(Contract.GetPurpose().Keywords), Tokens, 6, Scored);
			ScoreText(TEXT("synonym"), JoinValues(Contract.GetPurpose().Synonyms), Tokens, 5, Scored);
			ScoreText(TEXT("summary"), Contract.GetPurpose().Summary, Tokens, 3, Scored);
			ScoreText(TEXT("target"), Contract.GetPurpose().Target, Tokens, 2, Scored);
			if (Scored.Score > 0)
			{
				Scored.Candidate.MatchReason = FString::Join(Scored.Reasons, TEXT(","));
				Matches.Add(MoveTemp(Scored));
			}
		}
		Matches.Sort([](const FScoredCandidate& Left, const FScoredCandidate& Right)
		{
			return Left.Score == Right.Score
				? Left.Candidate.CommandId < Right.Candidate.CommandId
				: Left.Score > Right.Score;
		});

		OutPage.TotalMatches = Matches.Num();
		if (Request.Cursor > Matches.Num())
		{
			OutError.Code = TEXT("command_search_cursor_invalid");
			OutError.Detail = TEXT("cursor exceeds the current result set");
			return false;
		}
		const int32 End = FMath::Min(Request.Cursor + Request.Limit, Matches.Num());
		for (int32 Index = Request.Cursor; Index < End; ++Index)
		{
			OutPage.Candidates.Add(MoveTemp(Matches[Index].Candidate));
		}
		OutPage.bComplete = End >= Matches.Num();
		OutPage.NextCursor = OutPage.bComplete ? -1 : End;
		return true;
	}

	bool FCommandDiscoveryService::GetDetail(
		const FString& CommandId,
		const FString& ContractHash,
		FCommandDetail& OutDetail,
		FCommandDiscoveryError& OutError) const
	{
		OutDetail = {};
		OutError = {};
		const FUeAgentFrozenCommandContract* Contract = Registry->FindExact(CommandId);
		if (!Contract)
		{
			OutError.Code = TEXT("command_not_registered_exact");
			OutError.Detail = CommandId;
			return false;
		}
		if (Contract->GetContractHash() != ContractHash)
		{
			OutError.Code = TEXT("command_contract_version_mismatch");
			OutError.Detail = CommandId;
			return false;
		}
		OutDetail.CommandId = Contract->GetCommandId();
		OutDetail.ContractHash = Contract->GetContractHash();
		OutDetail.PythonNamespace = Contract->GetPythonNamespace();
		OutDetail.PythonFunction = Contract->GetPythonFunction();
		OutDetail.PublicOwner = Contract->GetPublicOwner();
		OutDetail.Purpose = Contract->GetPurpose();
		OutDetail.InputFields = Contract->GetInputFields();
		OutDetail.ResultFields = Contract->GetResultFields();
		OutDetail.Errors = Contract->GetErrors();
		OutDetail.TutorialReferences = {
			TEXT("uai-docs:python-sdk-and-cli#command-discovery"),
			TEXT("uai-skill:public-sdk-playbook#command-discovery"),
		};
		OutDetail.bHasSideEffects =
			Contract->GetSideEffect() != EUeAgentCommandSideEffect::None;
		return true;
	}

	FCommandDiscoveryService::FCommandDiscoveryService(
		TSharedRef<const FUeAgentImmutableCommandRegistry> InRegistry)
		: Registry(MoveTemp(InRegistry))
	{
	}
}

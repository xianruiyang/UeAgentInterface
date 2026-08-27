#include "Core/Discovery/Command/UeAgentCommandDiscovery.h"

#include "Core/Contract/Command/UeAgentCommandLifecyclePolicy.h"

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
			TSet<FString> MatchedTokens;
			TArray<FString> Reasons;
		};

		TArray<FString> SearchableTokens(FString Text)
		{
			Text.TrimStartAndEndInline();
			Text.ToLowerInline();
			for (TCHAR& Character : Text)
			{
				if (!FChar::IsAlnum(Character))
				{
					Character = TEXT(' ');
				}
			}
			TArray<FString> Tokens;
			Text.ParseIntoArrayWS(Tokens);
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

		TArray<FString> IntentTokens(FString Intent)
		{
			return SearchableTokens(MoveTemp(Intent));
		}

		void ScoreText(
			const FString& Label,
			FString Text,
			const TArray<FString>& Tokens,
			const int32 Weight,
			FScoredCandidate& Scored)
		{
			TSet<FString> TextTokens;
			for (FString& TextToken : SearchableTokens(MoveTemp(Text)))
			{
				TextTokens.Add(MoveTemp(TextToken));
			}
			for (const FString& Token : Tokens)
			{
				if (TextTokens.Contains(Token))
				{
					Scored.Score += Weight;
					Scored.MatchedTokens.Add(Token);
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
			const EUeAgentCommandAvailability Availability =
				Contract.GetAvailability();
			if (Availability == EUeAgentCommandAvailability::Disabled
				|| (Availability == EUeAgentCommandAvailability::Dormant
					&& Request.Mode != ECommandSearchMode::Compatibility))
			{
				continue;
			}
			const CommandLifecycle::FCommandLifecyclePolicy* Lifecycle =
				CommandLifecycle::FindExact(Contract.GetCommandId());
			FScoredCandidate Scored;
			Scored.Candidate.CommandId = Contract.GetCommandId();
			Scored.Candidate.ContractHash = Contract.GetContractHash();
			Scored.Candidate.Summary = Contract.GetPurpose().Summary;
			Scored.Candidate.Target = Contract.GetPurpose().Target;
			Scored.Candidate.LifecycleStatus =
				CommandLifecycle::LexToString(Availability);
			if (Lifecycle)
			{
				Scored.Candidate.SuccessorCommandIds =
					Lifecycle->SuccessorCommandIds;
				Scored.Candidate.bRequiresUserConfirmation =
					Lifecycle->bRequiresUserConfirmation;
			}
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
			for (const FUeAgentCommandVariantDeclaration& Variant :
				Contract.GetVariants())
			{
				FScoredCandidate VariantScored;
				VariantScored.Candidate.CommandId = Contract.GetCommandId();
				VariantScored.Candidate.ContractHash = Contract.GetContractHash();
				VariantScored.Candidate.VariantId = Variant.VariantId;
				VariantScored.Candidate.Summary = Variant.Purpose.Summary;
				VariantScored.Candidate.Target = Variant.Purpose.Target;
				VariantScored.Candidate.LifecycleStatus =
					CommandLifecycle::LexToString(Availability);
				if (Lifecycle)
				{
					VariantScored.Candidate.SuccessorCommandIds =
						Lifecycle->SuccessorCommandIds;
					VariantScored.Candidate.bRequiresUserConfirmation =
						Lifecycle->bRequiresUserConfirmation;
				}
				VariantScored.Candidate.bHasSideEffects =
					Contract.GetSideEffect() != EUeAgentCommandSideEffect::None;
				ScoreText(
					TEXT("command_id"),
					Contract.GetCommandId(),
					Tokens,
					8,
					VariantScored);
				ScoreText(
					TEXT("variant_id"),
					Variant.VariantId,
					Tokens,
					10,
					VariantScored);
				ScoreText(
					TEXT("variant_keyword"),
					JoinValues(Variant.Purpose.Keywords),
					Tokens,
					8,
					VariantScored);
				ScoreText(
					TEXT("variant_synonym"),
					JoinValues(Variant.Purpose.Synonyms),
					Tokens,
					7,
					VariantScored);
				ScoreText(
					TEXT("variant_summary"),
					Variant.Purpose.Summary,
					Tokens,
					6,
					VariantScored);
				ScoreText(
					TEXT("variant_target"),
					Variant.Purpose.Target,
					Tokens,
					4,
					VariantScored);
				if (VariantScored.Score > 0)
				{
					VariantScored.Candidate.MatchReason = FString::Join(
						VariantScored.Reasons, TEXT(","));
					Matches.Add(MoveTemp(VariantScored));
				}
			}
		}
		Matches.Sort([](const FScoredCandidate& Left, const FScoredCandidate& Right)
		{
			const FString LeftIdentity = Left.Candidate.CommandId
				+ TEXT("\n") + Left.Candidate.VariantId;
			const FString RightIdentity = Right.Candidate.CommandId
				+ TEXT("\n") + Right.Candidate.VariantId;
			if (Left.MatchedTokens.Num() != Right.MatchedTokens.Num())
			{
				return Left.MatchedTokens.Num() > Right.MatchedTokens.Num();
			}
			return Left.Score == Right.Score
				? LeftIdentity < RightIdentity
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
		return GetDetail(
			CommandId,
			ContractHash,
			FString(),
			OutDetail,
			OutError);
	}

	bool FCommandDiscoveryService::GetDetail(
		const FString& CommandId,
		const FString& ContractHash,
		const FString& VariantId,
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
		if (Contract->GetAvailability()
			== EUeAgentCommandAvailability::Disabled)
		{
			OutError.Code = TEXT("command_disabled");
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
		const FUeAgentCommandVariantDeclaration* Variant = nullptr;
		if (!VariantId.IsEmpty())
		{
			Variant = Contract->GetVariants().FindByPredicate(
				[&VariantId](
					const FUeAgentCommandVariantDeclaration& Candidate)
				{
					return Candidate.VariantId == VariantId;
				});
			if (Variant == nullptr)
			{
				OutError.Code = TEXT("command_variant_not_registered_exact");
				OutError.Detail = CommandId + TEXT(":") + VariantId;
				return false;
			}
		}
		OutDetail.VariantId = VariantId;
		OutDetail.Purpose = Variant
			? Variant->Purpose
			: Contract->GetPurpose();
		OutDetail.RequestSchemaJson = Variant
			? Variant->RequestSchemaJson
			: FString();
		OutDetail.InputFields = Contract->GetInputFields();
		OutDetail.ResultFields = Contract->GetResultFields();
		OutDetail.Errors = Contract->GetErrors();
		OutDetail.TutorialReferences = Contract->GetTutorialReferences();
		OutDetail.LifecycleStatus = CommandLifecycle::LexToString(
			Contract->GetAvailability());
		if (const CommandLifecycle::FCommandLifecyclePolicy* Lifecycle =
			CommandLifecycle::FindExact(CommandId))
		{
			OutDetail.SuccessorCommandIds = Lifecycle->SuccessorCommandIds;
			OutDetail.LifecycleReason = Lifecycle->Reason;
			OutDetail.bRequiresUserConfirmation =
				Lifecycle->bRequiresUserConfirmation;
		}
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

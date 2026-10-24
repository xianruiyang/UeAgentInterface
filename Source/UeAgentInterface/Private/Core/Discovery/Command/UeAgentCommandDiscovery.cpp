#include "Core/Discovery/Command/UeAgentCommandDiscovery.h"
#include "Core/Contract/UeAgentDiagnostic.h"

#include "Core/ContentDiscovery/UeAgentNameDiscoveryProjection.h"

#include "Core/Contract/Command/UeAgentCommandLifecyclePolicy.h"
#include "Dom/JsonValue.h"

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
			TMap<FString, int32> TokenScores;
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

		void ScoreText(
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
					const int32 Previous = Scored.TokenScores.FindRef(Token);
					if (Weight > Previous)
					{
						Scored.Score += Weight - Previous;
						Scored.TokenScores.Add(Token, Weight);
					}
					Scored.MatchedTokens.Add(Token);
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
			OutError = FCommandDiscoveryError::Failure(TEXT("command_registry_empty"),
				TEXT("Command discovery cannot start because the command registry is empty."), TEXT("command_discovery_setup"));
			OutError.ActualValue = MakeShared<FJsonValueNumber>(0);
			OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("at least one registered command"));
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
		const TArray<FString> Tokens = SearchableTokens(Request.Intent);
		if (Tokens.IsEmpty())
		{
			OutError = FCommandDiscoveryError::Failure(TEXT("command_intent_missing"),
				TEXT("The search intent must contain at least one searchable word."), TEXT("command_search"));
			OutError.Source.JsonPath = TEXT("/intent");
			OutError.ActualValue = MakeShared<FJsonValueString>(Request.Intent);
			return false;
		}
		const FString ExactIntent = Request.Intent.TrimStartAndEnd();
		if (const FUeAgentFrozenCommandContract* ExactContract =
			Registry->FindExact(ExactIntent))
		{
			if (ExactContract->GetAvailability()
				== EUeAgentCommandAvailability::Enabled)
			{
				OutPage.TotalMatches = 1;
				OutPage.ReturnedCount = 1;
				FCommandCandidate Candidate;
				Candidate.CommandId = ExactContract->GetCommandId();
				OutPage.Candidates.Add(MoveTemp(Candidate));
			}
			return true;
		}
		TArray<FScoredCandidate> Matches;
		for (const FUeAgentFrozenCommandContract& Contract : Registry->GetContracts())
		{
			const EUeAgentCommandAvailability Availability =
				Contract.GetAvailability();
			if (Availability != EUeAgentCommandAvailability::Enabled)
			{
				continue;
			}
			FScoredCandidate Scored;
			Scored.Candidate.CommandId = Contract.GetCommandId();
			ScoreText(Contract.GetCommandId(), Tokens, 8, Scored);
			ScoreText(JoinValues(Contract.GetPurpose().Keywords), Tokens, 6, Scored);
			ScoreText(JoinValues(Contract.GetPurpose().Synonyms), Tokens, 5, Scored);
			for (const FUeAgentCommandVariantDeclaration& Variant :
				Contract.GetVariants())
			{
				ScoreText(
					Variant.VariantId,
					Tokens,
					10,
					Scored);
				ScoreText(
					JoinValues(Variant.Purpose.Keywords),
					Tokens,
					8,
					Scored);
				ScoreText(
					JoinValues(Variant.Purpose.Synonyms),
					Tokens,
					7,
					Scored);
			}
			if (Scored.Score > 0)
			{
				Matches.Add(MoveTemp(Scored));
			}
		}
		Matches.Sort([](const FScoredCandidate& Left, const FScoredCandidate& Right)
		{
			const FString& LeftIdentity = Left.Candidate.CommandId;
			const FString& RightIdentity = Right.Candidate.CommandId;
			if (Left.MatchedTokens.Num() != Right.MatchedTokens.Num())
			{
				return Left.MatchedTokens.Num() > Right.MatchedTokens.Num();
			}
			return Left.Score == Right.Score
				? LeftIdentity < RightIdentity
				: Left.Score > Right.Score;
		});

		OutPage.TotalMatches = Matches.Num();
		OutPage.ReturnedCount = Matches.Num();
		UE::UAI::Contract::FDiagnostic CompletenessError;
		const ContentDiscovery::FNameDiscoveryCompleteness Completeness{
			OutPage.TotalMatches,
			OutPage.ReturnedCount};
		if (!ContentDiscovery::TryValidateNameDiscoveryCompleteness(
				Completeness, CompletenessError))
		{
			OutPage = {};
			OutError = MoveTemp(CompletenessError);
			return false;
		}
		const int32 End = Matches.Num();
		for (int32 Index = 0; Index < End; ++Index)
		{
			OutPage.Candidates.Add(MoveTemp(Matches[Index].Candidate));
		}
		return true;
	}

	bool FCommandDiscoveryService::GetDetail(
		const FString& CommandId,
		const FString& VariantId,
		FCommandDetail& OutDetail,
		FCommandDiscoveryError& OutError) const
	{
		OutDetail = {};
		OutError = {};
		const FUeAgentFrozenCommandContract* Contract = Registry->FindExact(CommandId);
		if (!Contract)
		{
			OutError = FCommandDiscoveryError::Failure(TEXT("command_not_registered_exact"),
				TEXT("No command is registered with this exact name."), TEXT("command_discovery"));
			OutError.CandidateName = CommandId;
			OutError.SuggestedFix = TEXT("Use query.command.search to find a registered command name.");
			return false;
		}
		if (Contract->GetAvailability()
			== EUeAgentCommandAvailability::Disabled)
		{
			OutError = FCommandDiscoveryError::Failure(TEXT("command_disabled"),
				TEXT("This command is disabled in the current Editor."), TEXT("command_discovery"));
			OutError.CandidateName = CommandId;
			return false;
		}
		OutDetail.CommandId = Contract->GetCommandId();
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
				OutError = FCommandDiscoveryError::Failure(TEXT("command_variant_not_registered_exact"),
					TEXT("The requested variant is not registered for this command."), TEXT("command_discovery"));
				OutError.CandidateName = CommandId;
				OutError.ActualValue = MakeShared<FJsonValueString>(VariantId);
				TArray<TSharedPtr<FJsonValue>> Variants;
				for (const auto& Candidate : Contract->GetVariants())
					Variants.Add(MakeShared<FJsonValueString>(Candidate.VariantId));
				OutError.ExpectedValue = MakeShared<FJsonValueArray>(MoveTemp(Variants));
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

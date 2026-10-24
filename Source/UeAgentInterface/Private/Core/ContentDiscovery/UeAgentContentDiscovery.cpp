#include "Core/ContentDiscovery/UeAgentContentDiscovery.h"

#include "Core/ContentDiscovery/UeAgentNameDiscoveryProjection.h"

#include "Algo/Sort.h"
#include "Dom/JsonValue.h"
#include "Misc/SecureHash.h"

namespace UE::UAI::ContentDiscovery
{
	bool FUeAgentContentDiscoveryService::TryResolveReadableName(
		const FString& Name, FContentDiscoveryItem& OutItem, FContentDiscoveryError& OutError) const
	{
		OutItem = {};
		OutError = {};
		FContentDiscoveryRequest Request;
		for (const auto& Provider : Providers)
			for (const auto& Kind : Provider.Value->GetContentKinds())
				Request.ContentKinds.AddUnique(Kind);
		Request.RequiredOperations = {TEXT("query")};
		FContentDiscoverySearchResult Result;
		if (!TrySearch(Request, Result, OutError)) return false;
		if (Result.HasFailures())
		{
			OutError = Result.GetFailureDiagnostic();
			return false;
		}
		TArray<FContentDiscoveryItem> Items;
		TArray<FScopedNameDiscoveryEntry> Names;
		for (const auto& Provider : Result.Providers)
		{
			if (!Provider.Page.IsSet()) continue;
			for (const auto& Item : Provider.Page->Items)
			{
				Items.Add(Item);
				Names.Add({Item.DisplayName, Item.PublicDiscoveryScopes, Item.Identity});
			}
		}
		int32 Index = INDEX_NONE;
		if (!TryResolveScopedDiscoveryName(Names, Name, ENameComparison::IgnoreCase, Index, OutError))
		{
			OutError.SubjectId = Name;
			return false;
		}
		OutItem = Items[Index];
		return true;
	}

	namespace
	{
		bool IsExactText(const FString& Value)
		{
			return !Value.IsEmpty() && Value == Value.TrimStartAndEnd();
		}

		FContentDiscoveryError Invalid(const TCHAR* Code, const FString& Reason, const FString& Path)
		{
			auto Error = FContentDiscoveryError::Failure(Code, Reason, TEXT("content_search"));
			Error.PropertyPath = Path;
			return Error;
		}

		FString ChildPath(const FString& Path, const TCHAR* Field)
		{
			return Path.IsEmpty() ? FString(Field) : Path + TEXT(".") + Field;
		}

		bool ValidateText(const FString& Value, const TCHAR* Code, const TCHAR* Reason,
			const FString& Path, FContentDiscoveryError& OutError, bool bPrivate = false)
		{
			if (IsExactText(Value)) return true;
			OutError = Invalid(Code, Reason, Path);
			if (bPrivate) OutError.ActualEvidence = MakeShared<FJsonValueString>(Value);
			else OutError.ActualValue = MakeShared<FJsonValueString>(Value);
			OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("non-empty text without surrounding whitespace"));
			return false;
		}

		bool ValidateUniqueTexts(const TArray<FString>& Values, const TCHAR* Code,
			const TCHAR* Label, const FString& Path, FContentDiscoveryError& OutError)
		{
			TSet<FString> Seen;
			for (int32 Index = 0; Index < Values.Num(); ++Index)
			{
				const FString& Value = Values[Index];
				const FString ItemPath = FString::Printf(TEXT("%s[%d]"), *Path, Index);
				if (!IsExactText(Value))
				{
					OutError = Invalid(Code, FString::Printf(TEXT("The %s must be non-empty and have no surrounding whitespace."), Label), ItemPath);
					OutError.ActualValue = MakeShared<FJsonValueString>(Value);
					return false;
				}
				if (Seen.Contains(Value))
				{
					OutError = Invalid(Code, FString::Printf(TEXT("The %s duplicates the value at index %d."), Label, Values.Find(Value)), ItemPath);
					OutError.ActualValue = MakeShared<FJsonValueString>(Value);
					return false;
				}
				Seen.Add(Value);
			}
			return true;
		}

		FString Sha1(const FString& Value)
		{
			const FTCHARToUTF8 Utf8(*Value);
			uint8 Digest[FSHA1::DigestSize]{};
			FSHA1::HashBuffer(Utf8.Get(), Utf8.Length(), Digest);
			return TEXT("sha1:") + BytesToHex(Digest, FSHA1::DigestSize).ToLower();
		}

		FString JoinSorted(TArray<FString> Values)
		{
			Values.Sort();
			return FString::Join(Values, TEXT("\x1f"));
		}

		bool ContainsExact(
			const TArray<FString>& Values,
			const FString& Expected)
		{
			return Values.ContainsByPredicate(
				[&Expected](const FString& Value)
				{
					return Value.Equals(Expected, ESearchCase::CaseSensitive);
				});
		}

		bool Intersects(
			const TArray<FString>& Left,
			const TArray<FString>& Right)
		{
			for (const FString& Value : Left)
			{
				if (ContainsExact(Right, Value))
				{
					return true;
				}
			}
			return false;
		}

		void SplitNormalizedTokens(
			const FString& Text,
			TArray<FString>& OutTokens)
		{
			OutTokens.Reset();
			FString Current;
			for (int32 Index = 0; Index < Text.Len(); ++Index)
			{
				const TCHAR Character = Text[Index];
				if (!FChar::IsAlnum(Character))
				{
					if (!Current.IsEmpty())
					{
						OutTokens.Add(Current.ToLower());
						Current.Reset();
					}
					continue;
				}

				const bool bHasPrevious = Index > 0;
				const bool bHasNext = Index + 1 < Text.Len();
				const TCHAR Previous = bHasPrevious ? Text[Index - 1] : 0;
				const TCHAR Next = bHasNext ? Text[Index + 1] : 0;
				const bool bAlphaNumericBoundary = !Current.IsEmpty()
					&& ((FChar::IsAlpha(Character) && FChar::IsDigit(Previous))
						|| (FChar::IsDigit(Character) && FChar::IsAlpha(Previous)));
				const bool bCaseBoundary = !Current.IsEmpty()
					&& FChar::IsUpper(Character)
					&& (FChar::IsLower(Previous)
						|| (FChar::IsUpper(Previous)
							&& bHasNext
							&& FChar::IsLower(Next)));
				if (bAlphaNumericBoundary || bCaseBoundary)
				{
					OutTokens.Add(Current.ToLower());
					Current.Reset();
				}
				Current.AppendChar(Character);
			}
			if (!Current.IsEmpty())
			{
				OutTokens.Add(Current.ToLower());
			}
		}

		int32 ScoreKeywordAgainstTokens(
			const FString& Keyword,
			const TArray<FString>& Tokens)
		{
			int32 BestScore = INDEX_NONE;
			for (int32 TokenIndex = 0; TokenIndex < Tokens.Num(); ++TokenIndex)
			{
				const FString& Token = Tokens[TokenIndex];
				if (Token == Keyword)
				{
					BestScore = FMath::Max(BestScore, 1000);
					continue;
				}
				if (Token.StartsWith(Keyword, ESearchCase::CaseSensitive))
				{
					BestScore = FMath::Max(
						BestScore,
						800 - FMath::Min(300, Token.Len() - Keyword.Len()));
					continue;
				}

				FString TokenSuffix;
				for (int32 SuffixIndex = TokenIndex;
					 SuffixIndex < Tokens.Num();
					 ++SuffixIndex)
				{
					TokenSuffix += Tokens[SuffixIndex];
					if (TokenSuffix.Len() >= Keyword.Len())
					{
						break;
					}
				}
				if (TokenSuffix.StartsWith(Keyword, ESearchCase::CaseSensitive))
				{
					BestScore = FMath::Max(
						BestScore,
						600 - FMath::Min(300, TokenIndex));
				}
			}
			return BestScore;
		}

		bool IsProviderDefinitionValid(
			const TSharedRef<IUeAgentContentDiscoveryProvider>& Provider,
			FContentDiscoveryError& OutError)
		{
			if (!IsExactText(Provider->GetProviderId()))
			{
				ValidateText(Provider->GetProviderId(), TEXT("content_discovery_provider_id_invalid"),
					TEXT("The content provider identifier must be non-empty and have no surrounding whitespace."), TEXT("provider_id"), OutError, true);
				return false;
			}
			if (Provider->GetContentKinds().IsEmpty())
			{
				OutError = Invalid(TEXT("content_discovery_provider_kind_missing"), TEXT("The content provider must declare at least one supported content kind."), TEXT("content_kinds"));
				OutError.SubjectId = Provider->GetProviderId();
				return false;
			}
			const bool bValid = ValidateUniqueTexts(Provider->GetContentKinds(), TEXT("content_discovery_provider_kind_invalid"),
				TEXT("provider content kind"), TEXT("content_kinds"), OutError);
			if (!bValid) OutError.SubjectId = Provider->GetProviderId();
			return bValid;
		}
	}

	bool FContentDiscoveryRequest::IsValid() const
	{
		FContentDiscoveryError Error;
		return TryValidate(Error);
	}

	bool FContentDiscoveryRequest::TryValidate(Contract::FDiagnostic& OutError) const
	{
		OutError = {};
		if (ContentKinds.IsEmpty())
		{
			OutError = Invalid(TEXT("content_search_request_invalid"), TEXT("Content search requires at least one content kind."), TEXT("content_kinds"));
			return false;
		}
		if (MatchMode == EContentDiscoveryMatchMode::Exact && Query.TrimStartAndEnd().IsEmpty())
		{
			OutError = Invalid(TEXT("content_search_request_invalid"), TEXT("Exact content search requires a non-empty query."), TEXT("query"));
			OutError.ActualValue = MakeShared<FJsonValueString>(Query);
			return false;
		}
		return ValidateUniqueTexts(ContentKinds, TEXT("content_search_request_invalid"), TEXT("content kind"), TEXT("content_kinds"), OutError)
			&& ValidateUniqueTexts(RequiredOperations, TEXT("content_search_request_invalid"), TEXT("required operation"), TEXT("required_operations"), OutError);
	}

	FString FContentDiscoveryRequest::GetRequestFingerprint() const
	{
		const FString Material = FString::Printf(
			TEXT("%d\x1e%s\x1e%s\x1e%s\x1e%s\x1e%s\x1e%s"),
			static_cast<int32>(MatchMode),
			*Query,
			*JoinSorted(ContentKinds),
			*ClassPath,
			*PathPrefix,
			*OwnerIdentity,
			*JoinSorted(RequiredOperations));
		return Sha1(Material);
	}

	bool FContentDiscoveryItem::IsValid() const
	{
		FContentDiscoveryError Error;
		return TryValidate(Error);
	}

	bool FContentDiscoveryItem::TryValidate(Contract::FDiagnostic& OutError, const FString& Path) const
	{
		OutError = {};
		const TCHAR* Code = TEXT("content_search_item_invalid");
		if (!ValidateText(Identity, Code, TEXT("The content item requires a valid internal identity."), ChildPath(Path, TEXT("identity")), OutError, true)
			|| !ValidateText(DisplayName, Code, TEXT("The content item requires a non-empty display name without surrounding whitespace."), ChildPath(Path, TEXT("display_name")), OutError)
			|| !ValidateText(Domain, Code, TEXT("The content item requires a valid domain."), ChildPath(Path, TEXT("domain")), OutError)
			|| !ValidateText(ContentKind, Code, TEXT("The content item requires a valid content kind."), ChildPath(Path, TEXT("content_kind")), OutError)
			|| !ValidateText(ClassPath, Code, TEXT("The content item requires a valid class path."), ChildPath(Path, TEXT("class_path")), OutError)
			|| !ValidateText(StateLayer, Code, TEXT("The content item requires a valid state layer."), ChildPath(Path, TEXT("state_layer")), OutError)
			|| !ValidateText(Revision, Code, TEXT("The content item requires a valid revision."), ChildPath(Path, TEXT("revision")), OutError, true)
			|| !ValidateText(DetailCommandId, Code, TEXT("The content item requires a valid detail command identifier."), ChildPath(Path, TEXT("detail_command_id")), OutError)
			|| !ValidateText(DetailTargetIdentity, Code, TEXT("The content item requires a valid detail target identity."), ChildPath(Path, TEXT("detail_target_identity")), OutError, true)) return false;
		if (!DetailInput.IsValid())
		{
			OutError = Invalid(Code, TEXT("The content item has no detail input object."), ChildPath(Path, TEXT("detail_input")));
			return false;
		}
		if (!ValidateText(MatchReason, Code, TEXT("The content item requires a non-empty match reason without surrounding whitespace."), ChildPath(Path, TEXT("match_reason")), OutError)) return false;
		if (SupportedOperations.IsEmpty())
		{
			OutError = Invalid(Code, TEXT("The content item declares no supported operations."), ChildPath(Path, TEXT("supported_operations")));
			return false;
		}
		for (int32 Index = 0; Index < PublicDiscoveryScopes.Num(); ++Index)
		{
			const FString& Scope = PublicDiscoveryScopes[Index];
			if (!IsExactText(Scope) || !IsReadableDiscoveryName(Scope))
			{
				OutError = Invalid(Code, TEXT("The content discovery scope must be a readable public name without surrounding whitespace."),
					FString::Printf(TEXT("%s[%d]"), *ChildPath(Path, TEXT("public_discovery_scopes")), Index));
				OutError.ActualValue = MakeShared<FJsonValueString>(Scope);
				return false;
			}
		}
		return true;
	}

	bool FContentDiscoveryProviderPage::IsValid() const
	{
		FContentDiscoveryError Error;
		return TryValidate(Error);
	}

	bool FContentDiscoveryProviderPage::TryValidate(Contract::FDiagnostic& OutError) const
	{
		OutError = {};
		const TCHAR* Code = TEXT("content_search_provider_page_invalid");
		if (!ValidateText(ProviderId, Code, TEXT("The content page requires a valid provider identifier."), TEXT("provider_id"), OutError, true)
			|| !ValidateText(Revision, Code, TEXT("The content page requires a valid revision."), TEXT("revision"), OutError, true)) return false;
		if (!bReadOnly)
		{
			OutError = Invalid(Code, TEXT("A content search provider must return a read-only page."), TEXT("read_only"));
			OutError.ActualValue = MakeShared<FJsonValueBoolean>(bReadOnly);
			OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
			return false;
		}
		if (TotalMatches < 0 || Items.Num() != TotalMatches)
		{
			OutError = Invalid(Code, TotalMatches < 0 ? TEXT("The content page match count cannot be negative.")
				: TEXT("The content page match count differs from its item count."), TEXT("total_matches"));
			OutError.ActualValue = MakeShared<FJsonValueNumber>(TotalMatches);
			OutError.ExpectedValue = MakeShared<FJsonValueNumber>(Items.Num());
			return false;
		}
		for (int32 Index = 0; Index < Items.Num(); ++Index)
			if (!Items[Index].TryValidate(OutError, FString::Printf(TEXT("items[%d]"), Index))) return false;
		return true;
	}

	

	bool FContentDiscoveryProviderResult::Succeeded() const
	{
		return Page.IsSet() && !Error.IsValid();
	}

	bool FContentDiscoverySearchResult::HasFailures() const
	{
		return Providers.ContainsByPredicate(
			[](const FContentDiscoveryProviderResult& Provider)
			{
				return Provider.Error.IsValid();
			});
	}

	bool FContentDiscoverySearchResult::IsComplete() const
	{
		return !HasFailures();
	}

	FContentDiscoveryError FContentDiscoverySearchResult::GetFailureDiagnostic() const
	{
		FContentDiscoveryError Error;
		TArray<FString> FailedKinds;
		for (const auto& Provider : Providers)
		{
			if (!Provider.Error.IsValid()) continue;
			if (Error.IsEmpty()) Error = Invalid(TEXT("content_search_incomplete"), TEXT("Content search could not complete for all requested content kinds."), TEXT("content_kinds"));
			auto Cause = Provider.Error;
			if (Cause.SubjectId.IsEmpty()) Cause.SubjectId = Provider.ProviderId;
			Error.Diagnostics.Add(MoveTemp(Cause));
			for (const auto& Kind : Provider.ContentKinds) FailedKinds.AddUnique(Kind);
		}
		if (!Error.IsEmpty())
		{
			TArray<TSharedPtr<FJsonValue>> Values;
			for (const auto& Kind : FailedKinds) Values.Add(MakeShared<FJsonValueString>(Kind));
			Error.ActualValue = MakeShared<FJsonValueArray>(MoveTemp(Values));
		}
		return Error;
	}

	FUeAgentContentDiscoveryService::FUeAgentContentDiscoveryService(
		TMap<FString, TSharedRef<IUeAgentContentDiscoveryProvider>>
			InProviders,
		TArray<FString> InProviderIds)
		: Providers(MoveTemp(InProviders))
		, ProviderIds(MoveTemp(InProviderIds))
	{
	}

	bool FUeAgentContentDiscoveryService::TryCreate(
		TArray<TSharedRef<IUeAgentContentDiscoveryProvider>> InProviders,
		TSharedPtr<const FUeAgentContentDiscoveryService>& OutService,
		FContentDiscoveryError& OutError)
	{
		OutService.Reset();
		OutError = {};
		if (InProviders.IsEmpty())
		{
			OutError = Invalid(TEXT("content_discovery_provider_missing"), TEXT("Content discovery requires at least one registered provider."), TEXT("providers"));
			return false;
		}
		TMap<FString, TSharedRef<IUeAgentContentDiscoveryProvider>> ById;
		for (int32 Index = 0; Index < InProviders.Num(); ++Index)
		{
			TSharedRef<IUeAgentContentDiscoveryProvider>& Provider = InProviders[Index];
			const FString ProviderPath = FString::Printf(TEXT("providers[%d]"), Index);
			if (!IsProviderDefinitionValid(Provider, OutError))
			{
				OutError.PropertyPath = ProviderPath + TEXT(".") + OutError.PropertyPath;
				return false;
			}
			if (ById.Contains(Provider->GetProviderId()))
			{
				OutError = Invalid(TEXT("content_discovery_provider_duplicate"), TEXT("More than one content provider declares the same identifier."), ProviderPath + TEXT(".provider_id"));
				OutError.ActualEvidence = MakeShared<FJsonValueString>(Provider->GetProviderId());
				return false;
			}
			ById.Add(Provider->GetProviderId(), MoveTemp(Provider));
		}
		TArray<FString> Ids;
		ById.GetKeys(Ids);
		Ids.Sort();
		OutService = TSharedPtr<const FUeAgentContentDiscoveryService>(
			new FUeAgentContentDiscoveryService(MoveTemp(ById), MoveTemp(Ids)));
		return true;
	}

	bool FUeAgentContentDiscoveryService::TrySearch(
		const FContentDiscoveryRequest& Request,
		FContentDiscoverySearchResult& OutResult,
		FContentDiscoveryError& OutError) const
	{
		OutResult = {};
		OutError = {};
		if (!Request.TryValidate(OutError))
		{
			return false;
		}
		const FString Fingerprint = Request.GetRequestFingerprint();
		OutResult.RequestFingerprint = Fingerprint;
		TSet<FString> Selected;
		for (const FString& ProviderId : ProviderIds)
		{
			const TSharedRef<IUeAgentContentDiscoveryProvider>& Provider =
				Providers.FindChecked(ProviderId);
			if (!Intersects(Provider->GetContentKinds(), Request.ContentKinds))
			{
				continue;
			}
			Selected.Add(ProviderId);
			FContentDiscoveryProviderPage Page;
			FContentDiscoveryError ProviderError;
			FContentDiscoveryProviderResult Result;
			Result.ProviderId = ProviderId;
			Result.ContentKinds = Provider->GetContentKinds();
			if (!Provider->TrySearch(
					Request,
					Page,
					ProviderError))
			{
				if (!ProviderError.IsValid())
				{
					ProviderError = Invalid(TEXT("content_search_provider_failed"), TEXT("The content provider failed without returning a diagnostic."), TEXT("provider"));
					ProviderError.SubjectId = ProviderId;
				}
				Result.Error = MoveTemp(ProviderError);
				OutResult.Providers.Add(MoveTemp(Result));
				continue;
			}
			if (Page.ProviderId != ProviderId)
			{
				OutError = Invalid(TEXT("content_search_provider_page_invalid"), TEXT("The content page identifies a different provider from the one queried."), TEXT("provider_id"));
				OutError.ActualEvidence = MakeShared<FJsonValueString>(Page.ProviderId);
				OutError.ExpectedEvidence = MakeShared<FJsonValueString>(ProviderId);
			}
			else Page.TryValidate(OutError);
			if (!OutError.IsEmpty())
			{
				if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = ProviderId;
				const auto PreviousFailure = OutResult.GetFailureDiagnostic();
				OutError.Diagnostics.Append(PreviousFailure.Diagnostics);
				return false;
			}
			Result.Page = MoveTemp(Page);
			OutResult.Providers.Add(MoveTemp(Result));
		}
		if (Selected.IsEmpty())
		{
			OutError = Invalid(TEXT("content_search_kind_unsupported"), TEXT("No registered content provider supports the requested content kinds."), TEXT("content_kinds"));
			TArray<TSharedPtr<FJsonValue>> Actual;
			for (const auto& Kind : Request.ContentKinds) Actual.Add(MakeShared<FJsonValueString>(Kind));
			OutError.ActualValue = MakeShared<FJsonValueArray>(MoveTemp(Actual));
			TArray<FString> SupportedKinds;
			for (const auto& Provider : Providers)
				for (const auto& Kind : Provider.Value->GetContentKinds()) SupportedKinds.AddUnique(Kind);
			SupportedKinds.Sort();
			TArray<TSharedPtr<FJsonValue>> Expected;
			for (const auto& Kind : SupportedKinds) Expected.Add(MakeShared<FJsonValueString>(Kind));
			OutError.ExpectedValue = MakeShared<FJsonValueArray>(MoveTemp(Expected));
			return false;
		}
		return true;
	}

	const TArray<FString>& FUeAgentContentDiscoveryService::GetProviderIds() const
	{
		return ProviderIds;
	}

	bool TryScoreContentDiscoveryMatch(
		const FContentDiscoveryRequest& Request,
		const FString& Identity,
		const FString& DisplayName,
		const TArray<FString>& ExtraTerms,
		int32& OutScore,
		FString& OutReason)
	{
		OutScore = 0;
		OutReason.Reset();
		const FString Query = Request.Query.TrimStartAndEnd();
		if (Query.IsEmpty())
		{
			OutReason = TEXT("unfiltered");
			return true;
		}
		TArray<FString> Terms{Identity, DisplayName};
		Terms.Append(ExtraTerms);
		switch (Request.MatchMode)
		{
		case EContentDiscoveryMatchMode::Exact:
			for (const FString& Term : Terms)
			{
				if (Term.Equals(Query, ESearchCase::CaseSensitive)
					|| Term.Equals(Query, ESearchCase::IgnoreCase))
				{
					OutScore = 1000;
					OutReason = TEXT("exact");
					return true;
				}
			}
			return false;
		case EContentDiscoveryMatchMode::Prefix:
			for (const FString& Term : Terms)
			{
				if (Term.StartsWith(Query, ESearchCase::IgnoreCase))
				{
					OutScore = 800 - FMath::Min(300, Term.Len() - Query.Len());
					OutReason = TEXT("prefix");
					return true;
				}
			}
			return false;
		case EContentDiscoveryMatchMode::Text:
		case EContentDiscoveryMatchMode::Fuzzy:
		{
			TArray<FString> Keywords;
			SplitNormalizedTokens(Query, Keywords);
			if (Keywords.IsEmpty())
			{
				return false;
			}
			const FString CompactQuery = FString::Join(Keywords, TEXT(""));
			int32 PhraseScore = 0;
			for (const FString& Term : Terms)
			{
				TArray<FString> TermTokens;
				SplitNormalizedTokens(Term, TermTokens);
				const FString CompactTerm = FString::Join(TermTokens, TEXT(""));
				if (CompactTerm == CompactQuery)
				{
					PhraseScore = FMath::Max(PhraseScore, 4000);
				}
				else if (CompactTerm.StartsWith(CompactQuery))
				{
					PhraseScore = FMath::Max(PhraseScore, 3000);
				}
				else if (CompactTerm.Contains(CompactQuery))
				{
					PhraseScore = FMath::Max(PhraseScore, 2000);
				}
			}
			for (const FString& Keyword : Keywords)
			{
				int32 BestKeywordScore = INDEX_NONE;
				for (const FString& Term : Terms)
				{
					TArray<FString> TermTokens;
					SplitNormalizedTokens(Term, TermTokens);
					BestKeywordScore = FMath::Max(
						BestKeywordScore,
						ScoreKeywordAgainstTokens(Keyword, TermTokens));
				}
				if (BestKeywordScore == INDEX_NONE)
				{
					return false;
				}
				OutScore += BestKeywordScore;
			}
			OutScore += PhraseScore;
			OutReason = TEXT("text");
			return true;
		}
		default:
			return false;
		}
	}

	bool MatchesContentDiscoveryFilters(
		const FContentDiscoveryRequest& Request,
		const FContentDiscoveryItem& Item)
	{
		if (!ContainsExact(Request.ContentKinds, Item.ContentKind))
		{
			return false;
		}
		if (!Request.ClassPath.IsEmpty()
			&& Item.ClassPath != Request.ClassPath)
		{
			return false;
		}
		if (!Request.PathPrefix.IsEmpty()
			&& !Item.Identity.StartsWith(
				Request.PathPrefix,
				ESearchCase::CaseSensitive))
		{
			return false;
		}
		if (!Request.OwnerIdentity.IsEmpty()
			&& Item.OwnerIdentity != Request.OwnerIdentity)
		{
			return false;
		}
		for (const FString& Operation : Request.RequiredOperations)
		{
			if (!ContainsExact(Item.SupportedOperations, Operation))
			{
				return false;
			}
		}
		return true;
	}

	bool TryBuildContentDiscoveryProviderPage(
		const FString& ProviderId,
		const FString& Revision,
		const FContentDiscoveryRequest& Request,
		TArray<FContentDiscoveryItem> Items,
		FContentDiscoveryProviderPage& OutPage,
		FContentDiscoveryError& OutError)
	{
		OutPage = {};
		OutError = {};
		TArray<FContentDiscoveryItem> Matches;
		TMap<FString, int32> MatchIndexByIdentity;
		for (FContentDiscoveryItem& Item : Items)
		{
			if (!IsReadableDiscoveryName(Item.DisplayName)
				|| !MatchesContentDiscoveryFilters(Request, Item)
				|| !TryScoreContentDiscoveryMatch(
					Request,
					Item.Identity,
					Item.DisplayName,
					Item.SearchTerms,
					Item.MatchScore,
					Item.MatchReason))
			{
				continue;
			}
			const FString IdentityKey = Item.Identity.ToLower();
			if (const int32* ExistingIndex = MatchIndexByIdentity.Find(IdentityKey))
			{
				if (Item.MatchScore > Matches[*ExistingIndex].MatchScore)
				{
					Matches[*ExistingIndex] = MoveTemp(Item);
				}
				continue;
			}
			MatchIndexByIdentity.Add(IdentityKey, Matches.Num());
			Matches.Add(MoveTemp(Item));
		}
		Matches.Sort(
			[](const FContentDiscoveryItem& Left,
			   const FContentDiscoveryItem& Right)
			{
				if (Left.MatchScore != Right.MatchScore)
				{
					return Left.MatchScore > Right.MatchScore;
				}
				const int32 IgnoreCaseOrder = Left.Identity.Compare(
					Right.Identity,
					ESearchCase::IgnoreCase);
				return IgnoreCaseOrder != 0
					? IgnoreCaseOrder < 0
					: Left.Identity < Right.Identity;
			});
		OutPage.ProviderId = ProviderId;
		OutPage.Revision = Revision;
		OutPage.TotalMatches = Matches.Num();
		OutPage.bReadOnly = true;
		OutPage.Items = MoveTemp(Matches);
		return true;
	}
}

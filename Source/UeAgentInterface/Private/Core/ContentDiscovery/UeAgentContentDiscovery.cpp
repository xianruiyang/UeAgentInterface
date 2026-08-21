#include "Core/ContentDiscovery/UeAgentContentDiscovery.h"

#include "Algo/Sort.h"
#include "Misc/SecureHash.h"

namespace UE::UAI::ContentDiscovery
{
	namespace
	{
		bool IsExactText(const FString& Value)
		{
			return !Value.IsEmpty() && Value == Value.TrimStartAndEnd();
		}

		void SetError(
			FContentDiscoveryError& OutError,
			const FString& Code,
			const FString& SubjectId = FString(),
			const FString& Detail = FString())
		{
			OutError = {};
			OutError.Code = Code;
			OutError.SubjectId = SubjectId;
			OutError.Detail = Detail;
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

		int32 LevenshteinDistance(const FString& Left, const FString& Right)
		{
			const FString A = Left.ToLower();
			const FString B = Right.ToLower();
			TArray<int32> Previous;
			TArray<int32> Current;
			Previous.SetNumUninitialized(B.Len() + 1);
			Current.SetNumUninitialized(B.Len() + 1);
			for (int32 Index = 0; Index <= B.Len(); ++Index)
			{
				Previous[Index] = Index;
			}
			for (int32 LeftIndex = 1; LeftIndex <= A.Len(); ++LeftIndex)
			{
				Current[0] = LeftIndex;
				for (int32 RightIndex = 1; RightIndex <= B.Len(); ++RightIndex)
				{
					const int32 Cost = A[LeftIndex - 1] == B[RightIndex - 1] ? 0 : 1;
					Current[RightIndex] = FMath::Min3(
						Current[RightIndex - 1] + 1,
						Previous[RightIndex] + 1,
						Previous[RightIndex - 1] + Cost);
				}
				Swap(Previous, Current);
			}
			return Previous[B.Len()];
		}

		bool IsProviderDefinitionValid(
			const TSharedRef<IUeAgentContentDiscoveryProvider>& Provider,
			FContentDiscoveryError& OutError)
		{
			if (!IsExactText(Provider->GetProviderId()))
			{
				SetError(
					OutError,
					TEXT("content_discovery_provider_id_invalid"),
					Provider->GetProviderId());
				return false;
			}
			if (Provider->GetContentKinds().IsEmpty())
			{
				SetError(
					OutError,
					TEXT("content_discovery_provider_kind_missing"),
					Provider->GetProviderId());
				return false;
			}
			TSet<FString> Seen;
			for (const FString& Kind : Provider->GetContentKinds())
			{
				if (!IsExactText(Kind) || Seen.Contains(Kind))
				{
					SetError(
						OutError,
						TEXT("content_discovery_provider_kind_invalid"),
						Provider->GetProviderId(),
						Kind);
					return false;
				}
				Seen.Add(Kind);
			}
			return true;
		}
	}

	bool FContentDiscoveryCursorEntry::IsValid() const
	{
		return IsExactText(ProviderId)
			&& IsExactText(ExpectedRevision)
			&& Offset >= -1;
	}

	bool FContentDiscoveryCursor::IsEmpty() const
	{
		return RequestFingerprint.IsEmpty() && Providers.IsEmpty();
	}

	bool FContentDiscoveryCursor::IsValid() const
	{
		if (IsEmpty())
		{
			return true;
		}
		if (!IsExactText(RequestFingerprint) || Providers.IsEmpty())
		{
			return false;
		}
		TSet<FString> Seen;
		for (const FContentDiscoveryCursorEntry& Entry : Providers)
		{
			if (!Entry.IsValid() || Seen.Contains(Entry.ProviderId))
			{
				return false;
			}
			Seen.Add(Entry.ProviderId);
		}
		return true;
	}

	bool FContentDiscoveryRequest::IsValid() const
	{
		if (LimitPerProvider < 1 || LimitPerProvider > 64
			|| ContentKinds.IsEmpty()
			|| !Cursor.IsValid()
			|| (MatchMode == EContentDiscoveryMatchMode::Exact
				&& Query.TrimStartAndEnd().IsEmpty()))
		{
			return false;
		}
		TSet<FString> Kinds;
		for (const FString& Kind : ContentKinds)
		{
			if (!IsExactText(Kind) || Kinds.Contains(Kind))
			{
				return false;
			}
			Kinds.Add(Kind);
		}
		TSet<FString> Operations;
		for (const FString& Operation : RequiredOperations)
		{
			if (!IsExactText(Operation) || Operations.Contains(Operation))
			{
				return false;
			}
			Operations.Add(Operation);
		}
		return true;
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
		return IsExactText(Identity)
			&& IsExactText(DisplayName)
			&& IsExactText(Domain)
			&& IsExactText(ContentKind)
			&& IsExactText(ClassPath)
			&& IsExactText(StateLayer)
			&& IsExactText(Revision)
			&& IsExactText(DetailCommandId)
			&& IsExactText(MatchReason)
			&& !SupportedOperations.IsEmpty();
	}

	bool FContentDiscoveryProviderPage::IsValid(const int32 Limit) const
	{
		if (!IsExactText(ProviderId)
			|| !IsExactText(Revision)
			|| !bReadOnly
			|| TotalMatches < 0
			|| Items.Num() > Limit
			|| NextOffset < -1
			|| bComplete != (NextOffset == -1))
		{
			return false;
		}
		for (const FContentDiscoveryItem& Item : Items)
		{
			if (!Item.IsValid())
			{
				return false;
			}
		}
		return true;
	}

	bool FContentDiscoveryError::IsPresent() const
	{
		return !Code.IsEmpty();
	}

	bool FContentDiscoveryProviderResult::Succeeded() const
	{
		return Page.IsSet() && !Error.IsPresent();
	}

	bool FContentDiscoverySearchResult::HasFailures() const
	{
		return Providers.ContainsByPredicate(
			[](const FContentDiscoveryProviderResult& Provider)
			{
				return Provider.Error.IsPresent();
			});
	}

	bool FContentDiscoverySearchResult::HasMore() const
	{
		return Providers.ContainsByPredicate(
			[](const FContentDiscoveryProviderResult& Provider)
			{
				return Provider.Page.IsSet()
					&& !Provider.Page->bComplete;
			});
	}

	bool FContentDiscoverySearchResult::IsComplete() const
	{
		return !HasFailures() && !HasMore();
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
			SetError(OutError, TEXT("content_discovery_provider_missing"));
			return false;
		}
		TMap<FString, TSharedRef<IUeAgentContentDiscoveryProvider>> ById;
		for (TSharedRef<IUeAgentContentDiscoveryProvider>& Provider : InProviders)
		{
			if (!IsProviderDefinitionValid(Provider, OutError))
			{
				return false;
			}
			if (ById.Contains(Provider->GetProviderId()))
			{
				SetError(
					OutError,
					TEXT("content_discovery_provider_duplicate"),
					Provider->GetProviderId());
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
		if (!Request.IsValid())
		{
			SetError(OutError, TEXT("content_search_request_invalid"));
			return false;
		}
		const FString Fingerprint = Request.GetRequestFingerprint();
		if (!Request.Cursor.IsEmpty()
			&& Request.Cursor.RequestFingerprint != Fingerprint)
		{
			SetError(
				OutError,
				TEXT("content_search_cursor_request_mismatch"),
				Request.Cursor.RequestFingerprint);
			return false;
		}
		TMap<FString, FContentDiscoveryCursorEntry> CursorByProvider;
		for (const FContentDiscoveryCursorEntry& Entry : Request.Cursor.Providers)
		{
			CursorByProvider.Add(Entry.ProviderId, Entry);
		}

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
			const FContentDiscoveryCursorEntry* Cursor =
				CursorByProvider.Find(ProviderId);
			const int32 Offset = Cursor ? FMath::Max(Cursor->Offset, 0) : 0;
			FContentDiscoveryProviderPage Page;
			FContentDiscoveryError ProviderError;
			FContentDiscoveryProviderResult Result;
			Result.ProviderId = ProviderId;
			Result.ContentKinds = Provider->GetContentKinds();
			if (!Provider->TrySearch(
					Request,
					Offset,
					Request.LimitPerProvider,
					Page,
					ProviderError))
			{
				if (!ProviderError.IsPresent())
				{
					SetError(
						ProviderError,
						TEXT("content_search_provider_failed"),
						ProviderId);
				}
				Result.Error = MoveTemp(ProviderError);
				OutResult.Providers.Add(MoveTemp(Result));
				continue;
			}
			if (Page.ProviderId != ProviderId
				|| !Page.IsValid(Request.LimitPerProvider))
			{
				SetError(
					OutError,
					TEXT("content_search_provider_page_invalid"),
					ProviderId);
				return false;
			}
			if (Cursor
				&& Cursor->ExpectedRevision != Page.Revision)
			{
				SetError(
					OutError,
					TEXT("content_search_revision_stale"),
					ProviderId,
					Page.Revision);
				return false;
			}
			if (Cursor && Cursor->Offset == -1)
			{
				Page.Items.Reset();
				Page.NextOffset = -1;
				Page.bComplete = true;
			}
			Result.Page = MoveTemp(Page);
			OutResult.Providers.Add(MoveTemp(Result));
		}
		if (Selected.IsEmpty())
		{
			SetError(
				OutError,
				TEXT("content_search_kind_unsupported"),
				FString::Join(Request.ContentKinds, TEXT(",")));
			return false;
		}
		for (const TPair<FString, FContentDiscoveryCursorEntry>& Pair :
			CursorByProvider)
		{
			if (!Selected.Contains(Pair.Key))
			{
				SetError(
					OutError,
					TEXT("content_search_cursor_provider_invalid"),
					Pair.Key);
				return false;
			}
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
			for (const FString& Term : Terms)
			{
				const int32 Position = Term.Find(Query, ESearchCase::IgnoreCase);
				if (Position != INDEX_NONE)
				{
					OutScore = 600 - FMath::Min(300, Position);
					OutReason = TEXT("text");
					return true;
				}
			}
			return false;
		case EContentDiscoveryMatchMode::Fuzzy:
			for (const FString& Term : Terms)
			{
				const int32 Position = Term.Find(Query, ESearchCase::IgnoreCase);
				if (Position != INDEX_NONE)
				{
					OutScore = 600 - FMath::Min(300, Position);
					OutReason = TEXT("text");
					return true;
				}
				if (Query.Len() <= 64 && Term.Len() <= 128)
				{
					const int32 Distance = LevenshteinDistance(Query, Term);
					const int32 Threshold = FMath::Max(2, Query.Len() / 3);
					if (Distance <= Threshold)
					{
						OutScore = 400 - Distance;
						OutReason = TEXT("fuzzy");
						return true;
					}
				}
			}
			return false;
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
}

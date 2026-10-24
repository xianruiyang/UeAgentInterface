#include "CommandHandlers/UnifiedContentSearch/UeAgentUnifiedContentSearchHandlers.h"

#include "Core/Contract/UeAgentDiagnostic.h"

#include "Core/ContentDiscovery/UeAgentContentDiscovery.h"
#include "Core/ContentDiscovery/UeAgentNameDiscoveryProjection.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Core/DomainCapability/UeAgentDomainCapability.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace UE::UAI::CommandHandlers::UnifiedContentSearch
{
	using namespace CommandEdge;
	using namespace CommandContract;
	using namespace ContentDiscovery;
	using namespace Contract;

	namespace
	{
		const FString CommandId = TEXT("query.content.search");
		const FString Owner = TEXT("unified-content-search-owner");

		FUeAgentCommandResult Failure(const FUeAgentCommandRequest& Request, UE::UAI::Contract::FDiagnostic Diagnostic,
            const FString& Subject = FString())
        {
            if (Diagnostic.SubjectId.IsEmpty()) Diagnostic.SubjectId = Subject;
            return FUeAgentCommandResult::Failure(Request, TEXT("{}"), MoveTemp(Diagnostic));
        }

		bool HasQueryAuthority(const FUeAgentCallerContext& Caller)
		{
			return Caller.IsValid()
				&& Caller.GetAuthorityScopes().Contains(TEXT("uai:query"));
		}

		FDiagnostic InvalidParameter(const FString& Path, const FString& Reason,
			const TSharedPtr<FJsonValue>& Actual, const FString& Expected)
		{
			auto Error = FDiagnostic::Failure(TEXT("content_search_params_invalid"), Reason, TEXT("query"));
			Error.PropertyPath = Path;
			Error.ActualValue = Actual;
			Error.ExpectedValue = MakeShared<FJsonValueString>(Expected);
			return Error;
		}

		bool ReadStringArray(
			const TSharedPtr<FJsonObject>& Object,
			const FString& Field,
			const TArray<FString>& Defaults,
			TArray<FString>& OutValues,
			FDiagnostic& OutError)
		{
			OutValues = Defaults;
			if (!Object.IsValid() || !Object->HasField(Field))
			{
				return true;
			}
			const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
			if (!Object->TryGetArrayField(Field, Values) || Values == nullptr)
			{
				OutError = InvalidParameter(Field, TEXT("The content kind list must be an array of strings."), Object->TryGetField(Field), TEXT("array of strings"));
				return false;
			}
			OutValues.Reset();
			for (int32 Index = 0; Index < Values->Num(); ++Index)
			{
				const TSharedPtr<FJsonValue>& Value = (*Values)[Index];
				FString Text;
				if (!Value.IsValid() || !Value->TryGetString(Text))
				{
					OutError = InvalidParameter(FString::Printf(TEXT("%s[%d]"), *Field, Index),
						TEXT("Each content kind must be a string."), Value, TEXT("string"));
					return false;
				}
				OutValues.Add(MoveTemp(Text));
			}
			return true;
		}

		bool ParseMatchMode(
			const FString& Text,
			EContentDiscoveryMatchMode& OutMode,
			FDiagnostic& OutError)
		{
			if (Text == TEXT("exact"))
			{
				OutMode = EContentDiscoveryMatchMode::Exact;
				return true;
			}
			if (Text == TEXT("prefix"))
			{
				OutMode = EContentDiscoveryMatchMode::Prefix;
				return true;
			}
			if (Text == TEXT("text"))
			{
				OutMode = EContentDiscoveryMatchMode::Text;
				return true;
			}
			if (Text == TEXT("fuzzy"))
			{
				OutMode = EContentDiscoveryMatchMode::Fuzzy;
				return true;
			}
			OutError = InvalidParameter(TEXT("match_mode"), TEXT("The content search match mode is not supported."),
				MakeShared<FJsonValueString>(Text), TEXT("exact, prefix, text or fuzzy"));
			return false;
		}

		class FUnifiedContentSearchHandler final
			: public IUeAgentExactCommandHandler
		{
		public:
			const FString& GetCommandId() const override
			{
				return CommandId;
			}

			const FString& GetPublicOwner() const override
			{
				return Owner;
			}

			FUeAgentCommandResult Submit(
				const FUeAgentCommandRequest& Request,
				const FUeAgentCallerContext& Caller,
				const TSharedRef<IUeAgentCancellationSignal>& Cancellation) override
			{
				if (!HasQueryAuthority(Caller))
				{
					return Failure(Request, UE::UAI::Contract::FDiagnostic::Failure(TEXT("query_authority_required"), TEXT("uai:query is required"), TEXT("query")), CommandId);
				}
				if (Cancellation->IsCancelled())
				{
					return Failure(Request, UE::UAI::Contract::FDiagnostic::Failure(TEXT("query_cancelled"), TEXT("request was cancelled"), TEXT("query")), Request.GetRequestId());
				}

				FContentDiscoveryRequest Search;
				const TSharedPtr<FJsonObject>& Params = Request.GetParams();
				FString MatchMode = TEXT("text");
				FDiagnostic ParamsError;
				if (!Params.IsValid())
					return Failure(Request, InvalidParameter(TEXT("params"), TEXT("Content search parameters must be an object."), nullptr, TEXT("object")), CommandId);
				if (Params->HasField(TEXT("query")) && !Params->TryGetStringField(TEXT("query"), Search.Query))
					return Failure(Request, InvalidParameter(TEXT("query"), TEXT("The content search query must be a string."), Params->TryGetField(TEXT("query")), TEXT("string")), CommandId);
				if (Params->HasField(TEXT("match_mode")) && !Params->TryGetStringField(TEXT("match_mode"), MatchMode))
					return Failure(Request, InvalidParameter(TEXT("match_mode"), TEXT("The content search match mode must be a string."), Params->TryGetField(TEXT("match_mode")), TEXT("string")), CommandId);
				if (!ParseMatchMode(MatchMode, Search.MatchMode, ParamsError)
					|| !ReadStringArray(
						Params,
						TEXT("content_kinds"),
						{TEXT("asset"), TEXT("actor"), TEXT("component")},
						Search.ContentKinds, ParamsError))
				{
					return Failure(Request, MoveTemp(ParamsError), CommandId);
				}
				Search.RequiredOperations = {TEXT("query")};
				if (!Search.TryValidate(ParamsError))
				{
					return Failure(Request, MoveTemp(ParamsError), CommandId);
				}

				DomainCapability::FDomainCapabilityAuthorities Authorities;
				UE::UAI::Contract::FDiagnostic AuthorityError;
				if (!DomainCapability::
						TryGetOrBuildProcessDomainCapabilityAuthorities(
							Authorities,
							AuthorityError)
					|| !Authorities.GetContentDiscoveryService().IsValid())
				{
					if (AuthorityError.IsEmpty())
						AuthorityError = FDiagnostic::Failure(TEXT("content_search_authority_unavailable"), TEXT("The content search service is unavailable."), TEXT("content_search"));
					return Failure(Request, AuthorityError, CommandId);
				}
				FContentDiscoverySearchResult SearchResult;
				FContentDiscoveryError SearchError;
				// Scope names against the same current universe used by readable-name
				// resolution, not only the matches of this particular search.
				FContentDiscoveryRequest Universe;
				Universe.ContentKinds = {TEXT("asset"), TEXT("actor"), TEXT("component")};
				Universe.RequiredOperations = Search.RequiredOperations;
				if (!Authorities.GetContentDiscoveryService()->TrySearch(
						Universe,
						SearchResult,
						SearchError))
				{
					return Failure(Request, SearchError);
				}

				if (SearchResult.HasFailures())
				{
					return Failure(Request, SearchResult.GetFailureDiagnostic(), CommandId);
				}
				TArray<FContentDiscoveryItem> Items;
				int32 TotalMatches = 0;
				for (const FContentDiscoveryProviderResult& Result :
					SearchResult.Providers)
				{
					if (Result.Page.IsSet())
					{
						TotalMatches += Result.Page->TotalMatches;
						for (const FContentDiscoveryItem& Item : Result.Page->Items)
						{
							Items.Add(Item);
						}
					}
				}
				TArray<FScopedNameDiscoveryEntry> ScopedEntries;
				TArray<int32> MatchedIndices;
				ScopedEntries.Reserve(Items.Num());
				for (const FContentDiscoveryItem& Item : Items)
				{
					int32 Score = 0;
					FString Reason;
					if (MatchesContentDiscoveryFilters(Search, Item)
						&& TryScoreContentDiscoveryMatch(Search, Item.Identity, Item.DisplayName,
							Item.SearchTerms, Score, Reason))
						MatchedIndices.Add(ScopedEntries.Num());
					FScopedNameDiscoveryEntry Entry;
					Entry.Name = Item.DisplayName;
					Entry.Identity = Item.Identity;
					Entry.Scopes = Item.PublicDiscoveryScopes;
					ScopedEntries.Add(MoveTemp(Entry));
				}
				FString PayloadJson;
				UE::UAI::Contract::FDiagnostic CanonicalError;
				FNameDiscoveryCompleteness Completeness;
				Completeness.TotalMatches = TotalMatches;
				Completeness.ReturnedCount = Items.Num();
				if (!TryValidateNameDiscoveryCompleteness(
						Completeness, CanonicalError))
				{
					return Failure(
						Request, CanonicalError);
				}
				if (!TryBuildScopedNamesOnlyPayloadForIndices(
						ScopedEntries,
						MatchedIndices,
						ENameComparison::IgnoreCase,
						PayloadJson,
						CanonicalError))
				{
					return Failure(
						Request, CanonicalError);
				}
				return FUeAgentCommandResult::Success(
					Request,
					MoveTemp(PayloadJson));
			}
		};
	}

	bool TryCreateUnifiedContentSearchHandlers(
		TArray<TSharedRef<IUeAgentExactCommandHandler>>& OutHandlers,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutHandlers = {MakeShared<FUnifiedContentSearchHandler>()};
		OutError.Reset();
		return true;
	}
}

#include "CommandHandlers/UnifiedContentSearch/UeAgentUnifiedContentSearchHandlers.h"

#include "Core/ContentDiscovery/UeAgentContentDiscovery.h"
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

		FUeAgentCommandResult Failure(
			const FUeAgentCommandRequest& Request,
			const FString& Code,
			const FString& SubjectId,
			const FString& Detail)
		{
			const TSharedPtr<FJsonObject> Error = MakeShared<FJsonObject>();
			Error->SetStringField(TEXT("code"), Code);
			Error->SetStringField(TEXT("subject_id"), SubjectId);
			Error->SetStringField(TEXT("detail"), Detail);
			FString ErrorJson;
			FString CanonicalError;
			if (!Json::TryCanonicalize(Error, ErrorJson, CanonicalError))
			{
				ErrorJson =
					TEXT("{\"code\":\"content_search_error_serialization_failed\"}");
			}
			return FUeAgentCommandResult::Failure(Request, TEXT("{}"), ErrorJson);
		}

		bool HasQueryAuthority(const FUeAgentCallerContext& Caller)
		{
			return Caller.IsValid()
				&& Caller.GetAuthorityScopes().Contains(TEXT("uai:query"));
		}

		bool ReadInteger(
			const TSharedPtr<FJsonObject>& Object,
			const FString& Field,
			const int32 DefaultValue,
			int32& OutValue)
		{
			if (!Object.IsValid() || !Object->HasField(Field))
			{
				OutValue = DefaultValue;
				return true;
			}
			double Number = 0.0;
			if (!Object->TryGetNumberField(Field, Number)
				|| !FMath::IsFinite(Number)
				|| Number != FMath::FloorToDouble(Number)
				|| Number < MIN_int32
				|| Number > MAX_int32)
			{
				return false;
			}
			OutValue = static_cast<int32>(Number);
			return true;
		}

		bool ReadStringArray(
			const TSharedPtr<FJsonObject>& Object,
			const FString& Field,
			const TArray<FString>& Defaults,
			TArray<FString>& OutValues)
		{
			OutValues = Defaults;
			if (!Object.IsValid() || !Object->HasField(Field))
			{
				return true;
			}
			const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
			if (!Object->TryGetArrayField(Field, Values) || Values == nullptr)
			{
				return false;
			}
			OutValues.Reset();
			for (const TSharedPtr<FJsonValue>& Value : *Values)
			{
				FString Text;
				if (!Value.IsValid() || !Value->TryGetString(Text))
				{
					return false;
				}
				OutValues.Add(MoveTemp(Text));
			}
			return true;
		}

		bool ParseMatchMode(
			const FString& Text,
			EContentDiscoveryMatchMode& OutMode)
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
			return false;
		}

		bool ParseCursor(
			const TSharedPtr<FJsonObject>& Params,
			FContentDiscoveryCursor& OutCursor)
		{
			OutCursor = {};
			if (!Params.IsValid() || !Params->HasField(TEXT("cursor")))
			{
				return true;
			}
			const TSharedPtr<FJsonObject>* Cursor = nullptr;
			if (!Params->TryGetObjectField(TEXT("cursor"), Cursor)
				|| Cursor == nullptr || !Cursor->IsValid()
				|| !(*Cursor)->TryGetStringField(
					TEXT("request_fingerprint"),
					OutCursor.RequestFingerprint))
			{
				return false;
			}
			const TArray<TSharedPtr<FJsonValue>>* Providers = nullptr;
			if (!(*Cursor)->TryGetArrayField(TEXT("providers"), Providers)
				|| Providers == nullptr)
			{
				return false;
			}
			for (const TSharedPtr<FJsonValue>& Value : *Providers)
			{
				const TSharedPtr<FJsonObject> Provider =
					Value.IsValid() ? Value->AsObject() : nullptr;
				FContentDiscoveryCursorEntry Entry;
				if (!Provider.IsValid()
					|| !Provider->TryGetStringField(
						TEXT("provider_id"), Entry.ProviderId)
					|| !Provider->TryGetStringField(
						TEXT("revision"), Entry.ExpectedRevision)
					|| !ReadInteger(Provider, TEXT("offset"), 0, Entry.Offset))
				{
					return false;
				}
				OutCursor.Providers.Add(MoveTemp(Entry));
			}
			return OutCursor.IsValid();
		}

		TArray<TSharedPtr<FJsonValue>> Strings(const TArray<FString>& Values)
		{
			TArray<TSharedPtr<FJsonValue>> Result;
			for (const FString& Value : Values)
			{
				Result.Add(MakeShared<FJsonValueString>(Value));
			}
			return Result;
		}

		TSharedPtr<FJsonObject> ItemJson(
			const FString& ProviderId,
			const FContentDiscoveryItem& Item)
		{
			const TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("result_type"), TEXT("existing_content"));
			Result->SetStringField(TEXT("provider_id"), ProviderId);
			Result->SetStringField(TEXT("identity"), Item.Identity);
			Result->SetStringField(TEXT("display_name"), Item.DisplayName);
			Result->SetStringField(TEXT("domain"), Item.Domain);
			Result->SetStringField(TEXT("content_kind"), Item.ContentKind);
			Result->SetStringField(TEXT("class_path"), Item.ClassPath);
			Result->SetStringField(TEXT("owner_identity"), Item.OwnerIdentity);
			Result->SetStringField(TEXT("state_layer"), Item.StateLayer);
			Result->SetStringField(TEXT("revision"), Item.Revision);
			Result->SetStringField(
				TEXT("detail_command_id"), Item.DetailCommandId);
			Result->SetStringField(TEXT("match_reason"), Item.MatchReason);
			Result->SetArrayField(
				TEXT("supported_operations"),
				Strings(Item.SupportedOperations));
			return Result;
		}

		TSharedPtr<FJsonObject> CursorJson(
			const FContentDiscoverySearchResult& Search)
		{
			const TSharedPtr<FJsonObject> Cursor = MakeShared<FJsonObject>();
			Cursor->SetStringField(
				TEXT("request_fingerprint"), Search.RequestFingerprint);
			TArray<TSharedPtr<FJsonValue>> Providers;
			for (const FContentDiscoveryProviderResult& Result : Search.Providers)
			{
				if (!Result.Page.IsSet())
				{
					continue;
				}
				const TSharedPtr<FJsonObject> Provider = MakeShared<FJsonObject>();
				Provider->SetStringField(TEXT("provider_id"), Result.ProviderId);
				Provider->SetStringField(
					TEXT("revision"), Result.Page->Revision);
				Provider->SetNumberField(
					TEXT("offset"), Result.Page->NextOffset);
				Providers.Add(MakeShared<FJsonValueObject>(Provider));
			}
			Cursor->SetArrayField(TEXT("providers"), MoveTemp(Providers));
			return Cursor;
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
					return Failure(
						Request,
						TEXT("query_authority_required"),
						CommandId,
						TEXT("uai:query is required"));
				}
				if (Cancellation->IsCancelled())
				{
					return Failure(
						Request,
						TEXT("query_cancelled"),
						Request.GetRequestId(),
						TEXT("request was cancelled"));
				}

				FContentDiscoveryRequest Search;
				const TSharedPtr<FJsonObject>& Params = Request.GetParams();
				FString MatchMode = TEXT("text");
				if (!Params.IsValid()
					|| (Params->HasField(TEXT("query"))
						&& !Params->TryGetStringField(TEXT("query"), Search.Query))
					|| (Params->HasField(TEXT("match_mode"))
						&& !Params->TryGetStringField(TEXT("match_mode"), MatchMode))
					|| !ParseMatchMode(MatchMode, Search.MatchMode)
					|| !ReadStringArray(
						Params,
						TEXT("content_kinds"),
						{TEXT("asset"), TEXT("actor"), TEXT("component")},
						Search.ContentKinds)
					|| !ReadStringArray(
						Params,
						TEXT("required_operations"),
						{TEXT("query")},
						Search.RequiredOperations)
					|| !ReadInteger(
						Params,
						TEXT("limit_per_provider"),
						20,
						Search.LimitPerProvider)
					|| !ParseCursor(Params, Search.Cursor))
				{
					return Failure(
						Request,
						TEXT("content_search_params_invalid"),
						CommandId,
						TEXT("query, filters, paging or cursor is invalid"));
				}
				Params->TryGetStringField(TEXT("class_path"), Search.ClassPath);
				Params->TryGetStringField(TEXT("path_prefix"), Search.PathPrefix);
				Params->TryGetStringField(
					TEXT("owner_identity"), Search.OwnerIdentity);
				if (!Search.IsValid())
				{
					return Failure(
						Request,
						TEXT("content_search_params_invalid"),
						CommandId,
						TEXT("the normalized content search request is invalid"));
				}

				DomainCapability::FDomainCapabilityAuthorities Authorities;
				FString AuthorityError;
				if (!DomainCapability::
						TryGetOrBuildProcessDomainCapabilityAuthorities(
							Authorities,
							AuthorityError)
					|| !Authorities.GetContentDiscoveryService().IsValid())
				{
					return Failure(
						Request,
						TEXT("content_search_authority_unavailable"),
						CommandId,
						AuthorityError);
				}
				FContentDiscoverySearchResult SearchResult;
				FContentDiscoveryError SearchError;
				if (!Authorities.GetContentDiscoveryService()->TrySearch(
						Search,
						SearchResult,
						SearchError))
				{
					return Failure(
						Request,
						SearchError.Code,
						SearchError.SubjectId,
						SearchError.Detail);
				}

				TArray<TSharedPtr<FJsonValue>> Items;
				TArray<TSharedPtr<FJsonValue>> ProviderStates;
				TArray<FString> RetryableProviders;
				bool bAnySuccess = false;
				for (const FContentDiscoveryProviderResult& Result :
					SearchResult.Providers)
				{
					const TSharedPtr<FJsonObject> State = MakeShared<FJsonObject>();
					State->SetStringField(TEXT("provider_id"), Result.ProviderId);
					State->SetArrayField(
						TEXT("content_kinds"), Strings(Result.ContentKinds));
					if (Result.Page.IsSet())
					{
						bAnySuccess = true;
						State->SetStringField(TEXT("status"), TEXT("succeeded"));
						State->SetStringField(
							TEXT("revision"), Result.Page->Revision);
						State->SetNumberField(
							TEXT("total_matches"), Result.Page->TotalMatches);
						State->SetNumberField(
							TEXT("returned_items"), Result.Page->Items.Num());
						State->SetBoolField(
							TEXT("complete"), Result.Page->bComplete);
						State->SetBoolField(
							TEXT("has_more"), !Result.Page->bComplete);
						State->SetNumberField(
							TEXT("next_offset"), Result.Page->NextOffset);
						for (const FContentDiscoveryItem& Item : Result.Page->Items)
						{
							Items.Add(MakeShared<FJsonValueObject>(
								ItemJson(Result.ProviderId, Item)));
						}
					}
					else
					{
						State->SetStringField(TEXT("status"), TEXT("failed"));
						const TSharedPtr<FJsonObject> Error = MakeShared<FJsonObject>();
						Error->SetStringField(TEXT("code"), Result.Error.Code);
						Error->SetStringField(
							TEXT("subject_id"), Result.Error.SubjectId);
						Error->SetStringField(TEXT("detail"), Result.Error.Detail);
						State->SetObjectField(TEXT("error"), Error);
						RetryableProviders.Add(Result.ProviderId);
					}
					ProviderStates.Add(MakeShared<FJsonValueObject>(State));
				}

				const FString Completeness = SearchResult.IsComplete()
					? TEXT("complete")
					: SearchResult.HasFailures()
						? (bAnySuccess ? TEXT("partial") : TEXT("degraded"))
						: TEXT("paged");
				const TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
				Payload->SetStringField(TEXT("command_id"), CommandId);
				Payload->SetBoolField(TEXT("read_only"), true);
				Payload->SetStringField(
					TEXT("request_fingerprint"), SearchResult.RequestFingerprint);
				Payload->SetStringField(TEXT("completeness"), Completeness);
				Payload->SetBoolField(TEXT("complete"), SearchResult.IsComplete());
				Payload->SetBoolField(TEXT("has_more"), SearchResult.HasMore());
				Payload->SetArrayField(TEXT("items"), MoveTemp(Items));
				Payload->SetArrayField(TEXT("providers"), MoveTemp(ProviderStates));
				Payload->SetArrayField(
					TEXT("retryable_providers"), Strings(RetryableProviders));
				if (SearchResult.HasMore())
				{
					Payload->SetObjectField(
						TEXT("next_cursor"), CursorJson(SearchResult));
				}
				else
				{
					Payload->SetField(
						TEXT("next_cursor"), MakeShared<FJsonValueNull>());
				}
				FString PayloadJson;
				FString CanonicalError;
				if (!Json::TryCanonicalize(
						Payload,
						PayloadJson,
						CanonicalError))
				{
					return Failure(
						Request,
						TEXT("content_search_result_serialization_failed"),
						CommandId,
						CanonicalError);
				}
				return FUeAgentCommandResult::Success(
					Request,
					MoveTemp(PayloadJson));
			}
		};
	}

	bool TryCreateUnifiedContentSearchHandlers(
		TArray<TSharedRef<IUeAgentExactCommandHandler>>& OutHandlers,
		FString& OutError)
	{
		OutHandlers = {MakeShared<FUnifiedContentSearchHandler>()};
		OutError.Reset();
		return true;
	}
}

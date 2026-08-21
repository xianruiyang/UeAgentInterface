#include "Composition/ReferenceCommand/UeAgentReferenceCommandHandlers.h"

#include "Core/Contract/UeAgentCommandJson.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace UE::UAI::Composition::ReferenceCommand
{
	using namespace CommandContract;
	using namespace CommandEdge;
	using namespace Contract;
	using namespace ReferenceCrud;

	namespace
	{
		const FString CatalogSearchCommandId(
			TEXT("reference.catalog_search"));
		const FString AssetReadCommandId(
			TEXT("reference.asset_read"));
		const FString AssetApplyCommandId(
			TEXT("reference.asset_apply"));

		FString ErrorJson(
			const FString& Code,
			const FString& Subject,
			const FString& Detail)
		{
			const TSharedPtr<FJsonObject> Error =
				MakeShared<FJsonObject>();
			Error->SetStringField(TEXT("code"), Code);
			Error->SetStringField(TEXT("subject_id"), Subject);
			Error->SetStringField(TEXT("detail"), Detail);
			FString Result;
			FString Ignored;
			Json::TryCanonicalize(Error, Result, Ignored);
			return Result;
		}

		bool TryRootAndError(
			const FString& JsonText,
			const FString& ErrorFieldName,
			TSharedPtr<FJsonObject>& OutRoot,
			FString& OutErrorJson)
		{
			OutRoot.Reset();
			OutErrorJson.Reset();
			FString Error;
			if (!Json::TryParseObject(JsonText, OutRoot, Error))
			{
				OutErrorJson = ErrorJson(
					TEXT("command_handler_result_json_invalid"),
					ErrorFieldName,
					Error);
				return false;
			}
			const TSharedPtr<FJsonObject>* ErrorObject = nullptr;
			if (OutRoot->TryGetObjectField(
				ErrorFieldName,
				ErrorObject)
				&& ErrorObject
				&& ErrorObject->IsValid())
			{
				Json::TryCanonicalize(
					*ErrorObject,
					OutErrorJson,
					Error);
				return false;
			}
			return true;
		}

		FString CanonicalObject(
			const TSharedPtr<FJsonObject>& Object)
		{
			FString Result;
			FString Error;
			return Json::TryCanonicalize(Object, Result, Error)
				? Result
				: FString(TEXT("{}"));
		}

		TSharedRef<FJsonObject> SessionJson(
			const FUeAgentReferenceCommandHandlerConfig& Config)
		{
			const TSharedRef<FJsonObject> Session =
				MakeShared<FJsonObject>();
			Session->SetStringField(
				TEXT("server_session_id"),
				Config.ServerSessionId);
			Session->SetField(
				TEXT("registry_epoch"),
				MakeShared<FJsonValueNumberString>(
					FString::Printf(
						TEXT("%lld"),
						Config.RegistryEpoch)));
			Session->SetStringField(
				TEXT("registry_hash"),
				Config.RegistryHash);
			return Session;
		}

		TSharedRef<FJsonObject> ProjectionJson()
		{
			const TSharedRef<FJsonObject> Projection =
				MakeShared<FJsonObject>();
			Projection->SetStringField(
				TEXT("profile_id"),
				TEXT("full"));
			Projection->SetBoolField(
				TEXT("include_defaults"),
				true);
			Projection->SetBoolField(
				TEXT("include_details"),
				true);
			Projection->SetBoolField(
				TEXT("include_provenance"),
				true);
			Projection->SetNumberField(
				TEXT("max_items"),
				25);
			Projection->SetNumberField(
				TEXT("token_budget"),
				4096);
			return Projection;
		}

		bool TryObject(
			const TSharedPtr<FJsonObject>& Parent,
			const FString& Name,
			TSharedPtr<FJsonObject>& Out)
		{
			Out.Reset();
			const TSharedPtr<FJsonObject>* Found = nullptr;
			if (!Parent.IsValid()
				|| !Parent->TryGetObjectField(Name, Found)
				|| !Found
				|| !Found->IsValid())
			{
				return false;
			}
			Out = *Found;
			return true;
		}

		bool TrySubmitQuery(
			const FUeAgentReferenceCommandHandlerConfig& Config,
			const FUeAgentCommandRequest& Request,
			const FUeAgentCallerContext& CallerContext,
			const TSharedRef<IUeAgentCancellationSignal>&
				Cancellation,
			const FString& Stage,
			const TSharedRef<FJsonObject>& Payload,
			TSharedPtr<FJsonObject>& OutRoot,
			FString& OutErrorJson)
		{
			const FUeAgentQueryResult Result =
				Config.QueryPort->Submit(
					FUeAgentQueryRequest(
						Config.QueryDescriptor,
						Request.GetRequestId()
							+ TEXT("-")
							+ Stage,
						FUeAgentContractValue(
							CanonicalObject(Payload))),
					CallerContext,
					Cancellation);
			const FString Raw =
				Result.GetValue().GetJsonValue();
			if (!Result.IsValid()
				|| !TryRootAndError(
					Raw,
					TEXT("query_error"),
					OutRoot,
					OutErrorJson))
			{
				if (OutErrorJson.IsEmpty())
				{
					OutErrorJson = ErrorJson(
						TEXT("command_readback_query_invalid"),
						Stage,
						TEXT(
							"C3Q returned an invalid readback result"));
				}
				return false;
			}
			return true;
		}

		class FCatalogSearchHandler final
			: public IUeAgentCommandHandler
		{
		public:
			explicit FCatalogSearchHandler(
				const FUeAgentReferenceCommandHandlerConfig&
					InConfig)
				: Config(InConfig)
			{
			}

			virtual const FString& GetCommandId() const override
			{
				return CatalogSearchCommandId;
			}

			virtual FUeAgentCommandResult Submit(
				const FUeAgentCommandRequest& Request,
				const FUeAgentCallerContext& CallerContext,
				const TSharedRef<IUeAgentCancellationSignal>&
					Cancellation) override
			{
				const TSharedPtr<FJsonObject>& Params =
					Request.GetParams();
				const TSharedPtr<FJsonObject> Session =
					MakeShared<FJsonObject>();
				Session->SetStringField(
					TEXT("server_session_id"),
					Config.ServerSessionId);
				Session->SetField(
					TEXT("registry_epoch"),
					MakeShared<FJsonValueNumberString>(
						FString::Printf(
							TEXT("%lld"),
							Config.RegistryEpoch)));
				Session->SetStringField(
					TEXT("registry_hash"),
					Config.RegistryHash);

				const TSharedPtr<FJsonObject> Query =
					MakeShared<FJsonObject>();
				Query->SetStringField(
					TEXT("catalog_kind"),
					Params->GetStringField(
						TEXT("catalog_kind")));
				Query->SetStringField(
					TEXT("search_text"),
					Params->GetStringField(
						TEXT("search_text")));
				Query->SetStringField(
					TEXT("match_mode"),
					Params->GetStringField(
						TEXT("match_mode")));
				Query->SetStringField(
					TEXT("materialization_policy"),
					Params->GetStringField(
						TEXT("materialization_policy")));

				TSharedPtr<FJsonObject> Projection;
				const TSharedPtr<FJsonObject>* ProjectionField =
					nullptr;
				if (Params->TryGetObjectField(
						TEXT("projection"),
						ProjectionField)
					&& ProjectionField
					&& ProjectionField->IsValid())
				{
					Projection = *ProjectionField;
				}
				else
				{
					Projection = MakeShared<FJsonObject>();
					Projection->SetStringField(
						TEXT("profile_id"),
						TEXT("full"));
					Projection->SetBoolField(
						TEXT("include_defaults"),
						true);
					Projection->SetBoolField(
						TEXT("include_details"),
						true);
					Projection->SetBoolField(
						TEXT("include_provenance"),
						true);
					Projection->SetNumberField(
						TEXT("max_items"),
						25);
					Projection->SetNumberField(
						TEXT("token_budget"),
						4096);
				}

				const TSharedPtr<FJsonObject> Payload =
					MakeShared<FJsonObject>();
				Payload->SetStringField(
					TEXT("query_kind"),
					TEXT("catalog_search"));
				Payload->SetObjectField(TEXT("session"), Session);
				Payload->SetObjectField(TEXT("query"), Query);
				Payload->SetObjectField(
					TEXT("projection"),
					Projection);
				const FUeAgentQueryResult Result =
					Config.QueryPort->Submit(
						FUeAgentQueryRequest(
							Config.QueryDescriptor,
							Request.GetRequestId(),
							FUeAgentContractValue(
								CanonicalObject(Payload))),
						CallerContext,
						Cancellation);
				const FString Raw =
					Result.GetValue().GetJsonValue();
				TSharedPtr<FJsonObject> Root;
				FString ResultError;
				if (!Result.IsValid()
					|| !TryRootAndError(
						Raw,
						TEXT("query_error"),
						Root,
						ResultError))
				{
					if (ResultError.IsEmpty())
					{
						ResultError = ErrorJson(
							TEXT(
								"command_query_result_invalid"),
							Request.GetRequestId(),
							TEXT(
								"C3Q returned an invalid result"));
					}
					return FUeAgentCommandResult::Failure(
						Request,
						Root.IsValid()
							? CanonicalObject(Root)
							: FString(TEXT("{}")),
						ResultError);
				}
				return FUeAgentCommandResult::Success(
					Request,
					CanonicalObject(Root));
			}

		private:
			FUeAgentReferenceCommandHandlerConfig Config;
		};

		class FAssetApplyHandler final
			: public IUeAgentCommandHandler
		{
		public:
			explicit FAssetApplyHandler(
				const FUeAgentReferenceCommandHandlerConfig&
					InConfig)
				: Config(InConfig)
			{
			}

			virtual const FString& GetCommandId() const override
			{
				return AssetApplyCommandId;
			}

			virtual FUeAgentCommandResult Submit(
				const FUeAgentCommandRequest& Request,
				const FUeAgentCallerContext& CallerContext,
				const TSharedRef<IUeAgentCancellationSignal>&
					Cancellation) override
			{
				const TSharedPtr<FJsonObject>& Params =
					Request.GetParams();
				FUeAgentReferenceCrudCommand Command;
				Command.RequestId = Request.GetRequestId();
				Command.IdempotencyKey =
					TEXT("uai-command:")
					+ Request.GetRequestId();
				Command.ObjectPath =
					Params->GetStringField(TEXT("object_path"));
				Command.Intent =
					Params->GetStringField(TEXT("intent"));
				const TSharedPtr<FJsonValue> Proposed =
					Params->TryGetField(TEXT("proposed_text"));
				if (Proposed.IsValid()
					&& Proposed->Type == EJson::String)
				{
					Command.ProposedText.Emplace(
						Proposed->AsString());
				}
				const FUeAgentOperationResult Result =
					Config.OperationApplication->SubmitCommand(
						Command,
						CallerContext,
						Cancellation);
				const FString Raw =
					Result.GetValue().GetJsonValue();
				TSharedPtr<FJsonObject> Root;
				FString ResultError;
				if (!Result.IsValid()
					|| !TryRootAndError(
						Raw,
						TEXT("operation_error"),
						Root,
						ResultError))
				{
					if (ResultError.IsEmpty())
					{
						ResultError = ErrorJson(
							TEXT(
								"command_operation_result_invalid"),
							Request.GetRequestId(),
							TEXT(
								"C3O returned an invalid result"));
					}
					return FUeAgentCommandResult::Failure(
						Request,
						Root.IsValid()
							? CanonicalObject(Root)
							: FString(TEXT("{}")),
						ResultError);
				}
				return FUeAgentCommandResult::Success(
					Request,
					CanonicalObject(Root));
			}

		private:
			FUeAgentReferenceCommandHandlerConfig Config;
		};

		class FAssetReadHandler final
			: public IUeAgentCommandHandler
		{
		public:
			explicit FAssetReadHandler(
				const FUeAgentReferenceCommandHandlerConfig&
					InConfig)
				: Config(InConfig)
			{
			}

			virtual const FString& GetCommandId() const override
			{
				return AssetReadCommandId;
			}

			virtual FUeAgentCommandResult Submit(
				const FUeAgentCommandRequest& Request,
				const FUeAgentCallerContext& CallerContext,
				const TSharedRef<IUeAgentCancellationSignal>&
					Cancellation) override
			{
				const FString ObjectPath =
					Request.GetParams()->GetStringField(
						TEXT("object_path"));
				const TSharedRef<FJsonObject> SearchQuery =
					MakeShared<FJsonObject>();
				SearchQuery->SetStringField(
					TEXT("catalog_kind"),
					TEXT("uai_reference_asset"));
				SearchQuery->SetStringField(
					TEXT("search_text"),
					ObjectPath);
				SearchQuery->SetStringField(
					TEXT("match_mode"),
					TEXT("exact"));
				SearchQuery->SetStringField(
					TEXT("materialization_policy"),
					TEXT("index_only"));
				const TSharedRef<FJsonObject> SearchPayload =
					MakeShared<FJsonObject>();
				SearchPayload->SetStringField(
					TEXT("query_kind"),
					TEXT("catalog_search"));
				SearchPayload->SetObjectField(
					TEXT("session"),
					SessionJson(Config));
				SearchPayload->SetObjectField(
					TEXT("query"),
					SearchQuery);
				SearchPayload->SetObjectField(
					TEXT("projection"),
					ProjectionJson());

				TSharedPtr<FJsonObject> SearchRoot;
				FString QueryError;
				if (!TrySubmitQuery(
						Config,
						Request,
						CallerContext,
						Cancellation,
						TEXT("search"),
						SearchPayload,
						SearchRoot,
						QueryError))
				{
					return FUeAgentCommandResult::Failure(
						Request,
						SearchRoot.IsValid()
							? CanonicalObject(SearchRoot)
							: FString(TEXT("{}")),
						QueryError);
				}

				TSharedPtr<FJsonObject> SearchProjection;
				TSharedPtr<FJsonObject> SearchProjectedPayload;
				TSharedPtr<FJsonObject> Catalog;
				const TArray<TSharedPtr<FJsonValue>>* Candidates =
					nullptr;
				if (!TryObject(
						SearchRoot,
						TEXT("projection"),
						SearchProjection)
					|| !TryObject(
						SearchProjection,
						TEXT("payload"),
						SearchProjectedPayload)
					|| !TryObject(
						SearchProjectedPayload,
						TEXT("catalog"),
						Catalog)
					|| !Catalog->TryGetArrayField(
						TEXT("candidates"),
						Candidates)
					|| !Candidates
					|| Candidates->Num() != 1)
				{
					return FUeAgentCommandResult::Failure(
						Request,
						CanonicalObject(SearchRoot),
						ErrorJson(
							TEXT(
								"command_readback_candidate_invalid"),
							ObjectPath,
							TEXT(
								"exact search did not return one candidate")));
				}

				const TSharedPtr<FJsonObject> Candidate =
					(*Candidates)[0]->AsObject();
				TSharedPtr<FJsonObject> Identity;
				if (!TryObject(
						Candidate,
						TEXT("identity"),
						Identity))
				{
					return FUeAgentCommandResult::Failure(
						Request,
						CanonicalObject(SearchRoot),
						ErrorJson(
							TEXT(
								"command_readback_identity_invalid"),
							ObjectPath,
							TEXT(
								"candidate identity is unavailable")));
				}
				const StateSemantic::FUeAgentCanonicalIdentity
					CanonicalIdentity(
						Identity->GetStringField(
							TEXT("semantic_id")),
						Identity->GetStringField(
							TEXT("target_kind")),
						Identity->GetStringField(
							TEXT("owner_id")),
						Identity->GetStringField(
							TEXT("selector")),
						Identity->GetStringField(
							TEXT("object_role")),
						Identity->GetStringField(
							TEXT("generation")));
				if (!CanonicalIdentity.IsValid())
				{
					return FUeAgentCommandResult::Failure(
						Request,
						CanonicalObject(SearchRoot),
						ErrorJson(
							TEXT(
								"command_readback_identity_invalid"),
							ObjectPath,
							TEXT(
								"candidate identity is not canonical")));
				}

				const TSharedRef<FJsonObject> ResolvePayload =
					MakeShared<FJsonObject>();
				ResolvePayload->SetStringField(
					TEXT("query_kind"),
					TEXT("catalog_resolve"));
				ResolvePayload->SetObjectField(
					TEXT("session"),
					SessionJson(Config));
				ResolvePayload->SetObjectField(
					TEXT("catalog"),
					Catalog.ToSharedRef());
				ResolvePayload->SetStringField(
					TEXT("candidate_id"),
					CanonicalIdentity.GetCanonicalKey());
				ResolvePayload->SetObjectField(
					TEXT("projection"),
					ProjectionJson());
				TSharedPtr<FJsonObject> ResolveRoot;
				if (!TrySubmitQuery(
						Config,
						Request,
						CallerContext,
						Cancellation,
						TEXT("resolve"),
						ResolvePayload,
						ResolveRoot,
						QueryError))
				{
					return FUeAgentCommandResult::Failure(
						Request,
						ResolveRoot.IsValid()
							? CanonicalObject(ResolveRoot)
							: FString(TEXT("{}")),
						QueryError);
				}

				TSharedPtr<FJsonObject> Ticket;
				TSharedPtr<FJsonObject> ResolveProjection;
				TSharedPtr<FJsonObject> ResolveProjectedPayload;
				TSharedPtr<FJsonObject> Binding;
				if (!TryObject(
						ResolveRoot,
						TEXT("ticket"),
						Ticket)
					|| !TryObject(
						ResolveRoot,
						TEXT("projection"),
						ResolveProjection)
					|| !TryObject(
						ResolveProjection,
						TEXT("payload"),
						ResolveProjectedPayload)
					|| !TryObject(
						ResolveProjectedPayload,
						TEXT("candidate_binding"),
						Binding))
				{
					return FUeAgentCommandResult::Failure(
						Request,
						CanonicalObject(ResolveRoot),
						ErrorJson(
							TEXT(
								"command_readback_binding_invalid"),
							ObjectPath,
							TEXT(
								"exact resolve omitted ticket or binding")));
				}

				const TSharedRef<FJsonObject> StatePayload =
					MakeShared<FJsonObject>();
				StatePayload->SetStringField(
					TEXT("query_kind"),
					TEXT("state_read"));
				StatePayload->SetObjectField(
					TEXT("session"),
					SessionJson(Config));
				StatePayload->SetObjectField(
					TEXT("ticket"),
					Ticket.ToSharedRef());
				StatePayload->SetObjectField(
					TEXT("binding"),
					Binding.ToSharedRef());
				StatePayload->SetStringField(
					TEXT("state_plane"),
					TEXT("persisted"));
				StatePayload->SetObjectField(
					TEXT("projection"),
					ProjectionJson());
				TSharedPtr<FJsonObject> StateRoot;
				if (!TrySubmitQuery(
						Config,
						Request,
						CallerContext,
						Cancellation,
						TEXT("state"),
						StatePayload,
						StateRoot,
						QueryError))
				{
					return FUeAgentCommandResult::Failure(
						Request,
						StateRoot.IsValid()
							? CanonicalObject(StateRoot)
							: FString(TEXT("{}")),
						QueryError);
				}
				return FUeAgentCommandResult::Success(
					Request,
					CanonicalObject(StateRoot));
			}

		private:
			FUeAgentReferenceCommandHandlerConfig Config;
		};
	}

	FUeAgentReferenceCommandHandlerConfig::
		FUeAgentReferenceCommandHandlerConfig(
			FString InServerSessionId,
			const int64 InRegistryEpoch,
			FString InRegistryHash,
			FUeAgentDescriptorIdentity InQueryDescriptor,
			TSharedRef<IUeAgentQueryApplicationPort> InQueryPort,
			TSharedRef<FUeAgentReferenceCrudApplication>
				InOperationApplication)
		: ServerSessionId(MoveTemp(InServerSessionId))
		, RegistryEpoch(InRegistryEpoch)
		, RegistryHash(MoveTemp(InRegistryHash))
		, QueryDescriptor(MoveTemp(InQueryDescriptor))
		, QueryPort(MoveTemp(InQueryPort))
		, OperationApplication(MoveTemp(InOperationApplication))
	{
	}

	bool FUeAgentReferenceCommandHandlerConfig::IsValid() const
	{
		return !ServerSessionId.IsEmpty()
			&& RegistryEpoch > 0
			&& RegistryHash.Len() == 71
			&& RegistryHash.Left(7) == TEXT("sha256:")
			&& QueryDescriptor.IsValid();
	}

	bool TryCreateReferenceCommandHandlers(
		const FUeAgentReferenceCommandHandlerConfig& Config,
		TArray<TSharedRef<IUeAgentCommandHandler>>& OutHandlers,
		FString& OutError)
	{
		OutHandlers.Reset();
		OutError.Reset();
		if (!Config.IsValid())
		{
			OutError =
				TEXT("reference_command_handler_config_invalid");
			return false;
		}
		OutHandlers.Add(MakeShared<FCatalogSearchHandler>(Config));
		OutHandlers.Add(MakeShared<FAssetApplyHandler>(Config));
		return true;
	}
}

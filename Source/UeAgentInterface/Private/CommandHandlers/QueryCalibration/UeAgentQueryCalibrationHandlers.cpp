#include "CommandHandlers/QueryCalibration/UeAgentQueryCalibrationHandlers.h"

#include "Core/Contract/UeAgentCommandJson.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace UE::UAI::CommandHandlers::QueryCalibration
{
	using namespace CommandContract;
	using namespace CommandEdge;
	using namespace Contract;
	using namespace Discovery::Command;
	using namespace DomainAdapters::QueryCalibration;

	namespace
	{
		const FString Owner = TEXT("query-calibration-command-owner");

		const TCHAR* CompletenessText(const EQueryCalibrationCompleteness Value)
		{
			switch (Value)
			{
			case EQueryCalibrationCompleteness::Complete:
				return TEXT("complete");
			case EQueryCalibrationCompleteness::Paged:
				return TEXT("paged");
			case EQueryCalibrationCompleteness::Partial:
				return TEXT("partial");
			case EQueryCalibrationCompleteness::Degraded:
				return TEXT("degraded");
			default:
				return TEXT("degraded");
			}
		}

		TArray<TSharedPtr<FJsonValue>> StringValues(const TArray<FString>& Values)
		{
			TArray<TSharedPtr<FJsonValue>> Result;
			for (const FString& Value : Values)
			{
				Result.Add(MakeShared<FJsonValueString>(Value));
			}
			return Result;
		}

		bool Canonical(
			const TSharedPtr<FJsonObject>& Object,
			FString& OutJson,
			FString& OutError)
		{
			return Json::TryCanonicalize(Object, OutJson, OutError);
		}

		FUeAgentCommandResult Failure(
			const FUeAgentCommandRequest& Request,
			const FString& Code,
			const FString& Detail)
		{
			const TSharedPtr<FJsonObject> Error = MakeShared<FJsonObject>();
			Error->SetStringField(TEXT("code"), Code);
			Error->SetStringField(TEXT("detail"), Detail);
			FString ErrorJson;
			FString CanonicalError;
			if (!Canonical(Error, ErrorJson, CanonicalError))
			{
				ErrorJson = TEXT("{\"code\":\"query_error_serialization_failed\"}");
			}
			return FUeAgentCommandResult::Failure(Request, TEXT("{}"), ErrorJson);
		}

		bool HasQueryAuthority(const FUeAgentCallerContext& Caller)
		{
			return Caller.IsValid()
				&& Caller.GetAuthorityScopes().Contains(TEXT("uai:query"));
		}

		bool ReadInteger(
			const TSharedPtr<FJsonObject>& Params,
			const FString& Name,
			const int32 DefaultValue,
			int32& OutValue)
		{
			double Number = 0.0;
			if (!Params.IsValid() || !Params->HasField(Name))
			{
				OutValue = DefaultValue;
				return true;
			}
			if (!Params->TryGetNumberField(Name, Number)
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

		TSharedPtr<FJsonObject> FieldDeclarationObject(
			const FUeAgentCommandFieldDeclaration& Field)
		{
			const TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("name"), Field.Name);
			Result->SetStringField(TEXT("wire_type"), Field.WireType);
			Result->SetBoolField(TEXT("required"), Field.bRequired);
			Result->SetBoolField(TEXT("nullable"), Field.bNullable);
			Result->SetStringField(TEXT("description"), Field.Description);
			if (!Field.SchemaJson.IsEmpty())
			{
				TSharedPtr<FJsonObject> Schema;
				FString Error;
				if (Json::TryParseObject(
						Field.SchemaJson,
						Schema,
						Error)
					&& Schema.IsValid())
				{
					Result->SetObjectField(TEXT("schema"), Schema);
				}
			}
			return Result;
		}

		TArray<TSharedPtr<FJsonValue>> FieldDeclarations(
			const TArray<FUeAgentCommandFieldDeclaration>& Fields)
		{
			TArray<TSharedPtr<FJsonValue>> Result;
			for (const FUeAgentCommandFieldDeclaration& Field : Fields)
			{
				Result.Add(MakeShared<FJsonValueObject>(FieldDeclarationObject(Field)));
			}
			return Result;
		}

		TSharedPtr<FJsonObject> PurposeObject(
			const FUeAgentCommandPurposeDeclaration& Purpose)
		{
			const TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("summary"), Purpose.Summary);
			Result->SetStringField(TEXT("target"), Purpose.Target);
			Result->SetArrayField(TEXT("preconditions"), StringValues(Purpose.Preconditions));
			Result->SetStringField(TEXT("parameter_guidance"), Purpose.ParameterGuidance);
			Result->SetStringField(TEXT("result_guidance"), Purpose.ResultGuidance);
			Result->SetStringField(TEXT("failure_guidance"), Purpose.FailureGuidance);
			Result->SetStringField(TEXT("side_effect_guidance"), Purpose.SideEffectGuidance);
			Result->SetStringField(TEXT("minimum_example"), Purpose.MinimumExample);
			Result->SetArrayField(TEXT("related_flows"), StringValues(Purpose.RelatedFlows));
			Result->SetArrayField(TEXT("keywords"), StringValues(Purpose.Keywords));
			Result->SetArrayField(TEXT("synonyms"), StringValues(Purpose.Synonyms));
			return Result;
		}

		class FCommandSearchHandler final : public IUeAgentExactCommandHandler
		{
		public:
			explicit FCommandSearchHandler(
				TSharedRef<const FCommandDiscoveryService> InDiscovery)
				: Discovery(MoveTemp(InDiscovery))
			{
			}

			virtual const FString& GetCommandId() const override
			{
				static const FString Id = TEXT("query.command.search");
				return Id;
			}

			virtual const FString& GetPublicOwner() const override
			{
				return Owner;
			}

			virtual FUeAgentCommandResult Submit(
				const FUeAgentCommandRequest& Request,
				const FUeAgentCallerContext& Caller,
				const TSharedRef<IUeAgentCancellationSignal>& Cancellation) override
			{
				if (!HasQueryAuthority(Caller))
				{
					return Failure(Request, TEXT("query_authority_required"), TEXT("uai:query is required"));
				}
				if (Cancellation->IsCancelled())
				{
					return Failure(Request, TEXT("query_cancelled"), TEXT("request was cancelled"));
				}
				FCommandSearchRequest Search;
				const TSharedPtr<FJsonObject>& Params = Request.GetParams();
				if (!Params.IsValid() || !Params->TryGetStringField(TEXT("intent"), Search.Intent)
					|| !ReadInteger(Params, TEXT("limit"), 3, Search.Limit)
					|| !ReadInteger(Params, TEXT("cursor"), 0, Search.Cursor))
				{
					return Failure(Request, TEXT("command_search_params_invalid"), TEXT("intent, limit or cursor is invalid"));
				}
				FCommandSearchPage Page;
				FCommandDiscoveryError Error;
				if (!Discovery->Search(Search, Page, Error))
				{
					return Failure(Request, Error.Code, Error.Detail);
				}
				TArray<TSharedPtr<FJsonValue>> Candidates;
				for (const FCommandCandidate& Candidate : Page.Candidates)
				{
					const TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
					Item->SetStringField(TEXT("command_id"), Candidate.CommandId);
					Item->SetStringField(TEXT("contract_hash"), Candidate.ContractHash);
					Item->SetStringField(TEXT("summary"), Candidate.Summary);
					Item->SetStringField(TEXT("target"), Candidate.Target);
					Item->SetStringField(TEXT("match_reason"), Candidate.MatchReason);
					Item->SetBoolField(TEXT("read_only"), !Candidate.bHasSideEffects);
					Candidates.Add(MakeShared<FJsonValueObject>(Item));
				}
				const TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
				Payload->SetArrayField(TEXT("candidates"), MoveTemp(Candidates));
				Payload->SetNumberField(TEXT("total_matches"), Page.TotalMatches);
				Payload->SetNumberField(TEXT("next_cursor"), Page.NextCursor);
				Payload->SetStringField(TEXT("completeness"), Page.bComplete ? TEXT("complete") : TEXT("paged"));
				Payload->SetBoolField(TEXT("read_only"), true);
				FString PayloadJson;
				FString ErrorText;
				if (!Canonical(Payload, PayloadJson, ErrorText))
				{
					return Failure(Request, TEXT("query_result_serialization_failed"), ErrorText);
				}
				return FUeAgentCommandResult::Success(Request, MoveTemp(PayloadJson));
			}

		private:
			TSharedRef<const FCommandDiscoveryService> Discovery;
		};

		class FCommandDetailHandler final : public IUeAgentExactCommandHandler
		{
		public:
			explicit FCommandDetailHandler(
				TSharedRef<const FCommandDiscoveryService> InDiscovery)
				: Discovery(MoveTemp(InDiscovery))
			{
			}

			virtual const FString& GetCommandId() const override
			{
				static const FString Id = TEXT("query.command.detail");
				return Id;
			}

			virtual const FString& GetPublicOwner() const override
			{
				return Owner;
			}

			virtual FUeAgentCommandResult Submit(
				const FUeAgentCommandRequest& Request,
				const FUeAgentCallerContext& Caller,
				const TSharedRef<IUeAgentCancellationSignal>& Cancellation) override
			{
				if (!HasQueryAuthority(Caller))
				{
					return Failure(Request, TEXT("query_authority_required"), TEXT("uai:query is required"));
				}
				if (Cancellation->IsCancelled())
				{
					return Failure(Request, TEXT("query_cancelled"), TEXT("request was cancelled"));
				}
				FString CommandId;
				FString ContractHash;
				const TSharedPtr<FJsonObject>& Params = Request.GetParams();
				if (!Params.IsValid()
					|| !Params->TryGetStringField(TEXT("command_id"), CommandId)
					|| !Params->TryGetStringField(TEXT("contract_hash"), ContractHash))
				{
					return Failure(Request, TEXT("command_detail_params_invalid"), TEXT("command_id and contract_hash are required"));
				}
				FCommandDetail Detail;
				FCommandDiscoveryError Error;
				if (!Discovery->GetDetail(CommandId, ContractHash, Detail, Error))
				{
					return Failure(Request, Error.Code, Error.Detail);
				}
				const TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
				Payload->SetStringField(TEXT("command_id"), Detail.CommandId);
				Payload->SetStringField(TEXT("contract_hash"), Detail.ContractHash);
				Payload->SetStringField(TEXT("python_namespace"), Detail.PythonNamespace);
				Payload->SetStringField(TEXT("python_function"), Detail.PythonFunction);
				Payload->SetStringField(TEXT("public_owner"), Detail.PublicOwner);
				Payload->SetBoolField(TEXT("read_only"), !Detail.bHasSideEffects);
				Payload->SetObjectField(TEXT("purpose"), PurposeObject(Detail.Purpose));
				Payload->SetArrayField(TEXT("input_fields"), FieldDeclarations(Detail.InputFields));
				Payload->SetArrayField(TEXT("result_fields"), FieldDeclarations(Detail.ResultFields));
				Payload->SetArrayField(
					TEXT("tutorial_references"),
					StringValues(Detail.TutorialReferences));
				const TSharedPtr<FJsonObject> Invocation = MakeShared<FJsonObject>();
				Invocation->SetStringField(
					TEXT("python_named"),
					Detail.Purpose.MinimumExample);
				Invocation->SetStringField(
					TEXT("python_generic"),
					FString::Printf(
						TEXT("client.execute(\"%s\", params)"),
						*Detail.CommandId));
				Invocation->SetStringField(
					TEXT("python_exact"),
					FString::Printf(
						TEXT("client.execute_exact(\"%s\", params, contract_hash=\"%s\")"),
						*Detail.CommandId,
						*Detail.ContractHash));
				Invocation->SetStringField(
					TEXT("cli_exact"),
					FString::Printf(
						TEXT("uai-cli --connection connection.toml exec %s --contract-hash %s --params-json <json-object>"),
						*Detail.CommandId,
						*Detail.ContractHash));
				Payload->SetObjectField(TEXT("invocation"), Invocation);
				TArray<TSharedPtr<FJsonValue>> Errors;
				for (const FUeAgentCommandErrorDeclaration& Declared : Detail.Errors)
				{
					const TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
					Item->SetStringField(TEXT("code"), Declared.Code);
					Item->SetStringField(TEXT("description"), Declared.Description);
					Errors.Add(MakeShared<FJsonValueObject>(Item));
				}
				Payload->SetArrayField(TEXT("errors"), MoveTemp(Errors));
				FString PayloadJson;
				FString ErrorText;
				if (!Canonical(Payload, PayloadJson, ErrorText))
				{
					return Failure(Request, TEXT("query_result_serialization_failed"), ErrorText);
				}
				return FUeAgentCommandResult::Success(Request, MoveTemp(PayloadJson));
			}

		private:
			TSharedRef<const FCommandDiscoveryService> Discovery;
		};

		class FCalibrationReadHandler final : public IUeAgentExactCommandHandler
		{
		public:
			explicit FCalibrationReadHandler(
				TSharedRef<IUeAgentQueryCalibrationSource> InSource)
				: Source(MoveTemp(InSource))
			{
			}

			virtual const FString& GetCommandId() const override
			{
				static const FString Id = TEXT("query.calibration.read");
				return Id;
			}

			virtual const FString& GetPublicOwner() const override
			{
				return Owner;
			}

			virtual FUeAgentCommandResult Submit(
				const FUeAgentCommandRequest& Request,
				const FUeAgentCallerContext& Caller,
				const TSharedRef<IUeAgentCancellationSignal>& Cancellation) override
			{
				if (!HasQueryAuthority(Caller))
				{
					return Failure(Request, TEXT("query_authority_required"), TEXT("uai:query is required"));
				}
				if (Cancellation->IsCancelled())
				{
					return Failure(Request, TEXT("query_cancelled"), TEXT("request was cancelled"));
				}
				FQueryCalibrationRequest Query;
				const TSharedPtr<FJsonObject>& Params = Request.GetParams();
				if (!Params.IsValid()
					|| !Params->TryGetStringField(TEXT("category"), Query.Category)
					|| !ReadInteger(Params, TEXT("limit"), 50, Query.Limit)
					|| !ReadInteger(Params, TEXT("cursor"), 0, Query.Cursor)
					|| Query.Limit < 1 || Query.Limit > 100 || Query.Cursor < 0)
				{
					return Failure(Request, TEXT("query_params_invalid"), TEXT("category, limit or cursor is invalid"));
				}
				Params->TryGetStringField(TEXT("search_text"), Query.SearchText);

				FQueryCalibrationPage SourcePage;
				FQueryCalibrationError Error;
				if (!Source->Read(Query, SourcePage, Error))
				{
					return Failure(Request, Error.Code, Error.Detail);
				}
				if (!SourcePage.bReadOnly)
				{
					return Failure(Request, TEXT("query_source_not_read_only"), SourcePage.SourceId);
				}
				if (Query.Cursor > SourcePage.Items.Num())
				{
					return Failure(Request, TEXT("query_cursor_invalid"), TEXT("cursor exceeds the current result set"));
				}
				const int32 End = FMath::Min(Query.Cursor + Query.Limit, SourcePage.Items.Num());
				TArray<TSharedPtr<FJsonValue>> Items;
				for (int32 Index = Query.Cursor; Index < End; ++Index)
				{
					const FQueryCalibrationItem& SourceItem = SourcePage.Items[Index];
					const TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
					Item->SetStringField(TEXT("identity"), SourceItem.Identity);
					Item->SetStringField(TEXT("display_name"), SourceItem.DisplayName);
					Item->SetStringField(TEXT("kind"), SourceItem.Kind);
					const TSharedPtr<FJsonObject> Fields = MakeShared<FJsonObject>();
					for (const TPair<FString, FString>& Pair : SourceItem.Fields)
					{
						Fields->SetStringField(Pair.Key, Pair.Value);
					}
					Item->SetObjectField(TEXT("fields"), Fields);
					Items.Add(MakeShared<FJsonValueObject>(Item));
				}
				EQueryCalibrationCompleteness Completeness = SourcePage.Completeness;
				if (Completeness == EQueryCalibrationCompleteness::Complete
					&& End < SourcePage.Items.Num())
				{
					Completeness = EQueryCalibrationCompleteness::Paged;
				}
				const TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
				Payload->SetStringField(TEXT("category"), SourcePage.Category);
				Payload->SetStringField(TEXT("source_id"), SourcePage.SourceId);
				Payload->SetStringField(TEXT("state_layer"), SourcePage.StateLayer);
				Payload->SetStringField(TEXT("revision"), SourcePage.Revision);
				Payload->SetBoolField(TEXT("read_only"), true);
				Payload->SetStringField(TEXT("completeness"), CompletenessText(Completeness));
				Payload->SetNumberField(TEXT("total_items"), SourcePage.Items.Num());
				Payload->SetNumberField(TEXT("next_cursor"), End < SourcePage.Items.Num() ? End : -1);
				Payload->SetArrayField(TEXT("missing_sources"), StringValues(SourcePage.MissingSources));
				Payload->SetArrayField(TEXT("items"), MoveTemp(Items));
				FString PayloadJson;
				FString ErrorText;
				if (!Canonical(Payload, PayloadJson, ErrorText))
				{
					return Failure(Request, TEXT("query_result_serialization_failed"), ErrorText);
				}
				return FUeAgentCommandResult::Success(Request, MoveTemp(PayloadJson));
			}

		private:
			TSharedRef<IUeAgentQueryCalibrationSource> Source;
		};
	}

	bool TryCreateQueryCalibrationHandlers(
		TSharedRef<const FCommandDiscoveryService> Discovery,
		TSharedRef<IUeAgentQueryCalibrationSource> Source,
		TArray<TSharedRef<IUeAgentExactCommandHandler>>& OutHandlers,
		FString& OutError)
	{
		OutHandlers.Reset();
		OutError.Reset();
		OutHandlers = {
			MakeShared<FCommandSearchHandler>(Discovery),
			MakeShared<FCommandDetailHandler>(Discovery),
			MakeShared<FCalibrationReadHandler>(Source),
		};
		return true;
	}
}

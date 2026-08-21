#include "CommandHandlers/AssetSimple/UeAgentAssetSimpleQueryCommandHandlers.h"

#include "Core/Contract/UeAgentCommandJson.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "QueryOwners/AssetSimple/UeAgentAssetSimpleQueryBoundary.h"

namespace UE::UAI::CommandHandlers::AssetSimple
{
	using namespace CommandContract;
	using namespace CommandEdge;
	using namespace QueryOwners::AssetSimple;

	namespace
	{
		const FString CommandId = TEXT("asset.factory.candidates");
		const FString Owner = TEXT("asset-simple-query-owner");

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
			Error->SetStringField(TEXT("terminal_status"), TEXT("not_started"));
			Error->SetBoolField(TEXT("retry_safe"), true);
			FString ErrorJson;
			FString JsonError;
			if (!Json::TryCanonicalize(Error, ErrorJson, JsonError))
			{
				ErrorJson = TEXT(
					"{\"code\":\"asset_factory_candidate_error_serialization_failed\"}");
			}
			return FUeAgentCommandResult::Failure(
				Request, TEXT("{}"), MoveTemp(ErrorJson));
		}

		bool TryReadInteger(
			const TSharedPtr<FJsonObject>& Params,
			const FString& Name,
			const int32 DefaultValue,
			int32& OutValue)
		{
			OutValue = DefaultValue;
			double Number = 0.0;
			if (!Params->TryGetNumberField(Name, Number))
			{
				return !Params->HasField(Name);
			}
			if (!FMath::IsFinite(Number)
				|| !FMath::IsNearlyEqual(Number, FMath::FloorToDouble(Number))
				|| Number < static_cast<double>(MIN_int32)
				|| Number > static_cast<double>(MAX_int32))
			{
				return false;
			}
			OutValue = static_cast<int32>(Number);
			return true;
		}

		TSharedPtr<FJsonObject> RevisionObject(
			const FString& SourceId,
			const FString& Generation,
			const int64 Sequence)
		{
			const TSharedPtr<FJsonObject> Revision = MakeShared<FJsonObject>();
			Revision->SetStringField(TEXT("source_id"), SourceId);
			Revision->SetStringField(TEXT("generation"), Generation);
			Revision->SetNumberField(
				TEXT("sequence"), static_cast<double>(Sequence));
			return Revision;
		}

		class FHandler final : public IUeAgentExactCommandHandler
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
				const Contract::FUeAgentCallerContext& Caller,
				const TSharedRef<Contract::IUeAgentCancellationSignal>& Cancellation)
				override
			{
				if (!Caller.IsValid()
					|| !Caller.GetAuthorityScopes().Contains(TEXT("uai:query")))
				{
					return Failure(
						Request,
						TEXT("asset_factory_candidate_authority_required"),
						Request.GetCommandId(),
						TEXT("the uai:query authority is required"));
				}
				if (Cancellation->IsCancelled())
				{
					return Failure(
						Request,
						TEXT("asset_factory_candidate_cancelled"),
						Request.GetCommandId(),
						TEXT("the request was cancelled before catalog access"));
				}
				if (!IsInGameThread())
				{
					return Failure(
						Request,
						TEXT("asset_factory_candidate_game_thread_required"),
						Request.GetCommandId(),
						TEXT("factory discovery runs on the UE game thread"));
				}

				TSharedPtr<FJsonObject> Params;
				FString JsonError;
				if (!Json::TryParseObject(
						Request.GetParamsJson(), Params, JsonError)
					|| !Params.IsValid())
				{
					return Failure(
						Request,
						TEXT("asset_factory_candidate_params_invalid"),
						Request.GetCommandId(),
						JsonError);
				}
				FString CreateCommandId;
				FString Query;
				if (!Params->TryGetStringField(
						TEXT("create_command"), CreateCommandId)
					|| CreateCommandId.IsEmpty()
					|| CreateCommandId != CreateCommandId.TrimStartAndEnd())
				{
					return Failure(
						Request,
						TEXT("asset_factory_candidate_command_required"),
						Request.GetCommandId(),
						TEXT("create_command must be one exact command identity"));
				}
				Params->TryGetStringField(TEXT("query"), Query);
				int32 Limit = 20;
				int32 Cursor = 0;
				if (!TryReadInteger(Params, TEXT("limit"), 20, Limit)
					|| !TryReadInteger(Params, TEXT("cursor"), 0, Cursor))
				{
					return Failure(
						Request,
						TEXT("asset_factory_candidate_page_invalid"),
						CreateCommandId,
						TEXT("limit and cursor must be exact integers"));
				}

				FAssetFactoryCandidateQueryResult QueryResult;
				FAssetFactoryCandidateQueryError QueryError;
				if (!TryQueryAssetFactoryCandidates(
						CreateCommandId,
						Query,
						Limit,
						Cursor,
						QueryResult,
						QueryError))
				{
					return Failure(
						Request,
						QueryError.Code.IsEmpty()
							? FString(TEXT("asset_factory_candidate_query_failed"))
							: QueryError.Code,
						QueryError.SubjectId,
						QueryError.Detail);
				}

				const TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
				Payload->SetStringField(TEXT("command_id"), CommandId);
				Payload->SetBoolField(TEXT("read_only"), true);
				Payload->SetStringField(
					TEXT("create_command"), QueryResult.CreateCommandId);
				Payload->SetStringField(TEXT("profile_id"), QueryResult.ProfileId);
				Payload->SetObjectField(
					TEXT("catalog_revision"),
					RevisionObject(
						QueryResult.CatalogSourceId,
						QueryResult.CatalogGeneration,
						QueryResult.CatalogSequence));
				Payload->SetNumberField(
					TEXT("total_matches"), QueryResult.TotalMatches);
				Payload->SetNumberField(
					TEXT("next_cursor"), QueryResult.NextCursor);
				Payload->SetBoolField(TEXT("complete"), QueryResult.bComplete);
				Payload->SetBoolField(TEXT("has_more"), QueryResult.bHasMore);
				TArray<TSharedPtr<FJsonValue>> Candidates;
				for (const FAssetFactoryCandidate& Candidate
					: QueryResult.Candidates)
				{
					const TSharedPtr<FJsonObject> FactoryRef =
						MakeShared<FJsonObject>();
					FactoryRef->SetStringField(
						TEXT("candidate_ref"), Candidate.CandidateRef);
					FactoryRef->SetObjectField(
						TEXT("catalog_revision"),
						RevisionObject(
							Candidate.CatalogSourceId,
							Candidate.CatalogGeneration,
							Candidate.CatalogSequence));
					const TSharedPtr<FJsonObject> Value = MakeShared<FJsonObject>();
					Value->SetStringField(
						TEXT("display_name"), Candidate.DisplayName);
					Value->SetStringField(TEXT("class_path"), Candidate.ClassPath);
					Value->SetStringField(TEXT("profile_id"), Candidate.ProfileId);
					Value->SetObjectField(TEXT("factory_ref"), FactoryRef);
					Candidates.Add(MakeShared<FJsonValueObject>(Value));
				}
				Payload->SetArrayField(TEXT("candidates"), MoveTemp(Candidates));
				FString PayloadJson;
				if (!Json::TryCanonicalize(Payload, PayloadJson, JsonError))
				{
					return Failure(
						Request,
						TEXT("asset_factory_candidate_result_invalid"),
						CreateCommandId,
						JsonError);
				}
				return FUeAgentCommandResult::Success(
					Request, MoveTemp(PayloadJson));
			}
		};
	}

	bool TryCreateAssetSimpleQueryCommandHandlers(
		TArray<TSharedRef<IUeAgentExactCommandHandler>>& OutHandlers,
		FString& OutError)
	{
		OutHandlers.Reset();
		OutError.Reset();
		OutHandlers.Add(MakeShared<FHandler>());
		return true;
	}
}

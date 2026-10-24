#include "CommandHandlers/AssetSimple/UeAgentAssetSimpleQueryCommandHandlers.h"

#include "Core/ContentDiscovery/UeAgentNameDiscoveryProjection.h"
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

		FUeAgentCommandResult Failure(const FUeAgentCommandRequest& Request, UE::UAI::Contract::FDiagnostic Diagnostic)
		{
			Diagnostic.TerminalStatus = TEXT("not_started");
			Diagnostic.bRetrySafe = true;
			return FUeAgentCommandResult::Failure(Request, TEXT("{}"), MoveTemp(Diagnostic));
		}

		UE::UAI::Contract::FDiagnostic ResultFailure(const FString& Code, const FString& SubjectId, const UE::UAI::Contract::FDiagnostic& Cause)
		{
			auto Diagnostic = UE::UAI::Contract::FDiagnostic::Failure(Code, TEXT("The query input or result could not be encoded or decoded."), TEXT("command"), FString(), SubjectId);
			Diagnostic.Schema = TEXT("uai.runtime.diagnostic/1");
			if (!Cause.IsEmpty()) Diagnostic.PublicCause = MakeShared<UE::UAI::Contract::FDiagnostic>(Cause);
			return Diagnostic;
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
					return Failure(Request, UE::UAI::Contract::FDiagnostic::Failure(TEXT("asset_factory_candidate_authority_required"), TEXT("the uai:query authority is required"), TEXT("command"), FString(), Request.GetCommandId()));
				}
				if (Cancellation->IsCancelled())
				{
					return Failure(Request, UE::UAI::Contract::FDiagnostic::Failure(TEXT("asset_factory_candidate_cancelled"), TEXT("the request was cancelled before catalog access"), TEXT("command"), FString(), Request.GetCommandId()));
				}
				if (!IsInGameThread())
				{
					return Failure(Request, UE::UAI::Contract::FDiagnostic::Failure(TEXT("asset_factory_candidate_game_thread_required"), TEXT("factory discovery runs on the UE game thread"), TEXT("command"), FString(), Request.GetCommandId()));
				}

				TSharedPtr<FJsonObject> Params;
				UE::UAI::Contract::FDiagnostic JsonError;
				if (!Json::TryParseObject(
						Request.GetParamsJson(), Params, JsonError)
					|| !Params.IsValid())
				{
					return Failure(Request, ResultFailure(TEXT("asset_factory_candidate_params_invalid"), Request.GetCommandId(), JsonError));
				}
				FString CreateCommandId;
				FString Query;
				if (!Params->TryGetStringField(
						TEXT("create_command"), CreateCommandId)
					|| CreateCommandId.IsEmpty()
					|| CreateCommandId != CreateCommandId.TrimStartAndEnd())
				{
					return Failure(Request, UE::UAI::Contract::FDiagnostic::Failure(TEXT("asset_factory_candidate_command_required"), TEXT("create_command must be one exact command identity"), TEXT("command"), FString(), Request.GetCommandId()));
				}
				Params->TryGetStringField(TEXT("query"), Query);

				FAssetFactoryCandidateQueryResult QueryResult;
				FAssetFactoryCandidateQueryError QueryError;
				if (!TryQueryAssetFactoryCandidates(
						CreateCommandId,
						Query,
						QueryResult,
						QueryError))
				{
					return Failure(Request, !QueryError.IsEmpty() ? QueryError : UE::UAI::Contract::FDiagnostic::Failure(TEXT("asset_factory_candidate_query_failed"), TEXT("The candidate query failed without a diagnostic."), TEXT("query")));
				}

				TArray<ContentDiscovery::FNameSelectionEntry> Names;
				Names.Reserve(QueryResult.Candidates.Num());
				for (const FAssetFactoryCandidate& Candidate : QueryResult.Candidates)
				{
					Names.Add({Candidate.Name.IsEmpty() ? Candidate.DisplayName : Candidate.Name, Candidate.CandidateRef, {}});
				}
				FString PayloadJson;
				ContentDiscovery::FNameDiscoveryCompleteness Completeness;
				Completeness.TotalMatches = QueryResult.TotalMatches;
				Completeness.ReturnedCount = Names.Num();
				if (!ContentDiscovery::TryValidateNameDiscoveryCompleteness(
						Completeness, JsonError))
				{
					return Failure(Request, JsonError);
				}
				if (!ContentDiscovery::TryBuildCandidateNamesOnlyPayload(
					Names, ContentDiscovery::ENameComparison::IgnoreCase,
					PayloadJson, JsonError))
				{
					return Failure(Request, JsonError);
				}
				const auto Scope = MakeShared<FJsonObject>();
				Scope->SetStringField(TEXT("create_command"), CreateCommandId);
				if (!ContentDiscovery::TryAddDiscoveryScope(Scope, PayloadJson, JsonError))
					return Failure(Request, JsonError);
				return FUeAgentCommandResult::Success(Request, MoveTemp(PayloadJson));
			}
		};
	}

	bool TryCreateAssetSimpleQueryCommandHandlers(
		TArray<TSharedRef<IUeAgentExactCommandHandler>>& OutHandlers,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutHandlers.Reset();
		OutError.Reset();
		OutHandlers.Add(MakeShared<FHandler>());
		return true;
	}
}

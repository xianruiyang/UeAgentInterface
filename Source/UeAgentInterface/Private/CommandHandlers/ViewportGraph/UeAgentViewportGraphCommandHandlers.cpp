#include "CommandHandlers/ViewportGraph/UeAgentViewportGraphCommandHandlers.h"

#include "Dom/JsonObject.h"
#include "OperationOwners/ViewportGraph/UeAgentViewportGraphOperationOwner.h"
#include "QueryOwners/ViewportGraph/UeAgentViewportGraphQueryOwner.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace UE::UAI::CommandHandlers::ViewportGraph
{
	using namespace CommandEdge;

	namespace
	{
		const FString Owner(TEXT("viewport-graph-control-owner"));

		FUeAgentCommandResult Failure(const FUeAgentCommandRequest& Request,
            UE::UAI::Contract::FDiagnostic Diagnostic, const FString& SubjectId = FString())
        {
            if (Diagnostic.SubjectId.IsEmpty()) Diagnostic.SubjectId = SubjectId.IsEmpty() ? Request.GetCommandId() : SubjectId;
            if (Diagnostic.Stage.IsEmpty()) Diagnostic.Stage = TEXT("command_submit");
            return FUeAgentCommandResult::Failure(Request, TEXT("{}"), MoveTemp(Diagnostic));
        }

		class FHandler final : public IUeAgentExactCommandHandler
		{
		public:
			explicit FHandler(FString InCommandId)
				: CommandId(MoveTemp(InCommandId))
			{
			}
			const FString& GetCommandId() const override { return CommandId; }
			const FString& GetPublicOwner() const override { return Owner; }
			FUeAgentCommandResult Submit(
				const FUeAgentCommandRequest& Request,
				const Contract::FUeAgentCallerContext& Caller,
				const TSharedRef<Contract::IUeAgentCancellationSignal>& Cancellation)
				override
			{
				const bool bQuery = QueryOwners::ViewportGraph::IsQuery(CommandId);
				const FString RequiredScope = bQuery
					? FString(TEXT("uai:query"))
					: FString(TEXT("uai:operate"));
				if (!Caller.IsValid()
					|| !Caller.GetAuthorityScopes().Contains(RequiredScope))
				{
					return Failure(Request, UE::UAI::Contract::FDiagnostic::Failure(TEXT("viewport_graph_authority_required"), RequiredScope + TEXT(" is required"), TEXT("command_submit")));
				}
				if (!IsInGameThread())
				{
					return Failure(Request, UE::UAI::Contract::FDiagnostic::Failure(TEXT("viewport_graph_game_thread_required"), TEXT("native viewport mechanics must run on the game thread"), TEXT("command_submit")));
				}
				return OperationOwners::ViewportGraph::Submit(
					Request,
					Caller,
					Cancellation);
			}
		private:
			FString CommandId;
		};
	}

	bool TryCreateHandlers(
		TArray<TSharedRef<IUeAgentExactCommandHandler>>& OutHandlers,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutHandlers.Reset();
		OutError.Reset();
		for (const FString& CommandId : QueryOwners::ViewportGraph::GetCommandIds())
		{
			OutHandlers.Add(MakeShared<FHandler>(CommandId));
		}
		return OutHandlers.Num() == QueryOwners::ViewportGraph::GetCommandIds().Num();
	}
}

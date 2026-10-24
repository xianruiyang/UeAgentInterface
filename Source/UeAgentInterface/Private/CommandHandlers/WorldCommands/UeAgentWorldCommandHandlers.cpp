#include "CommandHandlers/WorldCommands/UeAgentWorldCommandHandlers.h"

#include "Dom/JsonObject.h"
#include "OperationOwners/WorldCommands/UeAgentWorldCommandOperationOwner.h"
#include "QueryOwners/WorldCommands/UeAgentWorldCommandQueryOwner.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace UE::UAI::CommandHandlers::WorldCommands
{
	using namespace CommandEdge;

	namespace
	{
		const FString Owner(TEXT("world-command-owner"));

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
			explicit FHandler(FString InCommandId) : CommandId(MoveTemp(InCommandId)) {}
			const FString& GetCommandId() const override { return CommandId; }
			const FString& GetPublicOwner() const override { return Owner; }

			FUeAgentCommandResult Submit(
				const FUeAgentCommandRequest& Request,
				const Contract::FUeAgentCallerContext& Caller,
				const TSharedRef<Contract::IUeAgentCancellationSignal>& Cancellation) override
			{
				if (!Caller.IsValid()
					|| !Caller.GetAuthorityScopes().Contains(TEXT("uai:operate")))
				{
					return Failure(Request, UE::UAI::Contract::FDiagnostic::Failure(TEXT("world_command_authority_required"), TEXT("uai:operate is required"), TEXT("command_submit")));
				}
				if (!IsInGameThread())
				{
					return Failure(Request, UE::UAI::Contract::FDiagnostic::Failure(TEXT("world_command_game_thread_required"), TEXT("native World mechanics must run on the game thread"), TEXT("command_submit")));
				}
				return OperationOwners::WorldCommands::Submit(
					Request, Caller, Cancellation);
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
		for (const FString& CommandId : QueryOwners::WorldCommands::GetCommandIds())
		{
			OutHandlers.Add(MakeShared<FHandler>(CommandId));
		}
		return OutHandlers.Num() == QueryOwners::WorldCommands::GetCommandIds().Num();
	}
}

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

		FUeAgentCommandResult Failure(
			const FUeAgentCommandRequest& Request,
			const FString& Code,
			const FString& Detail)
		{
			const TSharedRef<FJsonObject> Error = MakeShared<FJsonObject>();
			Error->SetStringField(TEXT("code"), Code);
			Error->SetStringField(TEXT("subject_id"), Request.GetCommandId());
			Error->SetStringField(TEXT("detail"), Detail);
			FString Text;
			const auto Writer = TJsonWriterFactory<TCHAR,
				TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
			FJsonSerializer::Serialize(Error, Writer);
			return FUeAgentCommandResult::Failure(Request, TEXT("{}"), Text);
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
					return Failure(Request, TEXT("world_command_authority_required"),
						TEXT("uai:operate is required"));
				}
				if (!IsInGameThread())
				{
					return Failure(Request, TEXT("world_command_game_thread_required"),
						TEXT("native World mechanics must run on the game thread"));
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
		FString& OutError)
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

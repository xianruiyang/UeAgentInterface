#include "CommandHandlers/DestructiveContent/UeAgentDestructiveContentCommandHandlers.h"

#include "Dom/JsonObject.h"
#include "OperationOwners/DestructiveContent/UeAgentDestructiveContentOperationOwner.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace UE::UAI::CommandHandlers::DestructiveContent
{
	using namespace CommandEdge;

	namespace
	{
		const FString Owner = TEXT("destructive-content-owner");

		FUeAgentCommandResult Failure(
			const FUeAgentCommandRequest& Request,
			const FString& Code,
			const FString& Detail)
		{
			const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(TEXT("code"), Code);
			Object->SetStringField(TEXT("subject_id"), Request.GetCommandId());
			Object->SetStringField(TEXT("detail"), Detail);
			Object->SetStringField(TEXT("terminal_status"), TEXT("not_started"));
			Object->SetBoolField(TEXT("retry_safe"), true);
			FString ErrorJson;
			const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>>
				Writer = TJsonWriterFactory<TCHAR,
					TCondensedJsonPrintPolicy<TCHAR>>::Create(&ErrorJson);
			FJsonSerializer::Serialize(Object, Writer);
			return FUeAgentCommandResult::Failure(Request, TEXT("{}"), ErrorJson);
		}

		class FHandler final : public IUeAgentExactCommandHandler
		{
		public:
			explicit FHandler(FString InCommandId)
				: CommandId(MoveTemp(InCommandId))
			{
			}

			virtual const FString& GetCommandId() const override
			{
				return CommandId;
			}

			virtual const FString& GetPublicOwner() const override
			{
				return Owner;
			}

			virtual FUeAgentCommandResult Submit(
				const FUeAgentCommandRequest& Request,
				const Contract::FUeAgentCallerContext& Caller,
				const TSharedRef<Contract::IUeAgentCancellationSignal>& Cancellation)
				override
			{
				if (!Caller.IsValid()
					|| !Caller.GetAuthorityScopes().Contains(TEXT("uai:operate")))
				{
					return Failure(
						Request,
						TEXT("destructive_content_authority_required"),
						TEXT("the exact destructive operation requires uai:operate"));
				}
				return OperationOwners::DestructiveContent::
					SubmitProductionDestructiveContentOperation(
						Request, Caller, Cancellation);
			}

		private:
			FString CommandId;
		};
	}

	bool TryCreateDestructiveContentCommandHandlers(
		TArray<TSharedRef<IUeAgentExactCommandHandler>>& OutHandlers,
		FString& OutError)
	{
		OutHandlers.Reset();
		OutError.Reset();
		for (const FString& CommandId : OperationOwners::DestructiveContent::
			GetDestructiveContentCommandIds())
		{
			OutHandlers.Add(MakeShared<FHandler>(CommandId));
		}
		if (OutHandlers.Num() != 14)
		{
			OutHandlers.Reset();
			OutError = TEXT("destructive_content_handler_count_mismatch");
			return false;
		}
		return true;
	}
}

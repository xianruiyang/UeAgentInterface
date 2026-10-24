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

		FUeAgentCommandResult Failure(const FUeAgentCommandRequest& Request,
            UE::UAI::Contract::FDiagnostic Diagnostic, const FString& SubjectId = FString())
        {
            if (Diagnostic.SubjectId.IsEmpty()) Diagnostic.SubjectId = SubjectId.IsEmpty() ? Request.GetCommandId() : SubjectId;
            if (Diagnostic.Stage.IsEmpty()) Diagnostic.Stage = TEXT("command_submit");
            if (Diagnostic.TerminalStatus.IsEmpty()) { Diagnostic.TerminalStatus = TEXT("not_started"); Diagnostic.bRetrySafe = true; }
            return FUeAgentCommandResult::Failure(Request, TEXT("{}"), MoveTemp(Diagnostic));
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
					return Failure(Request, UE::UAI::Contract::FDiagnostic::Failure(TEXT("destructive_content_authority_required"), TEXT("the exact destructive operation requires uai:operate"), TEXT("command_submit")));
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
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutHandlers.Reset();
		OutError.Reset();
		for (const FString& CommandId : OperationOwners::DestructiveContent::
			GetDestructiveContentCommandIds())
		{
			OutHandlers.Add(MakeShared<FHandler>(CommandId));
		}
		return true;
	}
}

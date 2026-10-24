#include "CommandHandlers/AiCommands/UeAgentAiCommandHandlers.h"

#include "DomainAdapters/AiCommands/UeAgentAiCommandMechanicsBoundary.h"
#include "OperationOwners/AiCommands/UeAgentAiCommandOperationOwner.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "QueryOwners/AiCommands/UeAgentAiCommandQueryOwner.h"

namespace UE::UAI::CommandHandlers::AiCommands
{
	using namespace CommandEdge;
	namespace
	{
		const FString Owner = TEXT("ai-command-owner");

		FUeAgentCommandResult Unauthorized(
            const FUeAgentCommandRequest& Request,
            const FString& RequiredScope)
        {
            return FUeAgentCommandResult::Failure(Request, TEXT("{}"),
                UE::UAI::Contract::FDiagnostic::Failure(TEXT("ai_command_authority_required"),
                    RequiredScope + TEXT(" is required."), TEXT("authorization"),
                    FString(), Request.GetCommandId()));
        }

		class FHandler final : public IUeAgentExactCommandHandler
		{
		public:
			FHandler(FString InId, TSharedRef<IUeAgentCommandHandler> InMechanics)
				: Id(MoveTemp(InId)), Mechanics(MoveTemp(InMechanics)) {}
			const FString& GetCommandId() const override { return Id; }
			const FString& GetPublicOwner() const override { return Owner; }
			FUeAgentCommandResult Submit(
				const FUeAgentCommandRequest& Request,
				const Contract::FUeAgentCallerContext& Caller,
				const TSharedRef<Contract::IUeAgentCancellationSignal>& Cancellation) override
			{
				const bool bQuery = DomainAdapters::AiCommands::IsQuery(Id);
				const FString RequiredScope = bQuery
					? FString(TEXT("uai:query")) : FString(TEXT("uai:operate"));
				if (!Caller.IsValid()
					|| !Caller.GetAuthorityScopes().Contains(RequiredScope))
				{
					return Unauthorized(Request, RequiredScope);
				}
				TArray<FString> Scopes = Caller.GetAuthorityScopes();
				Scopes.AddUnique(TEXT("pcg_ai_node_graph.write"));
				const Contract::FUeAgentCallerContext EffectiveCaller(
					Caller.GetCallerId(), MoveTemp(Scopes), Caller.GetCorrelationId());
				return bQuery
					? QueryOwners::AiCommands::Submit(
						Mechanics, Request, EffectiveCaller, Cancellation)
					: OperationOwners::AiCommands::Submit(
						Mechanics, Request, EffectiveCaller, Cancellation);
			}
		private:
			FString Id;
			TSharedRef<IUeAgentCommandHandler> Mechanics;
		};
	}

	bool TryCreateHandlers(
		TArray<TSharedRef<IUeAgentExactCommandHandler>>& OutHandlers,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutHandlers.Reset();
		TMap<FString, TSharedRef<IUeAgentCommandHandler>> Mechanics;
		if (!OperationOwners::AiCommands::TryCreateMechanicsHandlers(Mechanics, OutError))
		{
			return false;
		}
		for (const FString& Id : DomainAdapters::AiCommands::GetPublicCommandIds())
		{
			const auto* Match = Mechanics.Find(Id);
			if (!Match)
			{
				OutHandlers.Reset();
				OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("ai_command_mechanics_handler_missing"), TEXT("No mechanics handler is registered for this public command."), TEXT("handler_factory"), FString(), Id);
				if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = Id;
				return false;
			}
			OutHandlers.Add(MakeShared<FHandler>(Id, *Match));
		}
		return true;
	}
}

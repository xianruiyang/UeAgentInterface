#include "CommandHandlers/PcgCommands/UeAgentPcgCommandHandlers.h"

#include "DomainAdapters/PcgCommands/UeAgentPcgCommandMechanicsBoundary.h"
#include "Dom/JsonObject.h"
#include "OperationOwners/PcgCommands/UeAgentPcgCommandOperationOwner.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace UE::UAI::CommandHandlers::PcgCommands
{
	using namespace CommandEdge;
	namespace
	{
		const FString Owner = TEXT("pcg-command-owner");

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
			explicit FHandler(FString InId) : Id(MoveTemp(InId)) {}
			const FString& GetCommandId() const override { return Id; }
			const FString& GetPublicOwner() const override { return Owner; }
			FUeAgentCommandResult Submit(
				const FUeAgentCommandRequest& Request,
				const Contract::FUeAgentCallerContext& Caller,
				const TSharedRef<Contract::IUeAgentCancellationSignal>& Cancellation)
				override
			{
				const FString RequiredScope = TEXT("uai:operate");
				if (!Caller.IsValid()
					|| !Caller.GetAuthorityScopes().Contains(RequiredScope))
				{
					return Failure(Request, UE::UAI::Contract::FDiagnostic::Failure(TEXT("pcg_command_authority_required"), RequiredScope + TEXT(" is required"), TEXT("command_submit")));
				}
				if (!IsInGameThread())
				{
					return Failure(Request, UE::UAI::Contract::FDiagnostic::Failure(TEXT("pcg_command_game_thread_required"), TEXT("native PCG mechanics must start on the game thread"), TEXT("command_submit")));
				}
				TSharedPtr<IUeAgentCommandHandler> Mechanics;
				TArray<FString> MechanicsScopes;
				UE::UAI::Contract::FDiagnostic Error;
				if (!OperationOwners::PcgCommands::TryResolveMechanics(
						Id, Mechanics, MechanicsScopes, Error)
					|| !Mechanics.IsValid())
				{
					return Failure(Request, Error);
				}
				TArray<FString> Scopes = Caller.GetAuthorityScopes();
				for (const FString& Scope : MechanicsScopes)
				{
					Scopes.AddUnique(Scope);
				}
				const Contract::FUeAgentCallerContext EffectiveCaller(
					Caller.GetCallerId(), MoveTemp(Scopes), Caller.GetCorrelationId());
				return OperationOwners::PcgCommands::Submit(
					Mechanics.ToSharedRef(), Request, EffectiveCaller, Cancellation);
			}
		private:
			FString Id;
		};
	}

	bool TryCreateHandlers(
		TArray<TSharedRef<IUeAgentExactCommandHandler>>& OutHandlers,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutHandlers.Reset();
		OutError.Reset();
		for (const FString& Id : DomainAdapters::PcgCommands::GetPublicCommandIds())
		{
			OutHandlers.Add(MakeShared<FHandler>(Id));
		}
		return true;
	}
}

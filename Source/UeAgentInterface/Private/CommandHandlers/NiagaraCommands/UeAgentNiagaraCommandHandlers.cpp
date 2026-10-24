#include "CommandHandlers/NiagaraCommands/UeAgentNiagaraCommandHandlers.h"

#include "DomainAdapters/NiagaraCommands/UeAgentNiagaraCommandMechanicsBoundary.h"
#include "Dom/JsonObject.h"
#include "OperationOwners/NiagaraCommands/UeAgentNiagaraCommandOperationOwner.h"
#include "QueryOwners/NiagaraCommands/UeAgentNiagaraCommandQueryOwner.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace UE::UAI::CommandHandlers::NiagaraCommands
{
	using namespace CommandEdge;
	namespace
	{
		const FString Owner = TEXT("niagara-command-owner");

		FUeAgentCommandResult Unauthorized(
			const FUeAgentCommandRequest& Request,
			const FString& RequiredScope)
		{
            return FUeAgentCommandResult::Failure(Request, TEXT("{}"),
                UE::UAI::Contract::FDiagnostic::Failure(TEXT("niagara_command_authority_required"), RequiredScope + TEXT(" is required"), TEXT("command_authority"), FString(), Request.GetCommandId()));
        }

		class FHandler final : public IUeAgentExactCommandHandler
		{
		public:
			FHandler(FString InId, TSharedRef<IUeAgentCommandHandler> InMechanics)
				: Id(MoveTemp(InId)), Mechanics(MoveTemp(InMechanics))
			{
			}
			const FString& GetCommandId() const override { return Id; }
			const FString& GetPublicOwner() const override { return Owner; }
			FUeAgentCommandResult Submit(
				const FUeAgentCommandRequest& Request,
				const Contract::FUeAgentCallerContext& Caller,
				const TSharedRef<Contract::IUeAgentCancellationSignal>& Cancellation)
				override
			{
				const bool bQuery =
					DomainAdapters::NiagaraCommands::IsQuery(Id);
				const FString RequiredScope = bQuery
					? FString(TEXT("uai:query"))
					: FString(TEXT("uai:operate"));
				if (!Caller.IsValid()
					|| !Caller.GetAuthorityScopes().Contains(RequiredScope))
				{
					return Unauthorized(Request, RequiredScope);
				}
				TArray<FString> Scopes = Caller.GetAuthorityScopes();
				Scopes.AddUnique(TEXT("material_niagara_graph.read"));
				if (!bQuery)
				{
					Scopes.AddUnique(TEXT("material_niagara_graph.write"));
				}
				if (DomainAdapters::NiagaraCommands::
					RequiresHjsonImportAuthority(Id))
				{
					Scopes.AddUnique(TEXT("uai:authoring:hjson:import"));
				}
				const Contract::FUeAgentCallerContext EffectiveCaller(
					Caller.GetCallerId(), MoveTemp(Scopes),
					Caller.GetCorrelationId());
				return bQuery
					? QueryOwners::NiagaraCommands::Submit(
						Mechanics, Request, EffectiveCaller, Cancellation)
					: OperationOwners::NiagaraCommands::Submit(
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
		if (!OperationOwners::NiagaraCommands::TryCreateMechanicsHandlers(
				Mechanics, OutError))
		{
			return false;
		}
		for (const FString& Id
			: DomainAdapters::NiagaraCommands::GetPublicCommandIds())
		{
			const TSharedRef<IUeAgentCommandHandler>* Match = Mechanics.Find(Id);
			if (!Match)
			{
				OutHandlers.Reset();
				OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("niagara_command_mechanics_handler_missing"), TEXT("No mechanics handler is registered for this public command."), TEXT("handler_factory"), FString(), Id);
				if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = Id;
				return false;
			}
			OutHandlers.Add(MakeShared<FHandler>(Id, *Match));
		}
		return OutHandlers.Num()
			== DomainAdapters::NiagaraCommands::GetPublicCommandIds().Num();
	}
}

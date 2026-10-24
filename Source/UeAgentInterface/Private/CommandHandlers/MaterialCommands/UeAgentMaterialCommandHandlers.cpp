#include "CommandHandlers/MaterialCommands/UeAgentMaterialCommandHandlers.h"

#include "DomainAdapters/MaterialCommands/UeAgentMaterialCommandMechanicsBoundary.h"
#include "Dom/JsonObject.h"
#include "OperationOwners/MaterialCommands/UeAgentMaterialCommandOperationOwner.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace UE::UAI::CommandHandlers::MaterialCommands
{
	using namespace CommandEdge;
	namespace
	{
		const FString Owner = TEXT("material-command-owner");

		FUeAgentCommandResult Unauthorized(
			const FUeAgentCommandRequest& Request,
			const FString& RequiredScope)
		{
            return FUeAgentCommandResult::Failure(Request, TEXT("{}"),
                UE::UAI::Contract::FDiagnostic::Failure(TEXT("material_command_authority_required"), RequiredScope + TEXT(" is required"), TEXT("command_authority"), FString(), Request.GetCommandId()));
        }

		

		class FHandler final : public IUeAgentExactCommandHandler
		{
		public:
			explicit FHandler(FString InId)
				: Id(MoveTemp(InId))
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
					DomainAdapters::MaterialCommands::IsQuery(Id);
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
				const Contract::FUeAgentCallerContext EffectiveCaller(
					Caller.GetCallerId(), MoveTemp(Scopes),
					Caller.GetCorrelationId());
				FUeAgentCommandResult Result =
					OperationOwners::MaterialCommands::Submit(
						Request, EffectiveCaller, Cancellation);
				return MoveTemp(Result);
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
		for (const FString& Id
			: DomainAdapters::MaterialCommands::GetPublicCommandIds())
		{
			OutHandlers.Add(MakeShared<FHandler>(Id));
		}
		return OutHandlers.Num()
			== DomainAdapters::MaterialCommands::GetPublicCommandIds().Num();
	}
}



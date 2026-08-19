#include "CommandHandlers/PcgCommands/UeAgentPcgCommandHandlers.h"

#include "DomainAdapters/PcgCommands/UeAgentPcgCommandMechanicsBoundary.h"
#include "Dom/JsonObject.h"
#include "OperationOwners/PcgCommands/UeAgentPcgCommandOperationOwner.h"
#include "QueryOwners/PcgCommands/UeAgentPcgCommandQueryOwner.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace UE::UAI::CommandHandlers::PcgCommands
{
	using namespace CommandEdge;
	namespace
	{
		const FString Owner = TEXT("pcg-command-owner");

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
			explicit FHandler(FString InId) : Id(MoveTemp(InId)) {}
			const FString& GetCommandId() const override { return Id; }
			const FString& GetPublicOwner() const override { return Owner; }
			FUeAgentCommandResult Submit(
				const FUeAgentCommandRequest& Request,
				const Contract::FUeAgentCallerContext& Caller,
				const TSharedRef<Contract::IUeAgentCancellationSignal>& Cancellation)
				override
			{
				const bool bQuery = DomainAdapters::PcgCommands::IsQuery(Id);
				const FString RequiredScope = bQuery
					? FString(TEXT("uai:query"))
					: FString(TEXT("uai:operate"));
				if (!Caller.IsValid()
					|| !Caller.GetAuthorityScopes().Contains(RequiredScope))
				{
					return Failure(
						Request,
						TEXT("pcg_command_authority_required"),
						RequiredScope + TEXT(" is required"));
				}
				if (!IsInGameThread())
				{
					return Failure(
						Request,
						TEXT("pcg_command_game_thread_required"),
						TEXT("native PCG mechanics must start on the game thread"));
				}
				TSharedPtr<IUeAgentCommandHandler> Mechanics;
				TArray<FString> MechanicsScopes;
				FString Error;
				if (!OperationOwners::PcgCommands::TryResolveMechanics(
						Id, Mechanics, MechanicsScopes, Error)
					|| !Mechanics.IsValid())
				{
					return Failure(
						Request,
						TEXT("pcg_command_runtime_unavailable"),
						Error);
				}
				TArray<FString> Scopes = Caller.GetAuthorityScopes();
				for (const FString& Scope : MechanicsScopes)
				{
					Scopes.AddUnique(Scope);
				}
				const Contract::FUeAgentCallerContext EffectiveCaller(
					Caller.GetCallerId(), MoveTemp(Scopes), Caller.GetCorrelationId());
				return bQuery
					? QueryOwners::PcgCommands::Submit(
						Mechanics.ToSharedRef(), Request, EffectiveCaller, Cancellation)
					: OperationOwners::PcgCommands::Submit(
						Mechanics.ToSharedRef(), Request, EffectiveCaller, Cancellation);
			}
		private:
			FString Id;
		};
	}

	bool TryCreateHandlers(
		TArray<TSharedRef<IUeAgentExactCommandHandler>>& OutHandlers,
		FString& OutError)
	{
		OutHandlers.Reset();
		OutError.Reset();
		for (const FString& Id : DomainAdapters::PcgCommands::GetCommandIds())
		{
			OutHandlers.Add(MakeShared<FHandler>(Id));
		}
		return OutHandlers.Num() == DomainAdapters::PcgCommands::GetCommandIds().Num();
	}
}

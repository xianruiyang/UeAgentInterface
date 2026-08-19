#include "CommandHandlers/AudioCommands/UeAgentAudioCommandHandlers.h"

#include "DomainAdapters/AudioCommands/UeAgentAudioCommandMechanicsBoundary.h"
#include "Dom/JsonObject.h"
#include "OperationOwners/AudioCommands/UeAgentAudioCommandOperationOwner.h"
#include "QueryOwners/AudioCommands/UeAgentAudioCommandQueryOwner.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace UE::UAI::CommandHandlers::AudioCommands
{
	using namespace CommandEdge;
	namespace
	{
		const FString Owner = TEXT("audio-media-command-owner");

		FUeAgentCommandResult Unauthorized(
			const FUeAgentCommandRequest& Request,
			const FString& RequiredScope)
		{
			const TSharedRef<FJsonObject> Error = MakeShared<FJsonObject>();
			Error->SetStringField(TEXT("code"), TEXT("audio_command_authority_required"));
			Error->SetStringField(TEXT("subject_id"), Request.GetCommandId());
			Error->SetStringField(TEXT("detail"), RequiredScope + TEXT(" is required"));
			FString Text;
			const auto Writer = TJsonWriterFactory<TCHAR,
				TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
			FJsonSerializer::Serialize(Error, Writer);
			return FUeAgentCommandResult::Failure(Request, TEXT("{}"), Text);
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
				const bool bQuery = DomainAdapters::AudioCommands::IsQuery(Id);
				const FString RequiredScope = bQuery
					? FString(TEXT("uai:query")) : FString(TEXT("uai:operate"));
				if (!Caller.IsValid()
					|| !Caller.GetAuthorityScopes().Contains(RequiredScope))
				{
					return Unauthorized(Request, RequiredScope);
				}
				TArray<FString> Scopes = Caller.GetAuthorityScopes();
				Scopes.AddUnique(TEXT("audio_media_graph.write"));
				const Contract::FUeAgentCallerContext EffectiveCaller(
					Caller.GetCallerId(), MoveTemp(Scopes), Caller.GetCorrelationId());
				return bQuery
					? QueryOwners::AudioCommands::Submit(
						Mechanics, Request, EffectiveCaller, Cancellation)
					: OperationOwners::AudioCommands::Submit(
						Mechanics, Request, EffectiveCaller, Cancellation);
			}
		private:
			FString Id;
			TSharedRef<IUeAgentCommandHandler> Mechanics;
		};
	}

	bool TryCreateHandlers(
		TArray<TSharedRef<IUeAgentExactCommandHandler>>& OutHandlers,
		FString& OutError)
	{
		OutHandlers.Reset();
		TMap<FString, TSharedRef<IUeAgentCommandHandler>> Mechanics;
		if (!OperationOwners::AudioCommands::TryCreateMechanicsHandlers(Mechanics, OutError))
		{
			return false;
		}
		for (const FString& Id : DomainAdapters::AudioCommands::GetCommandIds())
		{
			const auto* Match = Mechanics.Find(Id);
			if (!Match)
			{
				OutHandlers.Reset();
				OutError = TEXT("audio_command_mechanics_handler_missing:") + Id;
				return false;
			}
			OutHandlers.Add(MakeShared<FHandler>(Id, *Match));
		}
		return OutHandlers.Num() == DomainAdapters::AudioCommands::GetCommandIds().Num();
	}
}

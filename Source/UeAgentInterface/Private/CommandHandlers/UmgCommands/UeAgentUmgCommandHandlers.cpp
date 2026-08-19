#include "CommandHandlers/UmgCommands/UeAgentUmgCommandHandlers.h"

#include "DomainAdapters/UmgCommands/UeAgentUmgCommandMechanicsBoundary.h"
#include "Dom/JsonObject.h"
#include "OperationOwners/UmgCommands/UeAgentUmgCommandOperationOwner.h"
#include "QueryOwners/UmgCommands/UeAgentUmgCommandQueryOwner.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace UE::UAI::CommandHandlers::UmgCommands
{
	using namespace CommandEdge;
	namespace
	{
		const FString Owner = TEXT("umg-command-owner");

		FUeAgentCommandResult Unauthorized(
			const FUeAgentCommandRequest& Request,
			const FString& RequiredScope)
		{
			const TSharedRef<FJsonObject> Error = MakeShared<FJsonObject>();
			Error->SetStringField(TEXT("code"), TEXT("umg_command_authority_required"));
			Error->SetStringField(TEXT("subject_id"), Request.GetCommandId());
			Error->SetStringField(TEXT("detail"), RequiredScope + TEXT(" is required"));
			FString Text;
			const auto Writer = TJsonWriterFactory<TCHAR,
				TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
			FJsonSerializer::Serialize(Error, Writer);
			return FUeAgentCommandResult::Failure(Request, TEXT("{}"), Text);
		}

		FUeAgentCommandResult NormalizeFailure(
			const FUeAgentCommandRequest& Request,
			FUeAgentCommandResult Result)
		{
			if (Result.IsOk())
			{
				return Result;
			}
			TSharedPtr<FJsonObject> Error;
			const TSharedRef<TJsonReader<>> Reader =
				TJsonReaderFactory<>::Create(Result.GetErrorJson());
			if (!FJsonSerializer::Deserialize(Reader, Error)
				|| !Error.IsValid())
			{
				return Result;
			}
			FString Code;
			if (!Error->TryGetStringField(TEXT("code"), Code))
			{
				return Result;
			}
			static const TMap<FString, FString> Details = {
				{TEXT("graph_compound_asset_target_already_exists"),
					TEXT("target_ref already resolves to an existing asset.")},
				{TEXT("graph_compound_umg_event_owner_invalid"),
					TEXT("target_ref must resolve to a Widget Blueprint and owner_item_ref must use widget:<name>.")},
				{TEXT("graph_compound_event_owner_not_found"),
					TEXT("owner_item_ref does not identify a widget in the target Widget Blueprint.")},
			};
			const FString* Detail = Details.Find(Code);
			if (!Detail)
			{
				return Result;
			}
			Error->SetStringField(TEXT("detail"), *Detail);
			FString ErrorJson;
			const auto Writer = TJsonWriterFactory<TCHAR,
				TCondensedJsonPrintPolicy<TCHAR>>::Create(&ErrorJson);
			FJsonSerializer::Serialize(Error.ToSharedRef(), Writer);
			return FUeAgentCommandResult::Failure(
				Request, Result.GetPayloadJson(), MoveTemp(ErrorJson));
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
					DomainAdapters::UmgCommands::IsQuery(Id);
				const FString RequiredScope = bQuery
					? FString(TEXT("uai:query"))
					: FString(TEXT("uai:operate"));
				if (!Caller.IsValid()
					|| !Caller.GetAuthorityScopes().Contains(RequiredScope))
				{
					return Unauthorized(Request, RequiredScope);
				}
				TArray<FString> Scopes = Caller.GetAuthorityScopes();
				Scopes.AddUnique(TEXT("blueprint_umg_graph.read"));
				if (!bQuery)
				{
					Scopes.AddUnique(TEXT("blueprint_umg_graph.write"));
				}
				const Contract::FUeAgentCallerContext EffectiveCaller(
					Caller.GetCallerId(), MoveTemp(Scopes),
					Caller.GetCorrelationId());
				FUeAgentCommandResult Result = bQuery
					? QueryOwners::UmgCommands::Submit(
						Mechanics, Request, EffectiveCaller, Cancellation)
					: OperationOwners::UmgCommands::Submit(
						Mechanics, Request, EffectiveCaller, Cancellation);
				return NormalizeFailure(Request, MoveTemp(Result));
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
		if (!OperationOwners::UmgCommands::TryCreateMechanicsHandlers(
				Mechanics, OutError))
		{
			return false;
		}
		for (const FString& Id
			: DomainAdapters::UmgCommands::GetCommandIds())
		{
			const TSharedRef<IUeAgentCommandHandler>* Match = Mechanics.Find(Id);
			if (!Match)
			{
				OutHandlers.Reset();
				OutError = TEXT("umg_command_mechanics_handler_missing:") + Id;
				return false;
			}
			OutHandlers.Add(MakeShared<FHandler>(Id, *Match));
		}
		return OutHandlers.Num()
			== DomainAdapters::UmgCommands::GetCommandIds().Num();
	}
}

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
			const TSharedRef<FJsonObject> Error = MakeShared<FJsonObject>();
			Error->SetStringField(TEXT("code"), TEXT("material_command_authority_required"));
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
				{TEXT("graph_compound_material_target_invalid"),
					TEXT("target_ref does not identify the required material asset.")},
				{TEXT("graph_compound_material_parent_invalid"),
					TEXT("parent_ref does not identify a compatible material parent.")},
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
				return NormalizeFailure(Request, MoveTemp(Result));
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
		for (const FString& Id
			: DomainAdapters::MaterialCommands::GetCommandIds())
		{
			OutHandlers.Add(MakeShared<FHandler>(Id));
		}
		return OutHandlers.Num()
			== DomainAdapters::MaterialCommands::GetCommandIds().Num();
	}
}



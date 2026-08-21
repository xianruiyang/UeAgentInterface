#include "CommandHandlers/SpatialContent/UeAgentSpatialContentCommandHandlers.h"

#include "Dom/JsonObject.h"
#include "OperationOwners/SpatialContent/UeAgentSpatialContentOperationOwner.h"
#include "QueryOwners/SpatialContent/UeAgentSpatialContentQueryOwner.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace UE::UAI::CommandHandlers::SpatialContent
{
	using namespace CommandEdge;

	namespace
	{
		const FString Owner(TEXT("spatial-content-operation-owner"));

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
			explicit FHandler(FString InCommandId)
				: CommandId(MoveTemp(InCommandId))
			{
			}
			const FString& GetCommandId() const override { return CommandId; }
			const FString& GetPublicOwner() const override { return Owner; }
			FUeAgentCommandResult Submit(
				const FUeAgentCommandRequest& Request,
				const Contract::FUeAgentCallerContext& Caller,
				const TSharedRef<Contract::IUeAgentCancellationSignal>& Cancellation)
				override
			{
				const bool bQuery = QueryOwners::SpatialContent::IsQuery(CommandId);
				const FString RequiredScope = bQuery
					? FString(TEXT("uai:query"))
					: FString(TEXT("uai:operate"));
				if (!Caller.IsValid()
					|| !Caller.GetAuthorityScopes().Contains(RequiredScope))
				{
					return Failure(
						Request,
						TEXT("spatial_content_authority_required"),
						RequiredScope + TEXT(" is required"));
				}
				if (!IsInGameThread())
				{
					return Failure(
						Request,
						TEXT("spatial_content_game_thread_required"),
						TEXT("native spatial mechanics must run on the game thread"));
				}
				return OperationOwners::SpatialContent::Submit(
					Request,
					Caller,
					Cancellation);
			}
		private:
			FString CommandId;
		};
	}

	bool TryCreateHandlers(
		TArray<TSharedRef<IUeAgentExactCommandHandler>>& OutHandlers,
		FString& OutError)
	{
		OutHandlers.Reset();
		OutError.Reset();
		for (const FString& CommandId : QueryOwners::SpatialContent::GetCommandIds())
		{
			OutHandlers.Add(MakeShared<FHandler>(CommandId));
		}
		return OutHandlers.Num() == QueryOwners::SpatialContent::GetCommandIds().Num();
	}
}

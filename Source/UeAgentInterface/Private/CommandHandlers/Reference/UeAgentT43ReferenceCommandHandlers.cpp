#include "CommandHandlers/Reference/UeAgentT43ReferenceCommandHandlers.h"

#include "DomainAdapters/Reference/UeAgentReferenceMechanicsBoundary.h"
#include "Dom/JsonObject.h"
#include "OperationOwners/Reference/UeAgentReferenceOperationOwner.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace UE::UAI::CommandHandlers::Reference
{
	using namespace CommandEdge;

	namespace
	{
		const FString Owner(TEXT("reference-operation-owner"));

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
			const FString& GetCommandId() const override
			{
				return DomainAdapters::Reference::GetCommandId();
			}
			const FString& GetPublicOwner() const override { return Owner; }

			FUeAgentCommandResult Submit(
				const FUeAgentCommandRequest& Request,
				const Contract::FUeAgentCallerContext& Caller,
				const TSharedRef<Contract::IUeAgentCancellationSignal>& Cancellation)
				override
			{
				if (!Caller.IsValid()
					|| !Caller.GetAuthorityScopes().Contains(TEXT("uai:operate")))
				{
					return Failure(
						Request,
						TEXT("reference_authority_required"),
						TEXT("uai:operate is required"));
				}
				if (!IsInGameThread())
				{
					return Failure(
						Request,
						TEXT("reference_game_thread_required"),
						TEXT("native reference mechanics must run on the game thread"));
				}
				return OperationOwners::Reference::Submit(
					Request,
					Caller,
					Cancellation);
			}
		};
	}

	bool TryCreateHandlers(
		TArray<TSharedRef<IUeAgentExactCommandHandler>>& OutHandlers,
		FString& OutError)
	{
		OutHandlers.Reset();
		OutError.Reset();
		OutHandlers.Add(MakeShared<FHandler>());
		return true;
	}
}

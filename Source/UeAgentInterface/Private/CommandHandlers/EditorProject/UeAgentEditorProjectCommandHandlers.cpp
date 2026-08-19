#include "CommandHandlers/EditorProject/UeAgentEditorProjectCommandHandlers.h"

#include "DomainAdapters/EditorProject/UeAgentEditorProjectMechanicsBoundary.h"
#include "Dom/JsonObject.h"
#include "OperationOwners/EditorProject/UeAgentEditorProjectOperationOwner.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace UE::UAI::CommandHandlers::EditorProject
{
	using namespace CommandEdge;
	namespace
	{
		const FString Owner = TEXT("editor-project-operation-owner");

		FUeAgentCommandResult Unauthorized(const FUeAgentCommandRequest& Request)
		{
			const TSharedRef<FJsonObject> Error = MakeShared<FJsonObject>();
			Error->SetStringField(TEXT("code"), TEXT("editor_project_authority_required"));
			Error->SetStringField(TEXT("subject_id"), Request.GetCommandId());
			Error->SetStringField(TEXT("detail"), TEXT("uai:operate is required"));
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
				if (!Caller.IsValid()
					|| !Caller.GetAuthorityScopes().Contains(TEXT("uai:operate")))
				{
					return Unauthorized(Request);
				}
				return OperationOwners::EditorProject::Submit(
					Request, Caller, Cancellation);
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
		for (const FString& Id : DomainAdapters::EditorProject::GetCommandIds())
		{
			OutHandlers.Add(MakeShared<FHandler>(Id));
		}
		return OutHandlers.Num() == 7;
	}
}

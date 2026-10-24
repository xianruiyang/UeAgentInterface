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
            return FUeAgentCommandResult::Failure(Request, TEXT("{}"),
                UE::UAI::Contract::FDiagnostic::Failure(TEXT("editor_project_authority_required"), TEXT("uai:operate is required"), TEXT("command_authority"), FString(), Request.GetCommandId()));
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
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutHandlers.Reset();
		OutError.Reset();
		for (const FString& Id : DomainAdapters::EditorProject::GetCommandIds())
		{
			OutHandlers.Add(MakeShared<FHandler>(Id));
		}
		return true;
	}
}

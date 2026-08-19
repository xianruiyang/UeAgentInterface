#include "CommandHandlers/UnifiedGraphLayout/UeAgentUnifiedGraphLayoutCommandHandler.h"

#include "Core/GraphLayout/UeAgentUnifiedGraphLayoutService.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace UE::UAI::CommandHandlers::UnifiedGraphLayout
{
	using namespace CommandEdge;

	namespace
	{
		const FString CommandId(TEXT("graph.layout.apply"));
		const FString Owner(TEXT("unified-graph-layout-owner"));

		FUeAgentCommandResult Failure(
			const FUeAgentCommandRequest& Request,
			const FString& Code,
			const FString& Detail)
		{
			const TSharedRef<FJsonObject> Error = MakeShared<FJsonObject>();
			Error->SetStringField(TEXT("code"), Code);
			Error->SetStringField(TEXT("subject_id"), CommandId);
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
			const FString& GetCommandId() const override { return CommandId; }
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
					return Failure(Request,
						TEXT("unified_graph_layout_authority_required"),
						TEXT("uai:operate is required"));
				}
				if (Cancellation->IsCancelled())
				{
					return Failure(Request,
						TEXT("unified_graph_layout_cancelled"), TEXT("prewrite"));
				}
				return Core::GraphLayout::Execute(Request);
			}
		};
	}

	bool TryCreateHandler(
		TSharedPtr<IUeAgentExactCommandHandler>& OutHandler,
		FString& OutError)
	{
		OutError.Reset();
		OutHandler = MakeShared<FHandler>();
		return OutHandler.IsValid();
	}
}

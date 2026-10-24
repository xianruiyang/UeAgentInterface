#include "CommandHandlers/CaptureMedia/UeAgentCaptureMediaCommandHandlers.h"

#include "Core/Contract/UeAgentContractJson.h"
#include "Dom/JsonObject.h"
#include "OperationOwners/CaptureMedia/UeAgentCaptureMediaOwner.h"

namespace UE::UAI::CommandHandlers::CaptureMedia
{
	using namespace CommandEdge;
	using namespace CommandContract;
	using namespace Contract;
	using namespace OperationOwners::CaptureMedia;
	namespace
	{
		const FString Owner = TEXT("capture-media-job-owner");
		

		class FHandler final : public IUeAgentExactCommandHandler
		{
		public:
			explicit FHandler(FUeAgentCaptureMediaRoute InRoute)
				: Route(MoveTemp(InRoute)) {}
			const FString& GetCommandId() const override { return Route.CommandId; }
			const FString& GetPublicOwner() const override { return Owner; }
			FUeAgentCommandResult Submit(
				const FUeAgentCommandRequest& Request,
				const FUeAgentCallerContext& Caller,
				const TSharedRef<IUeAgentCancellationSignal>& Cancellation) override
			{
				if (!Caller.IsValid()
					|| !Caller.GetAuthorityScopes().Contains(TEXT("uai:operate")))
				{
					return FUeAgentCommandResult::Failure(Request, TEXT("{}"),
						UE::UAI::Contract::FDiagnostic::Failure(TEXT("capture_media_authority_required"), TEXT("the public capture route requires uai:operate"), TEXT("command_authority"), FString(), Request.GetCommandId()));
				}
				return OperationOwners::CaptureMedia::Submit(
					Request, Caller, Cancellation);
			}
		private:
			FUeAgentCaptureMediaRoute Route;
		};
	}

	bool TryCreateHandlers(
		TArray<TSharedRef<IUeAgentExactCommandHandler>>& OutHandlers,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutHandlers.Reset();
		OutError.Reset();
		const TArray<FUeAgentCaptureMediaRoute>* Routes = nullptr;
		if (!TryGetRoutes(Routes, OutError)) return false;
		for (const FUeAgentCaptureMediaRoute& Route : *Routes)
			OutHandlers.Add(MakeShared<FHandler>(Route));
		return true;
	}
}

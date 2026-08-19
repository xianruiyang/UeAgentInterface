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
		FString ErrorJson(const FString& Code, const FString& Subject, const FString& Detail)
		{
			const TSharedRef<FJsonObject> Error = MakeShared<FJsonObject>();
			Error->SetStringField(TEXT("code"), Code);
			Error->SetStringField(TEXT("subject_id"), Subject);
			Error->SetStringField(TEXT("detail"), Detail);
			FString Text;
			FString CanonicalError;
			return Json::TryCanonicalize(Error, Text, CanonicalError)
				? Text : FString(TEXT("{}"));
		}

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
						ErrorJson(TEXT("capture_media_authority_required"),
							Request.GetCommandId(), TEXT("the public capture route requires uai:operate")));
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
		FString& OutError)
	{
		OutHandlers.Reset();
		OutError.Reset();
		if (GetRoutes().Num() != 3)
		{
			OutError = TEXT("capture_media_route_count_invalid");
			return false;
		}
		for (const FUeAgentCaptureMediaRoute& Route : GetRoutes())
			OutHandlers.Add(MakeShared<FHandler>(Route));
		return true;
	}
}

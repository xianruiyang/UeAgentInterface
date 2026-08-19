#include "CommandHandlers/LongRunning/UeAgentLongRunningLifecycleCommandHandlers.h"

#include "Core/Contract/UeAgentContractJson.h"
#include "Dom/JsonObject.h"

namespace UE::UAI::CommandHandlers::LongRunning
{
	using namespace CommandEdge;
	using namespace Contract;
	using namespace OperationOwners::LongRunning;

	namespace
	{
		const FString Owner = TEXT("long-running-session-owner");

		FString ErrorJson(
			const FString& Code,
			const FString& SubjectId,
			const FString& Detail)
		{
			const TSharedRef<FJsonObject> Error = MakeShared<FJsonObject>();
			Error->SetStringField(TEXT("code"), Code);
			Error->SetStringField(TEXT("subject_id"), SubjectId);
			Error->SetStringField(TEXT("detail"), Detail);
			FString Text;
			FString CanonicalError;
			return Json::TryCanonicalize(Error, Text, CanonicalError)
				? Text
				: FString(TEXT("{}"));
		}

		bool TryReadOperationError(
			const FString& PayloadJson,
			FString& OutErrorJson)
		{
			OutErrorJson.Reset();
			TSharedPtr<FJsonObject> Root;
			FString ParseError;
			if (!Json::TryParseObject(PayloadJson, Root, ParseError)
				|| !Root.IsValid())
			{
				return false;
			}
			const TSharedPtr<FJsonObject>* Error = nullptr;
			if (Root->TryGetObjectField(TEXT("operation_error"), Error)
				&& Error
				&& Error->IsValid())
			{
				return Json::TryCanonicalize(
					Error->ToSharedRef(), OutErrorJson, ParseError);
			}
			return true;
		}

		class FHandler final : public IUeAgentExactCommandHandler
		{
		public:
			FHandler(
				FUeAgentLongRunningLifecycleRoute InRoute,
				TSharedRef<IUeAgentOperationApplicationPort> InOperationPort)
				: Route(MoveTemp(InRoute))
				, OperationPort(MoveTemp(InOperationPort))
			{
			}

			virtual const FString& GetCommandId() const override
			{
				return Route.CommandId;
			}

			virtual const FString& GetPublicOwner() const override
			{
				return Owner;
			}

			virtual FUeAgentCommandResult Submit(
				const FUeAgentCommandRequest& Request,
				const FUeAgentCallerContext& CallerContext,
				const TSharedRef<IUeAgentCancellationSignal>& Cancellation)
				override
			{
				if (!CallerContext.IsValid()
					|| !CallerContext.GetAuthorityScopes().Contains(TEXT("uai:operate")))
				{
					return FUeAgentCommandResult::Failure(
						Request,
						TEXT("{}"),
						ErrorJson(
							TEXT("long_running_lifecycle_authority_required"),
							Request.GetCommandId(),
							TEXT("the public lifecycle route requires uai:operate")));
				}
				const FUeAgentOperationResult Result = OperationPort->Submit(
					FUeAgentOperationRequest(
						Route.DescriptorIdentity,
						Request.GetRequestId(),
						TEXT("uai-command:") + Request.GetRequestId(),
						FUeAgentContractValue(Request.GetParamsJson())),
					CallerContext,
					Cancellation);
				if (!Result.IsValid()
					|| !(Result.GetDescriptorIdentity() == Route.DescriptorIdentity)
					|| Result.GetRequestId() != Request.GetRequestId())
				{
					return FUeAgentCommandResult::Failure(
						Request,
						TEXT("{}"),
						ErrorJson(
							TEXT("long_running_lifecycle_result_invalid"),
							Request.GetRequestId(),
							TEXT("OperationService returned an invalid or mismatched result.")));
				}
				const FString& PayloadJson = Result.GetValue().GetJsonValue();
				FString OperationError;
				if (!TryReadOperationError(PayloadJson, OperationError))
				{
					return FUeAgentCommandResult::Failure(
						Request,
						TEXT("{}"),
						ErrorJson(
							TEXT("long_running_lifecycle_payload_invalid"),
							Request.GetRequestId(),
							TEXT("OperationService returned a non-object payload.")));
				}
				if (!OperationError.IsEmpty())
				{
					return FUeAgentCommandResult::Failure(
						Request, PayloadJson, OperationError);
				}
				return FUeAgentCommandResult::Success(Request, PayloadJson);
			}

		private:
			FUeAgentLongRunningLifecycleRoute Route;
			TSharedRef<IUeAgentOperationApplicationPort> OperationPort;
		};
	}

	bool TryCreateLongRunningLifecycleCommandHandlers(
		TSharedRef<IUeAgentOperationApplicationPort> OperationPort,
		TArray<TSharedRef<IUeAgentExactCommandHandler>>& OutHandlers,
		FString& OutError)
	{
		OutHandlers.Reset();
		OutError.Reset();
		const TArray<FUeAgentLongRunningLifecycleRoute>& Routes =
			GetLongRunningLifecycleRoutes();
		if (Routes.Num() != 6)
		{
			OutError = TEXT("long_running_lifecycle_route_count_invalid");
			return false;
		}
		TSet<FString> CommandIds;
		for (const FUeAgentLongRunningLifecycleRoute& Route : Routes)
		{
			if (!Route.IsValid() || CommandIds.Contains(Route.CommandId))
			{
				OutHandlers.Reset();
				OutError = TEXT("long_running_lifecycle_route_invalid:")
					+ Route.CommandId;
				return false;
			}
			CommandIds.Add(Route.CommandId);
			OutHandlers.Add(MakeShared<FHandler>(Route, OperationPort));
		}
		return true;
	}
}

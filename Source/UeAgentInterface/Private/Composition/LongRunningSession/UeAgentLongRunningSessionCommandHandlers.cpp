#include "Composition/LongRunningSession/UeAgentLongRunningSessionCommandHandlers.h"

#include "Core/Contract/UeAgentContractJson.h"
#include "Dom/JsonObject.h"

namespace UE::UAI::Composition::LongRunningSession
{
	using namespace CommandEdge;
	using namespace Contract;

	namespace
	{
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

		class FLongRunningSessionCommandHandler final
			: public IUeAgentCommandHandler
		{
		public:
			FLongRunningSessionCommandHandler(
				FUeAgentLongRunningSessionActivationProfile InProfile,
				TSharedRef<IUeAgentOperationApplicationPort> InOperationPort)
				: Profile(MoveTemp(InProfile))
				, OperationPort(MoveTemp(InOperationPort))
			{
			}

			const FString& GetCommandId() const override
			{
				return Profile.CommandId;
			}

			FUeAgentCommandResult Submit(
				const FUeAgentCommandRequest& Request,
				const FUeAgentCallerContext& CallerContext,
				const TSharedRef<IUeAgentCancellationSignal>& Cancellation)
				override
			{
				const FUeAgentOperationResult Result = OperationPort->Submit(
					FUeAgentOperationRequest(
						Profile.DescriptorIdentity,
						Request.GetRequestId(),
						TEXT("uai-command:") + Request.GetRequestId(),
						FUeAgentContractValue(Request.GetParamsJson())),
					CallerContext,
					Cancellation);
				if (!Result.IsValid()
					|| !(Result.GetDescriptorIdentity()
						== Profile.DescriptorIdentity)
					|| Result.GetRequestId() != Request.GetRequestId())
				{
					return FUeAgentCommandResult::Failure(
						Request,
						TEXT("{}"),
						ErrorJson(
							TEXT("command_operation_result_invalid"),
							Request.GetRequestId(),
							TEXT("C3O returned an invalid or mismatched result.")));
				}
				const FString& PayloadJson = Result.GetValue().GetJsonValue();
				FString OperationError;
				if (!TryReadOperationError(PayloadJson, OperationError))
				{
					return FUeAgentCommandResult::Failure(
						Request,
						TEXT("{}"),
						ErrorJson(
							TEXT("command_operation_payload_invalid"),
							Request.GetRequestId(),
							TEXT("C3O returned a non-object JSON payload.")));
				}
				if (!OperationError.IsEmpty())
				{
					return FUeAgentCommandResult::Failure(
						Request, PayloadJson, OperationError);
				}
				return FUeAgentCommandResult::Success(Request, PayloadJson);
			}

		private:
			const FUeAgentLongRunningSessionActivationProfile Profile;
			TSharedRef<IUeAgentOperationApplicationPort> OperationPort;
		};
	}

	bool TryCreateLongRunningSessionCommandHandlers(
		const TArray<FUeAgentLongRunningSessionActivationProfile>& Profiles,
		TSharedRef<IUeAgentOperationApplicationPort> OperationPort,
		TArray<TSharedRef<IUeAgentCommandHandler>>& OutHandlers,
		FString& OutError)
	{
		OutHandlers.Reset();
		OutError.Reset();
		if (Profiles.Num() != 12)
		{
			OutError = TEXT("long_running_session_activation_profile_count_invalid");
			return false;
		}
		TSet<FString> CommandIds;
		TSet<FString> DescriptorIds;
		OutHandlers.Reserve(Profiles.Num());
		for (const FUeAgentLongRunningSessionActivationProfile& Profile : Profiles)
		{
			const FString& DescriptorId =
				Profile.DescriptorIdentity.GetDescriptorId();
			if (!Profile.IsValid())
			{
				OutHandlers.Reset();
				OutError = TEXT("long_running_session_activation_profile_invalid");
				return false;
			}
			if (CommandIds.Contains(Profile.CommandId)
				|| DescriptorIds.Contains(DescriptorId))
			{
				OutHandlers.Reset();
				OutError = TEXT("long_running_session_command_handler_duplicate");
				return false;
			}
			CommandIds.Add(Profile.CommandId);
			DescriptorIds.Add(DescriptorId);
			OutHandlers.Add(MakeShared<FLongRunningSessionCommandHandler>(
				Profile, OperationPort));
		}
		return true;
	}
}

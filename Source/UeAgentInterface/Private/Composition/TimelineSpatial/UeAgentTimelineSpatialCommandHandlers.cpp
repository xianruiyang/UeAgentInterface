#include "Composition/TimelineSpatial/UeAgentTimelineSpatialCommandHandlers.h"

#include "Core/Contract/UeAgentContractJson.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace UE::UAI::Composition::TimelineSpatial
{
	using namespace CommandEdge;
	using namespace Contract;

	namespace
	{
		FString JsonText(const TSharedRef<FJsonObject>& Object)
		{
			FString Text;
			FString Error;
			return Json::TryCanonicalize(Object, Text, Error)
				? Text
				: FString();
		}

		FString ErrorJson(
			const FString& Code,
			const FString& SubjectId,
			const FString& Detail)
		{
			const TSharedRef<FJsonObject> Error = MakeShared<FJsonObject>();
			Error->SetStringField(TEXT("code"), Code);
			Error->SetStringField(TEXT("subject_id"), SubjectId);
			Error->SetStringField(TEXT("detail"), Detail);
			return JsonText(Error);
		}

		bool TryReadApplicationError(
			const FString& PayloadJson,
			const TCHAR* ErrorField,
			FString& OutErrorJson)
		{
			OutErrorJson.Reset();
			TSharedPtr<FJsonObject> Root;
			const TSharedRef<TJsonReader<>> Reader =
				TJsonReaderFactory<>::Create(PayloadJson);
			if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
			{
				return false;
			}
			const TSharedPtr<FJsonObject>* Error = nullptr;
			if (Root->TryGetObjectField(ErrorField, Error)
				&& Error
				&& Error->IsValid())
			{
				OutErrorJson = JsonText(Error->ToSharedRef());
			}
			return true;
		}

		class FTimelineSpatialCommandHandler final
			: public IUeAgentCommandHandler
		{
		public:
			FTimelineSpatialCommandHandler(
				FUeAgentTimelineSpatialActivationProfile InProfile,
				TSharedRef<IUeAgentQueryApplicationPort> InQueryPort,
				TSharedRef<IUeAgentOperationApplicationPort> InOperationPort)
				: Profile(MoveTemp(InProfile))
				, QueryPort(MoveTemp(InQueryPort))
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
				const TSharedRef<IUeAgentCancellationSignal>& Cancellation) override
			{
				if (Profile.OperationKind == TEXT("query"))
				{
					const FUeAgentQueryResult Result = QueryPort->Submit(
						FUeAgentQueryRequest(
							Profile.DescriptorIdentity,
							Request.GetRequestId(),
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
								TEXT("command_query_result_invalid"),
								Request.GetRequestId(),
								TEXT("C3Q returned an invalid or mismatched result.")));
					}
					const FString& PayloadJson = Result.GetValue().GetJsonValue();
					FString ApplicationError;
					if (!TryReadApplicationError(
							PayloadJson,
							TEXT("query_error"),
							ApplicationError))
					{
						return FUeAgentCommandResult::Failure(
							Request,
							TEXT("{}"),
							ErrorJson(
								TEXT("command_query_payload_invalid"),
								Request.GetRequestId(),
								TEXT("C3Q returned a non-object JSON payload.")));
					}
					if (!ApplicationError.IsEmpty())
					{
						return FUeAgentCommandResult::Failure(
							Request,
							PayloadJson,
							ApplicationError);
					}
					return FUeAgentCommandResult::Success(Request, PayloadJson);
				}

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
				FString ApplicationError;
				if (!TryReadApplicationError(
						PayloadJson,
						TEXT("operation_error"),
						ApplicationError))
				{
					return FUeAgentCommandResult::Failure(
						Request,
						TEXT("{}"),
						ErrorJson(
							TEXT("command_operation_payload_invalid"),
							Request.GetRequestId(),
							TEXT("C3O returned a non-object JSON payload.")));
				}
				if (!ApplicationError.IsEmpty())
				{
					return FUeAgentCommandResult::Failure(
						Request,
						PayloadJson,
						ApplicationError);
				}
				return FUeAgentCommandResult::Success(Request, PayloadJson);
			}

		private:
			const FUeAgentTimelineSpatialActivationProfile Profile;
			TSharedRef<IUeAgentQueryApplicationPort> QueryPort;
			TSharedRef<IUeAgentOperationApplicationPort> OperationPort;
		};
	}

	bool TryCreateTimelineSpatialCommandHandlers(
		const TArray<FUeAgentTimelineSpatialActivationProfile>& Profiles,
		TSharedRef<IUeAgentQueryApplicationPort> QueryPort,
		TSharedRef<IUeAgentOperationApplicationPort> OperationPort,
		TArray<TSharedRef<IUeAgentCommandHandler>>& OutHandlers,
		FString& OutError)
	{
		OutHandlers.Reset();
		OutError.Reset();
		if (Profiles.Num() != 60)
		{
			OutError = TEXT("timeline_spatial_activation_profile_count_invalid");
			return false;
		}

		TSet<FString> CommandIds;
		TSet<FString> DescriptorIds;
		OutHandlers.Reserve(Profiles.Num());
		for (const FUeAgentTimelineSpatialActivationProfile& Profile : Profiles)
		{
			const FString DescriptorId =
				Profile.DescriptorIdentity.GetDescriptorId();
			if (!Profile.IsValid())
			{
				OutHandlers.Reset();
				OutError = TEXT("timeline_spatial_activation_profile_invalid");
				return false;
			}
			if (CommandIds.Contains(Profile.CommandId)
				|| DescriptorIds.Contains(DescriptorId))
			{
				OutHandlers.Reset();
				OutError = TEXT("timeline_spatial_command_handler_duplicate");
				return false;
			}
			CommandIds.Add(Profile.CommandId);
			DescriptorIds.Add(DescriptorId);
			OutHandlers.Add(MakeShared<FTimelineSpatialCommandHandler>(
				Profile,
				QueryPort,
				OperationPort));
		}
		return true;
	}
}

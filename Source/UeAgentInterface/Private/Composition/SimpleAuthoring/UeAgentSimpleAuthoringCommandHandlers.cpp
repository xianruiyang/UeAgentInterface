#include "Composition/SimpleAuthoring/UeAgentSimpleAuthoringCommandHandlers.h"

#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace UE::UAI::Composition::SimpleAuthoring
{
	namespace
	{
		using namespace CommandEdge;
		using namespace Contract;

		FString JsonText(const TSharedRef<FJsonObject>& Object)
		{
			FString Text;
			const TSharedRef<
				TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>>
				Writer =
				TJsonWriterFactory<
					TCHAR,
					TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
			FJsonSerializer::Serialize(Object, Writer);
			return Text;
		}

		FString ErrorJson(
			const FString& Code,
			const FString& SubjectId,
			const FString& Detail)
		{
			const TSharedRef<FJsonObject> Error =
				MakeShared<FJsonObject>();
			Error->SetStringField(TEXT("code"), Code);
			Error->SetStringField(TEXT("subject_id"), SubjectId);
			Error->SetStringField(TEXT("detail"), Detail);
			return JsonText(Error);
		}

		bool TryReadOperationError(
			const FString& PayloadJson,
			FString& OutErrorJson)
		{
			OutErrorJson.Reset();
			TSharedPtr<FJsonObject> Root;
			const TSharedRef<TJsonReader<>> Reader =
				TJsonReaderFactory<>::Create(PayloadJson);
			if (!FJsonSerializer::Deserialize(Reader, Root)
				|| !Root.IsValid())
			{
				return false;
			}
			const TSharedPtr<FJsonObject>* Error = nullptr;
			if (Root->TryGetObjectField(TEXT("operation_error"), Error)
				&& Error
				&& Error->IsValid())
			{
				OutErrorJson = JsonText(Error->ToSharedRef());
			}
			return true;
		}

		class FSimpleAuthoringCommandHandler final
			: public IUeAgentCommandHandler
		{
		public:
			FSimpleAuthoringCommandHandler(
				FUeAgentSimpleAuthoringCommandRoute InRoute,
				TSharedRef<IUeAgentOperationApplicationPort>
					InOperationPort)
				: Route(MoveTemp(InRoute))
				, OperationPort(MoveTemp(InOperationPort))
			{
			}

			const FString& GetCommandId() const override
			{
				return Route.CommandId;
			}

			FUeAgentCommandResult Submit(
				const FUeAgentCommandRequest& Request,
				const FUeAgentCallerContext& CallerContext,
				const TSharedRef<IUeAgentCancellationSignal>&
					Cancellation) override
			{
				const FUeAgentOperationRequest OperationRequest(
					Route.DescriptorIdentity,
					Request.GetRequestId(),
					TEXT("uai-command:") + Request.GetRequestId(),
					FUeAgentContractValue(Request.GetParamsJson()));
				const FUeAgentOperationResult Result =
					OperationPort->Submit(
						OperationRequest,
						CallerContext,
						Cancellation);
				if (!Result.IsValid()
					|| !(Result.GetDescriptorIdentity()
						== Route.DescriptorIdentity)
					|| Result.GetRequestId() != Request.GetRequestId())
				{
					return FUeAgentCommandResult::Failure(
						Request,
						TEXT("{}"),
						ErrorJson(
							TEXT(
								"command_operation_result_invalid"),
							Request.GetRequestId(),
							TEXT(
								"C3O returned an invalid or mismatched result.")));
				}

				const FString& PayloadJson =
					Result.GetValue().GetJsonValue();
				FString OperationError;
				if (!TryReadOperationError(PayloadJson, OperationError))
				{
					return FUeAgentCommandResult::Failure(
						Request,
						TEXT("{}"),
						ErrorJson(
							TEXT(
								"command_operation_payload_invalid"),
							Request.GetRequestId(),
							TEXT(
								"C3O returned a non-object JSON payload.")));
				}
				if (!OperationError.IsEmpty())
				{
					return FUeAgentCommandResult::Failure(
						Request,
						PayloadJson,
						OperationError);
				}
				return FUeAgentCommandResult::Success(
					Request,
					PayloadJson);
			}

		private:
			const FUeAgentSimpleAuthoringCommandRoute Route;
			TSharedRef<IUeAgentOperationApplicationPort> OperationPort;
		};
	}

	bool TryCreateSimpleAuthoringCommandHandlers(
		const TArray<FUeAgentSimpleAuthoringCommandRoute>& Routes,
		TSharedRef<Contract::IUeAgentOperationApplicationPort>
			OperationPort,
		TArray<TSharedRef<CommandEdge::IUeAgentCommandHandler>>&
			OutHandlers,
		FString& OutError)
	{
		OutHandlers.Reset();
		OutError.Reset();
		if (Routes.IsEmpty())
		{
			OutError = TEXT("simple_authoring_command_routes_missing");
			return false;
		}

		TSet<FString> CommandIds;
		TSet<FString> DescriptorIds;
		OutHandlers.Reserve(Routes.Num());
		for (const FUeAgentSimpleAuthoringCommandRoute& Route : Routes)
		{
			const FString DescriptorId =
				Route.DescriptorIdentity.GetDescriptorId();
			if (!Route.IsValid())
			{
				OutHandlers.Reset();
				OutError = TEXT("simple_authoring_command_route_invalid");
				return false;
			}
			if (CommandIds.Contains(Route.CommandId))
			{
				OutHandlers.Reset();
				OutError = TEXT("simple_authoring_command_handler_duplicate");
				return false;
			}
			if (DescriptorIds.Contains(DescriptorId))
			{
				OutHandlers.Reset();
				OutError = TEXT(
					"simple_authoring_descriptor_handler_duplicate");
				return false;
			}
			CommandIds.Add(Route.CommandId);
			DescriptorIds.Add(DescriptorId);
			OutHandlers.Add(
				MakeShared<FSimpleAuthoringCommandHandler>(
					Route,
					OperationPort));
		}
		return true;
	}
}

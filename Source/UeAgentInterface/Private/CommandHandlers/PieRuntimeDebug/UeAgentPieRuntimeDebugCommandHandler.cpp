#include "CommandHandlers/PieRuntimeDebug/UeAgentPieRuntimeDebugCommandHandler.h"

#include "Core/Contract/UeAgentContractJson.h"
#include "Dom/JsonObject.h"
#include "OperationOwners/PieRuntimeDebug/UeAgentPieRuntimeDebugOwner.h"

namespace UE::UAI::CommandHandlers::PieRuntimeDebug
{
	using namespace CommandEdge;
	using namespace Contract;
	using namespace OperationOwners::PieRuntimeDebug;

	namespace
	{
		const FString Owner(TEXT("pie-runtime-debug-command-owner"));

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
				? Text : FString(TEXT("{}"));
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
				&& Error && Error->IsValid())
			{
				return Json::TryCanonicalize(
					Error->ToSharedRef(), OutErrorJson, ParseError);
			}
			return true;
		}

		class FHandler final : public IUeAgentExactCommandHandler
		{
		public:
			explicit FHandler(
				TSharedRef<IUeAgentOperationApplicationPort> InOperationPort)
				: Route(GetRoute())
				, OperationPort(MoveTemp(InOperationPort))
			{
			}

			const FString& GetCommandId() const override
			{
				return Route.CommandId;
			}

			const FString& GetPublicOwner() const override
			{
				return Owner;
			}

			FUeAgentCommandResult Submit(
				const FUeAgentCommandRequest& Request,
				const FUeAgentCallerContext& Caller,
				const TSharedRef<IUeAgentCancellationSignal>& Cancellation) override
			{
				if (!Caller.IsValid()
					|| !Caller.GetAuthorityScopes().Contains(TEXT("uai:operate")))
				{
					return FUeAgentCommandResult::Failure(
						Request,
						TEXT("{}"),
						ErrorJson(
							TEXT("pie_runtime_debug_authority_required"),
							Request.GetCommandId(),
							TEXT("runtime.scenario.execute requires uai:operate")));
				}
				TArray<FString> Scopes = Caller.GetAuthorityScopes();
				Scopes.AddUnique(TEXT("uai:operation"));
				Scopes.AddUnique(TEXT("uai:runtime"));
				const FUeAgentCallerContext EffectiveCaller(
					Caller.GetCallerId(),
					MoveTemp(Scopes),
					Caller.GetCorrelationId());
				const FUeAgentOperationResult Result = OperationPort->Submit(
					FUeAgentOperationRequest(
						Route.DescriptorIdentity,
						Request.GetRequestId(),
						TEXT("uai-command:") + Request.GetRequestId(),
						FUeAgentContractValue(Request.GetParamsJson())),
					EffectiveCaller,
					Cancellation);
				if (!Result.IsValid()
					|| !(Result.GetDescriptorIdentity() == Route.DescriptorIdentity)
					|| Result.GetRequestId() != Request.GetRequestId())
				{
					return FUeAgentCommandResult::Failure(
						Request,
						TEXT("{}"),
						ErrorJson(
							TEXT("pie_runtime_debug_result_invalid"),
							Request.GetRequestId(),
							TEXT("OperationService returned a mismatched result.")));
				}
				const FString& PayloadJson = Result.GetValue().GetJsonValue();
				FString OperationError;
				if (!TryReadOperationError(PayloadJson, OperationError))
				{
					return FUeAgentCommandResult::Failure(
						Request,
						TEXT("{}"),
						ErrorJson(
							TEXT("pie_runtime_debug_payload_invalid"),
							Request.GetRequestId(),
							TEXT("OperationService returned a non-object payload.")));
				}
				return OperationError.IsEmpty()
					? FUeAgentCommandResult::Success(Request, PayloadJson)
					: FUeAgentCommandResult::Failure(
						Request, PayloadJson, OperationError);
			}

		private:
			FUeAgentPieRuntimeDebugRoute Route;
			TSharedRef<IUeAgentOperationApplicationPort> OperationPort;
		};
	}

	bool TryCreateHandler(
		TSharedRef<IUeAgentOperationApplicationPort> OperationPort,
		TSharedPtr<IUeAgentExactCommandHandler>& OutHandler,
		FString& OutError)
	{
		OutHandler.Reset();
		OutError.Reset();
		if (!GetRoute().IsValid())
		{
			OutError = TEXT("pie_runtime_debug_route_invalid");
			return false;
		}
		OutHandler = MakeShared<FHandler>(MoveTemp(OperationPort));
		return true;
	}
}

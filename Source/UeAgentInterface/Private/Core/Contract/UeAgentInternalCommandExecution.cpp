#include "Core/Contract/UeAgentInternalCommandExecution.h"

#include "CommandEdge/UeAgentCommandProtocol.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Dom/JsonValue.h"
#include "Misc/Guid.h"

namespace UE::UAI::CommandContract
{
	using namespace CommandEdge;

	namespace
	{
		class FNeverCancelled final
			: public Contract::IUeAgentCancellationSignal
		{
		public:
			bool IsCancelled() const override { return false; }
		};
	}

	bool TryExecuteInternalCommand(
		const FUeAgentCommandContract& CommandContract,
		const TSharedRef<IUeAgentCommandHandler>& Handler,
		const FString& ParamsJson,
		const Contract::FUeAgentCallerContext& Caller,
		const FString& ErrorPrefix,
		FString& OutPayloadJson,
		Contract::FDiagnostic& OutError)
	{
		OutPayloadJson.Reset();
		OutError.Reset();
		const FString& CommandId = CommandContract.GetCommandId();
		if (Handler->GetCommandId() != CommandId)
		{
			OutError = Contract::FDiagnostic::Failure(ErrorPrefix + TEXT("handler_contract_mismatch"),
				TEXT("The internal command handler does not match its contract."), TEXT("internal_command"));
			OutError.SubjectId = CommandId;
			OutError.PropertyPath = TEXT("command_id");
			OutError.ExpectedValue = MakeShared<FJsonValueString>(CommandId);
			OutError.ActualValue = MakeShared<FJsonValueString>(Handler->GetCommandId());
			OutError.SourceKind = TEXT("native_result");
			return false;
		}
		const FString RequestId = ErrorPrefix + TEXT("internal-")
			+ FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
		TSharedPtr<FJsonObject> Params;
		TOptional<FUeAgentCommandRequest> Request;
		Contract::FDiagnostic RequestError;
		if (!Json::TryParseObject(ParamsJson, Params, RequestError)
			|| !FUeAgentCommandRequest::TryCreate(RequestId, RequestId, CommandId,
				CommandContract.GetBindingHash(), Params, Request, RequestError))
		{
			OutError = Contract::FDiagnostic::WrapFailure(ErrorPrefix + TEXT("request_invalid"),
				RequestError, TEXT("internal_command"));
			if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = CommandId;
			return false;
		}
		const FString EffectiveCorrelationId = Caller.GetCorrelationId().IsEmpty()
			? RequestId : Caller.GetCorrelationId();
		const Contract::FUeAgentCallerContext EffectiveCaller(
			Caller.GetCallerId(), Caller.GetAuthorityScopes(), EffectiveCorrelationId);
		const FUeAgentCommandResult Result = Handler->Submit(
			Request.GetValue(), EffectiveCaller, MakeShared<FNeverCancelled>());
		if (!Result.IsOk())
		{
			OutError = Result.GetDiagnostic();
			return false;
		}
		OutPayloadJson = Result.GetPayloadJson();
		return true;
	}
}

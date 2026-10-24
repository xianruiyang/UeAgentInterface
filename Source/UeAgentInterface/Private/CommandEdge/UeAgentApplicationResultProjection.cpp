#include "CommandEdge/UeAgentApplicationResultProjection.h"

#include "Core/Contract/UeAgentContractJson.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace UE::UAI::CommandEdge
{
	using namespace Contract;

	namespace
	{
		FString DescriptorEvidence(const FUeAgentDescriptorIdentity& Identity)
		{
			return Identity.GetDescriptorId() + TEXT(":") + FString::FromInt(Identity.GetVersion())
				+ TEXT(":") + Identity.GetDescriptorHash();
		}

		FDiagnostic ResultMismatch(
			const FString& Code, const FUeAgentCommandRequest& Request, const FString& Reason,
			const FString& Path, const FString& Actual, const FString& Expected)
		{
			FDiagnostic Diagnostic = FDiagnostic::Failure(Code, Reason, TEXT("command"),
				Request.GetCommandId(), Request.GetRequestId());
			Diagnostic.Schema = TEXT("uai.runtime.diagnostic/1");
			Diagnostic.SourceKind = TEXT("native_result");
			Diagnostic.PropertyPath = Path;
			Diagnostic.ActualEvidence = MakeShared<FJsonValueString>(Actual);
			Diagnostic.ExpectedEvidence = MakeShared<FJsonValueString>(Expected);
			return Diagnostic;
		}

		template <typename TResult>
		FUeAgentCommandResult ProjectApplicationResult(
			const FUeAgentCommandRequest& Request,
			const FUeAgentDescriptorIdentity& ExpectedDescriptor,
			const TResult& Result,
			const FApplicationResultProjectionPolicy& Policy,
			const bool bQuery)
		{
			const FString InvalidResultCode = Policy.InvalidResultCode.IsEmpty()
				? FString(bQuery ? TEXT("command_query_result_invalid") : TEXT("command_operation_result_invalid"))
				: Policy.InvalidResultCode;
			FDiagnostic ValidationError = Result.GetValidationDiagnostic();
			if (ValidationError.IsEmpty() && !(Result.GetDescriptorIdentity() == ExpectedDescriptor))
			{
				ValidationError = ResultMismatch(InvalidResultCode, Request,
					bQuery ? TEXT("The query result descriptor does not match the requested query.")
						: TEXT("The operation result descriptor does not match the requested operation."),
					TEXT("/descriptor_identity"), DescriptorEvidence(Result.GetDescriptorIdentity()),
					DescriptorEvidence(ExpectedDescriptor));
			}
			if (ValidationError.IsEmpty() && Result.GetRequestId() != Request.GetRequestId())
			{
				ValidationError = ResultMismatch(InvalidResultCode, Request,
					bQuery ? TEXT("The query result request identifier does not match the submitted request.")
						: TEXT("The operation result request identifier does not match the submitted request."),
					TEXT("/request_id"), Result.GetRequestId(), Request.GetRequestId());
			}
			if (!ValidationError.IsEmpty())
			{
				if (Result.HasDiagnostic()) ValidationError.Diagnostics.Add(Result.GetDiagnostic());
				if (Policy.bInvalidResultMayHaveMutated)
				{
					ValidationError.TerminalStatus = TEXT("state_unknown");
					ValidationError.MutationStarted.Reset();
					ValidationError.MutationMayHaveStarted = true;
					ValidationError.bRetrySafe = false;
				}
				return FUeAgentCommandResult::Failure(Request, TEXT("{}"), MoveTemp(ValidationError),
					Policy.InvalidResultCode, Policy.BoundaryFailureKind, Result.GetDiagnostics());
			}
			if (Result.HasDiagnostic())
			{
				return FUeAgentCommandResult::Failure(Request, TEXT("{}"), Result.GetDiagnostic(),
					FString(), Policy.ApplicationFailureKind, Result.GetDiagnostics());
			}
			const FString& PayloadJson = Result.GetValue().GetJsonValue();
			if (Policy.bValidateObjectPayload)
			{
				TSharedPtr<FJsonObject> Payload;
				FDiagnostic ParseError;
				if (!Json::TryParseObject(PayloadJson, Payload, ParseError))
				{
					// The envelope classifies this boundary; the parser owns the full cause.
					const FString InvalidPayloadCode = Policy.InvalidPayloadCode.IsEmpty()
						? FString(bQuery ? TEXT("command_query_payload_invalid") : TEXT("command_operation_payload_invalid"))
						: Policy.InvalidPayloadCode;
					return FUeAgentCommandResult::Failure(Request, TEXT("{}"), MoveTemp(ParseError),
						InvalidPayloadCode, Policy.BoundaryFailureKind, Result.GetDiagnostics());
				}
			}
			return FUeAgentCommandResult::Success(Request, PayloadJson, Result.GetDiagnostics());
		}
	}

	FUeAgentCommandResult ProjectOperationResult(
		const FUeAgentCommandRequest& Request,
		const FUeAgentDescriptorIdentity& ExpectedDescriptor,
		const FUeAgentOperationResult& Result,
		const FApplicationResultProjectionPolicy& Policy)
	{
		return ProjectApplicationResult(Request, ExpectedDescriptor, Result, Policy, false);
	}

	FUeAgentCommandResult ProjectQueryResult(
		const FUeAgentCommandRequest& Request,
		const FUeAgentDescriptorIdentity& ExpectedDescriptor,
		const FUeAgentQueryResult& Result)
	{
		FApplicationResultProjectionPolicy Policy;
		Policy.BoundaryFailureKind = EApplicationFailureKind::Query;
		Policy.ApplicationFailureKind = EApplicationFailureKind::Query;
		return ProjectApplicationResult(Request, ExpectedDescriptor, Result, Policy, true);
	}
}

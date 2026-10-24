#include "Composition/LongRunningSession/UeAgentLongRunningSessionCommandHandlers.h"

#include "Core/Contract/UeAgentContractJson.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace UE::UAI::Composition::LongRunningSession
{
	using namespace CommandEdge;
	using namespace Contract;

	namespace
	{
		FDiagnostic CommandFailure(const FString& Code, const FString& SubjectId, const FString& Reason)
		{
			FDiagnostic Diagnostic = FDiagnostic::Failure(Code, Reason, TEXT("command"));
			Diagnostic.Schema = TEXT("uai.runtime.diagnostic/1");
			Diagnostic.SubjectId = SubjectId;
			return Diagnostic;
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
				if (!Result.IsValid())
					return FUeAgentCommandResult::Failure(Request, TEXT("{}"), Result.GetValidationDiagnostic());
				if (!(Result.GetDescriptorIdentity() == Profile.DescriptorIdentity))
				{
					FDiagnostic Error = CommandFailure(TEXT("command_operation_result_invalid"), Profile.CommandId,
						TEXT("The operation result descriptor does not match the command's exact descriptor."));
					Error.SourceKind = TEXT("native_result");
					Error.PropertyPath = TEXT("/descriptor_identity");
					auto Describe = [](const FUeAgentDescriptorIdentity& Identity)
					{
						const auto Value = MakeShared<FJsonObject>();
						Value->SetStringField(TEXT("descriptor_id"), Identity.GetDescriptorId());
						Value->SetNumberField(TEXT("version"), Identity.GetVersion());
						return MakeShared<FJsonValueObject>(Value);
					};
					Error.ActualValue = Describe(Result.GetDescriptorIdentity());
					Error.ExpectedValue = Describe(Profile.DescriptorIdentity);
					Error.ActualEvidence = MakeShared<FJsonValueString>(Result.GetDescriptorIdentity().GetDescriptorHash());
					Error.ExpectedEvidence = MakeShared<FJsonValueString>(Profile.DescriptorIdentity.GetDescriptorHash());
					if (Result.HasDiagnostic()) Error.PublicCause = MakeShared<FDiagnostic>(Result.GetDiagnostic());
					return FUeAgentCommandResult::Failure(Request, TEXT("{}"), MoveTemp(Error));
				}
				if (Result.GetRequestId() != Request.GetRequestId())
				{
					FDiagnostic Error = CommandFailure(TEXT("command_operation_result_invalid"), Profile.CommandId,
						TEXT("The operation result belongs to a different request."));
					Error.SourceKind = TEXT("native_result");
					Error.PropertyPath = TEXT("/request_id");
					Error.ActualValue = MakeShared<FJsonValueBoolean>(false);
					Error.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
					Error.ActualEvidence = MakeShared<FJsonValueString>(Result.GetRequestId());
					Error.ExpectedEvidence = MakeShared<FJsonValueString>(Request.GetRequestId());
					if (Result.HasDiagnostic()) Error.PublicCause = MakeShared<FDiagnostic>(Result.GetDiagnostic());
					return FUeAgentCommandResult::Failure(Request, TEXT("{}"), MoveTemp(Error));
				}
				if (Result.HasDiagnostic())
				{
					return FUeAgentCommandResult::Failure(Request, TEXT("{}"), Result.GetDiagnostic(),
						FString(), EApplicationFailureKind::Operation);
				}
				const FString& PayloadJson = Result.GetValue().GetJsonValue();
				TSharedPtr<FJsonObject> Payload;
				FDiagnostic ParseError;
				if (!Json::TryParseObject(PayloadJson, Payload, ParseError))
				{
					FDiagnostic Diagnostic = CommandFailure(TEXT("command_operation_payload_invalid"), Request.GetRequestId(),
						TEXT("The operation returned a non-object JSON payload."));
					Diagnostic.PublicCause = MakeShared<FDiagnostic>(MoveTemp(ParseError));
					return FUeAgentCommandResult::Failure(Request, TEXT("{}"), MoveTemp(Diagnostic));
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
		Contract::FDiagnostic& OutError)
	{
		OutHandlers.Reset();
		OutError.Reset();
		TSet<FString> CommandIds;
		TSet<FString> DescriptorIds;
		OutHandlers.Reserve(Profiles.Num());
		for (const FUeAgentLongRunningSessionActivationProfile& Profile : Profiles)
		{
			const FString& DescriptorId =
				Profile.DescriptorIdentity.GetDescriptorId();
			if (!Profile.TryValidate(OutError))
			{
				OutHandlers.Reset();
				return false;
			}
			if (CommandIds.Contains(Profile.CommandId)
				|| DescriptorIds.Contains(DescriptorId))
			{
				OutHandlers.Reset();
				OutError = FDiagnostic::Failure(TEXT("long_running_session_command_handler_duplicate"),
					CommandIds.Contains(Profile.CommandId) ? TEXT("More than one long-running handler uses the same command id.")
					: TEXT("More than one long-running handler uses the same operation descriptor."),
					TEXT("runtime"), DescriptorId, Profile.CommandId);
				OutError.SourceKind = TEXT("native_result");
				OutError.PropertyPath = CommandIds.Contains(Profile.CommandId) ? TEXT("command_registration_count") : TEXT("descriptor_registration_count");
				OutError.ActualValue = MakeShared<FJsonValueNumber>(2);
				OutError.ExpectedValue = MakeShared<FJsonValueNumber>(1);
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

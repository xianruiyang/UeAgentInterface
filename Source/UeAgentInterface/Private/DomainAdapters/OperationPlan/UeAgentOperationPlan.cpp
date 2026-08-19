#include "DomainAdapters/OperationPlan/UeAgentOperationPlan.h"

#include "Core/Contract/UeAgentContractJson.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Editor/TransBuffer.h"
#include "HAL/IConsoleManager.h"
#include "Misc/OutputDevice.h"

namespace UE::UAI::DomainAdapters::OperationPlan
{
	using namespace CommandContract;
	using namespace CommandEdge;
	using namespace CommandRegistry;
	using namespace Contract;

	namespace
	{
		constexpr int32 MaximumSteps = 128;
		constexpr int32 MaximumConsoleCharacters = 65536;
		constexpr int32 MaximumConsoleLines = 2048;

		struct FPlanStep final
		{
			FString StepId;
			FString Kind;
			FString Action;
			FString SubjectRef;
			FString PolicyId;
			FString CommandBindingHash;
			int64 ExpectedRevision = INDEX_NONE;
			TSharedPtr<FJsonObject> Parameters;
		};

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

		bool TryExactFields(
			const TSharedPtr<FJsonObject>& Object,
			const TSet<FString>& Required,
			const TSet<FString>& Optional)
		{
			if (!Object.IsValid())
			{
				return false;
			}
			for (const FString& Field : Required)
			{
				if (!Object->HasField(Field))
				{
					return false;
				}
			}
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Field :
				Object->Values)
			{
				if (!Required.Contains(Field.Key) && !Optional.Contains(Field.Key))
				{
					return false;
				}
			}
			return true;
		}

		int64 TransactionRevision()
		{
			const UTransBuffer* Buffer = GEditor
				? Cast<UTransBuffer>(GEditor->Trans) : nullptr;
			return Buffer
				? (static_cast<int64>(Buffer->GetQueueLength()) << 32)
					| static_cast<uint32>(Buffer->GetUndoCount())
				: INDEX_NONE;
		}

		bool IsHighRiskConsoleCommand(const FString& Command)
		{
			FString Normalized = Command;
			Normalized.TrimStartAndEndInline();
			Normalized.ToLowerInline();
			static const TArray<FString> HighRiskPrefixes = {
				TEXT("quit"), TEXT("exit"), TEXT("open "), TEXT("travel "),
				TEXT("restartlevel"), TEXT("obj garbage"), TEXT("deleteall"),
			};
			for (const FString& Prefix : HighRiskPrefixes)
			{
				if (Normalized.StartsWith(Prefix))
				{
					return true;
				}
			}
			return false;
		}

		bool TryParseStep(
			const TSharedPtr<FJsonObject>& Object,
			FPlanStep& Out,
			FString& OutError)
		{
			static const TSet<FString> Required = {
				TEXT("step_id"), TEXT("kind"), TEXT("action"),
				TEXT("subject_ref"), TEXT("policy_id"), TEXT("parameters"),
			};
			static const TSet<FString> Optional = {
				TEXT("command_binding_hash"), TEXT("expected_revision"),
			};
			const TSharedPtr<FJsonObject>* Parameters = nullptr;
			if (!TryExactFields(Object, Required, Optional)
				|| !Object->TryGetStringField(TEXT("step_id"), Out.StepId)
				|| !Object->TryGetStringField(TEXT("kind"), Out.Kind)
				|| !Object->TryGetStringField(TEXT("action"), Out.Action)
				|| !Object->TryGetStringField(TEXT("subject_ref"), Out.SubjectRef)
				|| !Object->TryGetStringField(TEXT("policy_id"), Out.PolicyId)
				|| !Object->TryGetObjectField(TEXT("parameters"), Parameters)
				|| !Parameters || !Parameters->IsValid())
			{
				OutError = TEXT("operation_plan_step_shape_invalid");
				return false;
			}
			Out.Parameters = *Parameters;
			Object->TryGetStringField(
				TEXT("command_binding_hash"), Out.CommandBindingHash);
			double ExpectedRevision = 0.0;
			if (Object->TryGetNumberField(
				TEXT("expected_revision"), ExpectedRevision))
			{
				Out.ExpectedRevision = static_cast<int64>(ExpectedRevision);
			}
			Out.StepId.TrimStartAndEndInline();
			Out.SubjectRef.TrimStartAndEndInline();
			if (Out.StepId.IsEmpty() || Out.StepId.Len() > 128
				|| Out.SubjectRef.IsEmpty() || Out.SubjectRef.Len() > 512)
			{
				OutError = TEXT("operation_plan_step_identity_invalid");
				return false;
			}
			return true;
		}

		bool TryPreflightStep(
			const FPlanStep& Step,
			const FUeAgentImmutableCommandRegistry& Registry,
			FString& OutError)
		{
			if (Step.Kind == TEXT("public_command"))
			{
				const FUeAgentFrozenCommandContract* Contract =
					Registry.FindExact(Step.SubjectRef);
				if (Step.Action != TEXT("execute")
					|| Step.PolicyId != TEXT("policy:public-command:exact/v1")
					|| Step.SubjectRef == TEXT("operation.plan.execute")
					|| !Contract
					|| Contract->GetAvailability()
						!= EUeAgentCommandAvailability::Enabled
					|| Step.CommandBindingHash != Contract->GetContractHash())
				{
					OutError = TEXT("operation_plan_public_command_invalid");
					return false;
				}
				return true;
			}
			if (Step.Kind == TEXT("editor_console"))
			{
				FString Command;
				bool bAllowHighRisk = false;
				int32 MaximumCharacters = 0;
				int32 MaximumLines = 0;
				const bool bHighRiskPolicy = Step.PolicyId
					== TEXT("policy:editor-console:high-risk/v1");
				if (Step.Action != TEXT("execute")
					|| (Step.PolicyId != TEXT("policy:editor-console:bounded/v1")
						&& !bHighRiskPolicy)
					|| Step.SubjectRef != TEXT("editor:current-world")
					|| !Step.Parameters->TryGetStringField(TEXT("command"), Command)
					|| Command.IsEmpty() || Command.Len() > 4096
					|| !Step.Parameters->TryGetBoolField(
						TEXT("allow_high_risk"), bAllowHighRisk)
					|| !Step.Parameters->TryGetNumberField(
						TEXT("max_output_chars"), MaximumCharacters)
					|| !Step.Parameters->TryGetNumberField(
						TEXT("max_output_lines"), MaximumLines)
					|| MaximumCharacters < 1
					|| MaximumCharacters > MaximumConsoleCharacters
					|| MaximumLines < 1 || MaximumLines > MaximumConsoleLines
					|| (IsHighRiskConsoleCommand(Command)
						&& (!bHighRiskPolicy || !bAllowHighRisk)))
				{
					OutError = TEXT("operation_plan_editor_console_invalid");
					return false;
				}
				return true;
			}
			if (Step.Kind == TEXT("transaction_history"))
			{
				if (Step.PolicyId
						!= TEXT("policy:transaction-history:standard/v1")
					|| Step.SubjectRef != TEXT("editor:transaction-history")
					|| (Step.Action != TEXT("status")
						&& Step.Action != TEXT("undo")
						&& Step.Action != TEXT("redo"))
					|| (Step.Action != TEXT("status")
						&& Step.ExpectedRevision == INDEX_NONE))
				{
					OutError = TEXT("operation_plan_transaction_invalid");
					return false;
				}
				return true;
			}
			OutError = TEXT("operation_plan_provider_unknown");
			return false;
		}

		bool TryDecodeNestedRequest(
			const FUeAgentCommandRequest& Outer,
			const FPlanStep& Step,
			TOptional<FUeAgentCommandRequest>& Out,
			FString& OutError)
		{
			const TSharedRef<FJsonObject> Envelope = MakeShared<FJsonObject>();
			Envelope->SetStringField(TEXT("schema"), TEXT("uai.command.v1"));
			Envelope->SetNumberField(TEXT("version"), 1);
			Envelope->SetStringField(TEXT("message_kind"), TEXT("command_request"));
			Envelope->SetStringField(
				TEXT("correlation_id"), Outer.GetCorrelationId());
			Envelope->SetStringField(TEXT("request_id"),
				Outer.GetRequestId() + TEXT(":") + Step.StepId);
			Envelope->SetStringField(TEXT("command_id"), Step.SubjectRef);
			Envelope->SetStringField(
				TEXT("command_binding_hash"), Step.CommandBindingHash);
			Envelope->SetObjectField(TEXT("params"), Step.Parameters);
			TArray<uint8> Bytes;
			if (!Json::TryCanonicalUtf8(Envelope, Bytes, OutError))
			{
				return false;
			}
			FUeAgentCommandProtocolDiagnostic Diagnostic;
			if (!FUeAgentCommandProtocolCodec().DecodeRequest(
				Bytes, Out, Diagnostic) || !Out.IsSet())
			{
				OutError = Diagnostic.Code + TEXT(":") + Diagnostic.Message;
				return false;
			}
			return true;
		}

		void SetJsonOrString(
			const TSharedRef<FJsonObject>& Target,
			const FString& Name,
			const FString& JsonText)
		{
			TSharedPtr<FJsonObject> Object;
			FString Error;
			if (Json::TryParseObject(JsonText, Object, Error) && Object.IsValid())
			{
				Target->SetObjectField(Name, Object);
			}
			else
			{
				Target->SetStringField(Name, JsonText);
			}
		}

		TSharedRef<FJsonObject> ExecuteConsole(const FPlanStep& Step)
		{
			const TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			FString Command;
			bool bAddToHistory = false;
			int32 MaximumCharacters = 0;
			int32 MaximumLines = 0;
			Step.Parameters->TryGetStringField(TEXT("command"), Command);
			Step.Parameters->TryGetBoolField(
				TEXT("add_to_history"), bAddToHistory);
			Step.Parameters->TryGetNumberField(
				TEXT("max_output_chars"), MaximumCharacters);
			Step.Parameters->TryGetNumberField(
				TEXT("max_output_lines"), MaximumLines);
			FStringOutputDevice Output;
			UWorld* World = GEditor
				? GEditor->GetEditorWorldContext().World() : nullptr;
			const bool bHandled = World && IConsoleManager::Get()
				.ProcessUserConsoleInput(*Command, Output, World);
			if (bHandled && bAddToHistory)
			{
				IConsoleManager::Get().AddConsoleHistoryEntry(TEXT("UAI"), *Command);
			}
			TArray<FString> Lines;
			Output.ParseIntoArrayLines(Lines, false);
			const bool bTruncatedByLines = Lines.Num() > MaximumLines;
			if (bTruncatedByLines)
			{
				Lines.SetNum(MaximumLines);
			}
			FString BoundedOutput = FString::Join(Lines, TEXT("\n"));
			const bool bTruncatedByCharacters =
				BoundedOutput.Len() > MaximumCharacters;
			BoundedOutput.LeftInline(MaximumCharacters);
			Result->SetBoolField(TEXT("ok"), bHandled);
			Result->SetStringField(TEXT("observation"),
				bHandled ? TEXT("console_executed") : TEXT("console_unhandled"));
			Result->SetStringField(TEXT("output"), BoundedOutput);
			Result->SetBoolField(TEXT("output_truncated"),
				bTruncatedByLines || bTruncatedByCharacters);
			return Result;
		}

		TSharedRef<FJsonObject> ExecuteTransaction(const FPlanStep& Step)
		{
			const TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			UTransBuffer* Buffer = GEditor ? Cast<UTransBuffer>(GEditor->Trans) : nullptr;
			const int64 BeforeRevision = TransactionRevision();
			bool bOk = Buffer != nullptr;
			if (bOk && Step.Action != TEXT("status")
				&& Step.ExpectedRevision != BeforeRevision)
			{
				bOk = false;
				Result->SetStringField(
					TEXT("observation"), TEXT("history_revision_mismatch"));
			}
			else if (bOk && Step.Action == TEXT("undo"))
			{
				bOk = Buffer->CanUndo() && GEditor->UndoTransaction();
			}
			else if (bOk && Step.Action == TEXT("redo"))
			{
				bOk = Buffer->CanRedo() && GEditor->RedoTransaction();
			}
			if (!Result->HasField(TEXT("observation")))
			{
				Result->SetStringField(TEXT("observation"), bOk
					? (Step.Action == TEXT("status")
						? TEXT("history_observed") : TEXT("transaction_applied"))
					: TEXT("transaction_unavailable"));
			}
			Result->SetBoolField(TEXT("ok"), bOk);
			Result->SetNumberField(TEXT("before_revision"), BeforeRevision);
			Result->SetNumberField(TEXT("after_revision"), TransactionRevision());
			Result->SetBoolField(TEXT("persistent"), false);
			return Result;
		}
	}

	FUeAgentOperationPlan::FUeAgentOperationPlan(
		TSharedRef<const FUeAgentImmutableCommandRegistry> InRegistry)
		: Registry(MoveTemp(InRegistry))
	{
	}

	bool FUeAgentOperationPlan::BindNestedExactDispatch(
		TWeakPtr<const FUeAgentExactCommandDispatch> InDispatch,
		TSharedRef<const FUeAgentImmutableCommandRegistry> InRegistry,
		FString& OutError)
	{
		OutError.Reset();
		if (!InDispatch.IsValid()
			|| InRegistry->GetRegistryHash() != Registry->GetRegistryHash())
		{
			OutError = TEXT("operation_plan_nested_dispatch_identity_invalid");
			return false;
		}
		Dispatch = MoveTemp(InDispatch);
		return true;
	}

	FUeAgentCommandResult FUeAgentOperationPlan::Execute(
		const FUeAgentCommandRequest& Request,
		const FUeAgentCallerContext& Caller,
		const TSharedRef<IUeAgentCancellationSignal>& Cancellation) const
	{
		if (!IsInGameThread())
		{
			return FUeAgentCommandResult::Failure(Request, TEXT("{}"),
				ErrorJson(TEXT("operation_plan_game_thread_required"),
					Request.GetCommandId(), TEXT("Editor providers require the game thread.")));
		}
		if (!Caller.IsValid()
			|| !Caller.GetAuthorityScopes().Contains(TEXT("uai:operate")))
		{
			return FUeAgentCommandResult::Failure(Request, TEXT("{}"),
				ErrorJson(TEXT("operation_plan_authority_required"),
					Request.GetCommandId(), TEXT("uai:operate is required")));
		}
		const TArray<TSharedPtr<FJsonValue>>* StepValues = nullptr;
		FString OnError;
		FString Atomicity;
		if (!Request.GetParams().IsValid()
			|| !Request.GetParams()->TryGetArrayField(TEXT("steps"), StepValues)
			|| !StepValues || StepValues->IsEmpty()
			|| StepValues->Num() > MaximumSteps
			|| !Request.GetParams()->TryGetStringField(TEXT("on_error"), OnError)
			|| !Request.GetParams()->TryGetStringField(TEXT("atomicity"), Atomicity)
			|| (OnError != TEXT("stop") && OnError != TEXT("continue"))
			|| (Atomicity != TEXT("reject_non_atomic")
				&& Atomicity != TEXT("best_effort")))
		{
			return FUeAgentCommandResult::Failure(Request, TEXT("{}"),
				ErrorJson(TEXT("operation_plan_input_invalid"),
					Request.GetCommandId(), TEXT("invalid bounded plan contract")));
		}

		TArray<FPlanStep> Steps;
		TSet<FString> StepIds;
		for (const TSharedPtr<FJsonValue>& Value : *StepValues)
		{
			FPlanStep Step;
			FString PreflightError;
			if (!Value.IsValid()
				|| !TryParseStep(Value->AsObject(), Step, PreflightError)
				|| StepIds.Contains(Step.StepId)
				|| !TryPreflightStep(Step, *Registry, PreflightError))
			{
				return FUeAgentCommandResult::Failure(Request, TEXT("{}"),
					ErrorJson(TEXT("operation_plan_preflight_failed"),
						Step.StepId, PreflightError));
			}
			StepIds.Add(Step.StepId);
			Steps.Add(MoveTemp(Step));
		}
		if (Atomicity == TEXT("reject_non_atomic")
			&& (Steps.Num() != 1 || Steps[0].Kind != TEXT("transaction_history")))
		{
			return FUeAgentCommandResult::Failure(Request, TEXT("{}"),
				ErrorJson(TEXT("operation_plan_non_atomic_rejected"),
					Request.GetCommandId(), TEXT("only one transaction-history step is atomic")));
		}

		const TSharedPtr<const FUeAgentExactCommandDispatch> ExactDispatch =
			Dispatch.Pin();
		if (!ExactDispatch.IsValid())
		{
			return FUeAgentCommandResult::Failure(Request, TEXT("{}"),
				ErrorJson(TEXT("operation_plan_nested_dispatch_unavailable"),
					Request.GetCommandId(), TEXT("the process exact dispatch is unavailable")));
		}

		TArray<TSharedPtr<FJsonValue>> Results;
		bool bPlanOk = true;
		for (const FPlanStep& Step : Steps)
		{
			if (Cancellation->IsCancelled())
			{
				bPlanOk = false;
				break;
			}
			TSharedRef<FJsonObject> StepResult = MakeShared<FJsonObject>();
			if (Step.Kind == TEXT("public_command"))
			{
				TOptional<FUeAgentCommandRequest> NestedRequest;
				FString DecodeError;
				if (!TryDecodeNestedRequest(
					Request, Step, NestedRequest, DecodeError))
				{
					StepResult->SetBoolField(TEXT("ok"), false);
					StepResult->SetStringField(
						TEXT("observation"), TEXT("nested_request_invalid"));
					StepResult->SetStringField(TEXT("detail"), DecodeError);
				}
				else
				{
					const FUeAgentCommandResult NestedResult = ExactDispatch->Submit(
						NestedRequest.GetValue(), Caller, Cancellation);
					StepResult->SetBoolField(TEXT("ok"), NestedResult.IsOk());
					StepResult->SetStringField(TEXT("observation"),
						NestedResult.IsOk()
							? TEXT("public_command_completed")
							: TEXT("public_command_failed"));
					SetJsonOrString(
						StepResult, TEXT("payload"), NestedResult.GetPayloadJson());
					if (!NestedResult.IsOk())
					{
						SetJsonOrString(
							StepResult, TEXT("error"), NestedResult.GetErrorJson());
					}
				}
			}
			else if (Step.Kind == TEXT("editor_console"))
			{
				StepResult = ExecuteConsole(Step);
			}
			else
			{
				StepResult = ExecuteTransaction(Step);
			}
			StepResult->SetStringField(TEXT("step_id"), Step.StepId);
			bool bStepOk = false;
			StepResult->TryGetBoolField(TEXT("ok"), bStepOk);
			bPlanOk = bPlanOk && bStepOk;
			Results.Add(MakeShared<FJsonValueObject>(StepResult));
			if (!bStepOk && OnError == TEXT("stop"))
			{
				break;
			}
		}

		const TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
		Payload->SetStringField(TEXT("schema"), TEXT("uai.operation.plan.receipt.v1"));
		Payload->SetBoolField(TEXT("ok"), bPlanOk);
		Payload->SetStringField(TEXT("atomicity"), Atomicity);
		Payload->SetStringField(TEXT("completion"), bPlanOk
			? TEXT("complete") : TEXT("partial_or_failed"));
		Payload->SetArrayField(TEXT("steps"), MoveTemp(Results));
		FString PayloadJson;
		FString JsonError;
		if (!Json::TryCanonicalize(Payload, PayloadJson, JsonError))
		{
			return FUeAgentCommandResult::Failure(Request, TEXT("{}"),
				ErrorJson(TEXT("operation_plan_receipt_invalid"),
					Request.GetCommandId(), JsonError));
		}
		return bPlanOk
			? FUeAgentCommandResult::Success(Request, MoveTemp(PayloadJson))
			: FUeAgentCommandResult::Failure(Request, MoveTemp(PayloadJson),
				ErrorJson(TEXT("operation_plan_step_failed"),
					Request.GetCommandId(), TEXT("one or more steps failed")));
	}
}

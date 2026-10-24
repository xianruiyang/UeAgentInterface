#include "DomainAdapters/OperationPlan/UeAgentOperationPlan.h"

#include "Core/Contract/UeAgentDiagnostic.h"

#include "Core/Contract/UeAgentContractJson.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include <initializer_list>
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
		struct FPlanStep final
		{
			FString StepId;
			FString InputPath;
			TSharedPtr<FJsonValue> ExpectedRevisionInput;
			FString Kind;
			FString Action;
			FString SubjectRef;
			FString PolicyId;
			int64 ExpectedRevision = INDEX_NONE;
			TSharedPtr<FJsonObject> Parameters;
		};

		

		FDiagnostic PlanFact(
			const TCHAR* Code, const FString& Reason, const FString& Path,
			TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected,
			const FString& StepId = FString(), const TCHAR* SourceKind = TEXT("command_input"))
		{
			FDiagnostic Error = FDiagnostic::Failure(Code, Reason, TEXT("operation_plan"));
			Error.PropertyPath = Path;
			Error.SemanticPointer = Path;
			Error.Source.JsonPath = Path;
			Error.SourceKind = SourceKind;
			Error.CandidateName = StepId;
			Error.ActualValue = MoveTemp(Actual);
			Error.ExpectedValue = MoveTemp(Expected);
			return Error;
		}

		FString FieldPath(const FString& Parent, FString Field)
		{
			Field.ReplaceInline(TEXT("~"), TEXT("~0"));
			Field.ReplaceInline(TEXT("/"), TEXT("~1"));
			return Parent + TEXT("/") + Field;
		}

		TSharedPtr<FJsonValue> InputFact(const TSharedPtr<FJsonValue>& Value)
		{
			if (!Value.IsValid()) return nullptr;
			switch (Value->Type)
			{
			case EJson::Object: return MakeShared<FJsonValueString>(TEXT("object"));
			case EJson::Array:
			{
				const TSharedRef<FJsonObject> Shape = MakeShared<FJsonObject>();
				Shape->SetStringField(TEXT("type"), TEXT("array"));
				Shape->SetNumberField(TEXT("count"), Value->AsArray().Num());
				return MakeShared<FJsonValueObject>(Shape);
			}
			case EJson::None: return MakeShared<FJsonValueString>(TEXT("none"));
			default: return Value;
			}
		}

		TSharedPtr<FJsonValue> Choices(std::initializer_list<const TCHAR*> Values)
		{
			TArray<TSharedPtr<FJsonValue>> Items;
			for (const TCHAR* Value : Values) Items.Add(MakeShared<FJsonValueString>(Value));
			return MakeShared<FJsonValueArray>(MoveTemp(Items));
		}

		FDiagnostic InputFailure(
			const TCHAR* Code, const FString& Reason, const FString& Path,
			const TSharedPtr<FJsonValue>& Actual, TSharedPtr<FJsonValue> Expected,
			const FString& StepId = FString())
		{
			FDiagnostic Error = PlanFact(Code, Reason, Path, InputFact(Actual), MoveTemp(Expected), StepId);
			if (Actual.IsValid() && (Actual->Type == EJson::Object || Actual->Type == EJson::Array))
				Error.ActualEvidence = Actual;
			Error.ActualValuePresent = Actual.IsValid();
			Error.ExpectedValuePresent = true;
			return Error;
		}

		bool TryReadString(
			const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, const FString& Parent,
			FString& Out, FDiagnostic& OutError, const TCHAR* Code)
		{
			if (Object->TryGetStringField(Field, Out)) return true;
			OutError = InputFailure(Code, FString::Printf(TEXT("Field '%s' must be a string."), Field),
				FieldPath(Parent, Field), Object->TryGetField(Field), MakeShared<FJsonValueString>(TEXT("string")));
			return false;
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
			const TSharedPtr<FJsonValue>& Value, const FString& Path,
			FPlanStep& Out, FDiagnostic& OutError)
		{
			Out.InputPath = Path;
			if (!Value.IsValid() || Value->Type != EJson::Object || !Value->AsObject().IsValid())
			{
				OutError = InputFailure(TEXT("operation_plan_step_shape_invalid"),
					TEXT("Each operation plan step must be an object."), Path, Value,
					MakeShared<FJsonValueString>(TEXT("object")));
				return false;
			}
			const TSharedPtr<FJsonObject> Object = Value->AsObject();
			static const TSet<FString> Required = {
				TEXT("step_id"), TEXT("kind"), TEXT("action"),
				TEXT("subject_ref"), TEXT("policy_id"), TEXT("parameters"),
			};
			static const TSet<FString> Optional = {
				TEXT("command_binding_hash"), TEXT("expected_revision"),
			};
			for (const FString& Field : Required)
			{
				if (!Object->HasField(Field))
				{
					OutError = InputFailure(TEXT("operation_plan_step_shape_invalid"),
						FString::Printf(TEXT("Required step field '%s' is missing."), *Field),
						FieldPath(Path, Field), nullptr, MakeShared<FJsonValueString>(TEXT("present")));
					return false;
				}
			}
			for (const auto& Field : Object->Values)
			{
				if (!Required.Contains(Field.Key) && !Optional.Contains(Field.Key))
				{
					OutError = PlanFact(TEXT("operation_plan_step_shape_invalid"),
						FString::Printf(TEXT("Unknown operation plan step field '%s'."), *Field.Key),
						FieldPath(Path, Field.Key), MakeShared<FJsonValueString>(Field.Key),
						Choices({TEXT("step_id"), TEXT("kind"), TEXT("action"), TEXT("subject_ref"),
							TEXT("policy_id"), TEXT("parameters"), TEXT("command_binding_hash"), TEXT("expected_revision")}));
					OutError.ActualEvidence = Field.Value;
					return false;
				}
			}
			if (!TryReadString(Object, TEXT("step_id"), Path, Out.StepId, OutError, TEXT("operation_plan_step_shape_invalid"))
				|| !TryReadString(Object, TEXT("kind"), Path, Out.Kind, OutError, TEXT("operation_plan_step_shape_invalid"))
				|| !TryReadString(Object, TEXT("action"), Path, Out.Action, OutError, TEXT("operation_plan_step_shape_invalid"))
				|| !TryReadString(Object, TEXT("subject_ref"), Path, Out.SubjectRef, OutError, TEXT("operation_plan_step_shape_invalid"))
				|| !TryReadString(Object, TEXT("policy_id"), Path, Out.PolicyId, OutError, TEXT("operation_plan_step_shape_invalid")))
			{
				return false;
			}
			const TSharedPtr<FJsonObject>* Parameters = nullptr;
			if (!Object->TryGetObjectField(TEXT("parameters"), Parameters)
				|| !Parameters || !Parameters->IsValid())
			{
				OutError = InputFailure(TEXT("operation_plan_step_shape_invalid"),
					TEXT("Step parameters must be an object."), Path + TEXT("/parameters"),
					Object->TryGetField(TEXT("parameters")), MakeShared<FJsonValueString>(TEXT("object")), Out.StepId);
				return false;
			}
			Out.Parameters = *Parameters;
			Out.ExpectedRevisionInput = Object->TryGetField(TEXT("expected_revision"));
			double ExpectedRevision = 0.0;
			if (Object->TryGetNumberField(TEXT("expected_revision"), ExpectedRevision))
			{
				Out.ExpectedRevision = static_cast<int64>(ExpectedRevision);
			}
			Out.StepId.TrimStartAndEndInline();
			Out.SubjectRef.TrimStartAndEndInline();
			if (Out.StepId.IsEmpty() || Out.SubjectRef.IsEmpty())
			{
				const TCHAR* Field = Out.StepId.IsEmpty() ? TEXT("step_id") : TEXT("subject_ref");
				OutError = InputFailure(TEXT("operation_plan_step_identity_invalid"),
					FString::Printf(TEXT("Step field '%s' must contain non-whitespace text."), Field),
					FieldPath(Path, Field), Object->TryGetField(Field),
					MakeShared<FJsonValueString>(TEXT("non-whitespace string")), Out.StepId);
				return false;
			}
			return true;
		}

		bool TryPreflightStep(
			const FPlanStep& Step,
			const FUeAgentImmutableCommandRegistry& Registry,
			FDiagnostic& OutError)
		{
			const auto RejectValue = [&](const TCHAR* Code, const FString& Reason,
				const TCHAR* Field, const FString& Actual, TSharedPtr<FJsonValue> Expected)
			{
				OutError = PlanFact(Code, Reason, FieldPath(Step.InputPath, Field),
					MakeShared<FJsonValueString>(Actual), MoveTemp(Expected), Step.StepId);
				return false;
			};
			if (Step.Kind == TEXT("public_command"))
			{
				const TCHAR* Code = TEXT("operation_plan_public_command_invalid");
				const FUeAgentFrozenCommandContract* Contract = Registry.FindExact(Step.SubjectRef);
				if (Step.Action != TEXT("execute"))
					return RejectValue(Code, TEXT("Public command steps require action 'execute'."),
						TEXT("action"), Step.Action, MakeShared<FJsonValueString>(TEXT("execute")));
				if (Step.PolicyId != TEXT("policy:public-command:exact/v1"))
					return RejectValue(Code, TEXT("The public command step uses an unsupported policy."),
						TEXT("policy_id"), Step.PolicyId, MakeShared<FJsonValueString>(TEXT("policy:public-command:exact/v1")));
				if (Step.SubjectRef == TEXT("operation.plan.execute"))
					return RejectValue(Code, TEXT("An operation plan cannot recursively execute another operation plan."),
						TEXT("subject_ref"), Step.SubjectRef, MakeShared<FJsonValueString>(TEXT("non-recursive public command")));
				if (!Contract || Contract->GetAvailability() != EUeAgentCommandAvailability::Enabled)
				{
					OutError = PlanFact(Code, Contract
						? TEXT("The public command is registered but is not enabled.")
						: TEXT("The public command is not registered."),
						Step.InputPath + TEXT("/subject_ref"), MakeShared<FJsonValueBoolean>(false),
						MakeShared<FJsonValueBoolean>(true), Step.StepId);
					OutError.TargetPath = Step.SubjectRef;
					return false;
				}
				return true;
			}
			if (Step.Kind == TEXT("editor_console"))
			{
				const TCHAR* Code = TEXT("operation_plan_editor_console_invalid");
				if (Step.Action != TEXT("execute"))
					return RejectValue(Code, TEXT("Editor console steps require action 'execute'."),
						TEXT("action"), Step.Action, MakeShared<FJsonValueString>(TEXT("execute")));
				if (Step.SubjectRef != TEXT("editor:current-world"))
					return RejectValue(Code, TEXT("Editor console steps require the current Editor world."),
						TEXT("subject_ref"), Step.SubjectRef, MakeShared<FJsonValueString>(TEXT("editor:current-world")));
				FString Command;
				if (!TryReadString(Step.Parameters, TEXT("command"), Step.InputPath + TEXT("/parameters"),
					Command, OutError, Code)) return false;
				if (Command.IsEmpty())
				{
					OutError = PlanFact(Code, TEXT("The Editor console command must not be empty."),
						Step.InputPath + TEXT("/parameters/command"), MakeShared<FJsonValueString>(Command),
						MakeShared<FJsonValueString>(TEXT("non-empty string")), Step.StepId);
					return false;
				}
				bool bAllowHighRisk = false;
				Step.Parameters->TryGetBoolField(TEXT("allow_high_risk"), bAllowHighRisk);
				if (IsHighRiskConsoleCommand(Command))
				{
					if (Step.PolicyId != TEXT("policy:editor-console:high-risk/v1"))
						return RejectValue(Code, TEXT("This console command requires the high-risk console policy."),
							TEXT("policy_id"), Step.PolicyId, MakeShared<FJsonValueString>(TEXT("policy:editor-console:high-risk/v1")));
					if (!bAllowHighRisk)
					{
						OutError = InputFailure(Code, TEXT("This console command requires allow_high_risk=true."),
							Step.InputPath + TEXT("/parameters/allow_high_risk"),
							Step.Parameters->TryGetField(TEXT("allow_high_risk")),
							MakeShared<FJsonValueBoolean>(true), Step.StepId);
						return false;
					}
				}
				return true;
			}
			if (Step.Kind == TEXT("transaction_history"))
			{
				const TCHAR* Code = TEXT("operation_plan_transaction_invalid");
				if (Step.PolicyId != TEXT("policy:transaction-history:standard/v1"))
					return RejectValue(Code, TEXT("The transaction-history step uses an unsupported policy."),
						TEXT("policy_id"), Step.PolicyId, MakeShared<FJsonValueString>(TEXT("policy:transaction-history:standard/v1")));
				if (Step.SubjectRef != TEXT("editor:transaction-history"))
					return RejectValue(Code, TEXT("Transaction steps require the Editor transaction history."),
						TEXT("subject_ref"), Step.SubjectRef, MakeShared<FJsonValueString>(TEXT("editor:transaction-history")));
				if (Step.Action != TEXT("status") && Step.Action != TEXT("undo") && Step.Action != TEXT("redo"))
					return RejectValue(Code, TEXT("Unknown transaction-history action."),
						TEXT("action"), Step.Action, Choices({TEXT("status"), TEXT("undo"), TEXT("redo")}));
				if (Step.Action != TEXT("status") && Step.ExpectedRevision == INDEX_NONE)
				{
					OutError = InputFailure(Code,
						TEXT("Undo and redo require a numeric expected_revision that does not resolve to the unavailable sentinel (-1)."),
						Step.InputPath + TEXT("/expected_revision"), Step.ExpectedRevisionInput,
						MakeShared<FJsonValueString>(TEXT("numeric transaction revision other than -1")), Step.StepId);
					return false;
				}
				return true;
			}
			return RejectValue(TEXT("operation_plan_provider_unknown"), TEXT("Unknown operation plan step kind."),
				TEXT("kind"), Step.Kind, Choices({TEXT("public_command"), TEXT("editor_console"), TEXT("transaction_history")}));
		}

		bool TryDecodeNestedRequest(
			const FUeAgentCommandRequest& Outer,
			const FPlanStep& Step,
			const FUeAgentImmutableCommandRegistry& Registry,
			TOptional<FUeAgentCommandRequest>& Out,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			const FUeAgentFrozenCommandContract* Contract =
				Registry.FindExact(Step.SubjectRef);
			if (!Contract)
			{
				OutError = PlanFact(TEXT("operation_plan_command_missing"),
					TEXT("The nested public command is no longer registered."), Step.InputPath + TEXT("/subject_ref"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Step.StepId);
				OutError.TargetPath = Step.SubjectRef;
				return false;
			}
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
				TEXT("command_binding_hash"), Contract->GetContractHash());
			Envelope->SetObjectField(TEXT("params"), Step.Parameters);
			TArray<uint8> Bytes;
			if (!Json::TryCanonicalUtf8(Envelope, Bytes, OutError))
			{
				return false;
			}
			FUeAgentCommandProtocolDiagnostic Diagnostic;
			if (!FUeAgentCommandProtocolCodec().DecodeRequest(
Bytes, Out, Diagnostic))
			{
				OutError = Diagnostic;
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
			UE::UAI::Contract::FDiagnostic Error;
			if (Json::TryParseObject(JsonText, Object, Error) && Object.IsValid())
			{
				Target->SetObjectField(Name, Object);
			}
			else
			{
				Target->SetStringField(Name, JsonText);
			}
		}

		TSharedRef<FJsonObject> ExecuteConsole(const FPlanStep& Step, FDiagnostic& OutError)
		{
			const TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			FString Command;
			bool bAddToHistory = false;
			Step.Parameters->TryGetStringField(TEXT("command"), Command);
			Step.Parameters->TryGetBoolField(TEXT("add_to_history"), bAddToHistory);
			FStringOutputDevice Output;
			UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
			const bool bHandled = World && IConsoleManager::Get()
				.ProcessUserConsoleInput(*Command, Output, World);
			if (bHandled && bAddToHistory)
			{
				IConsoleManager::Get().AddConsoleHistoryEntry(TEXT("UAI"), *Command);
			}
			if (!bHandled)
			{
				OutError = PlanFact(TEXT("operation_plan_console_unhandled"), World
					? TEXT("The Editor did not handle the console command.")
					: TEXT("The console command could not run because the Editor world is unavailable."),
					World ? TEXT("console.handled") : TEXT("editor.world.available"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true),
					Step.StepId, TEXT("native_result"));
				OutError.TargetPath = Step.SubjectRef;
				OutError.ProposedValue = MakeShared<FJsonValueString>(Command);
			}
			Result->SetBoolField(TEXT("ok"), bHandled);
			Result->SetStringField(TEXT("observation"),
				bHandled ? TEXT("console_executed") : TEXT("console_unhandled"));
			Result->SetStringField(TEXT("output"), Output);
			return Result;
		}

		TSharedRef<FJsonObject> ExecuteTransaction(const FPlanStep& Step, FDiagnostic& OutError)
		{
			const TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			UTransBuffer* Buffer = GEditor ? Cast<UTransBuffer>(GEditor->Trans) : nullptr;
			const int64 BeforeRevision = TransactionRevision();
			bool bOk = Buffer != nullptr;
			if (!Buffer)
			{
				OutError = PlanFact(TEXT("operation_plan_transaction_unavailable"),
					TEXT("The Editor transaction history is unavailable."), TEXT("transaction_history.available"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true),
					Step.StepId, TEXT("native_result"));
			}
			if (bOk && Step.Action != TEXT("status") && Step.ExpectedRevision != BeforeRevision)
			{
				bOk = false;
				OutError = PlanFact(TEXT("operation_plan_history_revision_mismatch"),
					TEXT("The transaction history changed after the plan was prepared."), TEXT("transaction_history.revision"),
					MakeShared<FJsonValueNumber>(BeforeRevision), MakeShared<FJsonValueNumber>(Step.ExpectedRevision),
					Step.StepId, TEXT("native_result"));
				Result->SetStringField(TEXT("observation"), TEXT("history_revision_mismatch"));
			}
			else if (bOk && (Step.Action == TEXT("undo") || Step.Action == TEXT("redo")))
			{
				const bool bUndo = Step.Action == TEXT("undo");
				const bool bCanApply = bUndo ? Buffer->CanUndo() : Buffer->CanRedo();
				bOk = bCanApply && (bUndo ? GEditor->UndoTransaction() : GEditor->RedoTransaction());
				if (!bOk)
				{
					OutError = PlanFact(TEXT("operation_plan_transaction_unavailable"), bCanApply
						? FString::Printf(TEXT("The Editor failed to apply transaction action '%s'."), *Step.Action)
						: FString::Printf(TEXT("No transaction is available for action '%s'."), *Step.Action),
						bCanApply ? TEXT("transaction_history.applied") : TEXT("transaction_history.action_available"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true),
						Step.StepId, TEXT("native_result"));
					OutError.ProposedValue = MakeShared<FJsonValueString>(Step.Action);
				}
			}
			if (!Result->HasField(TEXT("observation")))
			{
				Result->SetStringField(TEXT("observation"), bOk
					? (Step.Action == TEXT("status") ? TEXT("history_observed") : TEXT("transaction_applied"))
					: TEXT("transaction_unavailable"));
			}
			if (OutError.IsValid()) OutError.TargetPath = Step.SubjectRef;
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
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutError.Reset();
		if (!InDispatch.IsValid())
		{
			OutError = PlanFact(TEXT("operation_plan_nested_dispatch_identity_invalid"),
				TEXT("The nested exact dispatch is unavailable during operation-plan binding."),
				TEXT("nested_dispatch.available"), MakeShared<FJsonValueBoolean>(false),
				MakeShared<FJsonValueBoolean>(true), FString(), TEXT("native_result"));
			return false;
		}
		if (InRegistry->GetRegistryHash() != Registry->GetRegistryHash())
		{
			OutError = PlanFact(TEXT("operation_plan_nested_dispatch_identity_invalid"),
				TEXT("The nested dispatch registry does not match the operation-plan registry."),
				TEXT("nested_dispatch.registry_matches"), MakeShared<FJsonValueBoolean>(false),
				MakeShared<FJsonValueBoolean>(true), FString(), TEXT("native_result"));
			OutError.ActualEvidence = MakeShared<FJsonValueString>(InRegistry->GetRegistryHash());
			OutError.ExpectedEvidence = MakeShared<FJsonValueString>(Registry->GetRegistryHash());
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
				PlanFact(TEXT("operation_plan_game_thread_required"), TEXT("Editor providers require the game thread."),
					TEXT("execution.game_thread"), MakeShared<FJsonValueBoolean>(false),
					MakeShared<FJsonValueBoolean>(true), Request.GetCommandId(), TEXT("native_result")));
		}
		if (!Caller.IsValid() || !Caller.GetAuthorityScopes().Contains(TEXT("uai:operate")))
		{
			return FUeAgentCommandResult::Failure(Request, TEXT("{}"),
				PlanFact(TEXT("operation_plan_authority_required"), Caller.IsValid()
					? TEXT("The caller requires the uai:operate authority scope.")
					: TEXT("The operation plan requires a valid caller context."),
					Caller.IsValid() ? TEXT("caller.authority.uai:operate") : TEXT("caller.valid"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true),
					Request.GetCommandId(), TEXT("native_result")));
		}
		const auto FailInput = [&](FDiagnostic Error)
		{
			if (Error.CandidateName.IsEmpty()) Error.CandidateName = Request.GetCommandId();
			return FUeAgentCommandResult::Failure(Request, TEXT("{}"), MoveTemp(Error));
		};
		const TSharedPtr<FJsonObject>& Params = Request.GetParams();
		if (!Params.IsValid())
		{
			return FailInput(InputFailure(TEXT("operation_plan_input_invalid"),
				TEXT("Operation plan parameters must be an object."), FString(), nullptr,
				MakeShared<FJsonValueString>(TEXT("object"))));
		}
		const TArray<TSharedPtr<FJsonValue>>* StepValues = nullptr;
		if (!Params->TryGetArrayField(TEXT("steps"), StepValues) || !StepValues || StepValues->IsEmpty())
		{
			return FailInput(InputFailure(TEXT("operation_plan_input_invalid"),
				TEXT("The operation plan requires a non-empty steps array."), TEXT("/steps"),
				Params->TryGetField(TEXT("steps")), MakeShared<FJsonValueString>(TEXT("non-empty array"))));
		}
		FString OnError;
		FString Atomicity;
		FDiagnostic InputError;
		if (!TryReadString(Params, TEXT("on_error"), FString(), OnError, InputError, TEXT("operation_plan_input_invalid"))
			|| !TryReadString(Params, TEXT("atomicity"), FString(), Atomicity, InputError, TEXT("operation_plan_input_invalid")))
		{
			return FailInput(MoveTemp(InputError));
		}
		if (OnError != TEXT("stop") && OnError != TEXT("continue"))
		{
			return FailInput(PlanFact(TEXT("operation_plan_input_invalid"), TEXT("Unknown on_error policy."),
				TEXT("/on_error"), MakeShared<FJsonValueString>(OnError), Choices({TEXT("stop"), TEXT("continue")})));
		}
		if (Atomicity != TEXT("reject_non_atomic") && Atomicity != TEXT("best_effort"))
		{
			return FailInput(PlanFact(TEXT("operation_plan_input_invalid"), TEXT("Unknown atomicity policy."),
				TEXT("/atomicity"), MakeShared<FJsonValueString>(Atomicity),
				Choices({TEXT("reject_non_atomic"), TEXT("best_effort")})));
		}

		TArray<FPlanStep> Steps;
		TSet<FString> StepIds;
		for (int32 Index = 0; Index < StepValues->Num(); ++Index)
		{
			FPlanStep Step;
			FDiagnostic PreflightError;
			const FString StepPath = FString::Printf(TEXT("/steps/%d"), Index);
			if (!TryParseStep((*StepValues)[Index], StepPath, Step, PreflightError))
			{
				PreflightError.OperationIndex = Index;
				if (PreflightError.CandidateName.IsEmpty()) PreflightError.CandidateName = Step.StepId;
				return FailInput(MoveTemp(PreflightError));
			}
			if (StepIds.Contains(Step.StepId))
			{
				FDiagnostic Error = PlanFact(TEXT("operation_plan_step_id_duplicate"),
					TEXT("The operation plan contains a duplicate step_id."), StepPath + TEXT("/step_id"),
					MakeShared<FJsonValueString>(Step.StepId), MakeShared<FJsonValueString>(TEXT("unique step_id")), Step.StepId);
				Error.OperationIndex = Index;
				return FailInput(MoveTemp(Error));
			}
			if (!TryPreflightStep(Step, *Registry, PreflightError))
			{
				PreflightError.OperationIndex = Index;
				if (PreflightError.CandidateName.IsEmpty()) PreflightError.CandidateName = Step.StepId;
				return FailInput(MoveTemp(PreflightError));
			}
			StepIds.Add(Step.StepId);
			Steps.Add(MoveTemp(Step));
		}
		if (Atomicity == TEXT("reject_non_atomic")
			&& (Steps.Num() != 1 || Steps[0].Kind != TEXT("transaction_history")))
		{
			const TSharedRef<FJsonObject> Actual = MakeShared<FJsonObject>();
			Actual->SetNumberField(TEXT("step_count"), Steps.Num());
			if (Steps.Num() == 1) Actual->SetStringField(TEXT("kind"), Steps[0].Kind);
			return FailInput(PlanFact(TEXT("operation_plan_non_atomic_rejected"),
				TEXT("Atomic execution requires exactly one transaction-history step."), TEXT("/steps"),
				MakeShared<FJsonValueObject>(Actual), MakeShared<FJsonValueString>(TEXT("one transaction_history step"))));
		}

		const TSharedPtr<const FUeAgentExactCommandDispatch> ExactDispatch = Dispatch.Pin();
		if (!ExactDispatch.IsValid())
		{
			return FUeAgentCommandResult::Failure(Request, TEXT("{}"),
				PlanFact(TEXT("operation_plan_nested_dispatch_unavailable"), TEXT("The process exact dispatch is unavailable."),
					TEXT("nested_dispatch.available"), MakeShared<FJsonValueBoolean>(false),
					MakeShared<FJsonValueBoolean>(true), Request.GetCommandId(), TEXT("native_result")));
		}

		TArray<TSharedPtr<FJsonValue>> Results;
		bool bPlanOk = true;
        auto PlanFailure = UE::UAI::Contract::FDiagnostic::Failure(TEXT("operation_plan_step_failed"), TEXT("One or more operation plan steps failed."), TEXT("operation_plan"), FString(), Request.GetCommandId());
		for (int32 StepIndex = 0; StepIndex < Steps.Num(); ++StepIndex)
		{
			const FPlanStep& Step = Steps[StepIndex];
			if (Cancellation->IsCancelled())
			{
				bPlanOk = false;
				PlanFailure.Diagnostics.Add(PlanFact(TEXT("operation_plan_cancelled"),
					TEXT("The operation plan was cancelled before this step."), Step.InputPath,
					MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false),
					Step.StepId, TEXT("native_result")));
				break;
			}
			TSharedRef<FJsonObject> StepResult = MakeShared<FJsonObject>();
            UE::UAI::Contract::FDiagnostic StepError;
			if (Step.Kind == TEXT("public_command"))
			{
				TOptional<FUeAgentCommandRequest> NestedRequest;
				UE::UAI::Contract::FDiagnostic DecodeError;
				if (!TryDecodeNestedRequest(
					Request, Step, *Registry, NestedRequest, DecodeError))
				{
					StepResult->SetBoolField(TEXT("ok"), false);
					StepResult->SetStringField(
						TEXT("observation"), TEXT("nested_request_invalid"));
					StepError = DecodeError;
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
						StepError = NestedResult.GetDiagnostic();
					}
				}
			}
			else if (Step.Kind == TEXT("editor_console"))
			{
				StepResult = ExecuteConsole(Step, StepError);
			}
			else
			{
				StepResult = ExecuteTransaction(Step, StepError);
			}
			if (StepError.IsValid())
            {
                if (StepError.SubjectId.IsEmpty()) StepError.SubjectId = Step.StepId;
                if (StepError.CandidateName.IsEmpty()) StepError.CandidateName = Step.StepId;
                if (!StepError.OperationIndex.IsSet()) StepError.OperationIndex = StepIndex;
                PlanFailure.Diagnostics.Add(StepError);
                StepResult->SetObjectField(TEXT("error"), StepError.ToPublicJson());
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
		UE::UAI::Contract::FDiagnostic JsonError;
		if (!Json::TryCanonicalize(Payload, PayloadJson, JsonError))
		{
			return FUeAgentCommandResult::Failure(Request, TEXT("{}"),
				MoveTemp(JsonError));
		}
		return bPlanOk
			? FUeAgentCommandResult::Success(Request, MoveTemp(PayloadJson))
			: FUeAgentCommandResult::Failure(Request, MoveTemp(PayloadJson),
				MoveTemp(PlanFailure));
	}
}

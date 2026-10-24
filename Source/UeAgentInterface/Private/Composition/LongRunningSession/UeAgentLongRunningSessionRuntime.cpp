#include "Composition/LongRunningSession/UeAgentLongRunningSessionRuntime.h"

#include "Core/Contract/UeAgentDiagnostic.h"

#include "Composition/LongRunningSession/UeAgentLongRunningSessionActivation.h"
#include "Core/ContextResource/UeAgentContextResource.h"
#include "Core/ContextResource/UeAgentOperationContext.h"
#include "Core/Contract/UeAgentContractJson.h"
#include "Core/Contract/UeAgentContractSchema.h"
#include "Core/ExecutionMutation/UeAgentLongRunningOperation.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/PlatformTime.h"
#include "HAL/CriticalSection.h"
#include "Misc/ScopeLock.h"
#include "Misc/ScopeExit.h"
#include "Async/Async.h"
#include "Templates/Atomic.h"

namespace UE::UAI::Composition::LongRunningSession
{
	using namespace ContextResource;
	using namespace Contract;
	using namespace DomainAdapters;
	using namespace ExecutionMutation;
	using namespace StateSemantic;

	namespace
	{
		FDiagnostic RuntimeFailure(const FString& Code, const FString& SubjectId, const FString& Reason,
			const FDiagnostic& Cause = {})
		{
			FDiagnostic Diagnostic = FDiagnostic::Failure(Code, Reason, TEXT("runtime"), FString(), SubjectId);
			Diagnostic.Schema = TEXT("uai.runtime.diagnostic/1");
			if (!Cause.IsEmpty()) Diagnostic.PublicCause = MakeShared<FDiagnostic>(Cause);
			return Diagnostic;
		}

		FDiagnostic ValueFailure(const FString& Code, const FString& Reason, const FString& Path,
			TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected,
			const FString& SourceKind = TEXT("native_result"))
		{
			FDiagnostic Error = RuntimeFailure(Code, FString(), Reason);
			Error.SourceKind = SourceKind;
			Error.PropertyPath = Path;
			if (SourceKind == TEXT("command_input")) Error.Source.JsonPath = Path;
			Error.ActualValue = MoveTemp(Actual);
			Error.ExpectedValue = MoveTemp(Expected);
			return Error;
		}

		FDiagnostic InputValueFailure(const FString& Code, const FString& Reason, const FString& Path,
			const TSharedPtr<FJsonValue>& Value, const FString& Expected, const bool bInternal = false,
			const FString& SourceKind = TEXT("command_input"))
		{
			TSharedPtr<FJsonValue> Actual = Value;
			if (Value.IsValid() && (Value->Type == EJson::Array || Value->Type == EJson::Object))
			{
				const auto Summary = MakeShared<FJsonObject>();
				Summary->SetStringField(TEXT("type"), Value->Type == EJson::Array ? TEXT("array") : TEXT("object"));
				Summary->SetNumberField(TEXT("count"), Value->Type == EJson::Array ? Value->AsArray().Num()
					: Value->AsObject().IsValid() ? Value->AsObject()->Values.Num() : 0);
				Actual = MakeShared<FJsonValueObject>(Summary);
			}
			FDiagnostic Error = ValueFailure(Code, Reason, Path, bInternal ? nullptr : Actual,
				MakeShared<FJsonValueString>(Expected), SourceKind);
			Error.ActualValuePresent = Value.IsValid();
			Error.ExpectedValuePresent = true;
			if (bInternal) Error.ActualEvidence = Actual;
			return Error;
		}

		FDiagnostic UnboundPort(const FString& Action)
		{
			FDiagnostic Error = ValueFailure(TEXT("long_running_session_operation_port_not_bound"),
				TEXT("The long-running operation port is not bound; the requested action cannot run."),
				TEXT("/operation_port"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			Error.SubjectId = Action;
			return Error;
		}

		FString PathKey(FString Key)
		{
			return Key.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"));
		}

		TSharedPtr<FJsonValue> DescriptorValue(const FUeAgentDescriptorIdentity& Identity)
		{
			const auto Value = MakeShared<FJsonObject>();
			Value->SetStringField(TEXT("descriptor_id"), Identity.GetDescriptorId());
			Value->SetNumberField(TEXT("version"), Identity.GetVersion());
			return MakeShared<FJsonValueObject>(Value);
		}

		FDiagnostic MissingDescriptor(const FString& Code, const FString& Reason,
			const FUeAgentDescriptorIdentity& Identity)
		{
			FDiagnostic Error = ValueFailure(Code, Reason, TEXT("/descriptor_identity"), DescriptorValue(Identity),
				MakeShared<FJsonValueString>(TEXT("registered exact descriptor with compiled input schema")));
			Error.SubjectId = Identity.GetDescriptorId();
			Error.ActualEvidence = MakeShared<FJsonValueString>(Identity.GetDescriptorHash());
			return Error;
		}

		FUeAgentOperationResult Failure(const FUeAgentOperationRequest& Request,
			FDiagnostic Diagnostic, const FString& SubjectId = FString())
		{
			if (Diagnostic.SubjectId.IsEmpty()) Diagnostic.SubjectId = SubjectId;
			return FUeAgentOperationResult(Request.GetDescriptorIdentity(), Request.GetRequestId(), MoveTemp(Diagnostic));
		}


		class FRejectingDefaultPort final : public ILongRunningOperationPort
		{
		public:
			bool Start(
				const FLongRunningOperationRequest& Request,
				const FUeAgentCancellationToken& Cancellation,
				FLongRunningWorkIdentity& OutIdentity,
				FExecutionMutationError& OutError) override
			{
				OutError = UnboundPort(TEXT("start"));
				return false;
			}

			bool Observe(
				const FString& OperationId,
				const FLongRunningWorkIdentity& Identity,
				FUeAgentResourceTracker& Resources,
				FLongRunningWorkObservation& OutObservation,
				FExecutionMutationError& OutError) override
			{
				OutError = UnboundPort(TEXT("observe"));
				return false;
			}

			bool RequestStop(
				const FString& OperationId,
				const FLongRunningWorkIdentity& Identity,
				const FString& Reason,
				FExecutionMutationError& OutError) override
			{
				OutError = UnboundPort(TEXT("request_stop"));
				return false;
			}

			bool Join(
				const FString& OperationId,
				const FLongRunningWorkIdentity& Identity,
				FExecutionMutationError& OutError) override
			{
				OutError = UnboundPort(TEXT("join"));
				return false;
			}

			bool Release(
				const FString& OperationId,
				const FLongRunningWorkIdentity& Identity,
				FExecutionMutationError& OutError) override
			{
				OutError = UnboundPort(TEXT("release"));
				return false;
			}
		};

		struct FOperationRef final
		{
			FString OperationId;
			FString OwnerLeaseId;
			FString ServerSessionId;

			bool IsValid() const
			{
				return !OperationId.IsEmpty()
					&& !OwnerLeaseId.IsEmpty()
					&& !ServerSessionId.IsEmpty();
			}
		};

		TSharedRef<FJsonObject> OperationRefObject(const FOperationRef& Ref)
		{
			const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
			Object->SetStringField(TEXT("operation_id"), Ref.OperationId);
			Object->SetStringField(TEXT("owner_lease_id"), Ref.OwnerLeaseId);
			Object->SetStringField(TEXT("server_session_id"), Ref.ServerSessionId);
			return Object;
		}

		FOperationRef OperationRefFromSnapshot(
			const FLongRunningOperationSnapshot& Snapshot)
		{
			return {
				Snapshot.GetOperationId(),
				Snapshot.GetOwnerLeaseId(),
				Snapshot.GetServerSessionId(),
			};
		}

		FString LifecycleStatus(
			const FLongRunningOperationSnapshot& Snapshot)
		{
			switch (Snapshot.GetStatus())
			{
			case ELongRunningOperationStatus::Succeeded:
				return TEXT("native_exited_success");
			case ELongRunningOperationStatus::Cancelled:
				return TEXT("cancelled");
			case ELongRunningOperationStatus::Failed:
				return TEXT("failed");
			case ELongRunningOperationStatus::StateUnknown:
				return TEXT("state_unknown");
			case ELongRunningOperationStatus::None:
				return LexToString(Snapshot.GetPhase());
			default:
				return TEXT("state_unknown");
			}
		}

		FString ReceiptStatus(const FLongRunningOperationReceipt& Receipt)
		{
			switch (Receipt.GetStatus())
			{
			case ELongRunningOperationStatus::Succeeded:
				return TEXT("native_exited_success");
			case ELongRunningOperationStatus::Cancelled:
				return TEXT("cancelled");
			case ELongRunningOperationStatus::Failed:
				return TEXT("failed");
			case ELongRunningOperationStatus::StateUnknown:
				return TEXT("state_unknown");
			default:
				return TEXT("state_unknown");
			}
		}

		FUeAgentOperationResult ResultObject(const FUeAgentOperationRequest& Request, const TSharedRef<FJsonObject>& Object,
			const FDiagnostic& PayloadDiagnostic = {})
		{
			FString Payload;
			FDiagnostic Error;
			if (!Json::TryCanonicalize(Object, Payload, Error))
			{
				Error.Append(PayloadDiagnostic);
				return Failure(Request, MoveTemp(Error));
			}
			return FUeAgentOperationResult(Request.GetDescriptorIdentity(), Request.GetRequestId(), FUeAgentContractValue(MoveTemp(Payload)));
		}

		void SetExactIntegerField(
			const TSharedRef<FJsonObject>& Object,
			const TCHAR* Field,
			const int64 Value)
		{
			Object->SetField(
				Field,
				MakeShared<FJsonValueNumberString>(FString::Printf(
					TEXT("%lld"), static_cast<long long>(Value))));
		}

		bool TryAddLatestSignal(const FLongRunningOperationSnapshot& Snapshot,
			const TSharedRef<FJsonObject>& Root, FDiagnostic& OutError)
		{
			if (!Snapshot.GetLatestSignal().IsSet()) return true;
			const FLongRunningOperationSignal& Signal = Snapshot.GetLatestSignal().GetValue();
			TSharedPtr<FJsonValue> Payload;
			if (!StateSemantic::CanonicalJson::TryToJsonValue(*Signal.GetPayload(), Payload, OutError))
				return false;
			const TSharedRef<FJsonObject> LatestSignal = MakeShared<FJsonObject>();
			LatestSignal->SetStringField(TEXT("type_id"), Signal.GetTypeId());
			SetExactIntegerField(LatestSignal, TEXT("revision"), Signal.GetRevision());
			LatestSignal->SetField(TEXT("payload"), MoveTemp(Payload));
			Root->SetObjectField(TEXT("latest_signal"), LatestSignal);
			return true;
		}

		TSharedRef<FJsonObject> SnapshotObject(
			const FLongRunningOperationSnapshot& Snapshot, FDiagnostic& OutError)
		{
			const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
			Root->SetStringField(TEXT("schema"), TEXT("uai.long_running.snapshot.v1"));
			Root->SetObjectField(
				TEXT("operation_ref"),
				OperationRefObject(OperationRefFromSnapshot(Snapshot)));
			Root->SetStringField(
				TEXT("descriptor_id"),
				Snapshot.GetDescriptorIdentity().GetDescriptorId());
			Root->SetStringField(
				TEXT("binding_hash"),
				Snapshot.GetDescriptorIdentity().GetDescriptorHash());
			Root->SetStringField(TEXT("phase"), LexToString(Snapshot.GetPhase()));
			SetExactIntegerField(
				Root, TEXT("generation"), Snapshot.GetGeneration());
			Root->SetStringField(
				TEXT("lifecycle_status"), LifecycleStatus(Snapshot));
			Root->SetBoolField(
				TEXT("stop_requested"), Snapshot.IsStopRequested());
			Root->SetBoolField(TEXT("terminal"), Snapshot.IsTerminal());
			Root->SetBoolField(TEXT("cleanup_complete"), Snapshot.IsCleanupComplete());
			if (!Snapshot.GetProgressDiagnostic().IsEmpty())
				Root->SetObjectField(TEXT("progress_diagnostic"), Snapshot.GetProgressDiagnostic().ToPublicJson());
			Root->SetBoolField(TEXT("retry_safe"), false);
			if (!Snapshot.GetErrorCode().IsEmpty())
			{
				Root->SetStringField(TEXT("error_code"), Snapshot.GetErrorCode());
				Root->SetObjectField(TEXT("diagnostic"), Snapshot.GetDiagnostic().ToPublicJson());
			}
			TryAddLatestSignal(Snapshot, Root, OutError);
			return Root;
		}

		FUeAgentOperationResult SnapshotResult(
			const FUeAgentOperationRequest& Request,
			const FLongRunningOperationSnapshot& Snapshot)
		{
			FDiagnostic PayloadError;
			const auto Root = SnapshotObject(Snapshot, PayloadError);
			if (!PayloadError.IsEmpty())
				return Failure(Request, RuntimeFailure(TEXT("long_running_session_signal_encoding_failed"), Snapshot.GetOperationId(), TEXT("The latest operation signal could not be encoded."), PayloadError));
			return ResultObject(Request, Root, Snapshot.GetDiagnostic());
		}

		FUeAgentOperationResult UnknownReconcileResult(
			const FUeAgentOperationRequest& Request,
			const FOperationRef& Ref,
			const int64 ExpectedGeneration)
		{
			const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
			Root->SetStringField(TEXT("schema"), TEXT("uai.long_running.snapshot.v1"));
			Root->SetObjectField(TEXT("operation_ref"), OperationRefObject(Ref));
			Root->SetStringField(
				TEXT("descriptor_id"),
				Request.GetDescriptorIdentity().GetDescriptorId());
			Root->SetStringField(
				TEXT("binding_hash"),
				Request.GetDescriptorIdentity().GetDescriptorHash());
			Root->SetStringField(TEXT("phase"), TEXT("terminal"));
			SetExactIntegerField(Root, TEXT("generation"), ExpectedGeneration);
			Root->SetStringField(TEXT("lifecycle_status"), TEXT("state_unknown"));
			Root->SetStringField(
				TEXT("error_code"), TEXT("operation_reconcile_evidence_missing"));
			Root->SetBoolField(TEXT("stop_requested"), false);
			Root->SetBoolField(TEXT("terminal"), true);
			Root->SetBoolField(TEXT("cleanup_complete"), false);
			Root->SetBoolField(TEXT("retry_safe"), false);
			return ResultObject(Request, Root);
		}

		FUeAgentOperationResult ReceiptResult(
			const FUeAgentOperationRequest& Request,
			const FOperationRef& Ref,
			const FLongRunningOperationReceipt& Receipt,
			const FString& OverrideStatus = FString(),
			const FLongRunningOperationSnapshot* Snapshot = nullptr)
		{
			const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
			Root->SetStringField(TEXT("schema"), TEXT("uai.long_running.receipt.v1"));
			Root->SetObjectField(TEXT("operation_ref"), OperationRefObject(Ref));
			SetExactIntegerField(
				Root, TEXT("generation"), Receipt.GetGeneration());
			Root->SetStringField(
				TEXT("lifecycle_status"),
				OverrideStatus.IsEmpty()
					? ReceiptStatus(Receipt)
					: OverrideStatus);
			Root->SetStringField(TEXT("error_code"), Receipt.GetErrorCode());
			if (!Receipt.GetDiagnostic().IsEmpty()) Root->SetObjectField(TEXT("diagnostic"), Receipt.GetDiagnostic().ToPublicJson());
			Root->SetBoolField(TEXT("retry_safe"), Receipt.IsRetrySafe());
			Root->SetBoolField(TEXT("cleanup_complete"), Snapshot && Snapshot->IsCleanupComplete());
			FDiagnostic PayloadError;
			if (Snapshot && !TryAddLatestSignal(*Snapshot, Root, PayloadError))
			{
				// The receipt owns the terminal diagnostic; the earlier snapshot only supplies the signal.
				PayloadError.Append(Receipt.GetDiagnostic());
				return Failure(Request, RuntimeFailure(TEXT("long_running_session_signal_encoding_failed"), Ref.OperationId, TEXT("The latest operation signal could not be encoded."), PayloadError));
			}
			return ResultObject(Request, Root, Receipt.GetDiagnostic());
		}

		bool TryReadOperationRef(
			const TSharedPtr<FJsonObject>& Root,
			FOperationRef& OutRef, FDiagnostic& OutError)
		{
			OutRef = {};
			const TSharedPtr<FJsonObject>* Ref = nullptr;
			if (!Root.IsValid() || !Root->TryGetObjectField(TEXT("operation_ref"), Ref) || !Ref || !Ref->IsValid())
			{
				OutError = InputValueFailure(TEXT("long_running_session_operation_ref_invalid"),
					TEXT("operation_ref must be an object containing operation, lease and server session identifiers."),
					TEXT("/operation_ref"), Root.IsValid() ? Root->TryGetField(TEXT("operation_ref")) : nullptr, TEXT("object"), true);
				return false;
			}
			auto Read = [&](const TCHAR* Field, FString& Value)
			{
				if ((*Ref)->TryGetStringField(Field, Value)) return true;
				OutError = InputValueFailure(TEXT("long_running_session_operation_ref_invalid"),
					TEXT("The operation reference identifier must be a string."),
					FString(TEXT("/operation_ref/")) + Field, (*Ref)->TryGetField(Field), TEXT("string"), true);
				return false;
			};
			if (!Read(TEXT("operation_id"), OutRef.OperationId)
				|| !Read(TEXT("owner_lease_id"), OutRef.OwnerLeaseId)
				|| !Read(TEXT("server_session_id"), OutRef.ServerSessionId)) return false;
			if (!OutRef.IsValid())
			{
				const TCHAR* Field = OutRef.OperationId.IsEmpty() ? TEXT("operation_id")
					: OutRef.OwnerLeaseId.IsEmpty() ? TEXT("owner_lease_id") : TEXT("server_session_id");
				OutError = InputValueFailure(TEXT("long_running_session_operation_ref_invalid"),
					TEXT("The operation reference identifier must not be empty."),
					FString(TEXT("/operation_ref/")) + Field, (*Ref)->TryGetField(Field), TEXT("non-empty string"), true);
				return false;
			}
			return true;
		}

		bool TryReadInteger(
			const TSharedPtr<FJsonObject>& Root,
			const TCHAR* Field,
			int64& OutValue, FDiagnostic& OutError)
		{
			OutValue = 0;
			const TSharedPtr<FJsonValue> Value =
				Root.IsValid() ? Root->TryGetField(Field) : nullptr;
			FString Lexeme;
			if (Value.IsValid()
				&& Value->Type == EJson::Number
				&& Value->TryGetString(Lexeme)
				&& LexTryParseString(OutValue, *Lexeme)) return true;
			OutError = InputValueFailure(TEXT("operation_generation_invalid"),
				TEXT("The generation must be a representable 64-bit integer."), FString(TEXT("/")) + Field, Value, TEXT("int64 JSON number"));
			return false;
		}

		bool TryCanonicalStateValue(
			const TSharedPtr<FJsonValue>& RootValue,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FExecutionMutationError& OutError, const FString& RootPath = FString(),
			const FString& SourceKind = TEXT("native_result"))
		{
			OutValue.Reset();
			OutError = {};
			if (!RootValue.IsValid())
			{
				OutError = InputValueFailure(TEXT("long_running_session_canonical_value_missing"),
					TEXT("The value to canonicalize is missing."), RootPath, RootValue, TEXT("JSON value"), false, SourceKind);
				return false;
			}

			struct FWork final
			{
				TSharedPtr<FJsonValue> Value;
				bool bExpanded = false;
				FString Path;
			};
			TArray<FWork> Work;
			Work.Add({RootValue, false, RootPath});
			TMap<const FJsonValue*, FUeAgentCanonicalValuePtr> Built;
			while (!Work.IsEmpty())
			{
				const FWork Current = Work.Pop(EAllowShrinking::No);
				if (!Current.Value.IsValid())
				{
					OutError = InputValueFailure(TEXT("long_running_session_canonical_value_invalid"),
						TEXT("A nested JSON value is missing."), Current.Path, Current.Value, TEXT("JSON value"), false, SourceKind);
					return false;
				}
				const bool bContainer = Current.Value->Type == EJson::Array
					|| Current.Value->Type == EJson::Object;
				if (bContainer && !Current.bExpanded)
				{
					Work.Add({Current.Value, true, Current.Path});
					if (Current.Value->Type == EJson::Array)
					{
						const TArray<TSharedPtr<FJsonValue>>& Items =
							Current.Value->AsArray();
						for (int32 Index = Items.Num() - 1; Index >= 0; --Index)
						{
							Work.Add({Items[Index], false, Current.Path + TEXT("/") + FString::FromInt(Index)});
						}
					}
					else
					{
						TArray<FString> Keys;
						Current.Value->AsObject()->Values.GetKeys(Keys);
						Keys.Sort();
						for (int32 Index = Keys.Num() - 1; Index >= 0; --Index)
						{
							Work.Add({
								Current.Value->AsObject()->Values.FindRef(Keys[Index]),
								false, Current.Path + TEXT("/") + PathKey(Keys[Index])});
						}
					}
					continue;
				}

				FUeAgentCanonicalValuePtr Converted;
				FUeAgentStateSemanticError StateError;
				bool bConverted = false;
				switch (Current.Value->Type)
				{
				case EJson::Null:
					bConverted = FUeAgentCanonicalValue::TryNull(
						Resources, Converted, StateError);
					break;
				case EJson::Boolean:
					bConverted = FUeAgentCanonicalValue::TryBoolean(
						Current.Value->AsBool(), Resources, Converted, StateError);
					break;
				case EJson::String:
					bConverted = FUeAgentCanonicalValue::TryString(
						Current.Value->AsString(), Resources, Converted, StateError);
					break;
				case EJson::Number:
				{
					FString Lexeme;
					Current.Value->TryGetString(Lexeme);
					const bool bInteger = !Lexeme.Contains(TEXT("."))
						&& !Lexeme.Contains(TEXT("e"))
						&& !Lexeme.Contains(TEXT("E"));
					bConverted = bInteger
						? FUeAgentCanonicalValue::TryInteger(
							Lexeme, Resources, Converted, StateError)
						: FUeAgentCanonicalValue::TryDecimal(
							Lexeme, Resources, Converted, StateError);
					break;
				}
				case EJson::Array:
				{
					TArray<FUeAgentCanonicalValueRef> Items;
					const TArray<TSharedPtr<FJsonValue>>& Values = Current.Value->AsArray();
					for (int32 Index = 0; Index < Values.Num(); ++Index)
					{
						const TSharedPtr<FJsonValue>& Item = Values[Index];
						const FUeAgentCanonicalValuePtr* Found = Built.Find(Item.Get());
						if (!Found || !Found->IsValid())
						{
							OutError = ValueFailure(TEXT("long_running_session_canonical_child_missing"),
								TEXT("The canonical array element was not produced."),
								Current.Path + TEXT("/") + FString::FromInt(Index),
								MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), SourceKind);
							return false;
						}
						Items.Add(Found->ToSharedRef());
					}
					bConverted = FUeAgentCanonicalValue::TryArray(
						Items, Resources, Converted, StateError);
					break;
				}
				case EJson::Object:
				{
					TArray<FString> Keys;
					Current.Value->AsObject()->Values.GetKeys(Keys);
					Keys.Sort();
					TArray<TPair<FString, FUeAgentCanonicalValueRef>> Items;
					for (const FString& Key : Keys)
					{
						const TSharedPtr<FJsonValue> Child =
							Current.Value->AsObject()->Values.FindRef(Key);
						const FUeAgentCanonicalValuePtr* Found = Built.Find(Child.Get());
						if (!Found || !Found->IsValid())
						{
							OutError = ValueFailure(TEXT("long_running_session_canonical_child_missing"),
								TEXT("The canonical object member was not produced."),
								Current.Path + TEXT("/") + PathKey(Key),
								MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), SourceKind);
							return false;
						}
						Items.Emplace(Key, Found->ToSharedRef());
					}
					bConverted = FUeAgentCanonicalValue::TryObject(
						Items, Resources, Converted, StateError);
					break;
				}
				default:
					break;
				}
				if (!bConverted)
				{
					OutError = StateError.IsEmpty() ? ValueFailure(TEXT("long_running_session_canonical_value_failed"),
						TEXT("The JSON value kind cannot be converted to a canonical value."), Current.Path,
						MakeShared<FJsonValueNumber>(static_cast<int32>(Current.Value->Type)),
						MakeShared<FJsonValueString>(TEXT("null, boolean, string, number, array or object")), SourceKind) : MoveTemp(StateError);
					if (OutError.Source.JsonPath.IsEmpty()) OutError.Source.JsonPath = Current.Path;
					return false;
				}
				Built.Add(Current.Value.Get(), Converted);
			}
			const FUeAgentCanonicalValuePtr* Found = Built.Find(RootValue.Get());
			if (!Found || !Found->IsValid())
			{
				OutError = ValueFailure(TEXT("long_running_session_canonical_root_missing"),
					TEXT("The canonical root value was not produced."), RootPath,
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), SourceKind);
				return false;
			}
			OutValue = *Found;
			return true;
		}

		bool TryCompileFamily(
			const FString& FamilyId,
			CompileBuildJob::EUeAgentCompileBuildJobFamily& OutFamily)
		{
			for (const CompileBuildJob::FUeAgentCompileBuildJobFamilyContract& Contract
				: CompileBuildJob::GetCompileBuildJobFamilyContracts())
			{
				if (Contract.GetFamilyId() == FamilyId)
				{
					OutFamily = Contract.GetFamily();
					return true;
				}
			}
			return false;
		}

		bool TryEditorFamily(
			const FString& FamilyId,
			EditorSessionOperation::EUeAgentEditorSessionOperationFamily& OutFamily)
		{
			for (const EditorSessionOperation::
				FUeAgentEditorSessionOperationFamilyContract& Contract
				: EditorSessionOperation::GetEditorSessionOperationFamilyContracts())
			{
				if (Contract.GetFamilyId() == FamilyId)
				{
					OutFamily = Contract.GetFamily();
					return true;
				}
			}
			return false;
		}

		CaptureMediaLongJob::EUeAgentCaptureMediaLongJobFamily CaptureFamily(
			const FString& CommandId)
		{
			if (CommandId == TEXT("capture.image.execute"))
			{
				return CaptureMediaLongJob::EUeAgentCaptureMediaLongJobFamily::
					ImageCapture;
			}
			if (CommandId == TEXT("capture.media.execute"))
			{
				return CaptureMediaLongJob::EUeAgentCaptureMediaLongJobFamily::
					MediaRecording;
			}
			return CaptureMediaLongJob::EUeAgentCaptureMediaLongJobFamily::
				RenderResource;
		}
	}

	class FUeAgentLongRunningSessionRuntime::FState final
	{
	public:
		FState(
			FUeAgentLongRunningSessionRuntimeConfig InConfig,
			TSharedRef<const FUeAgentDescriptorRegistry> InDescriptors,
			const TArray<FUeAgentLongRunningSessionActivationProfile>& ActivationProfiles,
			TSharedRef<CompileBuildJob::IUeAgentCompileBuildJobPortFactory,
				ESPMode::ThreadSafe> InCompileBuildFactory,
			TSharedRef<EditorSessionOperation::
				IUeAgentEditorSessionOperationPortFactory,
				ESPMode::ThreadSafe> InEditorSessionFactory,
			TSharedRef<CaptureMediaLongJob::
				IUeAgentCaptureMediaLongJobPortFactory,
				ESPMode::ThreadSafe> InCaptureMediaFactory,
			TSharedRef<PieRuntimeDebug::
				IUeAgentPieRuntimeScenarioPortFactory,
				ESPMode::ThreadSafe> InPieRuntimeScenarioFactory)
			: Config(MoveTemp(InConfig))
			, Descriptors(MoveTemp(InDescriptors))
			, CompileBuildFactory(MoveTemp(InCompileBuildFactory))
			, EditorSessionFactory(MoveTemp(InEditorSessionFactory))
			, CaptureMediaFactory(MoveTemp(InCaptureMediaFactory))
			, PieRuntimeScenarioFactory(MoveTemp(InPieRuntimeScenarioFactory))
			, Owner(
				MakeShared<FRejectingDefaultPort, ESPMode::ThreadSafe>(),
				[]() { return FPlatformTime::Seconds(); })
		{
			for (const FUeAgentLongRunningSessionActivationProfile& Profile
				: ActivationProfiles)
			{
				Profiles.Add(
					Profile.DescriptorIdentity.GetDescriptorId(), Profile);
			}
		}

		const FUeAgentLongRunningSessionActivationProfile* FindProfile(
			const FUeAgentDescriptorIdentity& Identity) const
		{
			const FUeAgentLongRunningSessionActivationProfile* Profile =
				Profiles.Find(Identity.GetDescriptorId());
			return Profile && Profile->DescriptorIdentity == Identity
				? Profile
				: nullptr;
		}

		FUeAgentLongRunningSessionRuntimeConfig Config;
		TSharedRef<const FUeAgentDescriptorRegistry> Descriptors;
		TSharedRef<CompileBuildJob::IUeAgentCompileBuildJobPortFactory,
			ESPMode::ThreadSafe> CompileBuildFactory;
		TSharedRef<EditorSessionOperation::
			IUeAgentEditorSessionOperationPortFactory,
			ESPMode::ThreadSafe> EditorSessionFactory;
		TSharedRef<CaptureMediaLongJob::IUeAgentCaptureMediaLongJobPortFactory,
			ESPMode::ThreadSafe> CaptureMediaFactory;
		TSharedRef<PieRuntimeDebug::IUeAgentPieRuntimeScenarioPortFactory,
			ESPMode::ThreadSafe> PieRuntimeScenarioFactory;
		FLongRunningOperationOwner Owner;
		TAtomic<bool> bPolling{false};
		TAtomic<bool> bShutdownRequested{false};
		TMap<FString, FUeAgentLongRunningSessionActivationProfile> Profiles;
	};

	namespace
	{
		bool TryCanonicalRequest(
			const TSharedRef<FUeAgentLongRunningSessionRuntime::FState,
				ESPMode::ThreadSafe>& State,
			const FUeAgentOperationRequest& Request,
			const TSharedRef<IUeAgentCancellationSignal>& Cancellation,
			TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe>& OutResources,
			FUeAgentOperationDeadline& OutDeadline,
			TOptional<FUeAgentContractValue>& OutCanonical,
			FUeAgentContractSchemaError& OutError)
		{
			OutResources = MakeShared<FUeAgentResourceTracker, ESPMode::ThreadSafe>();
			const auto Clock = []() { return FPlatformTime::Seconds(); };
			OutDeadline = FUeAgentOperationDeadline();
			const FTCHARToUTF8 InputBytes(*Request.GetPayload().GetJsonValue());
			FUeAgentResourceChargeError ChargeError;
			if (!OutResources->TryCharge(
					EUeAgentResourceDimension::InputBytes,
					InputBytes.Length(),
					TEXT("long_running_session.c0.input"),
					ChargeError))
			{
				OutError = ChargeError;
				return false;
			}
			const FUeAgentContractSchema* Schema =
				State->Descriptors->FindCompiledInputSchemaExact(
					Request.GetDescriptorIdentity());
			if (!Schema)
			{
				OutError = MissingDescriptor(TEXT("long_running_session_schema_unavailable"),
					TEXT("No compiled input schema is registered for the requested descriptor identity."), Request.GetDescriptorIdentity());
				return false;
			}
			const FUeAgentContractSchemaCheckpoint Checkpoint = [
				Resources = OutResources,
				Deadline = OutDeadline,
				Cancellation
			](
				const int64 WorkUnits,
				const FString& InstancePath,
				FUeAgentContractSchemaError& Error)
			{
				if (Cancellation->IsCancelled())
				{
					Error = FDiagnostic::Failure(TEXT("operation_cancelled"), TEXT("The operation was cancelled."), TEXT("cancellation"));
					if (Error.Source.JsonPath.IsEmpty()) Error.Source.JsonPath = InstancePath;
					return false;
				}
				FUeAgentResourceChargeError ResourceError;
				if (!Resources->TryCharge(
						EUeAgentResourceDimension::WorkUnits,
						WorkUnits,
						TEXT("long_running_session.c0.schema"),
						ResourceError))
				{
					Error = ResourceError;
				if (Error.Source.JsonPath.IsEmpty()) Error.Source.JsonPath = InstancePath;
					return false;
				}
				return true;
			};
			return Schema->TryValidateAndCanonicalize(
				Request.GetPayload(), Checkpoint, OutCanonical, OutError);
		}

		bool TryContext(
			const FUeAgentLongRunningSessionRuntime::FState& State,
			const FUeAgentLongRunningSessionActivationProfile& Profile,
			const FString& FamilyId,
			const FString& SubjectRef,
			const FString& PolicyId,
			FUeAgentExecutionContextRefPtr& OutContext,
			FExecutionMutationError& OutError)
		{
			const FUeAgentOperationDescriptor* Descriptor =
				State.Descriptors->FindExact(Profile.DescriptorIdentity);
			if (!Descriptor)
			{
				OutError = MissingDescriptor(TEXT("long_running_session_descriptor_missing"),
					TEXT("The active profile's exact descriptor is absent from the registry."), Profile.DescriptorIdentity);
				return false;
			}
			FUeAgentExecutionContextInput Input;
			Input.ProjectInstanceId = State.Config.ProjectInstanceId;
			Input.EditorInstanceId = State.Config.EditorInstanceId;
			if (Profile.CommandId == TEXT("job.compile_build"))
			{
				Input.StatePlane = EUeAgentStatePlane::Persisted;
			}
			else if (Profile.CommandId == TEXT("editor.safe_exit"))
			{
				Input.StatePlane = EUeAgentStatePlane::RuntimeInstance;
			}
			else if (Profile.CommandId == TEXT("capture.media.execute")
				|| Profile.CommandId == TEXT("runtime.scenario.execute")
				|| FamilyId == TEXT("runtime_preview"))
			{
				Input.StatePlane = EUeAgentStatePlane::RuntimeInstance;
			}
			else
			{
				Input.StatePlane = EUeAgentStatePlane::AuthoringMemory;
			}
			if (Profile.CommandId == TEXT("runtime.scenario.execute"))
			{
				const FString Prefix(TEXT("runtime-session:pie-debug:"));
				const FString Suffix = SubjectRef.StartsWith(Prefix)
					? SubjectRef.RightChop(Prefix.Len()) : FString();
				int32 Separator = INDEX_NONE;
				int32 PieInstanceId = INDEX_NONE;
				if (!Suffix.FindChar(TEXT(':'), Separator)
					|| !LexTryParseString(
						PieInstanceId, *Suffix.Left(Separator))
					|| PieInstanceId < 0)
				{
					OutError = InputValueFailure(TEXT("long_running_session_pie_debug_subject_invalid"),
						TEXT("The runtime scenario subject must identify a PIE debug session with a non-negative instance id."), TEXT("/subject_ref"),
						MakeShared<FJsonValueString>(SubjectRef), TEXT("runtime-session:pie-debug:<non-negative instance>:<session>"));
					return false;
				}
				Input.WorldType = EUeAgentExecutionWorldType::Pie;
				Input.PieInstanceId = PieInstanceId;
				Input.WorldIdentity = SubjectRef;
			}
			else if (Profile.CommandId == TEXT("capture.image.execute")
				|| Profile.CommandId == TEXT("capture.media.execute")
				|| Profile.CommandId == TEXT("capture.render.execute")
				|| FamilyId == TEXT("world_context")
				|| FamilyId == TEXT("modeling_tool")
				|| FamilyId == TEXT("pcg_generation"))
			{
				Input.WorldType = EUeAgentExecutionWorldType::Editor;
			}
			if (FamilyId == TEXT("runtime_preview")
				&& PolicyId
					== TEXT("policy:runtime-session:culture-preview/v1"))
			{
				Input.WorldType = EUeAgentExecutionWorldType::Editor;
			}
			else if (FamilyId == TEXT("runtime_preview"))
			{
				const FString Prefix(TEXT("runtime-subject:pie:"));
				int32 PieInstanceId = INDEX_NONE;
				if (!SubjectRef.StartsWith(Prefix)
					|| !LexTryParseString(
						PieInstanceId,
						*SubjectRef.RightChop(Prefix.Len()))
					|| PieInstanceId < 0)
				{
					OutError = InputValueFailure(TEXT("long_running_session_pie_subject_invalid"),
						TEXT("The runtime preview subject must identify a non-negative PIE instance."), TEXT("/subject_ref"),
						MakeShared<FJsonValueString>(SubjectRef), TEXT("runtime-subject:pie:<non-negative instance>"));
					return false;
				}
				Input.WorldType = EUeAgentExecutionWorldType::Pie;
				Input.PieInstanceId = PieInstanceId;
				Input.WorldIdentity = SubjectRef;
			}
			if (Input.WorldType == EUeAgentExecutionWorldType::Editor)
			{
				Input.WorldIdentity = State.Config.EditorWorldIdentity;
				Input.LevelIdentity = State.Config.EditorLevelIdentity;
				if (State.Config.ResolveEditorIdentities)
				{
					State.Config.ResolveEditorIdentities(Input.WorldIdentity, Input.LevelIdentity);
				}
			}
			Input.ContextRevision =
				TEXT("long-running-session:") + State.Config.ServerSessionId
				+ TEXT(":") + FamilyId + TEXT(":") + SubjectRef;
			FUeAgentExecutionContextRefPtr Context;
			FUeAgentExecutionContextError ContextError;
			if (!FUeAgentExecutionContextRef::TryCreate(
					Input,
					Profile.DescriptorIdentity,
					EUeAgentRequestKind::Operation,
					Descriptor->GetExecutionContextPolicy(),
					Context,
					ContextError))
			{
				OutError = MoveTemp(ContextError);
				return false;
			}
			OutContext = Context;
			return true;
		}

		class FDeferredCommandPort final : public ILongRunningOperationPort
		{
			struct FWorkState
			{
				explicit FWorkState(TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe> InResources)
					: Resources(MoveTemp(InResources)) {}
				FCriticalSection Mutex;
				TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe> Resources;
				bool bComplete = false;
				bool bCancelRequested = false;
				bool bSignalDelivered = false;
				UE::UAI::Contract::FDiagnostic Error;
				TOptional<FLongRunningOperationSignal> Signal;
			};
		public:
			using FWork = TFunction<void(TFunction<bool()>, TFunction<void(CommandEdge::FUeAgentCommandResult)>)>;
			FDeferredCommandPort(TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe> Resources, FWork InWork)
				: Work(MoveTemp(InWork)), WorkState(MakeShared<FWorkState, ESPMode::ThreadSafe>(MoveTemp(Resources))) {}

			bool Start(const FLongRunningOperationRequest& Request, const FUeAgentCancellationToken& Cancellation,
				FLongRunningWorkIdentity& OutIdentity, FExecutionMutationError& OutError) override
			{
				OutIdentity = FLongRunningWorkIdentity(TEXT("deferred-command:") + Request.GetOperationId(), 1);
				Work([State = WorkState]() { FScopeLock Lock(&State->Mutex); return State->bCancelRequested; },
					[State = WorkState](CommandEdge::FUeAgentCommandResult Result)
					{
						CommandEdge::FUeAgentCommandProtocolCodec Codec;
						TSharedPtr<FJsonObject> Root;
						FUeAgentCanonicalValuePtr Canonical;
						FDiagnostic Error;
						bool bProjected = Codec.TryProjectResult(Result, Root, Error);
						if (bProjected)
						{
							bProjected = TryCanonicalStateValue(MakeShared<FJsonValueObject>(Root), *State->Resources, Canonical, Error);
							if (!bProjected)
							{
								if (!Result.GetDiagnostic().IsEmpty()) Error.Diagnostics.Add(Result.GetDiagnostic());
								Error.Diagnostics.Append(Result.GetDiagnostics());
							}
						}
						FScopeLock Lock(&State->Mutex);
						if (bProjected)
							State->Signal.Emplace(TEXT("uai.command.result.v1"), 1, Canonical.ToSharedRef());
						else
							State->Error = MoveTemp(Error);
						State->bComplete = true;
					});
				return true;
			}

			bool Observe(const FString& OperationId, const FLongRunningWorkIdentity& Identity,
				FUeAgentResourceTracker& Resources, FLongRunningWorkObservation& OutObservation,
				FExecutionMutationError& OutError) override
			{
				FScopeLock Lock(&WorkState->Mutex);
				TOptional<FLongRunningOperationSignal> Signal;
				if (WorkState->bComplete && !WorkState->bSignalDelivered && WorkState->Signal.IsSet())
				{
					Signal = WorkState->Signal;
					WorkState->bSignalDelivered = true;
				}
				OutObservation = FLongRunningWorkObservation(Identity.GetGeneration(),
					!WorkState->bComplete ? ELongRunningWorkState::Running
						: WorkState->Error.IsEmpty() ? ELongRunningWorkState::ExitedSuccess : ELongRunningWorkState::ExitedFailure,
					WorkState->Error, MoveTemp(Signal));
				return true;
			}

			bool RequestStop(const FString& OperationId, const FLongRunningWorkIdentity& Identity,
				const FString& Reason, FExecutionMutationError& OutError) override
			{
				FScopeLock Lock(&WorkState->Mutex);
				// A queued command can stop before dispatch. A dispatched atomic import
				// completes its native transaction; its actual reply remains in the receipt.
				WorkState->bCancelRequested = true;
				return true;
			}

			bool Join(const FString& OperationId, const FLongRunningWorkIdentity& Identity,
				FExecutionMutationError& OutError) override
			{
				FScopeLock Lock(&WorkState->Mutex);
				return WorkState->bComplete;
			}

			bool Release(const FString& OperationId, const FLongRunningWorkIdentity& Identity,
				FExecutionMutationError& OutError) override
			{
				Work = {};
				return true;
			}
		private:
			FWork Work;
			TSharedRef<FWorkState, ESPMode::ThreadSafe> WorkState;
		};

		FString DerivedIdentity(const FString& Prefix, const FString& Material)
		{
			FString Hash = Json::Sha256Utf8(Material);
			Hash.RemoveFromStart(TEXT("sha256:"));
			return Prefix + Hash;
		}

		FUeAgentOperationResult ExecuteStart(
			const TSharedRef<FUeAgentLongRunningSessionRuntime::FState,
				ESPMode::ThreadSafe>& State,
			const FUeAgentLongRunningSessionActivationProfile& Profile,
			const FUeAgentOperationRequest& Request,
			const FUeAgentCallerContext& CallerContext,
			const FUeAgentContractValue& CanonicalPayload,
			const TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe>& Resources,
			const FUeAgentOperationDeadline& Deadline)
		{
			if (State->bShutdownRequested.Load())
				return Failure(Request, RuntimeFailure(TEXT("operation_runtime_shutting_down"),
					Profile.CommandId, TEXT("The Editor is draining owned operations before shutdown.")));
			const FString ParamsHash =
				Json::Sha256Utf8(CanonicalPayload.GetJsonValue());
			const FString OperationId = DerivedIdentity(
				TEXT("operation:"),
				State->Config.ServerSessionId + TEXT("\n")
					+ Profile.CommandId + TEXT("\n")
					+ Request.GetRequestId() + TEXT("\n") + ParamsHash);
			const FString OwnerLeaseId = DerivedIdentity(
				TEXT("lease:"),
				State->Config.OwnerLeaseSecret + TEXT("\n") + OperationId);
			FLongRunningOperationSnapshot Snapshot;
			FExecutionMutationError OwnerError;
			if (State->Owner.GetSnapshot(
					OperationId,
					OwnerLeaseId,
					State->Config.ServerSessionId,
					Snapshot,
					OwnerError))
			{
				return SnapshotResult(Request, Snapshot);
			}
			if (OwnerError.Code != TEXT("operation_not_found"))
			{
				return Failure(Request, OwnerError, OperationId);
			}

			TSharedPtr<FJsonObject> Root;
			FDiagnostic JsonError;
			if (!Json::TryParseObject(CanonicalPayload.GetJsonValue(), Root, JsonError))
			{
				return Failure(Request, MoveTemp(JsonError));
			}

			FString SubjectRef;
			FString PolicyId;
			FString FamilyId;
			TSharedPtr<FJsonValue> Parameters;
			if (Profile.CommandId == TEXT("editor.safe_exit"))
			{
				SubjectRef = State->Config.EditorInstanceId;
				PolicyId = TEXT("policy:host-lifecycle:prepare-safe-exit/v1");
				FamilyId = TEXT("host_lifecycle");
				Parameters = MakeShared<FJsonValueObject>(Root);
			}
			else
			{
				Root->TryGetStringField(TEXT("subject_ref"), SubjectRef);
				Root->TryGetStringField(TEXT("policy_id"), PolicyId);
				Root->TryGetStringField(TEXT("family_id"), FamilyId);
				Parameters = Root->TryGetField(TEXT("parameters"));
			}
			FUeAgentExecutionContextRefPtr Context;
			if (!TryContext(
					*State,
					Profile,
					FamilyId,
					SubjectRef,
					PolicyId,
					Context,
					OwnerError))
			{
				return Failure(Request, OwnerError, OperationId);
			}
			const FLongRunningOperationRequest OperationRequest(
				OperationId,
				OwnerLeaseId,
				State->Config.ServerSessionId,
				Profile.DescriptorIdentity,
				Context.ToSharedRef(),
				Resources,
				Deadline,
				{CallerContext.GetCallerId(), Request.GetRequestId(), Profile.CommandId, SubjectRef,
					FamilyId == TEXT("host_lifecycle")});
			FUeAgentCanonicalValuePtr CanonicalParameters;
			if (!TryCanonicalStateValue(Parameters, *Resources, CanonicalParameters, OwnerError,
					Profile.CommandId == TEXT("editor.safe_exit") ? FString() : TEXT("/parameters"), TEXT("command_input")))
			{
				return Failure(Request, OwnerError, Request.GetRequestId());
			}

			bool bAccepted = false;
			if (Profile.CommandId == TEXT("job.compile_build"))
			{
				CompileBuildJob::EUeAgentCompileBuildJobFamily Family =
					CompileBuildJob::EUeAgentCompileBuildJobFamily::AssetCompile;
				if (!TryCompileFamily(FamilyId, Family))
				{
					TArray<TSharedPtr<FJsonValue>> Allowed;
					for (const auto& Contract : CompileBuildJob::GetCompileBuildJobFamilyContracts())
						Allowed.Add(MakeShared<FJsonValueString>(Contract.GetFamilyId()));
					return Failure(Request, ValueFailure(TEXT("compile_build_job_family_invalid"),
						TEXT("The requested CompileBuild family is not registered."), TEXT("/family_id"),
						MakeShared<FJsonValueString>(FamilyId), MakeShared<FJsonValueArray>(Allowed), TEXT("command_input")));
				}
				bAccepted = CompileBuildJob::TryAcceptCompileBuildJob(
					CompileBuildJob::FUeAgentCompileBuildJobTypedRequest(
						Family,
						MoveTemp(SubjectRef),
						MoveTemp(PolicyId),
						CanonicalParameters.ToSharedRef()),
					OperationRequest,
					*State->CompileBuildFactory,
					State->Owner,
					Snapshot,
					OwnerError);
			}
			else if (Profile.CommandId == TEXT("editor.session")
				|| Profile.CommandId == TEXT("editor.safe_exit"))
			{
				EditorSessionOperation::EUeAgentEditorSessionOperationFamily Family =
					EditorSessionOperation::EUeAgentEditorSessionOperationFamily::
						AssetEditor;
				if (!TryEditorFamily(FamilyId, Family)
					|| (Profile.CommandId == TEXT("editor.session")
						&& !EditorSessionOperation::IsEditorSessionCommandFamily(Family)))
				{
					TArray<TSharedPtr<FJsonValue>> Allowed;
					for (const auto& Contract : EditorSessionOperation::GetEditorSessionOperationFamilyContracts())
					{
						const bool bPublicSessionFamily = EditorSessionOperation::IsEditorSessionCommandFamily(Contract.GetFamily());
						if ((Profile.CommandId == TEXT("editor.session")) == bPublicSessionFamily)
							Allowed.Add(MakeShared<FJsonValueString>(Contract.GetFamilyId()));
					}
					return Failure(Request, ValueFailure(TEXT("editor_session_operation_family_invalid"),
						TEXT("The requested EditorSession family is not available through this command."), TEXT("/family_id"),
						MakeShared<FJsonValueString>(FamilyId), MakeShared<FJsonValueArray>(Allowed), TEXT("command_input")));
				}
				bAccepted = EditorSessionOperation::TryAcceptEditorSessionOperation(
					EditorSessionOperation::FUeAgentEditorSessionOperationTypedRequest(
						Family,
						MoveTemp(SubjectRef),
						MoveTemp(PolicyId),
						CanonicalParameters.ToSharedRef()),
					OperationRequest,
					*State->EditorSessionFactory,
					State->Owner,
					Snapshot,
					OwnerError);
			}
			else if (Profile.CommandId == TEXT("runtime.scenario.execute"))
			{
				bAccepted = PieRuntimeDebug::TryAcceptPieRuntimeScenario(
					PieRuntimeDebug::FUeAgentPieRuntimeScenarioTypedRequest(
						MoveTemp(SubjectRef),
						MoveTemp(PolicyId),
						CanonicalParameters.ToSharedRef()),
					OperationRequest,
					*State->PieRuntimeScenarioFactory,
					State->Owner,
					Snapshot,
					OwnerError);
			}
			else
			{
				bAccepted = CaptureMediaLongJob::TryAcceptCaptureMediaLongJob(
					CaptureMediaLongJob::FUeAgentCaptureMediaLongJobTypedRequest(
						CaptureFamily(Profile.CommandId),
						MoveTemp(SubjectRef),
						MoveTemp(PolicyId),
						CanonicalParameters.ToSharedRef()),
					OperationRequest,
					*State->CaptureMediaFactory,
					State->Owner,
					Snapshot,
					OwnerError);
			}
			if (!bAccepted)
			{
				if (OwnerError.Code == TEXT("operation_already_exists")
					&& State->Owner.GetSnapshot(
						OperationId,
						OwnerLeaseId,
						State->Config.ServerSessionId,
						Snapshot,
						OwnerError))
				{
					return SnapshotResult(Request, Snapshot);
				}
				return Failure(Request, OwnerError, OperationId);
			}
			return SnapshotResult(Request, Snapshot);
		}

		FUeAgentOperationResult ExecuteLifecycle(
			const TSharedRef<FUeAgentLongRunningSessionRuntime::FState,
				ESPMode::ThreadSafe>& State,
			const FUeAgentLongRunningSessionActivationProfile& Profile,
			const FUeAgentOperationRequest& Request,
			const FUeAgentCallerContext& CallerContext,
			const FUeAgentContractValue& CanonicalPayload,
			const FUeAgentOperationDeadline& CallDeadline)
		{
			TSharedPtr<FJsonObject> Root;
			FDiagnostic JsonError;
			FOperationRef Ref;
			if (!Json::TryParseObject(CanonicalPayload.GetJsonValue(), Root, JsonError))
			{
				return Failure(Request, MoveTemp(JsonError));
			}
			FExecutionMutationError OwnerError;
			FLongRunningOperationSnapshot Snapshot;
			if (Profile.CommandId == TEXT("operation.discover"))
			{
				TArray<TSharedPtr<FJsonValue>> Entries;
				for (const auto& Entry : State->Owner.Discover(CallerContext.GetCallerId()))
				{
					const auto Item = MakeShared<FJsonObject>();
					Item->SetStringField(TEXT("operation_id"), Entry.Snapshot.GetOperationId());
					Item->SetStringField(TEXT("command_id"), Entry.Origin.CommandId);
					Item->SetStringField(TEXT("subject_ref"), Entry.Origin.SubjectRef);
					Item->SetStringField(TEXT("phase"), LexToString(Entry.Snapshot.GetPhase()));
					Item->SetStringField(TEXT("lifecycle_status"), LifecycleStatus(Entry.Snapshot));
					Item->SetBoolField(TEXT("terminal"), Entry.Snapshot.IsTerminal());
					Item->SetBoolField(TEXT("cleanup_complete"), Entry.Snapshot.IsCleanupComplete());
					Entries.Add(MakeShared<FJsonValueObject>(Item));
				}
				const auto Result = MakeShared<FJsonObject>();
				Result->SetStringField(TEXT("schema"), TEXT("uai.long_running.discovery.v1"));
				Result->SetArrayField(TEXT("operations"), Entries);
				return ResultObject(Request, Result);
			}
			if (Profile.CommandId == TEXT("operation.recover"))
			{
				FString OriginalRequestId, CommandId;
				Root->TryGetStringField(TEXT("request_id"), OriginalRequestId);
				Root->TryGetStringField(TEXT("command_id"), CommandId);
				return State->Owner.Recover(CallerContext.GetCallerId(), OriginalRequestId, CommandId, Snapshot, OwnerError)
					? SnapshotResult(Request, Snapshot) : Failure(Request, OwnerError);
			}
			if (Profile.CommandId == TEXT("operation.takeover"))
			{
				FString OperationId;
				Root->TryGetStringField(TEXT("operation_id"), OperationId);
				const FString Lease = DerivedIdentity(TEXT("lease:"), State->Config.OwnerLeaseSecret
					+ TEXT("\n") + OperationId + TEXT("\ntakeover:") + Request.GetRequestId());
				return State->Owner.TakeOver(OperationId, CallerContext.GetCallerId(), Lease, Request.GetRequestId(), Snapshot, OwnerError)
					? SnapshotResult(Request, Snapshot) : Failure(Request, OwnerError, OperationId);
			}
			if (!TryReadOperationRef(Root, Ref, JsonError)) return Failure(Request, MoveTemp(JsonError));
			if (Ref.ServerSessionId != State->Config.ServerSessionId)
			{
				FDiagnostic Error = ValueFailure(TEXT("operation_server_session_mismatch"),
					TEXT("The operation reference server_session_id does not match the active server-derived identity."),
					TEXT("/operation_ref/server_session_id"), MakeShared<FJsonValueBoolean>(false),
					MakeShared<FJsonValueBoolean>(true), TEXT("command_input"));
				Error.ActualEvidence = MakeShared<FJsonValueString>(Ref.ServerSessionId);
				Error.ExpectedEvidence = MakeShared<FJsonValueString>(State->Config.ServerSessionId);
				return Failure(Request, MoveTemp(Error), Ref.OperationId);
			}
			if (Profile.CommandId == TEXT("operation.status"))
			{
				return State->Owner.GetSnapshot(
						Ref.OperationId,
						Ref.OwnerLeaseId,
						Ref.ServerSessionId,
						Snapshot,
						OwnerError)
					? SnapshotResult(Request, Snapshot)
					: Failure(Request, OwnerError, Ref.OperationId);
			}
			if (Profile.CommandId == TEXT("operation.resume"))
			{
				int64 ExpectedGeneration = 0;
				if (!TryReadInteger(Root, TEXT("expected_generation"), ExpectedGeneration, OwnerError))
				{
					return Failure(Request, MoveTemp(OwnerError), Ref.OperationId);
				}
				return State->Owner.Advance(
						Ref.OperationId,
						Ref.OwnerLeaseId,
						Ref.ServerSessionId,
						ExpectedGeneration,
						CallDeadline,
						Snapshot,
						OwnerError)
					? SnapshotResult(Request, Snapshot)
					: Failure(Request, OwnerError, Ref.OperationId);
			}
			if (Profile.CommandId == TEXT("operation.cancel"))
			{
				FString Reason;
				Root->TryGetStringField(TEXT("reason"), Reason);
				return State->Owner.RequestCancellation(
						Ref.OperationId,
						Ref.OwnerLeaseId,
						Ref.ServerSessionId,
						Reason,
						Snapshot,
						OwnerError)
					? SnapshotResult(Request, Snapshot)
					: Failure(Request, OwnerError, Ref.OperationId);
			}
			if (Profile.CommandId == TEXT("operation.reconcile"))
			{
				int64 ExpectedGeneration = 0;
				if (!TryReadInteger(Root, TEXT("expected_generation"), ExpectedGeneration, OwnerError))
				{
					return Failure(Request, MoveTemp(OwnerError), Ref.OperationId);
				}
				if (!State->Owner.GetSnapshot(
						Ref.OperationId,
						Ref.OwnerLeaseId,
						Ref.ServerSessionId,
						Snapshot,
						OwnerError))
				{
					return OwnerError.Code == TEXT("operation_not_found")
						? UnknownReconcileResult(Request, Ref, ExpectedGeneration)
						: Failure(Request, OwnerError, Ref.OperationId);
				}
				if (Snapshot.GetGeneration() != ExpectedGeneration)
				{
					return Failure(Request, ValueFailure(TEXT("operation_generation_mismatch"),
						TEXT("The supplied generation does not match the current operation snapshot."),
						TEXT("/expected_generation"), MakeShared<FJsonValueNumberString>(FString::Printf(TEXT("%lld"), static_cast<long long>(ExpectedGeneration))),
						MakeShared<FJsonValueNumberString>(FString::Printf(TEXT("%lld"), static_cast<long long>(Snapshot.GetGeneration()))), TEXT("command_input")), Ref.OperationId);
				}
				return SnapshotResult(Request, Snapshot);
			}

			FLongRunningOperationReceipt Receipt;
			if (Profile.CommandId == TEXT("operation.receipt"))
			{
				if (!State->Owner.GetSnapshot(Ref.OperationId, Ref.OwnerLeaseId,
					Ref.ServerSessionId, Snapshot, OwnerError))
					return Failure(Request, OwnerError, Ref.OperationId);
				return State->Owner.GetTerminalReceipt(
						Ref.OperationId,
						Ref.OwnerLeaseId,
						Ref.ServerSessionId,
						Receipt,
						OwnerError)
					? ReceiptResult(Request, Ref, Receipt, FString(), &Snapshot)
					: Failure(Request, OwnerError, Ref.OperationId);
			}
			if (!State->Owner.GetSnapshot(Ref.OperationId, Ref.OwnerLeaseId, Ref.ServerSessionId, Snapshot, OwnerError)
				|| !State->Owner.GetTerminalReceipt(
					Ref.OperationId,
					Ref.OwnerLeaseId,
					Ref.ServerSessionId,
					Receipt,
					OwnerError)
				|| !State->Owner.ReleaseTerminal(
					Ref.OperationId,
					Ref.OwnerLeaseId,
					Ref.ServerSessionId,
					OwnerError))
			{
				return Failure(Request, OwnerError, Ref.OperationId);
			}
			return ReceiptResult(Request, Ref, Receipt, TEXT("released"), &Snapshot);
		}

		FUeAgentOperationResult Execute(
			const TSharedRef<FUeAgentLongRunningSessionRuntime::FState,
				ESPMode::ThreadSafe>& State,
			const FUeAgentOperationRequest& Request,
			const FUeAgentCallerContext& CallerContext,
			const TSharedRef<IUeAgentCancellationSignal>& Cancellation)
		{
			const FUeAgentLongRunningSessionActivationProfile* Profile =
				State->FindProfile(Request.GetDescriptorIdentity());
			if (!Request.IsValid()) return Failure(Request, Request.GetValidationDiagnostic());
			if (!CallerContext.IsValid()) return Failure(Request, CallerContext.GetValidationDiagnostic());
			if (!Profile)
			{
				return Failure(Request, MissingDescriptor(TEXT("long_running_session_route_invalid"),
					TEXT("No active long-running session profile matches the requested exact descriptor."), Request.GetDescriptorIdentity()));
			}
			TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe> Resources =
				MakeShared<FUeAgentResourceTracker, ESPMode::ThreadSafe>();
			FUeAgentOperationDeadline Deadline;
			TOptional<FUeAgentContractValue> CanonicalPayload;
			FUeAgentContractSchemaError SchemaError;
			if (!TryCanonicalRequest(
					State,
					Request,
					Cancellation,
					Resources,
					Deadline,
					CanonicalPayload,
					SchemaError))
			{
				return Failure(Request, MoveTemp(SchemaError));
			}
			return Profile->IsStartCommand()
				? ExecuteStart(
					State,
					*Profile,
					Request,
					CallerContext,
					CanonicalPayload.GetValue(),
					Resources,
					Deadline)
				: ExecuteLifecycle(
					State,
					*Profile,
					Request,
					CallerContext,
					CanonicalPayload.GetValue(),
					Deadline);
		}

		class FOperationApplication final
			: public IUeAgentOperationApplicationPort
		{
		public:
			explicit FOperationApplication(
				TSharedRef<FUeAgentLongRunningSessionRuntime::FState,
					ESPMode::ThreadSafe> InState)
				: State(MoveTemp(InState))
			{
			}

			FUeAgentOperationResult Submit(
				const FUeAgentOperationRequest& Request,
				const FUeAgentCallerContext& CallerContext,
				const TSharedRef<IUeAgentCancellationSignal>& Cancellation)
				override
			{
				return Execute(State, Request, CallerContext, Cancellation);
			}

		private:
			TSharedRef<FUeAgentLongRunningSessionRuntime::FState,
				ESPMode::ThreadSafe> State;
		};
	}

	bool FUeAgentLongRunningSessionRuntimeConfig::IsValid() const
	{
		return !ProjectInstanceId.TrimStartAndEnd().IsEmpty()
			&& !EditorInstanceId.TrimStartAndEnd().IsEmpty()
			&& !ServerSessionId.TrimStartAndEnd().IsEmpty()
			&& !OwnerLeaseSecret.TrimStartAndEnd().IsEmpty();
	}

	FUeAgentLongRunningSessionRuntime::FUeAgentLongRunningSessionRuntime(
		TSharedRef<FState, ESPMode::ThreadSafe> InState,
		TSharedRef<IUeAgentOperationApplicationPort> InOperationPort)
		: State(MoveTemp(InState))
		, OperationPort(MoveTemp(InOperationPort))
	{
	}

	bool FUeAgentLongRunningSessionRuntime::TryCreate(
		const FUeAgentLongRunningSessionRuntimeConfig& Config,
		TSharedRef<CompileBuildJob::IUeAgentCompileBuildJobPortFactory,
			ESPMode::ThreadSafe> CompileBuildFactory,
		TSharedRef<EditorSessionOperation::
			IUeAgentEditorSessionOperationPortFactory,
			ESPMode::ThreadSafe> EditorSessionFactory,
		TSharedRef<CaptureMediaLongJob::IUeAgentCaptureMediaLongJobPortFactory,
			ESPMode::ThreadSafe> CaptureMediaFactory,
		TSharedRef<PieRuntimeDebug::IUeAgentPieRuntimeScenarioPortFactory,
			ESPMode::ThreadSafe> PieRuntimeScenarioFactory,
		TSharedPtr<FUeAgentLongRunningSessionRuntime>& OutRuntime,
		FUeAgentLongRunningSessionRuntimeError& OutError)
	{
		OutRuntime.Reset();
		OutError = {};
		if (!Config.IsValid())
		{
			const TCHAR* Field = Config.ProjectInstanceId.TrimStartAndEnd().IsEmpty() ? TEXT("project_instance_id")
				: Config.EditorInstanceId.TrimStartAndEnd().IsEmpty() ? TEXT("editor_instance_id")
				: Config.ServerSessionId.TrimStartAndEnd().IsEmpty() ? TEXT("server_session_id") : TEXT("owner_lease_secret");
			OutError = ValueFailure(TEXT("long_running_session_runtime_config_invalid"),
				TEXT("The runtime configuration requires a non-blank identity or lease secret."),
				FString(TEXT("/")) + Field, MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			OutError.SubjectId = TEXT("long_running_session");
			return false;
		}
		TSharedPtr<const FUeAgentDescriptorRegistry> Descriptors;
		FDiagnostic DescriptorError;
		if (!TryCreateLongRunningSessionDescriptorRegistry(
				Descriptors, DescriptorError))
		{
			OutError = MoveTemp(DescriptorError);
			return false;
		}
		const TArray<FUeAgentLongRunningSessionActivationProfile>* ActivationProfiles = nullptr;
		if (!TryGetLongRunningSessionActivationProfiles(ActivationProfiles, OutError)) return false;
		const TSharedRef<FState, ESPMode::ThreadSafe> StateRef =
			MakeShared<FState, ESPMode::ThreadSafe>(
				Config,
				Descriptors.ToSharedRef(),
				*ActivationProfiles,
				MoveTemp(CompileBuildFactory),
				MoveTemp(EditorSessionFactory),
				MoveTemp(CaptureMediaFactory),
				MoveTemp(PieRuntimeScenarioFactory));
		const TSharedRef<IUeAgentOperationApplicationPort> OperationPort =
			MakeShared<FOperationApplication>(StateRef);
		OutRuntime = MakeShareable(
			new FUeAgentLongRunningSessionRuntime(StateRef, OperationPort));
		return true;
	}

	CommandEdge::FUeAgentCommandResult FUeAgentLongRunningSessionRuntime::AcceptDeferredCommand(
		const CommandEdge::FUeAgentCommandRequest& Request, const FUeAgentCallerContext& CallerContext,
		FDeferredCommandPort::FWork Work)
	{
		using namespace CommandEdge;
		auto Fail = [&](const UE::UAI::Contract::FDiagnostic& Diagnostic) {
			return FUeAgentCommandResult::ProtocolError(Request.GetCorrelationId(), Request.GetRequestId(), Request.GetCommandId(), Request.GetCommandBindingHash(), Diagnostic);
		};
		if (!CallerContext.IsValid()) return Fail(CallerContext.GetValidationDiagnostic());
		if (State->bShutdownRequested.Load())
			return Fail(RuntimeFailure(TEXT("operation_runtime_shutting_down"),
				Request.GetCommandId(), TEXT("The Editor is draining owned operations before shutdown.")));
		const FString OperationId = DerivedIdentity(TEXT("operation:"), State->Config.ServerSessionId + TEXT("\n")
			+ Request.GetCommandId() + TEXT("\n") + Request.GetCommandBindingHash() + TEXT("\n")
			+ Request.GetRequestId() + TEXT("\n") + Json::Sha256Utf8(Request.GetParamsJson()));
		const FString Lease = DerivedIdentity(TEXT("lease:"), State->Config.OwnerLeaseSecret + TEXT("\n") + OperationId);
		FLongRunningOperationSnapshot Snapshot;
		FExecutionMutationError Error;
		if (!State->Owner.GetSnapshot(OperationId, Lease, State->Config.ServerSessionId, Snapshot, Error))
		{
			if (Error.Code != TEXT("operation_not_found")) return Fail(Error);
			const FUeAgentDescriptorIdentity Descriptor(Request.GetCommandId(), 1, Request.GetCommandBindingHash());
			FUeAgentExecutionContextInput Input;
			Input.ProjectInstanceId = State->Config.ProjectInstanceId;
			Input.EditorInstanceId = State->Config.EditorInstanceId;
			Input.StatePlane = EUeAgentStatePlane::AuthoringMemory;
			Input.ContextRevision = TEXT("deferred-command:") + State->Config.ServerSessionId;
			FUeAgentExecutionContextRefPtr Context;
			FUeAgentExecutionContextError ContextError;
			const FUeAgentExecutionContextPolicy Policy({EUeAgentStatePlane::AuthoringMemory}, {},
				EUeAgentContextResolutionPolicy::ExplicitOnly, EUeAgentTransientContextPolicy::TemporaryMutationAllowed);
			if (!FUeAgentExecutionContextRef::TryCreate(Input, Descriptor, EUeAgentRequestKind::Operation,
				Policy, Context, ContextError)) return Fail(ContextError);
			const auto Resources = MakeShared<FUeAgentResourceTracker, ESPMode::ThreadSafe>();
			const FLongRunningOperationRequest OperationRequest(OperationId, Lease, State->Config.ServerSessionId,
				Descriptor, Context.ToSharedRef(), Resources, FUeAgentOperationDeadline(),
				{CallerContext.GetCallerId(), Request.GetRequestId(), Request.GetCommandId(), FString()});
			if (!State->Owner.Accept(OperationRequest,
				MakeShared<FDeferredCommandPort, ESPMode::ThreadSafe>(Resources, MoveTemp(Work)), Snapshot, Error))
				return Fail(Error);
		}
		UE::UAI::Contract::FDiagnostic SignalError;
		const auto Payload = SnapshotObject(Snapshot, SignalError);
		if (!SignalError.IsEmpty()) return Fail(SignalError);
		FString PayloadText;
        if (!Json::TryCanonicalize(Payload, PayloadText, SignalError)) return Fail(SignalError);
        return FUeAgentCommandResult::Success(Request, PayloadText);
	}

	void FUeAgentLongRunningSessionRuntime::Pump()
	{
		if (State->bPolling.Exchange(true)) return;
		Async(EAsyncExecution::ThreadPool, [SharedState = State]()
		{
			ON_SCOPE_EXIT { SharedState->bPolling.Store(false); };
			if (SharedState->bShutdownRequested.Load())
				SharedState->Owner.RequestShutdown(TEXT("editor_shutdown"));
			SharedState->Owner.PollStarted();
		});
	}

	void FUeAgentLongRunningSessionRuntime::RequestShutdown()
	{
		State->bShutdownRequested.Store(true);
		Pump();
	}

	bool FUeAgentLongRunningSessionRuntime::IsPolling() const
	{
		return State->bPolling.Load();
	}

	int32 FUeAgentLongRunningSessionRuntime::GetPendingCount() const
	{
		return State->Owner.GetPendingCount();
	}

	const TSharedRef<IUeAgentOperationApplicationPort>&
	FUeAgentLongRunningSessionRuntime::GetOperationPort() const
	{
		return OperationPort;
	}
}

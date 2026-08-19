#include "Composition/LongRunningSession/UeAgentLongRunningSessionRuntime.h"

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

namespace UE::UAI::Composition::LongRunningSession
{
	using namespace ContextResource;
	using namespace Contract;
	using namespace DomainAdapters;
	using namespace ExecutionMutation;
	using namespace StateSemantic;

	namespace
	{
		void SetError(
			FUeAgentLongRunningSessionRuntimeError& OutError,
			FString Code,
			FString SubjectId,
			FString Detail)
		{
			OutError.Code = MoveTemp(Code);
			OutError.SubjectId = MoveTemp(SubjectId);
			OutError.Detail = MoveTemp(Detail);
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
			const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
			Root->SetObjectField(TEXT("operation_error"), Error);
			FString Text;
			FString CanonicalError;
			return Json::TryCanonicalize(Root, Text, CanonicalError)
				? Text
				: FString(TEXT("{\"operation_error\":{\"code\":"
					"\"long_running_session_error_encoding_failed\"," 
					"\"detail\":\"\",\"subject_id\":\"\"}}"));
		}

		FUeAgentOperationResult Failure(
			const FUeAgentOperationRequest& Request,
			const FString& Code,
			const FString& SubjectId,
			const FString& Detail)
		{
			return FUeAgentOperationResult(
				Request.GetDescriptorIdentity(),
				Request.GetRequestId(),
				FUeAgentContractValue(ErrorJson(
					Code.IsEmpty()
						? FString(TEXT("long_running_session_operation_failed"))
						: Code,
					SubjectId,
					Detail)));
		}

		FUeAgentOperationResult Failure(
			const FUeAgentOperationRequest& Request,
			const FExecutionMutationError& Error,
			const FString& SubjectId)
		{
			return Failure(
				Request,
				Error.GetCode(),
				SubjectId,
				Error.GetDetail());
		}

		FUeAgentResourceBudget RuntimeBudget(const int64 MaxFrameBytes)
		{
			return FUeAgentResourceBudget({
				{EUeAgentResourceDimension::InputBytes, MaxFrameBytes},
				{EUeAgentResourceDimension::OutputBytes, MaxFrameBytes * 4},
				{EUeAgentResourceDimension::StringBytes, MaxFrameBytes * 4},
				{EUeAgentResourceDimension::Nodes, MaxFrameBytes},
				{EUeAgentResourceDimension::Items, MaxFrameBytes},
				{EUeAgentResourceDimension::Files, 4096},
				{EUeAgentResourceDimension::ReferenceEdges, MaxFrameBytes},
				{EUeAgentResourceDimension::WorkUnits, MaxFrameBytes * 8},
				{EUeAgentResourceDimension::EstimatedMemoryBytes,
					MaxFrameBytes * 8},
			});
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
				OutError = FExecutionMutationError(
					TEXT("long_running_session_operation_port_not_bound"));
				return false;
			}

			bool Observe(
				const FString& OperationId,
				const FLongRunningWorkIdentity& Identity,
				FUeAgentResourceTracker& Resources,
				FLongRunningWorkObservation& OutObservation,
				FExecutionMutationError& OutError) override
			{
				OutError = FExecutionMutationError(
					TEXT("long_running_session_operation_port_not_bound"));
				return false;
			}

			bool RequestStop(
				const FString& OperationId,
				const FLongRunningWorkIdentity& Identity,
				const FString& Reason,
				FExecutionMutationError& OutError) override
			{
				OutError = FExecutionMutationError(
					TEXT("long_running_session_operation_port_not_bound"));
				return false;
			}

			bool Join(
				const FString& OperationId,
				const FLongRunningWorkIdentity& Identity,
				FExecutionMutationError& OutError) override
			{
				OutError = FExecutionMutationError(
					TEXT("long_running_session_operation_port_not_bound"));
				return false;
			}

			bool Release(
				const FString& OperationId,
				const FLongRunningWorkIdentity& Identity,
				FExecutionMutationError& OutError) override
			{
				OutError = FExecutionMutationError(
					TEXT("long_running_session_operation_port_not_bound"));
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

		FString CanonicalObjectText(const TSharedRef<FJsonObject>& Object)
		{
			FString Text;
			FString Error;
			return Json::TryCanonicalize(Object, Text, Error)
				? Text
				: FString(TEXT("{}"));
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

		FUeAgentOperationResult SnapshotResult(
			const FUeAgentOperationRequest& Request,
			const FLongRunningOperationSnapshot& Snapshot)
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
			Root->SetBoolField(TEXT("retry_safe"), false);
			if (!Snapshot.GetErrorCode().IsEmpty())
			{
				Root->SetStringField(TEXT("error_code"), Snapshot.GetErrorCode());
			}
			if (Snapshot.GetLatestSignal().IsSet())
			{
				const FLongRunningOperationSignal& Signal =
					Snapshot.GetLatestSignal().GetValue();
				TSharedPtr<FJsonValue> Payload;
				FString PayloadError;
				if (!StateSemantic::CanonicalJson::TryToJsonValue(
						*Signal.GetPayload(), Payload, PayloadError))
				{
					return Failure(
						Request,
						TEXT("long_running_session_signal_encoding_failed"),
						Snapshot.GetOperationId(),
						PayloadError);
				}
				const TSharedRef<FJsonObject> LatestSignal =
					MakeShared<FJsonObject>();
				LatestSignal->SetStringField(TEXT("type_id"), Signal.GetTypeId());
				SetExactIntegerField(
					LatestSignal, TEXT("revision"), Signal.GetRevision());
				LatestSignal->SetField(TEXT("payload"), MoveTemp(Payload));
				Root->SetObjectField(TEXT("latest_signal"), LatestSignal);
			}
			return FUeAgentOperationResult(
				Request.GetDescriptorIdentity(),
				Request.GetRequestId(),
				FUeAgentContractValue(CanonicalObjectText(Root)));
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
			Root->SetBoolField(TEXT("retry_safe"), false);
			return FUeAgentOperationResult(
				Request.GetDescriptorIdentity(),
				Request.GetRequestId(),
				FUeAgentContractValue(CanonicalObjectText(Root)));
		}

		FUeAgentOperationResult ReceiptResult(
			const FUeAgentOperationRequest& Request,
			const FOperationRef& Ref,
			const FLongRunningOperationReceipt& Receipt,
			const FString& OverrideStatus = FString())
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
			Root->SetBoolField(TEXT("retry_safe"), Receipt.IsRetrySafe());
			return FUeAgentOperationResult(
				Request.GetDescriptorIdentity(),
				Request.GetRequestId(),
				FUeAgentContractValue(CanonicalObjectText(Root)));
		}

		bool TryReadOperationRef(
			const TSharedPtr<FJsonObject>& Root,
			FOperationRef& OutRef)
		{
			OutRef = {};
			const TSharedPtr<FJsonObject>* Ref = nullptr;
			return Root.IsValid()
				&& Root->TryGetObjectField(TEXT("operation_ref"), Ref)
				&& Ref
				&& Ref->IsValid()
				&& (*Ref)->TryGetStringField(
					TEXT("operation_id"), OutRef.OperationId)
				&& (*Ref)->TryGetStringField(
					TEXT("owner_lease_id"), OutRef.OwnerLeaseId)
				&& (*Ref)->TryGetStringField(
					TEXT("server_session_id"), OutRef.ServerSessionId)
				&& OutRef.IsValid();
		}

		bool TryReadInteger(
			const TSharedPtr<FJsonObject>& Root,
			const TCHAR* Field,
			int64& OutValue)
		{
			OutValue = 0;
			const TSharedPtr<FJsonValue> Value =
				Root.IsValid() ? Root->TryGetField(Field) : nullptr;
			FString Lexeme;
			return Value.IsValid()
				&& Value->Type == EJson::Number
				&& Value->TryGetString(Lexeme)
				&& LexTryParseString(OutValue, *Lexeme);
		}

		bool TryCanonicalStateValue(
			const TSharedPtr<FJsonValue>& RootValue,
			FUeAgentResourceTracker& Resources,
			FUeAgentCanonicalValuePtr& OutValue,
			FExecutionMutationError& OutError)
		{
			OutValue.Reset();
			OutError = {};
			if (!RootValue.IsValid())
			{
				OutError = FExecutionMutationError(
					TEXT("long_running_session_canonical_value_missing"));
				return false;
			}

			struct FWork final
			{
				TSharedPtr<FJsonValue> Value;
				bool bExpanded = false;
			};
			TArray<FWork> Work;
			Work.Add({RootValue, false});
			TMap<const FJsonValue*, FUeAgentCanonicalValuePtr> Built;
			while (!Work.IsEmpty())
			{
				const FWork Current = Work.Pop(EAllowShrinking::No);
				if (!Current.Value.IsValid())
				{
					OutError = FExecutionMutationError(
						TEXT("long_running_session_canonical_value_invalid"));
					return false;
				}
				const bool bContainer = Current.Value->Type == EJson::Array
					|| Current.Value->Type == EJson::Object;
				if (bContainer && !Current.bExpanded)
				{
					Work.Add({Current.Value, true});
					if (Current.Value->Type == EJson::Array)
					{
						const TArray<TSharedPtr<FJsonValue>>& Items =
							Current.Value->AsArray();
						for (int32 Index = Items.Num() - 1; Index >= 0; --Index)
						{
							Work.Add({Items[Index], false});
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
								false});
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
					for (const TSharedPtr<FJsonValue>& Item
						: Current.Value->AsArray())
					{
						const FUeAgentCanonicalValuePtr* Found = Built.Find(Item.Get());
						if (!Found || !Found->IsValid())
						{
							OutError = FExecutionMutationError(
								TEXT("long_running_session_canonical_child_missing"));
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
							OutError = FExecutionMutationError(
								TEXT("long_running_session_canonical_child_missing"));
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
				if (!bConverted || !Converted.IsValid())
				{
					OutError = FExecutionMutationError(
						StateError.Code.IsEmpty()
							? FString(TEXT("long_running_session_canonical_value_failed"))
							: StateError.Code,
						StateError.Path);
					return false;
				}
				Built.Add(Current.Value.Get(), Converted);
			}
			const FUeAgentCanonicalValuePtr* Found = Built.Find(RootValue.Get());
			if (!Found || !Found->IsValid())
			{
				OutError = FExecutionMutationError(
					TEXT("long_running_session_canonical_root_missing"));
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
			TSharedRef<CompileBuildJob::IUeAgentCompileBuildJobPortFactory,
				ESPMode::ThreadSafe> InCompileBuildFactory,
			TSharedRef<EditorSessionOperation::
				IUeAgentEditorSessionOperationPortFactory,
				ESPMode::ThreadSafe> InEditorSessionFactory,
			TSharedRef<CaptureMediaLongJob::
				IUeAgentCaptureMediaLongJobPortFactory,
				ESPMode::ThreadSafe> InCaptureMediaFactory)
			: Config(MoveTemp(InConfig))
			, Descriptors(MoveTemp(InDescriptors))
			, CompileBuildFactory(MoveTemp(InCompileBuildFactory))
			, EditorSessionFactory(MoveTemp(InEditorSessionFactory))
			, CaptureMediaFactory(MoveTemp(InCaptureMediaFactory))
			, Owner(
				MakeShared<FRejectingDefaultPort, ESPMode::ThreadSafe>(),
				[]() { return FPlatformTime::Seconds(); },
				Config.MaxActiveOperations,
				Config.MaxRetainedOperations)
		{
			for (const FUeAgentLongRunningSessionActivationProfile& Profile
				: GetLongRunningSessionActivationProfiles())
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
		FLongRunningOperationOwner Owner;
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
			OutResources = MakeShared<FUeAgentResourceTracker, ESPMode::ThreadSafe>(
				RuntimeBudget(State->Config.MaxFrameBytes));
			const auto Clock = []() { return FPlatformTime::Seconds(); };
			OutDeadline = FUeAgentOperationDeadline::FromTimeout(
				State->Config.TimeoutSeconds, Clock);
			const FTCHARToUTF8 InputBytes(*Request.GetPayload().GetJsonValue());
			FUeAgentResourceBudgetExceeded BudgetError;
			if (!OutResources->TryCharge(
					EUeAgentResourceDimension::InputBytes,
					InputBytes.Length(),
					TEXT("long_running_session.c0.input"),
					BudgetError))
			{
				OutError.Code = BudgetError.Code;
				OutError.Detail = TEXT("Input byte budget was exceeded.");
				return false;
			}
			const FUeAgentContractSchema* Schema =
				State->Descriptors->FindCompiledInputSchemaExact(
					Request.GetDescriptorIdentity());
			if (!Schema)
			{
				OutError.Code = TEXT("long_running_session_schema_unavailable");
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
					Error.Code = TEXT("operation_cancelled");
					Error.InstancePath = InstancePath;
					return false;
				}
				FUeAgentOperationDeadlineExceeded Expired;
				if (!Deadline.TryCheckpoint(
						TEXT("long_running_session.c0.schema"), Expired))
				{
					Error.Code = TEXT("operation_deadline_exceeded");
					Error.InstancePath = InstancePath;
					Error.Detail = Expired.Stage;
					return false;
				}
				FUeAgentResourceBudgetExceeded ResourceError;
				if (!Resources->TryCharge(
						EUeAgentResourceDimension::WorkUnits,
						WorkUnits,
						TEXT("long_running_session.c0.schema"),
						ResourceError))
				{
					Error.Code = ResourceError.Code;
					Error.InstancePath = InstancePath;
					Error.Detail = TEXT("Schema work budget was exceeded.");
					return false;
				}
				return true;
			};
			return Schema->TryValidateAndCanonicalize(
				Request.GetPayload(), Checkpoint, OutCanonical, OutError)
				&& OutCanonical.IsSet();
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
				OutError = FExecutionMutationError(
					TEXT("long_running_session_descriptor_missing"));
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
				|| FamilyId == TEXT("runtime_preview"))
			{
				Input.StatePlane = EUeAgentStatePlane::RuntimeInstance;
			}
			else
			{
				Input.StatePlane = EUeAgentStatePlane::AuthoringMemory;
			}
			if (Profile.CommandId == TEXT("capture.image.execute")
				|| Profile.CommandId == TEXT("capture.media.execute")
				|| Profile.CommandId == TEXT("capture.render.execute")
				|| FamilyId == TEXT("world_context")
				|| FamilyId == TEXT("modeling_tool")
				|| FamilyId == TEXT("voxel_grid"))
			{
				Input.WorldType = EUeAgentExecutionWorldType::Editor;
				Input.WorldIdentity = State.Config.EditorWorldIdentity;
				Input.LevelIdentity = State.Config.EditorLevelIdentity;
			}
			if (FamilyId == TEXT("runtime_preview")
				&& PolicyId
					== TEXT("policy:runtime-session:culture-preview/v1"))
			{
				Input.WorldType = EUeAgentExecutionWorldType::Editor;
				Input.WorldIdentity = State.Config.EditorWorldIdentity;
				Input.LevelIdentity = State.Config.EditorLevelIdentity;
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
					OutError = FExecutionMutationError(
						TEXT("long_running_session_pie_subject_invalid"),
						TEXT("runtime_preview requires runtime-subject:pie:<instance>."));
					return false;
				}
				Input.WorldType = EUeAgentExecutionWorldType::Pie;
				Input.PieInstanceId = PieInstanceId;
				Input.WorldIdentity = SubjectRef;
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
					ContextError)
				|| !Context.IsValid())
			{
				OutError = FExecutionMutationError(
					ContextError.Code.IsEmpty()
						? FString(TEXT("long_running_session_context_invalid"))
						: ContextError.Code,
					TEXT("The explicit execution context does not satisfy the exact descriptor."));
				return false;
			}
			OutContext = Context;
			return true;
		}

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
			const FUeAgentContractValue& CanonicalPayload,
			const TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe>& Resources,
			const FUeAgentOperationDeadline& Deadline)
		{
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
			if (OwnerError.GetCode() != TEXT("operation_not_found"))
			{
				return Failure(Request, OwnerError, OperationId);
			}

			TSharedPtr<FJsonObject> Root;
			FString JsonError;
			if (!Json::TryParseObject(
					CanonicalPayload.GetJsonValue(), Root, JsonError)
				|| !Root.IsValid())
			{
				return Failure(
					Request,
					TEXT("long_running_session_canonical_payload_invalid"),
					Request.GetRequestId(),
					JsonError);
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
				Parameters = Root->TryGetField(TEXT("dirty_resolution"));
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
				Deadline);
			FUeAgentCanonicalValuePtr CanonicalParameters;
			if (!TryCanonicalStateValue(
					Parameters, *Resources, CanonicalParameters, OwnerError)
				|| !CanonicalParameters.IsValid())
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
					return Failure(
						Request,
						TEXT("compile_build_job_family_invalid"),
						FamilyId,
						TEXT("Unknown CompileBuild family."));
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
				if (!TryEditorFamily(FamilyId, Family))
				{
					return Failure(
						Request,
						TEXT("editor_session_operation_family_invalid"),
						FamilyId,
						TEXT("Unknown EditorSession family."));
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
				if (OwnerError.GetCode() == TEXT("operation_already_exists")
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
			const FUeAgentContractValue& CanonicalPayload,
			const FUeAgentOperationDeadline& CallDeadline)
		{
			TSharedPtr<FJsonObject> Root;
			FString JsonError;
			FOperationRef Ref;
			if (!Json::TryParseObject(
					CanonicalPayload.GetJsonValue(), Root, JsonError)
				|| !TryReadOperationRef(Root, Ref))
			{
				return Failure(
					Request,
					TEXT("long_running_session_operation_ref_invalid"),
					Request.GetRequestId(),
					JsonError);
			}
			if (Ref.ServerSessionId != State->Config.ServerSessionId)
			{
				return Failure(
					Request,
					TEXT("operation_server_session_mismatch"),
					Ref.OperationId,
					TEXT("operation_ref belongs to a different server session."));
			}
			const FString ExpectedOwnerLeaseId = DerivedIdentity(
				TEXT("lease:"),
				State->Config.OwnerLeaseSecret + TEXT("\n") + Ref.OperationId);
			if (Ref.OwnerLeaseId != ExpectedOwnerLeaseId)
			{
				return Failure(
					Request,
					TEXT("operation_owner_lease_mismatch"),
					Ref.OperationId,
					TEXT("operation_ref owner lease is not server-derived."));
			}

			FExecutionMutationError OwnerError;
			FLongRunningOperationSnapshot Snapshot;
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
				if (!TryReadInteger(Root, TEXT("expected_generation"), ExpectedGeneration))
				{
					return Failure(
						Request,
						TEXT("operation_generation_invalid"),
						Ref.OperationId,
						TEXT("expected_generation must be an integer."));
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
				if (!TryReadInteger(Root, TEXT("expected_generation"), ExpectedGeneration))
				{
					return Failure(
						Request,
						TEXT("operation_generation_invalid"),
						Ref.OperationId,
						TEXT("expected_generation must be an integer."));
				}
				if (!State->Owner.GetSnapshot(
						Ref.OperationId,
						Ref.OwnerLeaseId,
						Ref.ServerSessionId,
						Snapshot,
						OwnerError))
				{
					return OwnerError.GetCode() == TEXT("operation_not_found")
						? UnknownReconcileResult(Request, Ref, ExpectedGeneration)
						: Failure(Request, OwnerError, Ref.OperationId);
				}
				if (Snapshot.GetGeneration() != ExpectedGeneration)
				{
					return Failure(
						Request,
						TEXT("operation_generation_mismatch"),
						Ref.OperationId,
						TEXT("reconcile generation does not match current snapshot."));
				}
				return SnapshotResult(Request, Snapshot);
			}

			FLongRunningOperationReceipt Receipt;
			if (Profile.CommandId == TEXT("operation.receipt"))
			{
				return State->Owner.GetTerminalReceipt(
						Ref.OperationId,
						Ref.OwnerLeaseId,
						Ref.ServerSessionId,
						Receipt,
						OwnerError)
					? ReceiptResult(Request, Ref, Receipt)
					: Failure(Request, OwnerError, Ref.OperationId);
			}
			if (!State->Owner.GetTerminalReceipt(
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
			return ReceiptResult(Request, Ref, Receipt, TEXT("released"));
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
			if (!Request.IsValid()
				|| !CallerContext.IsValid()
				|| !Profile)
			{
				return Failure(
					Request,
					TEXT("long_running_session_route_invalid"),
					Request.GetDescriptorIdentity().GetDescriptorId(),
					TEXT("Exact active LongRunningSession descriptor is required."));
			}
			TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe> Resources =
				MakeShared<FUeAgentResourceTracker, ESPMode::ThreadSafe>(
					RuntimeBudget(State->Config.MaxFrameBytes));
			FUeAgentOperationDeadline Deadline =
				FUeAgentOperationDeadline::FromTimeout(
					State->Config.TimeoutSeconds,
					[]() { return FPlatformTime::Seconds(); });
			TOptional<FUeAgentContractValue> CanonicalPayload;
			FUeAgentContractSchemaError SchemaError;
			if (!TryCanonicalRequest(
					State,
					Request,
					Cancellation,
					Resources,
					Deadline,
					CanonicalPayload,
					SchemaError)
				|| !CanonicalPayload.IsSet())
			{
				return Failure(
					Request,
					SchemaError.Code.IsEmpty()
						? FString(TEXT("long_running_session_input_invalid"))
						: SchemaError.Code,
					SchemaError.InstancePath,
					SchemaError.Detail);
			}
			return Profile->IsStartCommand()
				? ExecuteStart(
					State,
					*Profile,
					Request,
					CanonicalPayload.GetValue(),
					Resources,
					Deadline)
				: ExecuteLifecycle(
					State,
					*Profile,
					Request,
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
			&& !OwnerLeaseSecret.TrimStartAndEnd().IsEmpty()
			&& TimeoutSeconds > 0.0
			&& MaxFrameBytes > 0
			&& MaxFrameBytes <= MAX_int64 / 8
			&& MaxActiveOperations > 0
			&& MaxRetainedOperations >= MaxActiveOperations;
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
		TSharedPtr<FUeAgentLongRunningSessionRuntime>& OutRuntime,
		FUeAgentLongRunningSessionRuntimeError& OutError)
	{
		OutRuntime.Reset();
		OutError = {};
		if (!Config.IsValid())
		{
			SetError(
				OutError,
				TEXT("long_running_session_runtime_config_invalid"),
				TEXT("long_running_session"),
				TEXT("Explicit server identity, lease secret and bounded resources are required."));
			return false;
		}
		TSharedPtr<const FUeAgentDescriptorRegistry> Descriptors;
		FString DescriptorError;
		if (!TryCreateLongRunningSessionDescriptorRegistry(
				Descriptors, DescriptorError)
			|| !Descriptors.IsValid()
			|| Descriptors->GetDescriptors().Num() != 12)
		{
			SetError(
				OutError,
				DescriptorError.IsEmpty()
					? FString(TEXT("long_running_session_descriptor_registry_invalid"))
					: DescriptorError,
				TEXT("long_running_session"),
				TEXT("The exact twelve-descriptor registry is required."));
			return false;
		}
		const TSharedRef<FState, ESPMode::ThreadSafe> StateRef =
			MakeShared<FState, ESPMode::ThreadSafe>(
				Config,
				Descriptors.ToSharedRef(),
				MoveTemp(CompileBuildFactory),
				MoveTemp(EditorSessionFactory),
				MoveTemp(CaptureMediaFactory));
		const TSharedRef<IUeAgentOperationApplicationPort> OperationPort =
			MakeShared<FOperationApplication>(StateRef);
		OutRuntime = MakeShareable(
			new FUeAgentLongRunningSessionRuntime(StateRef, OperationPort));
		return true;
	}

	const TSharedRef<IUeAgentOperationApplicationPort>&
	FUeAgentLongRunningSessionRuntime::GetOperationPort() const
	{
		return OperationPort;
	}
}

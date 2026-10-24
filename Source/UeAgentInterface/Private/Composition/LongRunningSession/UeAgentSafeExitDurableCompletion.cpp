#include "Composition/LongRunningSession/UeAgentSafeExitDurableCompletion.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Interfaces/IMainFrameModule.h"
#include "Misc/CoreDelegates.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace UE::UAI::Composition::LongRunningSession
{
	using namespace DomainAdapters::EditorSessionOperation;
	using namespace ExecutionMutation;

	namespace
	{
		constexpr int64 WitnessFreshnessMilliseconds = 5000;

		bool Fail(
			FExecutionMutationError& OutError,
			const TCHAR* Code,
			const FString& Reason,
			const FString& Property,
			FString Detail = FString())
		{
			OutError = FExecutionMutationError::Failure(Code, Reason, TEXT("safe_exit"), MoveTemp(Detail));
			OutError.PropertyPath = Property;
			OutError.SourceKind = TEXT("lifecycle_witness");
			return false;
		}

		bool IsLowerHex(const FString& Value, const int32 Length)
		{
			if (Value.Len() != Length)
			{
				return false;
			}
			for (const TCHAR Character : Value)
			{
				if (!((Character >= TEXT('0') && Character <= TEXT('9'))
					|| (Character >= TEXT('a') && Character <= TEXT('f'))))
				{
					return false;
				}
			}
			return true;
		}

		const TCHAR* InvalidWitnessField(const FUeAgentSafeExitExternalWitnessRef& Ref)
		{
			if (!IsLowerHex(Ref.LaunchId, 32)) return TEXT("launch_id");
			if (!IsLowerHex(Ref.ExitAttemptId, 32)) return TEXT("exit_attempt_id");
			if (!IsLowerHex(Ref.Nonce, 64)) return TEXT("nonce");
			return nullptr;
		}

		FString AttemptRoot(
			const FString& LaunchId,
			const FString& ExitAttemptId)
		{
			return FPaths::ConvertRelativePathToFull(FPaths::Combine(
				FPaths::ProjectSavedDir(),
				TEXT("UeAgentInterface"),
				TEXT("SafeExitCompletion"),
				TEXT("v1"),
				LaunchId,
				ExitAttemptId));
		}

		bool TryReadJsonObject(
			const FString& Path,
			TSharedPtr<FJsonObject>& OutObject,
			FExecutionMutationError& OutError,
			const TCHAR* MissingCode,
			const TCHAR* InvalidCode)
		{
			OutObject.Reset();
			FString Text;
			if (!FFileHelper::LoadFileToString(Text, *Path))
			{
				return Fail(OutError, MissingCode,
					FString::Printf(TEXT("The required safe-exit witness file '%s' could not be read."), *FPaths::GetCleanFilename(Path)),
					FPaths::GetCleanFilename(Path), Path);
			}
			const TSharedRef<TJsonReader<>> Reader =
				TJsonReaderFactory<>::Create(Text);
			if (!FJsonSerializer::Deserialize(Reader, OutObject)
				|| !OutObject.IsValid())
			{
				Fail(OutError, InvalidCode,
					FString::Printf(TEXT("The safe-exit witness file '%s' is not a valid JSON object."), *FPaths::GetCleanFilename(Path)),
					FPaths::GetCleanFilename(Path), Path);
				OutError.Source.Line = Reader->GetLineNumber();
				OutError.Source.Column = Reader->GetCharacterNumber();
				return false;
			}
			return true;
		}

		bool TryRequiredString(
			const TSharedRef<FJsonObject>& Object,
			const TCHAR* Field,
			FString& OutValue)
		{
			OutValue.Reset();
			return Object->TryGetStringField(Field, OutValue)
				&& !OutValue.IsEmpty()
				&& OutValue == OutValue.TrimStartAndEnd();
		}

		bool ExactIdentityMatches(
			const TSharedRef<FJsonObject>& A,
			const TSharedRef<FJsonObject>& B)
		{
			double APid = 0.0;
			double BPid = 0.0;
			FString AStart;
			FString BStart;
			FString ACreation;
			FString BCreation;
			FString AExecutable;
			FString BExecutable;
			return A->TryGetNumberField(TEXT("process_id"), APid)
				&& B->TryGetNumberField(TEXT("process_id"), BPid)
				&& APid == BPid
				&& TryRequiredString(A, TEXT("start_time_utc"), AStart)
				&& TryRequiredString(B, TEXT("start_time_utc"), BStart)
				&& AStart == BStart
				&& TryRequiredString(
					A, TEXT("creation_time_filetime_100ns"), ACreation)
				&& TryRequiredString(
					B, TEXT("creation_time_filetime_100ns"), BCreation)
				&& ACreation == BCreation
				&& TryRequiredString(
					A, TEXT("executable_path"), AExecutable)
				&& TryRequiredString(
					B, TEXT("executable_path"), BExecutable)
				&& AExecutable.Equals(BExecutable, ESearchCase::IgnoreCase);
		}

		bool ValidateSharedIdentity(
			const TSharedRef<FJsonObject>& Object,
			const FUeAgentSafeExitExternalWitnessRef& Ref)
		{
			FString LaunchId;
			FString ExitAttemptId;
			FString Nonce;
			return TryRequiredString(Object, TEXT("launch_id"), LaunchId)
				&& LaunchId == Ref.LaunchId
				&& TryRequiredString(
					Object, TEXT("exit_attempt_id"), ExitAttemptId)
				&& ExitAttemptId == Ref.ExitAttemptId
				&& TryRequiredString(Object, TEXT("nonce"), Nonce)
				&& Nonce == Ref.Nonce;
		}

		int64 UtcNowUnixMilliseconds()
		{
			const FDateTime Now = FDateTime::UtcNow();
			return Now.ToUnixTimestamp() * 1000LL + Now.GetMillisecond();
		}

		bool TryWriteJsonImmutable(
			const FString& FinalPath,
			const TSharedRef<FJsonObject>& Object,
			FExecutionMutationError& OutError)
		{
			IFileManager& FileManager = IFileManager::Get();
			auto WriteFailure = [&](const TCHAR* Code, const TCHAR* Reason, const TCHAR* Step)
			{
				Fail(OutError, Code, Reason, Step, FinalPath);
				OutError.Stage = TEXT("safe_exit_commit");
				OutError.SourceKind = TEXT("lifecycle_receipt");
				OutError.CandidateName = FPaths::GetCleanFilename(FinalPath);
				return false;
			};
			if (FileManager.FileExists(*FinalPath))
			{
				return WriteFailure(TEXT("safe_exit_receipt_already_exists"), TEXT("The immutable safe-exit receipt already exists."), TEXT("publish"));
			}
			if (!FileManager.MakeDirectory(*FPaths::GetPath(FinalPath), true))
			{
				return WriteFailure(TEXT("safe_exit_receipt_directory_failed"), TEXT("The safe-exit receipt directory could not be created."), TEXT("create_directory"));
			}
			FString Text;
			const TSharedRef<TJsonWriter<
				TCHAR,
				TCondensedJsonPrintPolicy<TCHAR>>> Writer =
				TJsonWriterFactory<
					TCHAR,
					TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
			if (!FJsonSerializer::Serialize(Object, Writer))
			{
				return WriteFailure(TEXT("safe_exit_receipt_encode_failed"), TEXT("The safe-exit receipt could not be encoded as JSON."), TEXT("encode"));
			}
			Text += TEXT("\n");
			const FString TemporaryPath = FinalPath + TEXT(".")
				+ FGuid::NewGuid().ToString(EGuidFormats::Digits)
				+ TEXT(".tmp");
			if (!FFileHelper::SaveStringToFile(
					Text,
					*TemporaryPath,
					FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
			{
				FileManager.Delete(*TemporaryPath, false, true);
				return WriteFailure(TEXT("safe_exit_receipt_write_failed"), TEXT("The safe-exit receipt temporary file could not be written."), TEXT("write"));
			}
			if (!FileManager.Move(
					*FinalPath,
					*TemporaryPath,
					false,
					true))
			{
				FileManager.Delete(*TemporaryPath, false, true);
				return WriteFailure(TEXT("safe_exit_receipt_publish_failed"), TEXT("The written safe-exit receipt could not be moved to its immutable destination."), TEXT("publish"));
			}
			return true;
		}

		class FDurableSafeExitTarget final
			: public IUeAgentEditorHostExitTarget
			, public TSharedFromThis<FDurableSafeExitTarget, ESPMode::ThreadSafe>
		{
		public:
			FDurableSafeExitTarget(
				FUeAgentValidatedSafeExitWitness InWitness,
				FUeAgentSafeExitDirtyResolutionReceipt InDirtyResolution,
				const FLongRunningOperationRequest& OperationRequest,
				FString InEditorInstanceId)
				: Witness(MoveTemp(InWitness))
				, DirtyResolution(MoveTemp(InDirtyResolution))
				, OperationId(OperationRequest.GetOperationId())
				, OwnerLeaseId(OperationRequest.GetOwnerLeaseId())
				, ServerSessionId(OperationRequest.GetServerSessionId())
				, EditorInstanceId(MoveTemp(InEditorInstanceId))
			{
			}

			~FDurableSafeExitTarget() override
			{
				if (PreExitHandle.IsValid())
				{
					FCoreDelegates::OnEnginePreExit.Remove(PreExitHandle);
				}
			}

			void Bind()
			{
				PreExitHandle = FCoreDelegates::OnEnginePreExit.AddSP(
					AsShared(),
					&FDurableSafeExitTarget::HandleEnginePreExit);
			}

			const FString& GetEditorInstanceId() const override
			{
				return EditorInstanceId;
			}

			void RequestExit() override
			{
				{
					FScopeLock Lock(&Mutex);
					if (bExitRequested) return;
					bExitRequested = true;
				}
				// Native shutdown closes asset toolkits while their preview worlds still exist
				// and saves the editor's own reopen state before destroying its windows.
				if (IMainFrameModule* MainFrame = FModuleManager::GetModulePtr<IMainFrameModule>(TEXT("MainFrame"));
					MainFrame && MainFrame->GetParentWindow().IsValid())
				{
					MainFrame->RequestCloseEditor();
					return;
				}
				if (GEditor) GEditor->BroadcastEditorClose();
				FPlatformMisc::RequestExit(false);
			}

		private:
			void HandleEnginePreExit()
			{
				FScopeLock Lock(&Mutex);
				if (!bExitRequested || bPublished)
				{
					return;
				}
				const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
				Root->SetStringField(
					TEXT("schema"),
					TEXT("uai.safe_exit.pre_exit_committed.v1"));
				Root->SetStringField(TEXT("launch_id"), Witness.GetLaunchId());
				Root->SetStringField(
					TEXT("exit_attempt_id"),
					Witness.GetExitAttemptId());
				Root->SetStringField(TEXT("nonce"), Witness.GetNonce());
				const TSharedRef<FJsonObject> OperationRef = MakeShared<FJsonObject>();
				OperationRef->SetStringField(TEXT("operation_id"), OperationId);
				OperationRef->SetStringField(TEXT("owner_lease_id"), OwnerLeaseId);
				OperationRef->SetStringField(
					TEXT("server_session_id"), ServerSessionId);
				Root->SetObjectField(TEXT("operation_ref"), OperationRef);
				Root->SetObjectField(
					TEXT("editor_process_identity"),
					Witness.GetEditorProcessIdentity());
				const TSharedRef<FJsonObject> Dirty = MakeShared<FJsonObject>();
				Dirty->SetStringField(TEXT("mode"), DirtyResolution.Mode);
				auto SetStringArray = [&Dirty](
					const TCHAR* Field,
					const TArray<FString>& Values)
				{
					TArray<TSharedPtr<FJsonValue>> Items;
					for (const FString& Value : Values)
					{
						Items.Add(MakeShared<FJsonValueString>(Value));
					}
					Dirty->SetArrayField(Field, Items);
				};
				SetStringArray(TEXT("save_resources"), DirtyResolution.SaveResources);
				SetStringArray(
					TEXT("discard_resources"),
					DirtyResolution.DiscardResources);
				SetStringArray(
					TEXT("remaining_dirty_resources"),
					DirtyResolution.RemainingDirtyResources);
				Root->SetObjectField(TEXT("dirty_resolution"), Dirty);
				Root->SetBoolField(TEXT("exit_requested"), true);
				Root->SetStringField(
					TEXT("committed_at_utc"),
					FDateTime::UtcNow().ToIso8601());
				FExecutionMutationError CommitError;
				bPublished = TryWriteJsonImmutable(
					FPaths::Combine(
						Witness.GetAttemptRoot(),
						TEXT("pre_exit_committed.json")),
					Root, CommitError);
				if (!bPublished)
				{
					const TSharedRef<FJsonObject> Failed = MakeShared<FJsonObject>();
					Failed->SetStringField(TEXT("schema"), TEXT("uai.safe_exit.pre_exit_failed.v1"));
					Failed->SetStringField(TEXT("launch_id"), Witness.GetLaunchId());
					Failed->SetStringField(TEXT("exit_attempt_id"), Witness.GetExitAttemptId());
					Failed->SetStringField(TEXT("nonce"), Witness.GetNonce());
					Failed->SetObjectField(TEXT("editor_process_identity"), Witness.GetEditorProcessIdentity());
					Failed->SetStringField(TEXT("failure_code"), CommitError.Code);
					Failed->SetObjectField(TEXT("diagnostic"), CommitError.ToPublicJson());
					FExecutionMutationError FailureWriteError;
					bPublished = TryWriteJsonImmutable(FPaths::Combine(Witness.GetAttemptRoot(), TEXT("pre_exit_failed.json")), Failed, FailureWriteError);
					if (!bPublished)
					{
						UE_LOG(LogTemp, Error, TEXT("UAI safe-exit completion: %s; failure receipt: %s"), *CommitError.PublicMessage, *FailureWriteError.PublicMessage);
					}
				}
			}

			FUeAgentValidatedSafeExitWitness Witness;
			FUeAgentSafeExitDirtyResolutionReceipt DirtyResolution;
			FString OperationId;
			FString OwnerLeaseId;
			FString ServerSessionId;
			FString EditorInstanceId;
			FCriticalSection Mutex;
			FDelegateHandle PreExitHandle;
			bool bExitRequested = false;
			bool bPublished = false;
		};
	}

	bool FUeAgentSafeExitExternalWitnessRef::IsValid() const
	{
		return InvalidWitnessField(*this) == nullptr;
	}

	bool FUeAgentSafeExitExternalWitnessRef::TryValidate(FExecutionMutationError& OutError) const
	{
		OutError.Reset();
		const TCHAR* Field = InvalidWitnessField(*this);
		if (!Field) return true;
		const FString& Value = FCString::Strcmp(Field, TEXT("launch_id")) == 0 ? LaunchId
			: FCString::Strcmp(Field, TEXT("exit_attempt_id")) == 0 ? ExitAttemptId : Nonce;
		const int32 Length = FCString::Strcmp(Field, TEXT("nonce")) == 0 ? 64 : 32;
		Fail(OutError, TEXT("editor_safe_exit_external_witness_ref_invalid"),
			TEXT("The witness identifier must contain the required number of lowercase hexadecimal characters."),
			FString(TEXT("/external_witness/")) + Field);
		OutError.Source.JsonPath = OutError.PropertyPath;
		const auto Actual = MakeShared<FJsonObject>();
		Actual->SetNumberField(TEXT("length"), Value.Len());
		Actual->SetBoolField(TEXT("valid_format"), false);
		OutError.ActualValue = MakeShared<FJsonValueObject>(Actual);
		OutError.ExpectedValue = MakeShared<FJsonValueString>(FString::Printf(TEXT("%d lowercase hexadecimal characters"), Length));
		OutError.ActualEvidence = MakeShared<FJsonValueString>(Value);
		return false;
	}

	bool FUeAgentSafeExitDirtyResolutionReceipt::IsValid() const
	{
		return (Mode == TEXT("reject") || Mode == TEXT("save")
				|| Mode == TEXT("discard"))
			&& RemainingDirtyResources.IsEmpty();
	}

	const FString& FUeAgentValidatedSafeExitWitness::GetLaunchId() const
	{
		return LaunchId;
	}

	const FString& FUeAgentValidatedSafeExitWitness::GetExitAttemptId() const
	{
		return ExitAttemptId;
	}

	const FString& FUeAgentValidatedSafeExitWitness::GetNonce() const
	{
		return Nonce;
	}

	const FString& FUeAgentValidatedSafeExitWitness::GetAttemptRoot() const
	{
		return AttemptRoot;
	}

	const TSharedRef<FJsonObject>&
	FUeAgentValidatedSafeExitWitness::GetEditorProcessIdentity() const
	{
		return EditorProcessIdentity;
	}

	bool TryValidateSafeExitExternalWitness(
		const FUeAgentSafeExitExternalWitnessRef& Ref,
		FUeAgentValidatedSafeExitWitness& OutWitness,
		FExecutionMutationError& OutError)
	{
		OutWitness = {};
		OutError = {};
		if (!Ref.TryValidate(OutError)) return false;
		const FString Root = AttemptRoot(Ref.LaunchId, Ref.ExitAttemptId);
		TSharedPtr<FJsonObject> Armed;
		if (!TryReadJsonObject(
				FPaths::Combine(Root, TEXT("armed.json")),
				Armed,
				OutError,
				TEXT("editor_safe_exit_external_witness_missing"),
				TEXT("editor_safe_exit_external_witness_invalid")))
		{
			return false;
		}
		FString ArmedSchema;
		if (!TryRequiredString(Armed.ToSharedRef(), TEXT("schema"), ArmedSchema)
			|| ArmedSchema != TEXT("uai.safe_exit.external_witness_armed.v1")
			|| !ValidateSharedIdentity(Armed.ToSharedRef(), Ref))
		{
			return Fail(
				OutError,
				TEXT("editor_safe_exit_external_witness_identity_mismatch"),
				ArmedSchema != TEXT("uai.safe_exit.external_witness_armed.v1") ? TEXT("The armed witness record has an unsupported schema.")
				: TEXT("The armed witness record does not belong to the requested launch and exit attempt."),
				ArmedSchema != TEXT("uai.safe_exit.external_witness_armed.v1") ? TEXT("armed.json.schema") : TEXT("external_witness"));
		}

		FString ExpiresAtText;
		FDateTime ExpiresAt;
		if (!TryRequiredString(
				Armed.ToSharedRef(), TEXT("expires_at_utc"), ExpiresAtText)
			|| !FDateTime::ParseIso8601(*ExpiresAtText, ExpiresAt)
			|| ExpiresAt <= FDateTime::UtcNow())
		{
			return Fail(
				OutError,
				TEXT("editor_safe_exit_external_witness_expired"),
				ExpiresAtText.IsEmpty() ? TEXT("The armed witness record has no expiry timestamp.")
				: !FDateTime::ParseIso8601(*ExpiresAtText, ExpiresAt) ? TEXT("The armed witness expiry timestamp is not valid ISO 8601.")
				: TEXT("The armed witness record has expired."), TEXT("armed.json.expires_at_utc"));
		}

		const TSharedPtr<FJsonObject>* EditorIdentity = nullptr;
		const TSharedPtr<FJsonObject>* WitnessIdentity = nullptr;
		if (!Armed->TryGetObjectField(
				TEXT("editor_process_identity"), EditorIdentity)
			|| !EditorIdentity || !EditorIdentity->IsValid()
			|| !Armed->TryGetObjectField(
				TEXT("witness_process_identity"), WitnessIdentity)
			|| !WitnessIdentity || !WitnessIdentity->IsValid())
		{
			return Fail(
				OutError,
				TEXT("editor_safe_exit_external_witness_process_identity_invalid"),
				TEXT("The armed witness record requires editor and witness process identity objects."),
				!EditorIdentity || !EditorIdentity->IsValid() ? TEXT("armed.json.editor_process_identity") : TEXT("armed.json.witness_process_identity"));
		}
		double EditorPidNumber = 0.0;
		FString EditorExecutable;
		if (!(*EditorIdentity)->TryGetNumberField(
				TEXT("process_id"), EditorPidNumber)
			|| static_cast<uint32>(EditorPidNumber)
				!= FPlatformProcess::GetCurrentProcessId()
			|| !TryRequiredString(
				(*EditorIdentity).ToSharedRef(),
				TEXT("executable_path"),
				EditorExecutable)
			|| !FPaths::GetCleanFilename(EditorExecutable).Equals(
				FPaths::GetCleanFilename(FPlatformProcess::ExecutablePath()),
				ESearchCase::IgnoreCase))
		{
			return Fail(
				OutError,
				TEXT("editor_safe_exit_editor_process_identity_mismatch"), TEXT("The armed witness identifies a different Editor process."),
				static_cast<uint32>(EditorPidNumber) != FPlatformProcess::GetCurrentProcessId()
				? TEXT("armed.json.editor_process_identity.process_id") : TEXT("armed.json.editor_process_identity.executable_path"));
		}

		double WitnessPidNumber = 0.0;
		FString WitnessExecutable;
		if (!(*WitnessIdentity)->TryGetNumberField(
				TEXT("process_id"), WitnessPidNumber)
			|| WitnessPidNumber <= 0.0
			|| static_cast<uint32>(WitnessPidNumber)
				== FPlatformProcess::GetCurrentProcessId()
			|| !TryRequiredString(
				(*WitnessIdentity).ToSharedRef(),
				TEXT("executable_path"),
				WitnessExecutable)
			|| !FPlatformProcess::IsApplicationRunning(
				static_cast<uint32>(WitnessPidNumber))
			|| !FPaths::GetCleanFilename(
				FPlatformProcess::GetApplicationName(
					static_cast<uint32>(WitnessPidNumber))).Equals(
				FPaths::GetCleanFilename(WitnessExecutable),
				ESearchCase::IgnoreCase))
		{
			return Fail(
				OutError,
				TEXT("editor_safe_exit_external_witness_not_alive"),
				WitnessPidNumber <= 0.0 ? TEXT("The armed witness process identifier is missing or non-positive.")
				: static_cast<uint32>(WitnessPidNumber) == FPlatformProcess::GetCurrentProcessId() ? TEXT("The external witness cannot be the Editor process itself.")
				: WitnessExecutable.IsEmpty() ? TEXT("The armed witness executable identity is missing.")
				: TEXT("The external witness process is no longer running with its recorded executable identity."),
				TEXT("armed.json.witness_process_identity"));
		}

		TSharedPtr<FJsonObject> Heartbeat;
		if (!TryReadJsonObject(
				FPaths::Combine(Root, TEXT("heartbeat.json")),
				Heartbeat,
				OutError,
				TEXT("editor_safe_exit_external_witness_heartbeat_missing"),
				TEXT("editor_safe_exit_external_witness_heartbeat_invalid")))
		{
			return false;
		}
		FString HeartbeatSchema;
		const TSharedPtr<FJsonObject>* HeartbeatWitnessIdentity = nullptr;
		double ObservedMilliseconds = 0.0;
		if (!TryRequiredString(
				Heartbeat.ToSharedRef(), TEXT("schema"), HeartbeatSchema)
			|| HeartbeatSchema != TEXT("uai.safe_exit.witness_heartbeat.v1")
			|| !ValidateSharedIdentity(Heartbeat.ToSharedRef(), Ref)
			|| !Heartbeat->TryGetObjectField(
				TEXT("witness_process_identity"), HeartbeatWitnessIdentity)
			|| !HeartbeatWitnessIdentity
			|| !HeartbeatWitnessIdentity->IsValid()
			|| !ExactIdentityMatches(
				(*WitnessIdentity).ToSharedRef(),
				(*HeartbeatWitnessIdentity).ToSharedRef())
			|| !Heartbeat->TryGetNumberField(
				TEXT("observed_at_unix_ms"), ObservedMilliseconds))
		{
			return Fail(
				OutError,
				TEXT("editor_safe_exit_external_witness_heartbeat_invalid"),
				HeartbeatSchema != TEXT("uai.safe_exit.witness_heartbeat.v1") ? TEXT("The witness heartbeat has an unsupported schema.")
				: !ValidateSharedIdentity(Heartbeat.ToSharedRef(), Ref) ? TEXT("The witness heartbeat belongs to a different launch or exit attempt.")
				: !HeartbeatWitnessIdentity || !HeartbeatWitnessIdentity->IsValid() ? TEXT("The heartbeat has no witness process identity object.")
				: !ExactIdentityMatches((*WitnessIdentity).ToSharedRef(), (*HeartbeatWitnessIdentity).ToSharedRef())
				? TEXT("The heartbeat and armed record identify different witness processes.")
				: TEXT("The witness heartbeat requires a numeric observation timestamp."),
				HeartbeatSchema != TEXT("uai.safe_exit.witness_heartbeat.v1") ? TEXT("heartbeat.json.schema")
				: !Heartbeat->HasTypedField<EJson::Number>(TEXT("observed_at_unix_ms")) ? TEXT("heartbeat.json.observed_at_unix_ms")
				: TEXT("heartbeat.json.witness_process_identity"));
		}
		const int64 HeartbeatAge = UtcNowUnixMilliseconds()
			- static_cast<int64>(ObservedMilliseconds);
		if (HeartbeatAge < 0 || HeartbeatAge > WitnessFreshnessMilliseconds)
		{
			Fail(
				OutError,
				TEXT("editor_safe_exit_external_witness_heartbeat_stale"),
				HeartbeatAge < 0 ? TEXT("The witness heartbeat timestamp is in the future.") : TEXT("The witness heartbeat is too old to authorize Editor exit."),
				TEXT("heartbeat.json.observed_at_unix_ms"));
			OutError.ResourceDimension = TEXT("heartbeat_age_ms");
			OutError.ResourceObserved = HeartbeatAge;
			OutError.ExpectedValue = MakeShared<FJsonValueString>(FString::Printf(TEXT("0..%lld ms"), WitnessFreshnessMilliseconds));
			return false;
		}

		OutWitness.LaunchId = Ref.LaunchId;
		OutWitness.ExitAttemptId = Ref.ExitAttemptId;
		OutWitness.Nonce = Ref.Nonce;
		OutWitness.AttemptRoot = Root;
		OutWitness.EditorProcessIdentity = (*EditorIdentity).ToSharedRef();
		return true;
	}

	bool TryCreateDurableSafeExitTarget(
		const FUeAgentValidatedSafeExitWitness& Witness,
		const FUeAgentSafeExitDirtyResolutionReceipt& DirtyResolution,
		const FLongRunningOperationRequest& OperationRequest,
		FString EditorInstanceId,
		TSharedPtr<IUeAgentEditorHostExitTarget, ESPMode::ThreadSafe>& OutTarget,
		FExecutionMutationError& OutError)
	{
		OutTarget.Reset();
		OutError = {};
		if (!DirtyResolution.IsValid())
		{
			return Fail(OutError, TEXT("editor_safe_exit_durable_completion_invalid"),
				TEXT("Safe exit requires a valid dirty-resource resolution with no unresolved resources."), TEXT("dirty_resolution"));
		}
		if (!OperationRequest.IsValid())
		{
			OutError = OperationRequest.GetValidationDiagnostic();
			return false;
		}
		if (EditorInstanceId.TrimStartAndEnd().IsEmpty())
		{
			return Fail(OutError, TEXT("editor_safe_exit_durable_completion_invalid"),
				TEXT("The Editor instance identity is missing."), TEXT("editor_instance"));
		}
		const TSharedRef<FDurableSafeExitTarget, ESPMode::ThreadSafe> Target =
			MakeShared<FDurableSafeExitTarget, ESPMode::ThreadSafe>(
				Witness,
				DirtyResolution,
				OperationRequest,
				MoveTemp(EditorInstanceId));
		Target->Bind();
		OutTarget = Target;
		return true;
	}
}

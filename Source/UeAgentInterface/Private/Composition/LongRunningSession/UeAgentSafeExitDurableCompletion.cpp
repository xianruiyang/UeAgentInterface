#include "Composition/LongRunningSession/UeAgentSafeExitDurableCompletion.h"

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/CoreDelegates.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
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
			FString Detail = FString())
		{
			OutError = FExecutionMutationError(Code, MoveTemp(Detail));
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
				return Fail(OutError, MissingCode, Path);
			}
			const TSharedRef<TJsonReader<>> Reader =
				TJsonReaderFactory<>::Create(Text);
			if (!FJsonSerializer::Deserialize(Reader, OutObject)
				|| !OutObject.IsValid())
			{
				return Fail(OutError, InvalidCode, Path);
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
			const TSharedRef<FJsonObject>& Object)
		{
			IFileManager& FileManager = IFileManager::Get();
			if (FileManager.FileExists(*FinalPath)
				|| !FileManager.MakeDirectory(
					*FPaths::GetPath(FinalPath), true))
			{
				return false;
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
				return false;
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
				return false;
			}
			if (!FileManager.Move(
					*FinalPath,
					*TemporaryPath,
					false,
					true))
			{
				FileManager.Delete(*TemporaryPath, false, true);
				return false;
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
					bExitRequested = true;
				}
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
				bPublished = TryWriteJsonImmutable(
					FPaths::Combine(
						Witness.GetAttemptRoot(),
						TEXT("pre_exit_committed.json")),
					Root);
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
		return IsLowerHex(LaunchId, 32)
			&& IsLowerHex(ExitAttemptId, 32)
			&& IsLowerHex(Nonce, 64);
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
		if (!Ref.IsValid())
		{
			return Fail(
				OutError,
				TEXT("editor_safe_exit_external_witness_ref_invalid"));
		}
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
				TEXT("editor_safe_exit_external_witness_identity_mismatch"));
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
				TEXT("editor_safe_exit_external_witness_expired"));
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
				TEXT("editor_safe_exit_external_witness_process_identity_invalid"));
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
				TEXT("editor_safe_exit_editor_process_identity_mismatch"));
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
				TEXT("editor_safe_exit_external_witness_not_alive"));
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
				TEXT("editor_safe_exit_external_witness_heartbeat_invalid"));
		}
		const int64 HeartbeatAge = UtcNowUnixMilliseconds()
			- static_cast<int64>(ObservedMilliseconds);
		if (HeartbeatAge < 0 || HeartbeatAge > WitnessFreshnessMilliseconds)
		{
			return Fail(
				OutError,
				TEXT("editor_safe_exit_external_witness_heartbeat_stale"));
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
		if (!DirtyResolution.IsValid()
			|| !OperationRequest.IsValid()
			|| EditorInstanceId.TrimStartAndEnd().IsEmpty())
		{
			return Fail(
				OutError,
				TEXT("editor_safe_exit_durable_completion_invalid"));
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

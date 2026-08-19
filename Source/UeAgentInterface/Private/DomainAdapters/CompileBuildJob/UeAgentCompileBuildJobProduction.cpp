#include "DomainAdapters/CompileBuildJob/UeAgentCompileBuildJobProduction.h"

#include "Async/Async.h"
#include "HAL/PlatformProcess.h"
#include "Misc/ScopeLock.h"

namespace UE::UAI::DomainAdapters::CompileBuildJob
{
	namespace
	{
		using namespace ContextResource;
		using namespace Contract;
		using namespace ExecutionMutation;
		using namespace StateSemantic;

		bool Fail(
			FExecutionMutationError& OutError,
			FString Code,
			FString Detail = FString())
		{
			OutError = FExecutionMutationError(
				MoveTemp(Code), MoveTemp(Detail));
			return false;
		}

		bool IsNormalizedNonEmpty(const FString& Value)
		{
			FString Normalized = Value;
			Normalized.TrimStartAndEndInline();
			return !Normalized.IsEmpty() && Normalized == Value;
		}

		bool MatchesPolicyKind(
			const EUeAgentCompileBuildJobFamily Family,
			const FString& PolicyId)
		{
			switch (Family)
			{
			case EUeAgentCompileBuildJobFamily::AssetCompile:
				return PolicyId.StartsWith(TEXT("policy:asset-compile:"));
			case EUeAgentCompileBuildJobFamily::WorldBuild:
				return PolicyId.StartsWith(TEXT("policy:world-build:"));
			case EUeAgentCompileBuildJobFamily::ExternalProcess:
				return PolicyId.StartsWith(TEXT("policy:owned-tool:"));
			case EUeAgentCompileBuildJobFamily::WorldPartitionResave:
				return PolicyId.StartsWith(
					TEXT("policy:world-partition-resave:"));
			case EUeAgentCompileBuildJobFamily::Packaging:
				return PolicyId.StartsWith(TEXT("policy:packaging:"));
			default:
				return false;
			}
		}

		bool IsSupportedMechanism(const FString& MechanismId)
		{
			return MechanismId == TEXT("game_thread_stage")
				|| MechanismId == TEXT("owned_process");
		}

		int64 NextGeneration()
		{
			static FCriticalSection Mutex;
			static int64 Generation = 0;
			FScopeLock Lock(&Mutex);
			return ++Generation;
		}

		bool MatchesPolicy(
			const FUeAgentCompileBuildJobTypedRequest& Request,
			const FUeAgentCompileBuildJobPolicyDescriptor& Policy)
		{
			return Request.GetFamily() == Policy.GetFamily()
				&& Request.GetPolicyId() == Policy.GetPolicyId();
		}

		bool ValidateOperationRequest(
			const FLongRunningOperationRequest& Request,
			FExecutionMutationError& OutError)
		{
			if (!Request.IsValid())
			{
				return Fail(
					OutError,
					TEXT("compile_build_job_operation_request_invalid"));
			}
			if (Request.GetExecutionContext().GetStatePlane()
				!= EUeAgentStatePlane::Persisted)
			{
				return Fail(
					OutError,
					TEXT("compile_build_job_context_not_persisted"));
			}
			return true;
		}

		bool ValidateCallIdentity(
			const FString& ExpectedOperationId,
			const FLongRunningWorkIdentity& ExpectedIdentity,
			const FString& OperationId,
			const FLongRunningWorkIdentity& Identity,
			FExecutionMutationError& OutError)
		{
			if (ExpectedOperationId != OperationId)
			{
				return Fail(
					OutError,
					TEXT("compile_build_job_operation_id_mismatch"));
			}
			if (!(ExpectedIdentity == Identity))
			{
				return Fail(
					OutError,
					TEXT("compile_build_job_work_identity_mismatch"));
			}
			return true;
		}

		bool TryBuildSignal(
			const FUeAgentCompileBuildJobTypedRequest& Request,
			const FUeAgentCompileBuildJobPolicyDescriptor& Policy,
			const FString& Phase,
			const FString& ReportRef,
			const int64 Revision,
			FUeAgentResourceTracker& Resources,
			TOptional<FLongRunningOperationSignal>& OutSignal,
			FExecutionMutationError& OutError)
		{
			const FUeAgentCompileBuildJobFamilyContract* Contract =
				FindCompileBuildJobFamilyContract(Request.GetFamily());
			if (Contract == nullptr)
			{
				return Fail(
					OutError,
					TEXT("compile_build_job_family_invalid"));
			}

			FUeAgentStateSemanticError StateError;
			TArray<TPair<FString, FUeAgentCanonicalValueRef>> Items;
			auto AddString = [&](const FString& Key, const FString& Value)
			{
				FUeAgentCanonicalValuePtr Canonical;
				if (!FUeAgentCanonicalValue::TryString(
						Value, Resources, Canonical, StateError))
				{
					return false;
				}
				Items.Emplace(Key, Canonical.ToSharedRef());
				return true;
			};
			if (!AddString(TEXT("phase"), Phase)
				|| !AddString(TEXT("subject_ref"), Request.GetSubjectRef())
				|| !AddString(TEXT("policy_id"), Policy.GetPolicyId())
				|| !AddString(
					TEXT("readback_oracle_id"),
					Policy.GetReadbackOracleId())
				|| (!ReportRef.IsEmpty()
					&& !AddString(TEXT("report_ref"), ReportRef)))
			{
				return Fail(
					OutError,
					TEXT("compile_build_job_signal_value_failed"),
					StateError.Code);
			}

			FUeAgentCanonicalValuePtr Payload;
			if (!FUeAgentCanonicalValue::TryObject(
					Items, Resources, Payload, StateError))
			{
				return Fail(
					OutError,
					TEXT("compile_build_job_signal_payload_failed"),
					StateError.Code);
			}
			OutSignal.Emplace(
				Contract->GetSignalTypeId(),
				Revision,
				Payload.ToSharedRef());
			return true;
		}

		enum class EGameThreadStageState : uint8
		{
			Queued,
			Executing,
			ExitedSuccess,
			ExitedFailure,
		};

		struct FGameThreadStageSharedState final
		{
			FGameThreadStageSharedState(
				FUeAgentCompileBuildJobTypedRequest InRequest,
				FUeAgentCompileBuildJobPolicyDescriptor InPolicy,
				TSharedRef<
					IUeAgentCompileBuildJobGameThreadStage,
					ESPMode::ThreadSafe> InStage,
				FUeAgentCancellationToken InCancellation,
				FString InOperationId,
				FLongRunningWorkIdentity InIdentity)
				: Request(MoveTemp(InRequest))
				, Policy(MoveTemp(InPolicy))
				, Stage(MoveTemp(InStage))
				, Cancellation(MoveTemp(InCancellation))
				, OperationId(MoveTemp(InOperationId))
				, Identity(MoveTemp(InIdentity))
			{
			}

			FCriticalSection Mutex;
			FUeAgentCompileBuildJobTypedRequest Request;
			FUeAgentCompileBuildJobPolicyDescriptor Policy;
			TSharedRef<IUeAgentCompileBuildJobGameThreadStage,
				ESPMode::ThreadSafe> Stage;
			FUeAgentCancellationToken Cancellation;
			FString OperationId;
			FLongRunningWorkIdentity Identity;
			EGameThreadStageState State = EGameThreadStageState::Queued;
			FString ErrorCode;
			bool bStopRequested = false;
			bool bJoined = false;
			bool bReleased = false;
			FString LastSignalPhase;
			int64 SignalRevision = 0;
		};

		class FGameThreadStagePort final
			: public ILongRunningOperationPort
		{
		public:
			FGameThreadStagePort(
				FUeAgentCompileBuildJobTypedRequest InRequest,
				FUeAgentCompileBuildJobPolicyDescriptor InPolicy,
				TSharedRef<
					IUeAgentCompileBuildJobGameThreadStage,
					ESPMode::ThreadSafe> InStage)
				: Request(MoveTemp(InRequest))
				, Policy(MoveTemp(InPolicy))
				, Stage(MoveTemp(InStage))
			{
			}

			bool Start(
				const FLongRunningOperationRequest& OperationRequest,
				const FUeAgentCancellationToken& Cancellation,
				FLongRunningWorkIdentity& OutIdentity,
				FExecutionMutationError& OutError) override
			{
				if (State.IsValid())
				{
					return Fail(
						OutError,
						TEXT("compile_build_job_already_started"));
				}
				if (!ValidateOperationRequest(OperationRequest, OutError))
				{
					return false;
				}
				OutIdentity = FLongRunningWorkIdentity(
					TEXT("native:compile-build:stage:")
						+ OperationRequest.GetOperationId(),
					NextGeneration());
				State = MakeShared<
					FGameThreadStageSharedState,
					ESPMode::ThreadSafe>(
					Request,
					Policy,
					Stage,
					Cancellation,
					OperationRequest.GetOperationId(),
					OutIdentity);
				const TSharedRef<
					FGameThreadStageSharedState,
					ESPMode::ThreadSafe> ScheduledState = State.ToSharedRef();
				AsyncTask(
					ENamedThreads::GameThread,
					[ScheduledState]()
					{
						{
							FScopeLock Lock(&ScheduledState->Mutex);
							if (ScheduledState->bStopRequested
								|| ScheduledState->Cancellation.IsCancelled())
							{
								ScheduledState->State =
									EGameThreadStageState::ExitedFailure;
								ScheduledState->ErrorCode =
									TEXT("compile_build_job_cancelled");
								return;
							}
							ScheduledState->State =
								EGameThreadStageState::Executing;
						}
						FExecutionMutationError Error;
						const bool bSucceeded = ScheduledState->Stage->Execute(
							ScheduledState->Cancellation, Error);
						FScopeLock Lock(&ScheduledState->Mutex);
						ScheduledState->State = bSucceeded
							? EGameThreadStageState::ExitedSuccess
							: EGameThreadStageState::ExitedFailure;
						ScheduledState->ErrorCode = bSucceeded
							? FString()
							: (Error.IsPresent()
								? Error.GetCode()
								: TEXT("compile_build_job_stage_failed"));
					});
				return true;
			}

			bool Observe(
				const FString& OperationId,
				const FLongRunningWorkIdentity& Identity,
				FUeAgentResourceTracker& Resources,
				FLongRunningWorkObservation& OutObservation,
				FExecutionMutationError& OutError) override
			{
				if (!State.IsValid())
				{
					return Fail(
						OutError,
						TEXT("compile_build_job_not_started"));
				}
				FScopeLock Lock(&State->Mutex);
				if (!ValidateCallIdentity(
						State->OperationId,
						State->Identity,
						OperationId,
						Identity,
						OutError))
				{
					return false;
				}

				ELongRunningWorkState WorkState =
					ELongRunningWorkState::Running;
				FString Phase = State->bStopRequested
					? TEXT("stop_requested")
					: TEXT("running");
				if (State->State == EGameThreadStageState::ExitedSuccess)
				{
					WorkState = ELongRunningWorkState::ExitedSuccess;
					Phase = TEXT("succeeded");
				}
				else if (State->State
					== EGameThreadStageState::ExitedFailure)
				{
					WorkState = ELongRunningWorkState::ExitedFailure;
					Phase = TEXT("failed");
				}
				TOptional<FLongRunningOperationSignal> Signal;
				if (State->LastSignalPhase != Phase)
				{
					const int64 NextRevision = State->SignalRevision + 1;
					if (!TryBuildSignal(
							State->Request,
							State->Policy,
							Phase,
							State->Stage->GetReportRef(),
							NextRevision,
							Resources,
							Signal,
							OutError))
					{
						return false;
					}
					State->LastSignalPhase = Phase;
					State->SignalRevision = NextRevision;
				}
				OutObservation = FLongRunningWorkObservation(
					Identity.GetGeneration(),
					WorkState,
					State->ErrorCode,
					MoveTemp(Signal));
				return true;
			}

			bool RequestStop(
				const FString& OperationId,
				const FLongRunningWorkIdentity& Identity,
				const FString& Reason,
				FExecutionMutationError& OutError) override
			{
				if (!State.IsValid())
				{
					return Fail(
						OutError,
						TEXT("compile_build_job_not_started"));
				}
				{
					FScopeLock Lock(&State->Mutex);
					if (!ValidateCallIdentity(
							State->OperationId,
							State->Identity,
							OperationId,
							Identity,
							OutError))
					{
						return false;
					}
					State->bStopRequested = true;
				}
				State->Stage->RequestStop(Reason);
				return true;
			}

			bool Join(
				const FString& OperationId,
				const FLongRunningWorkIdentity& Identity,
				FExecutionMutationError& OutError) override
			{
				if (!State.IsValid())
				{
					return Fail(
						OutError,
						TEXT("compile_build_job_not_started"));
				}
				FScopeLock Lock(&State->Mutex);
				if (!ValidateCallIdentity(
						State->OperationId,
						State->Identity,
						OperationId,
						Identity,
						OutError))
				{
					return false;
				}
				if (State->State != EGameThreadStageState::ExitedSuccess
					&& State->State != EGameThreadStageState::ExitedFailure)
				{
					return Fail(
						OutError,
						TEXT("compile_build_job_join_before_exit"));
				}
				State->bJoined = true;
				return true;
			}

			bool Release(
				const FString& OperationId,
				const FLongRunningWorkIdentity& Identity,
				FExecutionMutationError& OutError) override
			{
				if (!State.IsValid())
				{
					return Fail(
						OutError,
						TEXT("compile_build_job_not_started"));
				}
				FScopeLock Lock(&State->Mutex);
				if (!ValidateCallIdentity(
						State->OperationId,
						State->Identity,
						OperationId,
						Identity,
						OutError))
				{
					return false;
				}
				if (!State->bJoined)
				{
					return Fail(
						OutError,
						TEXT("compile_build_job_release_before_join"));
				}
				if (State->bReleased)
				{
					return Fail(
						OutError,
						TEXT("compile_build_job_already_released"));
				}
				State->bReleased = true;
				return true;
			}

		private:
			FUeAgentCompileBuildJobTypedRequest Request;
			FUeAgentCompileBuildJobPolicyDescriptor Policy;
			TSharedRef<IUeAgentCompileBuildJobGameThreadStage,
				ESPMode::ThreadSafe> Stage;
			TSharedPtr<FGameThreadStageSharedState, ESPMode::ThreadSafe>
				State;
		};

		class FOwnedProcessPort final : public ILongRunningOperationPort
		{
		public:
			FOwnedProcessPort(
				FUeAgentCompileBuildJobTypedRequest InRequest,
				FUeAgentCompileBuildJobPolicyDescriptor InPolicy,
				FUeAgentCompileBuildJobProcessLaunchSpec InLaunchSpec,
				TSharedRef<
					IUeAgentCompileBuildJobProcessOutputSink,
					ESPMode::ThreadSafe> InOutputSink)
				: Request(MoveTemp(InRequest))
				, Policy(MoveTemp(InPolicy))
				, LaunchSpec(MoveTemp(InLaunchSpec))
				, OutputSink(MoveTemp(InOutputSink))
			{
			}

			~FOwnedProcessPort() override
			{
				FScopeLock Lock(&Mutex);
				CleanupOwnedResources(true);
			}

			bool Start(
				const FLongRunningOperationRequest& OperationRequest,
				const FUeAgentCancellationToken& Cancellation,
				FLongRunningWorkIdentity& OutIdentity,
				FExecutionMutationError& OutError) override
			{
				FScopeLock Lock(&Mutex);
				if (bStarted)
				{
					return Fail(
						OutError,
						TEXT("compile_build_job_already_started"));
				}
				if (!ValidateOperationRequest(OperationRequest, OutError))
				{
					return false;
				}
				if (Cancellation.IsCancelled())
				{
					return Fail(
						OutError,
						TEXT("compile_build_job_cancelled_before_start"));
				}
				if (!FPlatformProcess::CreatePipe(ReadPipe, WritePipe))
				{
					return Fail(
						OutError,
						TEXT("compile_build_job_process_pipe_failed"));
				}
				uint32 ProcessId = 0;
				ProcessHandle = FPlatformProcess::CreateProc(
					*LaunchSpec.GetExecutable(),
					*LaunchSpec.GetArguments(),
					false,
					true,
					true,
					&ProcessId,
					0,
					*LaunchSpec.GetWorkingDirectory(),
					WritePipe,
					ReadPipe);
				if (!ProcessHandle.IsValid())
				{
					FPlatformProcess::ClosePipe(ReadPipe, WritePipe);
					ReadPipe = nullptr;
					WritePipe = nullptr;
					return Fail(
						OutError,
						TEXT("compile_build_job_process_launch_failed"));
				}
				OperationId = OperationRequest.GetOperationId();
				Identity = FLongRunningWorkIdentity(
					TEXT("native:compile-build:process:") + OperationId,
					NextGeneration());
				OutIdentity = Identity;
				bStarted = true;
				StartedAtSeconds = FPlatformTime::Seconds();
				return true;
			}

			bool Observe(
				const FString& InOperationId,
				const FLongRunningWorkIdentity& InIdentity,
				FUeAgentResourceTracker& Resources,
				FLongRunningWorkObservation& OutObservation,
				FExecutionMutationError& OutError) override
			{
				FScopeLock Lock(&Mutex);
				if (!ValidateStartedCall(
						InOperationId, InIdentity, OutError))
				{
					return false;
				}
				if (!DrainOutput(OutError))
				{
					return false;
				}
				if (!bObservedExit
					&& LaunchSpec.GetTimeoutSeconds() > 0.0
					&& FPlatformProcess::IsProcRunning(ProcessHandle)
					&& (FPlatformTime::Seconds() - StartedAtSeconds)
						>= LaunchSpec.GetTimeoutSeconds())
				{
					FPlatformProcess::TerminateProc(ProcessHandle, true);
					bObservedExit = true;
					bExitedSuccess = false;
					bStopRequested = true;
					ErrorCode = TEXT("compile_build_job_process_timeout");
				}
				if (!bObservedExit
					&& !FPlatformProcess::IsProcRunning(ProcessHandle))
				{
					if (!DrainOutput(OutError))
					{
						return false;
					}
					else
					{
						int32 ReturnCode = 0;
						if (!FPlatformProcess::GetProcReturnCode(
								ProcessHandle, &ReturnCode))
						{
							bStateUnknown = true;
							ErrorCode =
								TEXT("compile_build_job_process_exit_unknown");
						}
						else
						{
							if (!OutputSink->Append(
									FString::Printf(
										TEXT("\nUAI_PROCESS_RETURN_CODE=%d\n"),
										ReturnCode),
									OutError))
							{
								return false;
							}
							bExitedSuccess = ReturnCode == 0;
							ErrorCode = bExitedSuccess
								? FString()
								: TEXT("compile_build_job_process_failed");
						}
					}
					bObservedExit = true;
				}

				ELongRunningWorkState WorkState =
					ELongRunningWorkState::Running;
				FString Phase = bStopRequested
					? TEXT("stop_requested")
					: TEXT("running");
				if (bObservedExit)
				{
					if (bStateUnknown)
					{
						WorkState = ELongRunningWorkState::StateUnknown;
						Phase = TEXT("state_unknown");
					}
					else if (bExitedSuccess)
					{
						WorkState = ELongRunningWorkState::ExitedSuccess;
						Phase = TEXT("succeeded");
					}
					else
					{
						WorkState = ELongRunningWorkState::ExitedFailure;
						Phase = TEXT("failed");
					}
				}
				TOptional<FLongRunningOperationSignal> Signal;
				if (LastSignalPhase != Phase)
				{
					const int64 NextRevision = SignalRevision + 1;
					if (!TryBuildSignal(
							Request,
							Policy,
							Phase,
							OutputSink->GetReportRef(),
							NextRevision,
							Resources,
							Signal,
							OutError))
					{
						return false;
					}
					LastSignalPhase = Phase;
					SignalRevision = NextRevision;
				}
				OutObservation = FLongRunningWorkObservation(
					Identity.GetGeneration(),
					WorkState,
					ErrorCode,
					MoveTemp(Signal));
				return true;
			}

			bool RequestStop(
				const FString& InOperationId,
				const FLongRunningWorkIdentity& InIdentity,
				const FString& Reason,
				FExecutionMutationError& OutError) override
			{
				FScopeLock Lock(&Mutex);
				if (!ValidateStartedCall(
						InOperationId, InIdentity, OutError))
				{
					return false;
				}
				bStopRequested = true;
				if (!bObservedExit
					&& FPlatformProcess::IsProcRunning(ProcessHandle))
				{
					FPlatformProcess::TerminateProc(ProcessHandle, true);
				}
				return true;
			}

			bool Join(
				const FString& InOperationId,
				const FLongRunningWorkIdentity& InIdentity,
				FExecutionMutationError& OutError) override
			{
				FScopeLock Lock(&Mutex);
				if (!ValidateStartedCall(
						InOperationId, InIdentity, OutError))
				{
					return false;
				}
				if (!bObservedExit)
				{
					return Fail(
						OutError,
						TEXT("compile_build_job_join_before_exit"));
				}
				bJoined = true;
				return true;
			}

			bool Release(
				const FString& InOperationId,
				const FLongRunningWorkIdentity& InIdentity,
				FExecutionMutationError& OutError) override
			{
				FScopeLock Lock(&Mutex);
				if (!ValidateStartedCall(
						InOperationId, InIdentity, OutError))
				{
					return false;
				}
				if (!bJoined)
				{
					return Fail(
						OutError,
						TEXT("compile_build_job_release_before_join"));
				}
				if (bReleased)
				{
					return Fail(
						OutError,
						TEXT("compile_build_job_already_released"));
				}
				if (!OutputSink->Release(OutError))
				{
					return false;
				}
				bSinkReleased = true;
				CleanupOwnedResources(false);
				bReleased = true;
				return true;
			}

		private:
			bool ValidateStartedCall(
				const FString& InOperationId,
				const FLongRunningWorkIdentity& InIdentity,
				FExecutionMutationError& OutError) const
			{
				if (!bStarted)
				{
					return Fail(
						OutError,
						TEXT("compile_build_job_not_started"));
				}
				return ValidateCallIdentity(
					OperationId,
					Identity,
					InOperationId,
					InIdentity,
					OutError);
			}

			bool DrainOutput(FExecutionMutationError& OutError)
			{
				if (ReadPipe == nullptr)
				{
					return true;
				}
				const FString Chunk = FPlatformProcess::ReadPipe(ReadPipe);
				return Chunk.IsEmpty()
					? true
					: OutputSink->Append(Chunk, OutError);
			}

			void CleanupOwnedResources(const bool bTerminateIfRunning)
			{
				if (ProcessHandle.IsValid())
				{
					if (bTerminateIfRunning
						&& FPlatformProcess::IsProcRunning(ProcessHandle))
					{
						FPlatformProcess::TerminateProc(ProcessHandle, true);
					}
					FPlatformProcess::CloseProc(ProcessHandle);
				}
				if (ReadPipe != nullptr || WritePipe != nullptr)
				{
					FPlatformProcess::ClosePipe(ReadPipe, WritePipe);
					ReadPipe = nullptr;
					WritePipe = nullptr;
				}
				if (!bSinkReleased)
				{
					FExecutionMutationError Ignored;
					OutputSink->Release(Ignored);
					bSinkReleased = true;
				}
			}

			mutable FCriticalSection Mutex;
			FUeAgentCompileBuildJobTypedRequest Request;
			FUeAgentCompileBuildJobPolicyDescriptor Policy;
			FUeAgentCompileBuildJobProcessLaunchSpec LaunchSpec;
			TSharedRef<IUeAgentCompileBuildJobProcessOutputSink,
				ESPMode::ThreadSafe> OutputSink;
			FString OperationId;
			FLongRunningWorkIdentity Identity;
			FProcHandle ProcessHandle;
			void* ReadPipe = nullptr;
			void* WritePipe = nullptr;
			bool bStarted = false;
			bool bStopRequested = false;
			bool bObservedExit = false;
			bool bExitedSuccess = false;
			bool bStateUnknown = false;
			bool bJoined = false;
			bool bReleased = false;
			bool bSinkReleased = false;
			FString ErrorCode;
			FString LastSignalPhase;
			int64 SignalRevision = 0;
			double StartedAtSeconds = 0.0;
		};
	}

	FUeAgentCompileBuildJobPolicyDescriptor::
	FUeAgentCompileBuildJobPolicyDescriptor(
		const EUeAgentCompileBuildJobFamily InFamily,
		FString InPolicyId,
		FString InNativeMechanismId,
		FString InReadbackOracleId)
		: Family(InFamily)
		, PolicyId(MoveTemp(InPolicyId))
		, NativeMechanismId(MoveTemp(InNativeMechanismId))
		, ReadbackOracleId(MoveTemp(InReadbackOracleId))
	{
	}

	EUeAgentCompileBuildJobFamily
	FUeAgentCompileBuildJobPolicyDescriptor::GetFamily() const
	{
		return Family;
	}

	const FString&
	FUeAgentCompileBuildJobPolicyDescriptor::GetPolicyId() const
	{
		return PolicyId;
	}

	const FString&
	FUeAgentCompileBuildJobPolicyDescriptor::GetNativeMechanismId() const
	{
		return NativeMechanismId;
	}

	const FString&
	FUeAgentCompileBuildJobPolicyDescriptor::GetReadbackOracleId() const
	{
		return ReadbackOracleId;
	}

	bool FUeAgentCompileBuildJobPolicyDescriptor::IsValid() const
	{
		return FindCompileBuildJobFamilyContract(Family) != nullptr
			&& IsNormalizedNonEmpty(PolicyId)
			&& MatchesPolicyKind(Family, PolicyId)
			&& IsSupportedMechanism(NativeMechanismId)
			&& IsNormalizedNonEmpty(ReadbackOracleId)
			&& ReadbackOracleId.StartsWith(TEXT("uai.readback."));
	}

	FUeAgentCompileBuildJobPolicyRegistration::
	FUeAgentCompileBuildJobPolicyRegistration(
		FUeAgentCompileBuildJobPolicyDescriptor InDescriptor,
		TSharedRef<IUeAgentCompileBuildJobPortBuilder, ESPMode::ThreadSafe>
			InBuilder)
		: Descriptor(MoveTemp(InDescriptor))
		, Builder(MoveTemp(InBuilder))
	{
	}

	const FUeAgentCompileBuildJobPolicyDescriptor&
	FUeAgentCompileBuildJobPolicyRegistration::GetDescriptor() const
	{
		return Descriptor;
	}

	IUeAgentCompileBuildJobPortBuilder&
	FUeAgentCompileBuildJobPolicyRegistration::GetBuilder() const
	{
		return Builder.Get();
	}

	FUeAgentCompileBuildJobProductionPortFactory::
	FUeAgentCompileBuildJobProductionPortFactory(
		TArray<FUeAgentCompileBuildJobPolicyRegistration> InRegistrations)
		: Registrations(MoveTemp(InRegistrations))
	{
		Descriptors.Reserve(Registrations.Num());
		for (const FUeAgentCompileBuildJobPolicyRegistration& Registration
			: Registrations)
		{
			Descriptors.Add(Registration.GetDescriptor());
		}
	}

	bool FUeAgentCompileBuildJobProductionPortFactory::TryCreate(
		const TArray<FUeAgentCompileBuildJobPolicyRegistration>&
			InRegistrations,
		TSharedPtr<FUeAgentCompileBuildJobProductionPortFactory>& OutFactory,
		FExecutionMutationError& OutError)
	{
		OutFactory.Reset();
		if (InRegistrations.IsEmpty())
		{
			return Fail(
				OutError,
				TEXT("compile_build_job_policy_catalog_empty"));
		}
		for (int32 Index = 0; Index < InRegistrations.Num(); ++Index)
		{
			const FUeAgentCompileBuildJobPolicyDescriptor& Descriptor =
				InRegistrations[Index].GetDescriptor();
			if (!Descriptor.IsValid())
			{
				return Fail(
					OutError,
					TEXT("compile_build_job_policy_invalid"),
					Descriptor.GetPolicyId());
			}
			for (int32 Other = 0; Other < Index; ++Other)
			{
				const FUeAgentCompileBuildJobPolicyDescriptor& Existing =
					InRegistrations[Other].GetDescriptor();
				if (Existing.GetFamily() == Descriptor.GetFamily()
					&& Existing.GetPolicyId() == Descriptor.GetPolicyId())
				{
					return Fail(
						OutError,
						TEXT("compile_build_job_policy_duplicate"),
						Descriptor.GetPolicyId());
				}
			}
		}
		OutFactory = MakeShareable(
			new FUeAgentCompileBuildJobProductionPortFactory(
				InRegistrations));
		return true;
	}

	const TArray<FUeAgentCompileBuildJobPolicyDescriptor>&
	FUeAgentCompileBuildJobProductionPortFactory::
	GetPolicyDescriptors() const
	{
		return Descriptors;
	}

	bool FUeAgentCompileBuildJobProductionPortFactory::
	TryCreateOperationPort(
		const FUeAgentCompileBuildJobTypedRequest& Request,
		TSharedPtr<ILongRunningOperationPort, ESPMode::ThreadSafe>& OutPort,
		FExecutionMutationError& OutError)
	{
		OutPort.Reset();
		const FUeAgentCompileBuildJobPolicyRegistration* Registration =
			Registrations.FindByPredicate(
				[&Request](
					const FUeAgentCompileBuildJobPolicyRegistration& Candidate)
				{
					return MatchesPolicy(
						Request, Candidate.GetDescriptor());
				});
		if (Registration == nullptr)
		{
			return Fail(
				OutError,
				TEXT("compile_build_job_policy_not_registered"),
				Request.GetPolicyId());
		}
		if (!Request.IsValid())
		{
			return Fail(
				OutError,
				TEXT("compile_build_job_request_invalid"));
		}
		if (!Registration->GetBuilder().TryCreatePort(
				Request,
				Registration->GetDescriptor(),
				OutPort,
				OutError))
		{
			return false;
		}
		return OutPort.IsValid()
			? true
			: Fail(
				OutError,
				TEXT("compile_build_job_operation_port_missing"));
	}

	bool TryCreateCompileBuildJobGameThreadStagePort(
		const FUeAgentCompileBuildJobTypedRequest& Request,
		const FUeAgentCompileBuildJobPolicyDescriptor& Policy,
		TSharedRef<IUeAgentCompileBuildJobGameThreadStage, ESPMode::ThreadSafe>
			Stage,
		TSharedPtr<ILongRunningOperationPort, ESPMode::ThreadSafe>& OutPort,
		FExecutionMutationError& OutError)
	{
		OutPort.Reset();
		if (!Request.IsValid() || !Policy.IsValid()
			|| !MatchesPolicy(Request, Policy))
		{
			return Fail(
				OutError,
				TEXT("compile_build_job_stage_contract_invalid"));
		}
		if (Policy.GetNativeMechanismId() != TEXT("game_thread_stage"))
		{
			return Fail(
				OutError,
				TEXT("compile_build_job_native_mechanism_mismatch"));
		}
		OutPort = MakeShared<FGameThreadStagePort, ESPMode::ThreadSafe>(
			Request, Policy, MoveTemp(Stage));
		return true;
	}

	FUeAgentCompileBuildJobProcessLaunchSpec::
	FUeAgentCompileBuildJobProcessLaunchSpec(
		FString InExecutable,
		FString InArguments,
		FString InWorkingDirectory,
		const double InTimeoutSeconds)
		: Executable(MoveTemp(InExecutable))
		, Arguments(MoveTemp(InArguments))
		, WorkingDirectory(MoveTemp(InWorkingDirectory))
		, TimeoutSeconds(InTimeoutSeconds)
	{
	}

	const FString&
	FUeAgentCompileBuildJobProcessLaunchSpec::GetExecutable() const
	{
		return Executable;
	}

	const FString&
	FUeAgentCompileBuildJobProcessLaunchSpec::GetArguments() const
	{
		return Arguments;
	}

	const FString&
	FUeAgentCompileBuildJobProcessLaunchSpec::GetWorkingDirectory() const
	{
		return WorkingDirectory;
	}

	double FUeAgentCompileBuildJobProcessLaunchSpec::GetTimeoutSeconds() const
	{
		return TimeoutSeconds;
	}

	bool FUeAgentCompileBuildJobProcessLaunchSpec::IsValid() const
	{
		return IsNormalizedNonEmpty(Executable)
			&& IsNormalizedNonEmpty(WorkingDirectory)
			&& FMath::IsFinite(TimeoutSeconds)
			&& TimeoutSeconds >= 0.0;
	}

	bool TryCreateCompileBuildJobOwnedProcessPort(
		const FUeAgentCompileBuildJobTypedRequest& Request,
		const FUeAgentCompileBuildJobPolicyDescriptor& Policy,
		const FUeAgentCompileBuildJobProcessLaunchSpec& LaunchSpec,
		TSharedRef<
			IUeAgentCompileBuildJobProcessOutputSink,
			ESPMode::ThreadSafe> OutputSink,
		TSharedPtr<ILongRunningOperationPort, ESPMode::ThreadSafe>& OutPort,
		FExecutionMutationError& OutError)
	{
		OutPort.Reset();
		if (!Request.IsValid() || !Policy.IsValid()
			|| !MatchesPolicy(Request, Policy)
			|| !LaunchSpec.IsValid())
		{
			return Fail(
				OutError,
				TEXT("compile_build_job_process_contract_invalid"));
		}
		if (Policy.GetNativeMechanismId() != TEXT("owned_process"))
		{
			return Fail(
				OutError,
				TEXT("compile_build_job_native_mechanism_mismatch"));
		}
		OutPort = MakeShared<FOwnedProcessPort, ESPMode::ThreadSafe>(
			Request, Policy, LaunchSpec, MoveTemp(OutputSink));
		return true;
	}
}

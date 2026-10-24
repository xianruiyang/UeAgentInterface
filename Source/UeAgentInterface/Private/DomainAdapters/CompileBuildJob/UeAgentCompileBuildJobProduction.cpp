#include "DomainAdapters/CompileBuildJob/UeAgentCompileBuildJobProduction.h"

#include "Async/Async.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Core/ArtifactPublisher/UeAgentFileArtifactPublisher.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"

namespace UE::UAI::DomainAdapters::CompileBuildJob
{
	namespace
	{
		using namespace ContextResource;
		using namespace Contract;
		using namespace ExecutionMutation;
		using namespace StateSemantic;

		FExecutionMutationError JobFact(const TCHAR* Code, const FString& Reason,
			const FString& Path, TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected,
			const FUeAgentCompileBuildJobTypedRequest* Request = nullptr,
			const TCHAR* Stage = TEXT("native_execution"))
		{
			auto Error = FExecutionMutationError::Failure(Code, Reason, Stage);
			Error.PropertyPath = Path;
			Error.SemanticPointer = Path;
			Error.SourceKind = TEXT("native_result");
			Error.ActualValue = MoveTemp(Actual);
			Error.ExpectedValue = MoveTemp(Expected);
			if (Request)
			{
				Error.TargetPath = Request->GetSubjectRef();
				Error.CandidateName = Request->GetPolicyId();
			}
			return Error;
		}

		void SetRequestContext(FExecutionMutationError& Error, const FUeAgentCompileBuildJobTypedRequest& Request)
		{
			if (Error.TargetPath.IsEmpty()) Error.TargetPath = Request.GetSubjectRef();
			if (Error.CandidateName.IsEmpty()) Error.CandidateName = Request.GetPolicyId();
		}

		TSharedPtr<FJsonValue> FamilyValue(EUeAgentCompileBuildJobFamily Family)
		{
			if (const auto* Contract = FindCompileBuildJobFamilyContract(Family))
				return MakeShared<FJsonValueString>(Contract->GetFamilyId());
			return MakeShared<FJsonValueNumber>(static_cast<int32>(Family));
		}

		TSharedPtr<FJsonValue> PolicyPair(EUeAgentCompileBuildJobFamily Family, const FString& PolicyId)
		{
			auto Pair = MakeShared<FJsonObject>();
			Pair->SetField(TEXT("family"), FamilyValue(Family));
			Pair->SetStringField(TEXT("policy_id"), PolicyId);
			return MakeShared<FJsonValueObject>(Pair);
		}

		TSharedPtr<FJsonValue> FamilyChoices()
		{
			TArray<TSharedPtr<FJsonValue>> Items;
			for (const auto& Contract : GetCompileBuildJobFamilyContracts())
				Items.Add(MakeShared<FJsonValueString>(Contract.GetFamilyId()));
			return MakeShared<FJsonValueArray>(MoveTemp(Items));
		}

		TSharedPtr<FJsonValue> NumberFact(double Value)
		{
			if (FMath::IsFinite(Value)) return MakeShared<FJsonValueNumber>(Value);
			return MakeShared<FJsonValueString>(Value > 0.0 ? TEXT("Infinity") : Value < 0.0 ? TEXT("-Infinity") : TEXT("NaN"));
		}

		bool Fail(FExecutionMutationError& OutError, FExecutionMutationError Diagnostic)
		{
			OutError = MoveTemp(Diagnostic);
			return false;
		}
		bool IsNormalizedNonEmpty(const FString& Value)
		{
			FString Normalized = Value;
			Normalized.TrimStartAndEndInline();
			return !Normalized.IsEmpty() && Normalized == Value;
		}

		FString ExpectedPolicyPrefix(EUeAgentCompileBuildJobFamily Family)
		{
			switch (Family)
			{
			case EUeAgentCompileBuildJobFamily::AssetCompile: return TEXT("policy:asset-compile:");
			case EUeAgentCompileBuildJobFamily::WorldBuild: return TEXT("policy:world-build:");
			case EUeAgentCompileBuildJobFamily::ExternalProcess: return TEXT("policy:owned-tool:");
			case EUeAgentCompileBuildJobFamily::WorldPartitionResave: return TEXT("policy:world-partition-resave:");
			case EUeAgentCompileBuildJobFamily::Packaging: return TEXT("policy:packaging:");
			default: return FString();
			}
		}

		bool MatchesPolicyKind(EUeAgentCompileBuildJobFamily Family, const FString& PolicyId)
		{
			const FString Prefix = ExpectedPolicyPrefix(Family);
			return !Prefix.IsEmpty() && PolicyId.StartsWith(Prefix);
		}
		bool IsSupportedMechanism(const FString& MechanismId)
		{
			return MechanismId == TEXT("game_thread_stage")
				|| MechanismId == TEXT("owned_process");
		}

		bool ValidatePolicy(const FUeAgentCompileBuildJobPolicyDescriptor& Policy, FExecutionMutationError& OutError)
		{
			OutError.Reset();
			const TCHAR* Code = TEXT("compile_build_job_policy_invalid");
			if (!FindCompileBuildJobFamilyContract(Policy.GetFamily()))
				return Fail(OutError, JobFact(Code, TEXT("The policy uses an unsupported compile/build family."),
					TEXT("policy.family"), FamilyValue(Policy.GetFamily()), FamilyChoices(), nullptr, TEXT("runtime_factory")));
			if (!IsNormalizedNonEmpty(Policy.GetPolicyId()) || !MatchesPolicyKind(Policy.GetFamily(), Policy.GetPolicyId()))
				return Fail(OutError, JobFact(Code, !IsNormalizedNonEmpty(Policy.GetPolicyId())
					? TEXT("The policy reference must be non-empty and have no surrounding whitespace.")
					: TEXT("The policy reference does not match its compile/build family."),
					TEXT("policy.policy_id"), MakeShared<FJsonValueString>(Policy.GetPolicyId()),
					MakeShared<FJsonValueString>(ExpectedPolicyPrefix(Policy.GetFamily())), nullptr, TEXT("runtime_factory")));
			if (!IsSupportedMechanism(Policy.GetNativeMechanismId()))
			{
				TArray<TSharedPtr<FJsonValue>> Expected;
				Expected.Add(MakeShared<FJsonValueString>(TEXT("game_thread_stage")));
				Expected.Add(MakeShared<FJsonValueString>(TEXT("owned_process")));
				return Fail(OutError, JobFact(Code, TEXT("The policy uses an unsupported native execution mechanism."),
					TEXT("policy.native_mechanism_id"), MakeShared<FJsonValueString>(Policy.GetNativeMechanismId()),
					MakeShared<FJsonValueArray>(MoveTemp(Expected)), nullptr, TEXT("runtime_factory")));
			}
			if (!IsNormalizedNonEmpty(Policy.GetReadbackOracleId()) || !Policy.GetReadbackOracleId().StartsWith(TEXT("uai.readback.")))
				return Fail(OutError, JobFact(Code, !IsNormalizedNonEmpty(Policy.GetReadbackOracleId())
					? TEXT("The readback contract reference must be non-empty and have no surrounding whitespace.")
					: TEXT("The readback contract reference must start with 'uai.readback.'."),
					TEXT("policy.readback_oracle_id"), MakeShared<FJsonValueString>(Policy.GetReadbackOracleId()),
					MakeShared<FJsonValueString>(TEXT("normalized non-empty reference starting with uai.readback.")), nullptr, TEXT("runtime_factory")));
			return true;
		}

		bool ValidateLaunchSpec(const FUeAgentCompileBuildJobProcessLaunchSpec& Spec, FExecutionMutationError& OutError)
		{
			OutError.Reset();
			const TCHAR* Code = TEXT("compile_build_job_process_contract_invalid");
			if (!IsNormalizedNonEmpty(Spec.GetExecutable()) || !IsNormalizedNonEmpty(Spec.GetWorkingDirectory()))
			{
				const bool bExecutable = !IsNormalizedNonEmpty(Spec.GetExecutable());
				const FString& Value = bExecutable ? Spec.GetExecutable() : Spec.GetWorkingDirectory();
				auto Error = JobFact(Code, FString::Printf(TEXT("The process %s must be non-empty and have no surrounding whitespace."),
					bExecutable ? TEXT("executable path") : TEXT("working directory")),
					bExecutable ? TEXT("launch.executable.normalized_nonempty") : TEXT("launch.working_directory.normalized_nonempty"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), nullptr, TEXT("runtime_factory"));
				Error.ActualEvidence = MakeShared<FJsonValueString>(Value);
				return Fail(OutError, MoveTemp(Error));
			}
			if (!FMath::IsFinite(Spec.GetTimeoutSeconds()) || Spec.GetTimeoutSeconds() < 0.0)
				return Fail(OutError, JobFact(Code, TEXT("The process timeout must be a finite, non-negative number of seconds."),
					TEXT("launch.timeout_seconds"), NumberFact(Spec.GetTimeoutSeconds()),
					MakeShared<FJsonValueString>(TEXT("finite number >= 0 seconds")), nullptr, TEXT("runtime_factory")));
			return true;
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

		bool ValidateOperationRequest(const FLongRunningOperationRequest& Request,
			const FUeAgentCompileBuildJobTypedRequest& TypedRequest, FExecutionMutationError& OutError)
		{
			if (!Request.IsValid())
			{
				OutError = Request.GetValidationDiagnostic();
				SetRequestContext(OutError, TypedRequest);
				return false;
			}
			if (Request.GetExecutionContext().GetStatePlane() != EUeAgentStatePlane::Persisted)
				return Fail(OutError, JobFact(TEXT("compile_build_job_context_not_persisted"),
					TEXT("Compile/build jobs require a persisted execution state plane."), TEXT("execution_context.state_plane"),
					MakeShared<FJsonValueString>(UE::UAI::Contract::LexToString(Request.GetExecutionContext().GetStatePlane())),
					MakeShared<FJsonValueString>(UE::UAI::Contract::LexToString(EUeAgentStatePlane::Persisted)), &TypedRequest));
			return true;
		}

		bool ValidateCallIdentity(const FUeAgentCompileBuildJobTypedRequest& Request,
			const FString& ExpectedOperationId, const FLongRunningWorkIdentity& ExpectedIdentity,
			const FString& OperationId, const FLongRunningWorkIdentity& Identity, FExecutionMutationError& OutError)
		{
			if (ExpectedOperationId != OperationId)
			{
				auto Error = JobFact(TEXT("compile_build_job_operation_id_mismatch"),
					TEXT("The native call does not belong to this compile/build operation."), TEXT("call.operation_id_matches"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), &Request);
				Error.ActualEvidence = MakeShared<FJsonValueString>(OperationId);
				Error.ExpectedEvidence = MakeShared<FJsonValueString>(ExpectedOperationId);
				return Fail(OutError, MoveTemp(Error));
			}
			if (!(ExpectedIdentity == Identity))
			{
				auto Error = JobFact(TEXT("compile_build_job_work_identity_mismatch"),
					ExpectedIdentity.GetResourceId() != Identity.GetResourceId()
						? TEXT("The native work resource identity does not match this compile/build job.")
						: TEXT("The native work generation does not match this compile/build job."),
					TEXT("call.work_identity_matches"), MakeShared<FJsonValueBoolean>(false),
					MakeShared<FJsonValueBoolean>(true), &Request);
				auto Actual = MakeShared<FJsonObject>();
				Actual->SetStringField(TEXT("resource_id"), Identity.GetResourceId());
				Actual->SetNumberField(TEXT("generation"), Identity.GetGeneration());
				auto Expected = MakeShared<FJsonObject>();
				Expected->SetStringField(TEXT("resource_id"), ExpectedIdentity.GetResourceId());
				Expected->SetNumberField(TEXT("generation"), ExpectedIdentity.GetGeneration());
				Error.ActualEvidence = MakeShared<FJsonValueObject>(Actual);
				Error.ExpectedEvidence = MakeShared<FJsonValueObject>(Expected);
				return Fail(OutError, MoveTemp(Error));
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
				return Fail(OutError, Request.GetValidationDiagnostic());
			}

			FUeAgentStateSemanticError StateError;
			TArray<TPair<FString, FUeAgentCanonicalValueRef>> Items;
			auto AddString = [&](const FString& Key, const FString& Value)
			{
				FUeAgentCanonicalValuePtr Canonical;
				if (!FUeAgentCanonicalValue::TryString(
						Value, Resources, Canonical, StateError))
				{
					if (StateError.PropertyPath.IsEmpty()) StateError.PropertyPath = TEXT("signal.") + Key;
					if (StateError.SemanticPointer.IsEmpty()) StateError.SemanticPointer = TEXT("signal.") + Key;
					if (StateError.SourceKind.IsEmpty()) StateError.SourceKind = TEXT("native_result");
					SetRequestContext(StateError, Request);
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
				OutError = MoveTemp(StateError);
				return false;
			}

			// Publish completed reports through the same typed artifact contract as
			// capture jobs. ReportRef remains machine-owned; callers consume handles.
			FString ReportPath = ReportRef;
			if ((Phase == TEXT("succeeded") || Phase == TEXT("failed"))
				&& ReportPath.RemoveFromStart(TEXT("file:"))
				&& IFileManager::Get().FileExists(*ReportPath))
			{
				using namespace ArtifactPublisher;
				// Report owners choose .json for structured reports and .log for process output.
				// A family can produce either format (for example Localization external processes).
				const bool bJson = FPaths::GetExtension(ReportPath).Equals(TEXT("json"), ESearchCase::IgnoreCase);
				const FString FinalPath = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()
					/ TEXT("tmp/uai-compile-build") / FPaths::GetCleanFilename(ReportPath));
				FFileArtifactReference Artifact;
				FFileArtifactPublishError ArtifactError;
				FUeAgentCanonicalValuePtr ArtifactValue;
				if (!TryPublishFileArtifact(FFileArtifactPublishRequest(
						ReportPath, FinalPath, TEXT("uai.compile_build_job"), TEXT("report"),
						bJson ? TEXT("json") : TEXT("text"),
						bJson ? TEXT("application/json") : TEXT("text/plain")),
					Resources, Artifact, ArtifactError)
					|| !TryProjectFileArtifactReference(Artifact, Resources, ArtifactValue, ArtifactError))
					{ OutError = MoveTemp(ArtifactError); SetRequestContext(OutError, Request); return false; }
				Items.Emplace(TEXT("artifact_ref"), ArtifactValue.ToSharedRef());
			}

			FUeAgentCanonicalValuePtr Payload;
			if (!FUeAgentCanonicalValue::TryObject(
					Items, Resources, Payload, StateError))
			{
				OutError = UE::UAI::Contract::FDiagnostic::WrapFailure(TEXT("compile_build_job_signal_payload_failed"), StateError, TEXT("execution"));
				SetRequestContext(OutError, Request);
				return false;
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

		FString StageStateName(EGameThreadStageState State)
		{
			switch (State)
			{
			case EGameThreadStageState::Queued: return TEXT("queued");
			case EGameThreadStageState::Executing: return TEXT("executing");
			case EGameThreadStageState::ExitedSuccess: return TEXT("exited_success");
			case EGameThreadStageState::ExitedFailure: return TEXT("exited_failure");
			default: return FString::Printf(TEXT("unknown (%d)"), static_cast<int32>(State));
			}
		}

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
			UE::UAI::Contract::FDiagnostic Diagnostic;
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
					return Fail(OutError, JobFact(TEXT("compile_build_job_already_started"), TEXT("Cannot start a game-thread compile/build stage more than once."), TEXT("game_thread_stage.started"),
						MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false), &Request));
				}
				if (!ValidateOperationRequest(OperationRequest, Request, OutError))
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
								ScheduledState->Diagnostic = JobFact(TEXT("compile_build_job_cancelled"), ScheduledState->bStopRequested
									? TEXT("Stop was requested before the game-thread compile/build stage executed.")
									: TEXT("The game-thread compile/build stage was cancelled before execution."),
									ScheduledState->bStopRequested ? TEXT("game_thread_stage.stop_requested") : TEXT("game_thread_stage.cancelled"),
									MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false), &ScheduledState->Request);
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
						if (bSucceeded) ScheduledState->Diagnostic.Reset();
						else if (!Error.IsEmpty())
						{
							ScheduledState->Diagnostic = MoveTemp(Error);
							SetRequestContext(ScheduledState->Diagnostic, ScheduledState->Request);
						}
						else ScheduledState->Diagnostic = JobFact(TEXT("compile_build_job_stage_failed"),
							TEXT("The compile/build stage failed without returning a diagnostic."),
							TEXT("game_thread_stage.failure_diagnostic.available"), MakeShared<FJsonValueBoolean>(false),
							MakeShared<FJsonValueBoolean>(true), &ScheduledState->Request);
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
					return Fail(OutError, JobFact(TEXT("compile_build_job_not_started"), TEXT("Cannot observe a game-thread compile/build stage before Start."), TEXT("game_thread_stage.started"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), &Request));
				}
				FScopeLock Lock(&State->Mutex);
				if (!ValidateCallIdentity(
						State->Request,
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
						if (!State->Diagnostic.IsEmpty()) OutError.Append(State->Diagnostic);
						return false;
					}
					State->LastSignalPhase = Phase;
					State->SignalRevision = NextRevision;
				}
				OutObservation = FLongRunningWorkObservation(
					Identity.GetGeneration(),
					WorkState,
					State->Diagnostic,
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
					return Fail(OutError, JobFact(TEXT("compile_build_job_not_started"), TEXT("Cannot stop a game-thread compile/build stage before Start."), TEXT("game_thread_stage.started"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), &Request));
				}
				{
					FScopeLock Lock(&State->Mutex);
					if (!ValidateCallIdentity(
							State->Request,
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
					return Fail(OutError, JobFact(TEXT("compile_build_job_not_started"), TEXT("Cannot join a game-thread compile/build stage before Start."), TEXT("game_thread_stage.started"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), &Request));
				}
				FScopeLock Lock(&State->Mutex);
				if (!ValidateCallIdentity(
						State->Request,
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
					return Fail(OutError, JobFact(TEXT("compile_build_job_join_before_exit"),
						TEXT("Cannot join the game-thread compile/build stage before it exits."), TEXT("game_thread_stage.state"),
						MakeShared<FJsonValueString>(StageStateName(State->State)),
						MakeShared<FJsonValueString>(TEXT("exited_success or exited_failure")), &Request));
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
					return Fail(OutError, JobFact(TEXT("compile_build_job_not_started"), TEXT("Cannot release a game-thread compile/build stage before Start."), TEXT("game_thread_stage.started"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), &Request));
				}
				FScopeLock Lock(&State->Mutex);
				if (!ValidateCallIdentity(
						State->Request,
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
					return Fail(OutError, JobFact(TEXT("compile_build_job_release_before_join"), TEXT("Cannot release the game-thread compile/build stage before Join."), TEXT("game_thread_stage.joined"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), &Request));
				}
				if (State->bReleased)
				{
					return Fail(OutError, JobFact(TEXT("compile_build_job_already_released"), TEXT("The game-thread compile/build stage was already released."), TEXT("game_thread_stage.released"),
						MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false), &Request));
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
					return Fail(OutError, JobFact(TEXT("compile_build_job_already_started"), TEXT("Cannot start the owned compile/build process more than once."), TEXT("owned_process.started"),
						MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false), &Request));
				}
				if (!ValidateOperationRequest(OperationRequest, Request, OutError))
				{
					return false;
				}
				if (Cancellation.IsCancelled())
				{
					return Fail(OutError, JobFact(TEXT("compile_build_job_cancelled_before_start"), TEXT("The owned compile/build process was cancelled before Start."), TEXT("owned_process.cancelled"),
						MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false), &Request));
				}
				if (!FPlatformProcess::CreatePipe(ReadPipe, WritePipe))
				{
					return Fail(OutError, JobFact(TEXT("compile_build_job_process_pipe_failed"), TEXT("The native platform could not create the owned process output pipe."), TEXT("owned_process.pipe_created"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), &Request));
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
					auto Error = JobFact(TEXT("compile_build_job_process_launch_failed"),
						TEXT("The native process launch did not return a valid process handle."), TEXT("owned_process.started"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), &Request);
					auto Evidence = MakeShared<FJsonObject>();
					Evidence->SetStringField(TEXT("executable"), LaunchSpec.GetExecutable());
					Evidence->SetStringField(TEXT("working_directory"), LaunchSpec.GetWorkingDirectory());
					Error.ActualEvidence = MakeShared<FJsonValueObject>(Evidence);
					return Fail(OutError, MoveTemp(Error));
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
					&& !bStopRequested
					&& LaunchSpec.GetTimeoutSeconds() > 0.0
					&& FPlatformProcess::IsProcRunning(ProcessHandle)
					&& (FPlatformTime::Seconds() - StartedAtSeconds)
						>= LaunchSpec.GetTimeoutSeconds())
				{
					const double ElapsedSeconds = FPlatformTime::Seconds() - StartedAtSeconds;
					FPlatformProcess::TerminateProc(ProcessHandle, true);
					bStopRequested = true;
					Diagnostic = JobFact(TEXT("compile_build_job_process_timeout"),
						TEXT("The compile/build process exceeded its time limit."), TEXT("owned_process.elapsed_seconds"),
						NumberFact(ElapsedSeconds), NumberFact(LaunchSpec.GetTimeoutSeconds()), &Request);
				}
				if (!bObservedExit
					&& !FPlatformProcess::IsProcRunning(ProcessHandle))
				{
					if (!DrainOutput(OutError))
					{
						if (!Diagnostic.IsEmpty()) OutError.Append(Diagnostic);
						return false;
					}
					else
					{
						int32 ReturnCode = 0;
						if (!FPlatformProcess::GetProcReturnCode(
								ProcessHandle, &ReturnCode))
						{
							bStateUnknown = true;
							Diagnostic.Append(JobFact(TEXT("compile_build_job_process_exit_unknown"),
								TEXT("The compile/build process exited, but the native return code could not be read."),
								TEXT("owned_process.return_code.available"), MakeShared<FJsonValueBoolean>(false),
								MakeShared<FJsonValueBoolean>(true), &Request));
						}
						else
						{
							FDiagnostic ExitDiagnostic;
							if (ReturnCode != 0)
							{
								ExitDiagnostic = JobFact(TEXT("compile_build_job_process_failed"),
									TEXT("The compile/build process exited with a nonzero return code."), TEXT("owned_process.return_code"),
									MakeShared<FJsonValueNumber>(ReturnCode), MakeShared<FJsonValueNumber>(0), &Request);
							}
							if (!OutputSink->Append(
									FString::Printf(
										TEXT("\nUAI_PROCESS_RETURN_CODE=%d\n"),
										ReturnCode),
									OutError))
							{
								SetRequestContext(OutError, Request);
								if (!Diagnostic.IsEmpty()) OutError.Append(Diagnostic);
								if (!ExitDiagnostic.IsEmpty()) OutError.Append(ExitDiagnostic);
								return false;
							}
							bExitedSuccess = ReturnCode == 0 && Diagnostic.IsEmpty();
							if (!ExitDiagnostic.IsEmpty()) Diagnostic.Append(ExitDiagnostic);
						}
					}
					bObservedExit = true;
				}

				if (bObservedExit && !OutputSink->FinalizeReport(OutError))
				{
					SetRequestContext(OutError, Request);
					if (!Diagnostic.IsEmpty()) OutError.Append(Diagnostic);
					return false;
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
						if (bObservedExit && !Diagnostic.IsEmpty()) OutError.Append(Diagnostic);
						return false;
					}
					LastSignalPhase = Phase;
					SignalRevision = NextRevision;
				}
				OutObservation = FLongRunningWorkObservation(
					Identity.GetGeneration(),
					WorkState,
					// A timeout requests termination first. Retain its cause internally,
					// but publish a failure only after the native process has exited.
					bObservedExit ? Diagnostic : UE::UAI::Contract::FDiagnostic(),
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
					return Fail(OutError, JobFact(TEXT("compile_build_job_join_before_exit"), TEXT("Cannot join the owned compile/build process before its exit is observed."), TEXT("owned_process.exit_observed"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), &Request));
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
					return Fail(OutError, JobFact(TEXT("compile_build_job_release_before_join"), TEXT("Cannot release the owned compile/build process before Join."), TEXT("owned_process.joined"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), &Request));
				}
				if (bReleased)
				{
					return Fail(OutError, JobFact(TEXT("compile_build_job_already_released"), TEXT("The owned compile/build process was already released."), TEXT("owned_process.released"),
						MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false), &Request));
				}
				if (!OutputSink->Release(OutError))
				{
					SetRequestContext(OutError, Request);
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
					return Fail(OutError, JobFact(TEXT("compile_build_job_not_started"), TEXT("Cannot call the owned compile/build process before Start."), TEXT("owned_process.started"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), &Request));
				}
				return ValidateCallIdentity(
					Request,
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
				if (!Chunk.IsEmpty() && !OutputSink->Append(Chunk, OutError))
				{
					SetRequestContext(OutError, Request);
					if (bObservedExit && !Diagnostic.IsEmpty()) OutError.Append(Diagnostic);
					return false;
				}
				return true;
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
			UE::UAI::Contract::FDiagnostic Diagnostic;
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
		ValidatePolicy(*this, ValidationDiagnostic);
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
		return ValidationDiagnostic.IsEmpty();
	}

	const FExecutionMutationError& FUeAgentCompileBuildJobPolicyDescriptor::GetValidationDiagnostic() const
	{
		return ValidationDiagnostic;
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
			return Fail(OutError, JobFact(TEXT("compile_build_job_policy_catalog_empty"),
				TEXT("The compile/build policy catalog must contain at least one policy."), TEXT("policies.count"),
				MakeShared<FJsonValueNumber>(InRegistrations.Num()), MakeShared<FJsonValueString>(TEXT("greater than zero")),
				nullptr, TEXT("runtime_factory")));
		}
		for (int32 Index = 0; Index < InRegistrations.Num(); ++Index)
		{
			const FUeAgentCompileBuildJobPolicyDescriptor& Descriptor =
				InRegistrations[Index].GetDescriptor();
			if (!Descriptor.IsValid())
			{
				OutError = Descriptor.GetValidationDiagnostic();
				const FString Field = OutError.PropertyPath.StartsWith(TEXT("policy."))
					? OutError.PropertyPath.Mid(7) : OutError.PropertyPath;
				OutError.PropertyPath = FString::Printf(TEXT("policies/%d/"), Index) + Field;
				OutError.SemanticPointer = OutError.PropertyPath;
				if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = Descriptor.GetPolicyId();
				return false;
			}
			for (int32 Other = 0; Other < Index; ++Other)
			{
				const FUeAgentCompileBuildJobPolicyDescriptor& Existing =
					InRegistrations[Other].GetDescriptor();
				if (Existing.GetFamily() == Descriptor.GetFamily()
					&& Existing.GetPolicyId() == Descriptor.GetPolicyId())
				{
					auto Actual = MakeShared<FJsonObject>();
					Actual->SetField(TEXT("family"), FamilyValue(Descriptor.GetFamily()));
					Actual->SetStringField(TEXT("policy_id"), Descriptor.GetPolicyId());
					Actual->SetNumberField(TEXT("first_index"), Other);
					Actual->SetNumberField(TEXT("duplicate_index"), Index);
					auto Error = JobFact(TEXT("compile_build_job_policy_duplicate"),
						TEXT("The policy catalog contains a duplicate family/policy pair."),
						FString::Printf(TEXT("policies/%d"), Index), MakeShared<FJsonValueObject>(Actual),
						MakeShared<FJsonValueString>(TEXT("unique family/policy pair")), nullptr, TEXT("runtime_factory"));
					Error.CandidateName = Descriptor.GetPolicyId();
					return Fail(OutError, MoveTemp(Error));
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
			return Fail(OutError, JobFact(TEXT("compile_build_job_policy_not_registered"),
				TEXT("No compile/build policy is registered for this request's family/policy pair."),
				TEXT("request.policy"), PolicyPair(Request.GetFamily(), Request.GetPolicyId()),
				MakeShared<FJsonValueString>(TEXT("registered family/policy pair")), &Request, TEXT("runtime_factory")));
		}
		if (!Request.IsValid())
		{
			OutError = Request.GetValidationDiagnostic();
			SetRequestContext(OutError, Request);
			return false;
		}
		if (!Registration->GetBuilder().TryCreatePort(
				Request,
				Registration->GetDescriptor(),
				OutPort,
				OutError))
		{
			SetRequestContext(OutError, Request);
			return false;
		}
		if (!OutPort.IsValid())
			return Fail(OutError, JobFact(TEXT("compile_build_job_operation_port_missing"),
				TEXT("The registered policy builder reported success but returned no operation port."),
				TEXT("operation_port.available"), MakeShared<FJsonValueBoolean>(false),
				MakeShared<FJsonValueBoolean>(true), &Request, TEXT("runtime_factory")));
		return true;
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
		if (!Request.IsValid())
		{
			OutError = Request.GetValidationDiagnostic();
			SetRequestContext(OutError, Request);
			return false;
		}
		if (!Policy.IsValid())
		{
			OutError = Policy.GetValidationDiagnostic();
			SetRequestContext(OutError, Request);
			return false;
		}
		if (!MatchesPolicy(Request, Policy))
			return Fail(OutError, JobFact(TEXT("compile_build_job_stage_contract_invalid"),
				TEXT("The stage request's family/policy pair does not match the stage policy."), TEXT("request.policy"),
				PolicyPair(Request.GetFamily(), Request.GetPolicyId()), PolicyPair(Policy.GetFamily(), Policy.GetPolicyId()),
				&Request, TEXT("runtime_factory")));
		if (Policy.GetNativeMechanismId() != TEXT("game_thread_stage"))
		{
			return Fail(OutError, JobFact(TEXT("compile_build_job_native_mechanism_mismatch"),
				TEXT("A game-thread stage port requires the game_thread_stage execution mechanism."),
				TEXT("policy.native_mechanism_id"), MakeShared<FJsonValueString>(Policy.GetNativeMechanismId()),
				MakeShared<FJsonValueString>(TEXT("game_thread_stage")), &Request, TEXT("runtime_factory")));
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
		ValidateLaunchSpec(*this, ValidationDiagnostic);
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
		return ValidationDiagnostic.IsEmpty();
	}

	const FExecutionMutationError& FUeAgentCompileBuildJobProcessLaunchSpec::GetValidationDiagnostic() const
	{
		return ValidationDiagnostic;
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
		if (!Request.IsValid())
		{
			OutError = Request.GetValidationDiagnostic();
			SetRequestContext(OutError, Request);
			return false;
		}
		if (!Policy.IsValid())
		{
			OutError = Policy.GetValidationDiagnostic();
			SetRequestContext(OutError, Request);
			return false;
		}
		if (!MatchesPolicy(Request, Policy))
			return Fail(OutError, JobFact(TEXT("compile_build_job_process_contract_invalid"),
				TEXT("The process request's family/policy pair does not match the process policy."), TEXT("request.policy"),
				PolicyPair(Request.GetFamily(), Request.GetPolicyId()), PolicyPair(Policy.GetFamily(), Policy.GetPolicyId()),
				&Request, TEXT("runtime_factory")));
		if (!LaunchSpec.IsValid())
		{
			OutError = LaunchSpec.GetValidationDiagnostic();
			SetRequestContext(OutError, Request);
			return false;
		}
		if (Policy.GetNativeMechanismId() != TEXT("owned_process"))
		{
			return Fail(OutError, JobFact(TEXT("compile_build_job_native_mechanism_mismatch"),
				TEXT("An owned process port requires the owned_process execution mechanism."),
				TEXT("policy.native_mechanism_id"), MakeShared<FJsonValueString>(Policy.GetNativeMechanismId()),
				MakeShared<FJsonValueString>(TEXT("owned_process")), &Request, TEXT("runtime_factory")));
		}
		OutPort = MakeShared<FOwnedProcessPort, ESPMode::ThreadSafe>(
			Request, Policy, LaunchSpec, MoveTemp(OutputSink));
		return true;
	}
}

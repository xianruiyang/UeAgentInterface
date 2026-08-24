#include "DomainAdapters/EditorSessionOperation/UeAgentEditorSessionOperationProduction.h"

#include "Containers/Ticker.h"
#include "HAL/PlatformMisc.h"
#include "Misc/ScopeLock.h"

namespace UE::UAI::DomainAdapters::EditorSessionOperation
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
			const EUeAgentEditorSessionOperationFamily Family,
			const FString& PolicyId)
		{
			switch (Family)
			{
			case EUeAgentEditorSessionOperationFamily::AssetEditor:
				return PolicyId.StartsWith(TEXT("policy:asset-editor:"));
			case EUeAgentEditorSessionOperationFamily::RuntimePreview:
				return PolicyId.StartsWith(TEXT("policy:runtime-session:"));
			case EUeAgentEditorSessionOperationFamily::WorldContext:
				return PolicyId.StartsWith(TEXT("policy:world-context:"));
			case EUeAgentEditorSessionOperationFamily::ModelingTool:
				return PolicyId.StartsWith(TEXT("policy:modeling-tool:"));
			case EUeAgentEditorSessionOperationFamily::VoxelGrid:
				return PolicyId.StartsWith(TEXT("policy:voxel-session:"));
			case EUeAgentEditorSessionOperationFamily::HostLifecycle:
				return PolicyId.StartsWith(TEXT("policy:host-lifecycle:"));
			default:
				return false;
			}
		}

		bool MechanismMatchesFamily(
			const EUeAgentEditorSessionOperationFamily Family,
			const FString& MechanismId)
		{
			return Family == EUeAgentEditorSessionOperationFamily::HostLifecycle
				? MechanismId == TEXT("host_exit_schedule")
				: MechanismId == TEXT("native_session_resource");
		}

		bool AllowedWorldTypesAreValid(
			const TArray<EUeAgentExecutionWorldType>& WorldTypes)
		{
			for (int32 Index = 0; Index < WorldTypes.Num(); ++Index)
			{
				if (WorldTypes[Index] == EUeAgentExecutionWorldType::Invalid)
				{
					return false;
				}
				for (int32 Other = 0; Other < Index; ++Other)
				{
					if (WorldTypes[Other] == WorldTypes[Index])
					{
						return false;
					}
				}
			}
			return true;
		}

		bool MatchesPolicy(
			const FUeAgentEditorSessionOperationTypedRequest& Request,
			const FUeAgentEditorSessionOperationPolicyDescriptor& Policy)
		{
			return Request.GetFamily() == Policy.GetFamily()
				&& Request.GetPolicyId() == Policy.GetPolicyId();
		}

		int64 NextGeneration()
		{
			static FCriticalSection Mutex;
			static int64 Generation = 0;
			FScopeLock Lock(&Mutex);
			return ++Generation;
		}

		bool ValidateOperationContext(
			const FLongRunningOperationRequest& Request,
			const FUeAgentEditorSessionOperationPolicyDescriptor& Policy,
			FExecutionMutationError& OutError)
		{
			if (!Request.IsValid())
			{
				return Fail(
					OutError,
					TEXT("editor_session_operation_request_invalid"));
			}
			const FUeAgentExecutionContextRef& Context =
				Request.GetExecutionContext();
			if (Context.GetStatePlane() != Policy.GetRequiredStatePlane())
			{
				return Fail(
					OutError,
					TEXT(
						"editor_session_operation_context_state_plane_mismatch"));
			}
			const TArray<EUeAgentExecutionWorldType>& AllowedWorldTypes =
				Policy.GetAllowedWorldTypes();
			if (!AllowedWorldTypes.IsEmpty()
				&& !AllowedWorldTypes.Contains(Context.GetWorldType()))
			{
				return Fail(
					OutError,
					TEXT(
						"editor_session_operation_context_world_type_mismatch"));
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
					TEXT("editor_session_operation_id_mismatch"));
			}
			if (!(ExpectedIdentity == Identity))
			{
				return Fail(
					OutError,
					TEXT("editor_session_work_identity_mismatch"));
			}
			return true;
		}

		bool TryBuildSignal(
			const FUeAgentEditorSessionOperationTypedRequest& Request,
			const FUeAgentEditorSessionOperationPolicyDescriptor& Policy,
			const FString& Phase,
			const FString& SessionRef,
			const int64 Revision,
			FUeAgentResourceTracker& Resources,
			TOptional<FLongRunningOperationSignal>& OutSignal,
			FExecutionMutationError& OutError)
		{
			const FUeAgentEditorSessionOperationFamilyContract* Contract =
				FindEditorSessionOperationFamilyContract(Request.GetFamily());
			if (Contract == nullptr)
			{
				return Fail(
					OutError,
					TEXT("editor_session_operation_family_invalid"));
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
				|| (!SessionRef.IsEmpty()
					&& !AddString(TEXT("session_ref"), SessionRef)))
			{
				return Fail(
					OutError,
					TEXT("editor_session_signal_value_failed"),
					StateError.Code);
			}

			FUeAgentCanonicalValuePtr Payload;
			if (!FUeAgentCanonicalValue::TryObject(
					Items, Resources, Payload, StateError))
			{
				return Fail(
					OutError,
					TEXT("editor_session_signal_payload_failed"),
					StateError.Code);
			}
			OutSignal.Emplace(
				Contract->GetSignalTypeId(),
				Revision,
				Payload.ToSharedRef());
			return true;
		}

		class FNativeSessionResourcePort final
			: public ILongRunningOperationPort
		{
		public:
			FNativeSessionResourcePort(
				FUeAgentEditorSessionOperationTypedRequest InRequest,
				FUeAgentEditorSessionOperationPolicyDescriptor InPolicy,
				TSharedRef<
					IUeAgentEditorSessionNativeSession,
					ESPMode::ThreadSafe> InNativeSession)
				: Request(MoveTemp(InRequest))
				, Policy(MoveTemp(InPolicy))
				, NativeSession(MoveTemp(InNativeSession))
			{
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
						TEXT("editor_session_operation_already_started"));
				}
				if (!ValidateOperationContext(
						OperationRequest, Policy, OutError))
				{
					return false;
				}
				FString NativeResourceId;
				if (!NativeSession->Start(
						OperationRequest,
						Cancellation,
						NativeResourceId,
						OutError))
				{
					return false;
				}
				if (!IsNormalizedNonEmpty(NativeResourceId))
				{
					return Fail(
						OutError,
						TEXT("editor_session_native_resource_id_invalid"));
				}
				OperationId = OperationRequest.GetOperationId();
				Identity = FLongRunningWorkIdentity(
					TEXT("native:editor-session:") + OperationId
						+ TEXT(":") + NativeResourceId,
					NextGeneration());
				bStarted = true;
				OutIdentity = Identity;
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
				if (!ValidateStartedIdentity(
						InOperationId, InIdentity, OutError))
				{
					return false;
				}
				FUeAgentEditorSessionNativeObservation NativeObservation;
				if (!NativeSession->Observe(NativeObservation, OutError))
				{
					return false;
				}
				if (!NativeObservation.IsValid())
				{
					return Fail(
						OutError,
						TEXT("editor_session_native_observation_invalid"));
				}

				const FString SignalKey = NativeObservation.GetPhase()
					+ TEXT("\n") + NativeObservation.GetSessionRef();
				TOptional<FLongRunningOperationSignal> Signal;
				if (SignalKey != LastSignalKey)
				{
					const int64 NextRevision = SignalRevision + 1;
					if (!TryBuildSignal(
							Request,
							Policy,
							NativeObservation.GetPhase(),
							NativeObservation.GetSessionRef(),
							NextRevision,
							Resources,
							Signal,
							OutError))
					{
						return false;
					}
					LastSignalKey = SignalKey;
					SignalRevision = NextRevision;
				}
				LastObservedState = NativeObservation.GetState();
				OutObservation = FLongRunningWorkObservation(
					Identity.GetGeneration(),
					NativeObservation.GetState(),
					NativeObservation.GetErrorCode(),
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
				if (!ValidateStartedIdentity(
						InOperationId, InIdentity, OutError))
				{
					return false;
				}
				if (!IsNormalizedNonEmpty(Reason))
				{
					return Fail(
						OutError,
						TEXT("editor_session_stop_reason_invalid"));
				}
				if (bStopRequested)
				{
					return true;
				}
				if (!NativeSession->RequestStop(Reason, OutError))
				{
					return false;
				}
				bStopRequested = true;
				return true;
			}

			bool Join(
				const FString& InOperationId,
				const FLongRunningWorkIdentity& InIdentity,
				FExecutionMutationError& OutError) override
			{
				FScopeLock Lock(&Mutex);
				if (!ValidateStartedIdentity(
						InOperationId, InIdentity, OutError))
				{
					return false;
				}
				if (LastObservedState != ELongRunningWorkState::ExitedSuccess
					&& LastObservedState
						!= ELongRunningWorkState::ExitedFailure)
				{
					return Fail(
						OutError,
						TEXT("editor_session_join_before_native_exit"));
				}
				if (bJoined)
				{
					return true;
				}
				if (!NativeSession->Join(OutError))
				{
					return false;
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
				if (!ValidateStartedIdentity(
						InOperationId, InIdentity, OutError))
				{
					return false;
				}
				if (!bJoined)
				{
					return Fail(
						OutError,
						TEXT("editor_session_release_before_join"));
				}
				if (bReleased)
				{
					return Fail(
						OutError,
						TEXT("editor_session_already_released"));
				}
				if (!NativeSession->Release(OutError))
				{
					return false;
				}
				bReleased = true;
				return true;
			}

		private:
			bool ValidateStartedIdentity(
				const FString& InOperationId,
				const FLongRunningWorkIdentity& InIdentity,
				FExecutionMutationError& OutError) const
			{
				return bStarted
					? ValidateCallIdentity(
						OperationId,
						Identity,
						InOperationId,
						InIdentity,
						OutError)
					: Fail(
						OutError,
						TEXT("editor_session_operation_not_started"));
			}

			FUeAgentEditorSessionOperationTypedRequest Request;
			FUeAgentEditorSessionOperationPolicyDescriptor Policy;
			TSharedRef<IUeAgentEditorSessionNativeSession,
				ESPMode::ThreadSafe> NativeSession;
			mutable FCriticalSection Mutex;
			FString OperationId;
			FLongRunningWorkIdentity Identity;
			ELongRunningWorkState LastObservedState =
				ELongRunningWorkState::Invalid;
			FString LastSignalKey;
			int64 SignalRevision = 0;
			bool bStarted = false;
			bool bStopRequested = false;
			bool bJoined = false;
			bool bReleased = false;
		};

		enum class EHostExitScheduleState : uint8
		{
			NotStarted,
			Scheduled,
			CallbackRunning,
			ExitRequestedAwaitingExternalWitness,
			Cancelled,
		};

		struct FHostExitSharedState final
		{
			FHostExitSharedState(
				FUeAgentEditorSessionOperationTypedRequest InRequest,
				FUeAgentEditorSessionOperationPolicyDescriptor InPolicy,
				TSharedRef<
					IUeAgentEditorHostExitTarget,
					ESPMode::ThreadSafe> InTarget,
				FString InOperationId,
				FLongRunningWorkIdentity InIdentity)
				: Request(MoveTemp(InRequest))
				, Policy(MoveTemp(InPolicy))
				, Target(MoveTemp(InTarget))
				, OperationId(MoveTemp(InOperationId))
				, Identity(MoveTemp(InIdentity))
			{
			}

			FCriticalSection Mutex;
			FUeAgentEditorSessionOperationTypedRequest Request;
			FUeAgentEditorSessionOperationPolicyDescriptor Policy;
			TSharedRef<IUeAgentEditorHostExitTarget, ESPMode::ThreadSafe>
				Target;
			FString OperationId;
			FLongRunningWorkIdentity Identity;
			FTSTicker::FDelegateHandle TickerHandle;
			EHostExitScheduleState State = EHostExitScheduleState::NotStarted;
			FString LastSignalPhase;
			int64 SignalRevision = 0;
			bool bJoined = false;
			bool bReleased = false;
		};

		class FHostExitSchedulePort final
			: public ILongRunningOperationPort
		{
		public:
			FHostExitSchedulePort(
				FUeAgentEditorSessionOperationTypedRequest InRequest,
				FUeAgentEditorSessionOperationPolicyDescriptor InPolicy,
				const double InExitDelaySeconds,
				TSharedRef<
					IUeAgentEditorHostExitTarget,
					ESPMode::ThreadSafe> InExitTarget)
				: Request(MoveTemp(InRequest))
				, Policy(MoveTemp(InPolicy))
				, ExitDelaySeconds(InExitDelaySeconds)
				, ExitTarget(MoveTemp(InExitTarget))
			{
			}

			~FHostExitSchedulePort() override
			{
				FTSTicker::FDelegateHandle Handle;
				if (State.IsValid())
				{
					FScopeLock Lock(&State->Mutex);
					if (State->State == EHostExitScheduleState::Scheduled
						&& State->TickerHandle.IsValid())
					{
						State->State = EHostExitScheduleState::Cancelled;
						Handle = State->TickerHandle;
						State->TickerHandle.Reset();
					}
				}
				if (Handle.IsValid())
				{
					FTSTicker::GetCoreTicker().RemoveTicker(Handle);
				}
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
						TEXT("editor_host_exit_already_scheduled"));
				}
				if (!IsInGameThread())
				{
					return Fail(
						OutError,
						TEXT("editor_host_exit_requires_game_thread"));
				}
				if (!ValidateOperationContext(
						OperationRequest, Policy, OutError))
				{
					return false;
				}
				const FString& ExpectedEditorInstance =
					ExitTarget->GetEditorInstanceId();
				if (OperationRequest.GetExecutionContext().GetEditorInstanceId()
						!= ExpectedEditorInstance
					|| Request.GetSubjectRef() != ExpectedEditorInstance)
				{
					return Fail(
						OutError,
						TEXT("editor_host_exit_instance_mismatch"));
				}
				if (Cancellation.IsCancelled())
				{
					return Fail(
						OutError,
						TEXT("editor_host_exit_cancelled_before_schedule"));
				}

				OutIdentity = FLongRunningWorkIdentity(
					TEXT("native:editor-host-exit:")
						+ OperationRequest.GetOperationId(),
					NextGeneration());
				State = MakeShared<FHostExitSharedState, ESPMode::ThreadSafe>(
					Request,
					Policy,
					ExitTarget,
					OperationRequest.GetOperationId(),
					OutIdentity);
				const TWeakPtr<FHostExitSharedState, ESPMode::ThreadSafe>
					WeakState = State;
				const FTSTicker::FDelegateHandle Handle =
					FTSTicker::GetCoreTicker().AddTicker(
						FTickerDelegate::CreateLambda(
							[WeakState](float)
							{
								const TSharedPtr<
									FHostExitSharedState,
									ESPMode::ThreadSafe> Pinned =
									WeakState.Pin();
								if (!Pinned.IsValid())
								{
									return false;
								}
								TSharedPtr<
									IUeAgentEditorHostExitTarget,
									ESPMode::ThreadSafe> Target;
								{
									FScopeLock Lock(&Pinned->Mutex);
									if (Pinned->State
										!= EHostExitScheduleState::Scheduled)
									{
										return false;
									}
									Pinned->State =
										EHostExitScheduleState::CallbackRunning;
									Pinned->TickerHandle.Reset();
									Target = Pinned->Target;
								}
								Target->RequestExit();
								{
									FScopeLock Lock(&Pinned->Mutex);
									Pinned->State = EHostExitScheduleState::
										ExitRequestedAwaitingExternalWitness;
								}
								return false;
							}),
						static_cast<float>(ExitDelaySeconds));
				if (!Handle.IsValid())
				{
					State.Reset();
					return Fail(
						OutError,
						TEXT("editor_host_exit_schedule_failed"));
				}
				{
					FScopeLock Lock(&State->Mutex);
					State->TickerHandle = Handle;
					State->State = EHostExitScheduleState::Scheduled;
				}
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
						TEXT("editor_host_exit_not_scheduled"));
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
				FString Phase = TEXT("exit_scheduled");
				FString ErrorCode;
				if (State->State
					== EHostExitScheduleState::CallbackRunning
					|| State->State == EHostExitScheduleState::
						ExitRequestedAwaitingExternalWitness)
				{
					Phase = TEXT(
						"exit_requested_awaiting_external_witness");
				}
				else if (State->State == EHostExitScheduleState::Cancelled)
				{
					WorkState = ELongRunningWorkState::ExitedFailure;
					Phase = TEXT("cancelled");
					ErrorCode = TEXT("editor_host_exit_cancelled");
				}
				else if (State->State == EHostExitScheduleState::NotStarted)
				{
					return Fail(
						OutError,
						TEXT("editor_host_exit_not_scheduled"));
				}

				TOptional<FLongRunningOperationSignal> Signal;
				if (Phase != State->LastSignalPhase)
				{
					const int64 NextRevision = State->SignalRevision + 1;
					if (!TryBuildSignal(
							State->Request,
							State->Policy,
							Phase,
							FString(),
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
					MoveTemp(ErrorCode),
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
						TEXT("editor_host_exit_not_scheduled"));
				}
				if (!IsInGameThread())
				{
					return Fail(
						OutError,
						TEXT("editor_host_exit_requires_game_thread"));
				}
				FTSTicker::FDelegateHandle Handle;
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
					if (State->State == EHostExitScheduleState::Cancelled)
					{
						return true;
					}
					if (State->State
						!= EHostExitScheduleState::Scheduled)
					{
						return Fail(
							OutError,
							TEXT("editor_host_exit_already_requested"));
					}
					State->State = EHostExitScheduleState::Cancelled;
					Handle = State->TickerHandle;
					State->TickerHandle.Reset();
				}
				if (Handle.IsValid())
				{
					FTSTicker::GetCoreTicker().RemoveTicker(Handle);
				}
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
						TEXT("editor_host_exit_not_scheduled"));
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
				if (State->State != EHostExitScheduleState::Cancelled)
				{
					return Fail(
						OutError,
						TEXT(
							"editor_host_exit_requires_external_witness"));
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
						TEXT("editor_host_exit_not_scheduled"));
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
						TEXT("editor_host_exit_release_before_join"));
				}
				if (State->bReleased)
				{
					return Fail(
						OutError,
						TEXT("editor_host_exit_already_released"));
				}
				State->bReleased = true;
				return true;
			}

		private:
			FUeAgentEditorSessionOperationTypedRequest Request;
			FUeAgentEditorSessionOperationPolicyDescriptor Policy;
			double ExitDelaySeconds = 0.0;
			TSharedRef<IUeAgentEditorHostExitTarget, ESPMode::ThreadSafe>
				ExitTarget;
			TSharedPtr<FHostExitSharedState, ESPMode::ThreadSafe> State;
		};

		class FPlatformEditorHostExitTarget final
			: public IUeAgentEditorHostExitTarget
		{
		public:
			explicit FPlatformEditorHostExitTarget(FString InEditorInstanceId)
				: EditorInstanceId(MoveTemp(InEditorInstanceId))
			{
			}

			const FString& GetEditorInstanceId() const override
			{
				return EditorInstanceId;
			}

			void RequestExit() override
			{
				FPlatformMisc::RequestExit(false);
			}

		private:
			FString EditorInstanceId;
		};
	}

	FUeAgentEditorSessionOperationPolicyDescriptor::
	FUeAgentEditorSessionOperationPolicyDescriptor(
		const EUeAgentEditorSessionOperationFamily InFamily,
		FString InPolicyId,
		FString InNativeMechanismId,
		FString InReadbackOracleId,
		const EUeAgentStatePlane InRequiredStatePlane,
		TArray<EUeAgentExecutionWorldType> InAllowedWorldTypes)
		: Family(InFamily)
		, PolicyId(MoveTemp(InPolicyId))
		, NativeMechanismId(MoveTemp(InNativeMechanismId))
		, ReadbackOracleId(MoveTemp(InReadbackOracleId))
		, RequiredStatePlane(InRequiredStatePlane)
		, AllowedWorldTypes(MoveTemp(InAllowedWorldTypes))
	{
	}

	EUeAgentEditorSessionOperationFamily
	FUeAgentEditorSessionOperationPolicyDescriptor::GetFamily() const
	{
		return Family;
	}

	const FString&
	FUeAgentEditorSessionOperationPolicyDescriptor::GetPolicyId() const
	{
		return PolicyId;
	}

	const FString&
	FUeAgentEditorSessionOperationPolicyDescriptor::
	GetNativeMechanismId() const
	{
		return NativeMechanismId;
	}

	const FString&
	FUeAgentEditorSessionOperationPolicyDescriptor::GetReadbackOracleId() const
	{
		return ReadbackOracleId;
	}

	EUeAgentStatePlane
	FUeAgentEditorSessionOperationPolicyDescriptor::
	GetRequiredStatePlane() const
	{
		return RequiredStatePlane;
	}

	const TArray<EUeAgentExecutionWorldType>&
	FUeAgentEditorSessionOperationPolicyDescriptor::
	GetAllowedWorldTypes() const
	{
		return AllowedWorldTypes;
	}

	bool FUeAgentEditorSessionOperationPolicyDescriptor::IsValid() const
	{
		return FindEditorSessionOperationFamilyContract(Family) != nullptr
			&& IsNormalizedNonEmpty(PolicyId)
			&& MatchesPolicyKind(Family, PolicyId)
			&& MechanismMatchesFamily(Family, NativeMechanismId)
			&& IsNormalizedNonEmpty(ReadbackOracleId)
			&& ReadbackOracleId.StartsWith(TEXT("uai.readback."))
			&& RequiredStatePlane != EUeAgentStatePlane::Invalid
			&& AllowedWorldTypesAreValid(AllowedWorldTypes);
	}

	FUeAgentEditorSessionOperationPolicyRegistration::
	FUeAgentEditorSessionOperationPolicyRegistration(
		FUeAgentEditorSessionOperationPolicyDescriptor InDescriptor,
		TSharedRef<IUeAgentEditorSessionOperationPortBuilder,
			ESPMode::ThreadSafe> InBuilder)
		: Descriptor(MoveTemp(InDescriptor))
		, Builder(MoveTemp(InBuilder))
	{
	}

	const FUeAgentEditorSessionOperationPolicyDescriptor&
	FUeAgentEditorSessionOperationPolicyRegistration::GetDescriptor() const
	{
		return Descriptor;
	}

	IUeAgentEditorSessionOperationPortBuilder&
	FUeAgentEditorSessionOperationPolicyRegistration::GetBuilder() const
	{
		return Builder.Get();
	}

	FUeAgentEditorSessionOperationProductionPortFactory::
	FUeAgentEditorSessionOperationProductionPortFactory(
		TArray<FUeAgentEditorSessionOperationPolicyRegistration>
			InRegistrations)
		: Registrations(MoveTemp(InRegistrations))
	{
		Descriptors.Reserve(Registrations.Num());
		for (const FUeAgentEditorSessionOperationPolicyRegistration& Registration
			: Registrations)
		{
			Descriptors.Add(Registration.GetDescriptor());
		}
	}

	bool FUeAgentEditorSessionOperationProductionPortFactory::TryCreate(
		const TArray<FUeAgentEditorSessionOperationPolicyRegistration>&
			InRegistrations,
		TSharedPtr<FUeAgentEditorSessionOperationProductionPortFactory>&
			OutFactory,
		FExecutionMutationError& OutError)
	{
		OutFactory.Reset();
		if (InRegistrations.IsEmpty())
		{
			return Fail(
				OutError,
				TEXT("editor_session_operation_policy_catalog_empty"));
		}
		for (int32 Index = 0; Index < InRegistrations.Num(); ++Index)
		{
			const FUeAgentEditorSessionOperationPolicyDescriptor& Descriptor =
				InRegistrations[Index].GetDescriptor();
			if (!Descriptor.IsValid())
			{
				return Fail(
					OutError,
					TEXT("editor_session_operation_policy_invalid"),
					Descriptor.GetPolicyId());
			}
			for (int32 Other = 0; Other < Index; ++Other)
			{
				const FUeAgentEditorSessionOperationPolicyDescriptor& Existing =
					InRegistrations[Other].GetDescriptor();
				if (Existing.GetFamily() == Descriptor.GetFamily()
					&& Existing.GetPolicyId() == Descriptor.GetPolicyId())
				{
					return Fail(
						OutError,
						TEXT("editor_session_operation_policy_duplicate"),
						Descriptor.GetPolicyId());
				}
			}
		}
		OutFactory = MakeShareable(
			new FUeAgentEditorSessionOperationProductionPortFactory(
				InRegistrations));
		return true;
	}

	const TArray<FUeAgentEditorSessionOperationPolicyDescriptor>&
	FUeAgentEditorSessionOperationProductionPortFactory::
	GetPolicyDescriptors() const
	{
		return Descriptors;
	}

	bool FUeAgentEditorSessionOperationProductionPortFactory::
	TryCreateOperationPort(
		const FUeAgentEditorSessionOperationTypedRequest& Request,
		const FLongRunningOperationRequest& OperationRequest,
		TSharedPtr<ILongRunningOperationPort, ESPMode::ThreadSafe>& OutPort,
		FExecutionMutationError& OutError)
	{
		OutPort.Reset();
		const FUeAgentEditorSessionOperationPolicyRegistration* Registration =
			Registrations.FindByPredicate(
				[&Request](
					const FUeAgentEditorSessionOperationPolicyRegistration&
						Candidate)
				{
					return MatchesPolicy(
						Request, Candidate.GetDescriptor());
				});
		if (Registration == nullptr)
		{
			return Fail(
				OutError,
				TEXT("editor_session_operation_policy_not_registered"),
				Request.GetPolicyId());
		}
		if (!Request.IsValid())
		{
			return Fail(
				OutError,
				TEXT("editor_session_operation_request_invalid"));
		}
		if (!Registration->GetBuilder().TryCreatePort(
				Request,
				OperationRequest,
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
				TEXT("editor_session_operation_port_missing"));
	}

	FUeAgentEditorSessionNativeObservation::
	FUeAgentEditorSessionNativeObservation(
		const ELongRunningWorkState InState,
		FString InPhase,
		FString InErrorCode,
		FString InSessionRef)
		: State(InState)
		, Phase(MoveTemp(InPhase))
		, ErrorCode(MoveTemp(InErrorCode))
		, SessionRef(MoveTemp(InSessionRef))
	{
	}

	ELongRunningWorkState
	FUeAgentEditorSessionNativeObservation::GetState() const
	{
		return State;
	}

	const FString& FUeAgentEditorSessionNativeObservation::GetPhase() const
	{
		return Phase;
	}

	const FString&
	FUeAgentEditorSessionNativeObservation::GetErrorCode() const
	{
		return ErrorCode;
	}

	const FString&
	FUeAgentEditorSessionNativeObservation::GetSessionRef() const
	{
		return SessionRef;
	}

	bool FUeAgentEditorSessionNativeObservation::IsValid() const
	{
		if (State == ELongRunningWorkState::Invalid
			|| !IsNormalizedNonEmpty(Phase)
			|| (!SessionRef.IsEmpty()
				&& !IsNormalizedNonEmpty(SessionRef)))
		{
			return false;
		}
		if (State == ELongRunningWorkState::ExitedFailure
			|| State == ELongRunningWorkState::StateUnknown)
		{
			return IsNormalizedNonEmpty(ErrorCode);
		}
		return ErrorCode.IsEmpty();
	}

	bool TryCreateEditorSessionNativeResourcePort(
		const FUeAgentEditorSessionOperationTypedRequest& Request,
		const FUeAgentEditorSessionOperationPolicyDescriptor& Policy,
		TSharedRef<IUeAgentEditorSessionNativeSession, ESPMode::ThreadSafe>
			NativeSession,
		TSharedPtr<ILongRunningOperationPort, ESPMode::ThreadSafe>& OutPort,
		FExecutionMutationError& OutError)
	{
		OutPort.Reset();
		if (!Request.IsValid() || !Policy.IsValid()
			|| !MatchesPolicy(Request, Policy))
		{
			return Fail(
				OutError,
				TEXT("editor_session_native_resource_contract_invalid"));
		}
		if (Policy.GetNativeMechanismId()
			!= TEXT("native_session_resource"))
		{
			return Fail(
				OutError,
				TEXT("editor_session_native_mechanism_mismatch"));
		}
		OutPort = MakeShared<FNativeSessionResourcePort, ESPMode::ThreadSafe>(
			Request, Policy, MoveTemp(NativeSession));
		return true;
	}

	bool TryCreateEditorPlatformHostExitTarget(
		FString EditorInstanceId,
		TSharedPtr<IUeAgentEditorHostExitTarget, ESPMode::ThreadSafe>&
			OutTarget,
		FExecutionMutationError& OutError)
	{
		OutTarget.Reset();
		if (!IsNormalizedNonEmpty(EditorInstanceId))
		{
			return Fail(
				OutError,
				TEXT("editor_host_exit_instance_id_invalid"));
		}
		OutTarget = MakeShared<FPlatformEditorHostExitTarget,
			ESPMode::ThreadSafe>(MoveTemp(EditorInstanceId));
		return true;
	}

	bool TryCreateEditorHostExitSchedulePort(
		const FUeAgentEditorSessionOperationTypedRequest& Request,
		const FUeAgentEditorSessionOperationPolicyDescriptor& Policy,
		const double ExitDelaySeconds,
		TSharedRef<IUeAgentEditorHostExitTarget, ESPMode::ThreadSafe>
			ExitTarget,
		TSharedPtr<ILongRunningOperationPort, ESPMode::ThreadSafe>& OutPort,
		FExecutionMutationError& OutError)
	{
		OutPort.Reset();
		if (!Request.IsValid() || !Policy.IsValid()
			|| !MatchesPolicy(Request, Policy))
		{
			return Fail(
				OutError,
				TEXT("editor_host_exit_contract_invalid"));
		}
		if (Policy.GetNativeMechanismId() != TEXT("host_exit_schedule"))
		{
			return Fail(
				OutError,
				TEXT("editor_session_native_mechanism_mismatch"));
		}
		if (!FMath::IsFinite(ExitDelaySeconds)
			|| ExitDelaySeconds < 0.0
			|| ExitDelaySeconds > 300.0)
		{
			return Fail(
				OutError,
				TEXT("editor_host_exit_delay_invalid"));
		}
		if (!IsNormalizedNonEmpty(ExitTarget->GetEditorInstanceId()))
		{
			return Fail(
				OutError,
				TEXT("editor_host_exit_instance_id_invalid"));
		}
		OutPort = MakeShared<FHostExitSchedulePort, ESPMode::ThreadSafe>(
			Request,
			Policy,
			ExitDelaySeconds,
			MoveTemp(ExitTarget));
		return true;
	}
}

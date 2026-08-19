#include "DomainAdapters/CaptureMediaLongJob/UeAgentCaptureMediaLongJobProduction.h"

#include "Misc/ScopeLock.h"

namespace UE::UAI::DomainAdapters::CaptureMediaLongJob
{
	namespace
	{
		using namespace ArtifactPublisher;
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
			const EUeAgentCaptureMediaLongJobFamily Family,
			const FString& PolicyId)
		{
			switch (Family)
			{
			case EUeAgentCaptureMediaLongJobFamily::ImageCapture:
				return PolicyId.StartsWith(TEXT("policy:image-capture:"));
			case EUeAgentCaptureMediaLongJobFamily::MediaRecording:
				return PolicyId.StartsWith(TEXT("policy:media-recording:"));
			case EUeAgentCaptureMediaLongJobFamily::RenderResource:
				return PolicyId.StartsWith(TEXT("policy:render-operation:"));
			default:
				return false;
			}
		}

		bool MechanismMatchesFamily(
			const EUeAgentCaptureMediaLongJobFamily Family,
			const FString& MechanismId)
		{
			switch (Family)
			{
			case EUeAgentCaptureMediaLongJobFamily::ImageCapture:
				return MechanismId == TEXT("image_capture_resource");
			case EUeAgentCaptureMediaLongJobFamily::MediaRecording:
				return MechanismId == TEXT("media_recording_resource");
			case EUeAgentCaptureMediaLongJobFamily::RenderResource:
				return MechanismId == TEXT("gpu_render_resource");
			default:
				return false;
			}
		}

		bool AllowedWorldTypesAreValid(
			const TArray<EUeAgentExecutionWorldType>& WorldTypes)
		{
			if (WorldTypes.IsEmpty())
			{
				return false;
			}
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
			const FUeAgentCaptureMediaLongJobTypedRequest& Request,
			const FUeAgentCaptureMediaLongJobPolicyDescriptor& Policy)
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
			const FUeAgentCaptureMediaLongJobPolicyDescriptor& Policy,
			FExecutionMutationError& OutError)
		{
			if (!Request.IsValid())
			{
				return Fail(
					OutError,
					TEXT("capture_media_long_job_operation_request_invalid"));
			}
			const FUeAgentExecutionContextRef& Context =
				Request.GetExecutionContext();
			if (Context.GetStatePlane() != Policy.GetRequiredStatePlane())
			{
				return Fail(
					OutError,
					TEXT("capture_media_long_job_context_state_plane_mismatch"));
			}
			if (!Policy.GetAllowedWorldTypes().Contains(Context.GetWorldType()))
			{
				return Fail(
					OutError,
					TEXT("capture_media_long_job_context_world_type_mismatch"));
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
					OutError, TEXT("capture_media_long_job_operation_id_mismatch"));
			}
			if (!(ExpectedIdentity == Identity))
			{
				return Fail(
					OutError, TEXT("capture_media_long_job_work_identity_mismatch"));
			}
			return true;
		}

		bool TryBuildSignal(
			const FUeAgentCaptureMediaLongJobTypedRequest& Request,
			const FUeAgentCaptureMediaLongJobPolicyDescriptor& Policy,
			const FUeAgentCaptureMediaNativeObservation& Observation,
			const int64 Revision,
			FUeAgentResourceTracker& Resources,
			TOptional<FLongRunningOperationSignal>& OutSignal,
			FExecutionMutationError& OutError)
		{
			const FUeAgentCaptureMediaLongJobFamilyContract* Contract =
				FindCaptureMediaLongJobFamilyContract(Request.GetFamily());
			if (Contract == nullptr)
			{
				return Fail(
					OutError, TEXT("capture_media_long_job_family_invalid"));
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
			if (!AddString(TEXT("phase"), Observation.GetPhase())
				|| !AddString(TEXT("subject_ref"), Request.GetSubjectRef())
				|| !AddString(TEXT("policy_id"), Policy.GetPolicyId())
				|| !AddString(
					TEXT("readback_oracle_id"),
					Policy.GetReadbackOracleId()))
			{
				return Fail(
					OutError,
					TEXT("capture_media_long_job_signal_value_failed"),
					StateError.Code);
			}
			if (Observation.GetArtifact().IsSet())
			{
				FUeAgentCanonicalValuePtr ArtifactValue;
				FFileArtifactPublishError ArtifactError;
				if (!TryProjectFileArtifactReference(
					Observation.GetArtifact().GetValue(),
					Resources,
					ArtifactValue,
					ArtifactError))
				{
					return Fail(
						OutError,
						TEXT("capture_media_long_job_artifact_ref_invalid"),
						ArtifactError.GetCode());
				}
				Items.Emplace(TEXT("artifact_ref"), ArtifactValue.ToSharedRef());
			}
			FUeAgentCanonicalValuePtr Payload;
			if (!FUeAgentCanonicalValue::TryObject(
				Items, Resources, Payload, StateError))
			{
				return Fail(
					OutError,
					TEXT("capture_media_long_job_signal_payload_failed"),
					StateError.Code);
			}
			OutSignal.Emplace(
				Contract->GetSignalTypeId(), Revision, Payload.ToSharedRef());
			return true;
		}

		class FCaptureMediaNativeResourcePort final
			: public ILongRunningOperationPort
		{
		public:
			FCaptureMediaNativeResourcePort(
				FUeAgentCaptureMediaLongJobTypedRequest InRequest,
				FUeAgentCaptureMediaLongJobPolicyDescriptor InPolicy,
				TSharedRef<
					IUeAgentCaptureMediaNativeSession,
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
						TEXT("capture_media_long_job_already_started"));
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
						TEXT("capture_media_native_resource_id_invalid"));
				}
				OperationId = OperationRequest.GetOperationId();
				Identity = FLongRunningWorkIdentity(
					TEXT("native:capture-media:") + OperationId
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
				FUeAgentCaptureMediaNativeObservation NativeObservation;
				if (!NativeSession->Observe(NativeObservation, OutError))
				{
					return false;
				}
				if (!NativeObservation.IsValid())
				{
					return Fail(
						OutError,
						TEXT("capture_media_native_observation_invalid"));
				}
				FString SignalKey = NativeObservation.GetPhase();
				if (NativeObservation.GetArtifact().IsSet())
				{
					SignalKey += TEXT("\n")
						+ NativeObservation.GetArtifact()->GetArtifactId();
				}
				TOptional<FLongRunningOperationSignal> Signal;
				if (SignalKey != LastSignalKey)
				{
					const int64 NextRevision = SignalRevision + 1;
					if (!TryBuildSignal(
						Request,
						Policy,
						NativeObservation,
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
						TEXT("capture_media_stop_reason_invalid"));
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
					&& LastObservedState != ELongRunningWorkState::ExitedFailure)
				{
					return Fail(
						OutError,
						TEXT("capture_media_join_before_native_exit"));
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
						TEXT("capture_media_release_before_join"));
				}
				if (bReleased)
				{
					return Fail(
						OutError, TEXT("capture_media_already_released"));
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
						TEXT("capture_media_long_job_not_started"));
			}

			FUeAgentCaptureMediaLongJobTypedRequest Request;
			FUeAgentCaptureMediaLongJobPolicyDescriptor Policy;
			TSharedRef<IUeAgentCaptureMediaNativeSession, ESPMode::ThreadSafe>
				NativeSession;
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
	}

	FUeAgentCaptureMediaLongJobPolicyDescriptor::
	FUeAgentCaptureMediaLongJobPolicyDescriptor(
		const EUeAgentCaptureMediaLongJobFamily InFamily,
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

	EUeAgentCaptureMediaLongJobFamily
	FUeAgentCaptureMediaLongJobPolicyDescriptor::GetFamily() const
	{
		return Family;
	}

	const FString&
	FUeAgentCaptureMediaLongJobPolicyDescriptor::GetPolicyId() const
	{
		return PolicyId;
	}

	const FString&
	FUeAgentCaptureMediaLongJobPolicyDescriptor::GetNativeMechanismId() const
	{
		return NativeMechanismId;
	}

	const FString&
	FUeAgentCaptureMediaLongJobPolicyDescriptor::GetReadbackOracleId() const
	{
		return ReadbackOracleId;
	}

	EUeAgentStatePlane
	FUeAgentCaptureMediaLongJobPolicyDescriptor::GetRequiredStatePlane() const
	{
		return RequiredStatePlane;
	}

	const TArray<EUeAgentExecutionWorldType>&
	FUeAgentCaptureMediaLongJobPolicyDescriptor::GetAllowedWorldTypes() const
	{
		return AllowedWorldTypes;
	}

	bool FUeAgentCaptureMediaLongJobPolicyDescriptor::IsValid() const
	{
		return FindCaptureMediaLongJobFamilyContract(Family) != nullptr
			&& IsNormalizedNonEmpty(PolicyId)
			&& MatchesPolicyKind(Family, PolicyId)
			&& MechanismMatchesFamily(Family, NativeMechanismId)
			&& IsNormalizedNonEmpty(ReadbackOracleId)
			&& ReadbackOracleId.StartsWith(TEXT("uai.readback."))
			&& RequiredStatePlane != EUeAgentStatePlane::Invalid
			&& AllowedWorldTypesAreValid(AllowedWorldTypes);
	}

	FUeAgentCaptureMediaLongJobPolicyRegistration::
	FUeAgentCaptureMediaLongJobPolicyRegistration(
		FUeAgentCaptureMediaLongJobPolicyDescriptor InDescriptor,
		TSharedRef<IUeAgentCaptureMediaLongJobPortBuilder,
			ESPMode::ThreadSafe> InBuilder)
		: Descriptor(MoveTemp(InDescriptor))
		, Builder(MoveTemp(InBuilder))
	{
	}

	const FUeAgentCaptureMediaLongJobPolicyDescriptor&
	FUeAgentCaptureMediaLongJobPolicyRegistration::GetDescriptor() const
	{
		return Descriptor;
	}

	IUeAgentCaptureMediaLongJobPortBuilder&
	FUeAgentCaptureMediaLongJobPolicyRegistration::GetBuilder() const
	{
		return Builder.Get();
	}

	FUeAgentCaptureMediaLongJobProductionPortFactory::
	FUeAgentCaptureMediaLongJobProductionPortFactory(
		TArray<FUeAgentCaptureMediaLongJobPolicyRegistration> InRegistrations)
		: Registrations(MoveTemp(InRegistrations))
	{
		Descriptors.Reserve(Registrations.Num());
		for (const FUeAgentCaptureMediaLongJobPolicyRegistration& Registration
			: Registrations)
		{
			Descriptors.Add(Registration.GetDescriptor());
		}
	}

	bool FUeAgentCaptureMediaLongJobProductionPortFactory::TryCreate(
		const TArray<FUeAgentCaptureMediaLongJobPolicyRegistration>&
			InRegistrations,
		TSharedPtr<FUeAgentCaptureMediaLongJobProductionPortFactory>& OutFactory,
		FExecutionMutationError& OutError)
	{
		OutFactory.Reset();
		if (InRegistrations.IsEmpty())
		{
			return Fail(
				OutError,
				TEXT("capture_media_long_job_policy_catalog_empty"));
		}
		for (int32 Index = 0; Index < InRegistrations.Num(); ++Index)
		{
			const FUeAgentCaptureMediaLongJobPolicyDescriptor& Descriptor =
				InRegistrations[Index].GetDescriptor();
			if (!Descriptor.IsValid())
			{
				return Fail(
					OutError,
					TEXT("capture_media_long_job_policy_invalid"),
					Descriptor.GetPolicyId());
			}
			for (int32 Other = 0; Other < Index; ++Other)
			{
				const FUeAgentCaptureMediaLongJobPolicyDescriptor& Existing =
					InRegistrations[Other].GetDescriptor();
				if (Existing.GetFamily() == Descriptor.GetFamily()
					&& Existing.GetPolicyId() == Descriptor.GetPolicyId())
				{
					return Fail(
						OutError,
						TEXT("capture_media_long_job_policy_duplicate"),
						Descriptor.GetPolicyId());
				}
			}
		}
		OutFactory = MakeShareable(
			new FUeAgentCaptureMediaLongJobProductionPortFactory(
				InRegistrations));
		return true;
	}

	const TArray<FUeAgentCaptureMediaLongJobPolicyDescriptor>&
	FUeAgentCaptureMediaLongJobProductionPortFactory::
	GetPolicyDescriptors() const
	{
		return Descriptors;
	}

	bool FUeAgentCaptureMediaLongJobProductionPortFactory::
	TryCreateOperationPort(
		const FUeAgentCaptureMediaLongJobTypedRequest& Request,
		TSharedPtr<ILongRunningOperationPort, ESPMode::ThreadSafe>& OutPort,
		FExecutionMutationError& OutError)
	{
		OutPort.Reset();
		const FUeAgentCaptureMediaLongJobPolicyRegistration* Registration =
			Registrations.FindByPredicate(
				[&Request](
					const FUeAgentCaptureMediaLongJobPolicyRegistration& Candidate)
				{
					return MatchesPolicy(Request, Candidate.GetDescriptor());
				});
		if (Registration == nullptr)
		{
			return Fail(
				OutError,
				TEXT("capture_media_long_job_policy_not_registered"),
				Request.GetPolicyId());
		}
		if (!Request.IsValid())
		{
			return Fail(
				OutError, TEXT("capture_media_long_job_request_invalid"));
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
				OutError, TEXT("capture_media_long_job_operation_port_missing"));
	}

	FUeAgentCaptureMediaNativeObservation::
	FUeAgentCaptureMediaNativeObservation(
		const ELongRunningWorkState InState,
		FString InPhase,
		FString InErrorCode,
		TOptional<FFileArtifactReference> InArtifact)
		: State(InState)
		, Phase(MoveTemp(InPhase))
		, ErrorCode(MoveTemp(InErrorCode))
		, Artifact(MoveTemp(InArtifact))
	{
	}

	ELongRunningWorkState
	FUeAgentCaptureMediaNativeObservation::GetState() const
	{
		return State;
	}

	const FString& FUeAgentCaptureMediaNativeObservation::GetPhase() const
	{
		return Phase;
	}

	const FString& FUeAgentCaptureMediaNativeObservation::GetErrorCode() const
	{
		return ErrorCode;
	}

	const TOptional<FFileArtifactReference>&
	FUeAgentCaptureMediaNativeObservation::GetArtifact() const
	{
		return Artifact;
	}

	bool FUeAgentCaptureMediaNativeObservation::IsValid() const
	{
		if (State == ELongRunningWorkState::Invalid
			|| !IsNormalizedNonEmpty(Phase)
			|| (Artifact.IsSet() && !Artifact->IsValid()))
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

	bool TryCreateCaptureMediaNativeResourcePort(
		const FUeAgentCaptureMediaLongJobTypedRequest& Request,
		const FUeAgentCaptureMediaLongJobPolicyDescriptor& Policy,
		TSharedRef<IUeAgentCaptureMediaNativeSession, ESPMode::ThreadSafe>
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
				TEXT("capture_media_native_resource_contract_invalid"));
		}
		OutPort = MakeShared<FCaptureMediaNativeResourcePort,
			ESPMode::ThreadSafe>(Request, Policy, MoveTemp(NativeSession));
		return true;
	}
}


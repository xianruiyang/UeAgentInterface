#include "DomainAdapters/CaptureMediaLongJob/UeAgentCaptureMediaLongJobProduction.h"
#include "Core/Contract/UeAgentDiagnostic.h"
#include "DomainAdapters/CaptureMediaLongJob/UeAgentCaptureMediaDiagnostics.h"

#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/ScopeLock.h"

namespace UE::UAI::DomainAdapters::CaptureMediaLongJob
{
	using namespace CaptureDiagnostics;
	namespace
	{
		using namespace ArtifactPublisher;
		using namespace ContextResource;
		using namespace Contract;
		using namespace ExecutionMutation;
		using namespace StateSemantic;

		bool Fail(UE::UAI::Contract::FDiagnostic& OutError, UE::UAI::Contract::FDiagnostic Diagnostic, const FString& Subject = FString())
        {
            OutError = MoveTemp(Diagnostic);
            if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = Subject;
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

		FString WorldTypeName(EUeAgentExecutionWorldType Value)
		{
			switch (Value)
			{
			case EUeAgentExecutionWorldType::Invalid: return TEXT("Invalid");
			case EUeAgentExecutionWorldType::Editor: return TEXT("Editor");
			case EUeAgentExecutionWorldType::Pie: return TEXT("Pie");
			case EUeAgentExecutionWorldType::SimulateInEditor: return TEXT("SimulateInEditor");
			case EUeAgentExecutionWorldType::Game: return TEXT("Game");
			case EUeAgentExecutionWorldType::Preview: return TEXT("Preview");
			default: return TEXT("Unknown(") + ::LexToString(static_cast<int32>(Value)) + TEXT(")");
			}
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
				return Fail(OutError, Request.GetValidationDiagnostic());
			}
			const FUeAgentExecutionContextRef& Context =
				Request.GetExecutionContext();
			if (Context.GetStatePlane() != Policy.GetRequiredStatePlane())
			{
				return Fail(OutError, Native(TEXT("capture_media_long_job_context_state_plane_mismatch"),
					TEXT("The execution context state plane does not match the capture policy."), TEXT("execution"), TEXT("state_plane"),
					MakeShared<FJsonValueString>(LexToString(Context.GetStatePlane())), MakeShared<FJsonValueString>(LexToString(Policy.GetRequiredStatePlane()))));
			}
			if (!Policy.GetAllowedWorldTypes().Contains(Context.GetWorldType()))
			{
				TArray<TSharedPtr<FJsonValue>> Allowed;
				for (const auto WorldType : Policy.GetAllowedWorldTypes()) Allowed.Add(MakeShared<FJsonValueString>(WorldTypeName(WorldType)));
				return Fail(OutError, Native(TEXT("capture_media_long_job_context_world_type_mismatch"),
					TEXT("The execution context world type is not allowed by the capture policy."), TEXT("execution"), TEXT("world_type"),
					MakeShared<FJsonValueString>(WorldTypeName(Context.GetWorldType())), MakeShared<FJsonValueArray>(MoveTemp(Allowed))));
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
				OutError = Native(TEXT("capture_media_long_job_operation_id_mismatch"),
					TEXT("The lifecycle call belongs to a different operation."), TEXT("execution"), TEXT("operation_identity_matches"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				OutError.ActualEvidence = MakeShared<FJsonValueString>(OperationId);
				OutError.ExpectedEvidence = MakeShared<FJsonValueString>(ExpectedOperationId);
				return false;
			}
			if (!(ExpectedIdentity == Identity))
			{
				OutError = Native(TEXT("capture_media_long_job_work_identity_mismatch"),
					TEXT("The lifecycle call does not match the active native work identity and generation."), TEXT("execution"), TEXT("work_identity_matches"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				OutError.ActualEvidence = Facts({ { TEXT("resource_id"), MakeShared<FJsonValueString>(Identity.GetResourceId()) }, { TEXT("generation"), Number(Identity.GetGeneration()) } });
				OutError.ExpectedEvidence = Facts({ { TEXT("resource_id"), MakeShared<FJsonValueString>(ExpectedIdentity.GetResourceId()) }, { TEXT("generation"), Number(ExpectedIdentity.GetGeneration()) } });
				return false;
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
				return Fail(OutError, Native(TEXT("capture_media_long_job_family_invalid"),
					TEXT("No capture/media family contract exists for the native observation."), TEXT("execution"), TEXT("family"),
					Number(static_cast<int32>(Request.GetFamily())), MakeShared<FJsonValueString>(TEXT("image capture, media recording, or render resource"))));
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
				return Fail(OutError, StateError);
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
					return Fail(OutError, UE::UAI::Contract::FDiagnostic::WrapFailure(TEXT("capture_media_long_job_artifact_ref_invalid"), ArtifactError, TEXT("execution")));
				}
				Items.Emplace(TEXT("artifact_ref"), ArtifactValue.ToSharedRef());
			}
			if (!Observation.GetAdditionalArtifacts().IsEmpty())
			{
				TArray<FUeAgentCanonicalValueRef> ArtifactValues;
				ArtifactValues.Reserve(
					Observation.GetAdditionalArtifacts().Num());
				for (const FFileArtifactReference& Artifact
					: Observation.GetAdditionalArtifacts())
				{
					FUeAgentCanonicalValuePtr ArtifactValue;
					FFileArtifactPublishError ArtifactError;
					if (!TryProjectFileArtifactReference(
							Artifact,
							Resources,
							ArtifactValue,
							ArtifactError))
					{
						return Fail(OutError, UE::UAI::Contract::FDiagnostic::WrapFailure(TEXT("capture_media_long_job_additional_artifact_ref_invalid"), ArtifactError, TEXT("execution")));
					}
					ArtifactValues.Add(ArtifactValue.ToSharedRef());
				}
				FUeAgentCanonicalValuePtr AdditionalArtifacts;
				if (!FUeAgentCanonicalValue::TryArray(
						ArtifactValues,
						Resources,
						AdditionalArtifacts,
						StateError))
				{
					return Fail(OutError, StateError);
				}
				Items.Emplace(
					TEXT("additional_artifact_refs"),
					AdditionalArtifacts.ToSharedRef());
			}
			if (Observation.GetCaptureReceipt().IsValid())
			{
				FUeAgentCanonicalValuePtr Receipt;
				UE::UAI::Contract::FDiagnostic ReceiptError;
				if (!CanonicalJson::TryFromJsonValue(
						MakeShared<FJsonValueObject>(
							Observation.GetCaptureReceipt()),
						Resources,
						Receipt,
						ReceiptError))
				{
					return Fail(OutError, MoveTemp(ReceiptError));
				}
				Items.Emplace(TEXT("capture_receipt"), Receipt.ToSharedRef());
			}
			FUeAgentCanonicalValuePtr Payload;
			if (!FUeAgentCanonicalValue::TryObject(
				Items, Resources, Payload, StateError))
			{
				return Fail(OutError, StateError);
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
					return Fail(OutError, Native(TEXT("capture_media_long_job_already_started"),
						TEXT("The capture/media native operation has already started."), TEXT("execution"), TEXT("started"),
						MakeShared<FJsonValueBoolean>(bStarted), MakeShared<FJsonValueBoolean>(false)));
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
					OutError = Native(TEXT("capture_media_native_resource_id_invalid"),
						TEXT("The native session returned an empty resource ID or one with surrounding whitespace."), TEXT("execution"), TEXT("resource_id_valid"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
					OutError.ActualEvidence = MakeShared<FJsonValueString>(NativeResourceId);
					OutError.ExpectedEvidence = MakeShared<FJsonValueString>(TEXT("nonempty string without surrounding whitespace"));
					return false;
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
				if (!NativeObservation.TryValidate(OutError)) return false;
				FString SignalKey = NativeObservation.GetPhase();
				if (NativeObservation.GetArtifact().IsSet())
				{
					SignalKey += TEXT("\n")
						+ NativeObservation.GetArtifact()->GetArtifactId();
				}
				for (const FFileArtifactReference& Artifact
					: NativeObservation.GetAdditionalArtifacts())
				{
					SignalKey += TEXT("\n") + Artifact.GetArtifactId();
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
						OutError.Append(NativeObservation.GetDiagnostic());
						return false;
					}
					LastSignalKey = SignalKey;
					SignalRevision = NextRevision;
				}
				LastObservedState = NativeObservation.GetState();
				OutObservation = FLongRunningWorkObservation(Identity.GetGeneration(),
			NativeObservation.GetState(),
			NativeObservation.GetDiagnostic(),
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
					return Fail(OutError, Native(TEXT("capture_media_stop_reason_invalid"),
						TEXT("The native stop reason must be nonempty and have no surrounding whitespace."), TEXT("execution"), TEXT("stop_reason"),
						MakeShared<FJsonValueString>(Reason), MakeShared<FJsonValueString>(TEXT("nonempty string without surrounding whitespace"))));
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
					return Fail(OutError, Native(TEXT("capture_media_join_before_native_exit"),
						TEXT("The native capture/media operation cannot be joined before it exits."), TEXT("execution"), TEXT("work_state"),
						MakeShared<FJsonValueString>(LexToString(LastObservedState)),
						MakeShared<FJsonValueString>(TEXT("exited_success or exited_failure"))));
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
					return Fail(OutError, Native(TEXT("capture_media_release_before_join"),
						TEXT("The native capture/media operation must be joined before release."), TEXT("execution"), TEXT("joined"),
						MakeShared<FJsonValueBoolean>(bJoined), MakeShared<FJsonValueBoolean>(true)));
				}
				if (bReleased)
				{
					return Fail(OutError, Native(TEXT("capture_media_already_released"),
						TEXT("The native capture/media operation has already been released."), TEXT("execution"), TEXT("released"),
						MakeShared<FJsonValueBoolean>(bReleased), MakeShared<FJsonValueBoolean>(false)));
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
					: Fail(OutError, Native(TEXT("capture_media_long_job_not_started"),
						TEXT("The capture/media native operation has not started."), TEXT("execution"), TEXT("started"),
						MakeShared<FJsonValueBoolean>(bStarted), MakeShared<FJsonValueBoolean>(true)));
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
		UE::UAI::Contract::FDiagnostic Error;
		return TryValidate(Error);
	}

	bool FUeAgentCaptureMediaLongJobPolicyDescriptor::TryValidate(UE::UAI::Contract::FDiagnostic& OutError) const
	{
		OutError.Reset();
		const auto Invalid = [&](const FString& Reason, const FString& Field, const TSharedPtr<FJsonValue>& Actual, const TSharedPtr<FJsonValue>& Expected)
		{
			OutError = Native(TEXT("capture_media_long_job_policy_invalid"), Reason, TEXT("execution"), Field, Actual, Expected);
			OutError.CandidateName = PolicyId;
			return false;
		};
		if (!FindCaptureMediaLongJobFamilyContract(Family))
			return Invalid(TEXT("The capture/media policy family is unsupported."), TEXT("family"), Number(static_cast<int32>(Family)),
				MakeShared<FJsonValueString>(TEXT("image capture, media recording, or render resource")));
		if (!IsNormalizedNonEmpty(PolicyId))
			return Invalid(TEXT("The capture/media policy ID must be nonempty and have no surrounding whitespace."), TEXT("policy_id"),
				MakeShared<FJsonValueString>(PolicyId), MakeShared<FJsonValueString>(TEXT("nonempty string without surrounding whitespace")));
		if (!MatchesPolicyKind(Family, PolicyId))
			return Invalid(TEXT("The capture/media policy ID prefix does not match its family."), TEXT("policy_id"),
				MakeShared<FJsonValueString>(PolicyId), MakeShared<FJsonValueString>(Family == EUeAgentCaptureMediaLongJobFamily::ImageCapture ? TEXT("policy:image-capture:")
					: Family == EUeAgentCaptureMediaLongJobFamily::MediaRecording ? TEXT("policy:media-recording:") : TEXT("policy:render-operation:")));
		if (!MechanismMatchesFamily(Family, NativeMechanismId))
		{
			Invalid(TEXT("The native capture mechanism does not match the policy family."), TEXT("native_mechanism_matches_family"),
				MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			OutError.ActualEvidence = MakeShared<FJsonValueString>(NativeMechanismId);
			OutError.ExpectedEvidence = MakeShared<FJsonValueString>(Family == EUeAgentCaptureMediaLongJobFamily::ImageCapture ? TEXT("image_capture_resource")
				: Family == EUeAgentCaptureMediaLongJobFamily::MediaRecording ? TEXT("media_recording_resource") : TEXT("gpu_render_resource"));
			return false;
		}
		if (!IsNormalizedNonEmpty(ReadbackOracleId) || !ReadbackOracleId.StartsWith(TEXT("uai.readback.")))
		{
			Invalid(TEXT("The capture policy requires a normalized readback oracle ID with the registered prefix."), TEXT("readback_oracle_id_valid"),
				MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			OutError.ActualEvidence = MakeShared<FJsonValueString>(ReadbackOracleId);
			OutError.ExpectedEvidence = MakeShared<FJsonValueString>(TEXT("uai.readback.<name>, without surrounding whitespace"));
			return false;
		}
		if (RequiredStatePlane == EUeAgentStatePlane::Invalid)
			return Invalid(TEXT("The capture policy requires a valid execution state plane."), TEXT("state_plane"),
				MakeShared<FJsonValueString>(LexToString(RequiredStatePlane)), MakeShared<FJsonValueString>(TEXT("non-invalid state plane")));
		if (AllowedWorldTypes.IsEmpty())
			return Invalid(TEXT("The capture policy must allow at least one world type."), TEXT("allowed_world_types/count"), Number(0),
				MakeShared<FJsonValueString>(TEXT("greater than zero")));
		for (int32 Index = 0; Index < AllowedWorldTypes.Num(); ++Index)
		{
			const FString Field = TEXT("allowed_world_types/") + ::LexToString(Index);
			if (AllowedWorldTypes[Index] == EUeAgentExecutionWorldType::Invalid)
				return Invalid(TEXT("The capture policy contains an invalid allowed world type."), Field,
					MakeShared<FJsonValueString>(WorldTypeName(AllowedWorldTypes[Index])), MakeShared<FJsonValueString>(TEXT("non-invalid world type")));
			for (int32 Other = 0; Other < Index; ++Other)
				if (AllowedWorldTypes[Other] == AllowedWorldTypes[Index])
					return Invalid(TEXT("The capture policy repeats an allowed world type."), Field,
						MakeShared<FJsonValueString>(WorldTypeName(AllowedWorldTypes[Index])),
						MakeShared<FJsonValueString>(FString::Printf(TEXT("distinct from allowed_world_types/%d"), Other)));
		}
		return true;
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
			return Fail(OutError, Native(TEXT("capture_media_long_job_policy_catalog_empty"),
				TEXT("The capture/media production factory requires at least one policy registration."), TEXT("execution"), TEXT("policy_count"),
				Number(InRegistrations.Num()), MakeShared<FJsonValueString>(TEXT("greater than zero"))));
		}
		for (int32 Index = 0; Index < InRegistrations.Num(); ++Index)
		{
			const FUeAgentCaptureMediaLongJobPolicyDescriptor& Descriptor =
				InRegistrations[Index].GetDescriptor();
			if (!Descriptor.TryValidate(OutError)) return false;
			for (int32 Other = 0; Other < Index; ++Other)
			{
				const FUeAgentCaptureMediaLongJobPolicyDescriptor& Existing =
					InRegistrations[Other].GetDescriptor();
				if (Existing.GetFamily() == Descriptor.GetFamily()
					&& Existing.GetPolicyId() == Descriptor.GetPolicyId())
				{
					return Fail(OutError, Native(TEXT("capture_media_long_job_policy_duplicate"),
						TEXT("The policy catalog repeats the same family and policy ID."), TEXT("execution"),
						TEXT("registrations/") + ::LexToString(Index),
						Facts({ { TEXT("family"), Number(static_cast<int32>(Descriptor.GetFamily())) },
							{ TEXT("policy_id"), MakeShared<FJsonValueString>(Descriptor.GetPolicyId()) } }),
						MakeShared<FJsonValueString>(FString::Printf(TEXT("distinct from registrations/%d"), Other))));
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
			return Fail(OutError, Native(TEXT("capture_media_long_job_policy_not_registered"),
				TEXT("No registered capture/media policy matches this family and policy ID."), TEXT("execution"), TEXT("policy"),
				Facts({ { TEXT("family"), Number(static_cast<int32>(Request.GetFamily())) },
					{ TEXT("policy_id"), MakeShared<FJsonValueString>(Request.GetPolicyId()) } }),
				MakeShared<FJsonValueString>(TEXT("registered family and policy ID pair"))));
		}
		if (!Request.TryValidate(OutError)) return false;
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
			: Fail(OutError, Native(TEXT("capture_media_long_job_operation_port_missing"),
				TEXT("The registered capture/media builder returned no execution port."), TEXT("execution"), TEXT("execution_port_available"),
				MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)));
	}

	FUeAgentCaptureMediaNativeObservation::
	FUeAgentCaptureMediaNativeObservation(
		const ELongRunningWorkState InState,
		FString InPhase,
		UE::UAI::Contract::FDiagnostic InDiagnostic,
		TOptional<FFileArtifactReference> InArtifact,
		TArray<FFileArtifactReference> InAdditionalArtifacts,
		TSharedPtr<FJsonObject> InCaptureReceipt)
		: State(InState)
		, Phase(MoveTemp(InPhase))
		, Diagnostic(MoveTemp(InDiagnostic))
		, Artifact(MoveTemp(InArtifact))
		, AdditionalArtifacts(MoveTemp(InAdditionalArtifacts))
		, CaptureReceipt(MoveTemp(InCaptureReceipt))
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

	const UE::UAI::Contract::FDiagnostic& FUeAgentCaptureMediaNativeObservation::GetDiagnostic() const
	{
		return Diagnostic;
	}

	const TOptional<FFileArtifactReference>&
	FUeAgentCaptureMediaNativeObservation::GetArtifact() const
	{
		return Artifact;
	}

	const TArray<FFileArtifactReference>&
	FUeAgentCaptureMediaNativeObservation::GetAdditionalArtifacts() const
	{
		return AdditionalArtifacts;
	}

	const TSharedPtr<FJsonObject>&
	FUeAgentCaptureMediaNativeObservation::GetCaptureReceipt() const
	{
		return CaptureReceipt;
	}

	bool FUeAgentCaptureMediaNativeObservation::IsValid() const
	{
		UE::UAI::Contract::FDiagnostic Error;
		return TryValidate(Error);
	}

	bool FUeAgentCaptureMediaNativeObservation::TryValidate(UE::UAI::Contract::FDiagnostic& OutError) const
	{
		OutError.Reset();
		const auto Reject = [this, &OutError](UE::UAI::Contract::FDiagnostic Error)
		{
			Error.Append(Diagnostic);
			OutError = MoveTemp(Error);
			return false;
		};
		if (State == ELongRunningWorkState::Invalid)
			return Reject(Native(TEXT("capture_media_native_observation_invalid"), TEXT("The native capture observation has an invalid work state."),
				TEXT("execution"), TEXT("work_state"), MakeShared<FJsonValueString>(LexToString(State)),
				MakeShared<FJsonValueString>(TEXT("non-invalid work state"))));
		if (!IsNormalizedNonEmpty(Phase))
			return Reject(Native(TEXT("capture_media_native_observation_invalid"), TEXT("The native capture phase must be nonempty and have no surrounding whitespace."),
				TEXT("execution"), TEXT("phase"), MakeShared<FJsonValueString>(Phase), MakeShared<FJsonValueString>(TEXT("nonempty string without surrounding whitespace"))));
		if (Artifact.IsSet() && !Artifact->IsValid())
		{
			auto Error = UE::UAI::Contract::FDiagnostic::WrapFailure(TEXT("capture_media_native_observation_invalid"),
				*Artifact->GetValidationDiagnostic(), TEXT("execution"), TEXT("The native capture observation contains an invalid primary artifact."));
			Error.SourceKind = TEXT("native_result");
			Error.PropertyPath = TEXT("artifact");
			return Reject(MoveTemp(Error));
		}
		for (int32 Index = 0; Index < AdditionalArtifacts.Num(); ++Index)
			if (!AdditionalArtifacts[Index].IsValid())
			{
				auto Error = UE::UAI::Contract::FDiagnostic::WrapFailure(TEXT("capture_media_native_observation_invalid"),
					*AdditionalArtifacts[Index].GetValidationDiagnostic(), TEXT("execution"), TEXT("The native capture observation contains an invalid additional artifact."));
				Error.SourceKind = TEXT("native_result");
				Error.PropertyPath = TEXT("additional_artifacts/") + ::LexToString(Index);
				return Reject(MoveTemp(Error));
			}
		const bool bFailureState = State == ELongRunningWorkState::ExitedFailure || State == ELongRunningWorkState::StateUnknown;
		if (bFailureState ? !IsNormalizedNonEmpty(Diagnostic.Code) : !Diagnostic.IsEmpty())
		{
			auto Error = Native(TEXT("capture_media_native_observation_invalid"),
				bFailureState ? TEXT("A failed or unknown native capture state requires a diagnostic with a normalized nonempty code.")
					: TEXT("A native capture state that is not failed or unknown must not contain a failure diagnostic."),
				TEXT("execution"), TEXT("diagnostic/code"), MakeShared<FJsonValueString>(Diagnostic.Code),
				MakeShared<FJsonValueString>(bFailureState ? TEXT("nonempty code without surrounding whitespace") : TEXT("empty")));
			return Reject(MoveTemp(Error));
		}
		return true;
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
		if (!Request.TryValidate(OutError) || !Policy.TryValidate(OutError)) return false;
		if (!MatchesPolicy(Request, Policy))
			return Fail(OutError, Native(TEXT("capture_media_native_resource_contract_invalid"),
				TEXT("The native capture request family and policy ID do not match the selected policy."), TEXT("execution"), TEXT("policy_binding"),
				Facts({ { TEXT("family"), Number(static_cast<int32>(Request.GetFamily())) },
					{ TEXT("policy_id"), MakeShared<FJsonValueString>(Request.GetPolicyId()) } }),
				Facts({ { TEXT("family"), Number(static_cast<int32>(Policy.GetFamily())) },
					{ TEXT("policy_id"), MakeShared<FJsonValueString>(Policy.GetPolicyId()) } })));
		OutPort = MakeShared<FCaptureMediaNativeResourcePort,
			ESPMode::ThreadSafe>(Request, Policy, MoveTemp(NativeSession));
		return true;
	}
}

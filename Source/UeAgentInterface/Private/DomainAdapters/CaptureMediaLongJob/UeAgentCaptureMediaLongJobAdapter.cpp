#include "DomainAdapters/CaptureMediaLongJob/UeAgentCaptureMediaLongJobAdapter.h"

namespace UE::UAI::DomainAdapters::CaptureMediaLongJob
{
	namespace
	{
		using namespace ExecutionMutation;
		using namespace StateSemantic;

		bool Fail(
			FExecutionMutationError& OutError,
			FString Code,
			FString Detail = FString())
		{
			OutError = FExecutionMutationError(
				MoveTemp(Code),
				MoveTemp(Detail));
			return false;
		}

		FString NormalizeFieldName(FString Value)
		{
			Value.TrimStartAndEndInline();
			Value.ToLowerInline();
			return Value;
		}

		bool IsForbiddenRequestField(const FString& Field)
		{
			static const TSet<FString> ForbiddenFields = {
				TEXT("command_id"),
				TEXT("http_route"),
				TEXT("timeout_seconds"),
				TEXT("process_handle"),
				TEXT("capture_handle"),
				TEXT("task_map"),
				TEXT("session_registry"),
				TEXT("owner_lease_id"),
				TEXT("server_session_id"),
				TEXT("raw_image_bytes"),
				TEXT("raw_audio_bytes"),
				TEXT("raw_video_bytes"),
				TEXT("base64_payload"),
				TEXT("delivery_status"),
				TEXT("business_outcome"),
				TEXT("use_current_viewport"),
				TEXT("use_selection"),
			};
			return ForbiddenFields.Contains(NormalizeFieldName(Field));
		}

		bool TryFindForbiddenRequestField(
			const FUeAgentCanonicalValueRef& Root,
			FString& OutField)
		{
			TArray<FUeAgentCanonicalValueRef> Pending;
			Pending.Add(Root);
			for (int32 Index = 0; Index < Pending.Num(); ++Index)
			{
				const FUeAgentCanonicalValueRef& Value = Pending[Index];
				switch (Value->GetKind())
				{
				case EUeAgentValueKind::Object:
					for (const TPair<FString, FUeAgentCanonicalValueRef>& Item
						: Value->GetObjectItems())
					{
						if (IsForbiddenRequestField(Item.Key))
						{
							OutField = Item.Key;
							return true;
						}
						Pending.Add(Item.Value);
					}
					break;
				case EUeAgentValueKind::Array:
					Pending.Append(Value->GetArrayItems());
					break;
				default:
					break;
				}
			}
			return false;
		}

		bool IsNormalizedNonEmpty(const FString& Value)
		{
			FString Normalized = Value;
			Normalized.TrimStartAndEndInline();
			return !Normalized.IsEmpty() && Normalized == Value;
		}

		bool MatchesSubjectKind(
			const EUeAgentCaptureMediaLongJobFamily Family,
			const FString& SubjectRef)
		{
			switch (Family)
			{
			case EUeAgentCaptureMediaLongJobFamily::ImageCapture:
				return SubjectRef.StartsWith(TEXT("capture-subject:"));
			case EUeAgentCaptureMediaLongJobFamily::MediaRecording:
				return SubjectRef.StartsWith(TEXT("media-source:"));
			case EUeAgentCaptureMediaLongJobFamily::RenderResource:
				return SubjectRef.StartsWith(TEXT("render-resource:"));
			default:
				return false;
			}
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

		bool TryValidateRequest(
			const FUeAgentCaptureMediaLongJobTypedRequest& Request,
			FExecutionMutationError& OutError)
		{
			if (FindCaptureMediaLongJobFamilyContract(Request.GetFamily())
				== nullptr)
			{
				return Fail(
					OutError,
					TEXT("capture_media_long_job_family_invalid"));
			}
			if (!IsNormalizedNonEmpty(Request.GetSubjectRef()))
			{
				return Fail(
					OutError,
					TEXT("capture_media_long_job_subject_ref_invalid"));
			}
			if (!IsNormalizedNonEmpty(Request.GetPolicyId()))
			{
				return Fail(
					OutError,
					TEXT("capture_media_long_job_policy_id_invalid"));
			}
			if (Request.GetParameters()->GetKind()
				!= EUeAgentValueKind::Object)
			{
				return Fail(
					OutError,
					TEXT("capture_media_long_job_parameters_not_object"));
			}
			FString ForbiddenField;
			if (TryFindForbiddenRequestField(
					Request.GetParameters(),
					ForbiddenField))
			{
				return Fail(
					OutError,
					TEXT("capture_media_long_job_request_forbidden_field"),
					MoveTemp(ForbiddenField));
			}
			if (!MatchesSubjectKind(
					Request.GetFamily(),
					Request.GetSubjectRef()))
			{
				return Fail(
					OutError,
					TEXT("capture_media_long_job_subject_kind_mismatch"),
					Request.GetSubjectRef());
			}
			if (!MatchesPolicyKind(
					Request.GetFamily(),
					Request.GetPolicyId()))
			{
				return Fail(
					OutError,
					TEXT("capture_media_long_job_policy_kind_mismatch"),
					Request.GetPolicyId());
			}
			return true;
		}
	}

	FUeAgentCaptureMediaLongJobTypedRequest::
	FUeAgentCaptureMediaLongJobTypedRequest(
		const EUeAgentCaptureMediaLongJobFamily InFamily,
		FString InSubjectRef,
		FString InPolicyId,
		FUeAgentCanonicalValueRef InParameters)
		: Family(InFamily)
		, SubjectRef(MoveTemp(InSubjectRef))
		, PolicyId(MoveTemp(InPolicyId))
		, Parameters(MoveTemp(InParameters))
	{
	}

	EUeAgentCaptureMediaLongJobFamily
	FUeAgentCaptureMediaLongJobTypedRequest::GetFamily() const
	{
		return Family;
	}

	const FString&
	FUeAgentCaptureMediaLongJobTypedRequest::GetSubjectRef() const
	{
		return SubjectRef;
	}

	const FString&
	FUeAgentCaptureMediaLongJobTypedRequest::GetPolicyId() const
	{
		return PolicyId;
	}

	const FUeAgentCanonicalValueRef&
	FUeAgentCaptureMediaLongJobTypedRequest::GetParameters() const
	{
		return Parameters;
	}

	bool FUeAgentCaptureMediaLongJobTypedRequest::IsValid() const
	{
		FExecutionMutationError Error;
		return TryValidateRequest(*this, Error);
	}

	bool TryAcceptCaptureMediaLongJob(
		const FUeAgentCaptureMediaLongJobTypedRequest& TypedRequest,
		const FLongRunningOperationRequest& OperationRequest,
		IUeAgentCaptureMediaLongJobPortFactory& PortFactory,
		FLongRunningOperationOwner& Owner,
		FLongRunningOperationSnapshot& OutSnapshot,
		FExecutionMutationError& OutError)
	{
		if (!TryValidateRequest(TypedRequest, OutError))
		{
			return false;
		}
		if (!OperationRequest.IsValid())
		{
			return Fail(
				OutError,
				TEXT("capture_media_long_job_operation_request_invalid"));
		}

		TSharedPtr<ILongRunningOperationPort, ESPMode::ThreadSafe>
			OperationPort;
		if (!PortFactory.TryCreateOperationPort(
				TypedRequest,
				OperationPort,
				OutError))
		{
			return false;
		}
		if (!OperationPort.IsValid())
		{
			return Fail(
				OutError,
				TEXT("capture_media_long_job_operation_port_missing"));
		}

		return Owner.Accept(
			OperationRequest,
			OperationPort.ToSharedRef(),
			OutSnapshot,
			OutError);
	}
}

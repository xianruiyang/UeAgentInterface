#include "DomainAdapters/CaptureMediaLongJob/UeAgentCaptureMediaLongJobAdapter.h"

#include "Dom/JsonValue.h"
#include "DomainAdapters/CaptureMediaLongJob/UeAgentCaptureMediaDiagnostics.h"

namespace UE::UAI::DomainAdapters::CaptureMediaLongJob
{
	namespace
	{
		using namespace ExecutionMutation;
		using namespace StateSemantic;

		bool Fail(FExecutionMutationError& OutError, FString Code, const FString& Reason,
			const FString& Path = FString(), const TSharedPtr<FJsonValue>& Actual = nullptr,
			const TSharedPtr<FJsonValue>& Expected = nullptr)
		{
			OutError = CaptureDiagnostics::Input(Code, Reason, TEXT("execution"), Path, Actual, Expected);
			return false;
		}

		FString ValueKindName(EUeAgentValueKind Kind)
		{
			switch (Kind)
			{
			case EUeAgentValueKind::Null: return TEXT("null");
			case EUeAgentValueKind::Boolean: return TEXT("boolean");
			case EUeAgentValueKind::String: return TEXT("string");
			case EUeAgentValueKind::Integer: return TEXT("integer");
			case EUeAgentValueKind::Decimal: return TEXT("decimal");
			case EUeAgentValueKind::Array: return TEXT("array");
			case EUeAgentValueKind::Object: return TEXT("object");
			case EUeAgentValueKind::Temporal: return TEXT("temporal");
			case EUeAgentValueKind::Spatial: return TEXT("spatial");
			}
			return TEXT("unknown");
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

		bool TryFindForbiddenRequestField(const FUeAgentCanonicalValueRef& Root,
			FString& OutField, FString& OutPath)
		{
			struct FPendingValue { FUeAgentCanonicalValueRef Value; FString Path; int32 Depth; };
			TArray<FPendingValue> Pending;
			Pending.Add({Root, TEXT("/parameters"), 0});
			for (int32 Index = 0; Index < Pending.Num(); ++Index)
			{
				const auto Current = Pending[Index];
				if (Current.Value->GetKind() == EUeAgentValueKind::Object)
				{
					for (const auto& Item : Current.Value->GetObjectItems())
					{
						const FString Path = Current.Path + TEXT("/") + Item.Key.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"));
						if (IsForbiddenRequestField(Item.Key))
						{
							OutField = Item.Key;
							OutPath = Path;
							return true;
						}
						Pending.Add({Item.Value, Path, Current.Depth + 1});
					}
				}
				else if (Current.Value->GetKind() == EUeAgentValueKind::Array)
				{
					const auto& Items = Current.Value->GetArrayItems();
					for (int32 ItemIndex = 0; ItemIndex < Items.Num(); ++ItemIndex)
						Pending.Add({Items[ItemIndex], Current.Path + TEXT("/") + FString::FromInt(ItemIndex), Current.Depth + 1});
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

		bool MatchesSubjectKind(const EUeAgentCaptureMediaLongJobFamily Family, const FString& SubjectRef, FString& OutExpected)
		{
			switch (Family)
			{
			case EUeAgentCaptureMediaLongJobFamily::ImageCapture: OutExpected = TEXT("capture-subject:"); break;
			case EUeAgentCaptureMediaLongJobFamily::MediaRecording: OutExpected = TEXT("media-source:"); break;
			case EUeAgentCaptureMediaLongJobFamily::RenderResource: OutExpected = TEXT("render-resource:"); break;
			default: return false;
			}
			return SubjectRef.StartsWith(OutExpected);
		}

		bool MatchesPolicyKind(const EUeAgentCaptureMediaLongJobFamily Family, const FString& PolicyId, FString& OutExpected)
		{
			switch (Family)
			{
			case EUeAgentCaptureMediaLongJobFamily::ImageCapture: OutExpected = TEXT("policy:image-capture:"); break;
			case EUeAgentCaptureMediaLongJobFamily::MediaRecording: OutExpected = TEXT("policy:media-recording:"); break;
			case EUeAgentCaptureMediaLongJobFamily::RenderResource: OutExpected = TEXT("policy:render-operation:"); break;
			default: return false;
			}
			return PolicyId.StartsWith(OutExpected);
		}

		bool TryValidateRequest(const FUeAgentCaptureMediaLongJobTypedRequest& Request, FExecutionMutationError& OutError)
		{
			if (!FindCaptureMediaLongJobFamilyContract(Request.GetFamily()))
			{
				OutError = CaptureDiagnostics::Native(TEXT("capture_media_long_job_family_invalid"),
					TEXT("The command profile selected an unsupported capture/media operation family."), TEXT("execution"), TEXT("family"),
					MakeShared<FJsonValueNumber>(static_cast<int32>(Request.GetFamily())), MakeShared<FJsonValueString>(TEXT("image capture, media recording, or render resource")));
				return false;
			}
			if (!IsNormalizedNonEmpty(Request.GetSubjectRef()))
			{
				Fail(OutError, TEXT("capture_media_long_job_subject_ref_invalid"), TEXT("The operation subject reference must be non-empty and have no surrounding whitespace."), TEXT("/subject_ref"),
					MakeShared<FJsonValueString>(Request.GetSubjectRef()), MakeShared<FJsonValueString>(TEXT("nonempty string without surrounding whitespace")));
				return false;
			}
			if (!IsNormalizedNonEmpty(Request.GetPolicyId()))
			{
				Fail(OutError, TEXT("capture_media_long_job_policy_id_invalid"), TEXT("The operation policy identifier must be non-empty and have no surrounding whitespace."), TEXT("/policy_id"),
					MakeShared<FJsonValueString>(Request.GetPolicyId()), MakeShared<FJsonValueString>(TEXT("nonempty string without surrounding whitespace")));
				return false;
			}
			if (Request.GetParameters()->GetKind() != EUeAgentValueKind::Object)
				return Fail(OutError, TEXT("capture_media_long_job_parameters_not_object"), TEXT("Operation parameters must be an object."), TEXT("/parameters"),
					MakeShared<FJsonValueString>(ValueKindName(Request.GetParameters()->GetKind())), MakeShared<FJsonValueString>(TEXT("object")));
			FString ForbiddenField, ForbiddenPath;
			if (TryFindForbiddenRequestField(Request.GetParameters(), ForbiddenField, ForbiddenPath))
				return Fail(OutError, TEXT("capture_media_long_job_request_forbidden_field"), TEXT("This parameter is not allowed by the operation policy."), ForbiddenPath,
					MakeShared<FJsonValueString>(ForbiddenField), MakeShared<FJsonValueString>(TEXT("field absent")));
			FString ExpectedSubject;
			if (!MatchesSubjectKind(Request.GetFamily(), Request.GetSubjectRef(), ExpectedSubject))
			{
				Fail(OutError, TEXT("capture_media_long_job_subject_kind_mismatch"), TEXT("The subject reference kind is incompatible with this operation family and policy."), TEXT("/subject_ref"),
					MakeShared<FJsonValueString>(Request.GetSubjectRef()), MakeShared<FJsonValueString>(ExpectedSubject));
				return false;
			}
			FString ExpectedPolicy;
			if (!MatchesPolicyKind(Request.GetFamily(), Request.GetPolicyId(), ExpectedPolicy))
			{
				Fail(OutError, TEXT("capture_media_long_job_policy_kind_mismatch"), TEXT("The policy kind is incompatible with this operation family."), TEXT("/policy_id"),
					MakeShared<FJsonValueString>(Request.GetPolicyId()), MakeShared<FJsonValueString>(ExpectedPolicy));
				return false;
			}
			return true;
		}
	}

	FUeAgentCaptureMediaLongJobTypedRequest::
	FUeAgentCaptureMediaLongJobTypedRequest(
		const EUeAgentCaptureMediaLongJobFamily InFamily,
		FString InSubjectRef,
		FString InPolicyId,
		FUeAgentCanonicalValueRef InParameters,
		bool bInPolicyDefaulted)
		: Family(InFamily)
		, SubjectRef(MoveTemp(InSubjectRef))
		, PolicyId(MoveTemp(InPolicyId))
		, Parameters(MoveTemp(InParameters))
		, bPolicyDefaulted(bInPolicyDefaulted)
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
		return TryValidate(Error);
	}

	bool FUeAgentCaptureMediaLongJobTypedRequest::TryValidate(UE::UAI::Contract::FDiagnostic& OutError) const
	{
		OutError.Reset();
		return TryValidateRequest(*this, OutError);
	}

	bool TryAcceptCaptureMediaLongJob(
		const FUeAgentCaptureMediaLongJobTypedRequest& TypedRequest,
		const FLongRunningOperationRequest& OperationRequest,
		IUeAgentCaptureMediaLongJobPortFactory& PortFactory,
		FLongRunningOperationOwner& Owner,
		FLongRunningOperationSnapshot& OutSnapshot,
		FExecutionMutationError& OutError)
	{
		if (!TypedRequest.TryValidate(OutError))
		{
			return false;
		}
		if (!OperationRequest.IsValid())
		{
			OutError = OperationRequest.GetValidationDiagnostic();
			return false;
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
			OutError = CaptureDiagnostics::Native(TEXT("capture_media_long_job_operation_port_missing"),
				TEXT("The capture/media operation factory returned no execution port."), TEXT("execution"), TEXT("execution_port_available"),
				MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			return false;
		}

		return Owner.Accept(
			OperationRequest,
			OperationPort.ToSharedRef(),
			OutSnapshot,
			OutError);
	}
}

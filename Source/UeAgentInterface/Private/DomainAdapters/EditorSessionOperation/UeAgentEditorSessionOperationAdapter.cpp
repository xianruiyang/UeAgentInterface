#include "DomainAdapters/EditorSessionOperation/UeAgentEditorSessionOperationAdapter.h"

#include <initializer_list>

namespace UE::UAI::DomainAdapters::EditorSessionOperation
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
				TEXT("batch"),
				TEXT("commands"),
				TEXT("phase_index"),
				TEXT("failed_phase_index"),
				TEXT("retry_failed_phase"),
				TEXT("resume"),
				TEXT("timeout_seconds"),
				TEXT("process_handle"),
				TEXT("editor_pid"),
				TEXT("connection_id"),
				TEXT("owner_lease_id"),
				TEXT("server_session_id"),
				TEXT("allow_exit"),
				TEXT("force_exit"),
				TEXT("use_current_editor"),
				TEXT("use_current_level"),
				TEXT("use_active_tool"),
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

		bool HasAnyPrefix(
			const FString& Value,
			std::initializer_list<const TCHAR*> Prefixes)
		{
			for (const TCHAR* Prefix : Prefixes)
			{
				if (Value.StartsWith(Prefix))
				{
					return true;
				}
			}
			return false;
		}

		bool MatchesSubjectKind(
			const EUeAgentEditorSessionOperationFamily Family,
			const FString& SubjectRef,
			const FString& PolicyId)
		{
			switch (Family)
			{
			case EUeAgentEditorSessionOperationFamily::AssetEditor:
				return SubjectRef.StartsWith(TEXT("asset:"));
			case EUeAgentEditorSessionOperationFamily::RuntimePreview:
				if (PolicyId
					== TEXT("policy:runtime-session:culture-preview/v1"))
				{
					return SubjectRef.StartsWith(TEXT("culture:"))
						&& SubjectRef.Len() > FCString::Strlen(TEXT("culture:"));
				}
				return HasAnyPrefix(
					SubjectRef,
					{TEXT("runtime-subject:"), TEXT("runtime-session:"),
					 TEXT("pie-session:"), TEXT("asset:"),
					 TEXT("object:"), TEXT("component:"),
					 TEXT("actor:"), TEXT("world:")});
			case EUeAgentEditorSessionOperationFamily::WorldContext:
				return SubjectRef.StartsWith(TEXT("world:"));
			case EUeAgentEditorSessionOperationFamily::ModelingTool:
				return SubjectRef.StartsWith(TEXT("modeling-session:"));
			case EUeAgentEditorSessionOperationFamily::VoxelGrid:
				return SubjectRef.StartsWith(TEXT("voxel-session:"));
			case EUeAgentEditorSessionOperationFamily::HostLifecycle:
				return SubjectRef.StartsWith(TEXT("editor-instance:"));
			default:
				return false;
			}
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

		bool TryValidateRequest(
			const FUeAgentEditorSessionOperationTypedRequest& Request,
			FExecutionMutationError& OutError)
		{
			if (FindEditorSessionOperationFamilyContract(
					Request.GetFamily()) == nullptr)
			{
				return Fail(
					OutError,
					TEXT("editor_session_operation_family_invalid"));
			}
			if (!IsNormalizedNonEmpty(Request.GetSubjectRef()))
			{
				return Fail(
					OutError,
					TEXT("editor_session_operation_subject_ref_invalid"));
			}
			if (!IsNormalizedNonEmpty(Request.GetPolicyId()))
			{
				return Fail(
					OutError,
					TEXT("editor_session_operation_policy_id_invalid"));
			}
			if (Request.GetParameters()->GetKind()
				!= EUeAgentValueKind::Object)
			{
				return Fail(
					OutError,
					TEXT("editor_session_operation_parameters_not_object"));
			}
			FString ForbiddenField;
			if (TryFindForbiddenRequestField(
					Request.GetParameters(),
					ForbiddenField))
			{
				return Fail(
					OutError,
					TEXT(
						"editor_session_operation_request_forbidden_field"),
					MoveTemp(ForbiddenField));
			}
			if (!MatchesSubjectKind(
					Request.GetFamily(),
					Request.GetSubjectRef(),
					Request.GetPolicyId()))
			{
				return Fail(
					OutError,
					TEXT("editor_session_operation_subject_kind_mismatch"),
					Request.GetSubjectRef());
			}
			if (!MatchesPolicyKind(
					Request.GetFamily(),
					Request.GetPolicyId()))
			{
				return Fail(
					OutError,
					TEXT("editor_session_operation_policy_kind_mismatch"),
					Request.GetPolicyId());
			}
			return true;
		}
	}

	FUeAgentEditorSessionOperationTypedRequest::
	FUeAgentEditorSessionOperationTypedRequest(
		const EUeAgentEditorSessionOperationFamily InFamily,
		FString InSubjectRef,
		FString InPolicyId,
		FUeAgentCanonicalValueRef InParameters)
		: Family(InFamily)
		, SubjectRef(MoveTemp(InSubjectRef))
		, PolicyId(MoveTemp(InPolicyId))
		, Parameters(MoveTemp(InParameters))
	{
	}

	EUeAgentEditorSessionOperationFamily
	FUeAgentEditorSessionOperationTypedRequest::GetFamily() const
	{
		return Family;
	}

	const FString&
	FUeAgentEditorSessionOperationTypedRequest::GetSubjectRef() const
	{
		return SubjectRef;
	}

	const FString&
	FUeAgentEditorSessionOperationTypedRequest::GetPolicyId() const
	{
		return PolicyId;
	}

	const FUeAgentCanonicalValueRef&
	FUeAgentEditorSessionOperationTypedRequest::GetParameters() const
	{
		return Parameters;
	}

	bool FUeAgentEditorSessionOperationTypedRequest::IsValid() const
	{
		FExecutionMutationError Error;
		return TryValidateRequest(*this, Error);
	}

	bool TryAcceptEditorSessionOperation(
		const FUeAgentEditorSessionOperationTypedRequest& TypedRequest,
		const FLongRunningOperationRequest& OperationRequest,
		IUeAgentEditorSessionOperationPortFactory& PortFactory,
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
				TEXT("editor_session_operation_request_invalid"));
		}

		TSharedPtr<ILongRunningOperationPort, ESPMode::ThreadSafe>
			OperationPort;
		if (!PortFactory.TryCreateOperationPort(
				TypedRequest,
				OperationRequest,
				OperationPort,
				OutError))
		{
			return false;
		}
		if (!OperationPort.IsValid())
		{
			return Fail(
				OutError,
				TEXT("editor_session_operation_port_missing"));
		}

		return Owner.Accept(
			OperationRequest,
			OperationPort.ToSharedRef(),
			OutSnapshot,
			OutError);
	}
}

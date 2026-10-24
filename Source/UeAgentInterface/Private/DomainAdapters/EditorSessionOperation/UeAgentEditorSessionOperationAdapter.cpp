#include "DomainAdapters/EditorSessionOperation/UeAgentEditorSessionOperationAdapter.h"

#include "Dom/JsonValue.h"
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
			const FString& Reason,
			const FString& Path = FString())
		{
			OutError = FExecutionMutationError::Failure(MoveTemp(Code), Reason, TEXT("execution"));
			OutError.PropertyPath = Path;
			OutError.SourceKind = TEXT("command_input");
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
			FString& OutField,
			FString& OutPath)
		{
			TArray<TPair<FUeAgentCanonicalValueRef, FString>> Pending;
			Pending.Emplace(Root, TEXT("/parameters"));
			for (int32 Index = 0; Index < Pending.Num(); ++Index)
			{
				const auto Current = Pending[Index];
				const FUeAgentCanonicalValueRef& Value = Current.Key;
				switch (Value->GetKind())
				{
				case EUeAgentValueKind::Object:
					for (const TPair<FString, FUeAgentCanonicalValueRef>& Item
						: Value->GetObjectItems())
					{
						const FString Path = Current.Value + TEXT("/") + Item.Key.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1"));
						if (IsForbiddenRequestField(Item.Key))
						{
							OutField = Item.Key;
							OutPath = Path;
							return true;
						}
						Pending.Emplace(Item.Value, Path);
					}
					break;
				case EUeAgentValueKind::Array:
					for (int32 ItemIndex = 0; ItemIndex < Value->GetArrayItems().Num(); ++ItemIndex)
						Pending.Emplace(Value->GetArrayItems()[ItemIndex], Current.Value + TEXT("/") + FString::FromInt(ItemIndex));
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
			std::initializer_list<const TCHAR*> Prefixes,
			FString& OutExpected)
		{
			OutExpected.Reset();
			bool bMatches = false;
			for (const TCHAR* Prefix : Prefixes)
			{
				if (!OutExpected.IsEmpty()) OutExpected += TEXT(" | ");
				OutExpected += Prefix;
				bMatches |= Value.StartsWith(Prefix);
			}
			return bMatches;
		}

		bool MatchesSubjectKind(
			const EUeAgentEditorSessionOperationFamily Family,
			const FString& SubjectRef,
			const FString& PolicyId,
			FString& OutExpected)
		{
			switch (Family)
			{
			case EUeAgentEditorSessionOperationFamily::AssetEditor:
				return HasAnyPrefix(SubjectRef, {TEXT("asset:")}, OutExpected);
			case EUeAgentEditorSessionOperationFamily::RuntimePreview:
				if (PolicyId
					== TEXT("policy:runtime-session:culture-preview/v1"))
				{
					OutExpected = TEXT("culture:<non-empty culture name>");
					return SubjectRef.StartsWith(TEXT("culture:"))
						&& SubjectRef.Len() > FCString::Strlen(TEXT("culture:"));
				}
				return HasAnyPrefix(
					SubjectRef,
					{TEXT("runtime-subject:"), TEXT("runtime-session:"),
					 TEXT("pie-session:"), TEXT("asset:"),
					 TEXT("object:"), TEXT("component:"),
					 TEXT("actor:"), TEXT("world:")}, OutExpected);
			case EUeAgentEditorSessionOperationFamily::WorldContext:
				return HasAnyPrefix(SubjectRef, {TEXT("world:")}, OutExpected);
			case EUeAgentEditorSessionOperationFamily::ModelingTool:
				return HasAnyPrefix(SubjectRef, {TEXT("modeling-session:")}, OutExpected);
			case EUeAgentEditorSessionOperationFamily::HostLifecycle:
				return HasAnyPrefix(SubjectRef, {TEXT("editor-instance:")}, OutExpected);
			case EUeAgentEditorSessionOperationFamily::PcgGeneration:
				return HasAnyPrefix(SubjectRef, {TEXT("pcg-component:")}, OutExpected);
			default:
				return false;
			}
		}

		bool MatchesPolicyKind(
			const EUeAgentEditorSessionOperationFamily Family,
			const FString& PolicyId,
			FString& OutExpected)
		{
			switch (Family)
			{
			case EUeAgentEditorSessionOperationFamily::AssetEditor:
				return HasAnyPrefix(PolicyId, {TEXT("policy:asset-editor:")}, OutExpected);
			case EUeAgentEditorSessionOperationFamily::RuntimePreview:
				return HasAnyPrefix(PolicyId, {TEXT("policy:runtime-session:")}, OutExpected);
			case EUeAgentEditorSessionOperationFamily::WorldContext:
				return HasAnyPrefix(PolicyId, {TEXT("policy:world-context:")}, OutExpected);
			case EUeAgentEditorSessionOperationFamily::ModelingTool:
				return HasAnyPrefix(PolicyId, {TEXT("policy:modeling-tool:")}, OutExpected);
			case EUeAgentEditorSessionOperationFamily::HostLifecycle:
				return HasAnyPrefix(PolicyId, {TEXT("policy:host-lifecycle:")}, OutExpected);
			case EUeAgentEditorSessionOperationFamily::PcgGeneration:
				return HasAnyPrefix(PolicyId, {TEXT("policy:pcg-generation:")}, OutExpected);
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
				Fail(OutError, TEXT("editor_session_operation_family_invalid"),
					TEXT("The editor session operation family is not supported."), TEXT("family"));
				OutError.ActualValue = MakeShared<FJsonValueNumber>(static_cast<int32>(Request.GetFamily()));
				OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("asset editor, runtime preview, world context, modeling tool, host lifecycle, or PCG generation"));
				return false;
			}
			if (!IsNormalizedNonEmpty(Request.GetSubjectRef()))
			{
				Fail(OutError, TEXT("editor_session_operation_subject_ref_invalid"),
					TEXT("The operation subject reference must be non-empty and have no surrounding whitespace."), TEXT("subject_ref"));
				if (Request.GetFamily() == EUeAgentEditorSessionOperationFamily::HostLifecycle)
					OutError.ActualEvidence = MakeShared<FJsonValueString>(Request.GetSubjectRef());
				else OutError.ActualValue = MakeShared<FJsonValueString>(Request.GetSubjectRef());
				OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("non-empty reference without surrounding whitespace"));
				return false;
			}
			if (!IsNormalizedNonEmpty(Request.GetPolicyId()))
			{
				Fail(OutError, TEXT("editor_session_operation_policy_id_invalid"),
					TEXT("The operation policy identifier must be non-empty and have no surrounding whitespace."), TEXT("policy_id"));
				OutError.ActualValue = MakeShared<FJsonValueString>(Request.GetPolicyId());
				OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("non-empty policy identifier without surrounding whitespace"));
				return false;
			}
			if (Request.GetParameters()->GetKind()
				!= EUeAgentValueKind::Object)
			{
				Fail(OutError, TEXT("editor_session_operation_parameters_not_object"),
					TEXT("Editor session operation parameters must be an object."), TEXT("parameters"));
				const TCHAR* Kind = TEXT("unknown");
				switch (Request.GetParameters()->GetKind())
				{
				case EUeAgentValueKind::Null: Kind = TEXT("null"); break;
				case EUeAgentValueKind::Boolean: Kind = TEXT("boolean"); break;
				case EUeAgentValueKind::String: Kind = TEXT("string"); break;
				case EUeAgentValueKind::Integer: Kind = TEXT("integer"); break;
				case EUeAgentValueKind::Decimal: Kind = TEXT("decimal"); break;
				case EUeAgentValueKind::Array: Kind = TEXT("array"); break;
				case EUeAgentValueKind::Object: Kind = TEXT("object"); break;
				case EUeAgentValueKind::Temporal: Kind = TEXT("temporal"); break;
				case EUeAgentValueKind::Spatial: Kind = TEXT("spatial"); break;
				}
				OutError.ActualValue = MakeShared<FJsonValueString>(Kind);
				OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("object"));
				return false;
			}
			FString ForbiddenField;
			FString ForbiddenPath;
			if (TryFindForbiddenRequestField(
					Request.GetParameters(),
					ForbiddenField, ForbiddenPath))
			{
				Fail(OutError, TEXT("editor_session_operation_request_forbidden_field"),
					TEXT("The editor session operation parameters contain a field reserved for the runtime owner."), ForbiddenPath);
				OutError.ActualValue = MakeShared<FJsonValueString>(ForbiddenField);
				return false;
			}
			FString ExpectedSubject;
			if (!MatchesSubjectKind(
					Request.GetFamily(),
					Request.GetSubjectRef(),
					Request.GetPolicyId(), ExpectedSubject))
			{
				Fail(OutError, TEXT("editor_session_operation_subject_kind_mismatch"),
					TEXT("The subject reference kind is incompatible with the editor session operation family and policy."), TEXT("subject_ref"));
				if (Request.GetFamily() == EUeAgentEditorSessionOperationFamily::HostLifecycle)
					OutError.ActualEvidence = MakeShared<FJsonValueString>(Request.GetSubjectRef());
				else OutError.ActualValue = MakeShared<FJsonValueString>(Request.GetSubjectRef());
				OutError.ExpectedValue = MakeShared<FJsonValueString>(ExpectedSubject);
				return false;
			}
			FString ExpectedPolicy;
			if (!MatchesPolicyKind(
					Request.GetFamily(),
					Request.GetPolicyId(), ExpectedPolicy))
			{
				Fail(OutError, TEXT("editor_session_operation_policy_kind_mismatch"),
					TEXT("The policy kind is incompatible with the editor session operation family."), TEXT("policy_id"));
				OutError.ActualValue = MakeShared<FJsonValueString>(Request.GetPolicyId());
				OutError.ExpectedValue = MakeShared<FJsonValueString>(ExpectedPolicy);
				return false;
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
		TryValidateRequest(*this, ValidationDiagnostic);
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
		return ValidationDiagnostic.IsEmpty();
	}

	const FExecutionMutationError& FUeAgentEditorSessionOperationTypedRequest::GetValidationDiagnostic() const
	{
		return ValidationDiagnostic;
	}

	bool TryAcceptEditorSessionOperation(
		const FUeAgentEditorSessionOperationTypedRequest& TypedRequest,
		const FLongRunningOperationRequest& OperationRequest,
		IUeAgentEditorSessionOperationPortFactory& PortFactory,
		FLongRunningOperationOwner& Owner,
		FLongRunningOperationSnapshot& OutSnapshot,
		FExecutionMutationError& OutError)
	{
		if (!TypedRequest.IsValid())
		{
			OutError = TypedRequest.GetValidationDiagnostic();
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
				OperationRequest,
				OperationPort,
				OutError))
		{
			return false;
		}
		if (!OperationPort.IsValid())
		{
			Fail(OutError, TEXT("editor_session_operation_port_missing"),
				TEXT("The editor session operation factory returned no execution port."));
			OutError.SourceKind = TEXT("native_result");
			return false;
		}

		return Owner.Accept(
			OperationRequest,
			OperationPort.ToSharedRef(),
			OutSnapshot,
			OutError);
	}
}

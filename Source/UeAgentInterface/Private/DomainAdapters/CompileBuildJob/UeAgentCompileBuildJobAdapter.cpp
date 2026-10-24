#include "DomainAdapters/CompileBuildJob/UeAgentCompileBuildJobAdapter.h"

#include "Dom/JsonValue.h"
#include "Core/StateSemantic/UeAgentCanonicalValueJson.h"

namespace UE::UAI::DomainAdapters::CompileBuildJob
{
	namespace
	{
		using namespace ExecutionMutation;
		using namespace StateSemantic;

		bool Fail(FExecutionMutationError& OutError, FString Code, const FString& Reason,
			const FString& Path = FString(), const TSharedPtr<FJsonValue>& Actual = nullptr,
			const TSharedPtr<FJsonValue>& Expected = nullptr)
		{
			OutError = FExecutionMutationError::Failure(MoveTemp(Code), Reason, TEXT("execution"));
			const FString Pointer = Path.IsEmpty() || Path.StartsWith(TEXT("/")) ? Path : TEXT("/") + Path;
			OutError.PropertyPath = Pointer;
			OutError.SemanticPointer = Pointer;
			OutError.Source.JsonPath = Pointer;
			OutError.SourceKind = TEXT("command_input");
			OutError.ActualValue = Actual;
			OutError.ExpectedValue = Expected;
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
				TEXT("executable"),
				TEXT("arguments"),
				TEXT("command_line"),
				TEXT("timeout_seconds"),
				TEXT("http_route"),
				TEXT("task_directory"),
				TEXT("process_handle"),
			};
			return ForbiddenFields.Contains(NormalizeFieldName(Field));
		}

		bool IsLocalizationOwnedToolPolicy(const FString& PolicyId)
		{
			return PolicyId
					== TEXT("policy:owned-tool:localization-gather-text/v1")
				|| PolicyId
					== TEXT("policy:owned-tool:localization-compile-text/v1")
				|| PolicyId
					== TEXT("policy:owned-tool:localization-export-po/v1");
		}

		bool TryFindForbiddenRequestField(const FUeAgentCanonicalValueRef& Root,
			const bool bAllowTopLevelTimeoutSeconds,
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
						const bool bAllowedTimeout = bAllowTopLevelTimeoutSeconds && Current.Depth == 0
							&& NormalizeFieldName(Item.Key) == TEXT("timeout_seconds");
						if (IsForbiddenRequestField(Item.Key) && !bAllowedTimeout)
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

		bool MatchesSubjectKind(const EUeAgentCompileBuildJobFamily Family, const FString& SubjectRef,
			const FString& PolicyId, FString& OutExpected)
		{
			switch (Family)
			{
			case EUeAgentCompileBuildJobFamily::AssetCompile:
				OutExpected = TEXT("asset:"); return SubjectRef.StartsWith(OutExpected);
			case EUeAgentCompileBuildJobFamily::WorldBuild:
			case EUeAgentCompileBuildJobFamily::WorldPartitionResave:
				OutExpected = TEXT("world:"); return SubjectRef.StartsWith(OutExpected);
			case EUeAgentCompileBuildJobFamily::ExternalProcess:
				OutExpected = IsLocalizationOwnedToolPolicy(PolicyId) ? TEXT("project: | asset: | localization:") : TEXT("project: | asset:");
				return SubjectRef.StartsWith(TEXT("project:")) || SubjectRef.StartsWith(TEXT("asset:"))
					|| (IsLocalizationOwnedToolPolicy(PolicyId) && SubjectRef.StartsWith(TEXT("localization:")));
			case EUeAgentCompileBuildJobFamily::Packaging:
				OutExpected = TEXT("project:current"); return SubjectRef == OutExpected;
			default: return false;
			}
		}

		bool MatchesPolicyKind(const EUeAgentCompileBuildJobFamily Family, const FString& PolicyId, FString& OutExpected)
		{
			switch (Family)
			{
			case EUeAgentCompileBuildJobFamily::AssetCompile: OutExpected = TEXT("policy:asset-compile:"); break;
			case EUeAgentCompileBuildJobFamily::WorldBuild: OutExpected = TEXT("policy:world-build:"); break;
			case EUeAgentCompileBuildJobFamily::ExternalProcess: OutExpected = TEXT("policy:owned-tool:"); break;
			case EUeAgentCompileBuildJobFamily::WorldPartitionResave: OutExpected = TEXT("policy:world-partition-resave:"); break;
			case EUeAgentCompileBuildJobFamily::Packaging: OutExpected = TEXT("policy:packaging:"); break;
			default: return false;
			}
			return PolicyId.StartsWith(OutExpected);
		}

		bool TryValidateRequest(const FUeAgentCompileBuildJobTypedRequest& Request, FExecutionMutationError& OutError)
		{
			if (!FindCompileBuildJobFamilyContract(Request.GetFamily()))
				return Fail(OutError, TEXT("compile_build_job_family_invalid"), TEXT("The compile/build operation family is not supported."), TEXT("family"),
					MakeShared<FJsonValueNumber>(static_cast<int32>(Request.GetFamily())), MakeShared<FJsonValueString>(TEXT("asset compile, world build, external process, world partition resave, or packaging")));
			if (!IsNormalizedNonEmpty(Request.GetSubjectRef()))
			{
				Fail(OutError, TEXT("compile_build_job_subject_ref_invalid"), TEXT("The operation subject reference must be non-empty and have no surrounding whitespace."), TEXT("subject_ref"));
				OutError.ActualValue = MakeShared<FJsonValueString>(Request.GetSubjectRef());
				OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("non-empty subject reference without surrounding whitespace"));
				return false;
			}
			if (!IsNormalizedNonEmpty(Request.GetPolicyId()))
			{
				Fail(OutError, TEXT("compile_build_job_policy_id_invalid"), TEXT("The operation policy identifier must be non-empty and have no surrounding whitespace."), TEXT("policy_id"));
				OutError.ActualValue = MakeShared<FJsonValueString>(Request.GetPolicyId());
				OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("non-empty policy reference without surrounding whitespace"));
				return false;
			}
			if (Request.GetParameters()->GetKind() != EUeAgentValueKind::Object)
				return Fail(OutError, TEXT("compile_build_job_parameters_not_object"), TEXT("Operation parameters must be an object."), TEXT("parameters"),
					MakeShared<FJsonValueString>(ValueKindName(Request.GetParameters()->GetKind())), MakeShared<FJsonValueString>(TEXT("object")));
			FString ForbiddenField, ForbiddenPath;
			const bool bAllowTopLevelTimeoutSeconds = Request.GetFamily() == EUeAgentCompileBuildJobFamily::ExternalProcess
				&& IsLocalizationOwnedToolPolicy(Request.GetPolicyId());
			if (TryFindForbiddenRequestField(Request.GetParameters(), bAllowTopLevelTimeoutSeconds, ForbiddenField, ForbiddenPath))
				return Fail(OutError, TEXT("compile_build_job_request_forbidden_field"), TEXT("This parameter is not allowed by the operation policy."), ForbiddenPath,
					MakeShared<FJsonValueString>(ForbiddenField));
			if (bAllowTopLevelTimeoutSeconds)
			{
				const auto* TimeoutField = Request.GetParameters()->GetObjectItems().FindByPredicate(
					[](const auto& Item) { return NormalizeFieldName(Item.Key) == TEXT("timeout_seconds"); });
				if (TimeoutField)
				{
					const EUeAgentValueKind Kind = TimeoutField->Value->GetKind();
					double TimeoutSeconds = 0.0;
					if ((Kind != EUeAgentValueKind::Integer && Kind != EUeAgentValueKind::Decimal)
						|| !LexTryParseString(TimeoutSeconds, *TimeoutField->Value->GetLexeme())
						|| !FMath::IsFinite(TimeoutSeconds) || TimeoutSeconds < 0.0)
					{
						Fail(OutError, TEXT("compile_build_job_timeout_seconds_invalid"),
							TEXT("The localization tool timeout must be a finite, non-negative number of seconds."),
							TEXT("/parameters/") + TimeoutField->Key.Replace(TEXT("~"), TEXT("~0")).Replace(TEXT("/"), TEXT("~1")),
							nullptr, MakeShared<FJsonValueString>(TEXT("finite number >= 0 seconds")));
						FExecutionMutationError ValueError;
						if (!CanonicalJson::TryToJsonValue(*TimeoutField->Value, OutError.ActualValue, ValueError))
							OutError.PublicCause = MakeShared<FExecutionMutationError>(MoveTemp(ValueError));
						return false;
					}
				}
			}
			FString ExpectedSubject;
			if (!MatchesSubjectKind(Request.GetFamily(), Request.GetSubjectRef(), Request.GetPolicyId(), ExpectedSubject))
			{
				Fail(OutError, TEXT("compile_build_job_subject_kind_mismatch"), TEXT("The subject reference kind is incompatible with this operation family and policy."), TEXT("subject_ref"),
					nullptr, MakeShared<FJsonValueString>(ExpectedSubject));
				OutError.ActualValue = MakeShared<FJsonValueString>(Request.GetSubjectRef());
				return false;
			}
			FString ExpectedPolicy;
			if (!MatchesPolicyKind(Request.GetFamily(), Request.GetPolicyId(), ExpectedPolicy))
			{
				Fail(OutError, TEXT("compile_build_job_policy_kind_mismatch"), TEXT("The policy kind is incompatible with this operation family."), TEXT("policy_id"),
					nullptr, MakeShared<FJsonValueString>(ExpectedPolicy));
				OutError.ActualValue = MakeShared<FJsonValueString>(Request.GetPolicyId());
				return false;
			}
			return true;
		}
	}

	FUeAgentCompileBuildJobTypedRequest::
	FUeAgentCompileBuildJobTypedRequest(
		const EUeAgentCompileBuildJobFamily InFamily,
		FString InSubjectRef,
		FString InPolicyId,
		FUeAgentCanonicalValueRef InParameters)
		: Family(InFamily)
		, SubjectRef(MoveTemp(InSubjectRef))
		, PolicyId(MoveTemp(InPolicyId))
		, Parameters(MoveTemp(InParameters))
	{
		TryValidateRequest(*this, ValidationDiagnostic);
		if (ValidationDiagnostic.IsValid())
		{
			if (ValidationDiagnostic.TargetPath.IsEmpty()) ValidationDiagnostic.TargetPath = SubjectRef;
			if (ValidationDiagnostic.CandidateName.IsEmpty()) ValidationDiagnostic.CandidateName = PolicyId;
		}
	}

	EUeAgentCompileBuildJobFamily
	FUeAgentCompileBuildJobTypedRequest::GetFamily() const
	{
		return Family;
	}

	const FString&
	FUeAgentCompileBuildJobTypedRequest::GetSubjectRef() const
	{
		return SubjectRef;
	}

	const FString&
	FUeAgentCompileBuildJobTypedRequest::GetPolicyId() const
	{
		return PolicyId;
	}

	const FUeAgentCanonicalValueRef&
	FUeAgentCompileBuildJobTypedRequest::GetParameters() const
	{
		return Parameters;
	}

	bool FUeAgentCompileBuildJobTypedRequest::IsValid() const
	{
		return ValidationDiagnostic.IsEmpty();
	}

	const FExecutionMutationError& FUeAgentCompileBuildJobTypedRequest::GetValidationDiagnostic() const
	{
		return ValidationDiagnostic;
	}

	bool TryAcceptCompileBuildJob(
		const FUeAgentCompileBuildJobTypedRequest& TypedRequest,
		const FLongRunningOperationRequest& OperationRequest,
		IUeAgentCompileBuildJobPortFactory& PortFactory,
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
			if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = TypedRequest.GetSubjectRef();
			if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = TypedRequest.GetPolicyId();
			return false;
		}

		TSharedPtr<ILongRunningOperationPort, ESPMode::ThreadSafe>
			OperationPort;
		if (!PortFactory.TryCreateOperationPort(
				TypedRequest,
				OperationPort,
				OutError))
		{
			if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = TypedRequest.GetSubjectRef();
			if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = TypedRequest.GetPolicyId();
			return false;
		}
		if (!OperationPort.IsValid())
		{
			Fail(OutError, TEXT("compile_build_job_operation_port_missing"), TEXT("The compile/build operation factory returned no execution port."),
				TEXT("operation_port.available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			OutError.SourceKind = TEXT("native_result");
			OutError.Source.JsonPath.Reset();
			OutError.TargetPath = TypedRequest.GetSubjectRef();
			OutError.CandidateName = TypedRequest.GetPolicyId();
			return false;
		}

		const bool bAccepted = Owner.Accept(
			OperationRequest,
			OperationPort.ToSharedRef(),
			OutSnapshot,
			OutError);
		if (!bAccepted)
		{
			if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = TypedRequest.GetSubjectRef();
			if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = TypedRequest.GetPolicyId();
		}
		return bAccepted;
	}
}

#include "DomainAdapters/CompileBuildJob/UeAgentCompileBuildJobAdapter.h"

namespace UE::UAI::DomainAdapters::CompileBuildJob
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

		bool UsesLocalizationTimeout(const FString& PolicyId)
		{
			return PolicyId
					== TEXT("policy:owned-tool:localization-gather-text/v1")
				|| PolicyId
					== TEXT("policy:owned-tool:localization-compile-text/v1");
		}

		bool TryFindForbiddenRequestField(
			const FUeAgentCanonicalValueRef& Root,
			const bool bAllowTopLevelTimeoutSeconds,
			FString& OutField)
		{
			struct FPendingValue
			{
				FUeAgentCanonicalValueRef Value;
				int32 Depth = 0;
			};
			TArray<FPendingValue> Pending;
			Pending.Add({Root, 0});
			for (int32 Index = 0; Index < Pending.Num(); ++Index)
			{
				const FPendingValue& PendingValue = Pending[Index];
				const FUeAgentCanonicalValueRef& Value = PendingValue.Value;
				switch (Value->GetKind())
				{
				case EUeAgentValueKind::Object:
					for (const TPair<FString, FUeAgentCanonicalValueRef>& Item
						: Value->GetObjectItems())
					{
						const bool bAllowedTopLevelTimeout =
							bAllowTopLevelTimeoutSeconds
							&& PendingValue.Depth == 0
							&& NormalizeFieldName(Item.Key)
								== TEXT("timeout_seconds");
						if (IsForbiddenRequestField(Item.Key)
							&& !bAllowedTopLevelTimeout)
						{
							OutField = Item.Key;
							return true;
						}
						Pending.Add({Item.Value, PendingValue.Depth + 1});
					}
					break;
				case EUeAgentValueKind::Array:
					for (const FUeAgentCanonicalValueRef& Item
						: Value->GetArrayItems())
					{
						Pending.Add({Item, PendingValue.Depth + 1});
					}
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
			const EUeAgentCompileBuildJobFamily Family,
			const FString& SubjectRef,
			const FString& PolicyId)
		{
			switch (Family)
			{
			case EUeAgentCompileBuildJobFamily::AssetCompile:
				return SubjectRef.StartsWith(TEXT("asset:"));
			case EUeAgentCompileBuildJobFamily::WorldBuild:
			case EUeAgentCompileBuildJobFamily::WorldPartitionResave:
				return SubjectRef.StartsWith(TEXT("world:"));
			case EUeAgentCompileBuildJobFamily::ExternalProcess:
				return SubjectRef.StartsWith(TEXT("project:"))
					|| SubjectRef.StartsWith(TEXT("asset:"))
					|| (IsLocalizationOwnedToolPolicy(PolicyId)
						&& SubjectRef.StartsWith(TEXT("localization:")));
			case EUeAgentCompileBuildJobFamily::Packaging:
				return SubjectRef == TEXT("project:current");
			default:
				return false;
			}
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

		bool TryValidateRequest(
			const FUeAgentCompileBuildJobTypedRequest& Request,
			FExecutionMutationError& OutError)
		{
			if (FindCompileBuildJobFamilyContract(Request.GetFamily())
				== nullptr)
			{
				return Fail(
					OutError,
					TEXT("compile_build_job_family_invalid"));
			}
			if (!IsNormalizedNonEmpty(Request.GetSubjectRef()))
			{
				return Fail(
					OutError,
					TEXT("compile_build_job_subject_ref_invalid"));
			}
			if (!IsNormalizedNonEmpty(Request.GetPolicyId()))
			{
				return Fail(
					OutError,
					TEXT("compile_build_job_policy_id_invalid"));
			}
			if (Request.GetParameters()->GetKind()
				!= EUeAgentValueKind::Object)
			{
				return Fail(
					OutError,
					TEXT("compile_build_job_parameters_not_object"));
			}
			FString ForbiddenField;
			const bool bAllowTopLevelTimeoutSeconds =
				Request.GetFamily()
					== EUeAgentCompileBuildJobFamily::ExternalProcess
				&& UsesLocalizationTimeout(Request.GetPolicyId());
			if (TryFindForbiddenRequestField(
					Request.GetParameters(),
					bAllowTopLevelTimeoutSeconds,
					ForbiddenField))
			{
				return Fail(
					OutError,
					TEXT("compile_build_job_request_forbidden_field"),
					MoveTemp(ForbiddenField));
			}
			if (bAllowTopLevelTimeoutSeconds)
			{
				const TPair<FString, FUeAgentCanonicalValueRef>* TimeoutField =
					Request.GetParameters()->GetObjectItems().FindByPredicate(
						[](const TPair<FString, FUeAgentCanonicalValueRef>& Item)
						{
							return NormalizeFieldName(Item.Key)
								== TEXT("timeout_seconds");
						});
				if (TimeoutField)
				{
					const EUeAgentValueKind Kind = TimeoutField->Value->GetKind();
					double TimeoutSeconds = 0.0;
					if ((Kind != EUeAgentValueKind::Integer
							&& Kind != EUeAgentValueKind::Decimal)
						|| !LexTryParseString(
							TimeoutSeconds,
							*TimeoutField->Value->GetLexeme())
						|| !FMath::IsFinite(TimeoutSeconds)
						|| TimeoutSeconds <= 0.0
						|| TimeoutSeconds > 300.0)
					{
						return Fail(
							OutError,
							TEXT("compile_build_job_timeout_seconds_invalid"));
					}
				}
			}
			if (!MatchesSubjectKind(
					Request.GetFamily(),
					Request.GetSubjectRef(),
					Request.GetPolicyId()))
			{
				return Fail(
					OutError,
					TEXT("compile_build_job_subject_kind_mismatch"),
					Request.GetSubjectRef());
			}
			if (!MatchesPolicyKind(
					Request.GetFamily(),
					Request.GetPolicyId()))
			{
				return Fail(
					OutError,
					TEXT("compile_build_job_policy_kind_mismatch"),
					Request.GetPolicyId());
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
		FExecutionMutationError Error;
		return TryValidateRequest(*this, Error);
	}

	bool TryAcceptCompileBuildJob(
		const FUeAgentCompileBuildJobTypedRequest& TypedRequest,
		const FLongRunningOperationRequest& OperationRequest,
		IUeAgentCompileBuildJobPortFactory& PortFactory,
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
				TEXT("compile_build_job_operation_request_invalid"));
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
				TEXT("compile_build_job_operation_port_missing"));
		}

		return Owner.Accept(
			OperationRequest,
			OperationPort.ToSharedRef(),
			OutSnapshot,
			OutError);
	}
}

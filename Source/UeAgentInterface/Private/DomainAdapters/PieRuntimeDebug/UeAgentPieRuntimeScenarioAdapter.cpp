#include "DomainAdapters/PieRuntimeDebug/UeAgentPieRuntimeScenarioAdapter.h"

namespace UE::UAI::DomainAdapters::PieRuntimeDebug
{
	using namespace ExecutionMutation;
	using namespace StateSemantic;

	namespace
	{
		bool Fail(
			FExecutionMutationError& OutError,
			FString Code,
			FString Detail = FString())
		{
			OutError = FExecutionMutationError(MoveTemp(Code), MoveTemp(Detail));
			return false;
		}

		bool IsNormalizedNonEmpty(const FString& Value)
		{
			FString Normalized = Value;
			Normalized.TrimStartAndEndInline();
			return !Normalized.IsEmpty() && Normalized == Value;
		}

		bool IsForbiddenField(FString Field)
		{
			Field.TrimStartAndEndInline();
			Field.ToLowerInline();
			static const TSet<FString> Forbidden = {
				TEXT("command_id"),
				TEXT("http_route"),
				TEXT("owner_lease_id"),
				TEXT("server_session_id"),
				TEXT("operation_registry"),
				TEXT("os_input"),
				TEXT("slate_input"),
				TEXT("foreground_window"),
				TEXT("focus_window"),
				TEXT("real_cursor"),
				TEXT("fullscreen"),
				TEXT("hardware_device"),
			};
			return Forbidden.Contains(Field);
		}

		bool TryFindForbiddenField(
			const FUeAgentCanonicalValueRef& Root,
			FString& OutField)
		{
			TArray<FUeAgentCanonicalValueRef> Pending{Root};
			for (int32 Index = 0; Index < Pending.Num(); ++Index)
			{
				const FUeAgentCanonicalValueRef& Value = Pending[Index];
				if (Value->GetKind() == EUeAgentValueKind::Object)
				{
					for (const TPair<FString, FUeAgentCanonicalValueRef>& Item
						: Value->GetObjectItems())
					{
						if (IsForbiddenField(Item.Key))
						{
							OutField = Item.Key;
							return true;
						}
						Pending.Add(Item.Value);
					}
				}
				else if (Value->GetKind() == EUeAgentValueKind::Array)
				{
					Pending.Append(Value->GetArrayItems());
				}
			}
			return false;
		}

		bool TryValidateRequest(
			const FUeAgentPieRuntimeScenarioTypedRequest& Request,
			FExecutionMutationError& OutError)
		{
			if (!IsNormalizedNonEmpty(Request.GetSessionRef())
				|| !Request.GetSessionRef().StartsWith(
					TEXT("runtime-session:pie-debug:")))
			{
				return Fail(
					OutError, TEXT("pie_runtime_scenario_session_ref_invalid"));
			}
			if (Request.GetPolicyId()
				!= TEXT("policy:runtime-scenario:bounded/v1"))
			{
				return Fail(
					OutError, TEXT("pie_runtime_scenario_policy_invalid"));
			}
			if (Request.GetParameters()->GetKind()
				!= EUeAgentValueKind::Object)
			{
				return Fail(
					OutError, TEXT("pie_runtime_scenario_parameters_not_object"));
			}
			FString ForbiddenField;
			if (TryFindForbiddenField(Request.GetParameters(), ForbiddenField))
			{
				return Fail(
					OutError,
					TEXT("pie_runtime_scenario_forbidden_field"),
					MoveTemp(ForbiddenField));
			}
			return true;
		}
	}

	FUeAgentPieRuntimeScenarioTypedRequest::
	FUeAgentPieRuntimeScenarioTypedRequest(
		FString InSessionRef,
		FString InPolicyId,
		FUeAgentCanonicalValueRef InParameters)
		: SessionRef(MoveTemp(InSessionRef))
		, PolicyId(MoveTemp(InPolicyId))
		, Parameters(MoveTemp(InParameters))
	{
	}

	const FString& FUeAgentPieRuntimeScenarioTypedRequest::GetSessionRef() const
	{
		return SessionRef;
	}

	const FString& FUeAgentPieRuntimeScenarioTypedRequest::GetPolicyId() const
	{
		return PolicyId;
	}

	const FUeAgentCanonicalValueRef&
	FUeAgentPieRuntimeScenarioTypedRequest::GetParameters() const
	{
		return Parameters;
	}

	bool FUeAgentPieRuntimeScenarioTypedRequest::IsValid() const
	{
		FExecutionMutationError Error;
		return TryValidateRequest(*this, Error);
	}

	bool TryAcceptPieRuntimeScenario(
		const FUeAgentPieRuntimeScenarioTypedRequest& TypedRequest,
		const FLongRunningOperationRequest& OperationRequest,
		IUeAgentPieRuntimeScenarioPortFactory& PortFactory,
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
				OutError, TEXT("pie_runtime_scenario_operation_request_invalid"));
		}
		TSharedPtr<ILongRunningOperationPort, ESPMode::ThreadSafe> Port;
		if (!PortFactory.TryCreateOperationPort(
				TypedRequest, OperationRequest, Port, OutError))
		{
			return false;
		}
		if (!Port.IsValid())
		{
			return Fail(OutError, TEXT("pie_runtime_scenario_port_missing"));
		}
		return Owner.Accept(
			OperationRequest, Port.ToSharedRef(), OutSnapshot, OutError);
	}
}

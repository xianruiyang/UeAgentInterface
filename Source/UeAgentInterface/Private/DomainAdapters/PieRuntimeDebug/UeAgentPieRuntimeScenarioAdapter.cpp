#include "DomainAdapters/PieRuntimeDebug/UeAgentPieRuntimeScenarioAdapter.h"

#include "Dom/JsonValue.h"
#include "DomainAdapters/PieRuntimeDebug/UeAgentPieRuntimeDiagnostics.h"

namespace UE::UAI::DomainAdapters::PieRuntimeDebug
{
	using namespace ExecutionMutation;
	using namespace StateSemantic;

	namespace
	{
		bool Fail(FExecutionMutationError& OutError, FString Code, const FString& Reason,
			const FString& Path, const TSharedPtr<FJsonValue>& Actual = nullptr,
			const TSharedPtr<FJsonValue>& Expected = nullptr)
		{
			OutError = PieDiagnostics::Input(Code, Reason, TEXT("execution"), Path, Actual, Expected);
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

		bool TryFindForbiddenField(const FUeAgentCanonicalValueRef& Root,
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
						if (IsForbiddenField(Item.Key))
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

		bool TryValidateRequest(const FUeAgentPieRuntimeScenarioTypedRequest& Request, FExecutionMutationError& OutError)
		{
			if (!IsNormalizedNonEmpty(Request.GetSessionRef()) || !Request.GetSessionRef().StartsWith(TEXT("runtime-session:pie-debug:")))
			{
				Fail(OutError, TEXT("pie_runtime_scenario_session_ref_invalid"), TEXT("The PIE scenario requires a normalized PIE debug session reference."), TEXT("/subject_ref"),
					PieDiagnostics::Facts({ { TEXT("normalized_nonempty"), MakeShared<FJsonValueBoolean>(IsNormalizedNonEmpty(Request.GetSessionRef())) },
						{ TEXT("pie_debug_session_prefix"), MakeShared<FJsonValueBoolean>(Request.GetSessionRef().StartsWith(TEXT("runtime-session:pie-debug:"))) } }),
					PieDiagnostics::Facts({ { TEXT("normalized_nonempty"), MakeShared<FJsonValueBoolean>(true) },
						{ TEXT("pie_debug_session_prefix"), MakeShared<FJsonValueBoolean>(true) } }));
				OutError.ActualEvidence = MakeShared<FJsonValueString>(Request.GetSessionRef());
				return false;
			}
			if (Request.GetPolicyId() != TEXT("policy:runtime-scenario:bounded/v2"))
			{
				Fail(OutError, TEXT("pie_runtime_scenario_policy_invalid"), TEXT("The PIE scenario requires the bounded runtime scenario policy."), TEXT("/policy_id"),
					MakeShared<FJsonValueString>(Request.GetPolicyId()), MakeShared<FJsonValueString>(TEXT("policy:runtime-scenario:bounded/v2")));
				return false;
			}
			if (Request.GetParameters()->GetKind() != EUeAgentValueKind::Object)
				return Fail(OutError, TEXT("pie_runtime_scenario_parameters_not_object"), TEXT("PIE scenario parameters must be an object."), TEXT("/parameters"),
					PieDiagnostics::CanonicalValueFact(Request.GetParameters().Get()), MakeShared<FJsonValueString>(TEXT("object")));
			FString ForbiddenField, ForbiddenPath;
			if (TryFindForbiddenField(Request.GetParameters(), ForbiddenField, ForbiddenPath))
				return Fail(OutError, TEXT("pie_runtime_scenario_forbidden_field"), TEXT("This parameter is not allowed by the PIE scenario policy."), ForbiddenPath,
					MakeShared<FJsonValueString>(ForbiddenField), MakeShared<FJsonValueString>(TEXT("field omitted")));
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
			OutError = OperationRequest.GetValidationDiagnostic();
			return false;
		}
		TSharedPtr<ILongRunningOperationPort, ESPMode::ThreadSafe> Port;
		if (!PortFactory.TryCreateOperationPort(
				TypedRequest, OperationRequest, Port, OutError))
		{
			return false;
		}
		if (!Port.IsValid())
		{
			OutError = PieDiagnostics::Native(TEXT("pie_runtime_scenario_port_missing"),
				TEXT("The PIE scenario factory returned no execution port."), TEXT("execution"),
				TEXT("execution_port_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			return false;
		}
		return Owner.Accept(
			OperationRequest, Port.ToSharedRef(), OutSnapshot, OutError);
	}
}

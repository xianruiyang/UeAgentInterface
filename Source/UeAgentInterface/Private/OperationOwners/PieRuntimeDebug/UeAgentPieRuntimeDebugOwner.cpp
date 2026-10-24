#include "OperationOwners/PieRuntimeDebug/UeAgentPieRuntimeDebugOwner.h"

#include "Dom/JsonValue.h"

#include "Core/Contract/UeAgentDiagnostic.h"

#include "OperationOwners/LongRunning/UeAgentLongRunningLifecycleOwner.h"

namespace UE::UAI::OperationOwners::PieRuntimeDebug
{
	using namespace Composition::LongRunningSession;

	bool FUeAgentPieRuntimeDebugRoute::IsValid() const
	{
		UE::UAI::Contract::FDiagnostic Error;
		return TryValidate(Error);
	}

	bool FUeAgentPieRuntimeDebugRoute::TryValidate(UE::UAI::Contract::FDiagnostic& OutError) const
	{
		OutError.Reset();
		if (CommandId != TEXT("runtime.scenario.execute"))
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("pie_runtime_debug_route_invalid"),
				TEXT("The route field does not match the required command binding."), TEXT("route_validation"), FString(), CommandId);
			if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = CommandId;
			OutError.PropertyPath = TEXT("command_id");
			OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("runtime.scenario.execute"));
			OutError.ActualValue = MakeShared<FJsonValueString>(CommandId);
			return false;
		}
		if (PythonNamespace != TEXT("runtime"))
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("pie_runtime_debug_route_invalid"),
				TEXT("The route field does not match the required command binding."), TEXT("route_validation"), FString(), CommandId);
			if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = CommandId;
			OutError.PropertyPath = TEXT("python_namespace");
			OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("runtime"));
			OutError.ActualValue = MakeShared<FJsonValueString>(PythonNamespace);
			return false;
		}
		if (PythonFunction != TEXT("scenario_execute"))
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("pie_runtime_debug_route_invalid"),
				TEXT("The route field does not match the required command binding."), TEXT("route_validation"), FString(), CommandId);
			if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = CommandId;
			OutError.PropertyPath = TEXT("python_function");
			OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("scenario_execute"));
			OutError.ActualValue = MakeShared<FJsonValueString>(PythonFunction);
			return false;
		}
		if (!DescriptorIdentity.IsValid())
		{
			OutError = DescriptorIdentity.GetValidationDiagnostic();
			OutError.SubjectId = CommandId;
			if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = CommandId;
			return false;
		}
		return true;
	}

	bool TryGetRoute(const FUeAgentPieRuntimeDebugRoute*& OutRoute, UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutRoute = nullptr;
		OutError.Reset();
		const TArray<FUeAgentLongRunningSessionActivationProfile>* Profiles = nullptr;
		if (!TryGetLongRunningSessionActivationProfiles(Profiles, OutError)) return false;
		static const TSharedPtr<const FUeAgentPieRuntimeDebugRoute> Route = [Profiles]() -> TSharedPtr<const FUeAgentPieRuntimeDebugRoute>
		{
			for (const FUeAgentLongRunningSessionActivationProfile& Profile : *Profiles)
			{
				if (Profile.CommandId == TEXT("runtime.scenario.execute"))
				{
					return MakeShared<FUeAgentPieRuntimeDebugRoute>(FUeAgentPieRuntimeDebugRoute{
						Profile.CommandId, Profile.Namespace, Profile.Method, Profile.DescriptorIdentity});
				}
			}
			return nullptr;
		}();
		if (!Route.IsValid())
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("pie_runtime_debug_route_missing"),
				TEXT("No activation profile is registered for runtime.scenario.execute."), TEXT("route_construction"), FString(), TEXT("runtime.scenario.execute"));
			if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = TEXT("runtime.scenario.execute");
			OutError.SourceKind = TEXT("native_result");
			OutError.PropertyPath = TEXT("activation_profile_registered");
			OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
			OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
			return false;
		}
		if (!Route->TryValidate(OutError)) return false;
		OutRoute = Route.Get();
		return true;
	}

	bool TryGetProductionOperationPort(
		TSharedPtr<Contract::IUeAgentOperationApplicationPort>& OutPort,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		TSharedPtr<FUeAgentLongRunningSessionRuntime> Runtime;
		return OperationOwners::LongRunning::
			TryGetProductionLongRunningLifecyclePort(
				OutPort, Runtime, OutError)
			&& OutPort.IsValid();
	}
}

#include "OperationOwners/EditorSessionBuild/UeAgentEditorSessionBuildOwner.h"

#include "Dom/JsonValue.h"

#include "Core/Contract/UeAgentDiagnostic.h"

#include "OperationOwners/LongRunning/UeAgentLongRunningLifecycleOwner.h"

namespace UE::UAI::OperationOwners::EditorSessionBuild
{
	using namespace Composition::LongRunningSession;

	namespace
	{
		bool IsOwnedCommand(const FString& CommandId)
		{
			return CommandId == TEXT("editor.safe_exit")
				|| CommandId == TEXT("editor.session")
				|| CommandId == TEXT("job.compile_build");
		}
	}

	bool FUeAgentEditorSessionBuildRoute::IsValid() const
	{
		UE::UAI::Contract::FDiagnostic Error;
		return TryValidate(Error);
	}

	bool FUeAgentEditorSessionBuildRoute::TryValidate(UE::UAI::Contract::FDiagnostic& OutError) const
	{
		OutError.Reset();
		if (!IsOwnedCommand(CommandId))
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("editor_session_build_route_invalid"),
				TEXT("The command is not owned by the editor session/build route."), TEXT("route_validation"), FString(), CommandId);
			if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = CommandId;
			OutError.PropertyPath = TEXT("command_id");
			OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("editor.safe_exit, editor.session or job.compile_build"));
			OutError.ActualValue = MakeShared<FJsonValueString>(CommandId);
			return false;
		}
		if (PythonFunction.IsEmpty())
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("editor_session_build_route_invalid"),
				TEXT("The route requires a non-empty python_function."), TEXT("route_validation"), FString(), CommandId);
			if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = CommandId;
			OutError.PropertyPath = TEXT("python_function");
			OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("non-empty string"));
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

	bool TryGetRoutes(TArray<FUeAgentEditorSessionBuildRoute>& OutRoutes, UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutRoutes.Reset();
		OutError.Reset();
		const TArray<FUeAgentLongRunningSessionActivationProfile>* Profiles = nullptr;
		if (!TryGetLongRunningSessionActivationProfiles(Profiles, OutError)) return false;
		for (const FUeAgentLongRunningSessionActivationProfile& Profile : *Profiles)
		{
			if (!(IsOwnedCommand(Profile.CommandId))) continue;
			FUeAgentEditorSessionBuildRoute Route{Profile.CommandId, Profile.Method, Profile.DescriptorIdentity};
			if (!Route.TryValidate(OutError))
			{
				OutRoutes.Reset();
				return false;
			}
			OutRoutes.Add(MoveTemp(Route));
		}
		if (OutRoutes.Num() != 3)
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("editorsessionbuild_route_count_mismatch"),
				TEXT("The activation profiles did not produce the required number of command routes."), TEXT("route_construction"));
			OutError.ExpectedValue = MakeShared<FJsonValueNumber>(3);
			OutError.ActualValue = MakeShared<FJsonValueNumber>(OutRoutes.Num());
			OutRoutes.Reset();
			return false;
		}
		OutRoutes.Sort([](const auto& Left, const auto& Right) { return Left.CommandId < Right.CommandId; });
		return true;
	}

	bool TryGetProductionOperationPort(
		TSharedPtr<Contract::IUeAgentOperationApplicationPort>& OutPort,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		TSharedPtr<FUeAgentLongRunningSessionRuntime> Runtime;
		return OperationOwners::LongRunning::
			TryGetProductionLongRunningLifecyclePort(OutPort, Runtime, OutError)
			&& OutPort.IsValid();
	}
}

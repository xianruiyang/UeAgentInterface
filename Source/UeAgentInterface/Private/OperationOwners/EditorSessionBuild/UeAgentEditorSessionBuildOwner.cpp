#include "OperationOwners/EditorSessionBuild/UeAgentEditorSessionBuildOwner.h"

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
		return IsOwnedCommand(CommandId)
			&& !PythonFunction.IsEmpty()
			&& DescriptorIdentity.IsValid();
	}

	const TArray<FUeAgentEditorSessionBuildRoute>& GetRoutes()
	{
		static const TArray<FUeAgentEditorSessionBuildRoute> Routes = []()
		{
			TArray<FUeAgentEditorSessionBuildRoute> Result;
			for (const FUeAgentLongRunningSessionActivationProfile& Profile
				: GetLongRunningSessionActivationProfiles())
			{
				if (!IsOwnedCommand(Profile.CommandId))
				{
					continue;
				}
				FUeAgentEditorSessionBuildRoute Route{
					Profile.CommandId,
					Profile.Method,
					Profile.DescriptorIdentity,
				};
				if (!Route.IsValid())
				{
					Result.Reset();
					return Result;
				}
				Result.Add(MoveTemp(Route));
			}
			Result.Sort([](const auto& Left, const auto& Right)
			{
				return Left.CommandId < Right.CommandId;
			});
			if (Result.Num() != 3)
			{
				Result.Reset();
			}
			return Result;
		}();
		return Routes;
	}

	bool TryGetProductionOperationPort(
		TSharedPtr<Contract::IUeAgentOperationApplicationPort>& OutPort,
		FString& OutError)
	{
		TSharedPtr<FUeAgentLongRunningSessionRuntime> Runtime;
		return OperationOwners::LongRunning::
			TryGetProductionLongRunningLifecyclePort(OutPort, Runtime, OutError)
			&& OutPort.IsValid();
	}
}

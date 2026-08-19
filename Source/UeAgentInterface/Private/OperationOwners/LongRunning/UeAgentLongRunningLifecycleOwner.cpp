#include "OperationOwners/LongRunning/UeAgentLongRunningLifecycleOwner.h"

#include "Composition/LongRunningSession/UeAgentLongRunningSessionProduction.h"
#include "Editor.h"
#include "HAL/PlatformProcess.h"
#include "Misc/App.h"
#include "Misc/Guid.h"
#include "Misc/ScopeLock.h"

namespace UE::UAI::OperationOwners::LongRunning
{
	using namespace Composition::LongRunningSession;

	namespace
	{
		constexpr int32 ExpectedLifecycleRouteCount = 6;

		struct FProductionHost final
		{
			FCriticalSection Mutex;
			TSharedPtr<Contract::IUeAgentOperationApplicationPort> Port;
			TSharedPtr<FUeAgentLongRunningSessionRuntime> Runtime;
			FString ServerSessionId = TEXT("server-session:")
				+ FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
			FString LeaseSecret = FGuid::NewGuid().ToString(
				EGuidFormats::DigitsWithHyphensLower);
		};

		FProductionHost& ProductionHost()
		{
			static FProductionHost Host;
			return Host;
		}

		bool TryCreateRuntimeLocked(
			FProductionHost& Host,
			FString& OutError)
		{
			if (Host.Runtime.IsValid())
			{
				return true;
			}
			if (!IsInGameThread())
			{
				OutError = TEXT("long_running_lifecycle_game_thread_required");
				return false;
			}
			UWorld* const World = GEditor
				? GEditor->GetEditorWorldContext().World()
				: nullptr;
			if (!World)
			{
				OutError = TEXT("long_running_lifecycle_editor_world_unavailable");
				return false;
			}

			FUeAgentLongRunningSessionProductionConfig Config;
			Config.ProjectInstanceId = TEXT("project:")
				+ FString(FApp::GetProjectName());
			Config.EditorInstanceId = FString::Printf(
				TEXT("editor-instance:editor-process:%u"),
				FPlatformProcess::GetCurrentProcessId());
			Config.ServerSessionId = Host.ServerSessionId;
			Config.OwnerLeaseSecret = Host.LeaseSecret;
			Config.EditorWorld = World;
			Config.TimeoutSeconds = 300.0;
			Config.MaxFrameBytes = 4LL * 1024LL * 1024LL;
			Config.MaxActiveOperations = 32;
			Config.MaxRetainedOperations = 128;

			TArray<TSharedRef<CommandEdge::IUeAgentCommandHandler>> LegacyHandlers;
			TArray<FString> AuthorityScopes;
			FUeAgentLongRunningSessionProductionError Error;
			if (!TryCreateLongRunningSessionProduction(
					Config,
					Host.Runtime,
					LegacyHandlers,
					AuthorityScopes,
					Error)
				|| !Host.Runtime.IsValid())
			{
				Host.Runtime.Reset();
				OutError = Error.Code.IsEmpty()
					? FString(TEXT("long_running_lifecycle_runtime_create_failed"))
					: Error.Code + TEXT(":") + Error.Detail;
				return false;
			}
			return true;
		}

		class FProcessOperationPort final
			: public Contract::IUeAgentOperationApplicationPort
		{
		public:
			virtual Contract::FUeAgentOperationResult Submit(
				const Contract::FUeAgentOperationRequest& Request,
				const Contract::FUeAgentCallerContext& CallerContext,
				const TSharedRef<Contract::IUeAgentCancellationSignal>& Cancellation)
				override
			{
				TSharedPtr<Contract::IUeAgentOperationApplicationPort> RuntimePort;
				FString Error;
				{
					FProductionHost& Host = ProductionHost();
					FScopeLock Lock(&Host.Mutex);
					if (TryCreateRuntimeLocked(Host, Error))
					{
						RuntimePort = Host.Runtime->GetOperationPort();
					}
				}
				if (!RuntimePort.IsValid())
				{
					const FString ErrorPayload =
						FString(TEXT("{\"operation_error\":{"))
						+ TEXT("\"code\":\"long_running_lifecycle_runtime_unavailable\",")
						+ TEXT("\"detail\":\"") + Error
						+ TEXT("\",\"subject_id\":\"")
						+ Request.GetRequestId() + TEXT("\"}}");
					return Contract::FUeAgentOperationResult(
						Request.GetDescriptorIdentity(),
						Request.GetRequestId(),
						Contract::FUeAgentContractValue(ErrorPayload));
				}
				return RuntimePort->Submit(Request, CallerContext, Cancellation);
			}
		};
	}

	bool FUeAgentLongRunningLifecycleRoute::IsValid() const
	{
		return !CommandId.IsEmpty()
			&& !PythonFunction.IsEmpty()
			&& DescriptorIdentity.IsValid();
	}

	const TArray<FUeAgentLongRunningLifecycleRoute>&
	GetLongRunningLifecycleRoutes()
	{
		static const TArray<FUeAgentLongRunningLifecycleRoute> Routes = []()
		{
			TArray<FUeAgentLongRunningLifecycleRoute> Result;
			for (const FUeAgentLongRunningSessionActivationProfile& Profile
				: GetLongRunningSessionActivationProfiles())
			{
				if (!Profile.IsLifecycleCommand())
				{
					continue;
				}
				FUeAgentLongRunningLifecycleRoute Route{
					Profile.CommandId,
					TEXT("operation_") + Profile.Method,
					Profile.DescriptorIdentity,
				};
				if (!Route.IsValid())
				{
					Result.Reset();
					return Result;
				}
				Result.Add(MoveTemp(Route));
			}
			if (Result.Num() != ExpectedLifecycleRouteCount)
			{
				Result.Reset();
			}
			return Result;
		}();
		return Routes;
	}

	bool TryGetProductionLongRunningLifecyclePort(
		TSharedPtr<Contract::IUeAgentOperationApplicationPort>& OutPort,
		TSharedPtr<FUeAgentLongRunningSessionRuntime>& OutRuntime,
		FString& OutError)
	{
		OutPort.Reset();
		OutRuntime.Reset();
		OutError.Reset();
		FProductionHost& Host = ProductionHost();
		FScopeLock Lock(&Host.Mutex);
		if (!Host.Port.IsValid())
		{
			Host.Port = MakeShared<FProcessOperationPort>();
		}
		if (!Host.Runtime.IsValid()
			&& IsInGameThread()
			&& GEditor
			&& GEditor->GetEditorWorldContext().World())
		{
			TryCreateRuntimeLocked(Host, OutError);
		}
		OutRuntime = Host.Runtime;
		OutPort = Host.Port;
		return OutPort.IsValid();
	}
}

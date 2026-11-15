#include "OperationOwners/LongRunning/UeAgentLongRunningLifecycleOwner.h"

#include "Dom/JsonValue.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "Core/AssetPreview/UeAgentAssetEditorLifetime.h"

#include "Composition/LongRunningSession/UeAgentLongRunningSessionProduction.h"
#include "Editor.h"
#include "CoreGlobals.h"
#include "HAL/PlatformProcess.h"
#include "Misc/App.h"
#include "Misc/Guid.h"
#include "Misc/ScopeLock.h"
#include "Misc/CoreDelegates.h"
#include "Containers/Ticker.h"
#include "Async/TaskGraphInterfaces.h"
#include "HAL/PlatformTime.h"
#include "Logging/LogMacros.h"

namespace UE::UAI::OperationOwners::LongRunning
{
	using namespace Composition::LongRunningSession;

	namespace
	{
		void DrainProductionLongRunningLifecycle();

		constexpr int32 ExpectedLifecycleRouteCount = 9;

		struct FProductionHost final
		{
			FCriticalSection Mutex;
			TSharedPtr<Contract::IUeAgentOperationApplicationPort> Port;
			TSharedPtr<FUeAgentLongRunningSessionRuntime> Runtime;
			FTSTicker::FDelegateHandle PollTicker;
			FDelegateHandle PreExitHandle;
			bool bPreExitDrained = false;
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
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			if (Host.Runtime.IsValid())
			{
				return true;
			}
			if (!IsInGameThread())
			{
				OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("long_running_lifecycle_game_thread_required"),
					TEXT("The long-running operation runtime must be created on the game thread."), TEXT("runtime_prepare"));
				OutError.SourceKind = TEXT("native_result");
				OutError.PropertyPath = TEXT("game_thread");
				OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
				OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
				return false;
			}
			UWorld* const World = GEditor
				? GEditor->GetEditorWorldContext().World()
				: nullptr;
			if (!World)
			{
				OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("long_running_lifecycle_editor_world_unavailable"),
					TEXT("The long-running operation runtime requires an available Editor world."), TEXT("runtime_prepare"));
				OutError.SourceKind = TEXT("native_result");
				OutError.PropertyPath = TEXT("editor_world_available");
				OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
				OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
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


			TArray<TSharedRef<CommandEdge::IUeAgentCommandHandler>> LegacyHandlers;
			TArray<FString> AuthorityScopes;
			FUeAgentLongRunningSessionProductionError Error;
			if (!TryCreateLongRunningSessionProduction(
					Config,
					Host.Runtime,
					LegacyHandlers,
					AuthorityScopes,
					Error))
			{
				Host.Runtime.Reset();
				OutError = MoveTemp(Error);
				return false;
			}
			Host.PollTicker = FTSTicker::GetCoreTicker().AddTicker(
				FTickerDelegate::CreateLambda([](float)
				{
					FProductionHost& CurrentHost = ProductionHost();
					TSharedPtr<FUeAgentLongRunningSessionRuntime> Runtime;
					{
						FScopeLock Lock(&CurrentHost.Mutex);
						Runtime = CurrentHost.Runtime;
					}
					if (Runtime) Runtime->Pump();
					return true;
				}), 0.1f);
			if (!Host.PreExitHandle.IsValid())
				Host.PreExitHandle = FCoreDelegates::OnEnginePreExit.AddLambda(
					[]() { DrainProductionLongRunningLifecycle(); });
			return true;
		}

		// A concurrent control read borrows the existing port under the host
		// lifetime lock. Never copy its default-mode shared reference on the
		// network thread, or create a runtime/World there.
		thread_local Contract::IUeAgentOperationApplicationPort* BorrowedReadPort = nullptr;
		class FProcessReadScope final : public Contract::IUeAgentOperationReadScope
		{
		public:
			explicit FProcessReadScope(const Contract::FUeAgentOperationRequest& Request)
				: Host(ProductionHost()), Previous(BorrowedReadPort)
			{
				bLocked = Host.Mutex.TryLock();
				if (!bLocked || !Host.Runtime) return;
				auto& Port = Host.Runtime->GetOperationPort().Get();
				Inner = Port.TryBeginConcurrentRead(Request);
				if (Inner) BorrowedReadPort = &Port;
			}
			~FProcessReadScope() override
			{
				Inner.Reset();
				BorrowedReadPort = Previous;
				if (bLocked) Host.Mutex.Unlock();
			}
			bool IsAvailable() const { return Inner.IsValid(); }
		private:
			FProductionHost& Host;
			Contract::IUeAgentOperationApplicationPort* Previous;
			TUniquePtr<Contract::IUeAgentOperationReadScope> Inner;
			bool bLocked = false;
		};
		class FProcessOperationPort final
			: public Contract::IUeAgentOperationApplicationPort
		{
		public:
			TUniquePtr<Contract::IUeAgentOperationReadScope> TryBeginConcurrentRead(
				const Contract::FUeAgentOperationRequest& Request) override
			{
				auto Scope = MakeUnique<FProcessReadScope>(Request);
				if (!Scope->IsAvailable()) return {};
				return Scope;
			}
			virtual Contract::FUeAgentOperationResult Submit(
				const Contract::FUeAgentOperationRequest& Request,
				const Contract::FUeAgentCallerContext& CallerContext,
				const TSharedRef<Contract::IUeAgentCancellationSignal>& Cancellation)
				override
			{
				if (BorrowedReadPort) return BorrowedReadPort->Submit(Request, CallerContext, Cancellation);
				TSharedPtr<Contract::IUeAgentOperationApplicationPort> RuntimePort;
				UE::UAI::Contract::FDiagnostic Error;
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
                    if (Error.IsEmpty())
                    {
                        Error = UE::UAI::Contract::FDiagnostic::Failure(TEXT("long_running_lifecycle_runtime_unavailable"),
                            TEXT("The long-running operation runtime has no available application port."), TEXT("runtime_prepare"));
                        Error.SourceKind = TEXT("native_result");
                        Error.PropertyPath = TEXT("operation_port_ready");
                        Error.ActualValue = MakeShared<FJsonValueBoolean>(false);
                        Error.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
                    }
                    return Contract::FUeAgentOperationResult(
                        Request.GetDescriptorIdentity(), Request.GetRequestId(), MoveTemp(Error));
				}
				return RuntimePort->Submit(Request, CallerContext, Cancellation);
			}
		};
	}

	bool FUeAgentLongRunningLifecycleRoute::IsValid() const
	{
		UE::UAI::Contract::FDiagnostic Error;
		return TryValidate(Error);
	}

	bool FUeAgentLongRunningLifecycleRoute::TryValidate(UE::UAI::Contract::FDiagnostic& OutError) const
	{
		OutError.Reset();
		if (CommandId.IsEmpty())
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("long_running_lifecycle_route_invalid"),
				TEXT("The route requires a non-empty command_id."), TEXT("route_validation"), FString(), CommandId);
			if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = CommandId;
			OutError.PropertyPath = TEXT("command_id");
			OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("non-empty string"));
			OutError.ActualValue = MakeShared<FJsonValueString>(CommandId);
			return false;
		}
		if (PythonFunction.IsEmpty())
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("long_running_lifecycle_route_invalid"),
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

	bool TryGetLongRunningLifecycleRoutes(TArray<FUeAgentLongRunningLifecycleRoute>& OutRoutes, UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutRoutes.Reset();
		OutError.Reset();
		const TArray<FUeAgentLongRunningSessionActivationProfile>* Profiles = nullptr;
		if (!TryGetLongRunningSessionActivationProfiles(Profiles, OutError)) return false;
		for (const FUeAgentLongRunningSessionActivationProfile& Profile : *Profiles)
		{
			if (!(Profile.IsLifecycleCommand())) continue;
			FUeAgentLongRunningLifecycleRoute Route{Profile.CommandId, TEXT("operation_") + Profile.Method, Profile.DescriptorIdentity};
			if (!Route.TryValidate(OutError))
			{
				OutRoutes.Reset();
				return false;
			}
			OutRoutes.Add(MoveTemp(Route));
		}
		if (OutRoutes.Num() != ExpectedLifecycleRouteCount)
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("longrunning_route_count_mismatch"),
				TEXT("The activation profiles did not produce the required number of command routes."), TEXT("route_construction"));
			OutError.ExpectedValue = MakeShared<FJsonValueNumber>(ExpectedLifecycleRouteCount);
			OutError.ActualValue = MakeShared<FJsonValueNumber>(OutRoutes.Num());
			OutRoutes.Reset();
			return false;
		}
		return true;
	}

	bool TryGetProductionLongRunningLifecyclePort(
		TSharedPtr<Contract::IUeAgentOperationApplicationPort>& OutPort,
		TSharedPtr<FUeAgentLongRunningSessionRuntime>& OutRuntime,
		UE::UAI::Contract::FDiagnostic& OutError)
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

	namespace
	{
	void DrainProductionLongRunningLifecycle()
	{
		FProductionHost& Host = ProductionHost();
		TSharedPtr<FUeAgentLongRunningSessionRuntime> Runtime;
		{
			FScopeLock Lock(&Host.Mutex);
			Runtime = Host.Runtime;
			if (Host.bPreExitDrained) return;
			Host.bPreExitDrained = true;
			FTSTicker::GetCoreTicker().RemoveTicker(Host.PollTicker);
			Host.PollTicker.Reset();
			FCoreDelegates::OnEnginePreExit.Remove(Host.PreExitHandle);
			Host.PreExitHandle.Reset();
		}
		if (!Runtime)
		{
			UE::UAI::AssetPreview::DetachAssetEditorTracking();
			return;
		}
		Runtime->RequestShutdown();
		const double Deadline = FPlatformTime::Seconds() + 10.0;
		while (Runtime->IsPolling() || Runtime->GetPendingCount() > 0)
		{
			// Native ports queue stop work to GT; keep that thread available
			// while the worker observes and releases their resources.
			if (IsInGameThread())
			{
				FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
				FTSTicker::GetCoreTicker().Tick(0.01f);
			}
			if (!Runtime->IsPolling())
			{
				if (FPlatformTime::Seconds() >= Deadline) break;
				Runtime->Pump();
			}
			FPlatformProcess::Sleep(0.01f);
		}
		if (Runtime->GetPendingCount() > 0)
			UE_LOG(LogTemp, Error, TEXT("UAI shutdown: %d operations could not confirm native cleanup before engine exit."),
				Runtime->GetPendingCount());
		// Pre-exit witnesses share this broadcast. Keep their owners alive until
		// all callbacks have run; module shutdown releases the host afterwards.
		UE::UAI::AssetPreview::DetachAssetEditorTracking();
	}
	}

	void InitializeProductionLongRunningLifecycle()
	{
		FProductionHost& Host = ProductionHost();
		FScopeLock Lock(&Host.Mutex);
		if (!Host.PreExitHandle.IsValid())
			Host.PreExitHandle = FCoreDelegates::OnEnginePreExit.AddLambda(
				[]() { DrainProductionLongRunningLifecycle(); });
	}

	void ShutdownProductionLongRunningLifecycle()
	{
		// During engine exit the object subsystem is already closed here.
		// Never pump global tickers or GT callbacks a second time.
		if (!IsEngineExitRequested()) DrainProductionLongRunningLifecycle();
		FProductionHost& Host = ProductionHost();
		FScopeLock Lock(&Host.Mutex);
		Host.Runtime.Reset();
		Host.Port.Reset();
	}
}

#include "NetworkTransport/UeAgentNetworkTransport.h"

#if __has_include("ServiceComposition/CommandService/UeAgentCommandService.h") \
	&& __has_include("ServiceComposition/Transport/UeAgentTransportService.h")
#define UAI_STRICT_V2_T01_IMPLEMENTATION_AVAILABLE 1
#include "ServiceComposition/CommandService/UeAgentCommandService.h"
#include "ServiceComposition/Transport/UeAgentTransportService.h"
#else
#define UAI_STRICT_V2_T01_IMPLEMENTATION_AVAILABLE 0
#endif

#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace
{
	using namespace UE::UAI::NetworkTransport;

	struct FServerProbe
	{
		int32 StartCalls = 0;
		int32 StopCalls = 0;
		bool bFailStart = false;
		bool bFailStop = false;
	};

	class FFakeServerControl final : public IUeAgentNetworkServerControl
	{
	public:
		explicit FFakeServerControl(TSharedRef<FServerProbe> InProbe)
			: Probe(MoveTemp(InProbe))
		{
		}

		virtual bool Start(
			FUeAgentTransportServerIdentity& OutIdentity,
			FUeAgentTransportDiagnostic& OutDiagnostic) override
		{
			++Probe->StartCalls;
			if (Probe->bFailStart)
			{
				OutDiagnostic = {
					TEXT("fixture_start_failed"),
					TEXT("fixture refused to start"),
				};
				return false;
			}
			bRunning = true;
			OutIdentity = Identity();
			return true;
		}

		virtual bool Stop(
			const FString& OwnerId,
			FUeAgentTransportDiagnostic& OutDiagnostic) override
		{
			++Probe->StopCalls;
			if (OwnerId != TEXT("owner-a"))
			{
				OutDiagnostic = {
					TEXT("fixture_owner_mismatch"),
					TEXT("fixture received a different owner"),
				};
				return false;
			}
			if (Probe->bFailStop)
			{
				OutDiagnostic = {
					TEXT("fixture_stop_failed"),
					TEXT("fixture refused to stop"),
				};
				return false;
			}
			bRunning = false;
			return true;
		}

		virtual FUeAgentTransportServerHealth Health() const override
		{
			FUeAgentTransportServerHealth Result;
			Result.bRunning = bRunning;
			return Result;
		}

		virtual FUeAgentTransportServerIdentity Identity() const override
		{
			return {TEXT("owner-a"), TEXT("instance-a")};
		}

	private:
		TSharedRef<FServerProbe> Probe;
		bool bRunning = false;
	};

	class FEchoHandler final : public IUeAgentTransportRequestHandler
	{
	public:
		virtual FUeAgentTransportReply Handle(
			const FUeAgentTransportIngress& Ingress) override
		{
			++Calls;
			return FUeAgentTransportReply(Ingress.Frame);
		}

		int32 Calls = 0;
	};

#if UAI_STRICT_V2_T01_IMPLEMENTATION_AVAILABLE
	class FContextInvalidator final
		: public UE::UAI::ServiceComposition::CommandService::
			IUeAgentRequestContextInvalidator
	{
	public:
		virtual void InvalidateWorld(
			const FString& WorldIdentity) override
		{
			InvalidatedWorlds.Add(WorldIdentity);
		}

		TArray<FString> InvalidatedWorlds;
	};

	static UE::UAI::ServiceComposition::Transport::
		FUeAgentTransportServiceConfig MakeServiceConfig()
	{
		using namespace UE::UAI::ServiceComposition::Transport;
		FUeAgentTransportServiceConfig Config;
		Config.OwnerId = TEXT("owner-a");
		Config.Capabilities.bOrdered = true;
		Config.Capabilities.bReliable = true;
		Config.Capabilities.bPreservesMessageBoundaries = true;
		Config.Capabilities.MaxFrameBytes = 1024;
		Config.Capabilities.bRequestReply = true;
		Config.Requirements.bRequireOrdered = true;
		Config.Requirements.bRequireReliable = true;
		Config.Requirements.bRequireMessageBoundaries = true;
		Config.Requirements.MaxFrameBytes = 512;
		Config.Requirements.bRequireRequestReply = true;
		return Config;
	}
#endif

	static FString ProductSource(const FString& RelativePath)
	{
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(
			FPaths::ProjectPluginsDir(),
			TEXT("UeAgentInterface/Source/UeAgentInterface"),
			RelativePath));
	}

	static bool LoadSource(
		FAutomationTestBase& Test,
		const FString& RelativePath,
		FString& OutSource)
	{
		return Test.TestTrue(
			*FString::Printf(TEXT("load source %s"), *RelativePath),
			FFileHelper::LoadFileToString(
				OutSource,
				*ProductSource(RelativePath)));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUaiStrictV2T01OwnedTransportLifecycleTest,
	"UAI.StrictV2.T01.TRANSPORT.SERVICE.OwnedLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUaiStrictV2T01OwnedTransportLifecycleTest::RunTest(
	const FString& Parameters)
{
#if !UAI_STRICT_V2_T01_IMPLEMENTATION_AVAILABLE
	AddError(TEXT("strict_v2_t01_transport_service_missing"));
	return false;
#else
	using namespace UE::UAI::ServiceComposition::Transport;

	const TSharedRef<FServerProbe> Probe = MakeShared<FServerProbe>();
	TUniquePtr<IUeAgentNetworkServerControl> Control =
		MakeUnique<FFakeServerControl>(Probe);
	TUniquePtr<FUeAgentTransportService> Service;
	FUeAgentTransportDiagnostic Diagnostic;
	TestTrue(
		TEXT("transport-neutral service is created"),
		FUeAgentTransportService::TryCreate(
			MakeServiceConfig(),
			MoveTemp(Control),
			Service,
			Diagnostic));
	TestTrue(TEXT("service starts"), Service->Start(Diagnostic));
	TestTrue(TEXT("repeat start is idempotent"), Service->Start(Diagnostic));
	TestEqual(TEXT("underlying server starts once"), Probe->StartCalls, 1);
	TestTrue(TEXT("service reports running"), Service->IsRunning());
	TestTrue(TEXT("service stops"), Service->Stop(Diagnostic));
	TestTrue(TEXT("repeat stop is idempotent"), Service->Stop(Diagnostic));
	TestEqual(TEXT("underlying server stops once"), Probe->StopCalls, 1);
	TestFalse(TEXT("service reports stopped"), Service->IsRunning());

	const TSharedRef<FServerProbe> CleanupProbe = MakeShared<FServerProbe>();
	TUniquePtr<IUeAgentNetworkServerControl> CleanupControl =
		MakeUnique<FFakeServerControl>(CleanupProbe);
	TUniquePtr<FUeAgentTransportService> CleanupService;
	TestTrue(
		TEXT("cleanup service is created"),
		FUeAgentTransportService::TryCreate(
			MakeServiceConfig(),
			MoveTemp(CleanupControl),
			CleanupService,
			Diagnostic));
	TestTrue(
		TEXT("cleanup service starts"),
		CleanupService->Start(Diagnostic));
	CleanupService.Reset();
	TestEqual(
		TEXT("destruction stops the owned server exactly once"),
		CleanupProbe->StopCalls,
		1);
	return true;
#endif
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUaiStrictV2T01TransportFailureSemanticsTest,
	"UAI.StrictV2.T01.TRANSPORT.SERVICE.FailureSemantics",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUaiStrictV2T01TransportFailureSemanticsTest::RunTest(
	const FString& Parameters)
{
#if !UAI_STRICT_V2_T01_IMPLEMENTATION_AVAILABLE
	AddError(TEXT("strict_v2_t01_transport_service_missing"));
	return false;
#else
	using namespace UE::UAI::ServiceComposition::Transport;

	const TSharedRef<FServerProbe> CapabilityProbe = MakeShared<FServerProbe>();
	TUniquePtr<IUeAgentNetworkServerControl> CapabilityControl =
		MakeUnique<FFakeServerControl>(CapabilityProbe);
	FUeAgentTransportServiceConfig Unsupported = MakeServiceConfig();
	Unsupported.Capabilities.bReliable = false;
	TUniquePtr<FUeAgentTransportService> RejectedService;
	FUeAgentTransportDiagnostic Diagnostic;
	TestFalse(
		TEXT("unsupported delivery capability is rejected before start"),
		FUeAgentTransportService::TryCreate(
			Unsupported,
			MoveTemp(CapabilityControl),
			RejectedService,
			Diagnostic));
	TestEqual(
		TEXT("capability rejection remains explicit"),
		Diagnostic.Code,
		FString(TEXT("transport_capability_unsupported")));
	TestEqual(
		TEXT("rejected capability never starts transport"),
		CapabilityProbe->StartCalls,
		0);

	const TSharedRef<FServerProbe> StartProbe = MakeShared<FServerProbe>();
	StartProbe->bFailStart = true;
	TUniquePtr<IUeAgentNetworkServerControl> StartControl =
		MakeUnique<FFakeServerControl>(StartProbe);
	TUniquePtr<FUeAgentTransportService> Service;
	TestTrue(
		TEXT("valid service is created before runtime failure"),
		FUeAgentTransportService::TryCreate(
			MakeServiceConfig(),
			MoveTemp(StartControl),
			Service,
			Diagnostic));
	TestFalse(TEXT("start failure is returned"), Service->Start(Diagnostic));
	TestEqual(
		TEXT("transport diagnostic is preserved"),
		Diagnostic.Code,
		FString(TEXT("fixture_start_failed")));
	TestFalse(TEXT("failed start does not report running"), Service->IsRunning());
	TestEqual(TEXT("start is not replayed"), StartProbe->StartCalls, 1);
	return true;
#endif
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUaiStrictV2T01WorldContextInvalidationTest,
	"UAI.StrictV2.T01.TRANSPORT.SERVICE.WorldContextInvalidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUaiStrictV2T01WorldContextInvalidationTest::RunTest(
	const FString& Parameters)
{
#if !UAI_STRICT_V2_T01_IMPLEMENTATION_AVAILABLE
	AddError(TEXT("strict_v2_t01_command_service_missing"));
	return false;
#else
	using namespace UE::UAI::ServiceComposition::CommandService;

	const TSharedRef<FEchoHandler, ESPMode::ThreadSafe> Handler =
		MakeShared<FEchoHandler, ESPMode::ThreadSafe>();
	const TSharedRef<FContextInvalidator, ESPMode::ThreadSafe> Invalidator =
		MakeShared<FContextInvalidator, ESPMode::ThreadSafe>();
	TUniquePtr<FUeAgentCommandService> Service;
	FString Error;
	TestTrue(
		TEXT("command service is created from transport-neutral ingress"),
		FUeAgentCommandService::TryCreate(
			Handler,
			Invalidator,
			Service,
			Error));
	IUeAgentTransportRequestHandler* Before =
		&Service->GetIngressHandler().Get();
	Service->NotifyWorldChanged(TEXT("/Game/Maps/A"));
	Service->NotifyWorldChanged(TEXT("/Game/Maps/B"));
	IUeAgentTransportRequestHandler* After =
		&Service->GetIngressHandler().Get();
	TestTrue(
		TEXT("World changes do not rebuild the command ingress"),
		Before == After);
	TestEqual(
		TEXT("only request-context generation advances"),
		Service->GetContextGeneration(),
		static_cast<uint64>(2));
	TestEqual(
		TEXT("both World identities are invalidated"),
		Invalidator->InvalidatedWorlds.Num(),
		2);
	TestEqual(
		TEXT("latest World identity is preserved"),
		Invalidator->InvalidatedWorlds.Last(),
		FString(TEXT("/Game/Maps/B")));
	return true;
#endif
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUaiStrictV2T01ActiveHttpBoundaryTest,
	"UAI.StrictV2.T01.TRANSPORT.SERVICE.ActiveHttpBoundary",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUaiStrictV2T01ActiveHttpBoundaryTest::RunTest(
	const FString& Parameters)
{
	FString ModuleSource;
	if (!LoadSource(
			*this,
			TEXT("Private/Module/UeAgentInterfaceModule.cpp"),
			ModuleSource))
	{
		return false;
	}
	TestTrue(
		TEXT("module owns the concrete HTTP adapter"),
		ModuleSource.Contains(TEXT("UeAgentHttpTransportAdapter.h")));
	TestTrue(
		TEXT("module owns the engine HTTP route host"),
		ModuleSource.Contains(TEXT("FUeAgentEngineHttpRouteHost")));
	TestTrue(
		TEXT("module composes the transport-neutral service"),
		ModuleSource.Contains(TEXT("FUeAgentTransportService")));
	TestTrue(
		TEXT("module composes the command service"),
		ModuleSource.Contains(TEXT("FUeAgentCommandService")));
	TestFalse(
		TEXT("legacy HTTP server is unreachable from module startup"),
		ModuleSource.Contains(TEXT("FUeAgentHttpServer")));
	TestFalse(
		TEXT("legacy query composition is unreachable from module startup"),
		ModuleSource.Contains(TEXT("FUeAgentQueryServiceComposition")));
	TestFalse(
		TEXT("startup does not register legacy Authoring providers"),
		ModuleSource.Contains(TEXT("RegisterAuthoring(")));
	TestFalse(
		TEXT("startup does not register legacy HJSON compositions"),
		ModuleSource.Contains(
			TEXT("RegisterGraphTreeTargetCompositionContracts"))
			|| ModuleSource.Contains(
				TEXT("RegisterOtherTargetCompositionContracts")));
	TestFalse(
		TEXT("World changes do not replace the service composition"),
		ModuleSource.Contains(TEXT("ReplaceQueryCompositionForWorld")));

	for (const FString& RelativePath : {
		TEXT("Private/ServiceComposition/Transport/UeAgentTransportService.h"),
		TEXT("Private/ServiceComposition/Transport/UeAgentTransportService.cpp"),
		TEXT("Private/ServiceComposition/CommandService/UeAgentCommandService.h"),
		TEXT("Private/ServiceComposition/CommandService/UeAgentCommandService.cpp"),
	})
	{
		FString Source;
		if (!LoadSource(*this, RelativePath, Source))
		{
			continue;
		}
		TestFalse(
			*FString::Printf(
				TEXT("transport-neutral service excludes HTTP: %s"),
				*RelativePath),
			Source.Contains(TEXT("Http"))
				|| Source.Contains(TEXT("HTTP")));
	}
	return !HasAnyErrors();
}

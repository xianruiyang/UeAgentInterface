#include "ServiceComposition/Transport/UeAgentTransportService.h"

namespace UE::UAI::ServiceComposition::Transport
{
	using namespace NetworkTransport;

	bool FUeAgentTransportServiceConfig::IsValid() const
	{
		return !OwnerId.IsEmpty();
	}

	FUeAgentTransportService::~FUeAgentTransportService()
	{
		if (bStarted)
		{
			FUeAgentTransportDiagnostic Ignored;
			Stop(Ignored);
		}
	}

	bool FUeAgentTransportService::TryCreate(
		const FUeAgentTransportServiceConfig& Config,
		TUniquePtr<IUeAgentNetworkServerControl> Control,
		TUniquePtr<FUeAgentTransportService>& OutService,
		FUeAgentTransportDiagnostic& OutDiagnostic)
	{
		OutService.Reset();
		OutDiagnostic = {};
		if (!Config.IsValid() || !Control)
		{
			OutDiagnostic = {
				TEXT("transport_service_config_invalid"),
				TEXT("transport service requires an owner and server control"),
			};
			return false;
		}

		FUeAgentCapabilityBinding Binding;
		FUeAgentTransportBindingError BindingError;
		if (!TryBindCapabilities(
				Config.Capabilities,
				Config.Requirements,
				Binding,
				BindingError))
		{
			OutDiagnostic = {
				BindingError.Code,
				FString::Join(BindingError.MissingCapabilities, TEXT(",")),
			};
			return false;
		}

		OutService = TUniquePtr<FUeAgentTransportService>(
			new FUeAgentTransportService(
				Config,
				MoveTemp(Control),
				MoveTemp(Binding)));
		return true;
	}

	bool FUeAgentTransportService::Start(
		FUeAgentTransportDiagnostic& OutDiagnostic)
	{
		OutDiagnostic = {};
		if (bStarted)
		{
			if (Control->Health().bRunning)
			{
				return true;
			}
			OutDiagnostic = {
				TEXT("transport_service_unhealthy"),
				TEXT("owned server stopped outside its lifecycle owner"),
			};
			return false;
		}

		FUeAgentTransportServerIdentity StartedIdentity;
		if (!Control->Start(StartedIdentity, OutDiagnostic))
		{
			return false;
		}
		if (StartedIdentity.OwnerId != Config.OwnerId)
		{
			FUeAgentTransportDiagnostic CleanupDiagnostic;
			Control->Stop(StartedIdentity.OwnerId, CleanupDiagnostic);
			OutDiagnostic = {
				TEXT("transport_server_owner_mismatch"),
				TEXT("server identity does not match the lifecycle owner"),
			};
			return false;
		}
		if (!Control->Health().bRunning)
		{
			FUeAgentTransportDiagnostic CleanupDiagnostic;
			Control->Stop(Config.OwnerId, CleanupDiagnostic);
			OutDiagnostic = {
				TEXT("transport_server_not_running"),
				TEXT("server start returned without a running owned instance"),
			};
			return false;
		}

		Identity = MoveTemp(StartedIdentity);
		bStarted = true;
		return true;
	}

	bool FUeAgentTransportService::Stop(
		FUeAgentTransportDiagnostic& OutDiagnostic)
	{
		OutDiagnostic = {};
		if (!bStarted)
		{
			return true;
		}
		if (!Control->Stop(Config.OwnerId, OutDiagnostic))
		{
			return false;
		}
		bStarted = false;
		Identity = {};
		return true;
	}

	bool FUeAgentTransportService::IsRunning() const
	{
		return bStarted && Control && Control->Health().bRunning;
	}

	const FUeAgentTransportServerIdentity&
	FUeAgentTransportService::GetIdentity() const
	{
		return Identity;
	}

	FUeAgentTransportService::FUeAgentTransportService(
		FUeAgentTransportServiceConfig InConfig,
		TUniquePtr<IUeAgentNetworkServerControl> InControl,
		FUeAgentCapabilityBinding InBinding)
		: Config(MoveTemp(InConfig))
		, Control(MoveTemp(InControl))
		, Binding(MoveTemp(InBinding))
	{
	}
}

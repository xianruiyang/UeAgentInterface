#include "LifecycleHost/UeAgentLifecycleHost.h"

namespace UE::UAI::LifecycleHost
{
	namespace
	{
		static void SetDiagnostic(
			FUeAgentLifecycleDiagnostic& OutDiagnostic,
			const TCHAR* Code,
			FString Message = FString())
		{
			OutDiagnostic = FUeAgentLifecycleDiagnostic::Failure(Code, MoveTemp(Message), TEXT("lifecycle_host"));
		}
	}

	bool FUeAgentProcessIdentity::Matches(
		const FUeAgentProcessIdentity& Other) const
	{
		return ProcessId == Other.ProcessId
			&& StartTimeUtc == Other.StartTimeUtc
			&& Executable.Equals(
				Other.Executable,
				ESearchCase::IgnoreCase);
	}

	bool FUeAgentProcessIdentity::IsValid() const
	{
		return ProcessId != 0
			&& !StartTimeUtc.TrimStartAndEnd().IsEmpty()
			&& !Executable.TrimStartAndEnd().IsEmpty();
	}

	bool FUeAgentHostIdentity::IsValid() const
	{
		return !HostId.TrimStartAndEnd().IsEmpty()
			&& !SessionId.TrimStartAndEnd().IsEmpty()
			&& HostId != SessionId
			&& Process.IsValid();
	}

	bool FUeAgentOwnedWorkerSpec::IsValid() const
	{
		if (HostId.TrimStartAndEnd().IsEmpty()
			|| SessionId.TrimStartAndEnd().IsEmpty()
			|| HostId == SessionId
			|| Executable.TrimStartAndEnd().IsEmpty()
			|| ProjectPath.TrimStartAndEnd().IsEmpty())
		{
			return false;
		}
		for (const FString& Argument : Arguments)
		{
			if (Argument.TrimStartAndEnd().IsEmpty())
			{
				return false;
			}
		}
		return true;
	}

	bool FUeAgentTickerIdentity::IsValid() const
	{
		return !OwnerId.TrimStartAndEnd().IsEmpty()
			&& !TickerId.TrimStartAndEnd().IsEmpty()
			&& !HostId.TrimStartAndEnd().IsEmpty();
	}

	FUeAgentLifecycleHost::FUeAgentLifecycleHost(
		FString InOwnerId,
		const ContextResource::FUeAgentOperationDeadline& InDeadline,
		const ContextResource::FUeAgentCancellationToken& InCancellation,
		TSharedRef<IUeAgentProcessLifecycleBackend> InProcessBackend,
		TSharedRef<IUeAgentTickerLifecycleBackend> InTickerBackend)
		: Owner(MoveTemp(InOwnerId))
		, Deadline(InDeadline)
		, Cancellation(InCancellation)
		, ProcessBackend(MoveTemp(InProcessBackend))
		, TickerBackend(MoveTemp(InTickerBackend))
	{
	}

	bool FUeAgentLifecycleHost::Checkpoint(
		const FString& Stage,
		FUeAgentLifecycleDiagnostic& OutDiagnostic) const
	{
		ContextResource::FUeAgentCancellationRequested CancellationError;
		if (!Cancellation.TryCheckpoint(CancellationError))
		{
			OutDiagnostic = CancellationError;
			if (OutDiagnostic.Stage.IsEmpty()) OutDiagnostic.Stage = Stage;
			return false;
		}

		return true;
	}

	bool FUeAgentLifecycleHost::StartOwnedWorker(
		const FUeAgentOwnedWorkerSpec& Spec,
		NetworkTransport::IUeAgentNetworkServerControl& InServerControl,
		FUeAgentHostIdentity& OutIdentity,
		FUeAgentLifecycleDiagnostic& OutDiagnostic)
	{
		OutIdentity = {};
		OutDiagnostic = {};
		if (Host.IsSet())
		{
			SetDiagnostic(
				OutDiagnostic,
				TEXT("lifecycle_host_already_started"));
			return false;
		}
		if (bCleaned)
		{
			SetDiagnostic(
				OutDiagnostic,
				TEXT("lifecycle_host_already_cleaned"));
			return false;
		}
		if (Owner.TrimStartAndEnd().IsEmpty() || !Spec.IsValid())
		{
			SetDiagnostic(
				OutDiagnostic,
				TEXT("owned_worker_spec_invalid"));
			return false;
		}
		if (!Checkpoint(TEXT("lifecycle_start"), OutDiagnostic))
		{
			return false;
		}

		const NetworkTransport::FUeAgentTransportServerIdentity
			AdvertisedIdentity = InServerControl.Identity();
		if (AdvertisedIdentity.OwnerId != Owner)
		{
			SetDiagnostic(
				OutDiagnostic,
				TEXT("transport_server_owner_mismatch"));
			return false;
		}

		FUeAgentProcessIdentity ProcessIdentity;
		if (!ProcessBackend->StartOwned(
			Spec,
			ProcessIdentity,
			OutDiagnostic))
		{
			return false;
		}
		if (!ProcessIdentity.IsValid())
		{
			FUeAgentLifecycleDiagnostic CleanupDiagnostic;
			ProcessBackend->StopOwned(ProcessIdentity, CleanupDiagnostic);
			SetDiagnostic(
				OutDiagnostic,
				TEXT("owned_process_identity_invalid"));
			return false;
		}

		FUeAgentHostIdentity StartedHost{
			Spec.HostId,
			Spec.SessionId,
			EUeAgentHostKind::OwnedWorker,
			ProcessIdentity,
		};
		NetworkTransport::FUeAgentTransportServerIdentity
			StartedServer;
		NetworkTransport::FUeAgentTransportDiagnostic
			TransportDiagnostic;
		if (!InServerControl.Start(StartedServer, TransportDiagnostic))
		{
			FUeAgentLifecycleDiagnostic CleanupDiagnostic;
			ProcessBackend->StopOwned(ProcessIdentity, CleanupDiagnostic);
			SetDiagnostic(
				OutDiagnostic,
				TransportDiagnostic.Code.IsEmpty()
					? TEXT("transport_server_start_failed")
					: *TransportDiagnostic.Code,
				TransportDiagnostic.Message);
			OutDiagnostic.Append(CleanupDiagnostic);
			return false;
		}
		if (StartedServer.OwnerId != Owner
			|| StartedServer.InstanceId != AdvertisedIdentity.InstanceId)
		{
			bool bServerStopped = StartedServer.OwnerId != Owner;
			if (StartedServer.OwnerId == Owner)
			{
				NetworkTransport::FUeAgentTransportDiagnostic
					StopDiagnostic;
				bServerStopped = InServerControl.Stop(
					Owner,
					StopDiagnostic);
				if (!bServerStopped)
				{
					ServerControl = &InServerControl;
					Host = StartedHost;
					Server = StartedServer;
					RenderMode = Spec.RenderMode;
					SetDiagnostic(
						OutDiagnostic,
						TEXT("transport_server_identity_changed_cleanup_failed"),
						StopDiagnostic.Message);
					return false;
				}
			}
			FUeAgentLifecycleDiagnostic CleanupDiagnostic;
			ProcessBackend->StopOwned(ProcessIdentity, CleanupDiagnostic);
			SetDiagnostic(
				OutDiagnostic,
				TEXT("transport_server_identity_changed"));
			return false;
		}

		ServerControl = &InServerControl;
		Host = StartedHost;
		Server = StartedServer;
		RenderMode = Spec.RenderMode;
		OutIdentity = StartedHost;
		return true;
	}

	bool FUeAgentLifecycleHost::RegisterTicker(
		const FString& TickerId,
		FUeAgentTickerIdentity& OutIdentity,
		FUeAgentLifecycleDiagnostic& OutDiagnostic)
	{
		OutIdentity = {};
		OutDiagnostic = {};
		if (!Host.IsSet())
		{
			SetDiagnostic(
				OutDiagnostic,
				TEXT("lifecycle_host_not_started"));
			return false;
		}
		if (!Checkpoint(TEXT("ticker_register"), OutDiagnostic))
		{
			return false;
		}
		for (const FUeAgentTickerIdentity& Existing : Tickers)
		{
			if (Existing.TickerId == TickerId)
			{
				SetDiagnostic(
					OutDiagnostic,
					TEXT("ticker_already_registered"));
				return false;
			}
		}

		FUeAgentTickerIdentity Identity{
			Owner,
			TickerId,
			Host->HostId,
		};
		if (!Identity.IsValid())
		{
			SetDiagnostic(
				OutDiagnostic,
				TEXT("ticker_identity_invalid"));
			return false;
		}
		if (!TickerBackend->Register(Identity, OutDiagnostic))
		{
			return false;
		}
		Tickers.Add(Identity);
		OutIdentity = Identity;
		return true;
	}

	FUeAgentLifecycleHealth FUeAgentLifecycleHost::Health() const
	{
		FUeAgentLifecycleHealth Result;
		Result.OwnerId = Owner;
		Result.Host = Host;
		Result.Server = Server;
		Result.TickerCount = Tickers.Num();
		if (ServerControl)
		{
			Result.ServerHealth = ServerControl->Health();
		}
		if (Host.IsSet())
		{
			Result.bProcessAlive =
				ProcessBackend->IsAlive(Host->Process);
		}
		return Result;
	}

	bool FUeAgentLifecycleHost::Cleanup(FUeAgentCleanupReport& OutReport)
	{
		OutReport = {};
		if (bCleaned)
		{
			OutReport.bComplete = true;
			return true;
		}

		for (int32 Index = Tickers.Num() - 1; Index >= 0; --Index)
		{
			FUeAgentLifecycleDiagnostic Diagnostic;
			if (TickerBackend->Unregister(Tickers[Index], Diagnostic))
			{
				OutReport.CleanedTickerIds.Add(
					Tickers[Index].TickerId);
				Tickers.RemoveAt(Index);
			}
			else
			{
				OutReport.Failures.Add(FString::Printf(
					TEXT("ticker_cleanup_failed:%s:%s"),
					*Tickers[Index].TickerId,
					*Diagnostic.Code));
			}
		}

		if (Tickers.IsEmpty() && ServerControl && Server.IsSet())
		{
			const NetworkTransport::FUeAgentTransportServerIdentity
				CurrentIdentity = ServerControl->Identity();
			if (CurrentIdentity.OwnerId != Server->OwnerId
				|| CurrentIdentity.InstanceId != Server->InstanceId)
			{
				OutReport.Failures.Add(
					TEXT("server_cleanup_failed:transport_server_identity_changed"));
			}
			else
			{
				NetworkTransport::FUeAgentTransportDiagnostic Diagnostic;
				if (ServerControl->Stop(Owner, Diagnostic))
				{
					OutReport.CleanedServerInstanceId =
						Server->InstanceId;
					Server.Reset();
					ServerControl = nullptr;
				}
				else
				{
					OutReport.Failures.Add(FString::Printf(
						TEXT("server_cleanup_failed:%s"),
						*Diagnostic.Code));
				}
			}
		}

		if (Tickers.IsEmpty() && !Server.IsSet() && Host.IsSet())
		{
			FUeAgentLifecycleDiagnostic Diagnostic;
			if (!ProcessBackend->IsAlive(Host->Process)
				|| ProcessBackend->StopOwned(
					Host->Process,
					Diagnostic))
			{
				OutReport.CleanedProcess = Host->Process;
				Host.Reset();
				RenderMode.Reset();
			}
			else
			{
				OutReport.Failures.Add(FString::Printf(
					TEXT("process_cleanup_failed:%s"),
					*Diagnostic.Code));
			}
		}

		OutReport.bComplete = OutReport.Failures.IsEmpty()
			&& Tickers.IsEmpty()
			&& !Server.IsSet()
			&& !Host.IsSet();
		bCleaned = OutReport.bComplete;
		return OutReport.bComplete;
	}

	const FString& FUeAgentLifecycleHost::OwnerId() const
	{
		return Owner;
	}

	const FUeAgentHostIdentity*
	FUeAgentLifecycleHost::HostIdentity() const
	{
		return Host.IsSet() ? &Host.GetValue() : nullptr;
	}

	const NetworkTransport::FUeAgentTransportServerIdentity*
	FUeAgentLifecycleHost::ServerIdentity() const
	{
		return Server.IsSet() ? &Server.GetValue() : nullptr;
	}
}

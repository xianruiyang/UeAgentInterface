#pragma once

#include "Core/ContextResource/UeAgentContextResource.h"
#include "Core/Contract/UeAgentDiagnostic.h"
#include "NetworkTransport/UeAgentNetworkTransport.h"

#include "CoreMinimal.h"
#include "Misc/Optional.h"
#include "Templates/SharedPointer.h"

namespace UE::UAI::LifecycleHost
{
	enum class EUeAgentHostKind : uint8
	{
		MainEditor,
		OwnedWorker,
	};

	enum class EUeAgentRenderMode : uint8
	{
		NullRhi,
		Rhi,
		Interactive,
	};

	struct UEAGENTINTERFACE_API FUeAgentProcessIdentity
	{
		uint32 ProcessId = 0;
		FString StartTimeUtc;
		FString Executable;

		bool Matches(const FUeAgentProcessIdentity& Other) const;
		bool IsValid() const;
	};

	struct UEAGENTINTERFACE_API FUeAgentHostIdentity
	{
		FString HostId;
		FString SessionId;
		EUeAgentHostKind Kind = EUeAgentHostKind::MainEditor;
		FUeAgentProcessIdentity Process;

		bool IsValid() const;
	};

	struct UEAGENTINTERFACE_API FUeAgentOwnedWorkerSpec
	{
		FString HostId;
		FString SessionId;
		FString Executable;
		FString ProjectPath;
		TArray<FString> Arguments;
		EUeAgentRenderMode RenderMode = EUeAgentRenderMode::NullRhi;

		bool IsValid() const;
	};

	struct UEAGENTINTERFACE_API FUeAgentTickerIdentity
	{
		FString OwnerId;
		FString TickerId;
		FString HostId;

		bool IsValid() const;
	};

	using FUeAgentLifecycleDiagnostic = Contract::FDiagnostic;

	class UEAGENTINTERFACE_API IUeAgentProcessLifecycleBackend
	{
	public:
		virtual ~IUeAgentProcessLifecycleBackend() = default;

		virtual bool StartOwned(
			const FUeAgentOwnedWorkerSpec& Spec,
			FUeAgentProcessIdentity& OutIdentity,
			FUeAgentLifecycleDiagnostic& OutDiagnostic) = 0;
		virtual bool StopOwned(
			const FUeAgentProcessIdentity& Identity,
			FUeAgentLifecycleDiagnostic& OutDiagnostic) = 0;
		virtual bool IsAlive(const FUeAgentProcessIdentity& Identity) const = 0;
	};

	class UEAGENTINTERFACE_API IUeAgentTickerLifecycleBackend
	{
	public:
		virtual ~IUeAgentTickerLifecycleBackend() = default;

		virtual bool Register(
			const FUeAgentTickerIdentity& Identity,
			FUeAgentLifecycleDiagnostic& OutDiagnostic) = 0;
		virtual bool Unregister(
			const FUeAgentTickerIdentity& Identity,
			FUeAgentLifecycleDiagnostic& OutDiagnostic) = 0;
	};

	struct FUeAgentLifecycleHealth
	{
		FString OwnerId;
		TOptional<FUeAgentHostIdentity> Host;
		TOptional<NetworkTransport::FUeAgentTransportServerIdentity> Server;
		TOptional<NetworkTransport::FUeAgentTransportServerHealth> ServerHealth;
		int32 TickerCount = 0;
		bool bProcessAlive = false;
	};

	struct FUeAgentCleanupReport
	{
		bool bComplete = false;
		TArray<FString> Failures;
		TArray<FString> CleanedTickerIds;
		TOptional<FString> CleanedServerInstanceId;
		TOptional<FUeAgentProcessIdentity> CleanedProcess;
	};

	class UEAGENTINTERFACE_API FUeAgentLifecycleHost final
	{
	public:
		FUeAgentLifecycleHost(
			FString InOwnerId,
			const ContextResource::FUeAgentOperationDeadline& InDeadline,
			const ContextResource::FUeAgentCancellationToken& InCancellation,
			TSharedRef<IUeAgentProcessLifecycleBackend> InProcessBackend,
			TSharedRef<IUeAgentTickerLifecycleBackend> InTickerBackend);

		bool StartOwnedWorker(
			const FUeAgentOwnedWorkerSpec& Spec,
			NetworkTransport::IUeAgentNetworkServerControl& ServerControl,
			FUeAgentHostIdentity& OutIdentity,
			FUeAgentLifecycleDiagnostic& OutDiagnostic);
		bool RegisterTicker(
			const FString& TickerId,
			FUeAgentTickerIdentity& OutIdentity,
			FUeAgentLifecycleDiagnostic& OutDiagnostic);
		FUeAgentLifecycleHealth Health() const;
		bool Cleanup(FUeAgentCleanupReport& OutReport);

		const FString& OwnerId() const;
		const FUeAgentHostIdentity* HostIdentity() const;
		const NetworkTransport::FUeAgentTransportServerIdentity*
			ServerIdentity() const;

	private:
		bool Checkpoint(
			const FString& Stage,
			FUeAgentLifecycleDiagnostic& OutDiagnostic) const;

		FString Owner;
		const ContextResource::FUeAgentOperationDeadline& Deadline;
		ContextResource::FUeAgentCancellationToken Cancellation;
		TSharedRef<IUeAgentProcessLifecycleBackend> ProcessBackend;
		TSharedRef<IUeAgentTickerLifecycleBackend> TickerBackend;
		NetworkTransport::IUeAgentNetworkServerControl* ServerControl = nullptr;
		TOptional<FUeAgentHostIdentity> Host;
		TOptional<NetworkTransport::FUeAgentTransportServerIdentity> Server;
		TOptional<EUeAgentRenderMode> RenderMode;
		TArray<FUeAgentTickerIdentity> Tickers;
		bool bCleaned = false;
	};
}

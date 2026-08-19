#pragma once

#include "CoreMinimal.h"

#include "CommandEdge/UeAgentCommandProtocol.h"
#include "Composition/UeAgentCommandServiceComposition.h"
#include "Composition/LongRunningSession/UeAgentLongRunningSessionActivation.h"
#include "Composition/GraphCompound/UeAgentGraphCompoundActivation.h"
#include "Composition/UeAgentQueryServiceComposition.h"
#include "Core/Contract/UeAgentCommandContract.h"
#include "GameFramework/WorldSettings.h"
#include "Misc/Guid.h"
#include "NetworkTransport/UeAgentHttpTransportAdapter.h"

namespace UE::UAI::Tests::PublicCommand
{
	using namespace UE::UAI::CommandContract;
	using namespace UE::UAI::CommandEdge;
	using namespace UE::UAI::Composition;
	using namespace UE::UAI::NetworkTransport;

	class FFakeRouteHost final : public IUeAgentHttpRouteHost
	{
	public:
		bool Bind(
			const FString& InInstanceId,
			const FString& InMethod,
			const FString& InRoutePath,
			FUeAgentHttpRequestHandler InHandler,
			FUeAgentTransportDiagnostic& OutDiagnostic) override
		{
			OutDiagnostic = {};
			if (Handlers.Contains(InRoutePath)
				|| RouteByInstance.Contains(InInstanceId))
			{
				OutDiagnostic = {
					TEXT("fake_route_already_bound"),
					TEXT("fake route or instance is already owned"),
				};
				return false;
			}
			RouteByInstance.Add(InInstanceId, InRoutePath);
			Handlers.Add(InRoutePath, MoveTemp(InHandler));
			return true;
		}

		bool Unbind(
			const FString& InInstanceId,
			FUeAgentTransportDiagnostic& OutDiagnostic) override
		{
			OutDiagnostic = {};
			const FString* Route = RouteByInstance.Find(InInstanceId);
			if (!Route)
			{
				OutDiagnostic = {
					TEXT("fake_route_instance_mismatch"),
					TEXT("fake route host instance mismatch"),
				};
				return false;
			}
			Handlers.Remove(*Route);
			RouteByInstance.Remove(InInstanceId);
			return true;
		}

		bool IsBound(const FString& InInstanceId) const override
		{
			return RouteByInstance.Contains(InInstanceId);
		}

		FUeAgentHttpResponse Invoke(
			const FUeAgentHttpRequest& Request) const
		{
			const FUeAgentHttpRequestHandler* Handler =
				Handlers.Find(Request.RoutePath);
			return Handler
				? (*Handler)(Request)
				: FUeAgentHttpResponse{404};
		}

		TMap<FString, FString> RouteByInstance;
		TMap<FString, FUeAgentHttpRequestHandler> Handlers;
	};

	inline FUeAgentQueryServiceConfig MakeConfig(
		const FString& Scope = TEXT("read-export-e3"))
	{
		FUeAgentQueryServiceConfig Result;
		Result.OwnerId = Scope + TEXT("-owner");
		Result.InstanceId = Scope + TEXT("-instance");
		Result.ServerSessionId = Scope + TEXT("-session");
		Result.RegistryEpoch = 1;
		Result.AuthToken = Scope + TEXT("-auth-token");
		Result.TicketIntegrityKey = Scope + TEXT("-ticket-integrity-key");
		Result.QueryTimeoutSeconds = 15.0;
		Result.TicketTtlSeconds = 30.0;
		Result.MaxFrameBytes = 16 * 1024 * 1024;
		return Result;
	}

	inline TArray<uint8> Utf8Bytes(const FString& Text)
	{
		const FTCHARToUTF8 Converted(*Text);
		TArray<uint8> Result;
		Result.Append(
			reinterpret_cast<const uint8*>(Converted.Get()),
			Converted.Length());
		return Result;
	}

	class FScopedPublicCommandComposition final
	{
	public:
		explicit FScopedPublicCommandComposition(const FString& Scope)
			: RouteHost(MakeShared<FFakeRouteHost, ESPMode::ThreadSafe>())
			, Config(MakeConfig(Scope))
		{
		}

		~FScopedPublicCommandComposition()
		{
			Stop();
		}

		bool TryStart(AWorldSettings& WorldSettings, FString& OutError)
		{
			OutError.Reset();
			FUeAgentQueryServiceError ServiceError;
			if (!FUeAgentQueryServiceComposition::TryCreateReference(
					Config,
					WorldSettings,
					RouteHost.ToSharedRef(),
					Composition,
					ServiceError)
				|| !Composition)
			{
				OutError = ServiceError.Code + TEXT(":") + ServiceError.Detail;
				return false;
			}
			FUeAgentTransportDiagnostic TransportError;
			if (!Composition->Start(TransportError))
			{
				OutError = TransportError.Code + TEXT(":")
					+ TransportError.Message;
				Composition.Reset();
				return false;
			}
			bStarted = true;
			return true;
		}

		bool Stop()
		{
			if (!bStarted || !Composition)
			{
				Composition.Reset();
				bStarted = false;
				return true;
			}
			FUeAgentTransportDiagnostic TransportError;
			const bool bStopped = Composition->Stop(TransportError);
			Composition.Reset();
			bStarted = false;
			return bStopped;
		}

		const FFakeRouteHost& GetRouteHost() const
		{
			return *RouteHost;
		}

		const FUeAgentQueryServiceConfig& GetConfig() const
		{
			return Config;
		}

	private:
		TSharedPtr<FFakeRouteHost, ESPMode::ThreadSafe> RouteHost;
		FUeAgentQueryServiceConfig Config;
		TUniquePtr<FUeAgentQueryServiceComposition> Composition;
		bool bStarted = false;
	};

	inline bool TryInvokeCommand(
		const FFakeRouteHost& RouteHost,
		const FUeAgentQueryServiceConfig& ServiceConfig,
		const FString& CommandId,
		const FString& ParamsJson,
		TOptional<FUeAgentCommandResult>& OutResult,
		FString& OutError)
	{
		OutResult.Reset();
		OutError.Reset();
		TSharedPtr<const FUeAgentCommandContractRegistry> Registry;
		if (!GraphCompound::
				TryCreateGraphCompoundProductionCommandContractRegistry(
					Registry,
					OutError)
			|| !Registry.IsValid())
		{
			return false;
		}
		const FUeAgentCommandContract* Contract = Registry->FindExact(CommandId);
		TOptional<FUeAgentCommandContract> ExpectedContract;
		if (!Contract && CommandId == TEXT("asset.delete"))
		{
			TArray<FUeAgentCommandParameter> Parameters;
			Parameters.Emplace(
				TEXT("target_ref"),
				EUeAgentCommandWireType::String,
				true,
				false);
			ExpectedContract.Emplace(
				TEXT("asset.delete"),
				TEXT("asset"),
				TEXT("delete"),
				MoveTemp(Parameters),
				1,
				1,
				EUeAgentCommandStatus::Active);
			Contract = &ExpectedContract.GetValue();
		}
		if (!Contract)
		{
			OutError = TEXT("command_contract_missing");
			return false;
		}
		const FString RequestId = TEXT("composition-e3-")
			+ CommandId.Replace(TEXT("."), TEXT("-")) + TEXT("-")
			+ FGuid::NewGuid().ToString(EGuidFormats::Digits);
		const FString CorrelationId = RequestId + TEXT("-correlation");
		const FString Body =
			TEXT("{\"command_binding_hash\":\"")
			+ Contract->GetBindingHash()
			+ TEXT("\",\"command_id\":\"")
			+ CommandId
			+ TEXT("\",\"correlation_id\":\"")
			+ CorrelationId
			+ TEXT("\",\"message_kind\":\"command_request\",")
			  TEXT("\"params\":")
			+ ParamsJson
			+ TEXT(",\"request_id\":\"")
			+ RequestId
			+ TEXT("\",\"schema\":\"uai.command.v1\",")
			  TEXT("\"version\":1}");
		FUeAgentHttpRequest Request;
		Request.Method = TEXT("POST");
		Request.RoutePath = GetUaiCommandRoutePath();
		Request.Headers.Add(
			TEXT("X-UeAgentInterface-Token"),
			ServiceConfig.AuthToken);
		Request.Headers.Add(
			TEXT("X-Transport-Correlation"),
			CorrelationId);
		Request.Body = Utf8Bytes(Body);
		Request.PeerId = TEXT("public-command-test-peer");
		const FUeAgentHttpResponse Response = RouteHost.Invoke(Request);
		if (Response.StatusCode != 200)
		{
			OutError = FString::Printf(
				TEXT("command_http_status:%d"),
				Response.StatusCode);
			return false;
		}
		FUeAgentCommandProtocolDiagnostic Diagnostic;
		FUeAgentCommandProtocolCodec Codec;
		if (!Codec.DecodeResult(
			Response.Body,
			OutResult,
			Diagnostic))
		{
			OutError = Diagnostic.Code + TEXT(":") + Diagnostic.Message;
			return false;
		}
		return OutResult.IsSet();
	}
}

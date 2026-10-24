#pragma once

#include "CoreMinimal.h"

#include "CommandEdge/UeAgentCommandProtocol.h"
#include "Composition/LongRunningSession/UeAgentLongRunningSessionActivation.h"
#include "Composition/GraphCompound/UeAgentGraphCompoundActivation.h"
#include "Core/Contract/UeAgentCommandContract.h"
#include "Core/Contract/UeAgentContractJson.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Core/DomainCapability/UeAgentDomainCapability.h"
#include "GameFramework/WorldSettings.h"
#include "Misc/Guid.h"
#include "Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h"
#include "NetworkTransport/UeAgentHttpTransportAdapter.h"
#include "ServiceComposition/CommandService/UeAgentCommandService.h"
#include "ServiceComposition/Query/UeAgentQueryCommandIngress.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace UE::UAI::Tests::PublicCommand
{
	using namespace UE::UAI::CommandContract;
	using namespace UE::UAI::CommandEdge;
	using namespace UE::UAI::CommandRegistry;
	using namespace UE::UAI::Composition;
	using namespace UE::UAI::NetworkTransport;

	// Wire-facing tests inspect the response document, without reconstructing
	// internal command results or diagnostics from their public projection.
	inline bool ReadPublicResponse(const TArray<uint8>& Bytes,
		TSharedPtr<FJsonObject>& OutResponse, FString& OutError)
	{
		Contract::FDiagnostic ParseError;
		if (!Contract::Json::TryParseObject(Bytes, OutResponse, ParseError))
		{
			OutError = ParseError.PublicMessage;
			return false;
		}
		return OutResponse.IsValid();
	}

	inline FString PublicErrorText(const TSharedPtr<FJsonObject>& Response)
	{
		const auto Error = Response->TryGetField(TEXT("error"));
		if (!Error || Error->IsNull()) return TEXT("null");
		FString Text;
		FJsonSerializer::Serialize(Error->AsObject().ToSharedRef(), TJsonWriterFactory<>::Create(&Text));
		return Text;
	}

	class FActiveCommandNeverCancelled final
		: public UE::UAI::Contract::IUeAgentCancellationSignal
	{
	public:
		bool IsCancelled() const override { return false; }
	};

	inline FUeAgentCommandResult SubmitActiveCommandWithScopes(
		const TSharedPtr<const FUeAgentImmutableCommandRegistry>& Registry,
		const TSharedPtr<const FUeAgentExactCommandDispatch>& Dispatch,
		const FString& CommandId,
		const TSharedRef<FJsonObject>& Params,
		const FString& TestScope,
		const TArray<FString>& AuthorityScopes)
	{
		const auto* Contract = Registry.IsValid()
			? Registry->FindExact(CommandId)
			: nullptr;
		const FString CorrelationId = TestScope + TEXT("-correlation");
		const FString RequestId = TestScope + TEXT("-")
			+ FGuid::NewGuid().ToString(EGuidFormats::Digits);
		if (!Contract || !Dispatch.IsValid())
		{
			return FUeAgentCommandResult::ProtocolError(
				CorrelationId,
				RequestId,
				CommandId,
				TEXT("sha256:0000000000000000000000000000000000000000000000000000000000000000"),
				Contract::FDiagnostic::Failure(TEXT("active_public_contract_missing"), CommandId, TEXT("command_test")));
		}
		FString ParamsJson;
		Contract::FDiagnostic CanonicalError;
		if (!UE::UAI::Contract::Json::TryCanonicalize(
				Params, ParamsJson, CanonicalError))
		{
			return FUeAgentCommandResult::ProtocolError(
				CorrelationId,
				RequestId,
				CommandId,
				Contract->GetContractHash(),
				Contract::FDiagnostic::WrapFailure(TEXT("active_public_params_invalid"), CanonicalError, TEXT("command_test")));
		}
		const FString Envelope = FString::Printf(
			TEXT("{\"schema\":\"uai.command.v1\",\"version\":1,")
			TEXT("\"message_kind\":\"command_request\",")
			TEXT("\"correlation_id\":\"%s\",\"request_id\":\"%s\",")
			TEXT("\"command_id\":\"%s\",\"command_binding_hash\":\"%s\",")
			TEXT("\"params\":%s}"),
			*CorrelationId,
			*RequestId,
			*CommandId,
			*Contract->GetContractHash(),
			*ParamsJson);
		const FTCHARToUTF8 Encoded(*Envelope);
		TArray<uint8> Bytes;
		Bytes.Append(
			reinterpret_cast<const uint8*>(Encoded.Get()),
			Encoded.Length());
		TOptional<FUeAgentCommandRequest> Request;
		FUeAgentCommandProtocolDiagnostic Diagnostic;
		if (!FUeAgentCommandProtocolCodec().DecodeRequest(
				Bytes, Request, Diagnostic)
			|| !Request.IsSet())
		{
			return FUeAgentCommandResult::ProtocolError(
				CorrelationId,
				RequestId,
				CommandId,
				Contract->GetContractHash(),
				Diagnostic);
		}
		return Dispatch->Submit(
			Request.GetValue(),
			UE::UAI::Contract::FUeAgentCallerContext(
				TestScope,
				AuthorityScopes,
				CorrelationId),
			MakeShared<FActiveCommandNeverCancelled>());
	}

	inline FUeAgentCommandResult SubmitActiveCommand(
		const TSharedPtr<const FUeAgentImmutableCommandRegistry>& Registry,
		const TSharedPtr<const FUeAgentExactCommandDispatch>& Dispatch,
		const FString& CommandId,
		const TSharedRef<FJsonObject>& Params,
		const FString& TestScope)
	{
		return SubmitActiveCommandWithScopes(
			Registry,
			Dispatch,
			CommandId,
			Params,
			TestScope,
			{TEXT("uai:query"), TEXT("uai:operate")});
	}

	class FFakeRouteHost final : public IUeAgentHttpRouteHost
	{
	public:
		bool Bind(
			const FString& InInstanceId,
			const FString& InMethod,
			const FString& InRoutePath,
			FUeAgentHttpRequestHandler InHandler,
			FUeAgentTransportDiagnostic& OutDiagnostic,
			EUeAgentHttpRouteExecution Execution = EUeAgentHttpRouteExecution::GameThread) override
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

	struct FPublicCommandTransportConfig
	{
		FString OwnerId;
		FString InstanceId;
		FString ServerSessionId;
		int64 RegistryEpoch = 0;
		FString AuthToken;
		FString TicketIntegrityKey;
		double QueryTimeoutSeconds = 0.0;
		double TicketTtlSeconds = 0.0;
	};

	inline FPublicCommandTransportConfig MakeConfig(
		const FString& Scope = TEXT("read-export-e3"))
	{
		FPublicCommandTransportConfig Result;
		Result.OwnerId = Scope + TEXT("-owner");
		Result.InstanceId = Scope + TEXT("-instance");
		Result.ServerSessionId = Scope + TEXT("-session");
		Result.RegistryEpoch = 1;
		Result.AuthToken = Scope + TEXT("-auth-token");
		Result.TicketIntegrityKey = Scope + TEXT("-ticket-integrity-key");
		Result.QueryTimeoutSeconds = 15.0;
		Result.TicketTtlSeconds = 30.0;
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

	class FPublicCommandTestContextInvalidator final
		: public ServiceComposition::CommandService::
			IUeAgentRequestContextInvalidator
	{
	public:
		void InvalidateWorld(const FString& WorldIdentity) override
		{
			LastWorldIdentity = WorldIdentity;
		}

		FString LastWorldIdentity;
	};

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
			(void)WorldSettings;
			OutError.Reset();
			DomainCapability::FDomainCapabilityAuthorities Authorities;
			if (!DomainCapability::
					TryGetOrBuildProcessDomainCapabilityAuthorities(
						Authorities,
						OutError))
			{
				return false;
			}
			Registry = Authorities.GetCommandRegistry();
			Dispatch = Authorities.GetCommandDispatch();
			if (!Registry.IsValid() || !Dispatch.IsValid())
			{
				OutError = TEXT("active_public_command_authority_missing");
				return false;
			}
			TSharedPtr<
				IUeAgentTransportRequestHandler,
				ESPMode::ThreadSafe> Ingress;
			if (!ServiceComposition::Query::TryCreateQueryCommandIngress(
					Dispatch.ToSharedRef(),
					TEXT("uai-public-command-test-client"),
					{TEXT("uai:query"), TEXT("uai:export"),
						TEXT("uai:operate")},
					Ingress,
					OutError)
				|| !Ingress.IsValid())
			{
				Registry.Reset();
				Dispatch.Reset();
				return false;
			}
			ContextInvalidator = MakeShared<
				FPublicCommandTestContextInvalidator,
				ESPMode::ThreadSafe>();
			if (!ServiceComposition::CommandService::FUeAgentCommandService::
					TryCreate(
						Ingress.ToSharedRef(),
						ContextInvalidator.ToSharedRef(),
						CommandService,
						OutError)
				|| !CommandService)
			{
				Registry.Reset();
				Dispatch.Reset();
				ContextInvalidator.Reset();
				return false;
			}

			FUeAgentHttpAdapterConfig HttpConfig;
			HttpConfig.OwnerId = Config.OwnerId;
			HttpConfig.InstanceId = Config.InstanceId;
			HttpConfig.Method = TEXT("POST");
			HttpConfig.RoutePath = GetUaiHttpCommandRoutePath();
			HttpConfig.AuthHeader = TEXT("X-UeAgentInterface-Token");
			HttpConfig.AuthToken = Config.AuthToken;
			HttpConfig.Channel = TEXT("uai.command.v1");
			HttpConfig.PrincipalSubject =
				TEXT("uai-public-command-test-client");
			ServerControl = MakeUnique<FUeAgentHttpTransportAdapter>(
				MoveTemp(HttpConfig),
				RouteHost.ToSharedRef(),
				CommandService->GetIngressHandler());
			FUeAgentTransportServerIdentity Identity;
			FUeAgentTransportDiagnostic TransportError;
			if (!ServerControl->Start(Identity, TransportError))
			{
				OutError = TransportError.Code + TEXT(":")
					+ TransportError.Message;
				ServerControl.Reset();
				CommandService.Reset();
				ContextInvalidator.Reset();
				Registry.Reset();
				Dispatch.Reset();
				return false;
			}
			bStarted = true;
			return true;
		}

		bool Stop()
		{
			if (!bStarted || !ServerControl)
			{
				ServerControl.Reset();
				CommandService.Reset();
				ContextInvalidator.Reset();
				Registry.Reset();
				Dispatch.Reset();
				bStarted = false;
				return true;
			}
			FUeAgentTransportDiagnostic TransportError;
			const bool bStopped = ServerControl->Stop(
				Config.OwnerId, TransportError);
			ServerControl.Reset();
			CommandService.Reset();
			ContextInvalidator.Reset();
			Registry.Reset();
			Dispatch.Reset();
			bStarted = false;
			return bStopped;
		}

		const FFakeRouteHost& GetRouteHost() const
		{
			return *RouteHost;
		}

		const FPublicCommandTransportConfig& GetConfig() const
		{
			return Config;
		}

		const TSharedPtr<const FUeAgentImmutableCommandRegistry>&
		GetCommandRegistry() const
		{
			return Registry;
		}

	private:
		TSharedPtr<FFakeRouteHost, ESPMode::ThreadSafe> RouteHost;
		FPublicCommandTransportConfig Config;
		TSharedPtr<const FUeAgentImmutableCommandRegistry> Registry;
		TSharedPtr<const FUeAgentExactCommandDispatch> Dispatch;
		TSharedPtr<
			FPublicCommandTestContextInvalidator,
			ESPMode::ThreadSafe> ContextInvalidator;
		TUniquePtr<
			ServiceComposition::CommandService::
				FUeAgentCommandService> CommandService;
		TUniquePtr<IUeAgentNetworkServerControl> ServerControl;
		bool bStarted = false;
	};

	inline bool TryInvokeCommand(
		const FFakeRouteHost& RouteHost,
		const FPublicCommandTransportConfig& ServiceConfig,
		const FString& CommandId,
		const FString& ParamsJson,
		TSharedPtr<FJsonObject>& OutResult,
		FString& OutError)
	{
		OutResult.Reset();
		OutError.Reset();
		DomainCapability::FDomainCapabilityAuthorities Authorities;
		if (!DomainCapability::
				TryGetOrBuildProcessDomainCapabilityAuthorities(
					Authorities,
					OutError))
		{
			return false;
		}
		const TSharedPtr<const FUeAgentImmutableCommandRegistry> Registry =
			Authorities.GetCommandRegistry();
		const FUeAgentFrozenCommandContract* Contract = Registry.IsValid()
			? Registry->FindExact(CommandId) : nullptr;
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
			+ Contract->GetContractHash()
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
		return ReadPublicResponse(Response.Body, OutResult, OutError);
	}
}

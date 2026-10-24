#include "NetworkTransport/UeAgentHttpTransportAdapter.h"

#include "NetworkTransport/UeAgentHttpFraming.h"
#include "Misc/Guid.h"

namespace UE::UAI::NetworkTransport
{
	namespace
	{
		const FString UaiHttpCommandRoutePath(
			TEXT("/api/uai/command"));
		const FString UaiHttpProbeRoutePath(TEXT("/api/ping"));

	}


	const FString& GetUaiHttpCommandRoutePath()
	{
		return UaiHttpCommandRoutePath;
	}

	const FString& GetUaiHttpProbeRoutePath()
	{
		return UaiHttpProbeRoutePath;
	}

	FUeAgentHttpTransportAdapter::FUeAgentHttpTransportAdapter(
		FUeAgentHttpAdapterConfig InConfig,
		TSharedRef<IUeAgentHttpRouteHost, ESPMode::ThreadSafe> InRouteHost,
		TSharedRef<IUeAgentTransportRequestHandler, ESPMode::ThreadSafe> InHandler)
		: Config(MoveTemp(InConfig))
		, RouteHost(MoveTemp(InRouteHost))
		, Handler(MoveTemp(InHandler))
	{
	}

	FUeAgentHttpTransportAdapter::~FUeAgentHttpTransportAdapter()
	{
		if (bRunning)
		{
			FUeAgentTransportDiagnostic Ignored;
			Stop(Config.OwnerId, Ignored);
		}
	}

	bool FUeAgentHttpTransportAdapter::Start(
		FUeAgentTransportServerIdentity& OutIdentity,
		FUeAgentTransportDiagnostic& OutDiagnostic)
	{
		OutDiagnostic = {};
		if (bRunning)
		{
			if (Health().bRunning)
			{
				OutIdentity = Identity();
				return true;
			}
			OutDiagnostic = {
				TEXT("transport_partial_route_state"),
				TEXT("owned transport has an incomplete route set"),
			};
			return false;
		}
		if (!ValidateConfig(OutDiagnostic))
		{
			return false;
		}
		if (!RouteHost->Bind(
			Config.InstanceId,
			Config.Method,
			Config.RoutePath,
			[this](const FUeAgentHttpRequest& Request)
			{
				return HandleRequest(Request);
			},
			OutDiagnostic, EUeAgentHttpRouteExecution::GameThread))
		{
			return false;
		}
		bCommandRouteBound = true;
		if (!Config.ProbeRoutePath.IsEmpty())
		{
			FUeAgentTransportDiagnostic ProbeDiagnostic;
			if (!RouteHost->Bind(
					ProbeBindingId(),
					TEXT("GET"),
					Config.ProbeRoutePath,
					[this](const FUeAgentHttpRequest& Request)
					{
						return HandleProbeRequest(Request);
					},
					ProbeDiagnostic, EUeAgentHttpRouteExecution::NetworkThread))
			{
				FUeAgentTransportDiagnostic RollbackDiagnostic;
				if (RouteHost->Unbind(Config.InstanceId, RollbackDiagnostic))
				{
					bCommandRouteBound = false;
					OutDiagnostic = MoveTemp(ProbeDiagnostic);
				}
				else
				{
					bRunning = true;
					OutDiagnostic = {
						TEXT("http_probe_route_bind_rollback_failed"),
						ProbeDiagnostic.Code + TEXT(":")
							+ RollbackDiagnostic.Code,
					};
				}
				return false;
			}
			bProbeRouteBound = true;
		}

		bRunning = true;
		OutIdentity = Identity();
		return true;
	}

	bool FUeAgentHttpTransportAdapter::Stop(
		const FString& OwnerId,
		FUeAgentTransportDiagnostic& OutDiagnostic)
	{
		OutDiagnostic = {};
		if (OwnerId != Config.OwnerId)
		{
			OutDiagnostic = {
				TEXT("transport_server_owner_mismatch"),
				TEXT("only the owning identity may stop this instance"),
			};
			return false;
		}
		if (!bRunning)
		{
			return true;
		}
		FUeAgentTransportDiagnostic FirstFailure;
		if (bProbeRouteBound)
		{
			FUeAgentTransportDiagnostic Diagnostic;
			if (RouteHost->Unbind(ProbeBindingId(), Diagnostic))
			{
				bProbeRouteBound = false;
			}
			else
			{
				FirstFailure = MoveTemp(Diagnostic);
			}
		}
		if (bCommandRouteBound)
		{
			FUeAgentTransportDiagnostic Diagnostic;
			if (RouteHost->Unbind(Config.InstanceId, Diagnostic))
			{
				bCommandRouteBound = false;
			}
			else if (FirstFailure.Code.IsEmpty())
			{
				FirstFailure = MoveTemp(Diagnostic);
			}
		}
		bRunning = bCommandRouteBound || bProbeRouteBound;
		if (!FirstFailure.Code.IsEmpty())
		{
			OutDiagnostic = MoveTemp(FirstFailure);
			return false;
		}
		return !bRunning;
	}

	FUeAgentTransportServerHealth FUeAgentHttpTransportAdapter::Health() const
	{
		FUeAgentTransportServerHealth Result;
		const bool bCommandHealthy = bCommandRouteBound
			&& RouteHost->IsBound(Config.InstanceId);
		const bool bProbeHealthy = Config.ProbeRoutePath.IsEmpty()
			|| (bProbeRouteBound && RouteHost->IsBound(ProbeBindingId()));
		Result.bRunning = bRunning && bCommandHealthy && bProbeHealthy;
		if (bRunning && !Result.bRunning)
		{
			Result.Diagnostic = FUeAgentTransportDiagnostic{
				TEXT("transport_route_not_bound"),
				TEXT("owned route is no longer bound"),
			};
		}
		return Result;
	}

	FUeAgentTransportServerIdentity FUeAgentHttpTransportAdapter::Identity() const
	{
		return {
			Config.OwnerId,
			Config.InstanceId,
		};
	}

	FUeAgentHttpResponse FUeAgentHttpTransportAdapter::HandleRequest(
		const FUeAgentHttpRequest& Request) const
	{
		if (!bRunning || !bCommandRouteBound
			|| !RouteHost->IsBound(Config.InstanceId))
		{
			return FUeAgentHttpResponse::Failure(
				503,
				TEXT("transport_not_running"),
				TEXT("owned transport instance is not running"));
		}
		if (!Request.Method.Equals(Config.Method, ESearchCase::IgnoreCase))
		{
			return FUeAgentHttpResponse::Failure(
				405,
				TEXT("transport_method_mismatch"),
				TEXT("request method does not match the bound route"));
		}
		if (Request.RoutePath != Config.RoutePath)
		{
			return FUeAgentHttpResponse::Failure(
				404,
				TEXT("transport_route_mismatch"),
				TEXT("request route does not match the bound route"));
		}
		if (FindHeader(Request.Headers, Config.AuthHeader) != Config.AuthToken)
		{
			return FUeAgentHttpResponse::Failure(
				401,
				TEXT("transport_authentication_failed"),
				TEXT("transport credential was rejected"));
		}

		const FString CorrelationHeader =
			FindHeader(Request.Headers, TEXT("X-Transport-Correlation"));
		const FString CorrelationId = CorrelationHeader.IsEmpty()
			? FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower)
			: CorrelationHeader;
		const FUeAgentTransportPrincipal Principal{
			Config.PrincipalSubject,
			TEXT("http_header"),
		};
		const FUeAgentTransportPeer Peer{
			Request.PeerId.IsEmpty() ? TEXT("unknown-peer") : Request.PeerId,
			Principal,
		};
		const FUeAgentTransportFrame Frame(
			Config.Channel,
			Request.Body,
			CorrelationId,
			Peer);
		const FUeAgentTransportIngress Ingress(Frame, Peer);
		const FUeAgentTransportReply Reply = Handler->Handle(Ingress);
		if (Reply.Frame.GetCorrelationId() != CorrelationId)
		{
			return FUeAgentHttpResponse::Failure(
				502,
				TEXT("transport_correlation_mismatch"),
				TEXT("reply correlation does not match the request"));
		}

		FUeAgentHttpResponse Response;
		Response.StatusCode = 200;
		Response.Headers.Add(
			TEXT("Content-Type"),
			TEXT("application/octet-stream"));
		Response.Headers.Add(
			TEXT("X-Transport-Correlation"),
			CorrelationId);
		Response.Body = Reply.Frame.GetPayload();
		return Response;
	}

	FString FUeAgentHttpTransportAdapter::ProbeBindingId() const
	{
		return Config.InstanceId + TEXT(":probe");
	}

	FUeAgentHttpResponse FUeAgentHttpTransportAdapter::HandleProbeRequest(
		const FUeAgentHttpRequest& Request) const
	{
		if (!bRunning || !bProbeRouteBound
			|| !RouteHost->IsBound(ProbeBindingId()))
		{
			return FUeAgentHttpResponse::Failure(
				503,
				TEXT("transport_not_running"),
				TEXT("owned transport instance is not running"));
		}
		if (!Request.Method.Equals(TEXT("GET"), ESearchCase::IgnoreCase))
		{
			return FUeAgentHttpResponse::Failure(
				405,
				TEXT("transport_probe_method_mismatch"),
				TEXT("transport probe requires GET"));
		}
		if (Request.RoutePath != Config.ProbeRoutePath)
		{
			return FUeAgentHttpResponse::Failure(
				404,
				TEXT("transport_probe_route_mismatch"),
				TEXT("request route does not match the owned probe route"));
		}

		FUeAgentHttpResponse Response;
		Response.StatusCode = 200;
		Response.Headers.Add(
			TEXT("Content-Type"),
			TEXT("application/octet-stream"));
		return Response;
	}

	bool FUeAgentHttpTransportAdapter::ValidateConfig(
		FUeAgentTransportDiagnostic& OutDiagnostic) const
	{
		if (
			Config.OwnerId.IsEmpty()
			|| Config.InstanceId.IsEmpty()
			|| !IsSupportedHttpMethod(Config.Method)
			|| !Config.RoutePath.StartsWith(TEXT("/"))
			|| (!Config.ProbeRoutePath.IsEmpty()
				&& (!Config.ProbeRoutePath.StartsWith(TEXT("/"))
					|| Config.ProbeRoutePath == Config.RoutePath))
			|| Config.AuthHeader.IsEmpty()
			|| Config.AuthToken.IsEmpty()
			|| Config.Channel.IsEmpty()
			|| Config.PrincipalSubject.IsEmpty())
		{
			OutDiagnostic = {
				TEXT("http_adapter_config_invalid"),
				TEXT("HTTP adapter configuration is incomplete"),
			};
			return false;
		}
		return true;
	}

	FUeAgentHttpResponse FUeAgentHttpResponse::Failure(
		int32 StatusCode,
		const FString& Code,
		const FString& Message)
	{
		FUeAgentHttpResponse Response;
		Response.StatusCode = StatusCode;
		Response.Headers.Add(TEXT("X-Transport-Diagnostic"), Code);
		Response.Headers.Add(TEXT("X-Transport-Diagnostic-Message"), Message.Replace(TEXT("\r"), TEXT(" ")).Replace(TEXT("\n"), TEXT(" ")));
		FTCHARToUTF8 Converted(*Message);
		Response.Body.Append(
			reinterpret_cast<const uint8*>(Converted.Get()),
			Converted.Length());
		return Response;
	}

	FString FUeAgentHttpTransportAdapter::FindHeader(
		const TMap<FString, FString>& Headers,
		const FString& Name) const
	{
		for (const TPair<FString, FString>& Pair : Headers)
		{
			if (Pair.Key.Equals(Name, ESearchCase::IgnoreCase))
			{
				return Pair.Value;
			}
		}
		return FString();
	}

}

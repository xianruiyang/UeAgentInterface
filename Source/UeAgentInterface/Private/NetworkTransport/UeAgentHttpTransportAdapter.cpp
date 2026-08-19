#include "NetworkTransport/UeAgentHttpTransportAdapter.h"

#include "HttpPath.h"
#include "HttpRequestHandler.h"
#include "HttpRouteHandle.h"
#include "HttpServerConstants.h"
#include "HttpServerModule.h"
#include "HttpServerRequest.h"
#include "HttpServerResponse.h"
#include "IHttpRouter.h"
#include "IPAddress.h"
#include "Misc/Guid.h"

namespace UE::UAI::NetworkTransport
{
	namespace
	{
		const FString UaiHttpCommandRoutePath(
			TEXT("/api/uai/command"));

		static EHttpServerRequestVerbs ParseVerb(const FString& Method)
		{
			if (Method.Equals(TEXT("GET"), ESearchCase::IgnoreCase))
			{
				return EHttpServerRequestVerbs::VERB_GET;
			}
			if (Method.Equals(TEXT("POST"), ESearchCase::IgnoreCase))
			{
				return EHttpServerRequestVerbs::VERB_POST;
			}
			if (Method.Equals(TEXT("PUT"), ESearchCase::IgnoreCase))
			{
				return EHttpServerRequestVerbs::VERB_PUT;
			}
			if (Method.Equals(TEXT("PATCH"), ESearchCase::IgnoreCase))
			{
				return EHttpServerRequestVerbs::VERB_PATCH;
			}
			if (Method.Equals(TEXT("DELETE"), ESearchCase::IgnoreCase))
			{
				return EHttpServerRequestVerbs::VERB_DELETE;
			}
			if (Method.Equals(TEXT("OPTIONS"), ESearchCase::IgnoreCase))
			{
				return EHttpServerRequestVerbs::VERB_OPTIONS;
			}
			return EHttpServerRequestVerbs::VERB_NONE;
		}

		static FString LexVerb(EHttpServerRequestVerbs Verb)
		{
			switch (Verb)
			{
			case EHttpServerRequestVerbs::VERB_GET:
				return TEXT("GET");
			case EHttpServerRequestVerbs::VERB_POST:
				return TEXT("POST");
			case EHttpServerRequestVerbs::VERB_PUT:
				return TEXT("PUT");
			case EHttpServerRequestVerbs::VERB_PATCH:
				return TEXT("PATCH");
			case EHttpServerRequestVerbs::VERB_DELETE:
				return TEXT("DELETE");
			case EHttpServerRequestVerbs::VERB_OPTIONS:
				return TEXT("OPTIONS");
			default:
				return TEXT("NONE");
			}
		}

		static EHttpServerResponseCodes ToResponseCode(int32 StatusCode)
		{
			switch (StatusCode)
			{
			case 200:
				return EHttpServerResponseCodes::Ok;
			case 400:
				return EHttpServerResponseCodes::BadRequest;
			case 401:
				return EHttpServerResponseCodes::Denied;
			case 404:
				return EHttpServerResponseCodes::NotFound;
			case 405:
				return EHttpServerResponseCodes::BadMethod;
			case 413:
				return EHttpServerResponseCodes::RequestTooLarge;
			case 502:
				return EHttpServerResponseCodes::BadGateway;
			case 503:
				return EHttpServerResponseCodes::ServiceUnavail;
			default:
				return EHttpServerResponseCodes::ServerError;
			}
		}
	}

	const FString& GetUaiHttpCommandRoutePath()
	{
		return UaiHttpCommandRoutePath;
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
			OutIdentity = Identity();
			return true;
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
			OutDiagnostic))
		{
			return false;
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
		if (!RouteHost->Unbind(Config.InstanceId, OutDiagnostic))
		{
			return false;
		}
		bRunning = false;
		return true;
	}

	FUeAgentTransportServerHealth FUeAgentHttpTransportAdapter::Health() const
	{
		FUeAgentTransportServerHealth Result;
		Result.bRunning =
			bRunning && RouteHost->IsBound(Config.InstanceId);
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
		if (!bRunning || !RouteHost->IsBound(Config.InstanceId))
		{
			return MakeFailure(
				503,
				TEXT("transport_not_running"),
				TEXT("owned transport instance is not running"));
		}
		if (!Request.Method.Equals(Config.Method, ESearchCase::IgnoreCase))
		{
			return MakeFailure(
				405,
				TEXT("transport_method_mismatch"),
				TEXT("request method does not match the bound route"));
		}
		if (Request.RoutePath != Config.RoutePath)
		{
			return MakeFailure(
				404,
				TEXT("transport_route_mismatch"),
				TEXT("request route does not match the bound route"));
		}
		if (FindHeader(Request.Headers, Config.AuthHeader) != Config.AuthToken)
		{
			return MakeFailure(
				401,
				TEXT("transport_authentication_failed"),
				TEXT("transport credential was rejected"));
		}
		if (
			Config.MaxFrameBytes <= 0
			|| Request.Body.Num() > Config.MaxFrameBytes)
		{
			return MakeFailure(
				413,
				TEXT("frame_too_large"),
				TEXT("frame exceeds the configured byte capacity"));
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
			return MakeFailure(
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

	bool FUeAgentHttpTransportAdapter::ValidateConfig(
		FUeAgentTransportDiagnostic& OutDiagnostic) const
	{
		if (
			Config.OwnerId.IsEmpty()
			|| Config.InstanceId.IsEmpty()
			|| ParseVerb(Config.Method) == EHttpServerRequestVerbs::VERB_NONE
			|| !Config.RoutePath.StartsWith(TEXT("/"))
			|| Config.AuthHeader.IsEmpty()
			|| Config.AuthToken.IsEmpty()
			|| Config.Channel.IsEmpty()
			|| Config.PrincipalSubject.IsEmpty()
			|| Config.MaxFrameBytes <= 0)
		{
			OutDiagnostic = {
				TEXT("http_adapter_config_invalid"),
				TEXT("HTTP adapter configuration is incomplete"),
			};
			return false;
		}
		return true;
	}

	FUeAgentHttpResponse FUeAgentHttpTransportAdapter::MakeFailure(
		int32 StatusCode,
		const FString& Code,
		const FString& Message) const
	{
		FUeAgentHttpResponse Response;
		Response.StatusCode = StatusCode;
		Response.Headers.Add(TEXT("X-Transport-Diagnostic"), Code);
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

	struct FUeAgentEngineHttpRouteHost::FImpl
	{
		struct FBoundRoute
		{
			FHttpRouteHandle Handle;
			FUeAgentHttpRequestHandler Handler;
			FString RoutePath;
		};

		explicit FImpl(uint32 InPort)
			: Port(InPort)
		{
		}

		bool Bind(
			const FString& InInstanceId,
			const FString& Method,
			const FString& RoutePath,
			FUeAgentHttpRequestHandler InHandler,
			FUeAgentTransportDiagnostic& OutDiagnostic)
		{
			if (Routes.Contains(InInstanceId))
			{
				OutDiagnostic = {
					TEXT("http_route_already_bound"),
					TEXT("this route instance is already bound"),
				};
				return false;
			}
			for (const TPair<FString, FBoundRoute>& Pair : Routes)
			{
				if (Pair.Value.RoutePath.Equals(
						RoutePath,
						ESearchCase::CaseSensitive))
				{
					OutDiagnostic = {
						TEXT("http_route_already_bound"),
						TEXT("this route path is already bound"),
					};
					return false;
				}
			}
			const EHttpServerRequestVerbs Verb = ParseVerb(Method);
			if (Verb == EHttpServerRequestVerbs::VERB_NONE)
			{
				OutDiagnostic = {
					TEXT("http_method_unsupported"),
					TEXT("route host cannot bind the requested method"),
				};
				return false;
			}

			if (!Router.IsValid())
			{
				Router = FHttpServerModule::Get().GetHttpRouter(
					Port,
					true);
			}
			if (!Router.IsValid())
			{
				OutDiagnostic = {
					TEXT("http_router_unavailable"),
					TEXT("route host could not acquire the configured port"),
				};
				return false;
			}
			FHttpRouteHandle RouteHandle = Router->BindRoute(
				FHttpPath(RoutePath),
				Verb,
				FHttpRequestHandler::CreateLambda(
					[this, InInstanceId](
						const FHttpServerRequest& Request,
						const FHttpResultCallback& OnComplete)
					{
						return HandleEngineRequest(
							InInstanceId,
							Request,
							OnComplete);
					}));
			if (!RouteHandle.IsValid())
			{
				if (Routes.IsEmpty())
				{
					Router.Reset();
				}
				OutDiagnostic = {
					TEXT("http_route_bind_failed"),
					TEXT("route host could not bind the configured route"),
				};
				return false;
			}
			FBoundRoute Bound;
			Bound.Handle = RouteHandle;
			Bound.Handler = MoveTemp(InHandler);
			Bound.RoutePath = RoutePath;
			Routes.Add(InInstanceId, MoveTemp(Bound));

			FHttpServerModule::Get().StartAllListeners();
			return true;
		}

		bool Unbind(
			const FString& InInstanceId,
			FUeAgentTransportDiagnostic& OutDiagnostic)
		{
			FBoundRoute* Bound = Routes.Find(InInstanceId);
			if (!Bound || !Bound->Handle.IsValid())
			{
				OutDiagnostic = {
					TEXT("transport_server_instance_mismatch"),
					TEXT("route host refuses to unbind a different instance"),
				};
				return false;
			}
			Router->UnbindRoute(Bound->Handle);
			Routes.Remove(InInstanceId);
			if (Routes.IsEmpty())
			{
				Router.Reset();
			}
			return true;
		}

		bool IsBound(const FString& InInstanceId) const
		{
			const FBoundRoute* Bound = Routes.Find(InInstanceId);
			return Bound && Bound->Handle.IsValid();
		}

		bool HandleEngineRequest(
			const FString& InInstanceId,
			const FHttpServerRequest& Request,
			const FHttpResultCallback& OnComplete)
		{
			const FBoundRoute* Bound = Routes.Find(InInstanceId);
			if (!Bound || !Bound->Handler)
			{
				return false;
			}
			FUeAgentHttpRequest Mapped;
			Mapped.Method = LexVerb(Request.Verb);
			// IHttpRouter invokes this handler only after matching the
			// registered path. Request.RelativePath is the unmatched suffix,
			// not the canonical route identity required by N0.
			Mapped.RoutePath = Bound->RoutePath;
			Mapped.Body = Request.Body;
			Mapped.PeerId = Request.PeerAddress.IsValid()
				? Request.PeerAddress->ToString(true)
				: TEXT("unknown-peer");
			for (const TPair<FString, TArray<FString>>& Pair : Request.Headers)
			{
				if (!Pair.Value.IsEmpty())
				{
					Mapped.Headers.Add(Pair.Key, Pair.Value[0]);
				}
			}

			FUeAgentHttpResponse MappedResponse =
				Bound->Handler(Mapped);
			TUniquePtr<FHttpServerResponse> Response =
				FHttpServerResponse::Create(
					MoveTemp(MappedResponse.Body),
					TEXT("application/octet-stream"));
			Response->Code = ToResponseCode(MappedResponse.StatusCode);
			for (const TPair<FString, FString>& Pair : MappedResponse.Headers)
			{
				Response->Headers.Add(Pair.Key, {Pair.Value});
			}
			OnComplete(MoveTemp(Response));
			return true;
		}

		uint32 Port;
		TSharedPtr<IHttpRouter> Router;
		TMap<FString, FBoundRoute> Routes;
	};

	FUeAgentEngineHttpRouteHost::FUeAgentEngineHttpRouteHost(uint32 Port)
		: Impl(MakeUnique<FImpl>(Port))
	{
	}

	FUeAgentEngineHttpRouteHost::~FUeAgentEngineHttpRouteHost()
	{
		if (Impl)
		{
			TArray<FString> InstanceIds;
			Impl->Routes.GetKeys(InstanceIds);
			for (const FString& InstanceId : InstanceIds)
			{
				FUeAgentTransportDiagnostic Ignored;
				Impl->Unbind(InstanceId, Ignored);
			}
		}
	}

	bool FUeAgentEngineHttpRouteHost::Bind(
		const FString& InstanceId,
		const FString& Method,
		const FString& RoutePath,
		FUeAgentHttpRequestHandler Handler,
		FUeAgentTransportDiagnostic& OutDiagnostic)
	{
		return Impl->Bind(
			InstanceId,
			Method,
			RoutePath,
			MoveTemp(Handler),
			OutDiagnostic);
	}

	bool FUeAgentEngineHttpRouteHost::Unbind(
		const FString& InstanceId,
		FUeAgentTransportDiagnostic& OutDiagnostic)
	{
		return Impl->Unbind(InstanceId, OutDiagnostic);
	}

	bool FUeAgentEngineHttpRouteHost::IsBound(
		const FString& InstanceId) const
	{
		return Impl->IsBound(InstanceId);
	}
}

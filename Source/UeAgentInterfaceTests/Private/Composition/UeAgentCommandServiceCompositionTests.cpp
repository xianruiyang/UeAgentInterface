#include "Misc/AutomationTest.h"

#include "Composition/UeAgentCommandServiceComposition.h"
#include "CommandEdge/UeAgentCommandDispatch.h"
#include "CommandEdge/UeAgentCommandProtocol.h"
#include "Core/Contract/UeAgentCommandContract.h"


#include "NetworkTransport/UeAgentHttpTransportAdapter.h"

namespace
{
	using namespace UE::UAI::CommandContract;
	using namespace UE::UAI::CommandEdge;
	using namespace UE::UAI::Composition;
	using namespace UE::UAI::Contract;
	using namespace UE::UAI::NetworkTransport;

	class FFakeRouteHost final : public IUeAgentHttpRouteHost
	{
	public:
		bool Bind(
			const FString& InstanceId,
			const FString& Method,
			const FString& RoutePath,
			FUeAgentHttpRequestHandler Handler,
			FUeAgentTransportDiagnostic& OutDiagnostic) override
		{
			OutDiagnostic = {};
			if (Handlers.Contains(RoutePath)
				|| RouteByInstance.Contains(InstanceId))
			{
				OutDiagnostic = {
					TEXT("fake_route_already_bound"),
					TEXT("fake route is already owned"),
				};
				return false;
			}
			Methods.Add(RoutePath, Method);
			RouteByInstance.Add(InstanceId, RoutePath);
			Handlers.Add(RoutePath, MoveTemp(Handler));
			return true;
		}

		bool Unbind(
			const FString& InstanceId,
			FUeAgentTransportDiagnostic& OutDiagnostic) override
		{
			OutDiagnostic = {};
			const FString* Route = RouteByInstance.Find(InstanceId);
			if (!Route)
			{
				return false;
			}
			Handlers.Remove(*Route);
			Methods.Remove(*Route);
			RouteByInstance.Remove(InstanceId);
			return true;
		}

		bool IsBound(const FString& InstanceId) const override
		{
			return RouteByInstance.Contains(InstanceId);
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

		TMap<FString, FString> Methods;
		TMap<FString, FString> RouteByInstance;
		TMap<FString, FUeAgentHttpRequestHandler> Handlers;
	};

	class FFakeHandler final : public IUeAgentCommandHandler
	{
	public:
		explicit FFakeHandler(FString InCommandId)
			: CommandId(MoveTemp(InCommandId))
		{
		}

		const FString& GetCommandId() const override
		{
			return CommandId;
		}

		FUeAgentCommandResult Submit(
			const FUeAgentCommandRequest& Request,
			const FUeAgentCallerContext& CallerContext,
			const TSharedRef<IUeAgentCancellationSignal>&
				Cancellation) override
		{
			++Calls;
			LastCaller = CallerContext.GetCallerId();
			return FUeAgentCommandResult::Success(
				Request,
				TEXT("{\"handled\":true}"));
		}

		int32 Calls = 0;
		FString LastCaller;

	private:
		FString CommandId;
	};

	TArray<uint8> Utf8Bytes(const FString& Text)
	{
		const FTCHARToUTF8 Converted(*Text);
		TArray<uint8> Result;
		Result.Append(
			reinterpret_cast<const uint8*>(Converted.Get()),
			Converted.Length());
		return Result;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentCommandServiceCompositionRouteTest,
	"UeAgentInterface.CommandEdge.Composition.Route",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentCommandServiceCompositionRouteTest::RunTest(
	const FString& Parameters)
{
	const TSharedRef<const FUeAgentCommandContractRegistry> Registry =
		GetDefaultCommandContractRegistry();
	const TSharedRef<FFakeHandler> Query =
		MakeShared<FFakeHandler>(TEXT("reference.catalog_search"));
	const TSharedRef<FFakeHandler> Read =
		MakeShared<FFakeHandler>(TEXT("reference.asset_read"));
	const TSharedRef<FFakeHandler> Effect =
		MakeShared<FFakeHandler>(TEXT("reference.asset_apply"));
	const TSharedRef<FFakeRouteHost, ESPMode::ThreadSafe> RouteHost =
		MakeShared<FFakeRouteHost, ESPMode::ThreadSafe>();
	FUeAgentCommandServiceConfig Config;
	Config.PrincipalSubject = TEXT("command-service-test-agent");
	Config.AuthorityScopes = {
		TEXT("uai:query"),
		TEXT("uai:reference:write"),
	};
	TUniquePtr<FUeAgentCommandServiceComposition> Composition;
	FString Error;
	TestTrue(
		TEXT("command service graph composes"),
		FUeAgentCommandServiceComposition::TryCreate(
			Config,
			Registry,
			{Query, Read, Effect},
			Composition,
			Error));
	if (!Composition)
	{
		AddError(Error);
		return false;
	}
	FUeAgentHttpAdapterConfig HttpConfig;
	HttpConfig.OwnerId = TEXT("command-service-test-owner");
	HttpConfig.InstanceId = TEXT("command-service-test-instance");
	HttpConfig.Method = TEXT("POST");
	HttpConfig.RoutePath = GetUaiHttpCommandRoutePath();
	HttpConfig.AuthHeader = TEXT("X-UeAgentInterface-Token");
	HttpConfig.AuthToken = TEXT("command-service-test-token");
	HttpConfig.Channel = TEXT("uai.command");
	HttpConfig.PrincipalSubject = Config.PrincipalSubject;
	HttpConfig.MaxFrameBytes = 1024 * 1024;
	FUeAgentHttpTransportAdapter Adapter(
		HttpConfig,
		RouteHost,
		Composition->GetIngressHandler());
	FUeAgentTransportDiagnostic Diagnostic;
	FUeAgentTransportServerIdentity Identity;
	TestTrue(TEXT("one command route starts"), Adapter.Start(Identity, Diagnostic));
	TestEqual(TEXT("one route is bound"), RouteHost->Handlers.Num(), 1);
	TestTrue(
		TEXT("public command route is bound"),
		RouteHost->Handlers.Contains(GetUaiCommandRoutePath()));
	TestEqual(
		TEXT("route method is POST"),
		RouteHost->Methods.FindRef(GetUaiCommandRoutePath()),
		FString(TEXT("POST")));

	const FUeAgentCommandContract* Contract =
		Registry->FindExact(TEXT("reference.catalog_search"));
	const FString CorrelationId(TEXT("command-route-correlation"));
	const FString Body =
		TEXT("{\"command_binding_hash\":\"")
		+ Contract->GetBindingHash()
		+ TEXT("\",\"command_id\":\"reference.catalog_search\",")
		  TEXT("\"correlation_id\":\"")
		+ CorrelationId
		+ TEXT("\",\"message_kind\":\"command_request\",")
		  TEXT("\"params\":{\"catalog_kind\":\"uai_reference\",")
		  TEXT("\"match_mode\":\"broad\",")
		  TEXT("\"materialization_policy\":\"index_only\",")
		  TEXT("\"search_text\":\"PivotOffset\"},")
		  TEXT("\"request_id\":\"command-route-request\",")
		  TEXT("\"schema\":\"uai.command.v1\",\"version\":1}");
	FUeAgentHttpRequest Request;
	Request.Method = TEXT("POST");
	Request.RoutePath = GetUaiCommandRoutePath();
	Request.Headers.Add(
		TEXT("X-UeAgentInterface-Token"),
		HttpConfig.AuthToken);
	Request.Headers.Add(
		TEXT("X-Transport-Correlation"),
		CorrelationId);
	Request.Body = Utf8Bytes(Body);
	Request.PeerId = TEXT("command-route-peer");
	const FUeAgentHttpResponse Response = RouteHost->Invoke(Request);
	TestEqual(TEXT("HTTP adapter returns reply"), Response.StatusCode, 200);
	TestEqual(TEXT("handler called once"), Query->Calls, 1);
	TestEqual(
		TEXT("transport principal becomes caller"),
		Query->LastCaller,
		Config.PrincipalSubject);
	TestEqual(TEXT("effect handler remains idle"), Effect->Calls, 0);
	TestEqual(TEXT("read handler remains idle"), Read->Calls, 0);

	FUeAgentCommandProtocolCodec Codec;
	FUeAgentCommandProtocolDiagnostic ProtocolDiagnostic;
	TOptional<FUeAgentCommandResult> Result;
	TestTrue(
		TEXT("public command response decodes"),
		Codec.DecodeResult(
			Response.Body,
			Result,
			ProtocolDiagnostic));
	TestTrue(
		TEXT("public command result is successful"),
		Result.IsSet() && Result->IsOk());

	const FString InvalidCorrelationId(
		TEXT("command-route-invalid-correlation"));
	const FString InvalidBody =
		TEXT("{\"command_binding_hash\":\"")
		+ Contract->GetBindingHash()
		+ TEXT("\",\"command_id\":\"reference.catalog_search\",")
		  TEXT("\"correlation_id\":\"")
		+ InvalidCorrelationId
		+ TEXT("\",\"message_kind\":\"command_request\",")
		  TEXT("\"params\":{\"catalog_kind\":\"uai_reference\",")
		  TEXT("\"match_mode\":\"broad\",")
		  TEXT("\"materialization_policy\":\"index_only\",")
		  TEXT("\"search_text\":\"PivotOffset\",")
		  TEXT("\"unexpected\":true},")
		  TEXT("\"request_id\":\"command-route-invalid\",")
		  TEXT("\"schema\":\"uai.command.v1\",\"version\":1}");
	Request.Headers.Add(
		TEXT("X-Transport-Correlation"),
		InvalidCorrelationId);
	Request.Body = Utf8Bytes(InvalidBody);
	const FUeAgentHttpResponse InvalidResponse =
		RouteHost->Invoke(Request);
	Result.Reset();
	TestTrue(
		TEXT("unknown parameter response decodes"),
		Codec.DecodeResult(
			InvalidResponse.Body,
			Result,
			ProtocolDiagnostic));
	TestTrue(
		TEXT("unknown parameter is a protocol error"),
		Result.IsSet() && Result->IsProtocolError());
	TestEqual(
		TEXT("invalid request does not call handler"),
		Query->Calls,
		1);
	TestTrue(
		TEXT("owned route stops"),
		Adapter.Stop(HttpConfig.OwnerId, Diagnostic));
	TestTrue(TEXT("route is unbound"), RouteHost->Handlers.IsEmpty());
	return true;
}

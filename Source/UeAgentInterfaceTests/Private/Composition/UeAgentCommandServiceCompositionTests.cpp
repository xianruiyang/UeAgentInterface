#include "Misc/AutomationTest.h"

#include "CommandEdge/UeAgentCommandProtocol.h"
#include "Core/Contract/UeAgentContractJson.h"
#include "Dom/JsonObject.h"
#include "CommandEdge/UeAgentExactCommandDispatch.h"
#include "CommandRegistry/UeAgentImmutableCommandRegistry.h"
#include "NetworkTransport/UeAgentHttpTransportAdapter.h"
#include "ServiceComposition/CommandService/UeAgentCommandService.h"
#include "ServiceComposition/Query/UeAgentQueryCommandIngress.h"

namespace
{
	using namespace UE::UAI::CommandContract;
	using namespace UE::UAI::CommandEdge;
	using namespace UE::UAI::CommandRegistry;
	using namespace UE::UAI::Contract;
	using namespace UE::UAI::NetworkTransport;
	using namespace UE::UAI::ServiceComposition::CommandService;

	FUeAgentCommandDeclaration MakeTestDeclaration()
	{
		FUeAgentCommandDeclaration Result;
		Result.CommandId = TEXT("test.command.execute");
		Result.PythonNamespace = TEXT("test_command");
		Result.PythonFunction = TEXT("execute");
		Result.ContractVersion = 1;
		Result.PublicOwner = TEXT("test-command-service");
		Result.Availability = EUeAgentCommandAvailability::Enabled;
		Result.SideEffect = EUeAgentCommandSideEffect::AssetState;
		Result.StateLayers = {TEXT("test_state")};
		Result.InputFields = {
			{
				TEXT("target"),
				TEXT("string"),
				true,
				false,
				TEXT("Stable test target."),
			},
		};
		Result.ResultFields = {
			{
				TEXT("handled"),
				TEXT("boolean"),
				true,
				false,
				TEXT("Whether the test handler ran."),
			},
		};
		Result.Errors = {
			{
				TEXT("test_target_missing"),
				TEXT("The exact test target does not exist."),
			},
		};
		Result.Purpose.Summary = TEXT("Execute one test command.");
		Result.Purpose.Target = TEXT("A stable test target.");
		Result.Purpose.Preconditions = {TEXT("The test target exists.")};
		Result.Purpose.ParameterGuidance = TEXT("Pass the exact test target.");
		Result.Purpose.ResultGuidance = TEXT("Returns whether the handler ran.");
		Result.Purpose.FailureGuidance = TEXT("Preserves protocol errors.");
		Result.Purpose.SideEffectGuidance = TEXT("Mutates only test state.");
		Result.Purpose.MinimumExample =
			TEXT("test_command.execute(target='sample')");
		Result.Purpose.RelatedFlows = {TEXT("test command composition")};
		Result.Purpose.Keywords = {TEXT("test"), TEXT("execute")};
		Result.Purpose.Synonyms = {TEXT("run test command")};
		Result.TutorialReferences.Add(TEXT("uai-docs:test-command#execute"));
		return Result;
	}

	class FRecordingExactHandler final : public IUeAgentExactCommandHandler
	{
	public:
		const FString& GetCommandId() const override
		{
			return CommandId;
		}

		const FString& GetPublicOwner() const override
		{
			return PublicOwner;
		}

		FUeAgentCommandResult Submit(
			const FUeAgentCommandRequest& Request,
			const FUeAgentCallerContext& CallerContext,
			const TSharedRef<IUeAgentCancellationSignal>& Cancellation) override
		{
			++Calls;
			LastCaller = CallerContext.GetCallerId();
			LastAuthorityScopes = CallerContext.GetAuthorityScopes();
			return FUeAgentCommandResult::Success(
				Request,
				TEXT("{\"handled\":true}"));
		}

		int32 Calls = 0;
		FString LastCaller;
		TArray<FString> LastAuthorityScopes;

	private:
		FString CommandId = TEXT("test.command.execute");
		FString PublicOwner = TEXT("test-command-service");
	};

	class FFakeRouteHost final : public IUeAgentHttpRouteHost
	{
	public:
		bool Bind(
			const FString& InstanceId,
			const FString& Method,
			const FString& RoutePath,
			FUeAgentHttpRequestHandler Handler,
			FUeAgentTransportDiagnostic& OutDiagnostic,
			EUeAgentHttpRouteExecution Execution = EUeAgentHttpRouteExecution::GameThread) override
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

	class FTestContextInvalidator final
		: public IUeAgentRequestContextInvalidator
	{
	public:
		void InvalidateWorld(const FString& WorldIdentity) override {}
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
	FUeAgentCurrentCommandServiceCompositionRouteTest,
	"UeAgentInterface.CommandEdge.Composition.Route",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentCurrentCommandServiceCompositionRouteTest::RunTest(
	const FString& Parameters)
{
	FString Error;
	TSharedPtr<const FUeAgentImmutableCommandRegistry> Registry;
	TestTrue(
		TEXT("test declaration freezes into immutable registry"),
		FUeAgentImmutableCommandRegistry::TryFreeze(
			{MakeTestDeclaration()}, Registry, Error));
	const TSharedRef<FRecordingExactHandler> Handler =
		MakeShared<FRecordingExactHandler>();
	TSharedPtr<const FUeAgentExactCommandDispatch> Dispatch;
	TestTrue(
		TEXT("exact dispatch composes with recording handler"),
		Registry.IsValid()
			&& FUeAgentExactCommandDispatch::TryCreate(
				Registry.ToSharedRef(), {Handler}, Dispatch, Error));
	if (!Registry.IsValid() || !Dispatch.IsValid())
	{
		AddError(Error.IsEmpty()
			? TEXT("test command composition unavailable") : Error);
		return false;
	}
	const TSharedRef<FFakeRouteHost, ESPMode::ThreadSafe> RouteHost =
		MakeShared<FFakeRouteHost, ESPMode::ThreadSafe>();
	const FString PrincipalSubject(TEXT("command-service-test-agent"));
	const TArray<FString> AuthorityScopes = {
		TEXT("uai:query"),
	};
	TSharedPtr<IUeAgentTransportRequestHandler, ESPMode::ThreadSafe> Ingress;
	TestTrue(
		TEXT("query ingress composes from exact dispatch"),
		UE::UAI::ServiceComposition::Query::TryCreateQueryCommandIngress(
			Dispatch.ToSharedRef(),
			PrincipalSubject,
			AuthorityScopes,
			Ingress,
			Error));
	const TSharedRef<FTestContextInvalidator, ESPMode::ThreadSafe>
		ContextInvalidator =
			MakeShared<FTestContextInvalidator, ESPMode::ThreadSafe>();
	TUniquePtr<FUeAgentCommandService> CommandService;
	TestTrue(
		TEXT("command service owns query ingress"),
		Ingress.IsValid()
			&& FUeAgentCommandService::TryCreate(
				Ingress.ToSharedRef(),
				ContextInvalidator,
				CommandService,
				Error));
	if (!CommandService)
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
	HttpConfig.PrincipalSubject = PrincipalSubject;

	FUeAgentHttpTransportAdapter Adapter(
		HttpConfig,
		RouteHost,
		CommandService->GetIngressHandler());
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

	const FUeAgentFrozenCommandContract* Contract =
		Registry->FindExact(TEXT("test.command.execute"));
	if (!TestNotNull(TEXT("command contract exists"), Contract))
	{
		return false;
	}
	const FString CorrelationId(TEXT("command-route-correlation"));
	const FString Body =
		TEXT("{\"command_binding_hash\":\"")
		+ Contract->GetContractHash()
		+ TEXT("\",\"command_id\":\"test.command.execute\",")
		  TEXT("\"correlation_id\":\"")
		+ CorrelationId
		+ TEXT("\",\"message_kind\":\"command_request\",")
		  TEXT("\"params\":{\"target\":\"sample\"},")
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

	FUeAgentCommandProtocolDiagnostic ProtocolDiagnostic;
	TSharedPtr<FJsonObject> Result;
	TestTrue(
		TEXT("public command response is JSON"),
		UE::UAI::Contract::Json::TryParseObject(
			Response.Body,
			Result,
			ProtocolDiagnostic));
	TestTrue(
		TEXT("public command result is successful"),
		Result.IsValid() && Result->GetBoolField(TEXT("ok")));
	TestEqual(TEXT("handler called once"), Handler->Calls, 1);
	TestEqual(
		TEXT("transport principal becomes caller"),
		Handler->LastCaller,
		PrincipalSubject);
	TestEqual(
		TEXT("configured scopes reach handler"),
		Handler->LastAuthorityScopes,
		AuthorityScopes);

	TestTrue(
		TEXT("owned route stops"),
		Adapter.Stop(HttpConfig.OwnerId, Diagnostic));
	TestTrue(TEXT("route is unbound"), RouteHost->Handlers.IsEmpty());
	return true;
}

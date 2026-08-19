#include "NetworkTransport/UeAgentHttpTransportAdapter.h"
#include "NetworkTransport/UeAgentNetworkTransport.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
	using namespace UE::UAI::NetworkTransport;

	static FString FixturePath()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(
			FPaths::ProjectDir(),
			TEXT("UeAgentInterfaceCMD/tests/network_transport/fixtures/network_transport_cases.json")));
	}

	static TSharedPtr<FJsonObject> LoadFixture(FAutomationTestBase* Test)
	{
		FString Text;
		if (!Test->TestTrue(
			TEXT("load shared N0 fixture"),
			FFileHelper::LoadFileToString(Text, *FixturePath())))
		{
			return nullptr;
		}

		TSharedPtr<FJsonObject> Fixture;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
		if (!Test->TestTrue(
			TEXT("parse shared N0 fixture"),
			FJsonSerializer::Deserialize(Reader, Fixture) && Fixture.IsValid()))
		{
			return nullptr;
		}
		return Fixture;
	}

	static TArray<uint8> BytesFromHex(const FString& Hex)
	{
		TArray<uint8> Result;
		if ((Hex.Len() % 2) != 0)
		{
			return Result;
		}
		Result.Reserve(Hex.Len() / 2);
		for (int32 Index = 0; Index < Hex.Len(); Index += 2)
		{
			const int32 High = FParse::HexDigit(Hex[Index]);
			const int32 Low = FParse::HexDigit(Hex[Index + 1]);
			Result.Add(static_cast<uint8>((High << 4) | Low));
		}
		return Result;
	}

	static FUeAgentTransportCapabilities ReadCapabilities(
		const TSharedPtr<FJsonObject>& Fixture,
		const FString& Name)
	{
		const TSharedPtr<FJsonObject> Capabilities =
			Fixture->GetObjectField(TEXT("capabilities"))->GetObjectField(Name);
		FUeAgentTransportCapabilities Result;
		Result.bOrdered = Capabilities->GetBoolField(TEXT("ordered"));
		Result.bReliable = Capabilities->GetBoolField(TEXT("reliable"));
		Result.bPreservesMessageBoundaries =
			Capabilities->GetBoolField(TEXT("preserves_message_boundaries"));
		Result.MaxFrameBytes =
			static_cast<int64>(Capabilities->GetNumberField(TEXT("max_frame_bytes")));
		Result.bRequestReply = Capabilities->GetBoolField(TEXT("request_reply"));
		Result.bDuplicateSuppression =
			Capabilities->GetBoolField(TEXT("duplicate_suppression"));
		return Result;
	}

	static FUeAgentDeliveryRequirements ReadRequirements(
		const TSharedPtr<FJsonObject>& Fixture,
		const FString& Name)
	{
		const TSharedPtr<FJsonObject> Requirements =
			Fixture->GetObjectField(TEXT("requirements"))->GetObjectField(Name);
		FUeAgentDeliveryRequirements Result;
		Result.bRequireOrdered = Requirements->GetBoolField(TEXT("require_ordered"));
		Result.bRequireReliable = Requirements->GetBoolField(TEXT("require_reliable"));
		Result.bRequireMessageBoundaries =
			Requirements->GetBoolField(TEXT("require_message_boundaries"));
		Result.MaxFrameBytes =
			static_cast<int64>(Requirements->GetNumberField(TEXT("max_frame_bytes")));
		Result.bRequireRequestReply =
			Requirements->GetBoolField(TEXT("require_request_reply"));
		Result.bRequireDuplicateSuppression =
			Requirements->GetBoolField(TEXT("require_duplicate_suppression"));
		return Result;
	}

	class FRecordingHandler final : public IUeAgentTransportRequestHandler
	{
	public:
		virtual FUeAgentTransportReply Handle(
			const FUeAgentTransportIngress& Ingress) override
		{
			++Calls;
			LastPayload = Ingress.Frame.GetPayload();
			return FUeAgentTransportReply(Ingress.Frame);
		}

		int32 Calls = 0;
		TArray<uint8> LastPayload;
	};

	class FFakeHttpRouteHost final : public IUeAgentHttpRouteHost
	{
	public:
		virtual bool Bind(
			const FString& InstanceId,
			const FString& Method,
			const FString& RoutePath,
			FUeAgentHttpRequestHandler InHandler,
			FUeAgentTransportDiagnostic& OutDiagnostic) override
		{
			if (bBound)
			{
				OutDiagnostic = {
					TEXT("route_already_bound"),
					TEXT("fixture route is already bound"),
				};
				return false;
			}
			bBound = true;
			BoundInstanceId = InstanceId;
			BoundMethod = Method;
			BoundRoutePath = RoutePath;
			Handler = MoveTemp(InHandler);
			return true;
		}

		virtual bool Unbind(
			const FString& InstanceId,
			FUeAgentTransportDiagnostic& OutDiagnostic) override
		{
			if (!bBound || InstanceId != BoundInstanceId)
			{
				OutDiagnostic = {
					TEXT("transport_server_instance_mismatch"),
					TEXT("fixture refuses to unbind a different instance"),
				};
				return false;
			}
			LastUnboundInstanceId = InstanceId;
			bBound = false;
			Handler = nullptr;
			return true;
		}

		virtual bool IsBound(const FString& InstanceId) const override
		{
			return bBound && InstanceId == BoundInstanceId;
		}

		FUeAgentHttpResponse Dispatch(const FUeAgentHttpRequest& Request) const
		{
			return Handler(Request);
		}

		bool bBound = false;
		FString BoundInstanceId;
		FString LastUnboundInstanceId;
		FString BoundMethod;
		FString BoundRoutePath;
		FUeAgentHttpRequestHandler Handler;
	};

	static FUeAgentHttpAdapterConfig MakeAdapterConfig()
	{
		FUeAgentHttpAdapterConfig Config;
		Config.OwnerId = TEXT("owner-a");
		Config.InstanceId = TEXT("n0-http-fixture-1");
		Config.Method = TEXT("POST");
		Config.RoutePath = TEXT("/transport");
		Config.AuthHeader = TEXT("X-Transport-Token");
		Config.AuthToken = TEXT("fixture-secret");
		Config.Channel = TEXT("rpc");
		Config.PrincipalSubject = TEXT("fixture-agent");
		Config.MaxFrameBytes = 64;
		return Config;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentNetworkTransportOpaqueFrameTest,
	"UeAgentInterface.NetworkTransport.OpaqueFrame",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentNetworkTransportOpaqueFrameTest::RunTest(const FString& Parameters)
{
	using namespace UE::UAI::NetworkTransport;

	const TSharedPtr<FJsonObject> Fixture = LoadFixture(this);
	if (!Fixture.IsValid())
	{
		return false;
	}
	const TSharedPtr<FJsonObject> FrameRow =
		Fixture->GetArrayField(TEXT("frames"))[0]->AsObject();
	const TArray<uint8> Payload =
		BytesFromHex(FrameRow->GetStringField(TEXT("payload_hex")));
	const FUeAgentTransportPrincipal Principal{
		FrameRow->GetStringField(TEXT("principal_subject")),
		FrameRow->GetStringField(TEXT("authentication_method")),
	};
	const FUeAgentTransportPeer Peer{
		FrameRow->GetStringField(TEXT("peer_id")),
		Principal,
	};
	const FUeAgentTransportFrame Frame(
		FrameRow->GetStringField(TEXT("channel")),
		Payload,
		FrameRow->GetStringField(TEXT("correlation_id")),
		Peer);

	TestTrue(TEXT("opaque payload bytes preserved"), Frame.GetPayload() == Payload);
	TestEqual(TEXT("channel preserved"), Frame.GetChannel(), FString(TEXT("rpc")));
	TestEqual(
		TEXT("correlation preserved"),
		Frame.GetCorrelationId(),
		FString(TEXT("corr-0001")));
	TestEqual(
		TEXT("principal preserved"),
		Frame.GetPeer().Principal.Subject,
		FString(TEXT("agent-local")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentNetworkTransportCapabilityTest,
	"UeAgentInterface.NetworkTransport.CapabilityBinding",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentNetworkTransportCapabilityTest::RunTest(const FString& Parameters)
{
	using namespace UE::UAI::NetworkTransport;

	const TSharedPtr<FJsonObject> Fixture = LoadFixture(this);
	if (!Fixture.IsValid())
	{
		return false;
	}
	const FUeAgentTransportCapabilities Stream = ReadCapabilities(Fixture, TEXT("stream"));
	const FUeAgentTransportCapabilities Datagram = ReadCapabilities(Fixture, TEXT("datagram"));
	const FUeAgentDeliveryRequirements Portable = ReadRequirements(Fixture, TEXT("portable"));
	const FUeAgentDeliveryRequirements Strict = ReadRequirements(Fixture, TEXT("strict"));

	FUeAgentCapabilityBinding Binding;
	FUeAgentTransportBindingError Error;
	TestTrue(
		TEXT("portable requirements bind to stream"),
		TryBindCapabilities(Stream, Portable, Binding, Error));
	TestTrue(
		TEXT("portable requirements bind to datagram"),
		TryBindCapabilities(Datagram, Portable, Binding, Error));
	TestFalse(
		TEXT("strict requirements reject datagram before delivery"),
		TryBindCapabilities(Datagram, Strict, Binding, Error));
	TestEqual(
		TEXT("binding error code"),
		Error.Code,
		FString(TEXT("transport_capability_unsupported")));
	TestTrue(TEXT("ordered capability reported"), Error.MissingCapabilities.Contains(TEXT("ordered")));
	TestTrue(TEXT("reliable capability reported"), Error.MissingCapabilities.Contains(TEXT("reliable")));
	TestTrue(
		TEXT("duplicate suppression capability reported"),
		Error.MissingCapabilities.Contains(TEXT("duplicate_suppression")));

	FUeAgentTransportDelivery Failure;
	Failure.CorrelationId = TEXT("corr-0002");
	Failure.State = EUeAgentTransportDeliveryState::Disconnected;
	Failure.Attempts = 1;
	Failure.Diagnostic = FUeAgentTransportDiagnostic{
		TEXT("transport_disconnected"),
		TEXT("fixture disconnect"),
	};
	TestEqual(TEXT("transport failure is one attempt"), Failure.Attempts, 1);
	TestFalse(TEXT("transport failure has no reply"), Failure.Reply.IsSet());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentNetworkTransportHttpMappingTest,
	"UeAgentInterface.NetworkTransport.HttpPrivateMapping",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentNetworkTransportHttpMappingTest::RunTest(const FString& Parameters)
{
	using namespace UE::UAI::NetworkTransport;

	const TSharedRef<FFakeHttpRouteHost, ESPMode::ThreadSafe> RouteHost =
		MakeShared<FFakeHttpRouteHost, ESPMode::ThreadSafe>();
	const TSharedRef<FRecordingHandler, ESPMode::ThreadSafe> Handler =
		MakeShared<FRecordingHandler, ESPMode::ThreadSafe>();
	FUeAgentHttpTransportAdapter Adapter(
		MakeAdapterConfig(),
		RouteHost,
		Handler);

	FUeAgentTransportServerIdentity Identity;
	FUeAgentTransportDiagnostic Diagnostic;
	TestTrue(TEXT("HTTP adapter starts through owned route host"), Adapter.Start(Identity, Diagnostic));

	FUeAgentHttpRequest Request;
	Request.Method = TEXT("POST");
	Request.RoutePath = TEXT("/transport");
	Request.Headers.Add(TEXT("X-Transport-Token"), TEXT("fixture-secret"));
	Request.Headers.Add(TEXT("X-Transport-Correlation"), TEXT("corr-http-1"));
	Request.PeerId = TEXT("127.0.0.1");
	Request.Body = {
		0x00, 0x01, 0x7f, 0x80, 0xff, 0x55, 0xaa,
	};

	const FUeAgentHttpResponse Response = RouteHost->Dispatch(Request);
	TestEqual(TEXT("opaque HTTP mapping succeeds"), Response.StatusCode, 200);
	TestTrue(TEXT("response bytes are unchanged"), Response.Body == Request.Body);
	TestEqual(TEXT("handler called exactly once"), Handler->Calls, 1);
	TestTrue(TEXT("handler sees unchanged bytes"), Handler->LastPayload == Request.Body);

	Request.Headers[TEXT("X-Transport-Token")] = TEXT("wrong-token");
	const FUeAgentHttpResponse Unauthorized = RouteHost->Dispatch(Request);
	TestEqual(TEXT("HTTP auth failure remains private"), Unauthorized.StatusCode, 401);
	TestEqual(TEXT("unauthorized request never reaches handler"), Handler->Calls, 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentNetworkTransportOwnedControlTest,
	"UeAgentInterface.NetworkTransport.OwnedControl",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentNetworkTransportOwnedControlTest::RunTest(const FString& Parameters)
{
	using namespace UE::UAI::NetworkTransport;

	const TSharedRef<FFakeHttpRouteHost, ESPMode::ThreadSafe> RouteHost =
		MakeShared<FFakeHttpRouteHost, ESPMode::ThreadSafe>();
	const TSharedRef<FRecordingHandler, ESPMode::ThreadSafe> Handler =
		MakeShared<FRecordingHandler, ESPMode::ThreadSafe>();
	FUeAgentHttpTransportAdapter Adapter(
		MakeAdapterConfig(),
		RouteHost,
		Handler);

	FUeAgentTransportServerIdentity Identity;
	FUeAgentTransportDiagnostic Diagnostic;
	TestTrue(TEXT("owned adapter starts"), Adapter.Start(Identity, Diagnostic));
	TestEqual(TEXT("owner identity"), Identity.OwnerId, FString(TEXT("owner-a")));
	TestEqual(
		TEXT("instance identity"),
		Identity.InstanceId,
		FString(TEXT("n0-http-fixture-1")));
	TestTrue(TEXT("route is bound to exact instance"), RouteHost->IsBound(Identity.InstanceId));

	TestFalse(
		TEXT("different owner cannot stop adapter"),
		Adapter.Stop(TEXT("owner-b"), Diagnostic));
	TestTrue(TEXT("wrong owner leaves adapter running"), Adapter.Health().bRunning);
	TestTrue(TEXT("wrong owner leaves route bound"), RouteHost->IsBound(Identity.InstanceId));

	TestTrue(TEXT("owner stops exact adapter"), Adapter.Stop(TEXT("owner-a"), Diagnostic));
	TestFalse(TEXT("adapter reports stopped"), Adapter.Health().bRunning);
	TestEqual(
		TEXT("only exact instance was unbound"),
		RouteHost->LastUnboundInstanceId,
		Identity.InstanceId);
	TestTrue(TEXT("repeat stop is idempotent"), Adapter.Stop(TEXT("owner-a"), Diagnostic));
	return true;
}

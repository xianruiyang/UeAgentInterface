#include "Misc/AutomationTest.h"

#include "CommandEdge/UeAgentCommandProtocol.h"
#include "Core/Contract/UeAgentContractJson.h"
#include "Dom/JsonObject.h"
#include "CommandRegistry/UeAgentImmutableCommandRegistry.h"
#include "Core/DomainCapability/UeAgentDomainCapability.h"
#include "NetworkTransport/UeAgentNetworkTransport.h"
#include "ServiceComposition/CommandService/UeAgentCommandService.h"
#include "ServiceComposition/Query/UeAgentQueryCommandIngress.h"

namespace
{
	using namespace UE::UAI::CommandContract;
	using namespace UE::UAI::CommandEdge;
	using namespace UE::UAI::CommandRegistry;
	using namespace UE::UAI::NetworkTransport;
	using namespace UE::UAI::ServiceComposition::CommandService;

	class FTransportNeutralContextInvalidator final
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

	FString BuildRequestBody(
		const FUeAgentFrozenCommandContract& Contract,
		const FString& CorrelationId,
		const FString& RequestId)
	{
		return TEXT("{\"command_binding_hash\":\"")
			+ Contract.GetContractHash()
			+ TEXT("\",\"command_id\":\"query.command.search\",")
			  TEXT("\"correlation_id\":\"")
			+ CorrelationId
			+ TEXT("\",\"message_kind\":\"command_request\",")
			  TEXT("\"params\":{\"intent\":\"material expression\",\"limit\":1},")
			  TEXT("\"request_id\":\"")
			+ RequestId
			+ TEXT("\",\"schema\":\"uai.command.v1\",\"version\":1}");
	}

	FUeAgentTransportPeer MakePeer(
		const FString& PeerId,
		const FString& PrincipalSubject)
	{
		FUeAgentTransportPeer Peer;
		Peer.PeerId = PeerId;
		Peer.Principal.Subject = PrincipalSubject;
		Peer.Principal.AuthenticationMethod = TEXT("test-transport");
		return Peer;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentTransportNeutralCommandServiceTest,
	"UeAgentInterface.CommandEdge.TransportNeutral",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentTransportNeutralCommandServiceTest::RunTest(
	const FString& Parameters)
{
	UE::UAI::DomainCapability::FDomainCapabilityAuthorities Authorities;
	FString Error;
	TestTrue(
		TEXT("domain capability authorities compose"),
		UE::UAI::DomainCapability::
			TryGetOrBuildProcessDomainCapabilityAuthorities(
				Authorities, Error));
	const TSharedPtr<const FUeAgentImmutableCommandRegistry> Registry =
		Authorities.GetCommandRegistry();
	const TSharedPtr<const FUeAgentExactCommandDispatch> Dispatch =
		Authorities.GetCommandDispatch();
	if (!Registry.IsValid() || !Dispatch.IsValid())
	{
		AddError(Error.IsEmpty()
			? TEXT("domain capability authorities unavailable") : Error);
		return false;
	}
	const FString PrincipalSubject(TEXT("authenticated-uai-agent"));
	TSharedPtr<IUeAgentTransportRequestHandler, ESPMode::ThreadSafe> Ingress;
	TestTrue(
		TEXT("query ingress composes from exact dispatch"),
		UE::UAI::ServiceComposition::Query::TryCreateQueryCommandIngress(
			Dispatch.ToSharedRef(),
			PrincipalSubject,
			{TEXT("uai:query")},
			Ingress,
			Error));
	const TSharedRef<FTransportNeutralContextInvalidator, ESPMode::ThreadSafe>
		ContextInvalidator = MakeShared<
			FTransportNeutralContextInvalidator, ESPMode::ThreadSafe>();
	TUniquePtr<FUeAgentCommandService> CommandService;
	TestTrue(
		TEXT("transport-neutral command service owns query ingress"),
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

	const FUeAgentFrozenCommandContract* Contract =
		Registry->FindExact(TEXT("query.command.search"));
	if (!TestNotNull(TEXT("command contract exists"), Contract))
	{
		return false;
	}

	const FString CorrelationId(TEXT("transport-neutral-correlation"));
	FUeAgentTransportPeer Peer = MakePeer(
		TEXT("transport-neutral-peer"),
		PrincipalSubject);
	FUeAgentTransportFrame Frame(
		TEXT("uai.command"),
		Utf8Bytes(BuildRequestBody(
			*Contract,
			CorrelationId,
			TEXT("transport-neutral-request"))),
		CorrelationId,
		Peer);
	const FUeAgentTransportReply Reply =
		CommandService->GetIngressHandler()->Handle(
			FUeAgentTransportIngress(MoveTemp(Frame), Peer));

	FUeAgentCommandProtocolDiagnostic Diagnostic;
	TSharedPtr<FJsonObject> Result;
	TestTrue(
		TEXT("generic transport reply is JSON"),
		UE::UAI::Contract::Json::TryParseObject(Reply.Frame.GetPayload(), Result, Diagnostic));
	TestTrue(
		TEXT("generic transport reaches exact handler"),
		Result.IsValid() && Result->GetBoolField(TEXT("ok")));

	const FString RejectedCorrelationId(
		TEXT("transport-neutral-rejected-correlation"));
	FUeAgentTransportPeer RejectedPeer = MakePeer(
		TEXT("transport-neutral-rejected-peer"),
		TEXT("different-principal"));
	FUeAgentTransportFrame RejectedFrame(
		TEXT("uai.command"),
		Utf8Bytes(BuildRequestBody(
			*Contract,
			RejectedCorrelationId,
			TEXT("transport-neutral-rejected-request"))),
		RejectedCorrelationId,
		RejectedPeer);
	Result.Reset();
	const FUeAgentTransportReply RejectedReply =
		CommandService->GetIngressHandler()->Handle(
			FUeAgentTransportIngress(
				MoveTemp(RejectedFrame),
				RejectedPeer));
	TestTrue(
		TEXT("principal mismatch reply is JSON"),
		UE::UAI::Contract::Json::TryParseObject(
			RejectedReply.Frame.GetPayload(),
			Result,
			Diagnostic));
	TestTrue(
		TEXT("principal mismatch is a protocol error"),
		Result.IsValid() && Result->GetStringField(TEXT("message_kind")) == TEXT("protocol_error"));
	return true;
}

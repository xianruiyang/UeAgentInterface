#include "Misc/AutomationTest.h"

#include "CommandEdge/UeAgentCommandDispatch.h"
#include "CommandEdge/UeAgentCommandProtocol.h"
#include "Composition/UeAgentCommandServiceComposition.h"
#include "Core/Contract/UeAgentCommandContract.h"
#include "NetworkTransport/UeAgentNetworkTransport.h"

namespace
{
	using namespace UE::UAI::CommandContract;
	using namespace UE::UAI::CommandEdge;
	using namespace UE::UAI::Composition;
	using namespace UE::UAI::Contract;
	using namespace UE::UAI::NetworkTransport;

	class FTransportNeutralHandler final : public IUeAgentCommandHandler
	{
	public:
		explicit FTransportNeutralHandler(FString InCommandId)
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
			const TSharedRef<IUeAgentCancellationSignal>& Cancellation) override
		{
			++Calls;
			LastCaller = CallerContext.GetCallerId();
			return FUeAgentCommandResult::Success(
				Request,
				TEXT("{\"transport_neutral\":true}"));
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

	FString BuildRequestBody(
		const FUeAgentCommandContract& Contract,
		const FString& CorrelationId,
		const FString& RequestId)
	{
		return TEXT("{\"command_binding_hash\":\"")
			+ Contract.GetBindingHash()
			+ TEXT("\",\"command_id\":\"reference.catalog_search\",")
			  TEXT("\"correlation_id\":\"")
			+ CorrelationId
			+ TEXT("\",\"message_kind\":\"command_request\",")
			  TEXT("\"params\":{\"catalog_kind\":\"uai_reference\",")
			  TEXT("\"match_mode\":\"broad\",")
			  TEXT("\"materialization_policy\":\"index_only\",")
			  TEXT("\"search_text\":\"PivotOffset\"},")
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
	"UAI.StrictV2.T02.TRANSPORT.SERVICE",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentTransportNeutralCommandServiceTest::RunTest(
	const FString& Parameters)
{
	const TSharedRef<const FUeAgentCommandContractRegistry> Registry =
		GetDefaultCommandContractRegistry();
	const TSharedRef<FTransportNeutralHandler> Handler =
		MakeShared<FTransportNeutralHandler>(
			TEXT("reference.catalog_search"));
	const TSharedRef<FTransportNeutralHandler> ReadHandler =
		MakeShared<FTransportNeutralHandler>(
			TEXT("reference.asset_read"));
	const TSharedRef<FTransportNeutralHandler> EffectHandler =
		MakeShared<FTransportNeutralHandler>(
			TEXT("reference.asset_apply"));

	FUeAgentCommandServiceConfig Config;
	Config.PrincipalSubject = TEXT("authenticated-uai-agent");
	Config.AuthorityScopes = {TEXT("uai:query")};
	TUniquePtr<FUeAgentCommandServiceComposition> Composition;
	FString Error;
	TestTrue(
		TEXT("transport-neutral command service composes"),
		FUeAgentCommandServiceComposition::TryCreate(
			Config,
			Registry,
			{Handler, ReadHandler, EffectHandler},
			Composition,
			Error));
	if (!Composition)
	{
		AddError(Error);
		return false;
	}

	const FUeAgentCommandContract* Contract =
		Registry->FindExact(TEXT("reference.catalog_search"));
	if (!TestNotNull(TEXT("command contract exists"), Contract))
	{
		return false;
	}

	const FString CorrelationId(TEXT("transport-neutral-correlation"));
	FUeAgentTransportPeer Peer = MakePeer(
		TEXT("transport-neutral-peer"),
		Config.PrincipalSubject);
	FUeAgentTransportFrame Frame(
		TEXT("uai.command"),
		Utf8Bytes(BuildRequestBody(
			*Contract,
			CorrelationId,
			TEXT("transport-neutral-request"))),
		CorrelationId,
		Peer);
	const FUeAgentTransportReply Reply =
		Composition->GetIngressHandler()->Handle(
			FUeAgentTransportIngress(MoveTemp(Frame), Peer));

	FUeAgentCommandProtocolCodec Codec;
	FUeAgentCommandProtocolDiagnostic Diagnostic;
	TOptional<FUeAgentCommandResult> Result;
	TestTrue(
		TEXT("generic transport reply decodes"),
		Codec.DecodeResult(Reply.Frame.GetPayload(), Result, Diagnostic));
	TestTrue(
		TEXT("generic transport reaches exact handler"),
		Result.IsSet() && Result->IsOk());
	TestEqual(TEXT("handler called once"), Handler->Calls, 1);
	TestEqual(TEXT("read handler is not called"), ReadHandler->Calls, 0);
	TestEqual(TEXT("effect handler is not called"), EffectHandler->Calls, 0);
	TestEqual(
		TEXT("transport principal becomes caller"),
		Handler->LastCaller,
		Config.PrincipalSubject);

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
		Composition->GetIngressHandler()->Handle(
			FUeAgentTransportIngress(
				MoveTemp(RejectedFrame),
				RejectedPeer));
	TestTrue(
		TEXT("principal mismatch reply decodes"),
		Codec.DecodeResult(
			RejectedReply.Frame.GetPayload(),
			Result,
			Diagnostic));
	TestTrue(
		TEXT("principal mismatch is a protocol error"),
		Result.IsSet() && Result->IsProtocolError());
	TestEqual(
		TEXT("rejected principal does not call handler"),
		Handler->Calls,
		1);
	return true;
}

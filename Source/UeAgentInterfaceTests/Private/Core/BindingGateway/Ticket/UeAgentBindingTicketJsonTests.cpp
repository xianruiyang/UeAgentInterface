#include "Misc/AutomationTest.h"

#include "Core/BindingGateway/Ticket/UeAgentBindingTicketJson.h"


#include "Core/ContextResource/UeAgentContextResource.h"
#include "Core/Contract/UeAgentContract.h"
#include "Core/Discovery/UeAgentDiscovery.h"
#include "Core/StateSemantic/UeAgentStateSemantic.h"

namespace
{
	using namespace UE::UAI::BindingGateway::Ticket;
	using namespace UE::UAI::Contract;
	using namespace UE::UAI::Discovery;
	using namespace UE::UAI::StateSemantic;

	FUeAgentDescriptorIdentity Descriptor()
	{
		return FUeAgentDescriptorIdentity(
			TEXT("fixture.reference.crud"),
			1,
			TEXT("sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"));
	}

	FUeAgentCanonicalIdentity Identity()
	{
		return FUeAgentCanonicalIdentity(
			TEXT("core.reference_asset.display_name"),
			TEXT("asset_property"),
			TEXT("/Game/UAI/Reference/BP_Reference.BP_Reference"),
			TEXT("BlueprintDisplayName"),
			TEXT("blueprint_asset"),
			TEXT("ticket-json-session"));
	}

	FUeAgentCandidateBinding Binding()
	{
		return FUeAgentCandidateBinding(
			TEXT("reference-binding:ticket-json-session"),
			FUeAgentCandidateSummary(
				Identity(),
				Descriptor(),
				TEXT("uai_reference"),
				TEXT("Reference Blueprint Display Name"),
				TEXT("Controlled reference CRUD asset"),
				{TEXT("UAI"), TEXT("ReferenceCrud")}),
			FUeAgentCatalogRevision(
				TEXT("uai-reference-catalog"),
				TEXT("ticket-json-session"),
				7),
			TEXT("uai://reference/asset?object=/Game/UAI/Reference/BP_Reference.BP_Reference&property=BlueprintDisplayName"),
			TEXT("uai-reference-generic-read"),
			{
				EUeAgentCandidateOperation::Read,
				EUeAgentCandidateOperation::Create,
				EUeAgentCandidateOperation::Update,
				EUeAgentCandidateOperation::Replace,
				EUeAgentCandidateOperation::Delete,
			},
			TEXT("ue-reflection:UBlueprint.BlueprintDisplayName"));
	}
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentBindingTicketJsonRoundTripTest,
	"UeAgentInterface.Core.BindingGateway.Ticket.JsonRoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentBindingTicketJsonRoundTripTest::RunTest(
	const FString& Parameters)
{
	const FUeAgentCandidateBinding Candidate = Binding();
	const FUeAgentOperationSessionIdentity Session(
		TEXT("ticket-json-session"),
		9,
		TEXT("sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"));
	const FUeAgentCallerContext Caller(
		TEXT("ticket-json-agent"),
		{TEXT("uai:query"), TEXT("uai:reference:write")},
		TEXT("ticket-json-correlation"));
	TSharedPtr<const FUeAgentOperationTicketService> Service;
	FUeAgentOperationTicketError TicketError;
	TestTrue(
		TEXT("create shared ticket owner"),
		FUeAgentOperationTicketService::TryCreate(
			TEXT("ticket-json-integrity-key"),
			30.0,
			[]() { return 100.0; },
			[]() { return FString(TEXT("ticket-json-1")); },
			Service,
			TicketError));
	TOptional<FUeAgentOperationTicket> Ticket;
	TestTrue(
		TEXT("issue typed ticket"),
		Service->TryIssue(
			Candidate,
			Descriptor(),
			Session,
			Caller,
			FUeAgentStateRevision(
				TEXT("uai-reference-generic-read"),
				TEXT("ticket-json-session"),
				11),
			Ticket,
			TicketError));

	FUeAgentBindingTicketJsonError JsonError;
	const TSharedRef<FJsonObject> BindingJson =
		FUeAgentBindingTicketJsonCodec::BindingToJson(Candidate);
	TOptional<FUeAgentCandidateBinding> DecodedBinding;
	TestTrue(
		TEXT("candidate binding round-trips through shared codec"),
		FUeAgentBindingTicketJsonCodec::TryParseBinding(
			BindingJson,
			DecodedBinding,
			JsonError));
	TestEqual(
		TEXT("binding identity remains exact"),
		DecodedBinding->GetSummary().GetIdentity().GetCanonicalKey(),
		Candidate.GetSummary().GetIdentity().GetCanonicalKey());
	TestEqual(
		TEXT("binding operation set remains exact"),
		DecodedBinding->GetSupportedOperations(),
		Candidate.GetSupportedOperations());

	const TSharedRef<FJsonObject> TicketJson =
		FUeAgentBindingTicketJsonCodec::TicketToJson(
			Ticket.GetValue());
	TOptional<FUeAgentOperationTicket> DecodedTicket;
	TestTrue(
		TEXT("operation ticket round-trips through shared codec"),
		FUeAgentBindingTicketJsonCodec::TryParseTicket(
			TicketJson,
			DecodedTicket,
			JsonError));
	TestEqual(
		TEXT("ticket descriptor remains candidate operation descriptor"),
		DecodedTicket->GetDescriptorIdentity(),
		Descriptor());
	TestTrue(
		TEXT("decoded ticket validates with the same owner"),
		Service->TryValidate(
			DecodedTicket.GetValue(),
			DecodedBinding.GetValue(),
			Descriptor(),
			Session,
			Caller,
			TicketError));
	return true;
}

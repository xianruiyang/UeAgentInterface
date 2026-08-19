#include "Misc/AutomationTest.h"
#include <type_traits>

#include "Core/BindingGateway/Operation/UeAgentOperationBinding.h"
#include "Core/BindingGateway/Query/UeAgentQueryBinding.h"
#include "Core/BindingGateway/Ticket/UeAgentBindingTicket.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentOperationBindingModuleAvailableTest,
	"UeAgentInterface.Core.BindingGateway.Operation.ModuleAvailable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentOperationBindingModuleAvailableTest::RunTest(
	const FString& Parameters)
{
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentOperationBindingSharedTicketOwnerTest,
	"UeAgentInterface.Core.BindingGateway.Operation.SharedTicketOwner",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentOperationBindingSharedTicketOwnerTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI::BindingGateway;
	using namespace UE::UAI::ContextResource;
	using namespace UE::UAI::Contract;
	static_assert(std::is_same_v<
		Query::FUeAgentQuerySessionIdentity,
		Ticket::FUeAgentOperationSessionIdentity>);
	static_assert(std::is_same_v<
		Query::FUeAgentQueryTicket,
		Ticket::FUeAgentOperationTicket>);
	const Ticket::FUeAgentOperationSessionIdentity Session(
		TEXT("fixture-operation-session"),
		23,
		TEXT(
			"sha256:1111111111111111111111111111111111111111111111111111111111111111"));
	TestTrue(TEXT("shared session is valid"), Session.IsValid());
	using FSubmitSignature = bool (
		Operation::FUeAgentOperationBinding::*)(
		const Operation::FUeAgentOperationBindingRequest&,
		const FUeAgentCallerContext&,
		TSharedRef<FUeAgentResourceTracker, ESPMode::ThreadSafe>,
		const FUeAgentOperationDeadline&,
		const FUeAgentCancellationToken&,
		TSharedPtr<
			const Operation::FUeAgentOperationBindingResult>&,
		Operation::FUeAgentOperationBindingError&) const;
	const FSubmitSignature Submit =
		&Operation::FUeAgentOperationBinding::TrySubmit;
	TestTrue(
		TEXT("C3O carries one shared C1 tracker owner into C7"),
		Submit != nullptr);
	return true;
}

#include "Misc/AutomationTest.h"

#include "Composition/ReadExportCommand/UeAgentReadExportCommandHandlers.h"
#include "Core/Contract/UeAgentCommandContract.h"
#include "Core/Contract/UeAgentDescriptorRegistry.h"

namespace
{
	using namespace UE::UAI::CommandContract;
	using namespace UE::UAI::Composition::ReadExportCommand;
	using namespace UE::UAI::Contract;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentReadExportCommandContractTest,
	"UeAgentInterface.Composition.ReadExport.E3."
		"RegistryManifestAndExactDescriptorRoutes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentReadExportCommandContractTest::RunTest(
	const FString& Parameters)
{
	const TSharedRef<const FUeAgentCommandContractRegistry>
		CommandRegistry = GetDefaultCommandContractRegistry();
	const TArray<FUeAgentReadExportCommandRoute>& Routes =
		GetReadExportQueryCommandRoutes();
	TestEqual(
		TEXT("collapsed read inventory exposes 46 query commands"),
		Routes.Num(),
		46);
	TestEqual(
		TEXT("default manifest has 3 reference, 46 query and 1 export commands"),
		CommandRegistry->GetContracts().Num(),
		50);

	TSharedPtr<const FUeAgentDescriptorRegistry> DescriptorRegistry;
	FString Error;
	TestTrue(
		TEXT("read/export descriptor registry composes once"),
		TryCreateReadExportDescriptorRegistry(
			DescriptorRegistry,
			Error));
	TestTrue(
		TEXT("read/export descriptor registry exists"),
		DescriptorRegistry.IsValid());
	if (!DescriptorRegistry.IsValid())
	{
		return false;
	}
	TestEqual(
		TEXT("descriptor inventory stays collapsed"),
		DescriptorRegistry->GetDescriptors().Num(),
		46);

	TSet<FString> CommandIds;
	TSet<FString> DescriptorIds;
	for (const FUeAgentReadExportCommandRoute& Route : Routes)
	{
		TestTrue(TEXT("route is valid"), Route.IsValid());
		TestFalse(
			TEXT("public command has one route"),
			CommandIds.Contains(Route.CommandId));
		TestFalse(
			TEXT("descriptor has one public query route"),
			DescriptorIds.Contains(
				Route.DescriptorIdentity.GetDescriptorId()));
		CommandIds.Add(Route.CommandId);
		DescriptorIds.Add(
			Route.DescriptorIdentity.GetDescriptorId());
		TestNotNull(
			TEXT("command is in the formal C++ registry"),
			CommandRegistry->FindExact(Route.CommandId));
		TestNotNull(
			TEXT("route resolves an exact production descriptor"),
			DescriptorRegistry->FindExact(
				Route.DescriptorIdentity));
	}
	const FUeAgentCommandContract* Export =
		CommandRegistry->FindExact(
			GetSemanticExportCommandId());
	TestNotNull(
		TEXT("semantic export command is formal"),
		Export);
	TestTrue(
		TEXT("legacy export route is not a public command"),
		CommandRegistry->FindExact(TEXT("/api/uai/export"))
			== nullptr);
	return true;
}

#include "Misc/AutomationTest.h"

#include "CommandEdge/UeAgentExactCommandDispatch.h"
#include "CommandRegistry/UeAgentImmutableCommandRegistry.h"
#include "Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h"

namespace
{
	using namespace UE::UAI::CommandEdge;
	using namespace UE::UAI::CommandRegistry;
	using namespace UE::UAI::CommandContract;
	using namespace UE::UAI::Module::CommandRegistration::Active;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentReadExportCommandContractTest,
	"UeAgentInterface.Composition.ReadExport.E3."
		"RegistryManifestAndExactDescriptorRoutes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentReadExportCommandContractTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	TSharedPtr<const FUeAgentImmutableCommandRegistry> CommandRegistry;
	TSharedPtr<const FUeAgentExactCommandDispatch> CommandDispatch;
	FString Error;
	if (!TestTrue(
			TEXT("read/export consumes the process immutable command authority"),
			TryComposeProcessActiveCommandComposition(
				CommandRegistry,
				CommandDispatch,
				Error))
		|| !CommandRegistry.IsValid()
		|| !CommandDispatch.IsValid())
	{
		AddError(Error);
		return false;
	}

	const TArray<FString> RequiredReadExportCommandIds{
		TEXT("query.calibration.read"),
		TEXT("query.command.search"),
		TEXT("query.command.detail"),
		TEXT("content_graph.compound"),
		TEXT("content_graph.diagnostics"),
		TEXT("content_graph.export_workspace"),
		TEXT("content_graph.import_workspace"),
		TEXT("content_graph.items"),
		TEXT("content_graph.properties"),
		TEXT("content_graph.relations"),
		TEXT("content_graph.structure"),
		TEXT("content_graph.summary"),
		TEXT("reference.asset_apply"),
		TEXT("reference.asset_read"),
		TEXT("reference.origin.resolve"),
	};
	for (const FString& CommandId : RequiredReadExportCommandIds)
	{
		TestNotNull(
			TEXT("current read/export command is exact: ") + CommandId,
			CommandRegistry->FindExact(CommandId));
	}
	TestNull(
		TEXT("retired global reference catalog is absent"),
		CommandRegistry->FindExact(TEXT("reference.catalog_search")));
	TestNull(
		TEXT("legacy export route is not a public command"),
		CommandRegistry->FindExact(TEXT("/api/uai/export")));

	const FUeAgentFrozenCommandContract* Export =
		CommandRegistry->FindExact(TEXT("content_graph.export_workspace"));
	const FUeAgentFrozenCommandContract* Read =
		CommandRegistry->FindExact(TEXT("reference.asset_read"));
	if (!Export || !Read)
	{
		return false;
	}
	TestEqual(
		TEXT("workspace export has target, path, and profile inputs"),
		Export->GetInputFields().Num(),
		3);
	TestEqual(
		TEXT("asset read has object, context, and projection inputs"),
		Read->GetInputFields().Num(),
		3);
	return true;
}

#include "Misc/AutomationTest.h"

#include "CommandEdge/UeAgentExactCommandDispatch.h"
#include "CommandRegistry/UeAgentImmutableCommandRegistry.h"
#include "Core/Contract/UeAgentContractSchema.h"
#include "Core/Discovery/Command/UeAgentCommandDiscovery.h"
#include "Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h"

namespace
{
	using namespace UE::UAI::CommandEdge;
	using namespace UE::UAI::CommandRegistry;
	using namespace UE::UAI::CommandContract;
	using namespace UE::UAI::Discovery::Command;
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
	const FUeAgentFrozenCommandContract* Import =
		CommandRegistry->FindExact(TEXT("content_graph.import_workspace"));
	const FUeAgentFrozenCommandContract* Read =
		CommandRegistry->FindExact(TEXT("reference.asset_read"));
	if (!Export || !Import || !Read)
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
	const FUeAgentCommandFieldDeclaration* ImportSource =
		Import->GetInputFields().FindByPredicate(
			[](const FUeAgentCommandFieldDeclaration& Field)
			{
				return Field.Name == TEXT("source");
			});
	TestNotNull(
		TEXT("workspace import exposes the source field"),
		ImportSource);
	if (ImportSource)
	{
		TestTrue(
			TEXT("workspace import source schema exposes all public carriers"),
			ImportSource->SchemaJson.Contains(TEXT("\"oneOf\""))
				&& ImportSource->SchemaJson.Contains(TEXT("\"inline\""))
				&& ImportSource->SchemaJson.Contains(TEXT("\"single_file\""))
				&& ImportSource->SchemaJson.Contains(TEXT("\"directory\""))
				&& ImportSource->SchemaJson.Contains(TEXT("\"document\""))
				&& ImportSource->SchemaJson.Contains(TEXT("\"path\"")));
		TSharedPtr<const UE::UAI::Contract::FUeAgentContractSchema>
			CompiledSourceSchema;
		UE::UAI::Contract::FUeAgentContractSchemaError SchemaError;
		TestTrue(
			TEXT("workspace import source schema compiles as ContractSchema v1"),
			UE::UAI::Contract::FUeAgentContractSchema::TryCompile(
				UE::UAI::Contract::FUeAgentContractValue(
					ImportSource->SchemaJson),
				CompiledSourceSchema,
				SchemaError));
	}
	TSharedPtr<const FCommandDiscoveryService> Discovery;
	FCommandDiscoveryError DiscoveryError;
	if (!TestTrue(
		TEXT("active command discovery composes for read/export contracts"),
		FCommandDiscoveryService::TryCreate(
			CommandRegistry.ToSharedRef(), Discovery, DiscoveryError))
		|| !Discovery.IsValid())
	{
		AddError(DiscoveryError.Code + TEXT(":") + DiscoveryError.Detail);
		return false;
	}
	FCommandSearchRequest Search;
	Search.Intent = TEXT("initialize nested UObject graph with hjson");
	Search.Limit = 3;
	FCommandSearchPage Page;
	TestTrue(
		TEXT("natural recursive initialization intent searches"),
		Discovery->Search(Search, Page, DiscoveryError));
	TestTrue(
		TEXT("natural recursive initialization ranks exact import in top three"),
		Page.Candidates.ContainsByPredicate(
			[](const FCommandCandidate& Candidate)
			{
				return Candidate.CommandId
					== TEXT("content_graph.import_workspace");
			}));
	const FUeAgentFrozenCommandContract* Compound =
		CommandRegistry->FindExact(TEXT("content_graph.compound"));
	TestNotNull(TEXT("one recursive read contract is exact"), Compound);
	if (Compound)
	{
		TestFalse(
			TEXT("recursive read does not advertise import initialization"),
			Compound->GetPurpose().Keywords.Contains(TEXT("initialize"))
				|| Compound->GetPurpose().Keywords.Contains(TEXT("import")));
	}
	TestFalse(
		TEXT("workspace export does not advertise import initialization"),
		Export->GetPurpose().Keywords.Contains(TEXT("initialize"))
			|| Export->GetPurpose().Keywords.Contains(TEXT("import")));
	return !HasAnyErrors();
}

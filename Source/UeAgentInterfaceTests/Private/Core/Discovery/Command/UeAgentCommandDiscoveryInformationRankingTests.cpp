#include "Misc/AutomationTest.h"

#include "CommandRegistry/UeAgentImmutableCommandRegistry.h"
#include "Core/Discovery/Command/UeAgentCommandDiscovery.h"

namespace
{
	using namespace UE::UAI::CommandContract;
	using namespace UE::UAI::CommandRegistry;
	using namespace UE::UAI::Discovery::Command;

	FUeAgentCommandPurposeDeclaration Purpose(
		FString Summary,
		FString Target,
		TArray<FString> Keywords,
		TArray<FString> Synonyms = {})
	{
		FUeAgentCommandPurposeDeclaration Result;
		Result.Summary = MoveTemp(Summary);
		Result.Target = MoveTemp(Target);
		Result.Preconditions = {TEXT("Use the exact frozen command contract.")};
		Result.ParameterGuidance = TEXT("Forward exact public fields only.");
		Result.ResultGuidance = TEXT("Inspect the typed result and read back independently.");
		Result.FailureGuidance = TEXT("Typed failures do not authorize a fallback command.");
		Result.SideEffectGuidance = TEXT("Apply only the declared side effects.");
		Result.MinimumExample = TEXT("client.fixture.call() ");
		Result.RelatedFlows = {
			TEXT("query.command.search"),
			TEXT("query.command.usage")};
		Result.Keywords = MoveTemp(Keywords);
		Result.Synonyms = MoveTemp(Synonyms);
		return Result;
	}

	FUeAgentCommandDeclaration Declaration(
		FString CommandId,
		FString PythonFunction,
		FUeAgentCommandPurposeDeclaration CommandPurpose)
	{
		FUeAgentCommandDeclaration Result;
		Result.CommandId = MoveTemp(CommandId);
		Result.PythonNamespace = TEXT("fixture");
		Result.PythonFunction = MoveTemp(PythonFunction);
		Result.ContractVersion = 1;
		Result.PublicOwner = TEXT("command-discovery-ranking-fixture");
		Result.Availability = EUeAgentCommandAvailability::Enabled;
		Result.SideEffect = EUeAgentCommandSideEffect::EditorState;
		Result.StateLayers = {TEXT("fixture_state")};
		Result.ResultFields = {{
			TEXT("result"), TEXT("object"), true, false,
			TEXT("Typed fixture result."),
		}};
		Result.Errors = {{
			TEXT("fixture_failed"), TEXT("The fixture command failed."),
		}};
		Result.Purpose = MoveTemp(CommandPurpose);
		return Result;
	}

	FUeAgentCommandVariantDeclaration Variant(
		FString VariantId,
		FUeAgentCommandPurposeDeclaration VariantPurpose)
	{
		FUeAgentCommandVariantDeclaration Result;
		Result.VariantId = MoveTemp(VariantId);
		Result.RequestSchemaJson =
			TEXT("{\"type\":\"object\",\"additionalProperties\":false}");
		Result.Purpose = MoveTemp(VariantPurpose);
		return Result;
	}

	TSharedPtr<const FUeAgentImmutableCommandRegistry> BuildRegistry(
		FAutomationTestBase& Test)
	{
		TArray<FUeAgentCommandDeclaration> Declarations;
		Declarations.Add(Declaration(
			TEXT("asset.factory_create"),
			TEXT("factory_create"),
			Purpose(
				TEXT("Produce a new level through the generic factory."),
				TEXT("A level or map package."),
				{TEXT("create"), TEXT("level"), TEXT("factory")},
				{TEXT("new level")})));

		FUeAgentCommandDeclaration EditorSession = Declaration(
			TEXT("editor.session"),
			TEXT("session"),
			Purpose(
				TEXT("Control one existing Editor session."),
				TEXT("An Editor session target."),
				{TEXT("editor"), TEXT("session")},
				{TEXT("control")}));
		EditorSession.Variants.Add(Variant(
			TEXT("policy:world-context:set-current/v1"),
			Purpose(
				TEXT("Open or switch to one existing map."),
				TEXT("An existing level map."),
				{TEXT("open"), TEXT("existing"), TEXT("level"), TEXT("map")},
				{TEXT("switch world")})));
		Declarations.Add(MoveTemp(EditorSession));

		Declarations.Add(Declaration(
			TEXT("pcg.data_create"),
			TEXT("pcg_data_create"),
			Purpose(
				TEXT("Create PCG data from an existing level."),
				TEXT("PCG data derived from a level."),
				{TEXT("create"), TEXT("pcg"), TEXT("data"), TEXT("level")},
				{TEXT("procedural generation")})));

		Declarations.Add(Declaration(
			TEXT("query.noise"),
			TEXT("noise"),
			Purpose(
				TEXT("Create the current Unreal asset and the new level in the project."),
				TEXT("The current Unreal asset level."),
				{TEXT("inspect")},
				{TEXT("read")})));
		FUeAgentCommandDeclaration Dormant = Declaration(
			TEXT("legacy.retired"),
			TEXT("legacy_retired"),
			Purpose(
				TEXT("Legacy fixture command."),
				TEXT("A retired fixture target."),
				{TEXT("legacy"), TEXT("retired")}));
		Dormant.Availability = EUeAgentCommandAvailability::Dormant;
		Declarations.Add(MoveTemp(Dormant));

		TSharedPtr<const FUeAgentImmutableCommandRegistry> Registry;
		FString Error;
		Test.TestTrue(
			TEXT("frozen command-ranking fixture is valid"),
			FUeAgentImmutableCommandRegistry::TryFreeze(
				MoveTemp(Declarations), Registry, Error));
		if (!Registry.IsValid())
		{
			Test.AddError(Error);
		}
		return Registry;
	}

	const FCommandCandidate* SearchFirst(
		FAutomationTestBase& Test,
		const FCommandDiscoveryService& Discovery,
		const FString& Intent,
		FCommandSearchPage& OutPage)
	{
		FCommandSearchRequest Request;
		Request.Intent = Intent;
		FCommandDiscoveryError Error;
		Test.TestTrue(
			TEXT("focused intent searches: ") + Intent,
			Discovery.Search(Request, OutPage, Error));
		if (!Error.Code.IsEmpty())
		{
			Test.AddError(Error.Code + TEXT(":") + Error.Detail);
		}
		return OutPage.Candidates.IsEmpty()
			? nullptr
			: &OutPage.Candidates[0];
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentCommandDiscoveryInformationRankingTest,
	"UeAgentInterface.Core.Discovery.Command.E1.KeywordRanking",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentCommandDiscoveryInformationRankingTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	const TSharedPtr<const FUeAgentImmutableCommandRegistry> Registry =
		BuildRegistry(*this);
	if (!Registry.IsValid())
	{
		return false;
	}
	TSharedPtr<const FCommandDiscoveryService> Discovery;
	FCommandDiscoveryError Error;
	if (!TestTrue(
			TEXT("focused discovery service constructs"),
			FCommandDiscoveryService::TryCreate(
				Registry.ToSharedRef(), Discovery, Error))
		|| !Discovery.IsValid())
	{
		AddError(Error.Code + TEXT(":") + Error.Detail);
		return false;
	}

	FCommandSearchPage Page;
	const FCommandCandidate* Candidate = SearchFirst(
		*this, *Discovery, TEXT("editor.session"), Page);
	TestNotNull(TEXT("an exact active command name is returned directly"), Candidate);
	TestEqual(TEXT("exact active search returns one name"), Page.Candidates.Num(), 1);
	Candidate = SearchFirst(
		*this, *Discovery, TEXT("legacy.retired"), Page);
	TestNull(TEXT("an exact dormant command stays hidden"), Candidate);
	TestTrue(TEXT("exact dormant search returns no names"), Page.Candidates.IsEmpty());

	Candidate = SearchFirst(
		*this, *Discovery, TEXT("create level"), Page);
	TestNotNull(TEXT("create level has a candidate"), Candidate);
	if (Candidate)
	{
		TestEqual(
			TEXT("create level prefers the generic factory/create identity"),
			Candidate->CommandId,
			FString(TEXT("asset.factory_create")));
	}
	Candidate = SearchFirst(
		*this, *Discovery, TEXT("open existing Level"), Page);
	TestNotNull(TEXT("open existing level has a candidate"), Candidate);
	if (Candidate)
	{
		TestEqual(
			TEXT("open existing level prefers editor.session"),
			Candidate->CommandId,
			FString(TEXT("editor.session")));
		TestEqual(
			TEXT("one canonical command occupies one result slot"),
			Page.Candidates.FilterByPredicate(
				[](const FCommandCandidate& Item)
				{
					return Item.CommandId == TEXT("editor.session");
				}).Num(),
			1);
	}

	Candidate = SearchFirst(
		*this,
		*Discovery,
		TEXT("create PCG data from existing Level"),
		Page);
	TestNotNull(TEXT("PCG from level has a candidate"), Candidate);
	if (Candidate)
	{
		TestEqual(
			TEXT("PCG intent remains domain-specific"),
			Candidate->CommandId,
			FString(TEXT("pcg.data_create")));
	}

	Candidate = SearchFirst(
		*this, *Discovery, TEXT("inspect actor"), Page);
	TestNotNull(TEXT("nearby actor intent has a candidate"), Candidate);
	if (Candidate)
	{
		TestEqual(
			TEXT("actor remains a complete token and does not match factory"),
			Candidate->CommandId,
			FString(TEXT("query.noise")));
	}
	TestFalse(
		TEXT("actor never matches the factory token as a substring"),
		Page.Candidates.ContainsByPredicate(
			[](const FCommandCandidate& Item)
			{
				return Item.CommandId == TEXT("asset.factory_create");
			}));

	FCommandSearchRequest UnknownKeywordRequest;
	UnknownKeywordRequest.Intent = TEXT("unregistered_keyword");
	Page = {};
	TestTrue(
		TEXT("an unknown keyword is handled deterministically"),
		Discovery->Search(UnknownKeywordRequest, Page, Error));
	TestEqual(
		TEXT("an unknown keyword does not manufacture candidates"),
		Page.TotalMatches,
		0);
	TestTrue(
		TEXT("an unknown keyword has no candidates"),
		Page.Candidates.IsEmpty());
	return !HasAnyErrors();
}

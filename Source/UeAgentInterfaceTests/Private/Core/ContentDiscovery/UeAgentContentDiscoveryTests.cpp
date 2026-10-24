#include "Misc/AutomationTest.h"

#include "Core/ContentDiscovery/UeAgentContentDiscovery.h"
#include "Core/ContentDiscovery/UeAgentNameDiscoveryProjection.h"
#include "Core/ContentDiscovery/PaletteSearch/UeAgentPaletteSearchTypes.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "DomainAdapters/Blueprint/Node/UeAgentBlueprintNodeMechanics.h"
#include "DomainAdapters/BlueprintCommands/UeAgentBlueprintCommandMechanicsBoundary.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Components/AudioComponent.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SCS_Node.h"
#include "GameFramework/Actor.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Materials/Material.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringCatalogCandidates.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringTargetLocator.h"

namespace
{
	using namespace UE::UAI::ContentDiscovery;

	class FFixtureProvider final : public IUeAgentContentDiscoveryProvider
	{
	public:
		FFixtureProvider(
			FString InProviderId,
			FString InKind,
			FString InRevision,
			TArray<FString> InIdentities,
			bool bInFail = false)
			: ProviderId(MoveTemp(InProviderId))
			, Kinds({MoveTemp(InKind)})
			, Revision(MoveTemp(InRevision))
			, Identities(MoveTemp(InIdentities))
			, bFail(bInFail)
		{
		}

		const FString& GetProviderId() const override
		{
			return ProviderId;
		}

		const TArray<FString>& GetContentKinds() const override
		{
			return Kinds;
		}

		bool TrySearch(
			const FContentDiscoveryRequest& Request,
			FContentDiscoveryProviderPage& OutPage,
			FContentDiscoveryError& OutError) override
		{
			OutPage = {};
			OutError = {};
			if (bFail)
			{
				OutError.Code = TEXT("fixture_provider_failed");
				OutError.SubjectId = ProviderId;
				return false;
			}
			TArray<FContentDiscoveryItem> Items;
			for (const FString& Identity : Identities)
			{
				FContentDiscoveryItem Item;
				Item.Identity = Identity;
				Item.DisplayName = TEXT("Fixture Item");
				Item.Domain = TEXT("fixture");
				Item.ContentKind = Kinds[0];
				Item.ClassPath = TEXT("/Script/Fixture.Object");
				Item.OwnerIdentity = TEXT("fixture-owner");
				Item.PublicDiscoveryScopes = {Kinds[0]};
				Item.StateLayer = TEXT("fixture-current");
				Item.Revision = Revision;
				Item.DetailCommandId = TEXT("fixture.read");
				Item.DetailTargetIdentity = Identity;
				Item.DetailInput = MakeShared<FJsonObject>();
				Item.SupportedOperations = {TEXT("query")};
				Items.Add(MoveTemp(Item));
			}
			return TryBuildContentDiscoveryProviderPage(
				ProviderId,
				Revision,
				Request,
				MoveTemp(Items),
				OutPage,
				OutError);
		}

	private:
		FString ProviderId;
		TArray<FString> Kinds;
		FString Revision;
		TArray<FString> Identities;
		bool bFail = false;
	};

	FContentDiscoveryRequest Request()
	{
		FContentDiscoveryRequest Result;
		Result.MatchMode = EContentDiscoveryMatchMode::Text;
		Result.ContentKinds = {TEXT("asset"), TEXT("actor")};
		Result.RequiredOperations = {TEXT("query")};
		return Result;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentContentDiscoveryContractTest,
	"UeAgentInterface.Core.ContentDiscovery.Contract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentContentDiscoveryContractTest::RunTest(const FString& Parameters)
{
	FContentDiscoveryError Error;
	TSharedPtr<const FUeAgentContentDiscoveryService> Service;
	TestTrue(
		TEXT("compose two exact provider identities"),
		FUeAgentContentDiscoveryService::TryCreate(
			{
				MakeShared<FFixtureProvider>(
					TEXT("fixture.asset"),
					TEXT("asset"),
					TEXT("revision-a"),
					TArray<FString>{TEXT("/Game/A.A"), TEXT("/Game/B.B")}),
				MakeShared<FFixtureProvider>(
					TEXT("fixture.actor"),
					TEXT("actor"),
					TEXT("revision-w"),
					TArray<FString>{TEXT("World.ActorA"), TEXT("World.ActorB")}),
			},
			Service,
			Error));
	TestTrue(TEXT("content discovery service returned"), Service.IsValid());
	if (!Service.IsValid())
	{
		return false;
	}

	FContentDiscoveryRequest Search = Request();
	FContentDiscoverySearchResult Result;
	TestTrue(
		TEXT("search returns every selected provider"),
		Service->TrySearch(Search, Result, Error));
	TestEqual(TEXT("two provider states returned"), Result.Providers.Num(), 2);
	TestTrue(TEXT("search is complete"), Result.IsComplete());
	TestEqual(
		TEXT("request fingerprint is stable"),
		Result.RequestFingerprint,
		Search.GetRequestFingerprint());
	for (const FContentDiscoveryProviderResult& Provider : Result.Providers)
	{
		TestTrue(TEXT("provider result succeeded"), Provider.Page.IsSet());
		if (Provider.Page.IsSet())
		{
			TestEqual(TEXT("every provider item is returned"), Provider.Page->Items.Num(), 2);
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentContentDiscoveryModelSurfaceEligibilityTest,
	"UeAgentInterface.Core.ContentDiscovery.ModelSurfaceEligibility",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentContentDiscoveryModelSurfaceEligibilityTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	FContentDiscoveryRequest Search = Request();
	Search.ContentKinds = {TEXT("asset")};
	TArray<FContentDiscoveryItem> Items;
	auto AddItem = [&Items](const FString& Identity, const FString& DisplayName)
	{
		FContentDiscoveryItem Item;
		Item.Identity = Identity;
		Item.DisplayName = DisplayName;
		Item.Domain = TEXT("fixture");
		Item.ContentKind = TEXT("asset");
		Item.ClassPath = TEXT("/Script/Fixture.Object");
		Item.OwnerIdentity = TEXT("fixture-owner");
		Item.PublicDiscoveryScopes = {TEXT("asset")};
		Item.StateLayer = TEXT("fixture-current");
		Item.Revision = TEXT("revision-a");
		Item.DetailCommandId = TEXT("fixture.read");
		Item.DetailTargetIdentity = Identity;
		Item.DetailInput = MakeShared<FJsonObject>();
		Item.SupportedOperations = {TEXT("query")};
		Items.Add(MoveTemp(Item));
	};
	AddItem(
		TEXT("fixture:unsafe"),
		FString(TEXT("Generated_"))
			+ FString::ChrN(24, TEXT('Q')) + TEXT("1234"));
	AddItem(TEXT("fixture:safe"), TEXT("Readable Asset"));
	FContentDiscoveryProviderPage Page;
	FContentDiscoveryError Error;
	TestTrue(
		TEXT("provider page is built from model-addressable items"),
		TryBuildContentDiscoveryProviderPage(
			TEXT("fixture.asset"),
			TEXT("revision-a"),
			Search,
			MoveTemp(Items),
			Page,
			Error));
	TestEqual(TEXT("unreadable item is excluded before counting"),
		Page.TotalMatches, 1);
	TestEqual(TEXT("one readable item remains"), Page.Items.Num(), 1);
	if (Page.Items.Num() == 1)
	{
		TestEqual(TEXT("the readable item survives the public page"),
			Page.Items[0].DisplayName, FString(TEXT("Readable Asset")));
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentMaterialCandidateQueryReadOnlyTest,
	"UeAgentInterface.Core.ContentDiscovery.MaterialCandidateQueryReadOnly",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentMaterialCandidateQueryReadOnlyTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
	UMaterial* Material = NewObject<UMaterial>(GetTransientPackage());
	if (!TestNotNull(TEXT("transient material exists"), Material))
	{
		return false;
	}
	TestNull(TEXT("fixture starts without a material graph"),
		Material->MaterialGraph);
	UeAgentPaletteSearch::FSearchParams Search;
	Search.CatalogType = TEXT("material_node");
	Search.SearchText = TEXT("Scalar Parameter");
	Search.SearchMode = TEXT("contains");
	Search.AssetPath = Material->GetPathName();
	Search.GraphName = TEXT("MaterialGraph");
	UeAgentPaletteSearch::FSearchResult Result;
	FString Error;
	TestTrue(
		TEXT("native material candidates enumerate through a preview graph"),
		UeAgentPaletteSearch::ExecuteSearch(Search, Result, Error));
	TestNull(TEXT("query does not attach a graph to the target material"),
		Material->MaterialGraph);
	for (const UeAgentPaletteSearch::FCandidate& Candidate
		: Result.MatchingCandidates)
	{
		int32 Separator = INDEX_NONE;
		if (Candidate.Name.FindChar(TEXT('.'), Separator))
		{
			FString Left = Candidate.Name.Left(Separator);
			FString Right = Candidate.Name.Mid(Separator + 1);
			Left.TrimStartAndEndInline();
			Right.TrimStartAndEndInline();
			TestFalse(TEXT("public name does not repeat an object-path suffix"),
				!Left.IsEmpty() && Left.Equals(Right, ESearchCase::IgnoreCase));
		}
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentContentDiscoveryFailureIsolationTest,
	"UeAgentInterface.Core.ContentDiscovery.FailureIsolation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentContentDiscoveryFailureIsolationTest::RunTest(
	const FString& Parameters)
{
	FContentDiscoveryError Error;
	TSharedPtr<const FUeAgentContentDiscoveryService> Service;
	TestTrue(
		TEXT("compose one healthy and one failing provider"),
		FUeAgentContentDiscoveryService::TryCreate(
			{
				MakeShared<FFixtureProvider>(
					TEXT("fixture.asset"), TEXT("asset"), TEXT("revision-a"),
					TArray<FString>{TEXT("/Game/A.A")}),
				MakeShared<FFixtureProvider>(
					TEXT("fixture.actor"), TEXT("actor"), TEXT("revision-w"),
					TArray<FString>{}, true),
			},
			Service,
			Error));
	FContentDiscoverySearchResult SearchResult;
	TestTrue(
		TEXT("provider failure does not erase healthy results"),
		Service->TrySearch(Request(), SearchResult, Error));
	TestTrue(TEXT("aggregate retains provider failure"), SearchResult.HasFailures());
	TestFalse(TEXT("aggregate is not falsely complete"), SearchResult.IsComplete());
	TestTrue(
		TEXT("healthy provider remains addressable"),
		SearchResult.Providers[0].Succeeded()
			|| SearchResult.Providers[1].Succeeded());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentContentDiscoveryKeywordEngineTest,
	"UeAgentInterface.Core.ContentDiscovery.KeywordEngine",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentContentDiscoveryKeywordEngineTest::RunTest(
	const FString& Parameters)
{
	FContentDiscoveryRequest BoundarySearch;
	BoundarySearch.Query = TEXT("actor");
	BoundarySearch.MatchMode = EContentDiscoveryMatchMode::Text;
	int32 BoundaryScore = 0;
	FString BoundaryReason;
	TestFalse(
		TEXT("actor does not match inside the factory token"),
		TryScoreContentDiscoveryMatch(
			BoundarySearch,
			TEXT("candidate:factory"),
			TEXT("Factory"),
			{},
			BoundaryScore,
			BoundaryReason));
	TestTrue(
		TEXT("actor matches a camel-case field token"),
		TryScoreContentDiscoveryMatch(
			BoundarySearch,
			TEXT("candidate:set_actor_location"),
			TEXT("SetActorLocation"),
			{},
			BoundaryScore,
			BoundaryReason));

	BoundarySearch.Query = TEXT("actor location");
	TestTrue(
		TEXT("every positive keyword can match a distinct field token"),
		TryScoreContentDiscoveryMatch(
			BoundarySearch,
			TEXT("candidate:set_actor_location"),
			TEXT("SetActorLocation"),
			{},
			BoundaryScore,
			BoundaryReason));

	BoundarySearch.Query = TEXT("loc");
	TestTrue(
		TEXT("a keyword prefix matches from a field token boundary"),
		TryScoreContentDiscoveryMatch(
			BoundarySearch,
			TEXT("candidate:set_actor_location"),
			TEXT("SetActorLocation"),
			{},
			BoundaryScore,
			BoundaryReason));

	BoundarySearch.Query = TEXT("actorlocation");
	TestTrue(
		TEXT("contains semantics can span complete tokens from a token boundary"),
		TryScoreContentDiscoveryMatch(
			BoundarySearch,
			TEXT("candidate:set_actor_location"),
			TEXT("SetActorLocation"),
			{},
			BoundaryScore,
			BoundaryReason));

	BoundarySearch.Query = TEXT("FInterp To");
	int32 ExactPhraseScore = 0;
	int32 NoisyTokenScore = 0;
	TestTrue(
		TEXT("a compact camel-case action matches its readable phrase"),
		TryScoreContentDiscoveryMatch(
			BoundarySearch,
			TEXT("candidate:finterp"),
			TEXT("FInterpTo"),
			{TEXT("KismetMathLibrary")},
			ExactPhraseScore,
			BoundaryReason));
	TestTrue(
		TEXT("an action may still match the same individual tokens"),
		TryScoreContentDiscoveryMatch(
			BoundarySearch,
			TEXT("candidate:vinterp"),
			TEXT("VInterpTo"),
			{TEXT("Function"), TEXT("KismetMathLibrary")},
			NoisyTokenScore,
			BoundaryReason));
	TestTrue(
		TEXT("the exact compact phrase outranks scattered token matches"),
		ExactPhraseScore > NoisyTokenScore);

	FContentDiscoveryRequest Search;
	Search.Query = TEXT("actor location");
	Search.MatchMode = EContentDiscoveryMatchMode::Text;
	Search.ContentKinds = {TEXT("catalog_candidate")};
	TArray<FContentDiscoveryItem> Items;
	auto AddItem = [&Items](
		const FString& Identity,
		const FString& DisplayName,
		TArray<FString> SearchTerms)
	{
		FContentDiscoveryItem Item;
		Item.Identity = Identity;
		Item.DisplayName = DisplayName;
		Item.Domain = TEXT("fixture");
		Item.ContentKind = TEXT("catalog_candidate");
		Item.ClassPath = TEXT("/Script/Fixture.Candidate");
		Item.StateLayer = TEXT("native_catalog_current");
		Item.Revision = TEXT("revision-catalog");
		Item.DetailCommandId = TEXT("fixture.detail");
		Item.DetailTargetIdentity = Identity;
		Item.DetailInput = MakeShared<FJsonObject>();
		Item.SupportedOperations = {TEXT("create")};
		Item.SearchTerms = MoveTemp(SearchTerms);
		Items.Add(MoveTemp(Item));
	};
	AddItem(TEXT("candidate:a"), TEXT("Set Actor Location"), {TEXT("transform")});
	AddItem(TEXT("CANDIDATE:A"), TEXT("Set Actor Location"), {TEXT("duplicate")});
	AddItem(TEXT("candidate:b"), TEXT("Get Actor Location"), {TEXT("transform")});
	AddItem(TEXT("candidate:c"), TEXT("Set Material"), {TEXT("surface")});

	FContentDiscoveryProviderPage Page;
	FContentDiscoveryError Error;
	TestTrue(
		TEXT("positive keywords build one deterministic page"),
		TryBuildContentDiscoveryProviderPage(
			TEXT("fixture.catalog"),
			TEXT("revision-catalog"),
			Search,
			MoveTemp(Items),
			Page,
			Error));
	TestEqual(TEXT("duplicate identity is collapsed"), Page.TotalMatches, 2);
	TestEqual(TEXT("all matches are returned"), Page.Items.Num(), 2);

	Search.Query = TEXT("actro");
	Search.MatchMode = EContentDiscoveryMatchMode::Fuzzy;
	int32 Score = 0;
	FString Reason;
	TestFalse(
		TEXT("legacy fuzzy mode does not guess misspelled natural language"),
		TryScoreContentDiscoveryMatch(
			Search,
			TEXT("candidate:a"),
			TEXT("Actor"),
			{},
			Score,
			Reason));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentCatalogCandidateExactResolutionTest,
	"UeAgentInterface.Core.ContentDiscovery.ExactCandidateResolution",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentCatalogCandidateExactResolutionTest::RunTest(
	const FString& Parameters)
{
	using namespace UeAgentAuthoringCatalogCandidates;
	TArray<UeAgentAuthoringIdentity::FCanonicalIdentity> Identities;
	for (const FString& Name : {FString(TEXT("Branch")), FString(TEXT("Delay"))})
	{
		FIdentityInput Input;
		Input.CatalogType = TEXT("blueprint_node");
		Input.TargetKind = TEXT("blueprint_node");
		Input.Name = Name;
		Input.DisplayName = Name;
		Input.FullName = TEXT("K2Node_") + Name;
		Input.StableId = Input.FullName;
		Input.SearchKey = Name;
		UeAgentAuthoringIdentity::FCanonicalIdentity Identity;
		FString BuildError;
		TestTrue(
			TEXT("fixture candidate identity builds"),
			BuildIdentity(Input, Identity, BuildError));
		Identities.Add(MoveTemp(Identity));
	}

	UeAgentAuthoringTargetLocator::FResolveResult ResolveResult;
	TestTrue(
		TEXT("model-facing name resolves exactly"),
		ResolveExactCandidate(
			TEXT("Branch"),
			FString(),
			Identities,
			ResolveResult));
	TestEqual(TEXT("name selects Branch"), ResolveResult.ResolvedIndex, 0);
	TestTrue(
		TEXT("compact public id resolves exactly"),
		ResolveExactCandidate(
			FString(),
			BuildPublicId(Identities[1]),
			Identities,
			ResolveResult));
	TestEqual(TEXT("id selects Delay"), ResolveResult.ResolvedIndex, 1);
	TestFalse(
		TEXT("conflicting name and id are rejected"),
		ResolveExactCandidate(
			TEXT("Branch"),
			BuildPublicId(Identities[1]),
			Identities,
			ResolveResult));
	TestEqual(
		TEXT("conflict is explicit"),
		ResolveResult.ErrorCode,
		FString(TEXT("target_identity_conflict")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentPlaceableActorExactCandidateTest,
	"UeAgentInterface.Core.ContentDiscovery.PlaceableActorExactCandidate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentPlaceableActorExactCandidateTest::RunTest(
	const FString& Parameters)
{
	UeAgentPaletteSearch::FSearchParams Search;
	Search.CatalogType = TEXT("placeable_actor");
	UeAgentPaletteSearch::FSearchResult SearchResult;
	FString Error;
	if (!TestTrue(
			TEXT("native Place Actors registry is enumerated"),
			UeAgentPaletteSearch::ExecuteSearch(
				Search,
				SearchResult,
				Error)))
	{
		AddError(Error);
		return false;
	}
	const UeAgentPaletteSearch::FCandidate* Source =
		SearchResult.MatchingCandidates.FindByPredicate(
			[](const UeAgentPaletteSearch::FCandidate& Candidate)
			{
				return Candidate.bSupportedInContext
					&& !Candidate.Name.IsEmpty()
					&& !Candidate.GetPublicId().IsEmpty();
			});
	if (!TestNotNull(
			TEXT("one executable Place Actors candidate is available"),
			Source))
	{
		return false;
	}
	UeAgentPaletteSearch::FCandidate Resolved;
	TestTrue(
		TEXT("the public name/id pair re-resolves through the same registry"),
		UeAgentPaletteSearch::TryResolvePlaceableActorCandidate(
			Source->Name,
			Source->GetPublicId(),
			Source->CategoryPath,
			Resolved,
			Error));
	TestEqual(
		TEXT("re-resolve preserves canonical candidate identity"),
		Resolved.GetCandidateId(),
		Source->GetCandidateId());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentNamesOnlyProjectionTest,
	"UeAgentInterface.Core.ContentDiscovery.NamesOnly.Projection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentNamesOnlyProjectionTest::RunTest(const FString& Parameters)
{
	FString PayloadJson;
	FString Error;
	TestFalse(
		TEXT("duplicate names require a semantic scope"),
		TryBuildNamesOnlyPayload(
			{TEXT(" Sequence "), TEXT("sequence"), TEXT("Branch")},
			ENameComparison::IgnoreCase,
			PayloadJson,
			Error));
	TestEqual(TEXT("duplicate failure is typed"), Error,
		FString(TEXT("name_discovery_scope_required")));
	TestTrue(
		TEXT("complete names result passes shared completeness validation"),
		TryValidateNameDiscoveryCompleteness(
			{3, 3}, Error));
	for (const FNameDiscoveryCompleteness Invalid : {
		FNameDiscoveryCompleteness{-1, 0},
		FNameDiscoveryCompleteness{1, -1},
		FNameDiscoveryCompleteness{1, 2}})
	{
		TestFalse(
			TEXT("invalid completeness counters fail closed"),
			TryValidateNameDiscoveryCompleteness(Invalid, Error));
		TestEqual(TEXT("invalid completeness failure is typed"), Error,
			FString(TEXT("name_discovery_completeness_invalid")));
	}
	for (const FString& Unreadable : {
		FString(TEXT("/Game/Identity")),
		FString(TEXT("0123456789abcdef0123456789abcdef")),
		FString(TEXT("{01234567-89ab-cdef-0123-456789abcdef}")),
		FString(TEXT("GeneratedActor_"))
			+ FString::ChrN(24, TEXT('Q')) + TEXT("1234")})
	{
		TestFalse(
			TEXT("machine or path identity is rejected from names-only output"),
			TryBuildNamesOnlyPayload(
				{Unreadable}, ENameComparison::IgnoreCase,
				PayloadJson, Error));
		TestEqual(TEXT("unreadable name failure is typed"), Error,
			FString(TEXT("name_discovery_name_invalid")));
	}
	TestTrue(
		TEXT("ordinary numbered Unreal names remain readable"),
		IsReadableDiscoveryName(TEXT("StaticMeshComponent0"))
			&& IsReadableDiscoveryName(TEXT("MI_Wood_02")));
	if (!TestTrue(
		TEXT("unique names project without an envelope"),
		TryBuildNamesOnlyPayload(
			{TEXT(" Sequence "), TEXT("Branch")},
			ENameComparison::IgnoreCase,
			PayloadJson,
			Error)))
	{
		AddError(Error);
		return false;
	}
	TSharedPtr<FJsonObject> Payload;
	TestTrue(
		TEXT("names-only payload parses"),
		UE::UAI::CommandContract::Json::TryParseObject(
			PayloadJson, Payload, Error));
	if (!Payload.IsValid())
	{
		return false;
	}
	const TArray<TSharedPtr<FJsonValue>>* Names = nullptr;
	TestTrue(
		TEXT("names is the only result field"),
		Payload->Values.Num() == 1
			&& Payload->TryGetArrayField(TEXT("names"), Names)
			&& Names != nullptr);
	if (Names == nullptr)
	{
		return false;
	}
	TestEqual(TEXT("both unique names remain"), Names->Num(), 2);
	TestEqual(
		TEXT("first spelling is preserved"),
		(*Names)[0]->AsString(),
		FString(TEXT("Sequence")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentBlueprintComponentEventCandidateResolverTest,
	"UeAgentInterface.Core.ContentDiscovery.BlueprintComponentEventCandidateResolver",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentBlueprintComponentEventCandidateResolverTest::RunTest(
	const FString& Parameters)
{
	const FName BlueprintName = MakeUniqueObjectName(
		GetTransientPackage(), UBlueprint::StaticClass(), TEXT("UAI_ComponentEventFixture"));
	UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(), GetTransientPackage(), BlueprintName,
		BPTYPE_Normal, UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass());
	if (!TestNotNull(TEXT("temporary Blueprint fixture is created"), Blueprint)
		|| !TestNotNull(TEXT("temporary Blueprint has an SCS"),
			Blueprint->SimpleConstructionScript.Get()))
	{
		return false;
	}
	USCS_Node* ComponentNode = Blueprint->SimpleConstructionScript->CreateNode(
		UAudioComponent::StaticClass(), FName(TEXT("EventSource")));
	Blueprint->SimpleConstructionScript->AddNode(ComponentNode);
	FKismetEditorUtilities::CompileBlueprint(Blueprint);

	TArray<FString> ComponentNames;
	FString Error;
	TestTrue(
		TEXT("component enumeration returns the SCS component name"),
		UE::UAI::DomainAdapters::BlueprintCommands::TryGetBlueprintComponentNames(
			*Blueprint, ComponentNames, Error));
	TestTrue(TEXT("EventSource is enumerated once"),
		ComponentNames.Contains(TEXT("EventSource")));

	UE::UAI::DomainAdapters::BlueprintCommands::FBlueprintComponentEventDiscoveryRequest Discovery;
	Discovery.ComponentName = TEXT("EventSource");
	Discovery.bCompileBeforeQuery = false;
	UE::UAI::DomainAdapters::BlueprintCommands::FBlueprintComponentEventDiscoveryResult DiscoveryResult;
	TestTrue(
		TEXT("shared discovery finds component events"),
		UE::UAI::DomainAdapters::BlueprintCommands::TryDiscoverComponentEvents(
			*Blueprint, Discovery, DiscoveryResult, Error));
	const auto* Event = DiscoveryResult.Events.FindByPredicate(
		[](const UE::UAI::DomainAdapters::BlueprintCommands::FBlueprintComponentEventRecord& Candidate)
		{
			return !Candidate.NativeFieldName.IsEmpty();
		});
	if (!TestNotNull(TEXT("fixture exposes an assignable component event"), Event))
	{
		return false;
	}

	UeAgentPaletteSearch::FSearchParams Search;
	Search.CatalogType = TEXT("blueprint_node");
	Search.AssetPath = Blueprint->GetPathName();
	Search.GraphName = Blueprint->UbergraphPages.IsEmpty()
		? FString() : Blueprint->UbergraphPages[0]->GetName();
	Search.RequiredAuthoringSelectors.Add(TEXT("component_name"), TEXT("EventSource"));
	UeAgentPaletteSearch::FCandidate Resolved;
	const bool bResolved = UeAgentPaletteSearch::TryResolveBlueprintActionCandidate(
		Search, Event->DisplayName, FString(), Resolved, Error);
	TestTrue(
		*FString::Printf(
			TEXT("component event resolves by readable name and component selector (%s)"),
			*Error),
		bResolved);
	TestEqual(TEXT("resolved candidate keeps component selector"),
		Resolved.AuthoringSelectors.FindRef(TEXT("component_name")),
		FString(TEXT("EventSource")));
	TestEqual(TEXT("resolved candidate keeps delegate property selector"),
		Resolved.AuthoringSelectors.FindRef(TEXT("delegate_property_name")),
		Event->NativeFieldName);
	TestFalse(
		TEXT("unknown component event name fails before authoring"),
		UeAgentPaletteSearch::TryResolveBlueprintActionCandidate(
			Search, TEXT("__UAI_UNKNOWN_COMPONENT_EVENT__"), FString(),
			Resolved, Error));

	UE::UAI::DomainAdapters::Blueprint::Node::FUeAgentBlueprintNodeCandidateQuery BroadQuery;
	BroadQuery.Target = Blueprint->GetPathName();
	BroadQuery.GraphName = Search.GraphName;
	BroadQuery.Query = TEXT("Tick");
	FString CandidatePayload;
	UE::UAI::DomainAdapters::Blueprint::Node::FUeAgentBlueprintNodeMechanicsError QueryError;
	TestTrue(
		TEXT("a Blueprint query returns readable names"),
		UE::UAI::DomainAdapters::Blueprint::Node::TryQueryBlueprintNodeCandidates(
			BroadQuery, CandidatePayload, QueryError));
	TestTrue(TEXT("the Blueprint result remains names-only"),
		CandidatePayload.Contains(TEXT("names")));

	UE::UAI::DomainAdapters::Blueprint::Node::FUeAgentBlueprintNodeCandidateQuery BeginPlayQuery;
	BeginPlayQuery.Target = Blueprint->GetPathName();
	BeginPlayQuery.GraphName = Search.GraphName;
	BeginPlayQuery.Query = TEXT("Begin Play");
	QueryError = {};
	TestTrue(
		TEXT("Begin Play discovery uses a stable semantic event name"),
		UE::UAI::DomainAdapters::Blueprint::Node::TryQueryBlueprintNodeCandidates(
			BeginPlayQuery, CandidatePayload, QueryError));
	TestTrue(
		TEXT("Begin Play discovery returns the readable semantic name"),
		CandidatePayload.Contains(TEXT("Begin Play")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentScopedCandidateNamesTest,
	"UeAgentInterface.Core.ContentDiscovery.NamesOnly.ScopedAliases",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentScopedCandidateNamesTest::RunTest(const FString& Parameters)
{
	const TArray<FScopedNameDiscoveryEntry> Entries = {
		{TEXT("Shared"), {TEXT("asset"), TEXT("/Game/Props")}, TEXT("native-fixture-1")},
		{TEXT("Shared"), {TEXT("actor"), TEXT("CubeActor")}, TEXT("native-fixture-2")},
		{TEXT("Unique"), {TEXT("actor"), TEXT("LightActor")}, TEXT("native-fixture-3")},
		{TEXT("Shared"), {TEXT("actor"), TEXT("SphereActor")}, TEXT("native-fixture-4")},
	};
	TArray<FString> Names;
	FString Error;
	TestTrue(
		TEXT("semantic scoped readable names build"),
		TryBuildScopedDiscoveryNames(
			Entries, ENameComparison::IgnoreCase, Names, Error));
	TestEqual(TEXT("names remain aligned"), Names.Num(), Entries.Num());
	if (Names.Num() != Entries.Num())
	{
		return false;
	}
	TestEqual(TEXT("kind resolves cross-kind collision"), Names[0],
		FString(TEXT("Shared [asset]")));
	TestEqual(TEXT("second semantic scope resolves same-kind collision"), Names[1],
		FString(TEXT("Shared [actor · CubeActor]")));
	TestEqual(TEXT("unique name remains unchanged"), Names[2],
		FString(TEXT("Unique")));
	int32 ResolvedIndex = INDEX_NONE;
	TestTrue(
		TEXT("returned alias resolves through the same owner"),
		TryResolveScopedDiscoveryName(
			Entries,
			TEXT("Shared [actor · SphereActor]"),
			ENameComparison::IgnoreCase,
			ResolvedIndex,
			Error));
	TestEqual(TEXT("alias resolves exact item"), ResolvedIndex, 3);
	const TArray<FScopedNameDiscoveryEntry> ReducedEntries = {Entries[3]};
	TestTrue(TEXT("previously emitted scope survives a smaller candidate set"),
		TryResolveScopedDiscoveryName(ReducedEntries, Names[3],
			ENameComparison::IgnoreCase, ResolvedIndex, Error));
	TestEqual(TEXT("old scoped alias resolves the remaining item"), ResolvedIndex, 0);
	TestFalse(TEXT("a mismatched scope is not ignored for a unique name"),
		TryResolveScopedDiscoveryName(ReducedEntries, TEXT("Shared [actor · CubeActor]"),
			ENameComparison::IgnoreCase, ResolvedIndex, Error));
	TestFalse(TEXT("a prefix that now selects multiple entries stays ambiguous"),
		TryResolveScopedDiscoveryName(Entries, TEXT("Shared [actor]"),
			ENameComparison::IgnoreCase, ResolvedIndex, Error));
	TestEqual(TEXT("scope ambiguity remains typed"), Error,
		FString(TEXT("name_discovery_semantic_scope_ambiguous")));
	const TArray<FScopedNameDiscoveryEntry> Ambiguous = {
		{TEXT("Same"), {TEXT("kind")}, TEXT("native-fixture-5")},
		{TEXT("Same"), {TEXT("kind")}, TEXT("native-fixture-6")},
	};
	TestFalse(TEXT("non-semantic duplicate fails closed"),
		TryBuildScopedDiscoveryNames(
			Ambiguous, ENameComparison::IgnoreCase, Names, Error));
	TestEqual(TEXT("ambiguous scope failure is typed"), Error,
		FString(TEXT("name_discovery_semantic_scope_ambiguous")));

	const TArray<FScopedNameDiscoveryEntry> FocusedEntries = {
		{TEXT("Focus"), {}, TEXT("native-fixture-7")},
		{FString(TEXT("Generated_"))
			+ FString::ChrN(24, TEXT('Q')) + TEXT("1234"), {}, TEXT("native-fixture-8")},
		{TEXT("Blocked"), {}, TEXT("native-fixture-9")},
		{TEXT("Blocked"), {}, TEXT("native-fixture-10")},
	};
	TestTrue(
		TEXT("focused projection ignores an unrelated unreadable entry"),
		TryBuildScopedDiscoveryNamesForIndices(
			FocusedEntries,
			{0},
			ENameComparison::IgnoreCase,
			Names,
			Error));
	TestEqual(TEXT("focused projection keeps the matched name"), Names[0],
		FString(TEXT("Focus")));
	int32 FocusedIndex = INDEX_NONE;
	TestTrue(
		TEXT("focused resolver ignores an unrelated ambiguous group"),
		TryResolveScopedDiscoveryName(
			FocusedEntries,
			TEXT("Focus"),
			ENameComparison::IgnoreCase,
			FocusedIndex,
			Error));
	TestEqual(TEXT("focused resolver returns the matched index"), FocusedIndex, 0);
	TestFalse(
		TEXT("a matched unreadable entry still fails closed"),
		TryBuildScopedDiscoveryNamesForIndices(
			FocusedEntries,
			{1},
			ENameComparison::IgnoreCase,
			Names,
			Error));
	TestEqual(TEXT("matched unreadable entry remains typed"), Error,
		FString(TEXT("name_discovery_entry_invalid")));
	TestFalse(
		TEXT("a matched ambiguous group still fails closed"),
		TryBuildScopedDiscoveryNamesForIndices(
			FocusedEntries,
			{2},
			ENameComparison::IgnoreCase,
			Names,
			Error));
	TestEqual(TEXT("matched ambiguity remains typed"), Error,
		FString(TEXT("name_discovery_semantic_scope_ambiguous")));
	const TArray<FScopedNameDiscoveryEntry> ReadableWordScopes = {
		{TEXT("Twin"), {TEXT("shared")}, TEXT("native-fixture-11") },
		{TEXT("Twin"), {TEXT("other")}, TEXT("native-fixture-12") },
	};
	TestTrue(
		TEXT("readable scope words are not mistaken for machine identity"),
		TryBuildScopedDiscoveryNames(
			ReadableWordScopes,
			ENameComparison::IgnoreCase,
			Names,
			Error));
	TestEqual(TEXT("shortest readable scope is used"), Names[0],
		FString(TEXT("Twin [shared]")));
	TestEqual(TEXT("second shortest readable scope is used"), Names[1],
		FString(TEXT("Twin [other]")));
	const TArray<FScopedNameDiscoveryEntry> MachineScopes = {
		{TEXT("Twin"), {TEXT("sha256:0123456789abcdef0123456789abcdef")}, TEXT("native-fixture-13") },
		{TEXT("Twin"), {TEXT("readable")}, TEXT("native-fixture-14") },
	};
	TestFalse(
		TEXT("explicit digest scope is rejected"),
		TryBuildScopedDiscoveryNames(
			MachineScopes,
			ENameComparison::IgnoreCase,
			Names,
			Error));

	const TArray<FScopedNameDiscoveryEntry> CompleteEntries = {
		{TEXT("Shared"), {TEXT("node"), TEXT("Sequence")}, TEXT("native-fixture-15")},
		{TEXT("Shared"), {TEXT("node"), TEXT("Selector")}, TEXT("native-fixture-16")},
		{TEXT("Other"), {TEXT("node"), TEXT("Other")}, TEXT("native-fixture-17")},
	};
	FString ProjectedPayload;
	TestTrue(
		TEXT("subset projection keeps alias scope from complete set"),
		TryBuildScopedNamesOnlyPayloadForIndices(
			CompleteEntries,
			{1},
			ENameComparison::IgnoreCase,
			ProjectedPayload,
			Error));
	TestEqual(
		TEXT("subset contains the stable complete-set alias"),
		ProjectedPayload,
		FString(TEXT("{\"names\":[\"Shared [node · Selector]\"]}")));

	TestFalse(
		TEXT("subset projection rejects an out-of-range index"),
		TryBuildScopedNamesOnlyPayloadForIndices(
			CompleteEntries,
			{3},
			ENameComparison::IgnoreCase,
			ProjectedPayload,
			Error));
	TestEqual(
		TEXT("out-of-range failure is typed"), Error,
		FString(TEXT("name_discovery_match_index_out_of_range")));
	TestTrue(
		TEXT("subset projection groups repeated matches of the same native entry"),
		TryBuildScopedNamesOnlyPayloadForIndices(
			CompleteEntries,
			{1, 1},
			ENameComparison::IgnoreCase,
			ProjectedPayload,
			Error));
	TestEqual(
		TEXT("repeated matches preserve one replayable name"), ProjectedPayload,
		FString(TEXT("{\"names\":[\"Shared [node · Selector]\"]}")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUeAgentPlainNameSelectionTest,
	"UeAgentInterface.Core.ContentDiscovery.PlainNameSelection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUeAgentPlainNameSelectionTest::RunTest(const FString& Parameters)
{
	using namespace UE::UAI::ContentDiscovery;
	TArray<FNameSelectionEntry> Entries{
		{TEXT("Rule"), TEXT("internal-a"), {{TEXT("form"), TEXT("expression")}, {TEXT("source"), TEXT("Alpha")}}},
		{TEXT("Rule"), TEXT("internal-a"), {{TEXT("form"), TEXT("expression")}, {TEXT("source"), TEXT("Alpha")}}}};
	int32 Index; FString Error, Ambiguity;
	TestTrue(TEXT("same native identity is deduplicated without changing the name"), TryResolveNameSelection(Entries, TEXT("Rule"), {}, Index, Error, Ambiguity));
	Entries.Add({TEXT("Rule"), TEXT("internal-b"), {{TEXT("form"), TEXT("function")}, {TEXT("source"), TEXT("Beta")}}});
	TestFalse(TEXT("a real same-name ambiguity requires selection"), TryResolveNameSelection(Entries, TEXT("Rule"), {}, Index, Error, Ambiguity));
	TestEqual(TEXT("ambiguity is typed"), Error, FString(TEXT("candidate_ambiguous")));
	TestTrue(TEXT("the lowest-cost one-field choice is published"),
		Ambiguity.Contains(TEXT("\"choose\""))
			&& Ambiguity.Contains(TEXT("\"source\""))
			&& Ambiguity.Contains(TEXT("Alpha"))
			&& Ambiguity.Contains(TEXT("Beta"))
			&& !Ambiguity.Contains(TEXT("internal-")));
	TestTrue(TEXT("select resolves the original name"), TryResolveNameSelection(Entries, TEXT("Rule"), {{TEXT("form"), TEXT("function")}}, Index, Error, Ambiguity));
	TestEqual(TEXT("selected native entry is exact"), Index, 2);
	TestFalse(TEXT("names are not fuzzy or scoped aliases"), TryResolveNameSelection(Entries, TEXT("Beta Rule"), {}, Index, Error, Ambiguity));
	TestFalse(TEXT("unknown selection is rejected"), TryResolveNameSelection(Entries, TEXT("Rule"), {{TEXT("module"), TEXT("Unknown")}}, Index, Error, Ambiguity));
	TestEqual(TEXT("expired selection is typed"), Error, FString(TEXT("candidate_selector_invalid")));
	TestTrue(TEXT("expired selection returns the current choice set"), Ambiguity.Contains(TEXT("\"source\"")));
	Entries[2].Select = Entries[0].Select;
	TestFalse(TEXT("indistinguishable identities cannot leak a machine fallback"), TryResolveNameSelection(Entries, TEXT("Rule"), {}, Index, Error, Ambiguity));
	TestEqual(TEXT("missing readable distinction is explicit"), Error, FString(TEXT("candidate_discriminator_missing")));

	Entries = {
		{TEXT("Pair"), TEXT("pair-a"), {{TEXT("form"), TEXT("expression")}, {TEXT("source"), TEXT("A")}}},
		{TEXT("Pair"), TEXT("pair-b"), {{TEXT("form"), TEXT("expression")}, {TEXT("source"), TEXT("B")}}},
		{TEXT("Pair"), TEXT("pair-c"), {{TEXT("form"), TEXT("function")}, {TEXT("source"), TEXT("A")}}},
		{TEXT("Pair"), TEXT("pair-d"), {{TEXT("form"), TEXT("function")}, {TEXT("source"), TEXT("B")}}}};
	TestFalse(TEXT("two fields are used only when one field cannot distinguish every identity"), TryResolveNameSelection(Entries, TEXT("Pair"), {}, Index, Error, Ambiguity));
	TestTrue(TEXT("two-field choices preserve valid tuples"), Ambiguity.Contains(TEXT("\"choose\":[")) && Ambiguity.Contains(TEXT("\"form\"")) && Ambiguity.Contains(TEXT("\"source\"")));
	TestTrue(TEXT("a two-field tuple replays unchanged"), TryResolveNameSelection(Entries, TEXT("Pair"), {{TEXT("form"), TEXT("function")}, {TEXT("source"), TEXT("B")}}, Index, Error, Ambiguity));
	TestEqual(TEXT("the tuple selects its native identity"), Index, 3);
	Entries.RemoveAt(1);
	TestFalse(TEXT("partial selection still needs a complete independently replayable choice"),
		TryResolveNameSelection(Entries, TEXT("Pair"), {{TEXT("form"), TEXT("function")}}, Index, Error, Ambiguity));
	TSharedPtr<FJsonObject> PartialPayload;
	FString JsonError;
	const TArray<TSharedPtr<FJsonValue>>* Choices = nullptr;
	if (!TestTrue(TEXT("partial ambiguity returns complete two-field tuples"),
		UE::UAI::CommandContract::Json::TryParseObject(Ambiguity, PartialPayload, JsonError)
			&& PartialPayload->TryGetArrayField(TEXT("choose"), Choices) && Choices && Choices->Num() == 2)) return false;
	for (const auto& Choice : *Choices)
	{
		TMap<FString, FString> Retry;
		for (const auto& Field : Choice->AsObject()->Values) Retry.Add(Field.Key, Field.Value->AsString());
		TestTrue(TEXT("each returned choice alone resolves in the full original group"),
			TryResolveNameSelection(Entries, TEXT("Pair"), Retry, Index, Error, Ambiguity));
		TestTrue(TEXT("retry selects the intended partial group identity"), Index == 1 || Index == 2);
	}
	Entries = {{TEXT("Only"), TEXT("only"), {}}};
	TestFalse(TEXT("expired select on a unique name is rejected"),
		TryResolveNameSelection(Entries, TEXT("Only"), {{TEXT("form"), TEXT("removed")}}, Index, Error, Ambiguity));
	TestEqual(TEXT("unique-name stale selector is not a discriminator gap"), Error, FString(TEXT("candidate_selector_invalid")));
	TestTrue(TEXT("unique recovery retains its name and typed code without invented choices"),
		UE::UAI::CommandContract::Json::TryParseObject(Ambiguity, PartialPayload, JsonError)
			&& PartialPayload->GetStringField(TEXT("name")) == TEXT("Only")
			&& PartialPayload->GetStringField(TEXT("code")) == Error && PartialPayload->Values.Num() == 2);
	TestTrue(TEXT("unique current name retries without stale select"),
		TryResolveNameSelection(Entries, TEXT("Only"), {}, Index, Error, Ambiguity));
	Entries = {
		{TEXT("Legal"), TEXT("left"), {{TEXT("id"), TEXT("A")}, {TEXT("source"), TEXT("Alpha")}}},
		{TEXT("Legal"), TEXT("right"), {{TEXT("id"), TEXT("B")}, {TEXT("source"), TEXT("Beta")}}}};
	TestFalse(TEXT("a cheaper forbidden key cannot hide a valid semantic discriminator"),
		TryResolveNameSelection(Entries, TEXT("Legal"), {}, Index, Error, Ambiguity));
	TestEqual(TEXT("legal combinations are considered before cost selection"), Error, FString(TEXT("candidate_ambiguous")));
	TestTrue(TEXT("only the legal selector is published"), Ambiguity.Contains(TEXT("\"source\"")) && !Ambiguity.Contains(TEXT("\"id\"")));
	Entries[0].Select = {{TEXT("form"), TEXT("/A")}, {TEXT("source"), TEXT("Alpha")}};
	Entries[1].Select = {{TEXT("form"), TEXT("/B")}, {TEXT("source"), TEXT("Beta")}};
	TestFalse(TEXT("unreadable cheap values cannot hide readable choices"),
		TryResolveNameSelection(Entries, TEXT("Legal"), {}, Index, Error, Ambiguity));
	TestTrue(TEXT("readability participates in combination selection"), Error == TEXT("candidate_ambiguous") && Ambiguity.Contains(TEXT("\"source\"")));

	Entries.Reset();
	for ( int32 Choice = 0; Choice < 9; ++Choice)
	{
		Entries.Add({TEXT("Bounded"), FString::Printf(TEXT("identity-%d"), Choice), {{TEXT("form"), FString::Printf(TEXT("choice-%d"), Choice)}}});
	}
	TestFalse(TEXT("more than eight ambiguity choices fail closed"), TryResolveNameSelection(Entries, TEXT("Bounded"), {}, Index, Error, Ambiguity));
	TestEqual(TEXT("the choice bound reports a discriminator gap"), Error, FString(TEXT("candidate_discriminator_missing")));

	Entries = {
		{TEXT("Rule"), TEXT("internal-a"), {{TEXT("form"), TEXT("Alpha")}}},
		{TEXT("Rule"), TEXT("internal-b"), {{TEXT("form"), TEXT("Beta")}}}};
	for (const FString& Key : {FString(TEXT("id")), FString(TEXT("class_path")), FString::ChrN(65, TEXT('k'))})
	{
		Entries[0].Select = {{Key, TEXT("Alpha")}}; Entries[1].Select = {{Key, TEXT("Beta")}};
		TestFalse(TEXT("output selectors obey the formal DomainKit key grammar"), TryResolveNameSelection(Entries, TEXT("Rule"), {}, Index, Error, Ambiguity));
		TestEqual(TEXT("invalid output selector is not published"), Error, FString(TEXT("candidate_discriminator_missing")));
	}
	Entries[0].Select = {{TEXT("form"), TEXT(" Alpha ")}}; Entries[1].Select = {{TEXT("form"), TEXT("Beta")}};
	TestFalse(TEXT("normalization cannot change an emitted selector before replay"), TryResolveNameSelection(Entries, TEXT("Rule"), {}, Index, Error, Ambiguity));
	TestEqual(TEXT("unreplayable whitespace is rejected"), Error, FString(TEXT("candidate_discriminator_missing")));
	Entries[0].Select = {{TEXT("form"), TEXT("/Script/Hidden.MachineClass")}}; Entries[1].Select = {{TEXT("form"), TEXT("Beta")}};
	TestFalse(TEXT("syntax-valid machine paths are not public ambiguity values"), TryResolveNameSelection(Entries, TEXT("Rule"), {}, Index, Error, Ambiguity));
	TestEqual(TEXT("machine identity stays private"), Error, FString(TEXT("candidate_discriminator_missing")));
	return true;
}

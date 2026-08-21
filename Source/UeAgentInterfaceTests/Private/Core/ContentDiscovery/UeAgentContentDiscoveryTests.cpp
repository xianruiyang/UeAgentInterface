#include "Misc/AutomationTest.h"

#include "Core/ContentDiscovery/UeAgentContentDiscovery.h"

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
			const int32 Offset,
			const int32 Limit,
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
				Item.DisplayName = Identity;
				Item.Domain = TEXT("fixture");
				Item.ContentKind = Kinds[0];
				Item.ClassPath = TEXT("/Script/Fixture.Object");
				Item.OwnerIdentity = TEXT("fixture-owner");
				Item.StateLayer = TEXT("fixture-current");
				Item.Revision = Revision;
				Item.DetailCommandId = TEXT("fixture.read");
				Item.MatchReason = TEXT("fixture");
				Item.SupportedOperations = {TEXT("query")};
				if (MatchesContentDiscoveryFilters(Request, Item))
				{
					Items.Add(MoveTemp(Item));
				}
			}
			if (Offset < 0 || Offset > Items.Num())
			{
				OutError.Code = TEXT("fixture_cursor_invalid");
				return false;
			}
			const int32 End = FMath::Min(Offset + Limit, Items.Num());
			OutPage.ProviderId = ProviderId;
			OutPage.Revision = Revision;
			OutPage.TotalMatches = Items.Num();
			OutPage.NextOffset = End < Items.Num() ? End : -1;
			OutPage.bComplete = OutPage.NextOffset == -1;
			OutPage.bReadOnly = true;
			for (int32 Index = Offset; Index < End; ++Index)
			{
				OutPage.Items.Add(MoveTemp(Items[Index]));
			}
			return true;
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
		Result.LimitPerProvider = 1;
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
	FContentDiscoverySearchResult First;
	TestTrue(
		TEXT("first page searches every selected provider"),
		Service->TrySearch(Search, First, Error));
	TestEqual(TEXT("two provider states returned"), First.Providers.Num(), 2);
	TestTrue(TEXT("first page truthfully reports paging"), First.HasMore());
	TestFalse(TEXT("first page is not complete"), First.IsComplete());
	TestEqual(
		TEXT("request fingerprint is stable"),
		First.RequestFingerprint,
		Search.GetRequestFingerprint());

	Search.Cursor.RequestFingerprint = First.RequestFingerprint;
	for (const FContentDiscoveryProviderResult& Provider : First.Providers)
	{
		TestTrue(TEXT("provider page succeeded"), Provider.Page.IsSet());
		if (Provider.Page.IsSet())
		{
			Search.Cursor.Providers.Add({
				Provider.ProviderId,
				Provider.Page->Revision,
				Provider.Page->NextOffset,
			});
		}
	}
	FContentDiscoverySearchResult Second;
	TestTrue(
		TEXT("copied cursor returns the second stable page"),
		Service->TrySearch(Search, Second, Error));
	TestTrue(TEXT("second page is complete"), Second.IsComplete());
	for (const FContentDiscoveryProviderResult& Provider : Second.Providers)
	{
		TestTrue(TEXT("second provider page exists"), Provider.Page.IsSet());
		if (Provider.Page.IsSet())
		{
			TestEqual(TEXT("one item per provider"), Provider.Page->Items.Num(), 1);
			TestEqual(TEXT("provider page complete"), Provider.Page->NextOffset, -1);
		}
	}

	Search.Query = TEXT("different");
	TestFalse(
		TEXT("a cursor cannot be reused with changed search semantics"),
		Service->TrySearch(Search, Second, Error));
	TestEqual(
		TEXT("cursor mismatch is exact"),
		Error.Code,
		FString(TEXT("content_search_cursor_request_mismatch")));
	return true;
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

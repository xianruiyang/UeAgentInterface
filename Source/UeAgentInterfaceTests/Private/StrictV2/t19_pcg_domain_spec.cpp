#include "Misc/AutomationTest.h"

#include "Core/HjsonWorkspace/ImportInitialization/UeAgentHjsonDomainProviderRegistry.h"
#include "HAL/FileManager.h"
#include "Misc/PackageName.h"
#include "PCGGraph.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

#if __has_include("OperationOwners/Pcg/UeAgentPcgDomainService.h")
#define UAI_T19_PCG_DOMAIN_AVAILABLE 1
#include "OperationOwners/Pcg/UeAgentPcgDomainService.h"
#else
#define UAI_T19_PCG_DOMAIN_AVAILABLE 0
#endif

#if __has_include("QueryOwners/Pcg/UeAgentPcgNodeCandidateQuery.h")
#define UAI_T19_PCG_CANDIDATE_QUERY_AVAILABLE 1
#include "QueryOwners/Pcg/UeAgentPcgNodeCandidateQuery.h"
#else
#define UAI_T19_PCG_CANDIDATE_QUERY_AVAILABLE 0
#endif

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T19PcgDomainTest,
	"UAI.StrictV2.T19.PCG.DOMAIN",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T19PcgDomainTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
#if !UAI_T19_PCG_DOMAIN_AVAILABLE
	TestTrue(TEXT("T19 PCG domain authority is available"), false);
	return false;
#elif !UAI_T19_PCG_CANDIDATE_QUERY_AVAILABLE
	TestTrue(TEXT("T19 PCG node candidate query is available"), false);
	return false;
#else
	using namespace UE::UAI::HjsonWorkspace;
	using namespace UE::UAI::OperationOwners::Pcg;

	TSharedPtr<IImportInitializationDomainProvider> Provider;
	FWorkspaceError Error;
	if (!TestTrue(
		TEXT("the PCG domain provider is created"),
		TryCreatePcgDomainProvider(Provider, Error))
		|| !Provider.IsValid())
	{
		AddError(Error.GetCode());
		return false;
	}

	struct FCase
	{
		const TCHAR* Type;
		EInitializationNodeRole Role;
		const TCHAR* Contract;
	};
	const TArray<FCase> Cases = {
		{TEXT("/Script/PCG.PCGGraph"), EInitializationNodeRole::RootTarget, TEXT("uai.init.pcg_graph.root/1")},
		{TEXT("/Script/PCG.PCGNode"), EInitializationNodeRole::OwnedObject, TEXT("uai.init.pcg.node/1")},
		{TEXT("uai.pcg.endpoint"), EInitializationNodeRole::ContainerItem, TEXT("uai.init.pcg.endpoint/1")},
		{TEXT("uai.pcg.parameter"), EInitializationNodeRole::ContainerItem, TEXT("uai.init.pcg.parameter/1")},
	};
	for (const FCase& Case : Cases)
	{
		const FImportInitializationDomainRequest Request(
			TEXT("content_graph.import_workspace"),
			TEXT("uai.pcg.edit/1"),
			TEXT("/Game/UAI/StrictV2/T19/Target.Target"),
			Case.Type,
			Case.Role,
			TEXT("{}"));
		FString Contract;
		TestTrue(
			*FString::Printf(TEXT("the PCG initialization surface resolves: %s"), Case.Type),
			Provider->Matches(Request)
				&& Provider->TryResolveInitializationContract(Request, Contract, Error));
		TestEqual(TEXT("the PCG initialization contract is exact"), Contract,
			FString(Case.Contract));
	}

	const FImportInitializationDomainRequest Unsupported(
		TEXT("content_graph.import_workspace"),
		TEXT("uai.pcg.edit/1"),
		TEXT("/Game/UAI/StrictV2/T19/Unsupported.Unsupported"),
		TEXT("/Script/Engine.SoundWave"),
		EInitializationNodeRole::RootTarget,
		TEXT("{}"));
	TestFalse(TEXT("an unregistered PCG root is rejected before write"),
		Provider->Matches(Unsupported));

	using namespace UE::UAI::QueryOwners::Pcg;
	UPCGGraph* CandidateTarget = NewObject<UPCGGraph>(GetTransientPackage());
	FPcgNodeCandidateQueryResult CandidateResult;
	FPcgNodeCandidateQueryError CandidateError;
	if (!TestTrue(
		TEXT("the PCG owner resolves real node candidates"),
		TryQueryPcgNodeCandidates(
			*CandidateTarget,
			TEXT("Point"),
			CandidateResult,
			CandidateError)))
	{
		AddError(CandidateError.Code + TEXT(":") + CandidateError.Detail);
		return false;
	}
	if (!TestTrue(TEXT("the real PCG node catalog is non-empty"),
		!CandidateResult.Candidates.IsEmpty()))
	{
		return false;
	}
	const FPcgNodeCandidate& Candidate = CandidateResult.Candidates[0];
	TestTrue(TEXT("candidate.name is an exact settings class identity"),
		Candidate.Name.StartsWith(TEXT("/Script/")));
	TestEqual(TEXT("candidate kind is explicit"), Candidate.Kind,
		FString(TEXT("pcg_node")));
	TestTrue(TEXT("candidate declares create support"),
		Candidate.SupportedOperations.Contains(TEXT("create")));
	TestFalse(TEXT("candidate input contract is present"),
		Candidate.InputContract.IsEmpty());
	TestFalse(TEXT("candidate placement contract is present"),
		Candidate.PlacementContract.IsEmpty());
	TestFalse(TEXT("candidate required context is present"),
		Candidate.RequiredContext.IsEmpty());
	TestTrue(TEXT("candidate binding revision is a sha256 identity"),
		Candidate.BindingRevision.StartsWith(TEXT("sha256:")));
	TestTrue(TEXT("candidate query reports complete bounded results"),
		CandidateResult.bComplete && !CandidateResult.bHasMore);

	const FString FixturePackageName =
		TEXT("/Game/UAI/StrictV2/T19R63/PG_T19PublicR63");
	const FString FixtureAssetName = TEXT("PG_T19PublicR63");
	const FString FixtureFilename = FPackageName::LongPackageNameToFilename(
		FixturePackageName,
		FPackageName::GetAssetPackageExtension());
	if (IFileManager::Get().FileExists(*FixtureFilename))
	{
		TestTrue(
			TEXT("a stale task-owned PCG fixture is removed before setup"),
			IFileManager::Get().Delete(*FixtureFilename, false, true));
	}
	UPackage* const FixturePackage = CreatePackage(*FixturePackageName);
	UPCGGraph* const FixtureGraph = FixturePackage
		? NewObject<UPCGGraph>(
			FixturePackage,
			*FixtureAssetName,
			RF_Public | RF_Standalone | RF_Transactional)
		: nullptr;
	if (!TestNotNull(TEXT("the task-owned PCG fixture is created"), FixtureGraph))
	{
		return false;
	}
	FixtureGraph->SetFlags(RF_Public | RF_Standalone);
	FixturePackage->MarkPackageDirty();
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	TestTrue(
		TEXT("the task-owned PCG fixture is persisted for the public vertical"),
		UPackage::SavePackage(
			FixturePackage,
			FixtureGraph,
			*FixtureFilename,
			SaveArgs));
	FixturePackage->SetDirtyFlag(false);
	return !HasAnyErrors();
#endif
}

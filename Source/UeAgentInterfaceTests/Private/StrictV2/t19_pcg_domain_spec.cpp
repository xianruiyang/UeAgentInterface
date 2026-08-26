#include "Misc/AutomationTest.h"

#include "Core/HjsonWorkspace/ImportInitialization/UeAgentHjsonDomainProviderRegistry.h"
#include "HAL/FileManager.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "ObjectTools.h"
#include "PCGGraph.h"
#include "PCGSettings.h"
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

namespace
{
	void DeleteAssetIfPresent(const FString& ObjectPath)
	{
		if (UObject* Existing = StaticLoadObject(
			UObject::StaticClass(), nullptr, *ObjectPath))
		{
			ObjectTools::DeleteObjectsUnchecked({Existing});
		}
	}
}

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
	UClass* const ExpectedSettingsClass = LoadObject<UClass>(
		nullptr, TEXT("/Script/PCG.PCGCreatePointsGridSettings"));
	const UPCGSettings* const ExpectedSettings = ExpectedSettingsClass
		? Cast<UPCGSettings>(ExpectedSettingsClass->GetDefaultObject()) : nullptr;
	if (!TestNotNull(
		TEXT("the representative native PCG settings class resolves"),
		ExpectedSettings))
	{
		return false;
	}
	const FString ExpectedMenuName =
		ExpectedSettings->GetDefaultNodeTitle().ToString();
	FPcgNodeCandidateQueryResult CandidateResult;
	FPcgNodeCandidateQueryError CandidateError;
	if (!TestTrue(
		TEXT("the PCG owner resolves real node candidates"),
		TryQueryPcgNodeCandidates(
			*CandidateTarget,
			ExpectedMenuName,
			CandidateResult,
			CandidateError,
			64,
			0)))
	{
		AddError(CandidateError.Code + TEXT(":") + CandidateError.Detail);
		return false;
	}
	if (!TestTrue(TEXT("the real PCG node catalog is non-empty"),
		!CandidateResult.Candidates.IsEmpty()))
	{
		return false;
	}
	const FPcgNodeCandidate* const CandidatePtr =
		CandidateResult.Candidates.FindByPredicate(
			[&ExpectedMenuName](const FPcgNodeCandidate& Value)
			{
				return Value.bSupported
					&& Value.Name.Equals(
						ExpectedMenuName, ESearchCase::CaseSensitive);
			});
	if (!TestNotNull(
		TEXT("the exact native Create Points Grid action is returned"),
		CandidatePtr))
	{
		return false;
	}
	const FPcgNodeCandidate& Candidate = *CandidatePtr;
	TestTrue(TEXT("candidate.id is a compact exact native-action identity"),
		Candidate.Id.StartsWith(TEXT("candidate:")));
	TestFalse(TEXT("candidate.name is the native menu label, not a class path"),
		Candidate.Name.StartsWith(TEXT("/Script/")));
	TestEqual(TEXT("candidate kind is explicit"), Candidate.Kind,
		FString(TEXT("pcg_node")));
	TestEqual(TEXT("the exact result is a verified native schema action"),
		Candidate.ActionKind,
		FString(TEXT("FPCGEditorGraphSchemaAction_NewNativeElement")));
	TestTrue(TEXT("the first PCG native-action equivalence class is executable"),
		Candidate.bSupported);
	TestTrue(TEXT("the exact filtered page is complete within its bound"),
		CandidateResult.bComplete && !CandidateResult.bHasMore);
	TestTrue(TEXT("enumeration covers the native PCG context-menu owner"),
		CandidateResult.bCoverageComplete
			&& CandidateResult.NativeActionCount > 0);
	TestFalse(TEXT("search coverage and creation coverage are reported separately"),
		CandidateResult.bCreationCoverageComplete);
	FPcgNodeCandidate ResolvedCandidate;
	CandidateError = {};
	TestTrue(TEXT("the compact id re-enumerates one exact native action"),
		TryResolvePcgNodeCandidate(
			*CandidateTarget, FString(), Candidate.Id,
			ResolvedCandidate, CandidateError));
	TestEqual(TEXT("id resolution is conjunctively exact"),
		ResolvedCandidate.Id, Candidate.Id);

	const FString FixturePackageName =
		TEXT("/Game/UAI/StrictV2/T19R63/PG_T19PublicR63");
	const FString FixtureAssetName = TEXT("PG_T19PublicR63");
	const FString FixtureObjectPath =
		FixturePackageName + TEXT(".") + FixtureAssetName;
	const FString FixtureFilename = FPackageName::LongPackageNameToFilename(
		FixturePackageName,
		FPackageName::GetAssetPackageExtension());
	DeleteAssetIfPresent(FixtureObjectPath);
	if (IFileManager::Get().FileExists(*FixtureFilename))
	{
		TestTrue(
			TEXT("a stale task-owned PCG fixture is removed before setup"),
			IFileManager::Get().Delete(*FixtureFilename, false, true));
	}
	ON_SCOPE_EXIT
	{
		DeleteAssetIfPresent(FixtureObjectPath);
	};
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
	DeleteAssetIfPresent(FixtureObjectPath);
	TestFalse(TEXT("the task-owned PCG fixture leaves no asset file"),
		IFileManager::Get().FileExists(*FixtureFilename));
	return !HasAnyErrors();
#endif
}

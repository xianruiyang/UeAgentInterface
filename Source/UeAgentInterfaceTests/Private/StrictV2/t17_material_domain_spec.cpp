#include "Misc/AutomationTest.h"

#include "Core/HjsonWorkspace/ImportInitialization/UeAgentHjsonDomainProviderRegistry.h"
#include "DomainAdapters/ContentGraphRead/UeAgentContentGraphReadProductionEnvironment.h"
#include "Materials/Material.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialInstanceConstant.h"

#if __has_include("OperationOwners/Material/UeAgentMaterialDomainService.h")
#define UAI_T17_MATERIAL_DOMAIN_AVAILABLE 1
#include "OperationOwners/Material/UeAgentMaterialDomainService.h"
#else
#define UAI_T17_MATERIAL_DOMAIN_AVAILABLE 0
#endif

#if __has_include("QueryOwners/Material/UeAgentMaterialExpressionCandidateQuery.h")
#define UAI_T17_MATERIAL_CANDIDATE_QUERY_AVAILABLE 1
#include "QueryOwners/Material/UeAgentMaterialExpressionCandidateQuery.h"
#else
#define UAI_T17_MATERIAL_CANDIDATE_QUERY_AVAILABLE 0
#endif

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T17MaterialDomainTest,
	"UAI.StrictV2.T17.MATERIAL.DOMAIN",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T17MaterialDomainTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
#if !UAI_T17_MATERIAL_DOMAIN_AVAILABLE
	TestTrue(TEXT("T17 Material domain authority is available"), false);
	return false;
#elif !UAI_T17_MATERIAL_CANDIDATE_QUERY_AVAILABLE
	TestTrue(TEXT("T17 Material expression candidate query is available"), false);
	return false;
#else
	using namespace UE::UAI::HjsonWorkspace;
	using namespace UE::UAI::OperationOwners::Material;

	TSharedPtr<IImportInitializationDomainProvider> Provider;
	FWorkspaceError Error;
	if (!TestTrue(
		TEXT("the Material domain provider is created"),
		TryCreateMaterialDomainProvider(Provider, Error))
		|| !Provider.IsValid())
	{
		AddError(Error.GetCode());
		return false;
	}

	struct FCase
	{
		const TCHAR* RootType;
		const TCHAR* Contract;
	};
	const TArray<FCase> RootCases = {
		{TEXT("/Script/Engine.Material"), TEXT("uai.init.material.root/1")},
		{TEXT("/Script/Engine.MaterialFunction"), TEXT("uai.init.material_function.root/1")},
		{TEXT("/Script/Engine.MaterialInstanceConstant"), TEXT("uai.init.material_instance.root/1")},
	};
	for (const FCase& Case : RootCases)
	{
		const FImportInitializationDomainRequest Request(
			TEXT("content_graph.import_workspace"),
			TEXT("uai.material.edit/1"),
			TEXT("/Game/UAI/StrictV2/T17/Target.Target"),
			Case.RootType,
			EInitializationNodeRole::RootTarget,
			TEXT("{}"));
		FString Contract;
		TestTrue(
			*FString::Printf(TEXT("the root type resolves: %s"), Case.RootType),
			Provider->Matches(Request)
				&& Provider->TryResolveInitializationContract(
					Request, Contract, Error));
		TestEqual(TEXT("the root initialization contract is exact"), Contract,
			FString(Case.Contract));
	}

	const FImportInitializationDomainRequest ExpressionRequest(
		TEXT("content_graph.import_workspace"),
		TEXT("uai.material.edit/1"),
		TEXT("/Game/UAI/StrictV2/T17/Target.Target#expression"),
		TEXT("/Script/Engine.MaterialExpression"),
		EInitializationNodeRole::OwnedObject,
		TEXT("{}"));
	FString ExpressionContract;
	TestTrue(
		TEXT("a Material expression consumes the common owned-child seam"),
		Provider->Matches(ExpressionRequest)
			&& Provider->TryResolveInitializationContract(
				ExpressionRequest, ExpressionContract, Error));
	TestEqual(
		TEXT("the expression initialization contract is exact"),
		ExpressionContract,
		FString(TEXT("uai.init.material.expression/1")));

	const FImportInitializationDomainRequest Unsupported(
		TEXT("content_graph.import_workspace"),
		TEXT("uai.material.edit/1"),
		TEXT("/Game/UAI/StrictV2/T17/Unsupported.Unsupported"),
		TEXT("/Script/Engine.SoundWave"),
		EInitializationNodeRole::RootTarget,
		TEXT("{}"));
	TestFalse(
		TEXT("an unregistered root type is rejected before write"),
		Provider->Matches(Unsupported));

	using namespace UE::UAI::DomainAdapters::ContentGraphRead;
	for (UObject* MaterialRoot : TArray<UObject*>({
		NewObject<UMaterialFunction>(GetTransientPackage()),
		NewObject<UMaterialInstanceConstant>(GetTransientPackage()),
	}))
	{
		FUeAgentContentGraphCapture Capture;
		FUeAgentContentGraphReadError ReadError;
		TestTrue(
			*FString::Printf(
				TEXT("the common readback captures %s"),
				*MaterialRoot->GetClass()->GetPathName()),
			FUeAgentContentGraphReadProductionEnvironment::TryCaptureOwner(
				*MaterialRoot, Capture, ReadError));
		TestEqual(
			TEXT("all declared Material roots use the common Material readback domain"),
			Capture.DomainId,
			FString(TEXT("material")));
	}

	using namespace UE::UAI::QueryOwners::Material;
	UMaterial* CandidateTarget = NewObject<UMaterial>(GetTransientPackage());
	FMaterialExpressionCandidateQueryResult CandidateResult;
	FMaterialExpressionCandidateQueryError CandidateError;
	if (!TestTrue(
		TEXT("the Material owner resolves real expression candidates"),
		TryQueryMaterialExpressionCandidates(
			*CandidateTarget,
			TEXT("Scalar Parameter"),
			CandidateResult,
			CandidateError)))
	{
		AddError(CandidateError.Code + TEXT(":") + CandidateError.Detail);
		return false;
	}
	const FMaterialExpressionCandidate* ScalarParameter =
		CandidateResult.Candidates.FindByPredicate(
			[](const FMaterialExpressionCandidate& Candidate)
			{
				return Candidate.Name
					== TEXT("/Script/Engine.MaterialExpressionScalarParameter");
			});
	if (!TestNotNull(
		TEXT("candidate.name is the exact HJSON creation identity"),
		ScalarParameter))
	{
		return false;
	}
	TestEqual(TEXT("candidate kind is explicit"), ScalarParameter->Kind,
		FString(TEXT("material_expression")));
	TestTrue(TEXT("candidate declares create support"),
		ScalarParameter->SupportedOperations.Contains(TEXT("create")));
	TestFalse(TEXT("candidate input contract is present"),
		ScalarParameter->InputContract.IsEmpty());
	TestFalse(TEXT("candidate placement contract is present"),
		ScalarParameter->PlacementContract.IsEmpty());
	TestFalse(TEXT("candidate required context is present"),
		ScalarParameter->RequiredContext.IsEmpty());
	TestTrue(TEXT("candidate binding revision is a sha256 identity"),
		ScalarParameter->BindingRevision.StartsWith(TEXT("sha256:")));
	TestTrue(TEXT("candidate query reports complete bounded results"),
		CandidateResult.bComplete && !CandidateResult.bHasMore);
	return true;
#endif
}

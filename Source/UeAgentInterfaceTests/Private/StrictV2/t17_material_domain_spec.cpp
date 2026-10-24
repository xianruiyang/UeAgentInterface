#include "Misc/AutomationTest.h"

#include "Core/HjsonWorkspace/ImportInitialization/UeAgentHjsonDomainProviderRegistry.h"
#include "Core/HjsonWorkspace/UeAgentSemanticLayout.h"
#include "DomainAdapters/ContentGraphRead/UeAgentContentGraphReadProductionEnvironment.h"
#include "Materials/Material.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialInstanceConstant.h"

#include "OperationOwners/Material/UeAgentMaterialDomainService.h"

#include "QueryOwners/Material/UeAgentMaterialExpressionCandidateQuery.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T17MaterialDomainTest,
	"UAI.StrictV2.T17.MATERIAL.DOMAIN",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T17MaterialDomainTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
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

	const FImportInitializationDomainRequest SemanticWorkspaceRequest(
		TEXT("content_graph.import_workspace"),
		GetSemanticWorkspaceSchemaId(),
		TEXT("/Game/UAI/StrictV2/T17/Target.Target"),
		TEXT("/Script/Engine.Material"),
		EInitializationNodeRole::RootTarget,
		TEXT("{}"));
	FString SemanticWorkspaceContract;
	TestTrue(
		TEXT("the Material provider accepts the shared exported workspace schema"),
		Provider->Matches(SemanticWorkspaceRequest)
			&& Provider->TryResolveInitializationContract(
				SemanticWorkspaceRequest, SemanticWorkspaceContract, Error));
	TestEqual(
		TEXT("the exported workspace keeps the Material root contract"),
		SemanticWorkspaceContract,
		FString(TEXT("uai.init.material.root/1")));

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
			TEXT("MaterialExpressionScalarParameter"),
			CandidateResult,
			CandidateError)))
	{
		AddError(CandidateError.Code + TEXT(":") + CandidateError.PublicMessage);
		return false;
	}
	const FMaterialExpressionCandidate* ScalarParameter =
		CandidateResult.Candidates.FindByPredicate(
			[](const FMaterialExpressionCandidate& Candidate)
			{
				return Candidate.Kind == TEXT("material_expression");
			});
	if (!TestNotNull(
		TEXT("the class-narrowed native Material action is returned"),
		ScalarParameter))
	{
		return false;
	}
	TestTrue(TEXT("candidate.name is a non-empty native action label"),
		!ScalarParameter->Name.IsEmpty()
			&& !ScalarParameter->Name.StartsWith(TEXT("/Script/")));
	TestTrue(TEXT("candidate.id is the compact stable selector"),
		ScalarParameter->Id.StartsWith(TEXT("candidate:")));
	TestEqual(TEXT("candidate kind is explicit"), ScalarParameter->Kind,
		FString(TEXT("material_expression")));
	TestTrue(TEXT("candidate is supported in a persistent Material context"),
		ScalarParameter->bSupported);
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

	TSet<FString> ConversionNames;
	for (const FString& ActionName : TArray<FString>{
		TEXT("Make Float2"), TEXT("Make Float3"), TEXT("Make Float4")})
	{
		FMaterialExpressionCandidateQueryResult ConversionResult;
		CandidateError = {};
		if (!TestTrue(
			*FString::Printf(
				TEXT("the public candidate owner enumerates %s"), *ActionName),
			TryQueryMaterialExpressionCandidates(
				*CandidateTarget,
				ActionName,
				ConversionResult,
				CandidateError)))
		{
			AddError(CandidateError.Code + TEXT(":") + CandidateError.PublicMessage);
			return false;
		}
		const FMaterialExpressionCandidate* const Conversion =
			ConversionResult.Candidates.FindByPredicate(
				[&ActionName](const FMaterialExpressionCandidate& Candidate)
				{
					FString CompactCandidate = Candidate.Name;
					FString CompactAction = ActionName;
					CompactCandidate.ReplaceInline(TEXT(" "), TEXT(""));
					CompactAction.ReplaceInline(TEXT(" "), TEXT(""));
					return CompactCandidate.Equals(
							CompactAction, ESearchCase::IgnoreCase)
						|| CompactCandidate.StartsWith(
							CompactAction + TEXT("["), ESearchCase::IgnoreCase);
				});
		if (!TestNotNull(
			*FString::Printf(TEXT("%s retains its native action identity"),
				*ActionName),
			Conversion))
		{
			TArray<FString> ReturnedNames;
			for (const FMaterialExpressionCandidate& Candidate :
				ConversionResult.Candidates)
			{
				if (ReturnedNames.Num() == 5) break;
				ReturnedNames.Add(Candidate.Name);
			}
			AddError(TEXT("returned readable names: ")
				+ FString::Join(ReturnedNames, TEXT(", ")));
			return false;
		}
		ConversionNames.Add(Conversion->Name);
	}
	TestEqual(TEXT("same-class native actions retain distinct readable names"),
		ConversionNames.Num(), 3);
	return true;
}

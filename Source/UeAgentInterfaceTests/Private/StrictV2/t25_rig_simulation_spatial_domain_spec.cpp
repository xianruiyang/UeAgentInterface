#include "Misc/AutomationTest.h"

#if __has_include("OperationOwners/RigSimulationSpatial/UeAgentRigSimulationSpatialDomainService.h")
#define UAI_T25_RIG_SIMULATION_SPATIAL_DOMAIN_AVAILABLE 1
#include "Core/HjsonWorkspace/ImportInitialization/UeAgentHjsonDomainProviderRegistry.h"
#include "OperationOwners/RigSimulationSpatial/UeAgentRigSimulationSpatialDomainService.h"
#else
#define UAI_T25_RIG_SIMULATION_SPATIAL_DOMAIN_AVAILABLE 0
#endif

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T25RigSimulationSpatialDomainProviderTest,
	"UAI.StrictV2.T25.RIG.SIMULATION.SPATIAL.DOMAIN.ProviderContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T25RigSimulationSpatialDomainProviderTest::RunTest(
	const FString& Parameters)
{
#if !UAI_T25_RIG_SIMULATION_SPATIAL_DOMAIN_AVAILABLE
	TestTrue(TEXT("T25 Rig Simulation Spatial provider is available"), false);
	return false;
#else
	using namespace UE::UAI::HjsonWorkspace;
	FWorkspaceError Error;
	TSharedPtr<const FImportInitializationDomainProviderRegistry> Registry;
	if (!TryFreezeProcessImportInitializationDomainProviders(Registry, Error)
		|| !Registry.IsValid())
	{
		AddError(Error.GetCode());
		return false;
	}
	FImportInitializationDomainResolution Resolution;
	TestTrue(
		TEXT("a PoseableMeshComponent root resolves the T25 provider"),
		Registry->TryResolve(
			FImportInitializationDomainRequest(
				TEXT("content_graph.import_workspace"),
				TEXT("uai.rig_simulation_spatial.edit/1"),
				TEXT("/Game/Maps/WB_WhiteBox01.WB_WhiteBox01:PersistentLevel.UAI_T25_Pose"),
				TEXT("/Script/Engine.PoseableMeshComponent"),
				EInitializationNodeRole::RootTarget,
				TEXT("{}")),
			Resolution,
			Error));
	TestTrue(TEXT("the T25 resolution is valid"), Resolution.IsValid());
	if (Resolution.IsValid())
	{
		TestEqual(
			TEXT("the provider identity is unique"),
			Resolution.GetProvider()->GetProviderId(),
			FString(TEXT("rig-simulation-spatial-domain-provider")));
		TestEqual(
			TEXT("the runtime root contract is provider-owned"),
			Resolution.GetInitializationContractId(),
			FString(TEXT("uai.init.rig_simulation_spatial.pose_subject.root/1")));
	}
	return true;
#endif
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T25RigSimulationSpatialDomainRejectsUnknownTypeTest,
	"UAI.StrictV2.T25.RIG.SIMULATION.SPATIAL.DOMAIN.UnknownTypePrewriteFailure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T25RigSimulationSpatialDomainRejectsUnknownTypeTest::RunTest(
	const FString& Parameters)
{
#if !UAI_T25_RIG_SIMULATION_SPATIAL_DOMAIN_AVAILABLE
	TestTrue(TEXT("T25 prewrite type gate is available"), false);
	return false;
#else
	using namespace UE::UAI::HjsonWorkspace;
	FWorkspaceError Error;
	TSharedPtr<const FImportInitializationDomainProviderRegistry> Registry;
	if (!TryFreezeProcessImportInitializationDomainProviders(Registry, Error)
		|| !Registry.IsValid())
	{
		AddError(Error.GetCode());
		return false;
	}
	FImportInitializationDomainResolution Resolution;
	TestFalse(
		TEXT("an unregistered spatial root fails before execution"),
		Registry->TryResolve(
			FImportInitializationDomainRequest(
				TEXT("content_graph.import_workspace"),
				TEXT("uai.rig_simulation_spatial.edit/1"),
				TEXT("/Game/UAI/StrictV2/T25/Unknown.Unknown"),
				TEXT("/Script/CoreUObject.Object"),
				EInitializationNodeRole::RootTarget,
				TEXT("{}")),
			Resolution,
			Error));
	TestFalse(TEXT("the rejected resolution stays invalid"), Resolution.IsValid());
	return true;
#endif
}

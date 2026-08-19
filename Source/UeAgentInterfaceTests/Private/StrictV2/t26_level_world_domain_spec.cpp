#include "Misc/AutomationTest.h"

#include "Core/HjsonWorkspace/ImportInitialization/UeAgentHjsonDomainProviderRegistry.h"
#include "Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h"

#if __has_include("OperationOwners/WorldSpatial/UeAgentLevelWorldDomainService.h")
#define UAI_T26_LEVEL_WORLD_DOMAIN_AVAILABLE 1
#include "OperationOwners/WorldSpatial/UeAgentLevelWorldDomainService.h"
#else
#define UAI_T26_LEVEL_WORLD_DOMAIN_AVAILABLE 0
#endif

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T26LevelWorldDomainTest,
	"UAI.StrictV2.T26.LEVEL.WORLD.DOMAIN",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T26LevelWorldDomainTest::RunTest(
	const FString& Parameters)
{
	(void)Parameters;
#if !UAI_T26_LEVEL_WORLD_DOMAIN_AVAILABLE
	TestTrue(TEXT("T26 Level/World domain authority is available"), false);
	return false;
#else
	using namespace UE::UAI;
	using namespace UE::UAI::HjsonWorkspace;
	using namespace UE::UAI::OperationOwners::WorldSpatial;

	TSharedPtr<IImportInitializationDomainProvider> Provider;
	FWorkspaceError WorkspaceError;
	if (!TestTrue(TEXT("the Level/World provider is created"),
		TryCreateLevelWorldDomainProvider(Provider, WorkspaceError))
		|| !Provider.IsValid())
	{
		AddError(WorkspaceError.GetCode());
		return false;
	}

	struct FContractCase
	{
		const TCHAR* Type;
		EInitializationNodeRole Role;
		const TCHAR* Contract;
	};
	const TArray<FContractCase> ContractCases = {
		{TEXT("/Script/Engine.World"), EInitializationNodeRole::RootTarget,
			TEXT("uai.init.world.world/1")},
		{TEXT("/Script/Engine.Actor"), EInitializationNodeRole::ContainerItem,
			TEXT("uai.init.world.actor/1")},
		{TEXT("/Script/Engine.StaticMeshActor"), EInitializationNodeRole::ContainerItem,
			TEXT("uai.init.world.actor/1")},
		{TEXT("/Script/Engine.SceneComponent"), EInitializationNodeRole::OwnedObject,
			TEXT("uai.init.world.component/1")},
		{TEXT("/Script/Engine.PointLightComponent"), EInitializationNodeRole::OwnedObject,
			TEXT("uai.init.world.component/1")},
		{TEXT("uai.world.data_layer"), EInitializationNodeRole::ContainerItem,
			TEXT("uai.init.world.data_layer/1")},
	};
	for (const FContractCase& ContractCase : ContractCases)
	{
		const FImportInitializationDomainRequest Request(
			TEXT("content_graph.import_workspace"), TEXT("uai.world.edit/1"),
			TEXT("/Game/UAI/StrictV2/T26/LV_T26.LV_T26"),
			ContractCase.Type, ContractCase.Role, TEXT("{}"));
		FString Contract;
		TestTrue(*FString::Printf(TEXT("world surface resolves: %s"),
			ContractCase.Type), Provider->Matches(Request)
				&& Provider->TryResolveInitializationContract(
					Request, Contract, WorkspaceError));
		TestEqual(TEXT("the initialization contract is exact"), Contract,
			FString(ContractCase.Contract));
	}

	const FImportInitializationDomainRequest Unsupported(
		TEXT("content_graph.import_workspace"), TEXT("uai.world.edit/1"),
		TEXT("/Game/UAI/StrictV2/T26/Unsupported.Unsupported"),
		TEXT("/Script/Engine.StaticMesh"), EInitializationNodeRole::RootTarget,
		TEXT("{}"));
	TestFalse(TEXT("an unregistered World root is rejected before write"),
		Provider->Matches(Unsupported));

	TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
	FString Error;
	if (!TestTrue(TEXT("the process active composition freezes"),
		Module::CommandRegistration::Active::
			TryComposeProcessActiveCommandComposition(Registry, Dispatch, Error))
		|| !Registry.IsValid())
	{
		AddError(Error);
		return false;
	}
	const auto* ImportContract = Registry->FindExact(
		TEXT("content_graph.import_workspace"));
	TestNotNull(TEXT("the shared recursive HJSON ingress remains active"),
		ImportContract);
	if (ImportContract)
	{
		TestEqual(TEXT("T13 remains the shared recursive authority"),
			ImportContract->GetPublicOwner(),
			FString(TEXT("uobject-dataasset-recursive-initialization-owner")));
	}
	for (const FString& RetiredIngress : {
		FString(TEXT("level_apply_folder")),
		FString(TEXT("level.apply_folder")),
		FString(TEXT("level.apply_file")),
		FString(TEXT("world.authoring.apply"))})
	{
		TestNull(*FString::Printf(TEXT("%s remains absent"), *RetiredIngress),
			Registry->FindExact(RetiredIngress));
	}
	for (const FString& CommandId : {
		FString(TEXT("world.data_layer_instance.create")),
		FString(TEXT("world.data_layer_relation.update")),
		FString(TEXT("world.wall.create_with_opening"))})
	{
		const auto* WorldContract = Registry->FindExact(CommandId);
		TestNotNull(*FString::Printf(TEXT("%s remains active"), *CommandId),
			WorldContract);
		if (WorldContract)
		{
			TestEqual(TEXT("T59 remains the non-HJSON World owner"),
				WorldContract->GetPublicOwner(),
				FString(TEXT("world-command-owner")));
		}
	}
	return !HasAnyErrors();
#endif
}

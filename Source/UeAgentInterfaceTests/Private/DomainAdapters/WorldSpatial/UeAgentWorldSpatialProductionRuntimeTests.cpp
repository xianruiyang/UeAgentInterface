#include "Misc/AutomationTest.h"

#include "Engine/World.h"

#include "DomainAdapters/WorldSpatial/UeAgentWorldSpatialProductionRuntime.h"
#include "DomainAdapters/WorldSpatial/UeAgentWorldSpatialExecutionChains.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentWorldSpatialProductionRuntimeCompositionTest,
	"UeAgentInterface.DomainAdapters.WorldSpatial.E2.ProductionRuntimeComposition",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentWorldSpatialProductionRuntimeCompositionTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI::DomainAdapters::WorldSpatial;

	UWorld* World = UWorld::CreateWorld(
		EWorldType::EditorPreview,
		false,
		MakeUniqueObjectName(
			GetTransientPackage(),
			UWorld::StaticClass(),
			TEXT("UAI_TS009A_World")));
	TestNotNull(TEXT("owned explicit world is created"), World);
	if (!World)
	{
		return false;
	}

	FUeAgentWorldSpatialProductionRuntimeConfig Config;
	Config.ProjectInstanceId = TEXT("ts009a-project");
	Config.EditorInstanceId = TEXT("ts009a-editor");
	Config.ServerSessionId = TEXT("ts009a-session");
	Config.RegistryEpoch = 1;

	TSharedPtr<FUeAgentWorldSpatialProductionRuntime> Runtime;
	FUeAgentWorldSpatialExecutionError Error;
	const bool bCreated = TestTrue(
		TEXT("production runtime composes from an explicit world"),
		FUeAgentWorldSpatialProductionRuntime::TryCreate(
			Config,
			*World,
			Runtime,
			Error));
	if (!bCreated || !Runtime.IsValid())
	{
		AddError(
			TEXT("world_spatial_production_runtime_unavailable:")
			+ Error.Code + TEXT(":") + Error.Detail);
		World->DestroyWorld(false);
		return false;
	}

	TestEqual(
		TEXT("all 22 exact descriptors are owned by production runtime"),
		Runtime->GetDescriptorRegistry()->GetDescriptors().Num(),
		22);
	TestEqual(
		TEXT("all 12 operations have production chains"),
		Runtime->GetOperationChains().Num(),
		12);
	TArray<FString> UnboundViewportIds;
	Runtime->GetExplicitViewportIdentities(UnboundViewportIds);
	TestTrue(
		TEXT("production runtime never discovers an implicit current viewport"),
		UnboundViewportIds.IsEmpty());

	int32 QueryCount = 0;
	int32 OperationCount = 0;
	TSet<FString> CapabilityIds;
	for (const FUeAgentWorldSpatialExecutionChainContract& Contract
		: GetWorldSpatialExecutionChainContracts())
	{
		TestFalse(
			TEXT("production capability identity is unique"),
			CapabilityIds.Contains(Contract.GetCapabilityId()));
		CapabilityIds.Add(Contract.GetCapabilityId());
		if (Contract.GetC3Lane() == TEXT("c3q"))
		{
			++QueryCount;
			TestTrue(
				TEXT("query has an exact production provider: ")
					+ Contract.GetCapabilityId(),
				Runtime->GetQueryProviderRegistry()->FindExact(
					Contract.GetBindingId()).IsValid());
		}
		else if (Contract.GetC3Lane() == TEXT("c3o"))
		{
			++OperationCount;
		}
		else
		{
			AddError(
				TEXT("world_spatial_c3_lane_invalid:")
				+ Contract.GetCapabilityId() + TEXT(":")
				+ Contract.GetC3Lane());
		}
	}
	TestEqual(TEXT("production query count"), QueryCount, 10);
	TestEqual(TEXT("production operation count"), OperationCount, 12);
	TestEqual(TEXT("production capability count"), CapabilityIds.Num(), 22);

	World->DestroyWorld(false);
	return true;
}

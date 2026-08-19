#include "Misc/AutomationTest.h"

#include "DomainAdapters/WorldSpatial/UeAgentWorldSpatialProductionDependencies.h"
#include "Components/SceneComponent.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"

namespace
{
	using namespace UE::UAI::DomainAdapters::WorldSpatial;

	UWorld* CreateOwnedWorld(const TCHAR* Prefix)
	{
		return UWorld::CreateWorld(
			EWorldType::EditorPreview,
			false,
			MakeUniqueObjectName(
				GetTransientPackage(),
				UWorld::StaticClass(),
				FName(Prefix)));
	}

	bool CreateDependencies(
		FAutomationTestBase& Test,
		UWorld& World,
		FUeAgentWorldSpatialNativeDependencies& OutDependencies)
	{
		FUeAgentWorldSpatialExecutionError Error;
		const bool bCreated = TryCreateWorldSpatialProductionDependencies(
			World,
			TEXT("ts009a-project:ts009a-editor:ts009a-session:1"),
			TEXT("ts009a-session"),
			OutDependencies,
			Error);
		Test.TestTrue(TEXT("create concrete production dependencies"), bCreated);
		if (!bCreated)
		{
			Test.AddError(
				TEXT("world_spatial_production_dependencies_unavailable:")
				+ Error.Code + TEXT(":") + Error.Detail);
		}
		return bCreated && OutDependencies.IsValid();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentWorldSpatialProductionExactWorldBoundaryTest,
	"UeAgentInterface.DomainAdapters.WorldSpatial.E2.ProductionDependencies.ExactWorldBoundary",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentWorldSpatialProductionExactWorldBoundaryTest::RunTest(
	const FString& Parameters)
{
	UWorld* OwnedWorld = CreateOwnedWorld(TEXT("UAI_TS009A_OwnedWorld"));
	UWorld* OtherWorld = CreateOwnedWorld(TEXT("UAI_TS009A_OtherWorld"));
	TestNotNull(TEXT("owned world"), OwnedWorld);
	TestNotNull(TEXT("other world"), OtherWorld);
	if (!OwnedWorld || !OtherWorld)
	{
		return false;
	}
	AActor* OwnedActor = OwnedWorld->SpawnActor<AActor>(
		FVector(10.0, 20.0, 30.0), FRotator::ZeroRotator);
	AActor* OtherActor = OtherWorld->SpawnActor<AActor>(
		FVector(40.0, 50.0, 60.0), FRotator::ZeroRotator);
	TestNotNull(TEXT("owned actor"), OwnedActor);
	TestNotNull(TEXT("other actor"), OtherActor);

	FUeAgentWorldSpatialNativeDependencies Dependencies;
	if (OwnedActor && OtherActor
		&& CreateDependencies(*this, *OwnedWorld, Dependencies))
	{
		UObject* Resolved = nullptr;
		FUeAgentWorldSpatialExecutionError Error;
		TestTrue(
			TEXT("exact actor in the scoped world resolves"),
			Dependencies.Environment->TryResolveObject(
				OwnedActor->GetPathName(), Resolved, Error));
		TestTrue(TEXT("resolved object is exact"), Resolved == OwnedActor);

		Resolved = nullptr;
		Error = {};
		TestFalse(
			TEXT("actor in another world is rejected"),
			Dependencies.Environment->TryResolveObject(
				OtherActor->GetPathName(), Resolved, Error));
		TestEqual(
			TEXT("cross-world rejection is explicit"),
			Error.Code,
			FString(TEXT("world_spatial_object_world_mismatch")));
	}

	OtherWorld->DestroyWorld(false);
	OwnedWorld->DestroyWorld(false);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentWorldSpatialProductionConversionFailClosedTest,
	"UeAgentInterface.DomainAdapters.WorldSpatial.E2.ProductionDependencies.ConversionFailClosed",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentWorldSpatialProductionConversionFailClosedTest::RunTest(
	const FString& Parameters)
{
	UWorld* World = CreateOwnedWorld(TEXT("UAI_TS009A_ConversionWorld"));
	TestNotNull(TEXT("conversion world"), World);
	if (!World)
	{
		return false;
	}
	FUeAgentWorldSpatialNativeDependencies Dependencies;
	if (CreateDependencies(*this, *World, Dependencies))
	{
		FWorldSpatialValue Value;
		Value.Subject = TEXT("ts009a.subject");
		Value.Quantity = TEXT("point");
		Value.Frame.FrameId = TEXT("semantic.local");
		Value.Frame.BasisFrameId = TEXT("unreal.world");
		Value.Frame.Dimension = 3;
		Value.Frame.Unit = TEXT("centimeter");
		Value.Frame.BasisEvidenceId = TEXT("basis:explicit");
		Value.Frame.ObservedAt.ClockId = TEXT("editor");
		Value.Frame.ObservedAt.Tick = TEXT("1");
		Value.Frame.ObservedAt.RateNumerator = TEXT("60");
		Value.Frame.ObservedAt.RateDenominator = TEXT("1");
		Value.Frame.ObservedAt.State = TEXT("frozen");
		Value.Components = {TEXT("1"), TEXT("2"), TEXT("3")};
		Value.ConversionEvidenceIds = {TEXT("unexecuted:evidence")};

		FUeAgentWorldSpatialCompiledValue Compiled;
		FUeAgentWorldSpatialExecutionError Error;
		TestFalse(
			TEXT("an evidence label cannot substitute for an executable conversion"),
			Dependencies.SpaceCompiler->TryConvertToNative(
				Value,
				TEXT("unreal.world"),
				TEXT("centimeter"),
				Compiled,
				Error));
		TestEqual(
			TEXT("missing conversion path fails explicitly"),
			Error.Code,
			FString(TEXT("world_spatial_conversion_path_unavailable")));

		Value.Frame.FrameId = TEXT("unreal.world");
		Error = {};
		TestTrue(
			TEXT("same-frame same-unit conversion is executable"),
			Dependencies.SpaceCompiler->TryConvertToNative(
				Value,
				TEXT("unreal.world"),
				TEXT("centimeter"),
				Compiled,
				Error));
		TestEqual(TEXT("compiled component count"), Compiled.Components.Num(), 3);
	}
	World->DestroyWorld(false);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentWorldSpatialProductionRevisionTracksStateTest,
	"UeAgentInterface.DomainAdapters.WorldSpatial.E2.ProductionDependencies.RevisionTracksState",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentWorldSpatialProductionRevisionTracksStateTest::RunTest(
	const FString& Parameters)
{
	UWorld* World = CreateOwnedWorld(TEXT("UAI_TS009A_RevisionWorld"));
	TestNotNull(TEXT("revision world"), World);
	if (!World)
	{
		return false;
	}
	AActor* Actor = World->SpawnActor<AActor>(
		FVector::ZeroVector, FRotator::ZeroRotator);
	TestNotNull(TEXT("revision actor"), Actor);
	USceneComponent* Root = Actor
		? NewObject<USceneComponent>(Actor, TEXT("TS009ARevisionRoot"))
		: nullptr;
	TestNotNull(TEXT("revision actor root"), Root);
	if (Actor && Root)
	{
		Actor->SetRootComponent(Root);
		Actor->AddInstanceComponent(Root);
		Root->RegisterComponentWithWorld(World);
	}
	FUeAgentWorldSpatialNativeDependencies Dependencies;
	if (Actor && Root && CreateDependencies(*this, *World, Dependencies))
	{
		FWorldSpatialRevision Before;
		FWorldSpatialRevision After;
		FUeAgentWorldSpatialExecutionError Error;
		TestTrue(
			TEXT("read independent actor revision before mutation"),
			Dependencies.RevisionSource->TryReadRevision(
				Actor->GetPathName(), Before, Error));
		Actor->SetActorLocation(FVector(123.0, 456.0, 789.0));
		Error = {};
		TestTrue(
			TEXT("read independent actor revision after mutation"),
			Dependencies.RevisionSource->TryReadRevision(
				Actor->GetPathName(), After, Error));
		TestEqual(TEXT("revision source remains exact"), After.SourceId, Before.SourceId);
		TestEqual(TEXT("revision generation remains stable"), After.Generation, Before.Generation);
		TestNotEqual(
			TEXT("real actor state change advances the revision"),
			After.Sequence,
			Before.Sequence);
	}
	World->DestroyWorld(false);
	return true;
}

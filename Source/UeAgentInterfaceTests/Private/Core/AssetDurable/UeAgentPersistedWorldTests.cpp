#include "Misc/AutomationTest.h"

#include "Core/AssetDurable/UeAgentPersistedWorld.h"

namespace
{
	using namespace UE::UAI::AssetDurable;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentPersistedWorldReferenceMapTest,
	"UeAgentInterface.Core.AssetDurable.PersistedWorld.ReferenceMap",
	EAutomationTestFlags::EditorContext
		| EAutomationTestFlags::EngineFilter)

bool FUeAgentPersistedWorldReferenceMapTest::RunTest(
	const FString& Parameters)
{
	const FString SourceWorld =
		TEXT("/Game/UAI/Tests/T70/World.World");
	const FString SnapshotWorld =
		TEXT("/Temp/UAI/PersistedWorld/ABC/World.World");
	const FPersistedWorldReferenceMap ReferenceMap(
		SourceWorld,
		SnapshotWorld);
	TestTrue(TEXT("reference map is valid"), ReferenceMap.IsValid());

	FString Mapped;
	TestTrue(
		TEXT("world identity maps into the isolated snapshot"),
		ReferenceMap.TryMapReference(SourceWorld, Mapped));
	TestEqual(
		TEXT("world identity uses the snapshot world path"),
		Mapped,
		SnapshotWorld);

	const FString SourceActor = SourceWorld
		+ TEXT(":PersistentLevel.StaticMeshActor_UAID_1");
	TestTrue(
		TEXT("world-owned identity maps by exact relative path"),
		ReferenceMap.TryMapReference(SourceActor, Mapped));
	TestEqual(
		TEXT("world-owned suffix is preserved"),
		Mapped,
		SnapshotWorld
			+ TEXT(":PersistentLevel.StaticMeshActor_UAID_1"));

	TestFalse(
		TEXT("standalone assets remain outside the world mapping"),
		ReferenceMap.TryMapReference(
			TEXT("/Game/UAI/Shared/Asset.Asset"),
			Mapped));
	TestTrue(TEXT("failed mapping clears output"), Mapped.IsEmpty());
	TestFalse(
		TEXT("similar world prefixes are not accepted"),
		ReferenceMap.TryMapReference(
			SourceWorld + TEXT("Extra:PersistentLevel.Actor"),
			Mapped));
	return true;
}

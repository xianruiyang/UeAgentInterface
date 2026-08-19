#include "Misc/AutomationTest.h"

#include "Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h"

namespace UE::UAI::Tests::StrictV2::T38
{
	const TArray<FString>& ExpectedCommands()
	{
		static const TArray<FString> Commands = {
			TEXT("mesh.modeling_session"),
			TEXT("mesh.attribute.material_slot.add"),
			TEXT("mesh.attribute.material_slot.assign"),
			TEXT("mesh.attribute.normals.recompute"),
			TEXT("mesh.attribute.uv.auto"),
			TEXT("mesh.attribute.uv.project"),
			TEXT("mesh.collision.convex.generate"),
			TEXT("mesh.collision.simple.generate"),
			TEXT("mesh.primitive.box.create"),
			TEXT("mesh.primitive.cylinder.create"),
			TEXT("mesh.primitive.plane.create"),
			TEXT("mesh.primitive.ramp.create"),
			TEXT("mesh.primitive.ramp_corner.create"),
			TEXT("mesh.primitive.sphere.create"),
			TEXT("mesh.primitive.stairs.create"),
			TEXT("mesh.topology.boolean"),
			TEXT("mesh.topology.edges.bevel"),
			TEXT("mesh.topology.edges.weld"),
			TEXT("mesh.topology.faces.duplicate"),
			TEXT("mesh.topology.faces.extrude"),
			TEXT("mesh.topology.faces.inset"),
			TEXT("mesh.topology.faces.offset"),
			TEXT("mesh.topology.faces.push_pull"),
			TEXT("mesh.topology.holes.fill"),
			TEXT("mesh.topology.mesh_cut"),
			TEXT("mesh.topology.mirror"),
			TEXT("mesh.topology.plane_cut"),
			TEXT("mesh.topology.remesh"),
			TEXT("mesh.topology.simplify"),
			TEXT("mesh.topology.subdivide"),
			TEXT("mesh.topology.trim"),
			TEXT("mesh.topology.voxel_boolean"),
		};
		return Commands;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentStrictV2T38ModelingGeometryInventoryTest,
	"UAI.StrictV2.T38.MODELING.GEOMETRY.OPERATION.InventoryAndActivation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeAgentStrictV2T38ModelingGeometryInventoryTest::RunTest(
	const FString& Parameters)
{
	using namespace UE::UAI;
	using namespace UE::UAI::Module::CommandRegistration::Active;
	using namespace UE::UAI::Tests::StrictV2::T38;
	TSharedPtr<const CommandRegistry::FUeAgentImmutableCommandRegistry> Registry;
	TSharedPtr<const CommandEdge::FUeAgentExactCommandDispatch> Dispatch;
	FString Error;
	if (!TestTrue(TEXT("the process active composition freezes"),
		TryComposeProcessActiveCommandComposition(Registry, Dispatch, Error))
		|| !Registry.IsValid())
	{
		AddError(Error);
		return false;
	}
	TestEqual(TEXT("T38 owns exactly 32 inventory commands"),
		ExpectedCommands().Num(), 32);
	for (const FString& CommandId : ExpectedCommands())
	{
		const auto* Contract = Registry->FindExact(CommandId);
		TestNotNull(*FString::Printf(TEXT("%s is active"), *CommandId), Contract);
		if (Contract)
		{
			TestEqual(*FString::Printf(TEXT("%s has one T38 owner"), *CommandId),
				Contract->GetPublicOwner(),
				FString(TEXT("modeling-geometry-owner")));
		}
	}
	const auto* Session = Registry->FindExact(TEXT("mesh.modeling_session"));
	if (Session)
	{
		TestEqual(TEXT("modeling session has five exact top-level fields"),
			Session->GetInputFields().Num(), 5);
		TestEqual(TEXT("modeling session action is required"),
			Session->GetInputFields()[0].Name, FString(TEXT("action")));
	}
	return !HasAnyErrors();
}

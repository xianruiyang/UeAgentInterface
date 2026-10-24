#include "Misc/AutomationTest.h"

#include "Core/Contract/UeAgentCommandJson.h"
#include "Core/Contract/Command/UeAgentCommandLifecyclePolicy.h"
#include "Dom/JsonObject.h"
#include "DomainAdapters/ModelingGeometry/UeAgentModelingGeometryMechanicsBoundary.h"
#include "Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h"

namespace UE::UAI::Tests::StrictV2::T38
{
	const TArray<FString>& ExpectedCommands()
	{
		static const TArray<FString> Commands = {
			TEXT("mesh.modeling_session"),
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

	const TArray<FString>& DormantCommands()
	{
		static const TArray<FString> Commands = {
			TEXT("mesh.attribute.material_slot.add"),
			TEXT("mesh.attribute.material_slot.assign"),
			TEXT("mesh.pivot.update"),
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
	for (const FString& CommandId : ExpectedCommands())
	{
		const auto* Contract = Registry->FindExact(CommandId);
		TestNotNull(*FString::Printf(TEXT("%s is active"), *CommandId), Contract);
		if (Contract)
		{
			TestEqual(*FString::Printf(TEXT("%s has one T38 owner"), *CommandId),
				Contract->GetPublicOwner(),
				FString(TEXT("modeling-geometry-owner")));
			if (CommandId.EndsWith(TEXT(".create")))
			{
				const auto& Fields = Contract->GetModelResultFields();
				TestEqual(TEXT("primitive create has two model fields"),
					Fields.Num(), 2);
				if (Fields.Num() == 2)
				{
					TestEqual(TEXT("primitive create status source"),
						Fields[0].SourcePath,
						TArray<FString>{TEXT("projection"), TEXT("payload"),
							TEXT("status")});
					TestEqual(TEXT("primitive create outcome source"),
						Fields[1].SourcePath,
						TArray<FString>{TEXT("projection"), TEXT("payload"),
							TEXT("effect_outcome")});
				}
			}
		}
	}
	for (const FString& CommandId : DormantCommands())
	{
		const auto* Contract = Registry->FindExact(CommandId);
		TestNotNull(*FString::Printf(TEXT("%s remains registered"), *CommandId), Contract);
		if (Contract)
		{
			TestEqual(
				*FString::Printf(TEXT("%s is centrally dormant"), *CommandId),
				Contract->GetAvailability(),
				CommandContract::EUeAgentCommandAvailability::Dormant);
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
	TestTrue(TEXT("set_actor_selection remains session-native"),
		DomainAdapters::ModelingGeometry::IsModelingSessionAction(
			TEXT("set_actor_selection")));
	const TArray<FString> ExactCommandOwnedActions = {
		TEXT("observe_selection"),
		TEXT("select_via_world_ray"),
		TEXT("replace_actor_mesh"),
		TEXT("snap_to_surface"),
	};
	for (const FString& ExactCommandOwnedAction : ExactCommandOwnedActions)
	{
		TestFalse(
			*FString::Printf(
				TEXT("%s is not duplicated by mesh.modeling_session"),
				*ExactCommandOwnedAction),
			DomainAdapters::ModelingGeometry::IsModelingSessionAction(
				ExactCommandOwnedAction));
	}
	for (const FString& CommandId : ExpectedCommands())
	{
		if (CommandId == TEXT("mesh.modeling_session"))
		{
			continue;
		}
		const auto* Contract = Registry->FindExact(CommandId);
		if (!Contract)
		{
			continue;
		}
		for (const CommandContract::FUeAgentCommandFieldDeclaration& Field :
			Contract->GetInputFields())
		{
			TestTrue(
				*FString::Printf(
					TEXT("%s.%s publishes its exact parameter schema"),
					*CommandId,
					*Field.Name),
				!Field.SchemaJson.IsEmpty());
		}
	}

	const auto* Normals = Registry->FindExact(
		TEXT("mesh.attribute.normals.recompute"));
	if (Normals)
	{
		const CommandContract::FUeAgentCommandFieldDeclaration* Mode =
			Normals->GetInputFields().FindByPredicate(
				[](const CommandContract::FUeAgentCommandFieldDeclaration& Field)
				{
					return Field.Name == TEXT("mode");
				});
		TestNotNull(TEXT("normals mode field exists"), Mode);
		if (Mode)
		{
			TSharedPtr<FJsonObject> Schema;
			FString SchemaError;
			TestTrue(
				TEXT("normals mode schema is public canonical JSON"),
				CommandContract::Json::TryParseObject(
					Mode->SchemaJson,
					Schema,
					SchemaError));
			if (Schema.IsValid())
			{
				TSet<FString> Values;
				for (const TSharedPtr<FJsonValue>& Value :
					Schema->GetArrayField(TEXT("enum")))
				{
					Values.Add(Value->AsString());
				}
				TestTrue(
					TEXT("normals mode exposes every accepted exact value"),
					Values.Num() == 3
						&& Values.Contains(TEXT("area_angle_weighted"))
						&& Values.Contains(TEXT("face"))
						&& Values.Contains(TEXT("angle_threshold")));
			}
		}
	}
	return !HasAnyErrors();
}

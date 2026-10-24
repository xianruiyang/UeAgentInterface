#include "DomainAdapters/ModelingGeometry/UeAgentModelingGeometryMechanicsBoundary.h"

#include "DomainAdapters/ModelingGeometry/UeAgentModelingVoxelGrid.h"
#include "DomainAdapters/ModelingGeometry/UeAgentModelingMechanics.h"
#include "Dom/JsonValue.h"

namespace UE::UAI::DomainAdapters::ModelingGeometry
{
	namespace
	{
		class FModelingSessionNativeMechanicsPort final
			: public IUeAgentModelingSessionNativeMechanicsPort
		{
		public:
			bool Execute(
				const FString& Action,
				const FModelingSessionNativeRequest& Request,
				TSharedPtr<FJsonObject>& OutObservation,
				UE::UAI::Contract::FDiagnostic& OutError) const override
			{
				if (IsModelingSessionAction(Action))
				{
					return FModelingSession::ExecuteAction(Action, Request, OutObservation, OutError);
				}

				OutError = UeAgentModelingOps::ModelingFact(TEXT("modeling_session_native_action_unknown"), TEXT("The native modeling port does not support this session action."), TEXT("/action"), MakeShared<FJsonValueString>(Action), MakeShared<FJsonValueString>(TEXT("supported modeling session action")));
				return false;
			}
		};
	}

	const TArray<FString>& GetCommandIds()
	{
		static const TArray<FString> Ids = {
			TEXT("mesh.modeling_session"),
			VoxelGrid::GetBeginCommandId(),
			VoxelGrid::GetSessionCommandId(),
			VoxelGrid::GetApplyCommandId(),
			TEXT("mesh.attribute.material_slot.add"), TEXT("mesh.attribute.material_slot.assign"),
			TEXT("mesh.attribute.normals.recompute"), TEXT("mesh.attribute.uv.auto"),
			TEXT("mesh.attribute.uv.project"), TEXT("mesh.collision.convex.generate"),
			TEXT("mesh.collision.simple.generate"), TEXT("mesh.primitive.box.create"),
			TEXT("mesh.primitive.cylinder.create"), TEXT("mesh.primitive.plane.create"),
			TEXT("mesh.primitive.ramp.create"), TEXT("mesh.primitive.ramp_corner.create"),
			TEXT("mesh.primitive.sphere.create"), TEXT("mesh.primitive.stairs.create"),
			TEXT("mesh.topology.boolean"), TEXT("mesh.topology.edges.bevel"),
			TEXT("mesh.topology.edges.weld"), TEXT("mesh.topology.faces.duplicate"),
			TEXT("mesh.topology.faces.extrude"), TEXT("mesh.topology.faces.inset"),
			TEXT("mesh.topology.faces.offset"), TEXT("mesh.topology.faces.push_pull"),
			TEXT("mesh.topology.holes.fill"), TEXT("mesh.topology.mesh_cut"),
			TEXT("mesh.topology.mirror"), TEXT("mesh.topology.plane_cut"),
			TEXT("mesh.topology.remesh"), TEXT("mesh.topology.simplify"),
			TEXT("mesh.topology.subdivide"), TEXT("mesh.topology.trim"),
			TEXT("mesh.topology.voxel_boolean"),
		};
		return Ids;
	}

	const TArray<FString>& GetRequiredAuthorityScopes()
	{
		static const TArray<FString> Scopes = {
			TEXT("uai:modeling-geometry:write"),
		};
		return Scopes;
	}

	bool IsModelingSessionAction(const FString& Action)
	{
		static const TSet<FString> Actions = {
			TEXT("activate"), TEXT("observe_tool"), TEXT("start_tool"),
			TEXT("set_tool_property"), TEXT("invoke_tool_action"),
			TEXT("accept_tool"), TEXT("cancel_tool"),
			TEXT("set_actor_selection"),
			TEXT("set_selection_mode"), TEXT("clear_mesh_selection"),
			TEXT("select_via_screen"), TEXT("save_mesh_asset"),
		};
		return Actions.Contains(Action);
	}

	TSharedRef<const IUeAgentModelingSessionNativeMechanicsPort>
	CreateModelingSessionNativeMechanicsPort()
	{
		return MakeShared<FModelingSessionNativeMechanicsPort>();
	}
}

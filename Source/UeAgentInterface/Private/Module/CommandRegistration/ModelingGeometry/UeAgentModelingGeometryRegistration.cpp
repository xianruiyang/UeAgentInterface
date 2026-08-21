#include "Module/CommandRegistration/ModelingGeometry/UeAgentModelingGeometryRegistration.h"

#include "CommandHandlers/ModelingGeometry/UeAgentModelingGeometryCommandHandlers.h"
#include "Composition/SimpleAuthoring/UeAgentSimpleAuthoringComposition.h"
#include "Core/Contract/UeAgentCommandContract.h"
#include "Core/DomainCapability/UeAgentDomainCapability.h"
#include "DomainAdapters/ModelingGeometry/UeAgentModelingGeometryMechanicsBoundary.h"
#include "DomainAdapters/ModelingGeometry/UeAgentModelingVoxelGrid.h"
#include "OperationOwners/ModelingGeometry/UeAgentModelingGeometryOperationOwner.h"

namespace UE::UAI::Module::CommandRegistration::ModelingGeometry
{
	using namespace Active;
	using namespace CommandContract;
	using namespace CommandEdge;
	using namespace CommandRegistry;
	namespace
	{
		const FString Owner = TEXT("modeling-geometry-owner");
		FString WireType(EUeAgentCommandWireType Type)
		{
			switch (Type)
			{
			case EUeAgentCommandWireType::Boolean: return TEXT("boolean");
			case EUeAgentCommandWireType::Integer: return TEXT("integer");
			case EUeAgentCommandWireType::Number: return TEXT("number");
			case EUeAgentCommandWireType::String: return TEXT("string");
			case EUeAgentCommandWireType::Object: return TEXT("object");
			case EUeAgentCommandWireType::Array: return TEXT("array");
			default: return TEXT("any");
			}
		}
		FUeAgentCommandFieldDeclaration Field(
			const TCHAR* Name, const TCHAR* Type, bool Required, const TCHAR* Description)
		{
			return {Name, Type, Required, false, Description};
		}
		bool TryDeclaration(
			const FString& Id,
			const FUeAgentCommandContractRegistry& Legacy,
			FUeAgentCommandDeclaration& Out,
			FString& OutError)
		{
			const FUeAgentCommandContract* Contract = Legacy.FindExact(Id);
			if (!Contract || !Contract->IsValid())
			{
				OutError = TEXT("modeling_geometry_contract_missing:") + Id;
				return false;
			}
			FUeAgentCommandDeclaration Result;
			Result.CommandId = Id;
			Result.PythonNamespace = TEXT("t38_modeling_geometry_operation");
			Result.PythonFunction = Id.Replace(TEXT("."), TEXT("_"));
			Result.ContractVersion = 1;
			Result.PublicOwner = Owner;
			Result.Availability = EUeAgentCommandAvailability::Enabled;
			Result.SideEffect = EUeAgentCommandSideEffect::AssetState;
			Result.StateLayers = {TEXT("asset_persisted_state")};
			for (const FUeAgentCommandParameter& P : Contract->GetParameters())
			{
				Result.InputFields.Add({P.GetName(), WireType(P.GetWireType()),
					P.IsRequired(), P.IsNullable(),
					TEXT("Exact modeling geometry mechanics parameter.")});
			}
			Result.ResultFields = {
				Field(TEXT("operation_id"), TEXT("string"), true,
					TEXT("Stable identifier of the completed operation.")),
				Field(TEXT("projection"), TEXT("object"), true,
					TEXT("Verified native geometry result and evidence identities.")),
			};
			Result.Errors = {
				{TEXT("modeling_geometry_authority_required"), TEXT("uai:operate is required.")},
				{TEXT("simple_authoring_input_invalid"), TEXT("The exact geometry schema rejects the request before mutation.")},
				{TEXT("operation_state_unknown"), TEXT("The final geometry state cannot be proven and must not be replayed.")},
			};
			Result.Purpose.Summary = TEXT("Execute ") + Id
				+ TEXT(" through the common OperationService and modeling geometry mechanics.");
			Result.Purpose.Target = TEXT("One exact mesh asset or geometry target declared by the command contract.");
			Result.Purpose.Preconditions = {TEXT("Resolve every mesh identity before mutation.")};
			Result.Purpose.ParameterGuidance = TEXT("Use only query.command.detail fields.");
			Result.Purpose.ResultGuidance = TEXT("Success requires verified terminal projection evidence.");
			Result.Purpose.FailureGuidance = TEXT("state_unknown is non-replayable.");
			Result.Purpose.SideEffectGuidance = TEXT("Uses the formal native modeling geometry effect.");
			Result.Purpose.MinimumExample = Result.PythonFunction + TEXT("(client, params)");
			Result.Purpose.RelatedFlows = {TEXT("query.command.detail"), TEXT("asset readback")};
			Result.Purpose.Keywords = {TEXT("mesh"), TEXT("geometry"), TEXT("modeling")};
			Result.Purpose.Synonyms = {TEXT("mesh modeling operation")};
			Out = MoveTemp(Result);
			return true;
		}

		FUeAgentCommandDeclaration ModelingSessionDeclaration()
		{
			FUeAgentCommandDeclaration Result;
			Result.CommandId = TEXT("mesh.modeling_session");
			Result.PythonNamespace = TEXT("mesh");
			Result.PythonFunction = TEXT("modeling_session");
			Result.ContractVersion = 1;
			Result.PublicOwner = Owner;
			Result.Availability = EUeAgentCommandAvailability::Enabled;
			Result.SideEffect = EUeAgentCommandSideEffect::EditorState;
			Result.StateLayers = {TEXT("authoring_memory"), TEXT("asset_persisted_state")};
			Result.InputFields = {
				Field(TEXT("action"), TEXT("string"), true,
					TEXT("Exact Modeling Mode action discriminator.")),
				Field(TEXT("target"), TEXT("string"), false,
					TEXT("Exact asset, actor, component, session, or world target identity.")),
				Field(TEXT("input"), TEXT("object"), false,
					TEXT("Action-specific typed parameters.")),
				Field(TEXT("context"), TEXT("object"), false,
					TEXT("Explicit world, selection, viewport, and expected-revision context.")),
				Field(TEXT("projection"), TEXT("object"), false,
					TEXT("Bounded observation projection.")),
			};
			Result.ResultFields = {
				Field(TEXT("action"), TEXT("string"), true,
					TEXT("Executed action.")),
				Field(TEXT("terminal_status"), TEXT("string"), true,
					TEXT("Observed terminal status for the action.")),
				Field(TEXT("observation"), TEXT("object"), true,
					TEXT("Real Modeling Mode, selection, actor, or persisted asset observation.")),
				Field(TEXT("errors"), TEXT("array"), true,
					TEXT("Bounded structured action errors.")),
				Field(TEXT("warnings"), TEXT("array"), true,
					TEXT("Bounded structured action warnings.")),
			};
			Result.Errors = {
				{TEXT("modeling_geometry_authority_required"), TEXT("uai:operate is required.")},
				{TEXT("modeling_session_input_invalid"), TEXT("The action union rejects the request before mutation.")},
				{TEXT("modeling_session_action_failed"), TEXT("The real Modeling Mode action failed.")},
				{TEXT("modeling_session_state_unknown"), TEXT("The final action state cannot be proven and must not be replayed.")},
			};
			Result.Purpose.Summary = TEXT("Execute one exact action in the real Unreal Modeling Mode session.");
			Result.Purpose.Target = TEXT("One explicit Modeling Mode session, tool, selection, actor, or mesh asset identity.");
			Result.Purpose.Preconditions = {TEXT("Resolve the exact target, world context, action and expected revision before mutation.")};
			Result.Purpose.ParameterGuidance = TEXT("Use query.command.detail; fields not owned by the selected action are rejected.");
			Result.Purpose.ResultGuidance = TEXT("Inspect terminal_status and the independent observation for the selected action.");
			Result.Purpose.FailureGuidance = TEXT("Input failures are not_started; state_unknown is non-replayable.");
			Result.Purpose.SideEffectGuidance = TEXT("Tool actions affect authoring memory; save_mesh_asset and declared commits require persisted readback.");
			Result.Purpose.MinimumExample = TEXT("client.mesh.modeling_session('observe_tool', None, {}, {}, {})");
			Result.Purpose.RelatedFlows = {TEXT("query.command.detail"), TEXT("operation.status"), TEXT("content_graph.summary")};
			Result.Purpose.Keywords = {TEXT("modeling mode"), TEXT("tool"), TEXT("mesh selection")};
			Result.Purpose.Synonyms = {TEXT("modeling session action")};
			return Result;
		}

		FUeAgentCommandDeclaration VoxelGridSessionDeclaration()
		{
			FUeAgentCommandDeclaration Result;
			Result.CommandId = DomainAdapters::ModelingGeometry::VoxelGrid::
				GetSessionCommandId();
			Result.PythonNamespace = TEXT("mesh");
			Result.PythonFunction = TEXT("voxel_grid_session");
			Result.ContractVersion = 1;
			Result.PublicOwner = Owner;
			Result.Availability = EUeAgentCommandAvailability::Enabled;
			Result.SideEffect = EUeAgentCommandSideEffect::WorldState;
			Result.StateLayers = {TEXT("authoring_memory")};
			Result.InputFields = {
				Field(TEXT("action"), TEXT("string"), true,
					TEXT("begin, read, add_bounds, remove_bounds, close, or cancel.")),
				Field(TEXT("session_id"), TEXT("string"), false,
					TEXT("Opaque session identity returned by begin.")),
				Field(TEXT("expected_revision"), TEXT("integer"), false,
					TEXT("Required optimistic fence for every session mutation.")),
				Field(TEXT("mode"), TEXT("string"), false,
					TEXT("begin mode: create or edit.")),
				Field(TEXT("grid_id"), TEXT("string"), false,
					TEXT("Stable safe grid identity; generated for create when omitted.")),
				Field(TEXT("actor_ref"), TEXT("string"), false,
					TEXT("Exact DynamicMeshActor path, name, or unique label for edit.")),
				Field(TEXT("label"), TEXT("string"), false,
					TEXT("Optional label for a newly created actor.")),
				Field(TEXT("folder_path"), TEXT("string"), false,
					TEXT("Optional World Outliner folder for a newly created actor.")),
				Field(TEXT("cell_size"), TEXT("number"), false,
					TEXT("Grid cell size in Unreal centimeters.")),
				Field(TEXT("origin"), TEXT("object"), false,
					TEXT("Grid-frame origin in world centimeters.")),
				Field(TEXT("rotation"), TEXT("object"), false,
					TEXT("Grid-frame UE pitch/yaw/roll in degrees.")),
				Field(TEXT("anchor"), TEXT("object"), false,
					TEXT("Actor anchor in grid-local centimeters.")),
				Field(TEXT("bounds"), TEXT("object"), false,
					TEXT("Explicit grid_local, component_local, or world AABB.")),
				Field(TEXT("max_operation_cells"), TEXT("integer"), false,
					TEXT("Optional per-call limit no greater than the service hard limit.")),
				Field(TEXT("projection"), TEXT("object"), false,
					TEXT("Bounded session readback projection.")),
			};
			Result.ResultFields = {
				Field(TEXT("operation_id"), TEXT("string"), true,
					TEXT("Stable request operation identity.")),
				Field(TEXT("action"), TEXT("string"), true,
					TEXT("Executed session action.")),
				Field(TEXT("terminal_status"), TEXT("string"), true,
					TEXT("Observed operation terminal status.")),
				Field(TEXT("readback"), TEXT("object"), true,
					TEXT("Independent process-session or cleanup observation.")),
				Field(TEXT("errors"), TEXT("array"), true,
					TEXT("Bounded structured errors.")),
				Field(TEXT("warnings"), TEXT("array"), true,
					TEXT("Bounded structured warnings.")),
			};
			Result.Errors = {
				{TEXT("modeling_geometry_authority_required"),
					TEXT("uai:operate authority is required.")},
				{TEXT("voxel_grid_revision_mismatch"),
					TEXT("The session revision differs from expected_revision.")},
				{TEXT("voxel_grid_session_dirty"),
					TEXT("A dirty session must be applied or cancelled before close.")},
				{TEXT("voxel_grid_operation_cell_limit_exceeded"),
					TEXT("The requested conservative cover exceeds the bounded budget.")},
			};
			Result.Purpose.Summary =
				TEXT("Manage one bounded CubeGrid-style voxel whitebox session.");
			Result.Purpose.Target =
				TEXT("One exact DynamicMeshActor and one opaque grid session.");
			Result.Purpose.Preconditions = {
				TEXT("Use an explicit coordinate space for every bounds mutation."),
				TEXT("Copy session_id and revision unchanged from readback."),
			};
			Result.Purpose.ParameterGuidance =
				TEXT("World AABBs are conservatively covered after eight-corner grid conversion.");
			Result.Purpose.ResultGuidance =
				TEXT("Use readback revision and actor_ref for the next exact action.");
			Result.Purpose.FailureGuidance =
				TEXT("Do not replay stale or state_unknown mutations.");
			Result.Purpose.SideEffectGuidance =
				TEXT("Cancel cleans an uncommitted created actor; no viewport or window is changed.");
			Result.Purpose.MinimumExample =
				TEXT("client.mesh.voxel_grid_session('begin', mode='create', cell_size=100)");
			Result.Purpose.RelatedFlows = {
				TEXT("mesh.voxel_grid.apply"), TEXT("query.command.detail")};
			Result.Purpose.Keywords = {
				TEXT("cubegrid"), TEXT("voxel"), TEXT("whitebox"), TEXT("blockout")};
			Result.Purpose.Synonyms = {TEXT("grid blockout session")};
			return Result;
		}

		FUeAgentCommandDeclaration VoxelGridApplyDeclaration()
		{
			FUeAgentCommandDeclaration Result;
			Result.CommandId = DomainAdapters::ModelingGeometry::VoxelGrid::
				GetApplyCommandId();
			Result.PythonNamespace = TEXT("mesh");
			Result.PythonFunction = TEXT("voxel_grid_apply");
			Result.ContractVersion = 1;
			Result.PublicOwner = Owner;
			Result.Availability = EUeAgentCommandAvailability::Enabled;
			Result.SideEffect = EUeAgentCommandSideEffect::WorldState;
			Result.StateLayers = {
				TEXT("authoring_memory"), TEXT("project_saved_state")};
			Result.InputFields = {
				Field(TEXT("session_id"), TEXT("string"), true,
					TEXT("Opaque voxel-grid session identity.")),
				Field(TEXT("expected_revision"), TEXT("integer"), true,
					TEXT("Current session revision used as an optimistic fence.")),
				Field(TEXT("mesh_build"), TEXT("string"), false,
					TEXT("exposed_faces; other values fail closed.")),
				Field(TEXT("collision"), TEXT("object"), false,
					TEXT("exact_box_components or none, profile, enabled, and max_boxes.")),
				Field(TEXT("material_ref"), TEXT("string"), false,
					TEXT("Optional exact MaterialInterface object path.")),
				Field(TEXT("keep_session"), TEXT("boolean"), false,
					TEXT("Keep the clean session for further revision-fenced edits.")),
			};
			Result.ResultFields = {
				Field(TEXT("operation_id"), TEXT("string"), true,
					TEXT("Stable request operation identity.")),
				Field(TEXT("terminal_status"), TEXT("string"), true,
					TEXT("Verified terminal status.")),
				Field(TEXT("mesh"), TEXT("object"), true,
					TEXT("Exposed-face mesh counts and material readback.")),
				Field(TEXT("collision"), TEXT("object"), true,
					TEXT("Exact-cover collision observation.")),
				Field(TEXT("sidecar_path"), TEXT("string"), true,
					TEXT("Committed recoverable grid-state sidecar.")),
				Field(TEXT("readback"), TEXT("object"), true,
					TEXT("Independent Actor, mesh, collision, transform, and sidecar readback.")),
			};
			Result.Errors = {
				{TEXT("modeling_geometry_authority_required"),
					TEXT("uai:operate authority is required.")},
				{TEXT("voxel_grid_revision_mismatch"),
					TEXT("The session revision differs from expected_revision.")},
				{TEXT("voxel_grid_collision_box_limit_exceeded"),
					TEXT("The exact cover exceeds the declared bounded box limit.")},
				{TEXT("voxel_grid_independent_readback_mismatch"),
					TEXT("Published Actor state and committed sidecar are not equivalent.")},
			};
			Result.Purpose.Summary =
				TEXT("Apply one CubeGrid-style session as exposed-face mesh and exact-cover collision.");
			Result.Purpose.Target =
				TEXT("The exact DynamicMeshActor owned by session_id.");
			Result.Purpose.Preconditions = {
				TEXT("Read the current session revision immediately before apply."),
				TEXT("Use a collision box limit appropriate for the target cell set."),
			};
			Result.Purpose.ParameterGuidance =
				TEXT("The command never implicitly saves a Level package containing unrelated dirty content.");
			Result.Purpose.ResultGuidance =
				TEXT("Success includes actual mesh/collision/sidecar equivalence readback.");
			Result.Purpose.FailureGuidance =
				TEXT("rolled_back is retryable after a fresh read; state_unknown requires reconciliation.");
			Result.Purpose.SideEffectGuidance =
				TEXT("Updates only the target DynamicMeshActor, owned collision components, and grid sidecar.");
			Result.Purpose.MinimumExample =
				TEXT("client.mesh.voxel_grid_apply(session_id, revision, collision={'mode':'exact_box_components'})");
			Result.Purpose.RelatedFlows = {
				TEXT("mesh.voxel_grid.session"), TEXT("spatial readback")};
			Result.Purpose.Keywords = {
				TEXT("cubegrid"), TEXT("voxel mesh"), TEXT("exact collision")};
			Result.Purpose.Synonyms = {TEXT("apply blockout grid")};
			return Result;
		}

		class FContribution final : public IUeAgentActiveCommandContribution
		{
		public:
			FContribution(
				TArray<FUeAgentCommandDeclaration> In,
				TSharedRef<const DomainAdapters::ModelingGeometry::IUeAgentModelingSessionNativeMechanicsPort>
					InNativeMechanicsPort)
				: Declarations(MoveTemp(In))
				, NativeMechanicsPort(MoveTemp(InNativeMechanicsPort))
			{
			}
			const FString& GetContributionId() const override
			{
				static const FString Id(TEXT("modeling-geometry")); return Id;
			}
			void CollectDeclarations(TArray<FUeAgentCommandDeclaration>& Out) const override
			{
				Out.Append(Declarations);
			}
			bool CreateHandlers(
				TSharedRef<const FUeAgentImmutableCommandRegistry> Registry,
				TArray<TSharedRef<IUeAgentExactCommandHandler>>& Out,
				FString& OutError) const override
			{
				return CommandHandlers::ModelingGeometry::TryCreateHandlers(
					NativeMechanicsPort, Out, OutError);
			}
		private:
			TArray<FUeAgentCommandDeclaration> Declarations;
			TSharedRef<const DomainAdapters::ModelingGeometry::IUeAgentModelingSessionNativeMechanicsPort>
				NativeMechanicsPort;
		};
		const FUeAgentActiveCommandContributionFactoryRegistration Registration(
			TEXT("modeling-geometry"), &TryCreateContribution);

		bool TryCreateDomainCapability(
			TSharedPtr<const DomainCapability::FDomainCapabilityContribution>&
				OutContribution,
			FString& OutError)
		{
			DomainCapability::FDomainCapabilityDeclaration Declaration =
				DomainCapability::MakeExplicitlyUnsupportedDomainCapabilityDeclaration(
					TEXT("modeling-geometry"));
			Declaration.Operation.Support =
				DomainCapability::EDomainCapabilitySupport::Supported;
			for (const FString& OperationId
				: DomainAdapters::ModelingGeometry::GetCommandIds())
			{
				if (OperationId == TEXT("mesh.modeling_session")) continue;
				const TSharedPtr<const DomainCapability::
					IDomainOperationInvocationFactory> Factory =
						OperationOwners::ModelingGeometry::
							FindOperationInvocationFactory(OperationId);
				if (!Factory.IsValid())
				{
					OutError = TEXT("modeling_geometry_operation_factory_missing:")
						+ OperationId;
					return false;
				}
				DomainCapability::FDomainOperationEntry Operation;
				Operation.OperationId = OperationId;
				Operation.Kind = OperationId.EndsWith(TEXT(".create"))
					? DomainCapability::EDomainOperationKind::Create
					: DomainCapability::EDomainOperationKind::Write;
				Operation.TerminalPolicy = DomainCapability::
					EDomainOperationTerminalPolicy::
						SucceededRejectedRolledBackOrStateUnknown;
				Operation.ExecutionPolicy = DomainCapability::
					EDomainOperationExecutionPolicy::Immediate;
				const bool bTransient = OperationId
					== DomainAdapters::ModelingGeometry::VoxelGrid::
						GetSessionCommandId();
				Operation.TransientStatePolicy = bTransient
					? DomainCapability::EDomainTransientStatePolicy::RecoverableSession
					: DomainCapability::EDomainTransientStatePolicy::None;
				Operation.bPersists = !bTransient;
				Operation.bIndependentReadback = true;
				Operation.InvocationFactory = Factory;
				Declaration.Operation.Operations.Add(MoveTemp(Operation));
			}
			Declaration.State.Support =
				DomainCapability::EDomainCapabilitySupport::Supported;
			Declaration.State.Planes = {
				StateSemantic::EUeAgentStatePlane::AuthoringMemory,
				StateSemantic::EUeAgentStatePlane::Persisted,
			};
			Declaration.Migration.Support =
				DomainCapability::EDomainCapabilitySupport::Supported;
			Declaration.Migration.OwnerId =
				TEXT("TROOT-UAI-41-MODELING-WHITEBOX-PHYSICS-VERTICAL");
			return DomainCapability::FDomainCapabilityContribution::TryCreate(
				MoveTemp(Declaration), OutContribution, OutError);
		}

		const DomainCapability::FDomainCapabilityContributionFactoryRegistration
			DomainCapabilityRegistration(
				TEXT("modeling-geometry"), &TryCreateDomainCapability);
	}

	bool TryCreateContribution(
		TSharedPtr<IUeAgentActiveCommandContribution>& OutContribution,
		FString& OutError)
	{
		OutContribution.Reset(); OutError.Reset();
		TSharedPtr<const FUeAgentCommandContractRegistry> Legacy;
		if (!Composition::SimpleAuthoring::TryCreateProductionCommandContractRegistry(
				Legacy, OutError) || !Legacy.IsValid())
		{
			return false;
		}
		TArray<FUeAgentCommandDeclaration> Declarations;
		for (const FString& Id : DomainAdapters::ModelingGeometry::GetCommandIds())
		{
			if (Id == TEXT("mesh.modeling_session"))
			{
				Declarations.Add(ModelingSessionDeclaration());
				continue;
			}
			if (Id == DomainAdapters::ModelingGeometry::VoxelGrid::
				GetSessionCommandId())
			{
				Declarations.Add(VoxelGridSessionDeclaration());
				continue;
			}
			if (Id == DomainAdapters::ModelingGeometry::VoxelGrid::
				GetApplyCommandId())
			{
				Declarations.Add(VoxelGridApplyDeclaration());
				continue;
			}
			FUeAgentCommandDeclaration Declaration;
			if (!TryDeclaration(Id, *Legacy, Declaration, OutError)) return false;
			Declarations.Add(MoveTemp(Declaration));
		}
		if (Declarations.Num() != 34) return false;
		TSharedRef<const DomainAdapters::ModelingGeometry::IUeAgentModelingSessionNativeMechanicsPort>
			NativeMechanicsPort =
				DomainAdapters::ModelingGeometry::CreateModelingSessionNativeMechanicsPort();
		OutContribution = MakeShared<FContribution>(
			MoveTemp(Declarations), NativeMechanicsPort);
		return true;
	}
}

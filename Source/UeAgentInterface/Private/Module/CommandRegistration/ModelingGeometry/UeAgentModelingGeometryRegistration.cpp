#include "Module/CommandRegistration/ModelingGeometry/UeAgentModelingGeometryRegistration.h"

#include "Dom/JsonValue.h"

#include "CommandHandlers/ModelingGeometry/UeAgentModelingGeometryCommandHandlers.h"
#include "Composition/SimpleAuthoring/UeAgentSimpleAuthoringComposition.h"
#include "Core/Contract/UeAgentCommandContract.h"
#include "Core/Contract/UeAgentCommandJson.h"
#include "Core/DomainCapability/UeAgentDomainCapability.h"
#include "DomainAdapters/ModelingGeometry/UeAgentModelingGeometryMechanicsBoundary.h"
#include "DomainAdapters/ModelingGeometry/UeAgentModelingVoxelGrid.h"
#include "DomainAdapters/ModelingGeometryEffect/UeAgentModelingGeometryEffectCapabilities.h"
#include "Dom/JsonObject.h"
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
			const Contract::FUeAgentDescriptorRegistry& DescriptorRegistry,
			FUeAgentCommandDeclaration& Out,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			const FUeAgentCommandContract* WireContract = Legacy.FindExact(Id);
			if (!WireContract || !WireContract->TryValidate(OutError))
			{
				OutError = WireContract
					? UE::UAI::Contract::FDiagnostic::WrapFailure(TEXT("modeling_geometry_contract_missing"), OutError, TEXT("registration"))
					: UE::UAI::Contract::FDiagnostic::Failure(TEXT("modeling_geometry_contract_missing"), TEXT("No command contract is registered for this command."), TEXT("registration"), FString(), Id);
				if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = Id;
				return false;
			}
			const Contract::FUeAgentOperationDescriptor* Descriptor =
				DescriptorRegistry.FindByDescriptorId(
					TEXT("uai.operation.") + Id);
			if (!Descriptor || Descriptor->GetCapabilityId() != Id)
			{
				OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("modeling_geometry_descriptor_missing"), Descriptor ? TEXT("The operation descriptor capability does not match the command.") : TEXT("No operation descriptor was found for the command binding."), TEXT("registration"), FString(), Id);
				if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = Id;
				if (Descriptor)
				{
					OutError.PropertyPath = TEXT("capability_id");
					OutError.ExpectedValue = MakeShared<FJsonValueString>(Id);
					OutError.ActualValue = MakeShared<FJsonValueString>(Descriptor->GetCapabilityId());
				}
				return false;
			}
			TSharedPtr<FJsonObject> InputSchema;
			if (!Json::TryParseObject(
					Descriptor->GetInputSchema().GetJsonValue(),
					InputSchema,
					OutError)
				|| !InputSchema.IsValid())
			{
				OutError = UE::UAI::Contract::FDiagnostic::WrapFailure(TEXT("modeling_geometry_input_schema_invalid"), OutError, TEXT("registration"));
				OutError.SubjectId = Id;
				if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = Id;
				return false;
			}
			const TSharedPtr<FJsonObject>* Properties = nullptr;
			if (!InputSchema->TryGetObjectField(TEXT("properties"), Properties)
				|| !Properties
				|| !Properties->IsValid())
			{
				OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("modeling_geometry_input_schema_properties_missing"), TEXT("The operation input schema requires an object-valued properties field."), TEXT("registration"), FString(), Id);
				if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = Id;
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
			for (const FUeAgentCommandParameter& P : WireContract->GetParameters())
			{
				const TSharedPtr<FJsonObject>* ParameterSchema = nullptr;
				if (!(*Properties)->TryGetObjectField(
						P.GetName(),
						ParameterSchema)
					|| !ParameterSchema
					|| !ParameterSchema->IsValid())
				{
					OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("modeling_geometry_parameter_schema_missing"), TEXT("The command parameter has no object-valued schema in the operation descriptor."), TEXT("registration"), FString(), Id);
					if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = Id;
					OutError.PropertyPath = TEXT("properties.") + P.GetName();
					return false;
				}
				FString ParameterSchemaJson;
				if (!Json::TryCanonicalize(
						*ParameterSchema,
						ParameterSchemaJson,
						OutError))
				{
					OutError = UE::UAI::Contract::FDiagnostic::WrapFailure(TEXT("modeling_geometry_parameter_schema_invalid"), OutError, TEXT("registration"));
					OutError.SubjectId = Id;
					if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = Id;
					OutError.PropertyPath = TEXT("properties.") + P.GetName() + (OutError.PropertyPath.IsEmpty() ? FString() : TEXT(".") + OutError.PropertyPath);
					return false;
				}
				const FString ParameterWireType = WireType(P.GetWireType());
				Result.InputFields.Add({
					P.GetName(),
					ParameterWireType,
					P.IsRequired(),
					P.IsNullable(),
					FString::Printf(
						TEXT("Exact %s parameter; apply the attached JSON Schema constraints."),
						*ParameterWireType),
					MoveTemp(ParameterSchemaJson),
				});
			}
			Result.ResultFields = {
				Field(TEXT("operation_id"), TEXT("string"), true,
					TEXT("Stable identifier of the completed operation.")),
				Field(TEXT("projection"), TEXT("object"), true,
					TEXT("Verified native geometry result and evidence identities.")),
			};
			if (Id.EndsWith(TEXT(".create")))
			{
				Result.ModelResultFields = {
					{TEXT("status"), {
						TEXT("projection"), TEXT("payload"), TEXT("status")}},
					{TEXT("effect_outcome"), {
						TEXT("projection"), TEXT("payload"),
						TEXT("effect_outcome")}},
				};
			}
			Result.Errors = {
				{TEXT("modeling_geometry_authority_required"), TEXT("uai:operate is required.")},
				{TEXT("simple_authoring_input_invalid"), TEXT("The exact geometry schema rejects the request before mutation.")},
				{TEXT("operation_state_unknown"), TEXT("The final geometry state cannot be proven and must not be replayed.")},
			};
			Result.Purpose.Summary = TEXT("Execute ") + Id
				+ TEXT(" through the common OperationService and modeling geometry mechanics.");
			Result.Purpose.Target = TEXT("One exact mesh asset or geometry target declared by the command contract.");
			Result.Purpose.Preconditions = {TEXT("Resolve every mesh identity before mutation.")};
			Result.Purpose.ParameterGuidance = TEXT("Use the generated naming SDK method and its public parameters; apply every attached JSON Schema constraint exactly.");
			Result.Purpose.ResultGuidance = TEXT("Success requires verified terminal projection evidence.");
			Result.Purpose.FailureGuidance = TEXT("state_unknown is non-replayable.");
			Result.Purpose.SideEffectGuidance = TEXT("Uses the formal native modeling geometry effect.");
			Result.Purpose.MinimumExample = Result.PythonFunction + TEXT("(client, params)");
			Result.Purpose.RelatedFlows = {TEXT("query.command.usage"), TEXT("asset readback")};
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
			Result.Purpose.ParameterGuidance = TEXT("Use the generated naming SDK method and its public parameters; fields not owned by the selected action are rejected.");
			Result.Purpose.ResultGuidance = TEXT("Inspect terminal_status and the independent observation for the selected action.");
			Result.Purpose.FailureGuidance = TEXT("Input failures are not_started; state_unknown is non-replayable.");
			Result.Purpose.SideEffectGuidance = TEXT("Tool actions affect authoring memory; save_mesh_asset and declared commits require persisted readback.");
			Result.Purpose.MinimumExample = TEXT("client.mesh.modeling_session('observe_tool', None, {}, {}, {})");
			Result.Purpose.RelatedFlows = {TEXT("query.command.usage"), TEXT("operation.status"), TEXT("content_graph.summary")};
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
			Result.ContractVersion = 2;
			Result.PublicOwner = Owner;
			Result.Availability = EUeAgentCommandAvailability::Dormant;
			Result.SideEffect = EUeAgentCommandSideEffect::WorldState;
			Result.StateLayers = {
				TEXT("authoring_memory"), TEXT("durability_journal")};
			Result.InputFields = {
				Field(TEXT("action"), TEXT("string"), true,
					TEXT("begin, read, add_bounds, remove_bounds, add_shape, remove_shape, close, or cancel.")),
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
				Field(TEXT("shape"), TEXT("object"), false,
					TEXT("Exact solid: kind=box|square_pyramid|triangular_prism, explicit space=grid_local|component_local|world, center and positive size {x,y,z} in centimeters, optional rotation {pitch,yaw,roll} in degrees. Pyramid base is local -Z and apex +Z. Right triangular prism rises from -X to +X, extrudes along Y. add_shape unions volume; remove_shape cuts volume without cell rounding.")),
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
				{TEXT("voxel_grid_target_package_dirty"),
					TEXT("The exact Map or External Actor package was already dirty.")},
				{TEXT("voxel_grid_authoring_state_conflict"),
					TEXT("UE-owned state and a legacy active sidecar both exist; no source is guessed.")},
				{TEXT("voxel_grid_session_state_unknown"),
					TEXT("A prior rollback could not confirm the session state.")},
			};
			Result.Purpose.Summary =
				TEXT("Internal SDK continuation for one bounded VoxelGrid session.");
			Result.Purpose.Target =
				TEXT("One SDK-owned opaque VoxelGrid continuation and its managed DynamicMeshActor.");
			Result.Purpose.Preconditions = {
				TEXT("Only the SDK typed handle may retain and forward session identity and revision."),
				TEXT("Use an explicit coordinate space for every bounds mutation."),
			};
			Result.Purpose.ParameterGuidance =
				TEXT("SDK internal forwarding only; model-facing callers use the typed handle business methods.");
			Result.Purpose.ResultGuidance =
				TEXT("The SDK updates its private continuation from the lossless machine result.");
			Result.Purpose.FailureGuidance =
				TEXT("Do not replay stale or state_unknown mutations.");
			Result.Purpose.SideEffectGuidance =
				TEXT("Begin prepares C8D before mutation; close/cancel release it, cancel removes an uncommitted actor, and no viewport or window is changed.");
			Result.Purpose.MinimumExample =
				TEXT("SDK-internal typed-handle forwarding; no model-facing invocation.");
			Result.Purpose.RelatedFlows = {
				TEXT("mesh.voxel_grid.apply"), TEXT("query.command.usage")};
			Result.Purpose.Keywords = {
				TEXT("cubegrid"), TEXT("voxel"), TEXT("whitebox"), TEXT("blockout")};
			Result.Purpose.Synonyms = {TEXT("grid blockout session")};
			return Result;
		}

		FUeAgentCommandDeclaration VoxelGridBeginDeclaration()
		{
			FUeAgentCommandDeclaration Result;
			Result.CommandId = DomainAdapters::ModelingGeometry::VoxelGrid::
				GetBeginCommandId();
			Result.PythonNamespace = TEXT("mesh");
			Result.PythonFunction = TEXT("voxel_grid_begin");
			Result.ContractVersion = 1;
			Result.PublicOwner = Owner;
			Result.Availability = EUeAgentCommandAvailability::Enabled;
			Result.SideEffect = EUeAgentCommandSideEffect::WorldState;
			Result.StateLayers = {TEXT("authoring_memory")};
			Result.InputFields = {
				Field(TEXT("mode"), TEXT("string"), false,
					TEXT("create or edit; defaults to create.")),
				Field(TEXT("actor_ref"), TEXT("string"), false,
					TEXT("Existing DynamicMeshActor identity for edit.")),
				Field(TEXT("label"), TEXT("string"), false,
					TEXT("Optional label for a newly created actor.")),
				Field(TEXT("cell_size"), TEXT("number"), false,
					TEXT("Grid cell size in Unreal centimeters.")),
				Field(TEXT("origin"), TEXT("object"), false,
					TEXT("Grid-frame origin in world centimeters.")),
				Field(TEXT("rotation"), TEXT("object"), false,
					TEXT("Grid-frame UE pitch/yaw/roll in degrees.")),
				Field(TEXT("anchor"), TEXT("object"), false,
					TEXT("Actor anchor in grid-local centimeters.")),
			};
			Result.ResultFields = {
				Field(TEXT("action"), TEXT("string"), true,
					TEXT("The begin action.")),
				Field(TEXT("terminal_status"), TEXT("string"), true,
					TEXT("Observed begin status.")),
				Field(TEXT("readback"), TEXT("object"), true,
					TEXT("Bounded initial session observation.")),
				Field(TEXT("errors"), TEXT("array"), true,
					TEXT("Bounded structured errors.")),
				Field(TEXT("warnings"), TEXT("array"), true,
					TEXT("Bounded structured warnings.")),
			};
			Result.Errors = {
				{TEXT("modeling_geometry_authority_required"),
					TEXT("uai:operate authority is required.")},
				{TEXT("voxel_grid_begin_mode_unsupported"),
					TEXT("The begin mode must be create or edit.")},
				{TEXT("voxel_grid_actor_not_found"),
					TEXT("The edit actor could not be resolved uniquely.")},
			};
			Result.Purpose.Summary =
				TEXT("Begin one bounded CubeGrid-style voxel whitebox session.");
			Result.Purpose.Target =
				TEXT("A new or explicitly selected DynamicMeshActor grid target.");
			Result.Purpose.Preconditions = {
				TEXT("Provide only business begin fields; the service owns session identity.")};
			Result.Purpose.ParameterGuidance =
				TEXT("Use mode=create for a new grid or mode=edit with actor_ref for an existing grid.");
			Result.Purpose.ResultGuidance =
				TEXT("Use the typed handle: read, add_bounds/remove_bounds for cell-aligned blocks, add_shape/remove_shape for exact square_pyramid or triangular_prism volumes, apply, close or cancel. Shapes preserve slopes and exact convex collision. Machine continuation stays private.");
			Result.Purpose.FailureGuidance =
				TEXT("Do not retry a state_unknown result without reconciliation.");
			Result.Purpose.SideEffectGuidance =
				TEXT("Prepares transient authoring state; persistence is performed by voxel_grid.apply.");
			Result.Purpose.MinimumExample =
				TEXT("client.mesh.voxel_grid_begin(mode='create', cell_size=100)");
			Result.Purpose.RelatedFlows = {
				TEXT("query.command.usage")};
			Result.Purpose.Keywords = {
				TEXT("cubegrid"), TEXT("voxel"), TEXT("begin"), TEXT("pyramid"), TEXT("triangular prism"), TEXT("ramp")};
			Result.Purpose.Synonyms = {TEXT("grid blockout begin")};
			return Result;
		}

		FUeAgentCommandDeclaration VoxelGridApplyDeclaration()
		{
			FUeAgentCommandDeclaration Result;
			Result.CommandId = DomainAdapters::ModelingGeometry::VoxelGrid::
				GetApplyCommandId();
			Result.PythonNamespace = TEXT("mesh");
			Result.PythonFunction = TEXT("voxel_grid_apply");
			Result.ContractVersion = 2;
			Result.PublicOwner = Owner;
			Result.Availability = EUeAgentCommandAvailability::Dormant;
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
					TEXT("mode: exact_box_components for whole cells, exact_convex for precise solids, or none; omitted mode selects exact collision for the current geometry. profile_name defaults to BlockAll; enabled defaults to query_and_physics.")),
				Field(TEXT("material_ref"), TEXT("string"), false,
					TEXT("Optional exact MaterialInterface object path.")),
				Field(TEXT("color"), TEXT("array"), false,
					TEXT("Optional linear RGB [r,g,b], each in [0,1], for the built-in metric material. Stored per component.")),
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
				Field(TEXT("authoring_state"), TEXT("object"), true,
					TEXT("The unique serialized editor-only Actor component that owns grid authoring state.")),
				Field(TEXT("durability"), TEXT("object"), true,
					TEXT("C8D receipt identity and independent persisted-readback status.")),
				Field(TEXT("readback"), TEXT("object"), true,
					TEXT("Independent isolated-package Actor, state, mesh, collision, material, transform, and identity readback.")),
			};
			Result.Errors = {
				{TEXT("modeling_geometry_authority_required"),
					TEXT("uai:operate authority is required.")},
				{TEXT("voxel_grid_revision_mismatch"),
					TEXT("The session revision differs from expected_revision.")},
				{TEXT("voxel_grid_independent_readback_mismatch"),
					TEXT("Published Actor state and isolated persisted-package readback are not equivalent.")},
				{TEXT("voxel_grid_target_package_dirty"),
					TEXT("The exact Map or External Actor package was already dirty.")},
				{TEXT("voxel_grid_session_state_unknown"),
					TEXT("A prior rollback could not confirm the session state.")},
			};
			Result.Purpose.Summary =
				TEXT("Internal SDK apply for one VoxelGrid typed continuation.");
			Result.Purpose.Target =
				TEXT("The exact DynamicMeshActor owned by session_id.");
			Result.Purpose.Preconditions = {
				TEXT("Only the SDK typed handle may retain and forward session identity and revision."),
				TEXT("Use a collision box limit appropriate for the target cell set."),
			};
			Result.Purpose.ParameterGuidance =
				TEXT("SDK internal forwarding only; the command never saves unrelated dirty content.");
			Result.Purpose.ResultGuidance =
				TEXT("Success includes a C8D receipt and C9 isolated-package equivalence readback; the UE component is the only active grid-state owner.");
			Result.Purpose.FailureGuidance =
				TEXT("rolled_back is retryable after a fresh read; state_unknown requires reconciliation.");
			Result.Purpose.SideEffectGuidance =
				TEXT("Persists only the exact clean target Map/External Actor packages; a legacy sidecar is accepted once only when no component exists, then moved to a recoverable tombstone.");
			Result.Purpose.MinimumExample =
				TEXT("SDK-internal typed-handle forwarding; no model-facing invocation.");
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
				UE::UAI::Contract::FDiagnostic& OutError) const override
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
			UE::UAI::Contract::FDiagnostic& OutError)
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
					OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("modeling_geometry_operation_factory_missing"), TEXT("No operation invocation factory is registered for this operation."), TEXT("registration"), FString(), OperationId);
					if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = OperationId;
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
						GetBeginCommandId()
					|| OperationId
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
			return DomainCapability::FDomainCapabilityContribution::TryCreate(
				MoveTemp(Declaration), OutContribution, OutError);
		}

		const DomainCapability::FDomainCapabilityContributionFactoryRegistration
			DomainCapabilityRegistration(
				TEXT("modeling-geometry"), &TryCreateDomainCapability);
	}

	bool TryCreateContribution(
		TSharedPtr<IUeAgentActiveCommandContribution>& OutContribution,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutContribution.Reset(); OutError.Reset();
		TSharedPtr<const FUeAgentCommandContractRegistry> Legacy;
		if (!Composition::SimpleAuthoring::TryCreateProductionCommandContractRegistry(
				Legacy, OutError) || !Legacy.IsValid())
		{
			return false;
		}
		TSharedPtr<const Contract::FUeAgentDescriptorRegistry> DescriptorRegistry;
		if (!DomainAdapters::ModelingGeometryEffect::
				TryCreateModelingGeometryEffectDescriptorRegistry(
					DescriptorRegistry,
					OutError)
			|| !DescriptorRegistry.IsValid())
		{
			return false;
		}
		TArray<FUeAgentCommandDeclaration> Declarations;
		for (const FString& Id : DomainAdapters::ModelingGeometry::GetCommandIds())
		{
			if (Id == DomainAdapters::ModelingGeometry::VoxelGrid::
				GetBeginCommandId())
			{
				Declarations.Add(VoxelGridBeginDeclaration());
				continue;
			}
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
			if (!TryDeclaration(
					Id,
					*Legacy,
					*DescriptorRegistry,
					Declaration,
					OutError))
			{
				return false;
			}
			Declarations.Add(MoveTemp(Declaration));
		}
		TSharedRef<const DomainAdapters::ModelingGeometry::IUeAgentModelingSessionNativeMechanicsPort>
			NativeMechanicsPort =
				DomainAdapters::ModelingGeometry::CreateModelingSessionNativeMechanicsPort();
		OutContribution = MakeShared<FContribution>(
			MoveTemp(Declarations), NativeMechanicsPort);
		return true;
	}
}

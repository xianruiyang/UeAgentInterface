#include "Module/CommandRegistration/ModelingGeometry/UeAgentModelingGeometryRegistration.h"

#include "CommandHandlers/ModelingGeometry/UeAgentModelingGeometryCommandHandlers.h"
#include "Composition/SimpleAuthoring/UeAgentSimpleAuthoringComposition.h"
#include "Core/Contract/UeAgentCommandContract.h"
#include "DomainAdapters/ModelingGeometry/UeAgentModelingGeometryMechanicsBoundary.h"

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
			FUeAgentCommandDeclaration Declaration;
			if (!TryDeclaration(Id, *Legacy, Declaration, OutError)) return false;
			Declarations.Add(MoveTemp(Declaration));
		}
		if (Declarations.Num() != 32) return false;
		TSharedRef<const DomainAdapters::ModelingGeometry::IUeAgentModelingSessionNativeMechanicsPort>
			NativeMechanicsPort =
				DomainAdapters::ModelingGeometry::CreateModelingSessionNativeMechanicsPort();
		OutContribution = MakeShared<FContribution>(
			MoveTemp(Declarations), NativeMechanicsPort);
		return true;
	}
}

#include "Module/CommandRegistration/ViewportGraph/UeAgentViewportGraphRegistration.h"

#include "CommandHandlers/ViewportGraph/UeAgentViewportGraphCommandHandlers.h"
#include "Composition/TimelineSpatial/UeAgentTimelineSpatialActivation.h"
#include "Core/DomainCapability/UeAgentDomainCapability.h"
#include "OperationOwners/ViewportGraph/UeAgentViewportGraphOperationOwner.h"
#include "QueryOwners/ViewportGraph/UeAgentViewportGraphQueryOwner.h"

namespace UE::UAI::Module::CommandRegistration::ViewportGraph
{
	using namespace Active;
	using namespace CommandContract;
	using namespace CommandEdge;
	using namespace CommandRegistry;

	namespace
	{
		const FString Owner(TEXT("viewport-graph-control-owner"));

		FString WireType(const EUeAgentCommandWireType Type)
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

		bool TryDeclaration(
			const FString& CommandId,
			const FUeAgentCommandContractRegistry& Legacy,
			FUeAgentCommandDeclaration& OutDeclaration,
			FString& OutError)
		{
			const FUeAgentCommandContract* Contract = Legacy.FindExact(CommandId);
			if (!Contract || !Contract->IsValid())
			{
				OutError = TEXT("viewport_graph_contract_missing:") + CommandId;
				return false;
			}
			const bool bQuery = QueryOwners::ViewportGraph::IsQuery(CommandId);
			FUeAgentCommandDeclaration Result;
			Result.CommandId = CommandId;
			Result.PythonNamespace = TEXT("viewport_graph");
			Result.PythonFunction = CommandId.Replace(TEXT("."), TEXT("_"));
			Result.ContractVersion = 1;
			Result.PublicOwner = Owner;
			Result.Availability = EUeAgentCommandAvailability::Enabled;
			Result.SideEffect = bQuery
				? EUeAgentCommandSideEffect::None
				: EUeAgentCommandSideEffect::EditorState;
			Result.StateLayers = {
				bQuery ? FString(TEXT("editor_current_state"))
					: FString(TEXT("editor_transient_state")),
			};
			for (const FUeAgentCommandParameter& Parameter
				: Contract->GetParameters())
			{
				Result.InputFields.Add({
					Parameter.GetName(), WireType(Parameter.GetWireType()),
					Parameter.IsRequired(), Parameter.IsNullable(),
					TEXT("Exact viewport or graph mechanics parameter."),
				});
			}
			Result.ResultFields = {{
				TEXT("result"), TEXT("object"), true, false,
				TEXT("Native viewport or graph result with state revision."),
			}};
			Result.Errors = {
				{TEXT("viewport_graph_authority_required"),
					TEXT("The caller lacks Query or Operation authority.")},
				{TEXT("timeline_spatial_request_invalid"),
					TEXT("The exact viewport request is invalid.")},
				{TEXT("viewport_graph_runtime_unavailable"),
					TEXT("The native Editor viewport mechanics are unavailable.")},
				{TEXT("operation_state_unknown"),
					TEXT("A transient-state mutation requires reconciliation and restore.")},
			};
			Result.Purpose.Summary = TEXT("Execute ") + CommandId
				+ TEXT(" through the common Query/Operation service and native viewport mechanics.");
			Result.Purpose.Target =
				TEXT("One exact graph view, viewport client, actor selection, ray, or transient Editor state.");
			Result.Purpose.Preconditions = {
				TEXT("Resolve the exact viewport context and capture recoverable state before mutation."),
			};
			Result.Purpose.ParameterGuidance =
				TEXT("Use only query.command.detail fields and explicit viewport identities.");
			Result.Purpose.ResultGuidance =
				TEXT("Use the returned state revision and a separate read command for verification.");
			Result.Purpose.FailureGuidance =
				TEXT("Read failures are side-effect free; mutation failures preserve restore semantics.");
			Result.Purpose.SideEffectGuidance = bQuery
				? TEXT("Reads only bounded Editor or viewport state.")
				: TEXT("Mutates only recoverable Editor transient state and must be restored.");
			Result.Purpose.MinimumExample = Result.PythonFunction
				+ TEXT("(client, params)");
			Result.Purpose.RelatedFlows =
				QueryOwners::ViewportGraph::GetReadbackFlows();
			Result.Purpose.Keywords = {
				TEXT("viewport"), TEXT("graph"), TEXT("editor"), TEXT("control"),
			};
			Result.Purpose.Synonyms = {
				TEXT("viewport state"), TEXT("graph view"), TEXT("camera control"),
			};
			OutDeclaration = MoveTemp(Result);
			return true;
		}

		class FContribution final : public IUeAgentActiveCommandContribution
		{
		public:
			explicit FContribution(TArray<FUeAgentCommandDeclaration> InDeclarations)
				: Declarations(MoveTemp(InDeclarations))
			{
			}
			const FString& GetContributionId() const override
			{
				static const FString Id(TEXT("viewport-graph-control"));
				return Id;
			}
			void CollectDeclarations(
				TArray<FUeAgentCommandDeclaration>& OutDeclarations) const override
			{
				OutDeclarations.Append(Declarations);
			}
			bool CreateHandlers(
				TSharedRef<const FUeAgentImmutableCommandRegistry> Registry,
				TArray<TSharedRef<IUeAgentExactCommandHandler>>& OutHandlers,
				FString& OutError) const override
			{
				return CommandHandlers::ViewportGraph::TryCreateHandlers(
					OutHandlers,
					OutError);
			}
		private:
			TArray<FUeAgentCommandDeclaration> Declarations;
		};

		bool TryCreateDomainCapability(
			TSharedPtr<const DomainCapability::FDomainCapabilityContribution>&
				OutContribution,
			FString& OutError)
		{
			TSharedPtr<IUeAgentActiveCommandContribution> CommandContribution;
			if (!TryCreateContribution(CommandContribution, OutError)
				|| !CommandContribution.IsValid())
			{
				return false;
			}
			DomainCapability::FDomainCapabilityDeclaration Declaration =
				DomainCapability::MakeExplicitlyUnsupportedDomainCapabilityDeclaration(
					TEXT("viewport-graph-control"));
			Declaration.Command.Support =
				DomainCapability::EDomainCapabilitySupport::Supported;
			Declaration.Command.Contributions.Add(
				CommandContribution.ToSharedRef());
			Declaration.Operation.Support =
				DomainCapability::EDomainCapabilitySupport::Supported;
			for (const FString& CommandId
				: QueryOwners::ViewportGraph::GetCommandIds())
			{
				if (QueryOwners::ViewportGraph::IsQuery(CommandId))
				{
					continue;
				}
				const TSharedPtr<const DomainCapability::
					IDomainOperationInvocationFactory> Factory =
					OperationOwners::ViewportGraph::
						FindOperationInvocationFactory(CommandId);
				if (!Factory.IsValid())
				{
					OutError = TEXT("viewport_graph_operation_factory_missing:")
						+ CommandId;
					return false;
				}
				DomainCapability::FDomainOperationEntry Operation;
				Operation.OperationId = CommandId;
				Operation.Kind = DomainCapability::EDomainOperationKind::Write;
				Operation.TerminalPolicy = DomainCapability::
					EDomainOperationTerminalPolicy::
						SucceededRejectedRolledBackOrStateUnknown;
				Operation.ExecutionPolicy = DomainCapability::
					EDomainOperationExecutionPolicy::Immediate;
				Operation.TransientStatePolicy = DomainCapability::
					EDomainTransientStatePolicy::RecoverableSession;
				Operation.bPersists = false;
				Operation.bIndependentReadback = true;
				Operation.InvocationFactory = Factory;
				Declaration.Operation.Operations.Add(MoveTemp(Operation));
			}
			Declaration.State.Support =
				DomainCapability::EDomainCapabilitySupport::Supported;
			Declaration.State.Planes.Add(
				StateSemantic::EUeAgentStatePlane::AuthoringMemory);
			Declaration.Migration.Support =
				DomainCapability::EDomainCapabilitySupport::Supported;
			Declaration.Migration.OwnerId =
				TEXT("T45-VIEWPORT-GRAPH-CONTROL");
			return DomainCapability::FDomainCapabilityContribution::TryCreate(
				MoveTemp(Declaration), OutContribution, OutError);
		}

		const FUeAgentActiveCommandContributionFactoryRegistration Registration(
			TEXT("viewport-graph-control"),
			&TryCreateContribution);
		const DomainCapability::
			FDomainCapabilityContributionFactoryRegistration
			DomainCapabilityRegistration(
				TEXT("viewport-graph-control"), &TryCreateDomainCapability);
	}

	bool TryCreateContribution(
		TSharedPtr<IUeAgentActiveCommandContribution>& OutContribution,
		FString& OutError)
	{
		OutContribution.Reset();
		OutError.Reset();
		TSharedPtr<const FUeAgentCommandContractRegistry> Legacy;
		if (!Composition::TimelineSpatial::
				TryCreateTimelineSpatialProductionCommandContractRegistry(
					Legacy,
					OutError)
			|| !Legacy.IsValid())
		{
			return false;
		}
		TArray<FUeAgentCommandDeclaration> Declarations;
		for (const FString& CommandId : QueryOwners::ViewportGraph::GetCommandIds())
		{
			FUeAgentCommandDeclaration Declaration;
			if (!TryDeclaration(CommandId, *Legacy, Declaration, OutError))
			{
				return false;
			}
			Declarations.Add(MoveTemp(Declaration));
		}
		OutContribution = MakeShared<FContribution>(MoveTemp(Declarations));
		return true;
	}
}

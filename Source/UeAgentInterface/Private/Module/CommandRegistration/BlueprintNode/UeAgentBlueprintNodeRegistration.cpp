#include "Module/CommandRegistration/BlueprintNode/UeAgentBlueprintNodeRegistration.h"

#include "CommandHandlers/BlueprintNode/UeAgentBlueprintNodeCommandHandlers.h"

namespace UE::UAI::Module::CommandRegistration::BlueprintNode
{
	using namespace UE::UAI::CommandContract;
	using namespace UE::UAI::CommandEdge;
	using namespace UE::UAI::CommandHandlers::BlueprintNode;
	using namespace UE::UAI::CommandRegistry;
	using namespace UE::UAI::Module::CommandRegistration::Active;
	using namespace UE::UAI::OperationOwners::BlueprintNode;

	namespace
	{
		const FString CommandId = TEXT("blueprint.node.candidates");
		const FString Owner = TEXT("blueprint-node-authoring-owner");

		FUeAgentCommandFieldDeclaration Field(
			const TCHAR* Name,
			const TCHAR* WireType,
			const bool bRequired,
			const TCHAR* Description)
		{
			return {Name, WireType, bRequired, false, Description};
		}

		FUeAgentCommandDeclaration Declaration()
		{
			FUeAgentCommandDeclaration Result;
			Result.CommandId = CommandId;
			Result.PythonNamespace = TEXT("blueprint");
			Result.PythonFunction = TEXT("node_candidates");
			Result.ContractVersion = 2;
			Result.PublicOwner = Owner;
			Result.Availability = EUeAgentCommandAvailability::Enabled;
			Result.SideEffect = EUeAgentCommandSideEffect::None;
			Result.StateLayers = {TEXT("asset_persisted_state")};
			Result.InputFields = {
				Field(TEXT("target"), TEXT("string"), true,
					TEXT("Blueprint name returned by content search, or an exact Blueprint package.object path.")),
				Field(TEXT("graph_name"), TEXT("string"), true,
					TEXT("Readable graph name copied from the settled Blueprint structure.")),
				Field(TEXT("query"), TEXT("string"), false,
					TEXT("Optional candidate-name or display-name filter.")),
				Field(TEXT("node_name"), TEXT("string"), false,
					TEXT("Optional readable source-node name for a pin-drag context.")),
				Field(TEXT("node_occurrence"), TEXT("integer"), false,
					TEXT("Optional zero-based occurrence when node_name is repeated in the graph.")),
				Field(TEXT("pin_name"), TEXT("string"), false,
					TEXT("Optional readable source-pin name; requires node_name.")),
				Field(TEXT("pin_occurrence"), TEXT("integer"), false,
					TEXT("Optional zero-based occurrence when pin_name is repeated on the node.")),
			};
			Result.ResultFields = {
				Field(TEXT("scope"), TEXT("object"), true,
					TEXT("Public target and graph_name for this query; response context, not a node selector. Pin context is in candidate_context when present.")),
				Field(TEXT("names"), TEXT("array"), true,
					TEXT("Distinct readable candidate names accepted by Blueprint HJSON creation in the same context.")),
				Field(TEXT("candidate_context"), TEXT("object"), false,
					TEXT("For a pin-context query, copy this node/pin alias object unchanged beside the new HJSON node's candidate.")),
			};
			Result.Errors = {
				{TEXT("blueprint_node_query_authority_required"),
					TEXT("The uai:query scope is missing.")},
				{TEXT("blueprint_node_target_unresolved"),
					TEXT("The exact Blueprint identity could not be resolved.")},
				{TEXT("blueprint_node_graph_unresolved"),
					TEXT("The readable graph name could not be resolved.")},
				{TEXT("blueprint_node_graph_ambiguous"),
					TEXT("The readable graph name is repeated and cannot identify one graph.")},
				{TEXT("blueprint_node_candidate_unresolved"),
					TEXT("The real Blueprint action database has no supported candidate.")},
				{TEXT("blueprint_node_candidate_query_invalid"),
					TEXT("The requested readable occurrence is invalid.")},
				{TEXT("blueprint_node_pin_context_incomplete"),
					TEXT("A pin-drag context requires both node_name and pin_name.")},
				{TEXT("blueprint_node_context_ambiguous"),
					TEXT("The readable node name is repeated; provide node_occurrence.")},
				{TEXT("blueprint_node_context_not_found"),
					TEXT("The readable node name does not identify a current graph node.")},
				{TEXT("blueprint_pin_context_ambiguous"),
					TEXT("The readable pin name is repeated; provide pin_occurrence.")},
				{TEXT("blueprint_pin_context_not_found"),
					TEXT("The readable pin name does not identify a current node pin.")},
			};
			Result.Purpose.Summary =
				TEXT("Resolve exact Blueprint node creation candidates for one persisted graph.");
			Result.Purpose.Target =
				TEXT("One exact Blueprint graph identity.");
			Result.Purpose.Preconditions = {
				TEXT("Copy graph_name from the settled Blueprint structure."),
				TEXT("For a pin-drag context, use readable node and pin names from the settled graph interface."),
			};
			Result.Purpose.ParameterGuidance =
				TEXT("Search the intended target and graph or readable pin context. Copy one returned name into candidate.name, and returned candidate_context beside candidate when present. Add node_occurrence or pin_occurrence only when that readable name is repeated.");
			Result.Purpose.ResultGuidance =
				TEXT("Reuse names within the returned scope. When target, graph or pin context changes, query the needed names there. scope is response metadata; only candidate.name and optional candidate_context enter the new node. Creation re-enumerates the same native context.");
			Result.Purpose.FailureGuidance =
				TEXT("Unresolved or ambiguous candidates fail before any write and never fall back to a class alias.");
			Result.Purpose.SideEffectGuidance =
				TEXT("Read-only with no persistent or external side effects.");
			Result.Purpose.MinimumExample =
				TEXT("client.blueprint.node_candidates(target, graph_name='EventGraph', query='FInterp To')");
			Result.Purpose.RelatedFlows = {
				TEXT("query.command.search"),
				TEXT("content_graph.structure"),
				TEXT("content_graph.import_workspace"),
			};
			Result.Purpose.Keywords = {
				TEXT("blueprint"), TEXT("node"), TEXT("candidate"),
				TEXT("hjson"), TEXT("graph"),
			};
			Result.Purpose.Synonyms = {
				TEXT("find Blueprint node"),
				TEXT("resolve graph action"),
			};
			return Result;
		}

		class FContribution final : public IUeAgentActiveCommandContribution
		{
		public:
			explicit FContribution(
				TSharedRef<IUeAgentBlueprintNodeCommandService> InService)
				: Service(MoveTemp(InService))
			{
			}

			virtual const FString& GetContributionId() const override
			{
				static const FString Id = TEXT("blueprint-node");
				return Id;
			}

			virtual void CollectDeclarations(
				TArray<FUeAgentCommandDeclaration>& OutDeclarations) const
				override
			{
				OutDeclarations.Add(Declaration());
			}

			virtual bool CreateHandlers(
				TSharedRef<const FUeAgentImmutableCommandRegistry> Registry,
				TArray<TSharedRef<IUeAgentExactCommandHandler>>& OutHandlers,
				UE::UAI::Contract::FDiagnostic& OutError) const override
			{
				return TryCreateBlueprintNodeCommandHandlers(
					Service, OutHandlers, OutError);
			}

		private:
			TSharedRef<IUeAgentBlueprintNodeCommandService> Service;
		};

		const FUeAgentActiveCommandContributionFactoryRegistration
			BlueprintNodeContributionRegistration(
				TEXT("blueprint-node"),
				&TryCreateProductionBlueprintNodeContribution);
	}

	bool TryCreateBlueprintNodeContribution(
		TSharedRef<IUeAgentBlueprintNodeCommandService> Service,
		TSharedPtr<IUeAgentActiveCommandContribution>& OutContribution,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutContribution = MakeShared<FContribution>(MoveTemp(Service));
		OutError.Reset();
		return true;
	}

	bool TryCreateProductionBlueprintNodeContribution(
		TSharedPtr<IUeAgentActiveCommandContribution>& OutContribution,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		TSharedPtr<IUeAgentBlueprintNodeCommandService> Service;
		if (!TryCreateProductionBlueprintNodeCommandService(Service, OutError))
		{
			OutContribution.Reset();
			return false;
		}
		return TryCreateBlueprintNodeContribution(
			Service.ToSharedRef(), OutContribution, OutError);
	}

	bool TryCreateBlueprintNodeRegistration(
		TSharedRef<IUeAgentBlueprintNodeCommandService> Service,
		TSharedPtr<const FUeAgentImmutableCommandRegistry>& OutRegistry,
		TSharedPtr<const FUeAgentExactCommandDispatch>& OutDispatch,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutRegistry.Reset();
		OutDispatch.Reset();
		OutError.Reset();
		TSharedPtr<IUeAgentActiveCommandContribution> Contribution;
		FUeAgentActiveCommandComposition Composition;
		if (!TryCreateBlueprintNodeContribution(
				MoveTemp(Service), Contribution, OutError)
			|| !Composition.TryAdd(Contribution.ToSharedRef(), OutError)
			|| !Composition.TryCompose(OutRegistry, OutDispatch, OutError))
		{
			OutRegistry.Reset();
			OutDispatch.Reset();
			return false;
		}
		return true;
	}
}

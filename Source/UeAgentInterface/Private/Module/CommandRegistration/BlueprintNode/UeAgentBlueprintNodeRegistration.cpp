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
			Result.ContractVersion = 1;
			Result.PublicOwner = Owner;
			Result.Availability = EUeAgentCommandAvailability::Enabled;
			Result.SideEffect = EUeAgentCommandSideEffect::None;
			Result.StateLayers = {TEXT("asset_persisted_state")};
			Result.InputFields = {
				Field(TEXT("target"), TEXT("string"), true,
					TEXT("Exact public Blueprint package.object identity copied from content search.")),
				Field(TEXT("graph_id"), TEXT("string"), true,
					TEXT("Exact stable graph identity copied from content_graph.structure.")),
				Field(TEXT("query"), TEXT("string"), false,
					TEXT("Optional bounded candidate-name or display-name filter.")),
			};
			Result.ResultFields = {
				Field(TEXT("command_id"), TEXT("string"), true,
					TEXT("The unchanged exact public command identity.")),
				Field(TEXT("read_only"), TEXT("boolean"), true,
					TEXT("Always true.")),
				Field(TEXT("target"), TEXT("string"), true,
					TEXT("The unchanged exact Blueprint identity.")),
				Field(TEXT("graph"), TEXT("object"), true,
					TEXT("Resolved stable graph identity and kind.")),
				Field(TEXT("graph_revision"), TEXT("string"), true,
					TEXT("Current persisted graph revision for optimistic fencing.")),
				Field(TEXT("candidates"), TEXT("array"), true,
					TEXT("Real action-database candidates; candidate.name is an exact creation identity.")),
				Field(TEXT("complete"), TEXT("boolean"), true,
					TEXT("Truthful completeness for the supported candidate set.")),
				Field(TEXT("has_more"), TEXT("boolean"), true,
					TEXT("Always false for the bounded supported candidate set.")),
			};
			Result.Errors = {
				{TEXT("blueprint_node_query_authority_required"),
					TEXT("The uai:query scope is missing.")},
				{TEXT("blueprint_node_target_unresolved"),
					TEXT("The exact Blueprint identity could not be resolved.")},
				{TEXT("blueprint_node_graph_unresolved"),
					TEXT("The exact stable graph identity could not be resolved.")},
				{TEXT("blueprint_node_candidate_unresolved"),
					TEXT("The real Blueprint action database has no supported candidate.")},
			};
			Result.Purpose.Summary =
				TEXT("Resolve exact Blueprint node creation candidates for one persisted graph.");
			Result.Purpose.Target =
				TEXT("One exact Blueprint graph identity.");
			Result.Purpose.Preconditions = {
				TEXT("Copy target and graph_id unchanged from public content search and graph query."),
				TEXT("Invoke the exact contract hash on the UE game thread."),
			};
			Result.Purpose.ParameterGuidance =
				TEXT("Use candidate.name and binding_revision unchanged in the HJSON edit.");
			Result.Purpose.ResultGuidance =
				TEXT("Treat graph_revision, candidate.name and binding_revision as opaque exact identities.");
			Result.Purpose.FailureGuidance =
				TEXT("Unresolved or ambiguous candidates fail before any write and never fall back to a class alias.");
			Result.Purpose.SideEffectGuidance =
				TEXT("Read-only with no persistent or external side effects.");
			Result.Purpose.MinimumExample =
				TEXT("client.blueprint.node_candidates(target, graph_id, query='branch')");
			Result.Purpose.RelatedFlows = {
				TEXT("query.command.search"),
				TEXT("query.command.detail"),
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
				FString& OutError) const override
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
		FString& OutError)
	{
		OutContribution = MakeShared<FContribution>(MoveTemp(Service));
		OutError.Reset();
		return true;
	}

	bool TryCreateProductionBlueprintNodeContribution(
		TSharedPtr<IUeAgentActiveCommandContribution>& OutContribution,
		FString& OutError)
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
		FString& OutError)
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

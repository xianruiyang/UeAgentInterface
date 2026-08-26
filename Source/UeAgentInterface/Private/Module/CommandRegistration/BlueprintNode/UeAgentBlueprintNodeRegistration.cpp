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
					TEXT("Exact public Blueprint package.object identity copied from content search.")),
				Field(TEXT("graph_id"), TEXT("string"), true,
					TEXT("Exact stable graph identity copied from content_graph.structure.")),
				Field(TEXT("query"), TEXT("string"), false,
					TEXT("Optional bounded candidate-name or display-name filter.")),
				Field(TEXT("id"), TEXT("string"), false,
					TEXT("Optional exact candidate id returned by an earlier page.")),
				Field(TEXT("node_guid"), TEXT("string"), false,
					TEXT("Optional exact source-node GUID for a pin-drag menu context.")),
				Field(TEXT("pin_guid"), TEXT("string"), false,
					TEXT("Optional exact source-pin GUID for a pin-drag menu context.")),
				Field(TEXT("pin_name"), TEXT("string"), false,
					TEXT("Optional source-pin name when pin_guid is unavailable.")),
				Field(TEXT("limit"), TEXT("integer"), false,
					TEXT("Optional page size from 1 through 20; defaults to 20.")),
				Field(TEXT("cursor"), TEXT("integer"), false,
					TEXT("Optional non-negative result offset; defaults to zero.")),
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
				Field(TEXT("context"), TEXT("object"), true,
					TEXT("Exact graph and optional pin menu context used for enumeration.")),
				Field(TEXT("candidates"), TEXT("array"), true,
					TEXT("Native Action Menu candidates; candidate.name or candidate.id is a direct HJSON creation selector.")),
				Field(TEXT("total_matching"), TEXT("integer"), true,
					TEXT("Total native menu candidates matching the current query.")),
				Field(TEXT("next_cursor"), TEXT("integer"), true,
					TEXT("Next result offset, or -1 when complete.")),
				Field(TEXT("complete"), TEXT("boolean"), true,
					TEXT("True only when this is the final page of the native menu set.")),
				Field(TEXT("has_more"), TEXT("boolean"), true,
					TEXT("True only while another page remains.")),
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
				{TEXT("blueprint_node_candidate_page_invalid"),
					TEXT("The requested limit or cursor is outside the bounded contract.")},
				{TEXT("node_guid_invalid"),
					TEXT("The source-node GUID is not a valid GUID.")},
				{TEXT("node_not_found"),
					TEXT("The requested source node does not exist in this graph.")},
				{TEXT("pin_guid_invalid"),
					TEXT("The source-pin GUID is not a valid GUID.")},
				{TEXT("pin_selector_required"),
					TEXT("node_guid requires pin_guid or pin_name because node-only context is not a native Action Menu scope.")},
				{TEXT("pin_not_found"),
					TEXT("The requested source pin does not exist in this graph context.")},
				{TEXT("pin_ambiguous"),
					TEXT("The pin name matches more than one pin; provide node_guid or pin_guid.")},
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
				TEXT("Search the exact graph or pin context, continue paging while has_more is true, then copy either candidate.name or candidate.id into the HJSON candidate selector.");
			Result.Purpose.ResultGuidance =
				TEXT("candidate.name is sufficient only when unique in this exact context; candidate.id is the exact compact selector. Internal spawner and revision data are resolved server-side.");
			Result.Purpose.FailureGuidance =
				TEXT("Unresolved or ambiguous candidates fail before any write and never fall back to a class alias.");
			Result.Purpose.SideEffectGuidance =
				TEXT("Read-only with no persistent or external side effects.");
			Result.Purpose.MinimumExample =
				TEXT("client.blueprint.node_candidates(target, graph_id, query='FInterpTo', limit=10)");
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

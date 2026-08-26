#include "Module/CommandRegistration/BlueprintComponent/UeAgentBlueprintComponentRegistration.h"

#include "CommandHandlers/BlueprintComponent/UeAgentBlueprintComponentCommandHandlers.h"

namespace UE::UAI::Module::CommandRegistration::BlueprintComponent
{
	using namespace UE::UAI::CommandContract;
	using namespace UE::UAI::CommandEdge;
	using namespace UE::UAI::CommandHandlers::BlueprintComponent;
	using namespace UE::UAI::CommandRegistry;
	using namespace UE::UAI::Module::CommandRegistration::Active;
	using namespace UE::UAI::OperationOwners::BlueprintComponent;

	namespace
	{
		const FString CommandId = TEXT("blueprint.component.candidates");
		const FString Owner = TEXT("blueprint-component-authoring-owner");

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
			Result.PythonFunction = TEXT("component_candidates");
			Result.ContractVersion = 2;
			Result.PublicOwner = Owner;
			Result.Availability = EUeAgentCommandAvailability::Enabled;
			Result.SideEffect = EUeAgentCommandSideEffect::None;
			Result.StateLayers = {TEXT("asset_persisted_state")};
			Result.InputFields = {
				Field(TEXT("target"), TEXT("string"), true,
					TEXT("Exact public Blueprint package.object identity copied from content search.")),
				Field(TEXT("parent"), TEXT("string"), false,
					TEXT("Optional exact SCS parent variable name; actor root is used when omitted.")),
				Field(TEXT("query"), TEXT("string"), false,
					TEXT("Optional bounded Add Component menu search text.")),
				Field(TEXT("id"), TEXT("string"), false,
					TEXT("Optional exact candidate id returned by an earlier page.")),
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
				Field(TEXT("scs_revision"), TEXT("string"), true,
					TEXT("Current persisted SCS revision for optimistic fencing.")),
				Field(TEXT("context"), TEXT("object"), true,
					TEXT("Exact parent and deterministic content-selection policy used for enumeration.")),
				Field(TEXT("candidates"), TEXT("array"), true,
					TEXT("Native Add Component entries; candidate.name or candidate.id is a direct HJSON creation selector.")),
				Field(TEXT("total_matching"), TEXT("integer"), true,
					TEXT("Total native registry candidates matching the current query.")),
				Field(TEXT("next_cursor"), TEXT("integer"), true,
					TEXT("Next result offset, or -1 when complete.")),
				Field(TEXT("existing_components"), TEXT("array"), true,
					TEXT("Independent current SCS readback used after reload.")),
				Field(TEXT("complete"), TEXT("boolean"), true,
					TEXT("True only when this is the final page of the native menu set.")),
				Field(TEXT("has_more"), TEXT("boolean"), true,
					TEXT("True only while another page remains.")),
			};
			Result.Errors = {
				{TEXT("blueprint_component_query_authority_required"),
					TEXT("The uai:query scope is missing.")},
				{TEXT("blueprint_component_target_unresolved"),
					TEXT("The exact Blueprint identity could not be resolved.")},
				{TEXT("blueprint_component_parent_unresolved"),
					TEXT("The exact SCS parent does not exist in this Blueprint.")},
				{TEXT("blueprint_component_candidate_page_invalid"),
					TEXT("The requested limit or cursor is outside the bounded contract.")},
				{TEXT("blueprint_component_candidate_query_failed"),
					TEXT("The native Add Component registry could not be enumerated.")},
			};
			Result.Purpose.Summary =
				TEXT("Search the native Add Component menu for one Blueprint SCS context.");
			Result.Purpose.Target =
				TEXT("One exact persisted Blueprint SCS graph.");
			Result.Purpose.Preconditions = {
				TEXT("Copy target unchanged from public Blueprint asset creation or search."),
				TEXT("Provide parent only when reproducing a selected SCS parent context."),
			};
			Result.Purpose.ParameterGuidance =
				TEXT("Search and page the exact native registry, then copy either candidate.name or candidate.id into the HJSON candidate selector; the stable map key remains the component instance name.");
			Result.Purpose.ResultGuidance =
				TEXT("candidate.name is sufficient only when unique in this registry; candidate.id preserves class, asset override and native action identity. class_path and asset_path are optional diagnostics, not required creation fields.");
			Result.Purpose.FailureGuidance =
				TEXT("Unknown, ambiguous or stale selectors fail before mutation; there is no class-only fallback.");
			Result.Purpose.SideEffectGuidance =
				TEXT("Read-only with no persistent or external side effects.");
			Result.Purpose.MinimumExample =
				TEXT("client.blueprint.component_candidates(target=target, parent='DefaultSceneRoot', query='Point Light', limit=10)");
			Result.Purpose.RelatedFlows = {
				TEXT("blueprint.asset.create"),
				TEXT("content_graph.import_workspace"),
				TEXT("blueprint.node.candidates"),
			};
			Result.Purpose.Keywords = {
				TEXT("blueprint"), TEXT("component"), TEXT("candidate"),
				TEXT("scs"), TEXT("hjson"),
			};
			Result.Purpose.Synonyms = {
				TEXT("add Blueprint Component"),
				TEXT("resolve SCS Component"),
			};
			return Result;
		}

		class FContribution final : public IUeAgentActiveCommandContribution
		{
		public:
			explicit FContribution(
				TSharedRef<IUeAgentBlueprintComponentCommandService> InService)
				: Service(MoveTemp(InService))
			{
			}

			virtual const FString& GetContributionId() const override
			{
				static const FString Id = TEXT("blueprint-component");
				return Id;
			}

			virtual void CollectDeclarations(
				TArray<FUeAgentCommandDeclaration>& OutDeclarations) const override
			{
				OutDeclarations.Add(Declaration());
			}

			virtual bool CreateHandlers(
				TSharedRef<const FUeAgentImmutableCommandRegistry> Registry,
				TArray<TSharedRef<IUeAgentExactCommandHandler>>& OutHandlers,
				FString& OutError) const override
			{
				return TryCreateBlueprintComponentCommandHandlers(
					Service, OutHandlers, OutError);
			}

		private:
			TSharedRef<IUeAgentBlueprintComponentCommandService> Service;
		};

		const FUeAgentActiveCommandContributionFactoryRegistration
			BlueprintComponentContributionRegistration(
				TEXT("blueprint-component"),
				&TryCreateProductionBlueprintComponentContribution);
	}

	bool TryCreateBlueprintComponentContribution(
		TSharedRef<IUeAgentBlueprintComponentCommandService> Service,
		TSharedPtr<IUeAgentActiveCommandContribution>& OutContribution,
		FString& OutError)
	{
		OutContribution = MakeShared<FContribution>(MoveTemp(Service));
		OutError.Reset();
		return true;
	}

	bool TryCreateProductionBlueprintComponentContribution(
		TSharedPtr<IUeAgentActiveCommandContribution>& OutContribution,
		FString& OutError)
	{
		TSharedPtr<IUeAgentBlueprintComponentCommandService> Service;
		if (!TryCreateProductionBlueprintComponentCommandService(Service, OutError))
		{
			OutContribution.Reset();
			return false;
		}
		return TryCreateBlueprintComponentContribution(
			Service.ToSharedRef(), OutContribution, OutError);
	}

	bool TryCreateBlueprintComponentRegistration(
		TSharedRef<IUeAgentBlueprintComponentCommandService> Service,
		TSharedPtr<const FUeAgentImmutableCommandRegistry>& OutRegistry,
		TSharedPtr<const FUeAgentExactCommandDispatch>& OutDispatch,
		FString& OutError)
	{
		OutRegistry.Reset();
		OutDispatch.Reset();
		OutError.Reset();
		TSharedPtr<IUeAgentActiveCommandContribution> Contribution;
		FUeAgentActiveCommandComposition Composition;
		if (!TryCreateBlueprintComponentContribution(
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

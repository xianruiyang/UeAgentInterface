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
					TEXT("Blueprint name returned by content search, or an exact Blueprint package.object path.")),
				Field(TEXT("parent"), TEXT("string"), false,
					TEXT("Optional component name copied from the Blueprint export: an SCS node or inherited scene component; actor root is used when omitted.")),
				Field(TEXT("query"), TEXT("string"), false,
					TEXT("Optional Add Component menu search text.")),
			};
			Result.ResultFields = {
				Field(TEXT("scope"), TEXT("object"), true, TEXT("Public target and optional parent for this query; response metadata, not a selector.")),
				Field(TEXT("names"), TEXT("array"), true,
					TEXT("Names of components available in the requested native context.")),
			};
			Result.Errors = {
				{TEXT("blueprint_component_query_authority_required"),
					TEXT("The uai:query scope is missing.")},
				{TEXT("blueprint_component_target_unresolved"),
					TEXT("The exact Blueprint identity could not be resolved.")},
				{TEXT("blueprint_component_parent_unresolved"),
					TEXT("The component name does not resolve to an SCS node or inherited scene component in this Blueprint.")},
				{TEXT("blueprint_component_candidate_query_failed"),
					TEXT("The native Add Component registry could not be enumerated.")},
			};
			Result.Purpose.Summary =
				TEXT("Search the native Add Component menu for one Blueprint component context.");
			Result.Purpose.Target =
				TEXT("One exact persisted Blueprint component tree.");
			Result.Purpose.Preconditions = {
				TEXT("Copy target unchanged from public Blueprint asset creation or search."),
				TEXT("Copy an optional parent name from the current exported component tree, including inherited scene components."),
			};
			Result.Purpose.ParameterGuidance =
				TEXT("Search the exact native registry and use a returned name as the component creation selector.");
			Result.Purpose.ResultGuidance =
				TEXT("The response contains names and one scope. Re-query needed names when the target or parent context changes. Use a short semantic selection if ambiguous; scope is response metadata, not a candidate selector.");
			Result.Purpose.FailureGuidance =
				TEXT("Unknown, ambiguous or stale selectors fail before mutation; there is no class-only fallback.");
			Result.Purpose.SideEffectGuidance =
				TEXT("Read-only with no persistent or external side effects.");
			Result.Purpose.MinimumExample =
				TEXT("client.blueprint.component_candidates(target=target, parent='DefaultSceneRoot', query='Point Light')");
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
				UE::UAI::Contract::FDiagnostic& OutError) const override
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
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutContribution = MakeShared<FContribution>(MoveTemp(Service));
		OutError.Reset();
		return true;
	}

	bool TryCreateProductionBlueprintComponentContribution(
		TSharedPtr<IUeAgentActiveCommandContribution>& OutContribution,
		UE::UAI::Contract::FDiagnostic& OutError)
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
		UE::UAI::Contract::FDiagnostic& OutError)
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

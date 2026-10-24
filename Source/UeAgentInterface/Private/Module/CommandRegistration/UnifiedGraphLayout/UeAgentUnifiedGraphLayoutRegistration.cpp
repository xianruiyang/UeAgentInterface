#include "Module/CommandRegistration/UnifiedGraphLayout/UeAgentUnifiedGraphLayoutRegistration.h"

#include "CommandHandlers/UnifiedGraphLayout/UeAgentUnifiedGraphLayoutCommandHandler.h"

namespace UE::UAI::Module::CommandRegistration::UnifiedGraphLayout
{
	using namespace Active;
	using namespace CommandContract;
	using namespace CommandEdge;
	using namespace CommandRegistry;

	namespace
	{
		class FContribution final : public IUeAgentActiveCommandContribution
		{
		public:
			const FString& GetContributionId() const override
			{
				static const FString Id(TEXT("unified-graph-layout"));
				return Id;
			}
			void CollectDeclarations(
				TArray<FUeAgentCommandDeclaration>& OutDeclarations) const override
			{
				FUeAgentCommandDeclaration Declaration;
				Declaration.CommandId = TEXT("graph.layout.apply");
				Declaration.ContractVersion = 1;
				Declaration.PublicOwner = TEXT("unified-graph-layout-owner");
				Declaration.Availability = EUeAgentCommandAvailability::Enabled;
				Declaration.SideEffect = EUeAgentCommandSideEffect::AssetState;
				Declaration.StateLayers = {TEXT("persisted")};
				Declaration.InputFields = {
					{TEXT("targets"), TEXT("array"), true, false,
						TEXT("One or more exact graph identities from one owning Blueprint asset; the service binds the current revision during the operation.")},
					{TEXT("strategy"), TEXT("string"), true, false,
						TEXT("The shared layout strategy identity.")},
					{TEXT("parameters"), TEXT("object"), true, false,
						TEXT("Bounded strategy parameters such as spacing.")},
					{TEXT("projection"), TEXT("object"), false, false,
						TEXT("Optional bounded response projection.")},
				};
				Declaration.ResultFields = {
					{TEXT("terminal_status"), TEXT("string"), true, false,
						TEXT("Succeeded only after save and independent layout readback.")},
					{TEXT("layout_summary"), TEXT("array"), true, false,
						TEXT("Short per-target layout counts and readback status.")},
				};
				Declaration.Errors = {
					{TEXT("unified_graph_layout_target_duplicate"),
						TEXT("A graph identity appears more than once.")},
					{TEXT("unified_graph_layout_target_unsupported"),
						TEXT("The exact graph identity is unknown or unsupported.")},
					{TEXT("unified_graph_layout_provider_unsupported"),
						TEXT("The graph is not owned by a Blueprint, Widget Blueprint, or Animation Blueprint provider.")},
					{TEXT("unified_graph_layout_cross_package_atomicity_unsupported"),
						TEXT("The current atomic write boundary accepts only graphs from one owning asset and package.")},
					{TEXT("unified_graph_layout_stale_revision"),
						TEXT("A target revision changed before the first write.")},
					{TEXT("unified_graph_layout_geometry_unavailable"),
						TEXT("Exact offscreen graph geometry could not be measured without disturbing the user's editor.")},
					{TEXT("unified_graph_layout_algorithm_failed"),
						TEXT("The topology layout algorithm rejected the measured graph.")},
					{TEXT("unified_graph_layout_compile_failed"),
						TEXT("The owning Blueprint did not compile after layout and was restored before save.")},
					{TEXT("unified_graph_layout_save_failed"),
						TEXT("The shared owner could not persist the owning package and restored in-memory positions.")},
					{TEXT("unified_graph_layout_reload_failed_state_unknown"),
						TEXT("The package was saved but the mandatory reload failed; persisted state must be inspected.")},
					{TEXT("unified_graph_layout_independent_readback_failed"),
						TEXT("Reloaded graph identities, compile status, revision, or node positions did not match the applied result.")},
				};
				Declaration.Purpose.Summary =
					TEXT("Persistently lay out one or more exact graphs from one owning Blueprint asset through one public owner.");
				Declaration.Purpose.Target =
					TEXT("Exact Blueprint, Widget Blueprint, or Animation Blueprint graph identities that share one owning asset and package.");
				Declaration.Purpose.Preconditions = {
					TEXT("Keep every graph in one request within the same owning Blueprint asset and package."),
				};
				Declaration.Purpose.ParameterGuidance =
					TEXT("Do not pass provider names; use hierarchical_left_to_right and bounded spacing, padding, or origin parameters.");
				Declaration.Purpose.ResultGuidance =
					TEXT("Verify the short layout summary after the service reloads and reads the package back.");
				Declaration.Purpose.FailureGuidance =
					TEXT("Unknown, duplicate, cross-package, unsupported, stale, or unmeasurable targets fail before the first write; post-save reload failure is explicitly state-unknown.");
				Declaration.Purpose.SideEffectGuidance =
					TEXT("The owner measures graph geometry offscreen, applies the shared topology algorithm, compiles and saves one package, reloads it, then reads positions back.");
				Declaration.Purpose.MinimumExample =
					TEXT("client.graph.layout_apply(targets, strategy, parameters)");
				Declaration.Purpose.RelatedFlows = {
					TEXT("query.command.search"), TEXT("query.command.usage"),
					TEXT("content_graph.structure"),
				};
				Declaration.Purpose.Keywords = {
					TEXT("graph"), TEXT("layout"), TEXT("multi target"),
				};
				Declaration.Purpose.Synonyms = {
					TEXT("auto layout"), TEXT("arrange graph"),
				};
				OutDeclarations.Add(MoveTemp(Declaration));
			}
			bool CreateHandlers(
				TSharedRef<const FUeAgentImmutableCommandRegistry> Registry,
				TArray<TSharedRef<IUeAgentExactCommandHandler>>& OutHandlers,
				UE::UAI::Contract::FDiagnostic& OutError) const override
			{
				TSharedPtr<IUeAgentExactCommandHandler> Handler;
				if (!CommandHandlers::UnifiedGraphLayout::TryCreateHandler(
						Handler, OutError)
					|| !Handler.IsValid())
				{
					return false;
				}
				OutHandlers.Add(Handler.ToSharedRef());
				return true;
			}
		};

		const FUeAgentActiveCommandContributionFactoryRegistration Registration(
			TEXT("unified-graph-layout"), &TryCreateContribution);
	}

	bool TryCreateContribution(
		TSharedPtr<IUeAgentActiveCommandContribution>& OutContribution,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutError.Reset();
		OutContribution = MakeShared<FContribution>();
		return OutContribution.IsValid();
	}
}

#include "Module/CommandRegistration/UnifiedContentSearch/UeAgentUnifiedContentSearchRegistration.h"

#include "CommandHandlers/UnifiedContentSearch/UeAgentUnifiedContentSearchHandlers.h"
#include "Core/Contract/UeAgentCommandContract.h"
#include "DomainAdapters/ContentDiscovery/UeAgentEditorContentDiscovery.h"

namespace UE::UAI::Module::CommandRegistration::UnifiedContentSearch
{
	using namespace Active;
	using namespace CommandContract;
	using namespace CommandEdge;
	using namespace CommandHandlers::UnifiedContentSearch;
	using namespace CommandRegistry;
	using namespace ContentDiscovery;
	using namespace DomainCapability;

	namespace
	{
		const FString DomainId = TEXT("unified-content-search");
		const FString CommandId = TEXT("query.content.search");
		const FString Owner = TEXT("unified-content-search-owner");

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
			Result.PythonNamespace = TEXT("query");
			Result.PythonFunction = TEXT("content_search");
			Result.ContractVersion = 1;
			Result.PublicOwner = Owner;
			Result.Availability = EUeAgentCommandAvailability::Enabled;
			Result.SideEffect = EUeAgentCommandSideEffect::None;
			Result.StateLayers = {
				TEXT("asset_registry_current"),
				TEXT("editor_world_current"),
			};
			Result.InputFields = {
				Field(TEXT("query"), TEXT("string"), false, TEXT("Optional exact name/identity or discovery text; exact mode requires it.")),
				Field(TEXT("match_mode"), TEXT("string"), false, TEXT("exact, prefix, text or fuzzy; defaults to text.")),
				Field(TEXT("content_kinds"), TEXT("array"), false, TEXT("Exact subset of asset, actor and component; defaults to all three.")),
				Field(TEXT("class_path"), TEXT("string"), false, TEXT("Optional exact UE class path filter.")),
				Field(TEXT("path_prefix"), TEXT("string"), false, TEXT("Optional exact identity prefix; asset search defaults to /Game when omitted.")),
				Field(TEXT("owner_identity"), TEXT("string"), false, TEXT("Optional exact package, World or Actor owner identity.")),
				Field(TEXT("required_operations"), TEXT("array"), false, TEXT("Required exact operations; current existing-content providers support query.")),
				Field(TEXT("limit_per_provider"), TEXT("integer"), false, TEXT("Items per provider from 1 through 64; defaults to 20.")),
				Field(TEXT("cursor"), TEXT("object"), false, TEXT("Opaque next_cursor copied unchanged from the previous page.")),
			};
			Result.ResultFields = {
				Field(TEXT("items"), TEXT("array"), true, TEXT("Existing content with exact identity, class, owner, state layer, revision, detail command, detail target and minimal detail input.")),
				Field(TEXT("providers"), TEXT("array"), true, TEXT("Per-provider revision, paging, completeness or typed failure.")),
				Field(TEXT("completeness"), TEXT("string"), true, TEXT("complete, paged, partial or degraded without hiding provider failure.")),
				{TEXT("next_cursor"), TEXT("object"), true, true, TEXT("Opaque continuation cursor, or null when no provider has another page.")},
				Field(TEXT("read_only"), TEXT("boolean"), true, TEXT("Always true.")),
			};
			Result.Errors = {
				{TEXT("query_authority_required"), TEXT("The authenticated caller lacks uai:query.")},
				{TEXT("content_search_params_invalid"), TEXT("Match, filter, paging or cursor input is invalid.")},
				{TEXT("content_search_kind_unsupported"), TEXT("No frozen domain provider owns the requested content kind.")},
				{TEXT("content_search_revision_stale"), TEXT("A copied cursor belongs to an older provider revision.")},
				{TEXT("content_search_cursor_request_mismatch"), TEXT("A cursor was reused with different search semantics.")},
				{TEXT("content_search_authority_unavailable"), TEXT("The frozen DomainCapability authority did not publish content discovery.")},
			};
			Result.Purpose.Summary = TEXT("Search current assets, Actors and Components through one provider-based read-only contract.");
			Result.Purpose.Target = TEXT("Asset Registry state and the current Editor World without loading or mutating search results.");
			Result.Purpose.Preconditions = {
				TEXT("The caller has uai:query and the UAI Editor service is active."),
				TEXT("Use exact mode only with a complete known identity or name."),
			};
			Result.Purpose.ParameterGuidance = TEXT("Copy next_cursor unchanged. Use path_prefix/class_path/owner_identity to reduce work; text and fuzzy modes discover but never invent identities.");
			Result.Purpose.ResultGuidance = TEXT("Invoke item.detail_command_id with item.detail_target_identity as target and item.detail_input as input; do not substitute item.identity when the returned detail target differs. Inspect every provider status before concluding absence.");
			Result.Purpose.FailureGuidance = TEXT("Stale or mismatched cursors fail exactly; independent provider failures remain visible as partial/degraded results.");
			Result.Purpose.SideEffectGuidance = TEXT("Strictly read-only; it does not load assets, select Actors, save, compile, focus, open or alter Editor UI.");
			Result.Purpose.MinimumExample = TEXT("client.query.content_search(query='Cube', content_kinds=['actor'], match_mode='text')");
			Result.Purpose.RelatedFlows = {
				TEXT("query.command.search"),
				TEXT("query.command.detail"),
				TEXT("asset.summary"),
				TEXT("content_graph.items"),
				TEXT("content_graph.properties"),
				TEXT("content_graph.import_workspace"),
			};
			Result.Purpose.Keywords = {
				TEXT("content"), TEXT("asset"), TEXT("actor"),
				TEXT("component"), TEXT("search"), TEXT("identity"),
			};
			Result.Purpose.Synonyms = {
				TEXT("find UE object"),
				TEXT("search project content"),
			};
			return Result;
		}

		class FUnifiedContentSearchContribution final
			: public IUeAgentActiveCommandContribution
		{
		public:
			const FString& GetContributionId() const override
			{
				return DomainId;
			}

			void CollectDeclarations(
				TArray<FUeAgentCommandDeclaration>& OutDeclarations) const override
			{
				OutDeclarations.Add(Declaration());
			}

			bool CreateHandlers(
				TSharedRef<const FUeAgentImmutableCommandRegistry>,
				TArray<TSharedRef<IUeAgentExactCommandHandler>>& OutHandlers,
				FString& OutError) const override
			{
				return TryCreateUnifiedContentSearchHandlers(
					OutHandlers,
					OutError);
			}
		};

		const FDomainCapabilityContributionFactoryRegistration
			Registration(
				TEXT("unified-content-search"),
				&TryCreateProductionUnifiedContentSearchDomain);
	}

	bool TryCreateProductionUnifiedContentSearchDomain(
		TSharedPtr<const FDomainCapabilityContribution>& OutContribution,
		FString& OutError)
	{
		OutContribution.Reset();
		OutError.Reset();
		TSharedPtr<IUeAgentContentDiscoveryProvider> AssetProvider;
		TSharedPtr<IUeAgentContentDiscoveryProvider> WorldProvider;
		if (!DomainAdapters::ContentDiscovery::
				TryCreateEditorAssetContentDiscoveryProvider(
					AssetProvider,
					OutError)
			|| !DomainAdapters::ContentDiscovery::
				TryCreateEditorWorldContentDiscoveryProvider(
					WorldProvider,
					OutError)
			|| !AssetProvider.IsValid()
			|| !WorldProvider.IsValid())
		{
			return false;
		}
		FDomainCapabilityDeclaration Domain =
			MakeExplicitlyUnsupportedDomainCapabilityDeclaration(DomainId);
		Domain.Command.Support = EDomainCapabilitySupport::Supported;
		Domain.Command.Contributions.Add(
			MakeShared<FUnifiedContentSearchContribution>());
		Domain.Discovery.Support = EDomainCapabilitySupport::Supported;
		Domain.Discovery.ContentProviders = {
			AssetProvider.ToSharedRef(),
			WorldProvider.ToSharedRef(),
		};
		Domain.Migration.Support = EDomainCapabilitySupport::Supported;
		Domain.Migration.OwnerId = TEXT("TROOT-UAI-35");
		return FDomainCapabilityContribution::TryCreate(
			MoveTemp(Domain),
			OutContribution,
			OutError);
	}
}

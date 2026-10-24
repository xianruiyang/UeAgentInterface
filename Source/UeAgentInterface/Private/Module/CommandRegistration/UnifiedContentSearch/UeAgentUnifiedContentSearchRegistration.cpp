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
				Field(TEXT("query"), TEXT("string"), false, TEXT("Optional readable name or discovery text; exact mode requires it.")),
				Field(TEXT("match_mode"), TEXT("string"), false, TEXT("exact, prefix, text or fuzzy; defaults to text.")),
				Field(TEXT("content_kinds"), TEXT("array"), false, TEXT("Exact subset of asset, actor and component; defaults to all three.")),
			};
			Result.ResultFields = {
				Field(TEXT("names"), TEXT("array"), true, TEXT("Readable existing-content names; collisions use the shortest semantic owner scope.")),
			};
			Result.Errors = {
				{TEXT("query_authority_required"), TEXT("The authenticated caller lacks uai:query.")},
				{TEXT("content_search_params_invalid"), TEXT("The readable query or content kind input is invalid.")},
				{TEXT("content_search_kind_unsupported"), TEXT("No frozen domain provider owns the requested content kind.")},
				{TEXT("content_search_incomplete"), TEXT("A required provider failed, so no partial name list was returned.")},
				{TEXT("content_search_authority_unavailable"), TEXT("The frozen DomainCapability authority did not publish content discovery.")},
			};
			Result.Purpose.Summary = TEXT("Search current assets, Actors and Components through one provider-based read-only contract.");
			Result.Purpose.Target = TEXT("Asset Registry state and the current Editor World without loading or mutating search results.");
			Result.Purpose.Preconditions = {
				TEXT("The caller has uai:query and the UAI Editor service is active."),
				TEXT("Use exact mode only with a complete readable display name."),
			};
			Result.Purpose.ParameterGuidance = TEXT("Search by readable text and optionally narrow content_kinds.");
			Result.Purpose.ResultGuidance = TEXT("The result contains only readable names. A unique display name is unchanged; only real collisions receive a short kind and semantic owner alias.");
			Result.Purpose.FailureGuidance = TEXT("A required provider failure rejects the request; successful providers return their complete matching name lists.");
			Result.Purpose.SideEffectGuidance = TEXT("Strictly read-only; it does not load assets, select Actors, save, compile, focus, open or alter Editor UI.");
			Result.Purpose.MinimumExample = TEXT("client.query.content_search(query='Cube', content_kinds=['actor'], match_mode='text')");
			Result.Purpose.RelatedFlows = {
				TEXT("query.command.search"),
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
				UE::UAI::Contract::FDiagnostic& OutError) const override
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
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutContribution.Reset();
		OutError.Reset();
		TSharedPtr<ContentDiscovery::IUeAgentContentDiscoveryProvider>
			AssetProvider;
		TSharedPtr<ContentDiscovery::IUeAgentContentDiscoveryProvider>
			WorldProvider;
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
		return FDomainCapabilityContribution::TryCreate(
			MoveTemp(Domain),
			OutContribution,
			OutError);
	}
}

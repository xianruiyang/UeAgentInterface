#include "Module/CommandRegistration/WorldSpatial/UeAgentLevelWorldDomainRegistration.h"

#include "CommandHandlers/WorldSpatial/UeAgentLevelWorldCommandHandlers.h"
#include "Core/Contract/UeAgentCommandContract.h"
#include "Core/DomainCapability/UeAgentDomainCapability.h"
#include "Module/CommandRegistration/Active/UeAgentActiveCommandComposition.h"
#include "OperationOwners/WorldSpatial/UeAgentLevelWorldDomainService.h"

namespace UE::UAI::Module::CommandRegistration::WorldSpatial
{
	using namespace Active;
	using namespace CommandContract;
	using namespace CommandEdge;
	using namespace CommandHandlers::WorldSpatial;
	using namespace CommandRegistry;

	namespace
	{
		const FString DomainId = TEXT("level-world-domain");
		const FString ProviderRegistrationId = TEXT("level-world-domain-provider");

		const FString CandidateCommandId = TEXT("world.actor.candidates");
		const FString CandidateOwner = TEXT("level-world-domain-owner");

		FUeAgentCommandFieldDeclaration Field(
			const TCHAR* Name,
			const TCHAR* WireType,
			const bool bRequired,
			const TCHAR* Description)
		{
			return {Name, WireType, bRequired, false, Description};
		}

		FUeAgentCommandDeclaration CandidateDeclaration()
		{
			FUeAgentCommandDeclaration Result;
			Result.CommandId = CandidateCommandId;
			Result.PythonNamespace = TEXT("world");
			Result.PythonFunction = TEXT("actor_candidates");
			Result.ContractVersion = 1;
			Result.PublicOwner = CandidateOwner;
			Result.Availability = EUeAgentCommandAvailability::Enabled;
			Result.SideEffect = EUeAgentCommandSideEffect::None;
			Result.StateLayers = {TEXT("editor_registry_state"),
				TEXT("asset_persisted_state")};
			Result.InputFields = {
				Field(TEXT("target"), TEXT("string"), true,
					TEXT("Persisted World name from content search, or its exact package.object path, that will own the Actor.")),
				Field(TEXT("category"), TEXT("string"), false,
					TEXT("Optional exact native Place Actors category handle or display name.")),
				Field(TEXT("query"), TEXT("string"), false,
					TEXT("Optional native Place Actors text-filter expression; active text search uses AllClasses like the editor UI.")),
			};
			Result.ResultFields = {
				Field(TEXT("scope"), TEXT("object"), true, TEXT("Public target and supplied placement category; response metadata, not a selector.")),
				Field(TEXT("names"), TEXT("array"), true,
					TEXT("Names of actors available in the requested native placement context.")),
			};
			Result.Errors = {
				{TEXT("world_actor_candidate_query_authority_required"),
					TEXT("The uai:query authority is missing.")},
				{TEXT("world_actor_candidate_target_unresolved"),
					TEXT("The exact persisted World identity does not exist.")},
				{TEXT("world_actor_candidate_query_failed"),
					TEXT("The native Place Actors registry could not be enumerated.")},
			};
			Result.Purpose.Summary =
				TEXT("Search the same native registry and text filter used by the Place Actors UI.");
			Result.Purpose.Target =
				TEXT("One persisted World target plus one native Placement Mode context.");
			Result.Purpose.Preconditions = {
				TEXT("Copy target unchanged from current World state or content search."),
				TEXT("Use category only when reproducing an explicit Place Actors tab context."),
			};
			Result.Purpose.ParameterGuidance =
				TEXT("Search the native registry and use a returned name as the Actor creation selector.");
			Result.Purpose.ResultGuidance =
				TEXT("The response contains names and one scope; re-query needed names when the target or placement category changes. scope is response metadata, not a selector; ambiguous names require a short semantic selection.");
			Result.Purpose.FailureGuidance =
				TEXT("Unknown, ambiguous, stale, unsupported custom-drag or assertion-mismatched candidates fail before persistent mutation; no class-only fallback is used for candidate HJSON.");
			Result.Purpose.SideEffectGuidance =
				TEXT("Read-only; it opens no editor window, selects nothing and creates no Actor.");
			Result.Purpose.MinimumExample =
				TEXT("client.world.actor_candidates(target='/Game/Maps/MyMap.MyMap', query='Cube')");
			Result.Purpose.RelatedFlows = {
				TEXT("query.content.search"),
				TEXT("content_graph.import_workspace"),
				TEXT("blueprint.component.candidates"),
			};
			Result.Purpose.Keywords = {
				TEXT("world"), TEXT("actor"), TEXT("place actors"),
				TEXT("candidate"), TEXT("hjson"), TEXT("factory"),
			};
			Result.Purpose.Synonyms = {
				TEXT("search Place Actors"),
				TEXT("resolve Actor creation item"),
			};
			return Result;
		}

		FUeAgentCommandDeclaration ComponentCandidateDeclaration()
		{
			auto Result = CandidateDeclaration();
			Result.CommandId = TEXT("world.component.candidates");
			Result.PythonFunction = TEXT("component_candidates");
			Result.InputFields = {
				Field(TEXT("target"), TEXT("string"), true, TEXT("Exact owning Level Actor object_ref returned by content_graph.items.")),
				Field(TEXT("query"), TEXT("string"), false, TEXT("Optional native component catalog text filter.")),
				Field(TEXT("describe"), TEXT("object"), false, TEXT("One candidate: name, optional select, optional fields. Returns native default property descriptors usable as creation properties.")),
			};
			Result.ResultFields = {
				Field(TEXT("scope"), TEXT("object"), true, TEXT("Owning Actor and World context.")),
				Field(TEXT("names"), TEXT("array"), true, TEXT("Native component candidate names; the selected name when describing one candidate.")),
				Field(TEXT("description"), TEXT("object"), false, TEXT("Native defaults, creation support and selected editable property descriptors.")),
			};
			Result.Errors = {
				{TEXT("level_component_candidate_owner_invalid"), TEXT("target must identify a loaded, authorable Level Actor.")},
				{TEXT("level_component_not_authorable"), TEXT("The native component class or owner cannot be authored.")},
			};
			Result.Purpose.Summary = TEXT("Discover native components for an existing Level Actor and describe creation properties.");
			Result.Purpose.Target = TEXT("One owning Level Actor object_ref; not a Blueprint asset or World path.");
			Result.Purpose.Preconditions = {TEXT("Obtain the exact Actor object_ref from content_graph.items.")};
			Result.Purpose.ParameterGuidance = TEXT("Use query for names; use describe with name/select and a small fields list for native defaults.");
			Result.Purpose.ResultGuidance = TEXT("Add the chosen name and edited description.properties to a new key in the owning Actor's level_components.hjson components map.");
			Result.Purpose.FailureGuidance = TEXT("Resolve ambiguous names with select; instance edits cannot remove components owned by the Actor class or Blueprint construction.");
			Result.Purpose.SideEffectGuidance = TEXT("Read-only native discovery; no persistent Actor or component is created.");
			Result.Purpose.MinimumExample = TEXT("client.world.component_candidates(target=actor_object_ref, describe={'name':'Point Light','fields':['Intensity']})");
			Result.Purpose.RelatedFlows = {TEXT("content_graph.items"), TEXT("content_graph.export_workspace"), TEXT("content_graph.import_workspace")};
			Result.Purpose.Keywords = {TEXT("level"), TEXT("component"), TEXT("candidate"), TEXT("properties"), TEXT("hjson")};
			Result.Purpose.Synonyms = {TEXT("add Level Actor component")};
			return Result;
		}

		class FCandidateContribution final : public IUeAgentActiveCommandContribution
		{
		public:
			virtual const FString& GetContributionId() const override
			{
				static const FString Id = TEXT("level-world-actor-candidates");
				return Id;
			}

			virtual void CollectDeclarations(
				TArray<FUeAgentCommandDeclaration>& OutDeclarations) const override
			{
				OutDeclarations.Add(CandidateDeclaration());
				OutDeclarations.Add(ComponentCandidateDeclaration());
			}

			virtual bool CreateHandlers(
				TSharedRef<const FUeAgentImmutableCommandRegistry>,
				TArray<TSharedRef<IUeAgentExactCommandHandler>>& OutHandlers,
				UE::UAI::Contract::FDiagnostic& OutError) const override
			{
				return TryCreateWorldSpatialCommandHandlers(
					OutHandlers, OutError);
			}
		};

		bool TryCreateDomainCapability(
			TSharedPtr<const DomainCapability::FDomainCapabilityContribution>&
				OutContribution,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			TSharedPtr<HjsonWorkspace::IImportInitializationDomainProvider>
				Provider;
			HjsonWorkspace::FWorkspaceError WorkspaceError;
			if (!OperationOwners::WorldSpatial::TryCreateLevelWorldDomainProvider(
					Provider, WorkspaceError)
				|| !Provider.IsValid())
			{
				OutError = WorkspaceError.GetDiagnostic();
				return false;
			}
			DomainCapability::FDomainCapabilityDeclaration Declaration =
				DomainCapability::MakeExplicitlyUnsupportedDomainCapabilityDeclaration(
					DomainId);
			Declaration.Hjson.Support =
				DomainCapability::EDomainCapabilitySupport::Supported;
			Declaration.Hjson.Providers.Add(Provider.ToSharedRef());
			Declaration.Hjson.Routes = {{
				ProviderRegistrationId,
				TEXT("content_graph.import_workspace"),
				TEXT("uai.world.edit/1"),
				TEXT("level_world"),
				TEXT("/Script/Engine.World"),
				HjsonWorkspace::EInitializationNodeRole::RootTarget,
				TEXT("uai.init.world.world/1"),
				true,
			}};
			Declaration.Initialization.Support =
				DomainCapability::EDomainCapabilitySupport::Supported;
			Declaration.Initialization.ContractIds = {
				TEXT("uai.init.world.world/1"),
				TEXT("uai.init.world.actor/1"),
				TEXT("uai.init.world.component/1"),
				TEXT("uai.init.world.data_layer/1"),
			};
			return DomainCapability::FDomainCapabilityContribution::TryCreate(
				MoveTemp(Declaration), OutContribution, OutError);
		}

		const DomainCapability::FDomainCapabilityContributionFactoryRegistration
			DomainCapabilityRegistration(*DomainId, &TryCreateDomainCapability);

		const FUeAgentActiveCommandContributionFactoryRegistration
			CandidateRegistration(
				TEXT("level-world-actor-candidates"),
				&TryCreateLevelWorldCandidateContribution);
	}

	const FString& GetProviderRegistrationId()
	{
		return ProviderRegistrationId;
	}

	bool TryCreateLevelWorldCandidateContribution(
		TSharedPtr<IUeAgentActiveCommandContribution>& OutContribution,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutContribution = MakeShared<FCandidateContribution>();
		OutError.Reset();
		return true;
	}
}

#include "Module/CommandRegistration/Pcg/UeAgentPcgDomainRegistration.h"

#include "CommandHandlers/Pcg/UeAgentPcgDomainCommandHandlers.h"
#include "Core/Contract/UeAgentCommandContract.h"
#include "Core/DomainCapability/UeAgentDomainCapability.h"
#include "Core/HjsonWorkspace/ImportInitialization/UeAgentHjsonDomainProviderRegistry.h"
#include "OperationOwners/Pcg/UeAgentPcgDomainService.h"

namespace UE::UAI::Module::CommandRegistration::Pcg
{
	using namespace Active;
	using namespace CommandContract;
	using namespace CommandEdge;
	using namespace CommandHandlers::Pcg;
	using namespace CommandRegistry;
	using namespace OperationOwners::Pcg;

	namespace
	{
		const FString DomainId = TEXT("pcg-command-migration");
		const FString ProviderRegistrationId = TEXT("pcg-domain-provider");
		const FString CandidateCommandId = TEXT("pcg.node.candidates");
		const FString CandidateOwner = TEXT("pcg-domain-owner");
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
			Result.PythonNamespace = TEXT("pcg");
			Result.PythonFunction = TEXT("node_candidates");
			Result.ContractVersion = 1;
			Result.PublicOwner = CandidateOwner;
			Result.Availability = EUeAgentCommandAvailability::Enabled;
			Result.SideEffect = EUeAgentCommandSideEffect::None;
			Result.StateLayers = {TEXT("asset_persisted_state")};
			Result.InputFields = {
				Field(TEXT("target"), TEXT("string"), true, TEXT("Persisted PCG Graph name from content search, or its exact package.object path.")),
				Field(TEXT("query"), TEXT("string"), false, TEXT("Optional search over the native PCG graph context-menu text.")),
				Field(TEXT("describe"), TEXT("object"), false, TEXT("Optional selected {name, select?, fields?}; returns editable native settings after the candidate's preconfiguration, without creating a graph node.")),
			};
			Result.ResultFields = {
				Field(TEXT("scope"), TEXT("object"), true, TEXT("Public target of this candidate query; response metadata, not a selector.")),
				Field(TEXT("names"), TEXT("array"), true, TEXT("Ordered, readable PCG node names.")),
				Field(TEXT("description"), TEXT("object"), false, TEXT("Selected settings defaults and referenced types; only for describe. Dynamic pins require a created node.")),
			};
			Result.Errors = {
				{TEXT("pcg_candidate_query_authority_required"), TEXT("The uai:query authority is missing.")},
				{TEXT("pcg_candidate_target_unresolved"), TEXT("The exact target could not be loaded.")},
				{TEXT("pcg_candidate_target_type_unsupported"), TEXT("The target is not an exact PCG Graph.")},
				{TEXT("pcg_candidate_machine_selector_not_supported"), TEXT("Machine identity selectors are not accepted; use the returned readable name.")},
				{TEXT("pcg_candidate_editor_context_unavailable"), TEXT("The native PCG editor graph context could not be composed without UI.")},
				{TEXT("pcg_candidate_identity_ambiguous"), TEXT("The complete native action set did not yield unique readable semantic names.")},
			};
			Result.Purpose.Summary = TEXT("Search the same native PCG graph background context-menu actions used by the Editor and return readable names.");
			Result.Purpose.Target = TEXT("One exact persisted PCG Graph and its native background graph context.");
			Result.Purpose.Preconditions = {
				TEXT("Copy the target unchanged from public content search or readback."),
				TEXT("Run in the Editor on the UE game thread with uai:query."),
			};
			Result.Purpose.ParameterGuidance = TEXT("Use query to search like the PCG graph right-click menu. Put only the returned readable candidate.name in HJSON; graph, placement, semantic node identity, and optional assertions stay outside candidate.");
			Result.Purpose.ResultGuidance = TEXT("The result contains the complete matching names and one scope. Re-query needed names when the target changes. Creation re-enumerates that native menu; scope is response metadata, not a candidate selector.");
			Result.Purpose.FailureGuidance = TEXT("Unresolved targets, unavailable native context and readable-name collisions fail without graph mutation.");
			Result.Purpose.SideEffectGuidance = TEXT("Content read-only: it may load the PCGEditor module and native action metadata but creates no node, package, file, or asset state.");
			Result.Purpose.MinimumExample = TEXT("client.pcg.node_candidates(target, query='Point')");
			Result.Purpose.RelatedFlows = {
				TEXT("query.command.search"),
				TEXT("query.command.usage"),
				TEXT("content_graph.summary"),
				TEXT("content_graph.import_workspace"),
			};
			Result.Purpose.Keywords = {TEXT("pcg"), TEXT("node"), TEXT("candidate"), TEXT("graph"), TEXT("hjson")};
			Result.Purpose.Synonyms = {TEXT("find pcg node"), TEXT("search pcg right click menu"), TEXT("resolve pcg native action")};
			return Result;
		}

		FUeAgentCommandDeclaration SpatialDeclaration(const bool bGeneratedData)
		{
			FUeAgentCommandDeclaration Result;
			Result.CommandId = bGeneratedData ? TEXT("pcg.generated_spatial_data.read") : TEXT("pcg.partition.spatial_state.read");
			Result.PythonNamespace = TEXT("pcg");
			Result.PythonFunction = bGeneratedData ? TEXT("generated_spatial_data_read") : TEXT("partition_spatial_state_read");
			Result.ContractVersion = 2;
			Result.PublicOwner = CandidateOwner;
			Result.Availability = EUeAgentCommandAvailability::Enabled;
			Result.SideEffect = EUeAgentCommandSideEffect::None;
			Result.StateLayers = {TEXT("editor_session_state")};
			Result.InputFields = {
				Field(TEXT("target"), TEXT("string"), true, TEXT("Exact currently loaded world, level, actor or PCG component reference from public readback.")),
				Field(TEXT("input"), TEXT("object"), true, TEXT("{} reads the complete target scope. Optional actor/component are exact readable names or paths; max_points_per_data=-1 reads all points, 0 counts only, nonnegative samples explicitly.")),
				Field(TEXT("context"), TEXT("object"), true, TEXT("{} uses the target's native world. Optional world/level must match its actual scope; no generation identity is required.")),
				Field(TEXT("projection"), TEXT("object"), false, TEXT("Public profile_id/include_defaults/include_details/include_provenance flags. include_details includes editable component properties; native output sampling is controlled by input.max_points_per_data.")),
			};
			Result.ResultFields = {
				Field(TEXT("schema"), TEXT("string"), true, TEXT("uai.pcg.current_spatial_state/1.")),
				Field(TEXT("target"), TEXT("string"), true, TEXT("Actual loaded target read.")),
				Field(TEXT("components"), TEXT("array"), true, TEXT("Sorted current native component status and graph output or mapped partition actors. Empty output/partitions is a valid observation, not a generation verdict.")),
			};
			Result.Errors = {
				{TEXT("pcg_spatial_target_unresolved"), TEXT("The target is not currently loaded.")},
				{TEXT("pcg_spatial_target_type_unsupported"), TEXT("Use a world, level, actor or PCG component rather than a graph definition.")},
				{TEXT("pcg_spatial_selector_unresolved"), TEXT("The explicit component selector matches nothing in the target scope.")},
				{TEXT("pcg_spatial_context_mismatch"), TEXT("The context does not match the actual target world/level.")},
				{TEXT("pcg_spatial_input_invalid"), TEXT("The input diagnostic identifies the invalid field.")},
			};
			Result.Purpose.Summary = bGeneratedData ? TEXT("Read current UPCGComponent graph output, including native local partition component outputs, without generating.") : TEXT("Read the current native subsystem partition mapping for PCG components, without creating partitions.");
			Result.Purpose.Target = TEXT("One explicit loaded world, level, actor or PCG component scope.");
			Result.Purpose.Preconditions = {TEXT("Read current state on the UE game thread with uai:query; generation remains owned by native component authoring.")};
			Result.Purpose.ParameterGuidance = TEXT("Start with input={} and context={}. For counts only use input={'max_points_per_data': 0}. Copy readable actor/component selectors from public readback. Do not construct machine generation identities.");
			Result.Purpose.ResultGuidance = TEXT("Status distinguishes generating/cleaning/generated and actual output counts. Graph output can be empty when its Output node is unconnected even when meshes were spawned; generated geometry is available through content_graph.items geometry_components. Point transforms remain in native PCG data space; point bounds remain point-local.");
			Result.Purpose.FailureGuidance = TEXT("Located diagnostics distinguish unloaded targets, invalid selectors and context mismatches. Reads leave native generation and authored state unchanged.");
			Result.Purpose.SideEffectGuidance = TEXT("Reads live objects only. Does not load a replacement world, generate, settle, create a DataAsset, partition actor or cache.");
			Result.Purpose.MinimumExample = bGeneratedData ? TEXT("client.pcg.generated_spatial_data_read(target, {}, {})") : TEXT("client.pcg.partition_spatial_state_read(target, {}, {})");
			Result.Purpose.RelatedFlows = {TEXT("query.command.usage"), TEXT("content_graph.items"), TEXT("content_graph.import_workspace")};
			Result.Purpose.Keywords = {TEXT("pcg"), TEXT("generated"), TEXT("partition"), TEXT("spatial"), TEXT("read")};
			return Result;
		}

		class FPcgCandidateContribution final : public IUeAgentActiveCommandContribution
		{
		public:
			explicit FPcgCandidateContribution(
				TSharedRef<IUeAgentPcgCandidateCommandService> InService)
				: Service(MoveTemp(InService))
			{
			}

			virtual const FString& GetContributionId() const override
			{
				static const FString Id = TEXT("pcg-domain-candidates");
				return Id;
			}

			virtual void CollectDeclarations(TArray<FUeAgentCommandDeclaration>& OutDeclarations) const override
			{
				OutDeclarations.Add(CandidateDeclaration());
				OutDeclarations.Add(SpatialDeclaration(true));
				OutDeclarations.Add(SpatialDeclaration(false));
			}

			virtual bool CreateHandlers(
				TSharedRef<const FUeAgentImmutableCommandRegistry> Registry,
				TArray<TSharedRef<IUeAgentExactCommandHandler>>& OutHandlers,
				UE::UAI::Contract::FDiagnostic& OutError) const override
			{
				(void)Registry;
				return TryCreatePcgCandidateCommandHandlers(Service, OutHandlers, OutError);
			}

		private:
			TSharedRef<IUeAgentPcgCandidateCommandService> Service;
		};

		bool TryCreateDomainCapability(
			TSharedPtr<const DomainCapability::
				FDomainCapabilityContribution>& OutContribution,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			TSharedPtr<HjsonWorkspace::IImportInitializationDomainProvider>
				Provider;
			HjsonWorkspace::FWorkspaceError WorkspaceError;
			if (!OperationOwners::Pcg::TryCreatePcgDomainProvider(
					Provider, WorkspaceError)
				|| !Provider.IsValid())
			{
				OutError = WorkspaceError.GetDiagnostic();
				return false;
			}
			DomainCapability::FDomainCapabilityDeclaration Declaration =
				DomainCapability::
					MakeExplicitlyUnsupportedDomainCapabilityDeclaration(DomainId);
			Declaration.Hjson.Support =
				DomainCapability::EDomainCapabilitySupport::Supported;
			Declaration.Hjson.Providers.Add(Provider.ToSharedRef());
			Declaration.Hjson.Routes = {{
				ProviderRegistrationId,
				TEXT("content_graph.import_workspace"),
				TEXT("uai.pcg.edit/1"),
				TEXT("ue.pcg_graph.asset"),
				TEXT("/Script/PCG.PCGGraph"),
				HjsonWorkspace::EInitializationNodeRole::RootTarget,
				TEXT("uai.init.pcg_graph.root/1"),
				true,
			}};
			Declaration.Initialization.Support =
				DomainCapability::EDomainCapabilitySupport::Supported;
			Declaration.Initialization.ContractIds = {
				TEXT("uai.init.pcg_graph.root/1"),
				TEXT("uai.init.pcg.node/1"),
				TEXT("uai.init.pcg.endpoint/1"),
				TEXT("uai.init.pcg.parameter/1"),
			};
			return DomainCapability::FDomainCapabilityContribution::TryCreate(
				MoveTemp(Declaration), OutContribution, OutError);
		}

		const DomainCapability::
			FDomainCapabilityContributionFactoryRegistration
			DomainCapabilityRegistration(*DomainId, &TryCreateDomainCapability);

		const FUeAgentActiveCommandContributionFactoryRegistration CandidateRegistration(
			TEXT("pcg-domain-candidates"),
			&TryCreateProductionPcgCandidateContribution);
	}

	const FString& GetProviderRegistrationId()
	{
		return ProviderRegistrationId;
	}

	bool TryCreateProductionPcgCandidateContribution(
		TSharedPtr<IUeAgentActiveCommandContribution>& OutContribution,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		TSharedPtr<IUeAgentPcgCandidateCommandService> Service;
		if (!TryCreateProductionPcgCandidateCommandService(Service, OutError)
			|| !Service.IsValid())
		{
			OutContribution.Reset();
			return false;
		}
		OutContribution = MakeShared<FPcgCandidateContribution>(Service.ToSharedRef());
		OutError.Reset();
		return true;
	}
}

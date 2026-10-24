#include "Module/CommandRegistration/Niagara/UeAgentNiagaraDomainRegistration.h"

#include "CommandHandlers/Niagara/UeAgentNiagaraDomainCommandHandlers.h"
#include "Core/Contract/UeAgentCommandContract.h"
#include "Core/DomainCapability/UeAgentDomainCapability.h"
#include "Core/HjsonWorkspace/ImportInitialization/UeAgentHjsonDomainProviderRegistry.h"
#include "OperationOwners/Niagara/UeAgentNiagaraDomainService.h"

namespace UE::UAI::Module::CommandRegistration::Niagara
{
	using namespace Active;
	using namespace CommandContract;
	using namespace CommandEdge;
	using namespace CommandHandlers::Niagara;
	using namespace CommandRegistry;
	using namespace OperationOwners::Niagara;

	namespace
	{
		const FString DomainId = TEXT("niagara-command-migration");
		const FString ProviderRegistrationId = TEXT("niagara-domain-provider");
		const FString CandidateCommandId = TEXT("niagara.module.candidates");
		const FString CandidateOwner = TEXT("niagara-domain-owner");
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
			Result.PythonNamespace = TEXT("niagara");
			Result.PythonFunction = TEXT("module_candidates");
			Result.ContractVersion = 1;
			Result.PublicOwner = CandidateOwner;
			Result.Availability = EUeAgentCommandAvailability::Enabled;
			Result.SideEffect = EUeAgentCommandSideEffect::None;
			Result.StateLayers = {TEXT("asset_persisted_state")};
			Result.InputFields = {
				Field(TEXT("target"), TEXT("string"), true, TEXT("Persisted Niagara System or standalone Emitter name from content search, or its exact package.object path.")),
				Field(TEXT("target_usage"), TEXT("string"), true, TEXT("Niagara stack usage resolved against a real output node, such as system_update or particle_update.")),
				Field(TEXT("emitter_name"), TEXT("string"), false, TEXT("Optional readable emitter instance name when the system has multiple emitters.")),
				Field(TEXT("stage_name"), TEXT("string"), false, TEXT("Optional readable stage name; normally the same semantic value as target_usage.")),
				Field(TEXT("query"), TEXT("string"), false, TEXT("Optional native module readable-name or category filter.")),
				Field(TEXT("describe"), TEXT("object"), false, TEXT("Optional {name} selected from this stack. Returns creation.supported/reason without creating a module; module input defaults require the created stack context.")),
			};
			Result.ResultFields = {
				Field(TEXT("scope"), TEXT("object"), true, TEXT("Public target, target_usage and optional emitter/stage context; response metadata, not a selector.")),
				Field(TEXT("names"), TEXT("array"), true, TEXT("Ordered, readable Niagara module names.")),
				Field(TEXT("description"), TEXT("object"), false, TEXT("Selected native action's current HJSON creation support, only when describe is requested.")),
			};
			Result.Errors = {
				{TEXT("niagara_candidate_query_authority_required"), TEXT("The uai:query authority is missing.")},
				{TEXT("niagara_candidate_target_unresolved"), TEXT("The exact target could not be loaded.")},
				{TEXT("niagara_candidate_target_type_unsupported"), TEXT("The target is not a Niagara System or standalone Niagara Emitter stack owner.")},
				{TEXT("niagara_candidate_target_usage_required"), TEXT("The target stack usage is missing.")},
				{TEXT("niagara_candidate_target_usage_invalid"), TEXT("The requested target usage is not a Niagara script usage.")},
				{TEXT("niagara_candidate_stage_not_found"), TEXT("No real output node matches the requested context.")},
				{TEXT("niagara_candidate_stage_ambiguous"), TEXT("The readable stack context is ambiguous; provide emitter_name or stage_name." )},
				{TEXT("niagara_candidate_palette_query_failed"), TEXT("The real Niagara module palette could not be queried.")},
			};
			Result.Purpose.Summary = TEXT("Resolve the exact native Niagara Stack Add Module action set in one real output-node context.");
			Result.Purpose.Target = TEXT("One exact persisted Niagara System or standalone Niagara Emitter stack owner and one resolved output node.");
			Result.Purpose.Preconditions = {
				TEXT("Copy the target unchanged from public content search or readback."),
				TEXT("Run in the Editor on the UE game thread with uai:query."),
			};
			Result.Purpose.ParameterGuidance = TEXT("Use the returned readable module name in the module candidate object. Put target_usage and optional emitter_name/stage_name on the stage target; the server re-enumerates the current native stack and fails closed on ambiguity.");
			Result.Purpose.ResultGuidance = TEXT("Default discovery returns names and one scope, including search-only native actions. Re-query needed names when target or stack context changes; do not copy scope into candidate. Request describe={name: returned_name} in the same scope for creation.supported/reason before choosing an unfamiliar action. Module input defaults require a created stack.");
			Result.Purpose.FailureGuidance = TEXT("Invalid or ambiguous readable output contexts and unresolved module names fail before mutation; the server re-enumerates the same context again at mutation.");
			Result.Purpose.SideEffectGuidance = TEXT("Read-only; it creates no module, package, file, or external state.");
			Result.Purpose.MinimumExample = TEXT("client.niagara.module_candidates(target, target_usage='system_update', query='System')");
			Result.Purpose.RelatedFlows = {
				TEXT("query.command.search"),
				TEXT("query.command.usage"),
				TEXT("content_graph.summary"),
				TEXT("content_graph.import_workspace"),
				TEXT("content_graph.items"),
			};
			Result.Purpose.Keywords = {TEXT("niagara"), TEXT("module"), TEXT("candidate"), TEXT("stack"), TEXT("hjson")};
			Result.Purpose.Synonyms = {TEXT("find niagara module"), TEXT("resolve niagara stack module")};
			return Result;
		}

		class FNiagaraCandidateContribution final : public IUeAgentActiveCommandContribution
		{
		public:
			explicit FNiagaraCandidateContribution(
				TSharedRef<IUeAgentNiagaraCandidateCommandService> InService)
				: Service(MoveTemp(InService))
			{
			}

			virtual const FString& GetContributionId() const override
			{
				static const FString Id = TEXT("niagara-domain-candidates");
				return Id;
			}

			virtual void CollectDeclarations(TArray<FUeAgentCommandDeclaration>& OutDeclarations) const override
			{
				OutDeclarations.Add(CandidateDeclaration());
			}

			virtual bool CreateHandlers(
				TSharedRef<const FUeAgentImmutableCommandRegistry> Registry,
				TArray<TSharedRef<IUeAgentExactCommandHandler>>& OutHandlers,
				UE::UAI::Contract::FDiagnostic& OutError) const override
			{
				(void)Registry;
				return TryCreateNiagaraCandidateCommandHandlers(Service, OutHandlers, OutError);
			}

		private:
			TSharedRef<IUeAgentNiagaraCandidateCommandService> Service;
		};

		bool TryCreateDomainCapability(
			TSharedPtr<const DomainCapability::
				FDomainCapabilityContribution>& OutContribution,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			TSharedPtr<HjsonWorkspace::IImportInitializationDomainProvider>
				Provider;
			HjsonWorkspace::FWorkspaceError WorkspaceError;
			if (!OperationOwners::Niagara::TryCreateNiagaraDomainProvider(
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
			Declaration.Hjson.Routes = {
				{
					ProviderRegistrationId,
					TEXT("content_graph.import_workspace"),
					TEXT("uai.niagara.edit/1"),
					TEXT("ue.niagara_system.asset"),
					TEXT("/Script/Niagara.NiagaraSystem"),
					HjsonWorkspace::EInitializationNodeRole::RootTarget,
					TEXT("uai.init.niagara_system.root/1"),
					true,
				},
				{
					ProviderRegistrationId,
					TEXT("content_graph.import_workspace"),
					TEXT("uai.niagara.edit/1"),
					TEXT("ue.niagara_emitter.asset"),
					TEXT("/Script/Niagara.NiagaraEmitter"),
					HjsonWorkspace::EInitializationNodeRole::RootTarget,
					TEXT("uai.init.niagara_emitter.root/1"),
					true,
				},
				{
					ProviderRegistrationId,
					TEXT("content_graph.import_workspace"),
					TEXT("uai.niagara.edit/1"),
					TEXT("ue.niagara_script.asset"),
					TEXT("/Script/Niagara.NiagaraScript"),
					HjsonWorkspace::EInitializationNodeRole::RootTarget,
					TEXT("uai.init.niagara_script.root/1"),
					true,
				},
			};
			Declaration.Initialization.Support =
				DomainCapability::EDomainCapabilitySupport::Supported;
			Declaration.Initialization.ContractIds = {
				TEXT("uai.init.niagara_system.root/1"),
				TEXT("uai.init.niagara_emitter.root/1"),
				TEXT("uai.init.niagara_script.root/1"),
				TEXT("uai.init.niagara.stack/1"),
				TEXT("uai.init.niagara.input/1"),
				TEXT("uai.init.niagara.parameter/1"),
				TEXT("uai.init.niagara.module/1"),
			};
			return DomainCapability::FDomainCapabilityContribution::TryCreate(
				MoveTemp(Declaration), OutContribution, OutError);
		}

		const DomainCapability::
			FDomainCapabilityContributionFactoryRegistration
			DomainCapabilityRegistration(*DomainId, &TryCreateDomainCapability);

		const FUeAgentActiveCommandContributionFactoryRegistration CandidateRegistration(
			TEXT("niagara-domain-candidates"),
			&TryCreateProductionNiagaraCandidateContribution);
	}

	const FString& GetProviderRegistrationId()
	{
		return ProviderRegistrationId;
	}

	bool TryCreateProductionNiagaraCandidateContribution(
		TSharedPtr<IUeAgentActiveCommandContribution>& OutContribution,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		TSharedPtr<IUeAgentNiagaraCandidateCommandService> Service;
		if (!TryCreateProductionNiagaraCandidateCommandService(Service, OutError)
			|| !Service.IsValid())
		{
			OutContribution.Reset();
			return false;
		}
		OutContribution = MakeShared<FNiagaraCandidateContribution>(Service.ToSharedRef());
		OutError.Reset();
		return true;
	}
}

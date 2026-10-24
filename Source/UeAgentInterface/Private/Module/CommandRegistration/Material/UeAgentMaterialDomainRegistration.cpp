#include "Module/CommandRegistration/Material/UeAgentMaterialDomainRegistration.h"

#include "CommandHandlers/Material/UeAgentMaterialDomainCommandHandlers.h"
#include "Core/Contract/UeAgentCommandContract.h"
#include "Core/DomainCapability/UeAgentDomainCapability.h"
#include "Core/HjsonWorkspace/ImportInitialization/UeAgentHjsonDomainProviderRegistry.h"
#include "OperationOwners/Material/UeAgentMaterialDomainService.h"

namespace UE::UAI::Module::CommandRegistration::Material
{
	using namespace Active;
	using namespace CommandContract;
	using namespace CommandEdge;
	using namespace CommandHandlers::Material;
	using namespace CommandRegistry;
	using namespace OperationOwners::Material;

	namespace
	{
		const FString DomainId = TEXT("material-command-migration");
		const FString ProviderRegistrationId = TEXT("material-domain-provider");
		const FString CandidateCommandId = TEXT("material.expression.candidates");
		const FString CandidateOwner = TEXT("material-domain-owner");
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
			Result.PythonNamespace = TEXT("material");
			Result.PythonFunction = TEXT("expression_candidates");
			Result.ContractVersion = 1;
			Result.PublicOwner = CandidateOwner;
			Result.Availability = EUeAgentCommandAvailability::Enabled;
			Result.SideEffect = EUeAgentCommandSideEffect::None;
			Result.StateLayers = {TEXT("asset_persisted_state")};
			Result.InputFields = {
				Field(TEXT("target"), TEXT("string"), true,
					TEXT("Persisted Material or Material Function name from content search, or its exact package.object path.")),
				Field(TEXT("query"), TEXT("string"), false,
					TEXT("Optional label or exact creation-name filter.")),
				Field(TEXT("graph_name"), TEXT("string"), false,
					TEXT("Optional exact MaterialGraph or MaterialFunctionGraph selector; defaults from target type.")),
				Field(TEXT("pin_name"), TEXT("string"), false,
					TEXT("Optional FromPin name; ambiguous names fail closed.")),
				Field(TEXT("describe"), TEXT("object"), false,
					TEXT("Optional selected candidate {name, select?, fields?}; return native editable defaults in workspace property shape. fields are native property names.")),
			};
			Result.ResultFields = {
				Field(TEXT("scope"), TEXT("object"), true, TEXT("Public target and any graph/pin query context; response metadata, not a selector.")),
				Field(TEXT("names"), TEXT("array"), true,
					TEXT("Ordered, readable Material expression names.")),
				Field(TEXT("description"), TEXT("object"), false,
					TEXT("Selected candidate property defaults and referenced types; present only when describe is requested.")),
			};
			Result.Errors = {
				{TEXT("material_candidate_query_authority_required"),
					TEXT("The uai:query authority is missing.")},
				{TEXT("material_candidate_target_unresolved"),
					TEXT("The exact target could not be loaded.")},
				{TEXT("material_candidate_target_type_unsupported"),
					TEXT("The target is not a Material or Material Function.")},
				{TEXT("material_candidate_palette_query_failed"),
					TEXT("The real Material palette could not be queried.")},
				{TEXT("material_action_candidate_machine_selector_not_supported"),
					TEXT("Machine identity selectors are not accepted; use the returned readable name.")},
			};
			Result.Purpose.Summary =
				TEXT("Resolve exact Material expression creation candidates from the real Material palette.");
			Result.Purpose.Target =
				TEXT("One exact persisted Material or Material Function graph owner.");
			Result.Purpose.Preconditions = {
				TEXT("Copy the target unchanged from public content search or readback."),
				TEXT("Run in the Editor on the UE game thread with uai:query."),
			};
			Result.Purpose.ParameterGuidance =
				TEXT("Search in the target graph/pin context, then use the returned name; provide a short semantic selector only when the name is ambiguous.");
			Result.Purpose.ResultGuidance =
				TEXT("Search returns names plus one scope. Re-query needed names when target or graph/pin context changes. describe returns editable defaults in that scope; dynamic pins require the created graph context. Do not copy scope into candidate.");
			Result.Purpose.FailureGuidance =
				TEXT("Unresolved targets, stale bindings, and unsupported types fail before any write and never fall back to reference.catalog_search.");
			Result.Purpose.SideEffectGuidance =
				TEXT("Read-only; it creates no expression, package, file, or external state.");
			Result.Purpose.MinimumExample =
				TEXT("client.material.expression_candidates(target, query='Multiply', graph_name='MaterialGraph')");
			Result.Purpose.RelatedFlows = {
				TEXT("query.command.search"),
				TEXT("query.command.usage"),
				TEXT("content_graph.summary"),
				TEXT("content_graph.import_workspace"),
			};
			Result.Purpose.Keywords = {
				TEXT("material"), TEXT("expression"), TEXT("candidate"),
				TEXT("hjson"), TEXT("palette"),
			};
			Result.Purpose.Synonyms = {
				TEXT("find material node"),
				TEXT("resolve material expression"),
			};
			return Result;
		}

		class FMaterialCandidateContribution final
			: public IUeAgentActiveCommandContribution
		{
		public:
			explicit FMaterialCandidateContribution(
				TSharedRef<IUeAgentMaterialCandidateCommandService> InService)
				: Service(MoveTemp(InService))
			{
			}

			virtual const FString& GetContributionId() const override
			{
				static const FString Id = TEXT("material-domain-candidates");
				return Id;
			}

			virtual void CollectDeclarations(
				TArray<FUeAgentCommandDeclaration>& OutDeclarations) const
				override
			{
				OutDeclarations.Add(CandidateDeclaration());
			}

			virtual bool CreateHandlers(
				TSharedRef<const FUeAgentImmutableCommandRegistry> Registry,
				TArray<TSharedRef<IUeAgentExactCommandHandler>>& OutHandlers,
				UE::UAI::Contract::FDiagnostic& OutError) const override
			{
				(void)Registry;
				return TryCreateMaterialCandidateCommandHandlers(
					Service, OutHandlers, OutError);
			}

		private:
			TSharedRef<IUeAgentMaterialCandidateCommandService> Service;
		};

		bool TryCreateDomainCapability(
			TSharedPtr<const DomainCapability::
				FDomainCapabilityContribution>& OutContribution,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			TSharedPtr<HjsonWorkspace::IImportInitializationDomainProvider>
				Provider;
			HjsonWorkspace::FWorkspaceError WorkspaceError;
			if (!OperationOwners::Material::TryCreateMaterialDomainProvider(
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
					TEXT("uai.material.edit/1"),
					TEXT("ue.material.asset"),
					TEXT("/Script/Engine.Material"),
					HjsonWorkspace::EInitializationNodeRole::RootTarget,
					TEXT("uai.init.material.root/1"),
					true,
				},
				{
					ProviderRegistrationId,
					TEXT("content_graph.import_workspace"),
					TEXT("uai.material.edit/1"),
					TEXT("ue.material_function.asset"),
					TEXT("/Script/Engine.MaterialFunction"),
					HjsonWorkspace::EInitializationNodeRole::RootTarget,
					TEXT("uai.init.material_function.root/1"),
					true,
				},
				{
					ProviderRegistrationId,
					TEXT("content_graph.import_workspace"),
					TEXT("uai.material.edit/1"),
					TEXT("ue.material_function.asset"),
					TEXT("/Script/Engine.MaterialFunctionMaterialLayer"),
					HjsonWorkspace::EInitializationNodeRole::RootTarget,
					TEXT("uai.init.material_function.root/1"),
					true,
				},
				{
					ProviderRegistrationId,
					TEXT("content_graph.import_workspace"),
					TEXT("uai.material.edit/1"),
					TEXT("ue.material_function.asset"),
					TEXT("/Script/Engine.MaterialFunctionMaterialLayerBlend"),
					HjsonWorkspace::EInitializationNodeRole::RootTarget,
					TEXT("uai.init.material_function.root/1"),
					true,
				},
				{
					ProviderRegistrationId,
					TEXT("content_graph.import_workspace"),
					TEXT("uai.material.edit/1"),
					TEXT("ue.material_instance.asset"),
					TEXT("/Script/Engine.MaterialInstanceConstant"),
					HjsonWorkspace::EInitializationNodeRole::RootTarget,
					TEXT("uai.init.material_instance.root/1"),
					true,
				},
			};
			Declaration.Initialization.Support =
				DomainCapability::EDomainCapabilitySupport::Supported;
			Declaration.Initialization.ContractIds = {
				TEXT("uai.init.material.root/1"),
				TEXT("uai.init.material_function.root/1"),
				TEXT("uai.init.material_instance.root/1"),
				TEXT("uai.init.material.expression/1"),
			};
			return DomainCapability::FDomainCapabilityContribution::TryCreate(
				MoveTemp(Declaration), OutContribution, OutError);
		}

		const DomainCapability::
			FDomainCapabilityContributionFactoryRegistration
			DomainCapabilityRegistration(*DomainId, &TryCreateDomainCapability);

		const FUeAgentActiveCommandContributionFactoryRegistration
			CandidateRegistration(
				TEXT("material-domain-candidates"),
				&TryCreateProductionMaterialCandidateContribution);
	}

	const FString& GetProviderRegistrationId()
	{
		return ProviderRegistrationId;
	}

	bool TryCreateProductionMaterialCandidateContribution(
		TSharedPtr<IUeAgentActiveCommandContribution>& OutContribution,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		TSharedPtr<IUeAgentMaterialCandidateCommandService> Service;
		if (!TryCreateProductionMaterialCandidateCommandService(
				Service, OutError)
			|| !Service.IsValid())
		{
			OutContribution.Reset();
			return false;
		}
		OutContribution = MakeShared<FMaterialCandidateContribution>(
			Service.ToSharedRef());
		OutError.Reset();
		return true;
	}
}

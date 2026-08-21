#include "Module/CommandRegistration/Blueprint/Surfaces/UeAgentBlueprintSurfaceRegistration.h"

#include "Core/DomainCapability/UeAgentDomainCapability.h"
#include "Core/HjsonWorkspace/ImportInitialization/UeAgentHjsonDomainProviderRegistry.h"
#include "OperationOwners/Blueprint/Surfaces/UeAgentBlueprintSurfaceDomainService.h"

namespace UE::UAI::Module::CommandRegistration::Blueprint::Surfaces
{
	namespace
	{
		const FString DomainId = TEXT("blueprint-command-migration");
		const FString ProviderRegistrationId =
			TEXT("blueprint-surface-domain-provider");

		bool TryCreateDomainCapability(
			TSharedPtr<const DomainCapability::
				FDomainCapabilityContribution>& OutContribution,
			FString& OutError)
		{
			TSharedPtr<HjsonWorkspace::IImportInitializationDomainProvider>
				Provider;
			HjsonWorkspace::FWorkspaceError WorkspaceError;
			if (!OperationOwners::Blueprint::Surfaces::
					TryCreateBlueprintSurfaceDomainProvider(
						Provider, WorkspaceError)
				|| !Provider.IsValid())
			{
				OutError = WorkspaceError.GetCode();
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
				TEXT("uai.blueprint.edit/1"),
				TEXT("ue.blueprint.asset"),
				TEXT("/Script/Engine.Blueprint"),
				HjsonWorkspace::EInitializationNodeRole::RootTarget,
				TEXT("uai.init.blueprint.asset/1"),
				true,
			}};
			Declaration.Initialization.Support =
				DomainCapability::EDomainCapabilitySupport::Supported;
			Declaration.Initialization.ContractIds = {
				TEXT("uai.init.blueprint.asset/1"),
				TEXT("uai.init.blueprint.interface/1"),
				TEXT("uai.init.blueprint.member/1"),
				TEXT("uai.init.blueprint.component/1"),
				TEXT("uai.init.blueprint.graph/1"),
				TEXT("uai.init.blueprint.node/1"),
				TEXT("uai.init.blueprint.pin/1"),
				TEXT("uai.init.blueprint.edge/1"),
			};
			Declaration.Migration.Support =
				DomainCapability::EDomainCapabilitySupport::Supported;
			Declaration.Migration.OwnerId = TEXT("T14-BLUEPRINT-SURFACES");
			return DomainCapability::FDomainCapabilityContribution::TryCreate(
				MoveTemp(Declaration), OutContribution, OutError);
		}

		const DomainCapability::
			FDomainCapabilityContributionFactoryRegistration
			DomainCapabilityRegistration(*DomainId, &TryCreateDomainCapability);
	}

	const FString& GetProviderRegistrationId()
	{
		return ProviderRegistrationId;
	}
}

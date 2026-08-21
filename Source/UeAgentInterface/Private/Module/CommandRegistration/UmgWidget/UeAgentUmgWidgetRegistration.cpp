#include "Module/CommandRegistration/UmgWidget/UeAgentUmgWidgetRegistration.h"

#include "Core/DomainCapability/UeAgentDomainCapability.h"
#include "Core/HjsonWorkspace/ImportInitialization/UeAgentHjsonDomainProviderRegistry.h"
#include "OperationOwners/UmgWidget/UeAgentUmgWidgetDomainService.h"

namespace UE::UAI::Module::CommandRegistration::UmgWidget
{
	namespace
	{
		const FString DomainId = TEXT("umg-command-migration");
		const FString ProviderRegistrationId = TEXT("umg-widget-domain-provider");

		bool TryCreateDomainCapability(
			TSharedPtr<const DomainCapability::
				FDomainCapabilityContribution>& OutContribution,
			FString& OutError)
		{
			TSharedPtr<HjsonWorkspace::IImportInitializationDomainProvider>
				Provider;
			HjsonWorkspace::FWorkspaceError WorkspaceError;
			if (!OperationOwners::UmgWidget::TryCreateUmgWidgetDomainProvider(
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
				TEXT("uai.umg.edit/1"),
				TEXT("ue.widget_blueprint.asset"),
				TEXT("/Script/UMGEditor.WidgetBlueprint"),
				HjsonWorkspace::EInitializationNodeRole::RootTarget,
				TEXT("uai.init.umg.widget_blueprint/1"),
				true,
			}};
			Declaration.Initialization.Support =
				DomainCapability::EDomainCapabilitySupport::Supported;
			Declaration.Initialization.ContractIds = {
				TEXT("uai.init.umg.widget_blueprint/1"),
				TEXT("uai.init.umg.widget/1"),
				TEXT("uai.init.umg.slot/1"),
				TEXT("uai.init.umg.binding/1"),
				TEXT("uai.init.umg.animation/1"),
			};
			Declaration.Migration.Support =
				DomainCapability::EDomainCapabilitySupport::Supported;
			Declaration.Migration.OwnerId = TEXT("T15-UMG-WIDGET");
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

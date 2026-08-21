#include "Module/CommandRegistration/AnimationBlueprint/UeAgentAnimationBlueprintRegistration.h"

#include "Core/DomainCapability/UeAgentDomainCapability.h"
#include "Core/HjsonWorkspace/ImportInitialization/UeAgentHjsonDomainProviderRegistry.h"
#include "OperationOwners/AnimationBlueprint/UeAgentAnimationBlueprintDomainService.h"

namespace UE::UAI::Module::CommandRegistration::AnimationBlueprint
{
	namespace
	{
		const FString DomainId = TEXT("animation-blueprint-command-migration");
		const FString ProviderRegistrationId =
			TEXT("animation-blueprint-domain-provider");

		bool TryCreateDomainCapability(
			TSharedPtr<const DomainCapability::
				FDomainCapabilityContribution>& OutContribution,
			FString& OutError)
		{
			TSharedPtr<HjsonWorkspace::IImportInitializationDomainProvider>
				Provider;
			HjsonWorkspace::FWorkspaceError WorkspaceError;
			if (!OperationOwners::AnimationBlueprint::
					TryCreateAnimationBlueprintDomainProvider(
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
				TEXT("uai.animation_blueprint.edit/1"),
				TEXT("ue.anim_blueprint.asset"),
				TEXT("/Script/Engine.AnimBlueprint"),
				HjsonWorkspace::EInitializationNodeRole::RootTarget,
				TEXT("uai.init.animation_blueprint.asset/1"),
				true,
			}};
			Declaration.Initialization.Support =
				DomainCapability::EDomainCapabilitySupport::Supported;
			Declaration.Initialization.ContractIds = {
				TEXT("uai.init.animation_blueprint.asset/1"),
				TEXT("uai.init.animation_blueprint.graph/1"),
				TEXT("uai.init.animation_blueprint.state_machine/1"),
				TEXT("uai.init.animation_blueprint.state/1"),
				TEXT("uai.init.animation_blueprint.transition/1"),
				TEXT("uai.init.animation_blueprint.class_settings/1"),
			};
			Declaration.Migration.Support =
				DomainCapability::EDomainCapabilitySupport::Supported;
			Declaration.Migration.OwnerId =
				TEXT("T16-ANIMATION-BLUEPRINT");
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

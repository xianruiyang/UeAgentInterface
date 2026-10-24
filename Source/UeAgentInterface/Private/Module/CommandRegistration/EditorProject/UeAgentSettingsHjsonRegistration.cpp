#include "Core/DomainCapability/UeAgentDomainCapability.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "DomainAdapters/EditorProject/UeAgentSettingsAuthoring.h"

namespace UE::UAI::Module::CommandRegistration::EditorProject
{
namespace
{
bool Create(TSharedPtr<const DomainCapability::FDomainCapabilityContribution>& Out, UE::UAI::Contract::FDiagnostic& Error)
{
    auto Declaration = DomainCapability::MakeExplicitlyUnsupportedDomainCapabilityDeclaration(TEXT("project-settings-hjson"));
    auto Provider = DomainAdapters::EditorProject::CreateSettingsHjsonProvider();
    Declaration.Hjson.Support = DomainCapability::EDomainCapabilitySupport::Supported;
    Declaration.Hjson.Providers.Add(Provider);
    Declaration.Hjson.Routes.Add({Provider->GetProviderId(), TEXT("content_graph.import_workspace"),
        TEXT("uai.project_settings.edit/1"), TEXT("ue.project_settings.config"), TEXT("/Script/CoreUObject.Object"),
        HjsonWorkspace::EInitializationNodeRole::RootTarget, TEXT("uai.init.project_settings.root/1"), false});
    Declaration.Initialization.Support = DomainCapability::EDomainCapabilitySupport::Supported;
    Declaration.Initialization.ContractIds = {TEXT("uai.init.project_settings.root/1")};
    return DomainCapability::FDomainCapabilityContribution::TryCreate(MoveTemp(Declaration), Out, Error);
}
const DomainCapability::FDomainCapabilityContributionFactoryRegistration Registration(TEXT("project-settings-hjson"), &Create);
}
}

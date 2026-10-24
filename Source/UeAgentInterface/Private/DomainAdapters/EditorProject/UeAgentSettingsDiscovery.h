#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"

class ISettingsContainer;
class ISettingsCategory;
class ISettingsSection;
class UObject;

namespace UE::UAI::DomainAdapters::EditorProject
{
struct FSettingsPage
{
	FName ContainerName;
	FName CategoryName;
	FName SectionName;
	FString SectionId;
	TSharedPtr<ISettingsContainer> Container;
	TSharedPtr<ISettingsCategory> Category;
	TSharedPtr<ISettingsSection> Section;
	UObject* SettingsObject = nullptr;
};

// The settings registry contains custom pages; auto-discovered developer settings
// also exist when their viewer has not registered a page in this Editor session.
UEAGENTINTERFACE_API bool CollectSettingsPages(TArray<FSettingsPage>& Pages, UE::UAI::Contract::FDiagnostic& Error);
UEAGENTINTERFACE_API bool FindSettingsPage(const FString& ObjectPath, FSettingsPage& Page, UE::UAI::Contract::FDiagnostic& Error);
UEAGENTINTERFACE_API bool FindSettingsPageBySectionId(const FString& SectionId, FSettingsPage& Page, UE::UAI::Contract::FDiagnostic& Error);
}

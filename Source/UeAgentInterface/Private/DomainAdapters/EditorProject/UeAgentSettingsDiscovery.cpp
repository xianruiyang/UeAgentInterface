#include "DomainAdapters/EditorProject/UeAgentSettingsDiscovery.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "Dom/JsonValue.h"

#include "Engine/DeveloperSettings.h"
#include "ISettingsCategory.h"
#include "ISettingsContainer.h"
#include "ISettingsModule.h"
#include "ISettingsSection.h"
#include "Modules/ModuleManager.h"
#include "UObject/UObjectIterator.h"

namespace UE::UAI::DomainAdapters::EditorProject
{
bool CollectSettingsPages(TArray<FSettingsPage>& Pages, UE::UAI::Contract::FDiagnostic& Error)
{
	Pages.Reset();
	Error.Reset();
	ISettingsModule* Module = FModuleManager::LoadModulePtr<ISettingsModule>(TEXT("Settings"));
	if (!Module)
	{
		Error = UE::UAI::Contract::FDiagnostic::Failure(TEXT("settings_module_unavailable"),
			TEXT("The Settings module could not be loaded for settings discovery."), TEXT("settings_discovery"));
		Error.SourceKind = TEXT("native_result");
		Error.CandidateName = TEXT("Settings");
		Error.PropertyPath = TEXT("/module_available");
		Error.ActualValue = MakeShared<FJsonValueBoolean>(false);
		Error.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
		Error.ActualValuePresent = true;
		Error.ExpectedValuePresent = true;
		return false;
	}
	TSet<FString> Known;
	auto Add = [&](FSettingsPage Page)
	{
		Page.SectionId = Page.ContainerName.ToString() + TEXT(".") + Page.CategoryName.ToString() + TEXT(".") + Page.SectionName.ToString();
		if (!Known.Contains(Page.SectionId)) { Known.Add(Page.SectionId); Pages.Add(MoveTemp(Page)); }
	};
	TArray<FName> Containers;
	Module->GetContainerNames(Containers);
	for (const FName ContainerName : Containers)
	{
		const auto Container = Module->GetContainer(ContainerName);
		if (!Container) continue;
		TArray<TSharedPtr<ISettingsCategory>> Categories;
		Container->GetCategories(Categories);
		for (const auto& Category : Categories)
		{
			if (!Category) continue;
			TArray<TSharedPtr<ISettingsSection>> Sections;
			Category->GetSections(Sections, true);
			for (const auto& Section : Sections)
			{
				if (!Section) continue;
				FSettingsPage Page;
				Page.ContainerName = ContainerName;
				Page.CategoryName = Category->GetName();
				Page.SectionName = Section->GetName();
				Page.Container = Container;
				Page.Category = Category;
				Page.Section = Section;
				Page.SettingsObject = Section->GetSettingsObject().Get();
				Add(MoveTemp(Page));
			}
		}
	}
	for (TObjectIterator<UClass> It; It; ++It)
	{
		UClass* Class = *It;
		if (!Class->IsChildOf(UDeveloperSettings::StaticClass())
			|| Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists)) continue;
		auto* Settings = Cast<UDeveloperSettings>(Class->GetDefaultObject());
		if (!Settings || !Settings->SupportsAutoRegistration()) continue;
		FSettingsPage Page;
		Page.ContainerName = Settings->GetContainerName();
		Page.CategoryName = Settings->GetCategoryName();
		Page.SectionName = Settings->GetSectionName();
		Page.SettingsObject = Settings;
		Add(MoveTemp(Page));
	}
	Pages.Sort([](const FSettingsPage& A, const FSettingsPage& B) { return A.SectionId < B.SectionId; });
	return true;
}

bool FindSettingsPageBySectionId(const FString& SectionId, FSettingsPage& Page, UE::UAI::Contract::FDiagnostic& Error)
{
	TArray<FSettingsPage> Pages;
	if (!CollectSettingsPages(Pages, Error)) return false;
	for (const auto& Candidate : Pages)
	{
		if (Candidate.SectionId.Equals(SectionId, ESearchCase::IgnoreCase))
		{ Page = Candidate; return true; }
	}
	Error = UE::UAI::Contract::FDiagnostic::Failure(TEXT("settings_section_not_found"),
		TEXT("The requested settings section is not registered."), TEXT("settings_discovery"));
	Error.CandidateName = SectionId;
	Error.PropertyPath = TEXT("/section_id");
	Error.ActualValue = MakeShared<FJsonValueString>(SectionId);
	Error.ExpectedValue = MakeShared<FJsonValueString>(TEXT("a registered settings section"));
	Error.ActualValuePresent = true;
	Error.ExpectedValuePresent = true;
	return false;
}

bool FindSettingsPage(const FString& ObjectPath, FSettingsPage& Page, UE::UAI::Contract::FDiagnostic& Error)
{
	TArray<FSettingsPage> Pages;
	if (!CollectSettingsPages(Pages, Error)) return false;
	for (const auto& Candidate : Pages)
	{
		if (Candidate.SettingsObject && Candidate.SettingsObject->GetPathName() == ObjectPath)
		{ Page = Candidate; return true; }
	}
	Error = UE::UAI::Contract::FDiagnostic::Failure(TEXT("settings_object_not_registered"),
		TEXT("The requested native object is not registered as a settings page."), TEXT("settings_discovery"),
		ObjectPath);
	Error.SourceKind = TEXT("native_result");
	Error.SubjectId = ObjectPath;
	Error.PropertyPath = TEXT("/settings_page_registered");
	Error.ActualValue = MakeShared<FJsonValueBoolean>(false);
	Error.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
	Error.ActualValuePresent = true;
	Error.ExpectedValuePresent = true;
	return false;
}
}

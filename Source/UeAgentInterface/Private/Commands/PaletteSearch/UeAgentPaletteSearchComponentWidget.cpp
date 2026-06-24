// Copyright Epic Games, Inc. All Rights Reserved.

#include "Commands/PaletteSearch/UeAgentPaletteSearchTypes.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Blueprint/BlueprintSupport.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "Commands/PaletteSearch/UeAgentPaletteSearchAuthoring.h"
#include "Commands/PaletteSearch/UeAgentPaletteSearchNaming.h"
#include "Components/ActorComponent.h"
#include "Components/Widget.h"
#include "ComponentTypeRegistry.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Internationalization/Text.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/App.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "SComponentClassCombo.h"
#include "UObject/CoreRedirects.h"
#include "UObject/UObjectIterator.h"
#include "WidgetBlueprint.h"
#include "WidgetBlueprintEditorUtils.h"

namespace UeAgentPaletteSearch
{
namespace
{
	static FString NormalizeToken(const FString& Value)
	{
		return Value.TrimStartAndEnd().ToLower();
	}

	static FString SourceOriginFromPath(const FString& Path)
	{
		if (Path.StartsWith(TEXT("/Game/")))
		{
			return Path.StartsWith(TEXT("/Game/Developers/")) ? TEXT("developer") : TEXT("project");
		}
		if (Path.StartsWith(TEXT("/Engine/")))
		{
			return TEXT("engine");
		}
		if (Path.StartsWith(TEXT("/Script/")))
		{
			const FString ModuleAndObject = Path.Mid(8);
			FString ModuleName;
			if (!ModuleAndObject.Split(TEXT("."), &ModuleName, nullptr))
			{
				ModuleName = ModuleAndObject;
			}
			if (ModuleName.Equals(FApp::GetProjectName(), ESearchCase::IgnoreCase))
			{
				return TEXT("project");
			}

			const bool bEngineModule =
				ModuleName.Equals(TEXT("CoreUObject"), ESearchCase::IgnoreCase) ||
				ModuleName.Equals(TEXT("Engine"), ESearchCase::IgnoreCase) ||
				ModuleName.Equals(TEXT("InputCore"), ESearchCase::IgnoreCase) ||
				ModuleName.Equals(TEXT("NavigationSystem"), ESearchCase::IgnoreCase) ||
				ModuleName.Equals(TEXT("Slate"), ESearchCase::IgnoreCase) ||
				ModuleName.Equals(TEXT("SlateCore"), ESearchCase::IgnoreCase) ||
				ModuleName.Equals(TEXT("UMG"), ESearchCase::IgnoreCase);
			return bEngineModule ? TEXT("engine") : TEXT("plugins");
		}
		return Path.StartsWith(TEXT("/")) ? TEXT("plugins") : TEXT("unknown");
	}

	static FString SourceOriginFromClass(const UClass* Class, const FString& FallbackPath)
	{
		if (Class)
		{
			return SourceOriginFromPath(Class->GetPathName());
		}
		return SourceOriginFromPath(FallbackPath);
	}

	static bool IsSourceAllowed(const FString& SourceOrigin, const TArray<FString>& SourceFilters)
	{
		if (SourceFilters.IsEmpty())
		{
			return true;
		}

		const FString NormalizedOrigin = NormalizeToken(SourceOrigin);
		for (const FString& SourceFilter : SourceFilters)
		{
			const FString NormalizedFilter = NormalizeToken(SourceFilter);
			if (NormalizedFilter.IsEmpty())
			{
				continue;
			}
			if (NormalizedFilter.Equals(NormalizedOrigin))
			{
				return true;
			}
			if (NormalizedFilter.Equals(TEXT("game")) && NormalizedOrigin.Equals(TEXT("project")))
			{
				return true;
			}
		}
		return false;
	}

	static FString MakeSafeObjectName(const FString& Name, const FString& Suffix)
	{
		FString Result;
		for (const TCHAR Ch : Name)
		{
			if (FChar::IsAlnum(Ch) || Ch == TEXT('_'))
			{
				Result.AppendChar(Ch);
			}
		}
		if (Result.IsEmpty())
		{
			Result = TEXT("New");
		}
		if (!Suffix.IsEmpty() && !Result.EndsWith(Suffix, ESearchCase::IgnoreCase))
		{
			Result += Suffix;
		}
		return Result;
	}

	static TSharedRef<FJsonObject> BuildLegacyAddComponent(const FCandidate& Candidate)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("operation"), TEXT("blueprint_add_component"));
		Obj->SetStringField(TEXT("component_class"), Candidate.FullName);
		Obj->SetStringField(TEXT("component_name"), MakeSafeObjectName(Candidate.Name, TEXT("Component")));
		return Obj;
	}

	static TSharedRef<FJsonObject> BuildLegacyAddWidget(const FCandidate& Candidate)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("operation"), TEXT("umg_add_widget"));
		Obj->SetStringField(TEXT("widget_class"), Candidate.FullName);
		Obj->SetStringField(TEXT("widget_name"), MakeSafeObjectName(Candidate.Name, TEXT("Widget")));
		Obj->SetBoolField(TEXT("make_variable"), false);
		return Obj;
	}

	static FString FullNameFromComponentEntry(const FComponentClassComboEntryPtr& Entry)
	{
		if (UClass* ComponentClass = Entry->GetComponentClass())
		{
			return ComponentClass->GetPathName();
		}
		return Entry->GetComponentPath();
	}

	static FString DisplayNameFromClassPath(const FString& FullName)
	{
		FString DisplayName = FPackageName::GetLongPackageAssetName(FullName);
		if (DisplayName.IsEmpty())
		{
			DisplayName = FullName;
		}
		DisplayName.RemoveFromEnd(TEXT("_C"), ESearchCase::CaseSensitive);
		DisplayName.RemoveFromEnd(TEXT("Component"), ESearchCase::IgnoreCase);
		return FName::NameToDisplayString(DisplayName, false);
	}

	static FString ActorComponentMenuName(const FComponentClassComboEntryPtr& Entry, const FString& FullName)
	{
		if (!Entry.IsValid())
		{
			return DisplayNameFromClassPath(FullName);
		}
		if (!Entry->GetComponentNameOverride().IsEmpty())
		{
			return FName::NameToDisplayString(Entry->GetComponentNameOverride(), false);
		}
		if (UClass* ComponentClass = Entry->GetComponentClass())
		{
			FString DisplayName;
			if (ComponentClass->HasMetaData(TEXT("DisplayName")))
			{
				DisplayName = ComponentClass->GetMetaData(TEXT("DisplayName"));
			}
			else
			{
				DisplayName = MenuNameFromText(ComponentClass->GetDisplayNameText(), ComponentClass->GetName());
				if (!ComponentClass->HasAnyClassFlags(CLASS_CompiledFromBlueprint))
				{
					DisplayName.RemoveFromEnd(TEXT("Component"), ESearchCase::IgnoreCase);
				}
			}
			DisplayName.TrimStartAndEndInline();
			return FName::NameToDisplayString(DisplayName, false);
		}
		return DisplayNameFromClassPath(FullName);
	}

	static void AddActorComponentCandidates(const FSearchParams& Params, TArray<FCandidate>& OutCandidates, TSet<FString>& SeenFullNames)
	{
		TArray<FComponentClassComboEntryPtr>* ComponentClassList = nullptr;
		FComponentTypeRegistry::Get().SubscribeToComponentList(ComponentClassList);
		if (!ComponentClassList)
		{
			return;
		}

		FString CurrentSection = TEXT("Components");
		int32 UiOrder = 0;
		for (const FComponentClassComboEntryPtr& Entry : *ComponentClassList)
		{
			if (!Entry.IsValid())
			{
				continue;
			}
			if (Entry->IsHeading())
			{
				CurrentSection = Entry->GetHeadingText().TrimStartAndEnd();
				if (CurrentSection.IsEmpty())
				{
					CurrentSection = TEXT("Components");
				}
				continue;
			}
			if (!Entry->IsClass() || Entry->GetComponentCreateAction() != EComponentCreateAction::SpawnExistingClass)
			{
				continue;
			}
			if (Entry->GetAssetOverride())
			{
				continue;
			}

			const FString FullName = FullNameFromComponentEntry(Entry);
			if (FullName.IsEmpty() || SeenFullNames.Contains(FullName))
			{
				continue;
			}

			UClass* ComponentClass = Entry->GetComponentClass();
			const FString SourceOrigin = SourceOriginFromClass(ComponentClass, FullName);
			if (!IsSourceAllowed(SourceOrigin, Params.SourceFilters))
			{
				continue;
			}

			SeenFullNames.Add(FullName);
			const FString Name = ActorComponentMenuName(Entry, FullName);
			FCandidate Candidate;
			Candidate.Kind = TEXT("actor_component");
			Candidate.Type = Candidate.Kind;
			Candidate.Name = Name.IsEmpty() ? DisplayNameFromClassPath(FullName) : Name;
			Candidate.DisplayName = Candidate.Name;
			Candidate.FullName = FullName;
			Candidate.StableId = FullName;
			Candidate.CanonicalKey = Candidate.Kind + TEXT(":") + Candidate.FullName;
			Candidate.CandidateId = Candidate.CanonicalKey;
			Candidate.CategoryPath = CurrentSection;
			Candidate.Section = CurrentSection;
			Candidate.SourceOrigin = SourceOrigin;
			Candidate.SourceLabel = SourceOrigin;
			Candidate.PackagePath = ComponentClass ? ComponentClass->GetOutermost()->GetName() : FPackageName::ObjectPathToPackageName(FullName);
			Candidate.ObjectPath = FullName;
			Candidate.Tooltip = ComponentClass ? ComponentClass->GetToolTipText().ToString() : Candidate.Name;
			Candidate.Keywords = FString::Printf(TEXT("%s %s %s"), *Candidate.Name, *FullName, *CurrentSection);
			Candidate.WriteSupport = TEXT("blueprint.components[].component_class");
			Candidate.SupportReason = TEXT("Use full_name as component_class in Blueprint folder JSON; the same class path is accepted by blueprint_add_component compatibility.");
			Candidate.bInLibrary = true;
			Candidate.bSupportedInContext = true;
			Candidate.UiOrder = UiOrder++;
			Candidate.JsonAuthoring = BuildJsonAuthoringPatch(Candidate);
			Candidate.LegacyAuthoring = BuildLegacyAddComponent(Candidate);
			OutCandidates.Add(MoveTemp(Candidate));
		}
	}

	static FString ObjectPathFromExportTextPath(const FString& Path)
	{
		if (Path.IsEmpty())
		{
			return FString();
		}
		return FPackageName::ExportTextPathToObjectPath(Path);
	}

	static UClass* LoadClassFromAssetTagPath(const FString& Path)
	{
		const FString ObjectPath = ObjectPathFromExportTextPath(Path);
		if (ObjectPath.IsEmpty() || !ObjectPath.StartsWith(TEXT("/")))
		{
			return nullptr;
		}
		const FString RedirectedPath = FCoreRedirects::GetRedirectedName(ECoreRedirectFlags::Type_Class, FCoreRedirectObjectName(ObjectPath)).ToString();
		if (UClass* ExistingClass = FindObject<UClass>(nullptr, *RedirectedPath))
		{
			return ExistingClass;
		}
		return LoadObject<UClass>(nullptr, *RedirectedPath);
	}

	static bool IsWidgetClassUsableWithoutEditor(const UClass* WidgetClass)
	{
		if (!WidgetClass || !WidgetClass->IsChildOf(UWidget::StaticClass()))
		{
			return false;
		}
		if (WidgetClass == UWidget::StaticClass() || WidgetClass == UUserWidget::StaticClass())
		{
			return false;
		}
		if (WidgetClass->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
		{
			return false;
		}
		if (WidgetClass->HasAnyFlags(RF_Transient) && WidgetClass->HasAnyClassFlags(CLASS_CompiledFromBlueprint))
		{
			return false;
		}
		return true;
	}

	static FString ClassMenuName(const UClass* Class, const FString& Fallback)
	{
		FString Name = Class ? MenuNameFromText(Class->GetDisplayNameText(), Fallback) : Fallback;
		if (Name.IsEmpty())
		{
			Name = Fallback;
		}
		Name.RemoveFromEnd(TEXT("_C"), ESearchCase::CaseSensitive);
		return FName::NameToDisplayString(Name, false);
	}

	static void FillWidgetCandidate(
		const FString& FullName,
		const FString& Name,
		const FString& Category,
		const FString& SourceOrigin,
		const FString& PackagePath,
		const FString& ObjectPath,
		const FString& Tooltip,
		int32 UiOrder,
		FCandidate& OutCandidate)
	{
		OutCandidate.Kind = TEXT("umg_widget");
		OutCandidate.Type = OutCandidate.Kind;
		OutCandidate.Name = Name;
		OutCandidate.DisplayName = Name;
		OutCandidate.FullName = FullName;
		OutCandidate.StableId = FullName;
		OutCandidate.CanonicalKey = OutCandidate.Kind + TEXT(":") + FullName;
		OutCandidate.CandidateId = OutCandidate.CanonicalKey;
		OutCandidate.CategoryPath = Category.IsEmpty() ? TEXT("Widget") : Category;
		OutCandidate.Section = OutCandidate.CategoryPath;
		OutCandidate.SourceOrigin = SourceOrigin;
		OutCandidate.SourceLabel = SourceOrigin;
		OutCandidate.PackagePath = PackagePath;
		OutCandidate.ObjectPath = ObjectPath.IsEmpty() ? FullName : ObjectPath;
		OutCandidate.Tooltip = Tooltip;
		OutCandidate.Keywords = FString::Printf(TEXT("%s %s %s"), *Name, *FullName, *OutCandidate.CategoryPath);
		OutCandidate.WriteSupport = TEXT("umg.widget_tree.widgets[].widget_class");
		OutCandidate.SupportReason = TEXT("Use full_name as widget_class in UMG folder JSON; the same class path is accepted by umg_add_widget compatibility.");
		OutCandidate.bInLibrary = true;
		OutCandidate.bSupportedInContext = true;
		OutCandidate.UiOrder = UiOrder;
		OutCandidate.JsonAuthoring = BuildJsonAuthoringPatch(OutCandidate);
		OutCandidate.LegacyAuthoring = BuildLegacyAddWidget(OutCandidate);
	}

	static void AddLoadedWidgetCandidates(const FSearchParams& Params, TArray<FCandidate>& OutCandidates, TSet<FString>& SeenFullNames, int32& InOutUiOrder)
	{
		for (TObjectIterator<UClass> ClassIt; ClassIt; ++ClassIt)
		{
			UClass* WidgetClass = *ClassIt;
			if (!IsWidgetClassUsableWithoutEditor(WidgetClass))
			{
				continue;
			}

			const FString FullName = WidgetClass->GetPathName();
			if (FullName.IsEmpty() || SeenFullNames.Contains(FullName))
			{
				continue;
			}

			const FString SourceOrigin = SourceOriginFromClass(WidgetClass, FullName);
			if (!IsSourceAllowed(SourceOrigin, Params.SourceFilters))
			{
				continue;
			}

			SeenFullNames.Add(FullName);
			FCandidate Candidate;
			FillWidgetCandidate(
				FullName,
				ClassMenuName(WidgetClass, WidgetClass->GetName()),
				MenuNameFromText(FWidgetBlueprintEditorUtils::GetPaletteCategory(TSubclassOf<UWidget>(WidgetClass)), TEXT("Widget")),
				SourceOrigin,
				WidgetClass->GetOutermost()->GetName(),
				FullName,
				WidgetClass->GetToolTipText().ToString(),
				InOutUiOrder++,
				Candidate);
			OutCandidates.Add(MoveTemp(Candidate));
		}
	}

	static bool AddWidgetBlueprintAssetCandidates(const FSearchParams& Params, TArray<FCandidate>& OutCandidates, TSet<FString>& SeenFullNames, int32& InOutUiOrder, FString& OutError)
	{
		FAssetRegistryModule* AssetRegistryModule = FModuleManager::LoadModulePtr<FAssetRegistryModule>(TEXT("AssetRegistry"));
		if (!AssetRegistryModule)
		{
			OutError = TEXT("asset_registry_module_unavailable");
			return false;
		}

		TArray<FAssetData> Assets;
		AssetRegistryModule->Get().GetAssetsByClass(UWidgetBlueprint::StaticClass()->GetClassPathName(), Assets, true);
		AssetRegistryModule->Get().GetAssetsByClass(UWidgetBlueprintGeneratedClass::StaticClass()->GetClassPathName(), Assets, true);

		for (const FAssetData& Asset : Assets)
		{
			FString GeneratedClassTag;
			if (!Asset.GetTagValue(FBlueprintTags::GeneratedClassPath, GeneratedClassTag))
			{
				continue;
			}
			const FString FullName = ObjectPathFromExportTextPath(GeneratedClassTag);
			if (FullName.IsEmpty() || SeenFullNames.Contains(FullName))
			{
				continue;
			}

			FString NativeParentTag;
			if (!Asset.GetTagValue(FBlueprintTags::NativeParentClassPath, NativeParentTag))
			{
				Asset.GetTagValue(FBlueprintTags::ParentClassPath, NativeParentTag);
			}
			UClass* NativeParentClass = LoadClassFromAssetTagPath(NativeParentTag);
			if (!NativeParentClass || !NativeParentClass->IsChildOf(UWidget::StaticClass()))
			{
				continue;
			}

			const FString SourceOrigin = SourceOriginFromPath(FullName);
			if (!IsSourceAllowed(SourceOrigin, Params.SourceFilters))
			{
				continue;
			}

			SeenFullNames.Add(FullName);
			const FString Category = MenuNameFromText(FWidgetBlueprintEditorUtils::GetPaletteCategory(Asset, TSubclassOf<UWidget>(NativeParentClass)), TEXT("Widget"));
			FCandidate Candidate;
			FillWidgetCandidate(
				FullName,
				FName::NameToDisplayString(Asset.AssetName.ToString(), false),
				Category,
				SourceOrigin,
				Asset.PackageName.ToString(),
				Asset.GetSoftObjectPath().ToString(),
				Asset.GetTagValueRef<FString>(FBlueprintTags::BlueprintDescription),
				InOutUiOrder++,
				Candidate);
			OutCandidates.Add(MoveTemp(Candidate));
		}
		return true;
	}
}

bool CollectComponentWidgetCandidates(const FSearchParams& Params, TArray<FCandidate>& OutCandidates, TArray<FString>& OutWarnings, FString& OutError)
{
	TSet<FString> SeenFullNames;
	if (Params.CatalogType.Equals(TEXT("actor_component")))
	{
		AddActorComponentCandidates(Params, OutCandidates, SeenFullNames);
		return true;
	}

	if (Params.CatalogType.Equals(TEXT("umg_widget")))
	{
		int32 UiOrder = 0;
		AddLoadedWidgetCandidates(Params, OutCandidates, SeenFullNames, UiOrder);
		if (!AddWidgetBlueprintAssetCandidates(Params, OutCandidates, SeenFullNames, UiOrder, OutError))
		{
			return false;
		}
		return true;
	}

	OutWarnings.Add(FString::Printf(TEXT("component_widget_provider_ignored_catalog_type:%s"), *Params.CatalogType));
	return true;
}
}

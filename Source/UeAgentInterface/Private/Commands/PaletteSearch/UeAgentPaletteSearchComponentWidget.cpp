// Copyright Epic Games, Inc. All Rights Reserved.

#include "Commands/PaletteSearch/UeAgentPaletteSearchTypes.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Blueprint/BlueprintSupport.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "ClassViewerFilter.h"
#include "ClassViewerModule.h"
#include "Commands/PaletteSearch/UeAgentPaletteSearchNaming.h"
#include "Components/ActorComponent.h"
#include "Components/Widget.h"
#include "ComponentTypeRegistry.h"
#include "Engine/Blueprint.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Internationalization/Text.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/App.h"
#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
#include "SComponentClassCombo.h"
#include "SubobjectDataSubsystem.h"
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

	struct FActorComponentActionEntry final
	{
		FComponentClassComboEntryPtr Entry;
		FCandidate Candidate;
	};

	static FString ComponentAssetPath(const FComponentClassComboEntryPtr& Entry)
	{
		return Entry.IsValid() && Entry->GetAssetOverride()
			? Entry->GetAssetOverride()->GetPathName()
			: FString();
	}

	static FString ComponentNativeActionSignature(
		const FComponentClassComboEntryPtr& Entry,
		const FString& ComponentClassPath)
	{
		if (!Entry.IsValid()
			|| Entry->GetComponentCreateAction()
				!= EComponentCreateAction::SpawnExistingClass)
		{
			return FString();
		}
		return FString::Printf(
			TEXT("spawn_existing_class|class=%s|asset=%s|name=%s"),
			*ComponentClassPath,
			*ComponentAssetPath(Entry),
			*Entry->GetComponentNameOverride());
	}

	static bool IsComponentClassAllowed(
		const FComponentClassComboEntryPtr& Entry,
		const FClassViewerInitializationOptions& Options,
		const TSharedRef<IClassViewerFilter>& ClassFilter,
		const TSharedRef<FClassViewerFilterFuncs>& FilterFuncs)
	{
		if (const UClass* ComponentClass = Entry->GetComponentClass())
		{
			return ClassFilter->IsClassAllowed(
				Options, ComponentClass, FilterFuncs);
		}
		const TSharedPtr<IUnloadedBlueprintData> UnloadedData =
			Entry->GetUnloadedBlueprintData();
		return !UnloadedData.IsValid()
			|| ClassFilter->IsUnloadedClassAllowed(
				Options, UnloadedData.ToSharedRef(), FilterFuncs);
	}

	static bool TryBuildActorComponentEntries(
		const FSearchParams& Params,
		TArray<FActorComponentActionEntry>& OutEntries,
		FString& OutError)
	{
		OutEntries.Reset();
		TArray<FComponentClassComboEntryPtr>* ComponentClassList = nullptr;
		FComponentTypeRegistry::Get().SubscribeToComponentList(ComponentClassList);
		if (!ComponentClassList)
		{
			OutError = TEXT("component_type_registry_unavailable");
			return false;
		}
		FClassViewerModule* ClassViewer =
			FModuleManager::LoadModulePtr<FClassViewerModule>(TEXT("ClassViewer"));
		if (!ClassViewer)
		{
			OutError = TEXT("component_class_filter_unavailable");
			return false;
		}
		const FClassViewerInitializationOptions FilterOptions;
		const TSharedRef<IClassViewerFilter> ClassFilter =
			ClassViewer->CreateClassFilter(FilterOptions);
		const TSharedRef<FClassViewerFilterFuncs> FilterFuncs =
			ClassViewer->CreateFilterFuncs();

		FString CurrentSection = TEXT("Components");
		int32 UiOrder = 0;
		TSet<FString> SeenActions;
		const bool bHasSearchText = !Params.SearchText.TrimStartAndEnd().IsEmpty();
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
			if ((bHasSearchText && !Entry->IsIncludedInFilter())
				|| !IsComponentClassAllowed(
					Entry, FilterOptions, ClassFilter, FilterFuncs))
			{
				continue;
			}

			const FString FullName = FullNameFromComponentEntry(Entry);
			const FString NativeActionSignature =
				ComponentNativeActionSignature(Entry, FullName);
			FString ActionKey = NativeActionSignature.ToLower();
			if (FullName.IsEmpty() || NativeActionSignature.IsEmpty()
				|| SeenActions.Contains(ActionKey))
			{
				continue;
			}

			UClass* ComponentClass = Entry->GetComponentClass();
			const FString SourceOrigin = SourceOriginFromClass(ComponentClass, FullName);
			if (!IsSourceAllowed(SourceOrigin, Params.SourceFilters))
			{
				continue;
			}

			SeenActions.Add(MoveTemp(ActionKey));
			const FString Name = ActorComponentMenuName(Entry, FullName);
			FCandidate Candidate;
			Candidate.Kind = TEXT("actor_component");
			Candidate.Type = Candidate.Kind;
			Candidate.Name = Name.IsEmpty() ? DisplayNameFromClassPath(FullName) : Name;
			Candidate.DisplayName = Candidate.Name;
			Candidate.FullName = FullName;
			Candidate.CategoryPath = CurrentSection;
			Candidate.Section = CurrentSection;
			Candidate.SourceOrigin = SourceOrigin;
			Candidate.SourceLabel = SourceOrigin;
			Candidate.PackagePath = ComponentClass ? ComponentClass->GetOutermost()->GetName() : FPackageName::ObjectPathToPackageName(FullName);
			const FString AssetPath = ComponentAssetPath(Entry);
			Candidate.ObjectPath = AssetPath.IsEmpty() ? FullName : AssetPath;
			Candidate.AuthoringSelectors.Add(TEXT("component_class"), FullName);
			Candidate.AuthoringSelectors.Add(
				TEXT("native_action_signature"), NativeActionSignature);
			if (!AssetPath.IsEmpty())
			{
				Candidate.AuthoringSelectors.Add(TEXT("asset_override"), AssetPath);
			}
			Candidate.Tooltip = ComponentClass ? ComponentClass->GetToolTipText().ToString() : Candidate.Name;
			Candidate.Keywords = FString::Printf(
				TEXT("%s %s %s %s"),
				*Candidate.Name, *FullName, *AssetPath, *CurrentSection);
			Candidate.WriteSupport = TEXT("blueprint.components[].candidate");
			Candidate.SupportReason = TEXT("Use candidate.name or candidate.id in the Blueprint component-tree authoring surface; the server resolves the native registry entry.");
			Candidate.bInLibrary = true;
			Candidate.bSupportedInContext = true;
			Candidate.UiOrder = UiOrder++;
			FString IdentityError;
			if (!SetCandidateIdentity(
					Params.CatalogType,
					NativeActionSignature,
					ComponentClass
						? ComponentClass->GetName()
						: FPackageName::GetLongPackageAssetName(FullName),
					Candidate,
					IdentityError))
			{
				OutError = FString::Printf(
					TEXT("component_catalog_identity_failed:%s"),
					*IdentityError);
				return false;
			}
			FActorComponentActionEntry ActionEntry;
			ActionEntry.Entry = Entry;
			ActionEntry.Candidate = MoveTemp(Candidate);
			OutEntries.Add(MoveTemp(ActionEntry));
		}
		OutError.Reset();
		return true;
	}

	static bool MatchesExactComponentName(
		const FCandidate& Candidate,
		const FString& Name)
	{
		if (Name.TrimStartAndEnd().IsEmpty())
		{
			return true;
		}
		const auto Matches = [&Name](const FString& Value)
		{
			return !Value.IsEmpty()
				&& Value.Equals(Name, ESearchCase::IgnoreCase);
		};
		if (Matches(Candidate.Name)
			|| Matches(Candidate.DisplayName)
			|| Matches(Candidate.FullName)
			|| Matches(Candidate.SearchKey)
			|| Matches(Candidate.GetStableId())
			|| Matches(Candidate.ObjectPath))
		{
			return true;
		}
		for (const TPair<FString, FString>& Selector
			: Candidate.AuthoringSelectors)
		{
			if (Matches(Selector.Value))
			{
				return true;
			}
		}
		return false;
	}

	static bool TryResolveActorComponentEntry(
		const FString& Name,
		const FString& Id,
		FActorComponentActionEntry& OutEntry,
		FString& OutError)
	{
		OutEntry = {};
		if (Name.TrimStartAndEnd().IsEmpty()
			&& Id.TrimStartAndEnd().IsEmpty())
		{
			OutError = TEXT("blueprint_component_candidate_selector_required");
			return false;
		}
		FSearchParams Params;
		Params.CatalogType = TEXT("actor_component");
		TArray<FActorComponentActionEntry> Entries;
		if (!TryBuildActorComponentEntries(Params, Entries, OutError))
		{
			return false;
		}
		TMap<FString, int32> Matches;
		for (int32 Index = 0; Index < Entries.Num(); ++Index)
		{
			const FCandidate& Candidate = Entries[Index].Candidate;
			const bool bIdMatches = Id.TrimStartAndEnd().IsEmpty()
				|| Candidate.GetPublicId().Equals(Id, ESearchCase::IgnoreCase)
				|| Candidate.GetCandidateId().Equals(
					Id, ESearchCase::CaseSensitive);
			if (bIdMatches && MatchesExactComponentName(Candidate, Name))
			{
				Matches.FindOrAdd(Candidate.GetCandidateId(), Index);
			}
		}
		if (Matches.IsEmpty())
		{
			OutError = TEXT("blueprint_component_candidate_not_found");
			return false;
		}
		if (Matches.Num() != 1)
		{
			OutError = FString::Printf(
				TEXT("blueprint_component_candidate_ambiguous:%d"),
				Matches.Num());
			return false;
		}
		for (const TPair<FString, int32>& Match : Matches)
		{
			OutEntry = Entries[Match.Value];
			break;
		}
		OutError.Reset();
		return true;
	}

	static UClass* ResolveComponentClass(
		const FComponentClassComboEntryPtr& Entry)
	{
		if (!Entry.IsValid())
		{
			return nullptr;
		}
		if (UClass* ComponentClass = Entry->GetComponentClass())
		{
			return ComponentClass;
		}
		const FString ComponentPath = Entry->GetComponentPath();
		UObject* LoadedObject = ComponentPath.IsEmpty()
			? nullptr
			: LoadObject<UObject>(nullptr, *ComponentPath);
		if (UClass* LoadedClass = Cast<UClass>(LoadedObject))
		{
			return LoadedClass->GetAuthoritativeClass();
		}
		if (UBlueprint* LoadedBlueprint = Cast<UBlueprint>(LoadedObject))
		{
			UClass* BlueprintClass = LoadedBlueprint->SkeletonGeneratedClass
				? LoadedBlueprint->SkeletonGeneratedClass
				: LoadedBlueprint->GeneratedClass;
			if (!BlueprintClass)
			{
				BlueprintClass = LoadedBlueprint->ParentClass;
			}
			return BlueprintClass
				? BlueprintClass->GetAuthoritativeClass()
				: nullptr;
		}
		return nullptr;
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
		OutCandidate.CategoryPath = Category.IsEmpty() ? TEXT("Widget") : Category;
		OutCandidate.Section = OutCandidate.CategoryPath;
		OutCandidate.SourceOrigin = SourceOrigin;
		OutCandidate.SourceLabel = SourceOrigin;
		OutCandidate.PackagePath = PackagePath;
		OutCandidate.ObjectPath = ObjectPath.IsEmpty() ? FullName : ObjectPath;
		OutCandidate.AuthoringSelectors.Add(TEXT("widget_class"), FullName);
		OutCandidate.Tooltip = Tooltip;
		OutCandidate.Keywords = FString::Printf(TEXT("%s %s %s"), *Name, *FullName, *OutCandidate.CategoryPath);
		OutCandidate.WriteSupport = TEXT("umg.widget_tree.widgets[].widget_class");
		OutCandidate.SupportReason = TEXT("Use full_name as widget_class in the UMG widget-tree authoring surface.");
		OutCandidate.bInLibrary = true;
		OutCandidate.bSupportedInContext = true;
		OutCandidate.UiOrder = UiOrder;
		FString IdentityError;
		ensureAlwaysMsgf(SetCandidateIdentity(TEXT("umg_widget"), FullName, Name, OutCandidate, IdentityError), TEXT("Widget catalog identity failed: %s"), *IdentityError);
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

bool TryResolveActorComponentCandidate(
	const FString& Name,
	const FString& Id,
	FCandidate& OutCandidate,
	FString& OutError)
{
	FActorComponentActionEntry Entry;
	if (!TryResolveActorComponentEntry(Name, Id, Entry, OutError))
	{
		OutCandidate = {};
		return false;
	}
	OutCandidate = MoveTemp(Entry.Candidate);
	return true;
}

bool TryPerformBlueprintComponentCandidate(
	UBlueprint& Blueprint,
	USCS_Node* Parent,
	const FString& Name,
	const FString& Id,
	const FString& RequestedName,
	USCS_Node*& OutNode,
	FCandidate& OutCandidate,
	FString& OutError)
{
	OutNode = nullptr;
	OutCandidate = {};
	FActorComponentActionEntry Action;
	if (!TryResolveActorComponentEntry(Name, Id, Action, OutError))
	{
		return false;
	}
	UClass* ComponentClass = ResolveComponentClass(Action.Entry);
	if (!ComponentClass
		|| !ComponentClass->IsChildOf(UActorComponent::StaticClass())
		|| ComponentClass->HasAnyClassFlags(
			CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
	{
		OutError = TEXT("blueprint_component_candidate_class_unresolved");
		return false;
	}
	USimpleConstructionScript* SCS = Blueprint.SimpleConstructionScript;
	USubobjectDataSubsystem* SubobjectSystem =
		USubobjectDataSubsystem::Get();
	if (!SCS || !SubobjectSystem)
	{
		OutError = TEXT("blueprint_component_native_owner_unavailable");
		return false;
	}

	TArray<FSubobjectDataHandle> Handles;
	SubobjectSystem->K2_GatherSubobjectDataForBlueprint(&Blueprint, Handles);
	FSubobjectDataHandle RootHandle = FSubobjectDataHandle::InvalidHandle;
	FSubobjectDataHandle ParentHandle = FSubobjectDataHandle::InvalidHandle;
	int32 ParentMatches = 0;
	for (const FSubobjectDataHandle& Handle : Handles)
	{
		const FSubobjectData* Data = Handle.GetData();
		if (!Data)
		{
			continue;
		}
		if (!RootHandle.IsValid() && Data->IsRootActor())
		{
			RootHandle = Handle;
		}
		if (Parent
			&& Data->GetVariableName() == Parent->GetVariableName())
		{
			ParentHandle = Handle;
			++ParentMatches;
		}
	}
	if (!RootHandle.IsValid())
	{
		OutError = TEXT("blueprint_component_root_context_unresolved");
		return false;
	}
	if (Parent && ParentMatches != 1)
	{
		OutError = ParentMatches == 0
			? TEXT("blueprint_component_parent_context_unresolved")
			: TEXT("blueprint_component_parent_context_ambiguous");
		return false;
	}
	if (!Parent)
	{
		ParentHandle = RootHandle;
	}

	FAddNewSubobjectParams AddParams;
	AddParams.ParentHandle = ParentHandle;
	AddParams.NewClass = ComponentClass;
	AddParams.AssetOverride = Action.Entry->GetAssetOverride();
	AddParams.BlueprintContext = &Blueprint;
	FText FailReason;
	const FSubobjectDataHandle NewHandle =
		SubobjectSystem->AddNewSubobject(AddParams, FailReason);
	if (!NewHandle.IsValid())
	{
		OutError = FailReason.IsEmpty()
			? FString(TEXT("blueprint_component_native_create_failed"))
			: FString(TEXT("blueprint_component_native_create_failed:"))
				+ FailReason.ToString();
		return false;
	}
	Action.Entry->GetOnSubobjectCreated().ExecuteIfBound(NewHandle);

	const FString TrimmedRequestedName = RequestedName.TrimStartAndEnd();
	const FSubobjectData* NewData = NewHandle.GetData();
	const FString CurrentName = NewData
		? NewData->GetVariableName().ToString()
		: FString();
	if (TrimmedRequestedName.IsEmpty()
		|| (!CurrentName.Equals(
				TrimmedRequestedName, ESearchCase::CaseSensitive)
			&& !SubobjectSystem->RenameSubobject(
				NewHandle, FText::FromString(TrimmedRequestedName))))
	{
		SubobjectSystem->DeleteSubobject(
			RootHandle, NewHandle, &Blueprint);
		OutError = TEXT("blueprint_component_native_rename_failed");
		return false;
	}

	for (USCS_Node* Node : SCS->GetAllNodes())
	{
		if (Node && Node->GetVariableName().ToString().Equals(
				TrimmedRequestedName, ESearchCase::CaseSensitive))
		{
			OutNode = Node;
			break;
		}
	}
	if (!OutNode)
	{
		SubobjectSystem->DeleteSubobject(
			RootHandle, NewHandle, &Blueprint);
		OutError = TEXT("blueprint_component_native_result_unresolved");
		return false;
	}
	OutCandidate = MoveTemp(Action.Candidate);
	OutError.Reset();
	return true;
}

bool CollectComponentWidgetCandidates(const FSearchParams& Params, TArray<FCandidate>& OutCandidates, TArray<FString>& OutWarnings, FString& OutError)
{
	TSet<FString> SeenFullNames;
	if (Params.CatalogType.Equals(TEXT("actor_component")))
	{
		TArray<FActorComponentActionEntry> Entries;
		if (!TryBuildActorComponentEntries(Params, Entries, OutError))
		{
			return false;
		}
		for (FActorComponentActionEntry& Entry : Entries)
		{
			OutCandidates.Add(MoveTemp(Entry.Candidate));
		}
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

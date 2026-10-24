// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/ContentDiscovery/PaletteSearch/UeAgentPaletteSearchTypes.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Blueprint/BlueprintSupport.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "ClassViewerFilter.h"
#include "ClassViewerModule.h"
#include "Core/ContentDiscovery/PaletteSearch/UeAgentPaletteSearchNaming.h"
#include "Core/ContentDiscovery/UeAgentNameDiscoveryProjection.h"
#include "Core/ContentDiscovery/UeAgentNativeTypeDiscovery.h"
#include "Components/ActorComponent.h"
#include "Components/Widget.h"
#include "ComponentTypeRegistry.h"
#include "Dom/JsonValue.h"
#include "Domains/Blueprint/Authoring/UeAgentBlueprintAuthoringOps.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "EditorClassUtils.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
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
			FString SemanticName = ComponentClass->GetName();
			if (!ComponentClass->HasAnyClassFlags(CLASS_CompiledFromBlueprint))
			{
				SemanticName.RemoveFromEnd(
					TEXT("Component"), ESearchCase::IgnoreCase);
			}
			return ReadableSemanticName(SemanticName);
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
		FDiagnostic& OutError)
	{
		OutEntries.Reset();
		TArray<FComponentClassComboEntryPtr>* ComponentClassList = nullptr;
		FComponentTypeRegistry::Get().SubscribeToComponentList(ComponentClassList);
		if (!ComponentClassList)
		{
			OutError = FDiagnostic::Failure(TEXT("component_type_registry_unavailable"), TEXT("The native component registry did not provide its component class list."), TEXT("candidate_resolve")); OutError.CandidateName = Params.CatalogType; OutError.SourceKind = TEXT("native_result");
			return false;
		}
		FClassViewerModule* ClassViewer =
			FModuleManager::LoadModulePtr<FClassViewerModule>(TEXT("ClassViewer"));
		if (!ClassViewer)
		{
			OutError = FDiagnostic::Failure(TEXT("component_class_filter_unavailable"), TEXT("The ClassViewer module required to filter component classes could not be loaded."), TEXT("candidate_resolve")); OutError.CandidateName = TEXT("ClassViewer"); OutError.SourceKind = TEXT("native_result");
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
			// The registry flag hides duplicate category entries during menu search.
			// We deduplicate native actions below; match the current query in the
			// common palette search instead of hiding the retained category entry.
			if (!IsComponentClassAllowed(Entry, FilterOptions, ClassFilter, FilterFuncs))
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
			Candidate.SupportReason = TEXT("Use candidate.name and, when needed, candidate.select in the Blueprint component-tree authoring surface; the server resolves the native registry entry.");
			Candidate.bInLibrary = true;
			Candidate.bSupportedInContext = true;
			Candidate.UiOrder = UiOrder++;
			FDiagnostic IdentityError;
			if (!SetCandidateIdentity(
					Params.CatalogType,
					NativeActionSignature,
					ComponentClass
						? ComponentClass->GetName()
						: FPackageName::GetLongPackageAssetName(FullName),
					Candidate,
					IdentityError))
			{
				OutError = MoveTemp(IdentityError);
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
		FDiagnostic& OutError, const TMap<FString, FString>& Select)
	{
		OutEntry = {};
		if (Name.TrimStartAndEnd().IsEmpty()
			&& Id.TrimStartAndEnd().IsEmpty())
		{
			OutError = FDiagnostic::Failure(TEXT("blueprint_component_candidate_selector_required"), TEXT("A component candidate requires a non-empty name or identity selector."), TEXT("candidate_resolve")); OutError.PropertyPath = TEXT("candidate"); OutError.ExpectedValuePresent = true; OutError.ActualValuePresent = false;
			return false;
		}
		FSearchParams Params;
		Params.CatalogType = TEXT("actor_component");
		TArray<FActorComponentActionEntry> Entries;
		if (!TryBuildActorComponentEntries(Params, Entries, OutError))
		{
			return false;
		}
		if (!Name.TrimStartAndEnd().IsEmpty())
		{
			TArray<UE::UAI::ContentDiscovery::FNameSelectionEntry> ScopedEntries;
			ScopedEntries.Reserve(Entries.Num());
			for (const FActorComponentActionEntry& Entry : Entries)
			{
				UE::UAI::ContentDiscovery::FNameSelectionEntry Scoped;
				Scoped.Name = Entry.Candidate.Name.IsEmpty()
					? Entry.Candidate.DisplayName
					: Entry.Candidate.Name;
				Scoped.Identity = Entry.Candidate.Identity.CanonicalKey;
				Scoped.Select.Add(TEXT("class"), FPackageName::ObjectPathToObjectName(
					Entry.Candidate.AuthoringSelectors.FindRef(TEXT("component_class"))));
				if (!Entry.Candidate.SourceOrigin.IsEmpty())
				{
					Scoped.Select.Add(TEXT("source"), Entry.Candidate.SourceOrigin);
				}
				const FString Asset = FPackageName::ObjectPathToObjectName(
					Entry.Candidate.AuthoringSelectors.FindRef(TEXT("asset_override")));
				if (!Asset.IsEmpty()) Scoped.Select.Add(TEXT("asset"), Asset);
				for (const auto& Field : {
					TPair<FString, FString>(TEXT("class_source"), TEXT("component_class")),
					TPair<FString, FString>(TEXT("asset_source"), TEXT("asset_override"))})
				{
					const FString Source = UE::UAI::ContentDiscovery::MakeReadableDiscoveryPath(
						Entry.Candidate.AuthoringSelectors.FindRef(Field.Value));
					if (!Source.IsEmpty()) Scoped.Select.Add(Field.Key, Source);
				}
				ScopedEntries.Add(MoveTemp(Scoped));
			}
			int32 ScopedIndex = INDEX_NONE;
			FDiagnostic ScopedError;
			if (!UE::UAI::ContentDiscovery::TryResolveNameSelection(
					ScopedEntries, Name, Select, ScopedIndex, ScopedError,
					UE::UAI::ContentDiscovery::ENameComparison::IgnoreCase))
			{
				OutError = MoveTemp(ScopedError);
				return false;
			}
			if (!Entries.IsValidIndex(ScopedIndex))
			{
				OutError = FDiagnostic::Failure(TEXT("blueprint_component_candidate_resolved_index_invalid"), TEXT("The selected component index is outside the native component catalog."), TEXT("candidate_resolve")); OutError.CandidateName = Name; OutError.SourceKind = TEXT("native_result"); OutError.ExpectedValue = MakeShared<FJsonValueString>(FString::Printf(TEXT("0 <= index < %d"), Entries.Num())); OutError.ActualValue = MakeShared<FJsonValueNumber>(ScopedIndex);
				return false;
			}
			OutEntry = MoveTemp(Entries[ScopedIndex]);
			return true;
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
			OutError = FDiagnostic::Failure(TEXT("blueprint_component_candidate_not_found"), TEXT("No native component candidate matches the supplied name and identity selectors."), TEXT("candidate_resolve")); OutError.CandidateName = Name; OutError.ActualEvidence = MakeShared<FJsonValueString>(Id);
			return false;
		}
		if (Matches.Num() != 1)
		{
			OutError = FDiagnostic::Failure(TEXT("blueprint_component_candidate_ambiguous"), FString::Printf(TEXT("blueprint component candidate ambiguous: %d"), Matches.Num()), TEXT("candidate_resolve"));
			OutError.CandidateName = Name; OutError.ExpectedValue = MakeShared<FJsonValueNumber>(1); OutError.ActualValue = MakeShared<FJsonValueNumber>(Matches.Num());
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

	static FString ClassMenuName(const UClass* Class, const FString& Fallback)
	{
		FString Name = Class ? Class->GetName() : Fallback;
		if (Name.IsEmpty())
		{
			Name = Fallback;
		}
		return ReadableSemanticName(Name);
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
		FDiagnostic IdentityError;
		ensureAlwaysMsgf(SetCandidateIdentity(TEXT("umg_widget"), FullName, Name, OutCandidate, IdentityError), TEXT("Widget catalog identity failed: %s"), *IdentityError.ToPublicDetail());
	}

	static void AddLoadedWidgetCandidates(UWidgetBlueprint* Target, const FSearchParams& Params, TArray<FCandidate>& OutCandidates, TSet<FString>& SeenFullNames, int32& InOutUiOrder)
	{
		for (TObjectIterator<UClass> ClassIt; ClassIt; ++ClassIt)
		{
			UClass* WidgetClass = *ClassIt;
			if (!FWidgetBlueprintEditorUtils::IsUsableWidgetClass(WidgetClass, Target)
				|| WidgetClass->HasAnyClassFlags(CLASS_HideDropDown | CLASS_Hidden)
				|| (Target && WidgetClass == Target->GeneratedClass))
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
				ClassMenuName(WidgetClass, FullName),
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

	static bool AddWidgetBlueprintAssetCandidates(UWidgetBlueprint* Target, const FSearchParams& Params, TArray<FCandidate>& OutCandidates, TSet<FString>& SeenFullNames, int32& InOutUiOrder, FDiagnostic& OutError)
	{
		TArray<FAssetData> Assets, GeneratedAssets;
		if (!UE::UAI::ContentDiscovery::TryCollectTypeAssets(*UBlueprint::StaticClass(), Assets, OutError)
			|| !UE::UAI::ContentDiscovery::TryCollectTypeAssets(*UBlueprintGeneratedClass::StaticClass(), GeneratedAssets, OutError)) return false;
		Assets.Append(MoveTemp(GeneratedAssets));
		for (const FAssetData& Asset : Assets)
		{
			const auto ClassPath = FEditorClassUtils::GetClassPathNameFromAssetTag(Asset);
			const FString FullName = ClassPath.IsValid() ? ClassPath.ToString() : Asset.GetObjectPathString();
			if (FullName.IsEmpty() || SeenFullNames.Contains(FullName)
				|| (Target && (Asset.GetObjectPathString() == Target->GetPathName()
					|| (Target->GeneratedClass && FullName == Target->GeneratedClass->GetPathName())))) continue;
			// Loaded classes have already been evaluated with the same native policy.
			if (FindObject<UClass>(nullptr, *FullName)) continue;
			auto Usable = FWidgetBlueprintEditorUtils::IsUsableWidgetClass(Asset, Target);
			if (Usable.HasError() || (Usable.GetValue().AssetClassFlags & (CLASS_Hidden | CLASS_HideDropDown)) != 0) continue;
			const FString SourceOrigin = SourceOriginFromPath(FullName);
			if (!IsSourceAllowed(SourceOrigin, Params.SourceFilters)) continue;
			SeenFullNames.Add(FullName);
			FString Name = Asset.AssetName.ToString();
			Name.RemoveFromEnd(TEXT("_C"), ESearchCase::CaseSensitive);
			FCandidate Candidate;
			FillWidgetCandidate(FullName, ReadableSemanticName(Name),
				MenuNameFromText(FWidgetBlueprintEditorUtils::GetPaletteCategory(Asset, TSubclassOf<UWidget>(const_cast<UClass*>(Usable.GetValue().NativeParentClass))), TEXT("Widget")),
				SourceOrigin, Asset.PackageName.ToString(), Asset.GetSoftObjectPath().ToString(),
				Asset.GetTagValueRef<FString>(FBlueprintTags::BlueprintDescription), InOutUiOrder++, Candidate);
			OutCandidates.Add(MoveTemp(Candidate));
		}
		return true;
	}
}

bool TryResolveActorComponentCandidate(
	const FString& Name,
	const FString& Id,
	FCandidate& OutCandidate,
	FDiagnostic& OutError, const TMap<FString, FString>& Select)
{
	FActorComponentActionEntry Entry;
	if (!TryResolveActorComponentEntry(Name, Id, Entry, OutError, Select))
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
	FDiagnostic& OutError,
	USceneComponent* InheritedParent, const TMap<FString, FString>& Select)
{
	OutNode = nullptr;
	OutCandidate = {};
	FActorComponentActionEntry Action;
	if (!TryResolveActorComponentEntry(Name, Id, Action, OutError, Select))
	{
		return false;
	}
	UClass* ComponentClass = ResolveComponentClass(Action.Entry);
	if (!ComponentClass
		|| !ComponentClass->IsChildOf(UActorComponent::StaticClass())
		|| ComponentClass->HasAnyClassFlags(
			CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
	{
		OutError = FDiagnostic::Failure(TEXT("blueprint_component_candidate_class_unresolved"), !ComponentClass ? TEXT("The selected component candidate's class could not be loaded.") : !ComponentClass->IsChildOf(UActorComponent::StaticClass()) ? TEXT("The selected class does not derive from ActorComponent.") : ComponentClass->HasAnyClassFlags(CLASS_Abstract) ? TEXT("The selected component class is abstract and cannot be instantiated.") : ComponentClass->HasAnyClassFlags(CLASS_Deprecated) ? TEXT("The selected component class is deprecated and cannot be instantiated.") : TEXT("The selected component class has been superseded by a newer version."), TEXT("candidate_resolve"));
		OutError.CandidateName = Name; OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("instantiable ActorComponent class")); OutError.ActualValue = MakeShared<FJsonValueString>(ComponentClass ? ComponentClass->GetPathName() : Action.Candidate.AuthoringSelectors.FindRef(TEXT("component_class")));
		return false;
	}
	const TFunction<void(const FSubobjectDataHandle&)> OnCreated =
		[&Action](const FSubobjectDataHandle& NewHandle)
		{
			Action.Entry->GetOnSubobjectCreated().ExecuteIfBound(NewHandle);
		};
	if (!UeAgentBlueprintAuthoringOps::TryCreateBlueprintComponentNode(
			Blueprint,
			Parent,
			*ComponentClass,
			RequestedName,
			Action.Entry->GetAssetOverride(),
			OnCreated,
			OutNode,
			OutError,
			InheritedParent))
	{
		return false;
	}
	OutCandidate = MoveTemp(Action.Candidate);
	OutError.Reset();
	return true;
}

bool CollectComponentWidgetCandidates(const FSearchParams& Params, TArray<FCandidate>& OutCandidates, TArray<FString>& OutWarnings, FDiagnostic& OutError)
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
		UWidgetBlueprint* Target = Params.AssetPath.IsEmpty() ? nullptr : FindObject<UWidgetBlueprint>(nullptr, *Params.AssetPath);
		if (!Target && !Params.AssetPath.IsEmpty()) Target = LoadObject<UWidgetBlueprint>(nullptr, *Params.AssetPath);
		if (!Params.AssetPath.IsEmpty() && !Target)
		{
			OutError = FDiagnostic::Failure(TEXT("umg_widget_candidate_target_invalid"), TEXT("The Widget Blueprint context could not be loaded."), TEXT("candidate_discovery"));
			OutError.TargetPath = Params.AssetPath;
			return false;
		}
		if (!UE::UAI::ContentDiscovery::TryEnsureAssetDiscovery(OutError)) return false;
		int32 UiOrder = 0;
		AddLoadedWidgetCandidates(Target, Params, OutCandidates, SeenFullNames, UiOrder);
		if (!AddWidgetBlueprintAssetCandidates(Target, Params, OutCandidates, SeenFullNames, UiOrder, OutError))
		{
			return false;
		}
		return true;
	}

	OutWarnings.Add(FString::Printf(TEXT("component_widget_provider_ignored_catalog_type:%s"), *Params.CatalogType));
	return true;
}
}

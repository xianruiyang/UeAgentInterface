// Copyright Epic Games, Inc. All Rights Reserved.

#include "Commands/PaletteSearch/UeAgentPaletteSearchTypes.h"

#include "ActorFactories/ActorFactory.h"
#include "AssetRegistry/AssetData.h"
#include "Commands/PaletteSearch/UeAgentPaletteSearchNaming.h"
#include "Editor.h"
#include "Elements/Framework/TypedElementRegistry.h"
#include "Elements/Interfaces/TypedElementObjectInterface.h"
#include "Engine/Level.h"
#include "Factories/AssetFactoryInterface.h"
#include "GameFramework/Actor.h"
#include "IPlacementModeModule.h"
#include "Internationalization/Text.h"
#include "Misc/App.h"
#include "Misc/PackageName.h"
#include "Misc/TextFilter.h"
#include "Subsystems/PlacementSubsystem.h"

namespace UeAgentPaletteSearch
{
namespace
{
	struct FNativePlaceableRecord
	{
		FCandidate Candidate;
		TSharedPtr<FPlaceableItem> Item;
		FName Category;
	};

	using FPlaceableTextFilter = TTextFilter<const FPlaceableItem&>;

	FString NormalizeToken(const FString& Value)
	{
		return Value.TrimStartAndEnd().ToLower();
	}

	FString SourceOriginFromPath(const FString& Path)
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
			return ModuleName.Equals(TEXT("Engine"), ESearchCase::IgnoreCase)
				? TEXT("engine")
				: TEXT("plugins");
		}
		return Path.StartsWith(TEXT("/")) ? TEXT("plugins") : TEXT("unknown");
	}

	bool IsSourceAllowed(const FString& SourceOrigin, const TArray<FString>& SourceFilters)
	{
		if (SourceFilters.IsEmpty())
		{
			return true;
		}

		const FString NormalizedOrigin = NormalizeToken(SourceOrigin);
		for (const FString& SourceFilter : SourceFilters)
		{
			const FString NormalizedFilter = NormalizeToken(SourceFilter);
			if (NormalizedFilter.Equals(NormalizedOrigin)
				|| (NormalizedFilter.Equals(TEXT("game")) && NormalizedOrigin.Equals(TEXT("project"))))
			{
				return true;
			}
		}
		return false;
	}

	void GetNativeSearchStrings(const FPlaceableItem& Item, TArray<FString>& OutStrings)
	{
		OutStrings.Add(Item.DisplayName.ToString());
		if (!Item.NativeName.IsEmpty())
		{
			OutStrings.Add(Item.NativeName);
		}
		if (const FString* SourceString = FTextInspector::GetSourceString(Item.DisplayName))
		{
			OutStrings.Add(*SourceString);
		}
	}

	bool CategoryMatches(const FPlacementCategoryInfo& Category, const FString& Selector)
	{
		return Category.UniqueHandle.ToString().Equals(Selector, ESearchCase::IgnoreCase)
			|| Category.DisplayName.ToString().Equals(Selector, ESearchCase::IgnoreCase);
	}

	void SelectCategories(
		IPlacementModeModule& PlacementMode,
		const FSearchParams& Params,
		TArray<FPlacementCategoryInfo>& OutCategories,
		TArray<FString>& OutWarnings)
	{
		TArray<FPlacementCategoryInfo> Categories;
		PlacementMode.GetSortedCategories(Categories);

		// Place Actors switches to AllClasses as soon as text search becomes active.
		if (!Params.SearchText.TrimStartAndEnd().IsEmpty())
		{
			if (const FPlacementCategoryInfo* All = Categories.FindByPredicate([](const FPlacementCategoryInfo& Category)
				{
					return Category.UniqueHandle == FBuiltInPlacementCategories::AllClasses();
				}))
			{
				OutCategories.Add(*All);
			}
			return;
		}

		if (!Params.CategoryFilters.IsEmpty())
		{
			for (const FString& Selector : Params.CategoryFilters)
			{
				if (const FPlacementCategoryInfo* Match = Categories.FindByPredicate([&Selector](const FPlacementCategoryInfo& Category)
					{
						return CategoryMatches(Category, Selector);
					}))
				{
					OutCategories.Add(*Match);
				}
				else
				{
					OutWarnings.Add(TEXT("placement_category_not_found:") + Selector);
				}
			}
			return;
		}

		// An id can originate from an explicit category. Name-only/default discovery
		// uses AllClasses, matching the complete native search surface without duplicates.
		if (!Params.CandidateId.IsEmpty())
		{
			OutCategories = MoveTemp(Categories);
			return;
		}

		if (const FPlacementCategoryInfo* All = Categories.FindByPredicate([](const FPlacementCategoryInfo& Category)
			{
				return Category.UniqueHandle == FBuiltInPlacementCategories::AllClasses();
			}))
		{
			OutCategories.Add(*All);
		}
	}

	bool NativeOrderLess(
		const TSharedPtr<FPlaceableItem>& Left,
		const TSharedPtr<FPlaceableItem>& Right,
		const bool bNameOnly)
	{
		if (bNameOnly)
		{
			return Left->DisplayName.CompareTo(Right->DisplayName) < 0;
		}
		if (Left->SortOrder.IsSet())
		{
			return !Right->SortOrder.IsSet()
				|| Left->SortOrder.GetValue() < Right->SortOrder.GetValue();
		}
		if (Right->SortOrder.IsSet())
		{
			return false;
		}
		return Left->DisplayName.CompareTo(Right->DisplayName) < 0;
	}

	FString BuildNativeSignature(
		const FName Category,
		const FString& FactoryClassPath,
		const FString& AssetPath,
		const FString& ActorClassPath,
		const FString& NativeName,
		const bool bCustomDrag)
	{
		return FString::Printf(
			TEXT("category=%s|factory=%s|asset=%s|actor=%s|native=%s|custom_drag=%s"),
			*Category.ToString(),
			*FactoryClassPath,
			*AssetPath,
			*ActorClassPath,
			*NativeName,
			bCustomDrag ? TEXT("true") : TEXT("false"));
	}

	bool BuildCandidate(
		const FPlacementCategoryInfo& Category,
		const TSharedPtr<FPlaceableItem>& Item,
		const int32 UiOrder,
		FNativePlaceableRecord& OutRecord,
		FString& OutError)
	{
		if (!Item.IsValid())
		{
			OutError = TEXT("placeable_actor_item_invalid");
			return false;
		}

		UObject* FactoryObject = Item->AssetFactory.GetObject();
		if (!FactoryObject)
		{
			FactoryObject = Item->Factory;
		}
		UActorFactory* ActorFactory = Cast<UActorFactory>(FactoryObject);
		if (!ActorFactory)
		{
			ActorFactory = Item->Factory;
		}
		UClass* ActorClass = ActorFactory ? ActorFactory->GetDefaultActorClass(Item->AssetData) : nullptr;

		const FString AssetPath = Item->AssetData.IsValid()
			? Item->AssetData.GetObjectPathString()
			: FString();
		const FString FactoryClassPath = FactoryObject
			? FactoryObject->GetClass()->GetPathName()
			: FString();
		const FString ActorClassPath = ActorClass ? ActorClass->GetPathName() : FString();
		const FString DisplayName = Item->DisplayName.ToString();
		const FString Name = Item->NativeName.IsEmpty()
			? MenuNameFromText(Item->DisplayName, ActorClass ? ActorClass->GetName() : AssetPath)
			: Item->NativeName;
		const FString Signature = BuildNativeSignature(
			Category.UniqueHandle,
			FactoryClassPath,
			AssetPath,
			ActorClassPath,
			Name,
			Item->DragHandler.IsValid());
		const FString SourcePath = !AssetPath.IsEmpty() ? AssetPath : ActorClassPath;
		const FString SourceOrigin = SourceOriginFromPath(SourcePath);

		FCandidate Candidate;
		Candidate.Kind = TEXT("placeable_actor");
		Candidate.Type = Candidate.Kind;
		Candidate.Name = Name;
		Candidate.DisplayName = DisplayName.IsEmpty() ? Name : DisplayName;
		Candidate.FullName = Signature;
		Candidate.CategoryPath = Category.UniqueHandle.ToString();
		Candidate.Section = Category.DisplayName.ToString();
		Candidate.SourceOrigin = SourceOrigin;
		Candidate.SourceLabel = SourceOrigin;
		Candidate.ObjectPath = SourcePath;
		Candidate.PackagePath = SourcePath.IsEmpty()
			? FString()
			: FPackageName::ObjectPathToPackageName(SourcePath);
		Candidate.Tooltip = Candidate.DisplayName;
		Candidate.Keywords = FString::Printf(
			TEXT("%s %s %s %s %s %s"),
			*Name,
			*Candidate.DisplayName,
			*Candidate.CategoryPath,
			*AssetPath,
			*FactoryClassPath,
			*ActorClassPath);
		Candidate.WriteSupport = TEXT("uai.world.edit/1:root.document.provider_extension.actors[].candidate");
		Candidate.bInLibrary = true;
		Candidate.bSupportedInContext = FactoryObject != nullptr
			&& ActorFactory != nullptr
			&& ActorClass != nullptr
			&& !Item->DragHandler.IsValid();
		Candidate.SupportReason = Candidate.bSupportedInContext
			? TEXT("Resolved through the native Place Actors registry and executable through UPlacementSubsystem with the same asset factory.")
			: Item->DragHandler.IsValid()
				? TEXT("The native item uses a custom drag handler and has no headless factory execution path yet.")
				: FactoryObject && !ActorFactory
					? TEXT("The native item uses an asset factory that does not promise one Actor result, so it is outside the world.actor HJSON surface.")
					: TEXT("The native item does not expose an executable asset factory and resolved Actor class.");
		Candidate.UiOrder = UiOrder;
		Candidate.AuthoringSelectors.Add(TEXT("native_action_signature"), Signature);
		Candidate.AuthoringSelectors.Add(TEXT("placement_category"), Category.UniqueHandle.ToString());
		if (!FactoryClassPath.IsEmpty())
		{
			Candidate.AuthoringSelectors.Add(TEXT("actor_factory"), FactoryClassPath);
		}
		if (!AssetPath.IsEmpty())
		{
			Candidate.AuthoringSelectors.Add(TEXT("asset_override"), AssetPath);
		}
		if (!ActorClassPath.IsEmpty())
		{
			Candidate.AuthoringSelectors.Add(TEXT("actor_class"), ActorClassPath);
		}
		if (!SetCandidateIdentity(TEXT("placeable_actor"), Signature, Name, Candidate, OutError))
		{
			return false;
		}

		OutRecord.Candidate = MoveTemp(Candidate);
		OutRecord.Item = Item;
		OutRecord.Category = Category.UniqueHandle;
		return true;
	}

	bool CollectNativeRecords(
		const FSearchParams& Params,
		TArray<FNativePlaceableRecord>& OutRecords,
		TArray<FString>& OutWarnings,
		FString& OutError)
	{
		IPlacementModeModule& PlacementMode = IPlacementModeModule::Get();
		TArray<FPlacementCategoryInfo> Categories;
		SelectCategories(PlacementMode, Params, Categories, OutWarnings);
		if (Categories.IsEmpty())
		{
			OutError = TEXT("placeable_actor_context_has_no_native_category");
			return false;
		}

		FPlaceableTextFilter TextFilter(
			FPlaceableTextFilter::FItemToStringArray::CreateStatic(&GetNativeSearchStrings));
		TextFilter.SetRawFilterText(FText::FromString(Params.SearchText));

		for (const FPlacementCategoryInfo& Category : Categories)
		{
			if (Category.CustomGenerator && Category.CustomDraggableItems.IsEmpty())
			{
				OutWarnings.Add(TEXT("placement_category_custom_generator_not_queryable:") + Category.UniqueHandle.ToString());
				continue;
			}

			PlacementMode.RegenerateItemsForCategory(Category.UniqueHandle);
			TArray<TSharedPtr<FPlaceableItem>> Items;
			if (!Params.SearchText.TrimStartAndEnd().IsEmpty())
			{
				PlacementMode.GetFilteredItemsForCategory(
					Category.UniqueHandle,
					Items,
					[&TextFilter](const TSharedPtr<FPlaceableItem>& Item)
					{
						return Item.IsValid() && TextFilter.PassesFilter(*Item);
					});
			}
			else if (!Category.CustomDraggableItems.IsEmpty())
			{
				for (const TSharedRef<FPlaceableItem>& Item : Category.CustomDraggableItems)
				{
					Items.Add(Item);
				}
			}
			else
			{
				PlacementMode.GetItemsForCategory(Category.UniqueHandle, Items);
			}

			if (Category.bSortable)
			{
				const bool bNameOnly = !Params.SearchText.TrimStartAndEnd().IsEmpty()
					|| Category.UniqueHandle == FBuiltInPlacementCategories::AllClasses();
				Items.Sort([bNameOnly](const TSharedPtr<FPlaceableItem>& Left, const TSharedPtr<FPlaceableItem>& Right)
				{
					return NativeOrderLess(Left, Right, bNameOnly);
				});
			}

			for (int32 Index = 0; Index < Items.Num(); ++Index)
			{
				FNativePlaceableRecord Record;
				if (!BuildCandidate(Category, Items[Index], Index, Record, OutError))
				{
					return false;
				}
				if (IsSourceAllowed(Record.Candidate.SourceOrigin, Params.SourceFilters))
				{
					OutRecords.Add(MoveTemp(Record));
				}
			}
		}

		return true;
	}

	bool ResolveRecord(
		const FString& Name,
		const FString& Id,
		const FString& Category,
		FNativePlaceableRecord& OutRecord,
		FString& OutError)
	{
		const FString CleanName = Name.TrimStartAndEnd();
		const FString CleanId = Id.TrimStartAndEnd();
		if (CleanName.IsEmpty() && CleanId.IsEmpty())
		{
			OutError = TEXT("placeable_actor_candidate_requires_name_or_id");
			return false;
		}

		FSearchParams Params;
		Params.CatalogType = TEXT("placeable_actor");
		Params.SearchMode = TEXT("exact");
		Params.bIncludeDebugFields = true;
		Params.CandidateId = CleanId;
		if (!Category.TrimStartAndEnd().IsEmpty())
		{
			Params.CategoryFilters.Add(Category.TrimStartAndEnd());
		}

		TArray<FNativePlaceableRecord> Records;
		TArray<FString> Warnings;
		if (!CollectNativeRecords(Params, Records, Warnings, OutError))
		{
			return false;
		}

		TArray<int32> Matches;
		for (int32 Index = 0; Index < Records.Num(); ++Index)
		{
			const FCandidate& Candidate = Records[Index].Candidate;
			const bool bIdMatches = CleanId.IsEmpty()
				|| Candidate.GetPublicId().Equals(CleanId, ESearchCase::IgnoreCase)
				|| Candidate.GetCandidateId().Equals(CleanId, ESearchCase::IgnoreCase);
			const bool bNameMatches = CleanName.IsEmpty()
				|| Candidate.Name.Equals(CleanName, ESearchCase::IgnoreCase)
				|| Candidate.DisplayName.Equals(CleanName, ESearchCase::IgnoreCase);
			if (bIdMatches && bNameMatches)
			{
				Matches.Add(Index);
			}
		}

		if (Matches.Num() == 0)
		{
			OutError = TEXT("placeable_actor_candidate_not_found_in_native_context");
			return false;
		}
		if (Matches.Num() != 1)
		{
			OutError = TEXT("placeable_actor_candidate_ambiguous_in_native_context");
			return false;
		}

		OutRecord = MoveTemp(Records[Matches[0]]);
		return true;
	}
}

bool CollectPlaceableActorCandidates(
	const FSearchParams& Params,
	TArray<FCandidate>& OutCandidates,
	TArray<FString>& OutWarnings,
	FString& OutError)
{
	if (!Params.CatalogType.Equals(TEXT("placeable_actor")))
	{
		OutWarnings.Add(TEXT("placeable_actor_provider_ignored_catalog_type:") + Params.CatalogType);
		return true;
	}

	TArray<FNativePlaceableRecord> Records;
	if (!CollectNativeRecords(Params, Records, OutWarnings, OutError))
	{
		return false;
	}
	for (FNativePlaceableRecord& Record : Records)
	{
		OutCandidates.Add(MoveTemp(Record.Candidate));
	}
	return true;
}

bool TryResolvePlaceableActorCandidate(
	const FString& Name,
	const FString& Id,
	const FString& Category,
	FCandidate& OutCandidate,
	FString& OutError)
{
	FNativePlaceableRecord Record;
	if (!ResolveRecord(Name, Id, Category, Record, OutError))
	{
		return false;
	}
	OutCandidate = MoveTemp(Record.Candidate);
	return true;
}

bool TryPerformPlaceableActorCandidate(
	const FString& Name,
	const FString& Id,
	const FString& Category,
	ULevel& Level,
	const FTransform& Transform,
	const FActorSpawnParameters& SpawnParameters,
	AActor*& OutActor,
	FCandidate& OutCandidate,
	FString& OutError)
{
	OutActor = nullptr;
	FNativePlaceableRecord Record;
	if (!ResolveRecord(Name, Id, Category, Record, OutError))
	{
		return false;
	}
	OutCandidate = Record.Candidate;
	if (!Record.Candidate.bSupportedInContext)
	{
		OutError = TEXT("placeable_actor_candidate_has_no_headless_native_factory:")
			+ Record.Candidate.SupportReason;
		return false;
	}

	TScriptInterface<IAssetFactoryInterface> AssetFactory =
		Record.Item->AssetFactory;
	if (!AssetFactory && Record.Item->Factory)
	{
		AssetFactory = Record.Item->Factory;
	}
	if (!AssetFactory)
	{
		OutError = TEXT("placeable_actor_candidate_factory_unavailable");
		return false;
	}

	if (!AssetFactory->CanPlaceElementsFromAssetData(Record.Item->AssetData))
	{
		OutError = TEXT("placeable_actor_candidate_factory_rejected");
		return false;
	}
	if (SpawnParameters.OverrideActorGuid.IsValid())
	{
		OutError = TEXT("placeable_actor_candidate_explicit_actor_guid_unsupported");
		return false;
	}
	if (!GEditor)
	{
		OutError = TEXT("placeable_actor_candidate_editor_unavailable");
		return false;
	}
	UPlacementSubsystem* PlacementSubsystem =
		GEditor->GetEditorSubsystem<UPlacementSubsystem>();
	if (!PlacementSubsystem)
	{
		OutError = TEXT("placeable_actor_candidate_placement_subsystem_unavailable");
		return false;
	}

	FAssetPlacementInfo PlacementInfo;
	PlacementInfo.AssetToPlace = Record.Item->AssetData;
	PlacementInfo.NameOverride = SpawnParameters.Name;
	PlacementInfo.PreferredLevel = &Level;
	PlacementInfo.FinalizedTransform = Transform;
	PlacementInfo.FactoryOverride = AssetFactory;
	FPlacementOptions PlacementOptions;
	PlacementOptions.bPreferBatchPlacement = false;
	PlacementOptions.bIsCreatingPreviewElements = false;
	const TArray<FTypedElementHandle> PlacedHandles =
		PlacementSubsystem->PlaceAsset(PlacementInfo, PlacementOptions);
	TArray<AActor*> PlacedActors;
	for (const FTypedElementHandle& Handle : PlacedHandles)
	{
		const TTypedElement<ITypedElementObjectInterface> ObjectInterface =
			UTypedElementRegistry::GetInstance()->GetElement<
				ITypedElementObjectInterface>(Handle);
		if (ObjectInterface)
		{
			if (AActor* Actor = ObjectInterface.GetObjectAs<AActor>())
			{
				PlacedActors.Add(Actor);
			}
		}
	}
	if (PlacedActors.Num() != 1)
	{
		for (AActor* Actor : PlacedActors)
		{
			Actor->Destroy();
		}
		OutError = FString::Printf(
			TEXT("placeable_actor_candidate_factory_result_count_invalid:%d"),
			PlacedActors.Num());
		return false;
	}
	OutActor = PlacedActors[0];

	const FString ExpectedActorClass = Record.Candidate.AuthoringSelectors.FindRef(TEXT("actor_class"));
	if (!ExpectedActorClass.IsEmpty()
		&& !OutActor->GetClass()->GetPathName().Equals(ExpectedActorClass, ESearchCase::CaseSensitive))
	{
		OutActor->Destroy();
		OutActor = nullptr;
		OutError = TEXT("placeable_actor_candidate_factory_result_class_mismatch");
		return false;
	}
	return true;
}
}

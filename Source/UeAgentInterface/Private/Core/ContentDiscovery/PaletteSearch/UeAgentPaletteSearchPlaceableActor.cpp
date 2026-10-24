// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/ContentDiscovery/PaletteSearch/UeAgentPaletteSearchTypes.h"

#include "ActorFactories/ActorFactory.h"
#include "ActorFactories/ActorFactoryBlueprint.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Core/ContentDiscovery/PaletteSearch/UeAgentPaletteSearchNaming.h"
#include "Core/ContentDiscovery/PaletteSearch/UeAgentBrushBuilderActorFactory.h"
#include "Core/ContentDiscovery/UeAgentNameDiscoveryProjection.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Elements/Framework/TypedElementRegistry.h"
#include "Elements/Interfaces/TypedElementObjectInterface.h"
#include "Engine/Blueprint.h"
#include "Engine/BrushBuilder.h"
#include "Engine/Level.h"
#include "Factories/AssetFactoryInterface.h"
#include "GameFramework/Actor.h"
#include "IPlacementModeModule.h"
#include "Internationalization/Text.h"
#include "Misc/App.h"
#include "Misc/PackageName.h"
#include "Misc/TextFilter.h"
#include "Subsystems/PlacementSubsystem.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringCatalogCandidates.h"

namespace UeAgentPaletteSearch
{
namespace
{
	struct FNativePlaceableRecord
	{
		FCandidate Candidate;
		TSharedPtr<FPlaceableItem> Item;
		TScriptInterface<IAssetFactoryInterface> AssetFactory;
		FName Category;
	};

	using FPlaceableTextFilter = TTextFilter<const FPlaceableItem&>;

	bool IsBlueprintAssetRecord(const FNativePlaceableRecord& Record)
	{
		return Record.Item.IsValid()
			&& Record.Item->AssetData.IsValid()
			&& Record.Item->AssetData.IsInstanceOf(UBlueprint::StaticClass());
	}

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
		const FString& FactoryClassPath,
		const FString& AssetPath,
		const FString& ActorClassPath,
		const FString& NativeName,
		const bool bCustomDrag)
	{
		return FString::Printf(
			TEXT("factory=%s|asset=%s|actor=%s|native=%s|custom_drag=%s"),
			*FactoryClassPath,
			*AssetPath,
			*ActorClassPath,
			*NativeName,
			bCustomDrag ? TEXT("true") : TEXT("false"));
	}

	FString ItemSemanticName(const FPlaceableItem& Item, const UClass* ActorClass = nullptr)
	{
		if (Item.DragHandler.IsValid() && Item.AssetData.IsValid())
		{
			const UClass* BuilderClass = Cast<UClass>(Item.AssetData.GetAsset());
			if (BuilderClass && BuilderClass->IsChildOf(UBrushBuilder::StaticClass()))
			{
				FString BuilderName = BuilderClass->GetName();
				BuilderName.RemoveFromEnd(TEXT("Builder"));
				return ReadableSemanticName(TEXT("BSP_") + BuilderName);
			}
		}
		FString Identifier = Item.NativeName;
		if (Identifier.IsEmpty() && Item.AssetData.IsValid())
		{
			Identifier = FPackageName::GetLongPackageAssetName(Item.AssetData.GetObjectPathString());
		}
		if (Identifier.IsEmpty() && ActorClass)
		{
			Identifier = ActorClass->GetName();
		}
		return ReadableSemanticName(Identifier);
	}

	bool NameCanResolve(const FString& BaseName, const FString& RequestedName)
	{
		// Include every possible base-name group, leaving exact scoped-name
		// resolution and ambiguity handling to the common projection below.
		return RequestedName.Equals(BaseName, ESearchCase::IgnoreCase)
			|| RequestedName.StartsWith(BaseName + TEXT(" ["), ESearchCase::IgnoreCase);
	}

	bool BuildCandidate(
		const FPlacementCategoryInfo& Category,
		const TSharedPtr<FPlaceableItem>& Item,
		const int32 UiOrder,
		FNativePlaceableRecord& OutRecord,
		FDiagnostic& OutError)
	{
		if (!Item.IsValid())
		{
			OutError = FDiagnostic::Failure(TEXT("placeable_actor_item_invalid"), TEXT("The native placement category contains an unavailable item."), TEXT("candidate_resolve")); OutError.CandidateName = Category.DisplayName.ToString(); OutError.PropertyPath = TEXT("item"); OutError.ActualValue = MakeShared<FJsonValueNumber>(UiOrder); OutError.SourceKind = TEXT("native_result");
			return false;
		}

		TScriptInterface<IAssetFactoryInterface> AssetFactory = Item->AssetFactory;
		if (!AssetFactory && Item->Factory)
		{
			AssetFactory = Item->Factory;
		}
		if (!AssetFactory && Item->DragHandler.IsValid() && Item->AssetData.IsValid())
		{
			FText Reason;
			auto* BrushFactory = GetMutableDefault<UUeAgentBrushBuilderActorFactory>();
			if (BrushFactory->CanCreateActorFrom(Item->AssetData, Reason)) AssetFactory = BrushFactory;
		}
		if (!AssetFactory && !Item->DragHandler.IsValid() && GEditor)
		{
			if (UPlacementSubsystem* PlacementSubsystem =
				GEditor->GetEditorSubsystem<UPlacementSubsystem>())
			{
				AssetFactory = PlacementSubsystem->FindAssetFactoryFromAssetData(Item->AssetData);
			}
		}
		UObject* FactoryObject = AssetFactory.GetObject();
		UActorFactory* ActorFactory = Cast<UActorFactory>(FactoryObject);
		UClass* ActorClass = nullptr;
		FString ActorClassPath;
		if (ActorFactory && ActorFactory->GetClass() == UActorFactoryBlueprint::StaticClass()
			&& Item->AssetData.IsValid() && Item->AssetData.IsInstanceOf(UBlueprint::StaticClass()))
		{
			// The native Blueprint factory resolves precisely this registry tag.
			// Listing names must not load every Blueprint class and its dependencies.
			// Keep native factory qualification; PlaceAsset loads the selected asset
			// and the execution path verifies the resulting Actor class below.
			if (AssetFactory->CanPlaceElementsFromAssetData(Item->AssetData))
			{
				ActorClassPath = FPackageName::ExportTextPathToObjectPath(
					Item->AssetData.GetTagValueRef<FString>(FBlueprintTags::GeneratedClassPath));
			}
		}
		else
		{
			AActor* DefaultActor = ActorFactory ? ActorFactory->GetDefaultActor(Item->AssetData) : nullptr;
			ActorClass = DefaultActor ? DefaultActor->GetClass() : nullptr;
			ActorClassPath = ActorClass ? ActorClass->GetPathName() : FString();
		}

		const FString AssetPath = Item->AssetData.IsValid()
			? Item->AssetData.GetObjectPathString()
			: FString();
		const FString FactoryClassPath = FactoryObject
			? FactoryObject->GetClass()->GetPathName()
			: FString();
		const FString DisplayName = Item->DisplayName.ToString();
		const FString Name = ItemSemanticName(*Item, ActorClass);
		const FString Signature = BuildNativeSignature(
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
			&& !ActorClassPath.IsEmpty();
		Candidate.SupportReason = TEXT("Resolved through the native Place Actors registry and executable through UPlacementSubsystem with the same asset factory.");
		if (!Candidate.bSupportedInContext)
		{
			const FString PublicReason = Item->DragHandler.IsValid() && !FactoryObject
				? TEXT("The native custom-drag placement item does not expose a supported creation input and asset factory.")
				: FactoryObject && !ActorFactory
					? TEXT("The native asset factory does not promise one Actor result, as required by the world.actor HJSON surface.")
					: !FactoryObject
						? TEXT("The native placement item has no executable asset factory.")
						: TEXT("The native Actor factory did not resolve an Actor class for this placement item.");
			Candidate.SupportDiagnostic = FDiagnostic::Failure(
				TEXT("placeable_actor_candidate_has_no_headless_native_factory"),
				PublicReason, TEXT("candidate_resolve"), Signature);
			Candidate.SupportDiagnostic.CandidateName = Name;
			Candidate.SupportDiagnostic.SourceKind = TEXT("native_result");
			Candidate.SupportDiagnostic.PropertyPath = TEXT("placement_support");
			auto ActualSupport = MakeShared<FJsonObject>();
			ActualSupport->SetBoolField(TEXT("asset_factory_available"), FactoryObject != nullptr);
			ActualSupport->SetBoolField(TEXT("actor_factory_available"), ActorFactory != nullptr);
			ActualSupport->SetBoolField(TEXT("actor_class_resolved"), !ActorClassPath.IsEmpty());
			ActualSupport->SetBoolField(TEXT("custom_drag_handler"), Item->DragHandler.IsValid());
			Candidate.SupportDiagnostic.ActualValue = MakeShared<FJsonValueObject>(ActualSupport);
			auto ExpectedSupport = MakeShared<FJsonObject>();
			ExpectedSupport->SetBoolField(TEXT("asset_factory_available"), true);
			ExpectedSupport->SetBoolField(TEXT("actor_factory_available"), true);
			ExpectedSupport->SetBoolField(TEXT("actor_class_resolved"), true);
			Candidate.SupportDiagnostic.ExpectedValue = MakeShared<FJsonValueObject>(ExpectedSupport);
			Candidate.SupportReason = Candidate.SupportDiagnostic.PublicMessage;
		}
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
		OutRecord.AssetFactory = AssetFactory;
		OutRecord.Category = Category.UniqueHandle;
		return true;
	}

	bool CollectNativeRecords(
		const FSearchParams& Params,
		TArray<FNativePlaceableRecord>& OutRecords,
		TArray<FString>& OutWarnings,
		FDiagnostic& OutError,
		const FString& RequestedName = FString())
	{
		IPlacementModeModule& PlacementMode = IPlacementModeModule::Get();
		TArray<FPlacementCategoryInfo> Categories;
		SelectCategories(PlacementMode, Params, Categories, OutWarnings);
		if (Categories.IsEmpty())
		{
			OutError = FDiagnostic::Failure(TEXT("placeable_actor_context_has_no_native_category"), TEXT("No native placement category is available for the requested discovery context."), TEXT("candidate_resolve")); OutError.PropertyPath = TEXT("category_filters");
			TArray<TSharedPtr<FJsonValue>> Filters; for (const FString& Filter : Params.CategoryFilters) Filters.Add(MakeShared<FJsonValueString>(Filter)); OutError.ActualValue = MakeShared<FJsonValueArray>(MoveTemp(Filters));
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
			if (!Params.bIncludeNameContext && !Params.SearchText.TrimStartAndEnd().IsEmpty())
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
					Record.Candidate.bMatchesNativeSearch = TextFilter.PassesFilter(*Items[Index]);
					OutRecords.Add(MoveTemp(Record));
				}
			}

			// Place Actors' AllClasses catalog is class-oriented and does not promise
			// to contain the Blueprint assets that the Content Browser can place. Add
			// those assets through the placement subsystem's registered factory
			// authority, then retain the same FPlaceableItem for execute/apply.
			if (Category.UniqueHandle == FBuiltInPlacementCategories::AllClasses()
				&& GEditor)
			{
				UPlacementSubsystem* PlacementSubsystem =
					GEditor->GetEditorSubsystem<UPlacementSubsystem>();
				if (!PlacementSubsystem)
				{
					OutError = FDiagnostic::Failure(TEXT("placeable_actor_candidate_placement_subsystem_unavailable"), TEXT("The editor placement subsystem required to enumerate placeable Blueprint assets is unavailable."), TEXT("candidate_resolve")); OutError.CandidateName = Category.DisplayName.ToString(); OutError.SourceKind = TEXT("native_result");
					return false;
				}

				FARFilter BlueprintFilter;
				BlueprintFilter.ClassPaths.Add(UBlueprint::StaticClass()->GetClassPathName());
				BlueprintFilter.bRecursiveClasses = true;
				TArray<FAssetData> BlueprintAssets;
				FAssetRegistryModule& AssetRegistryModule =
					FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
				AssetRegistryModule.Get().GetAssets(BlueprintFilter, BlueprintAssets);
				BlueprintAssets.Sort([](const FAssetData& Left, const FAssetData& Right)
				{
					return Left.GetObjectPathString() < Right.GetObjectPathString();
				});

				// Native factory qualification may load a Blueprint, its skeleton and
				// preview assets. Names and native text filtering need only registry
				// metadata. Qualify matching names plus their complete same-name groups;
				// unrelated groups cannot affect common scoped-name disambiguation.
				const bool bNarrowNames = !RequestedName.IsEmpty()
					|| !Params.SearchText.TrimStartAndEnd().IsEmpty();
				TSet<FString> RequiredNames;
				if (bNarrowNames)
				{
					for (const FNativePlaceableRecord& Record : OutRecords)
					{
						if (RequestedName.IsEmpty() ? Record.Candidate.bMatchesNativeSearch
							: NameCanResolve(Record.Candidate.Name, RequestedName))
						{
							RequiredNames.Add(NormalizeToken(Record.Candidate.Name));
						}
					}
					for (const FAssetData& BlueprintAsset : BlueprintAssets)
					{
						const FPlaceableItem MetadataItem(TScriptInterface<IAssetFactoryInterface>(), BlueprintAsset);
						const FString Name = ItemSemanticName(MetadataItem);
						if (RequestedName.IsEmpty() ? TextFilter.PassesFilter(MetadataItem)
							: NameCanResolve(Name, RequestedName))
						{
							RequiredNames.Add(NormalizeToken(Name));
						}
					}
				}

				for (const FAssetData& BlueprintAsset : BlueprintAssets)
				{
					const FPlaceableItem MetadataItem(TScriptInterface<IAssetFactoryInterface>(), BlueprintAsset);
					if ((bNarrowNames && !RequiredNames.Contains(NormalizeToken(ItemSemanticName(MetadataItem))))
						|| !IsSourceAllowed(SourceOriginFromPath(BlueprintAsset.GetObjectPathString()), Params.SourceFilters))
					{
						continue;
					}
					TScriptInterface<IAssetFactoryInterface> AssetFactory =
						PlacementSubsystem->FindAssetFactoryFromAssetData(BlueprintAsset);
					UActorFactory* ActorFactory =
						AssetFactory ? Cast<UActorFactory>(AssetFactory.GetObject()) : nullptr;
					if (!ActorFactory)
					{
						continue;
					}

					const TSharedPtr<FPlaceableItem> Item =
						MakeShared<FPlaceableItem>(AssetFactory, BlueprintAsset);
					const bool bMatchesNativeSearch = TextFilter.PassesFilter(*Item);
					if (!Params.bIncludeNameContext && !bMatchesNativeSearch)
					{
						continue;
					}

					FNativePlaceableRecord Record;
					if (!BuildCandidate(Category, Item, Items.Num() + OutRecords.Num(), Record, OutError))
					{
						return false;
					}
					if (!Record.Candidate.bSupportedInContext
						|| !IsSourceAllowed(Record.Candidate.SourceOrigin, Params.SourceFilters))
					{
						continue;
					}

					const FString ActorClass =
						Record.Candidate.AuthoringSelectors.FindRef(TEXT("actor_class"));
					Record.Candidate.bMatchesNativeSearch = bMatchesNativeSearch;
					const FString AssetIdentity =
						Record.Candidate.AuthoringSelectors.FindRef(TEXT("asset_override"));
					OutRecords.RemoveAll([&ActorClass, &AssetIdentity](
						const FNativePlaceableRecord& Existing)
					{
						const FString ExistingAsset =
							Existing.Candidate.AuthoringSelectors.FindRef(TEXT("asset_override"));
						const bool bSameAsset = !AssetIdentity.IsEmpty()
							&& ExistingAsset.Equals(AssetIdentity, ESearchCase::CaseSensitive);
						const bool bClassOnlyDuplicate = !IsBlueprintAssetRecord(Existing)
							&& !ActorClass.IsEmpty()
							&& Existing.Candidate.AuthoringSelectors.FindRef(TEXT("actor_class"))
								.Equals(ActorClass, ESearchCase::CaseSensitive);
						return bSameAsset || bClassOnlyDuplicate;
					});
					OutRecords.Add(MoveTemp(Record));
				}
			}
		}

		return true;
	}

	FString ReadSelectionNamespace(const FCandidate& Candidate, const FString& Selector)
	{
		const FString Path = Candidate.AuthoringSelectors.FindRef(Selector);
		if (Path.IsEmpty()) return FString();
		const FString Package = FPackageName::ObjectPathToPackageName(Path);
		return Selector == TEXT("asset_override")
			? FPackageName::GetLongPackagePath(Package) : Package;
	}

	FString BuildSelectionNamespace(const TArray<FNativePlaceableRecord>& Records,
		const int32 RecordIndex, const FString& Selector)
	{
		const FCandidate& Candidate = Records[RecordIndex].Candidate;
		const FString Namespace = ReadSelectionNamespace(Candidate, Selector);
		TArray<FString> Segments;
		UE::UAI::ContentDiscovery::AppendReadableDiscoveryPathScopes(Namespace, Segments);
		FString Suffix;
		FString Readable;
		for (const FString& Segment : Segments)
		{
			Suffix = TEXT("/") + Segment + Suffix;
			Readable = Readable.IsEmpty() ? Segment : Segment + TEXT(" · ") + Readable;
			bool bCollision = false;
			for (const FNativePlaceableRecord& Other : Records)
			{
				if (!Other.Candidate.Name.Equals(Candidate.Name, ESearchCase::IgnoreCase)) continue;
				const FString OtherNamespace = ReadSelectionNamespace(Other.Candidate, Selector);
				if (!OtherNamespace.Equals(Namespace, ESearchCase::IgnoreCase)
					&& OtherNamespace.EndsWith(Suffix, ESearchCase::IgnoreCase))
				{
					bCollision = true;
					break;
				}
			}
			if (!bCollision) return Readable;
		}
		return Readable;
	}

	bool ResolveRecord(
		const FString& Name,
		const FString& Id,
		const FString& Category,
		FNativePlaceableRecord& OutRecord,
		FDiagnostic& OutError, const TMap<FString, FString>& Select)
	{
		const FString CleanName = Name.TrimStartAndEnd();
		const FString CleanId = Id.TrimStartAndEnd();
		if (CleanName.IsEmpty() && CleanId.IsEmpty())
		{
			OutError = FDiagnostic::Failure(TEXT("placeable_actor_candidate_requires_name_or_id"), TEXT("A placeable Actor candidate requires a non-empty name or identity selector."), TEXT("candidate_resolve")); OutError.PropertyPath = TEXT("candidate"); OutError.ExpectedValuePresent = true; OutError.ActualValuePresent = false;
			return false;
		}

		FSearchParams Params;
		Params.CatalogType = TEXT("placeable_actor");
		Params.SearchMode = TEXT("exact");
		Params.CandidateId = CleanId;
		if (!Category.TrimStartAndEnd().IsEmpty())
		{
			Params.CategoryFilters.Add(Category.TrimStartAndEnd());
		}

		TArray<FNativePlaceableRecord> Records;
		TArray<FString> Warnings;
		if (!CollectNativeRecords(Params, Records, Warnings, OutError, CleanName))
		{
			return false;
		}
		TSet<FString> SeenRecordIdentities;
		for (int32 Index = Records.Num() - 1; Index >= 0; --Index)
		{
			if (SeenRecordIdentities.Contains(Records[Index].Candidate.Identity.CanonicalKey))
			{
				Records.RemoveAt(Index);
				continue;
			}
			SeenRecordIdentities.Add(Records[Index].Candidate.Identity.CanonicalKey);
		}

		if (!CleanName.IsEmpty())
		{
			TArray<UE::UAI::ContentDiscovery::FNameSelectionEntry> Entries;
			TArray<int32> EntryRecordIndices;
			Entries.Reserve(Records.Num());
			EntryRecordIndices.Reserve(Records.Num());
			for (int32 RecordIndex = 0; RecordIndex < Records.Num(); ++RecordIndex)
			{
				const FNativePlaceableRecord& Record = Records[RecordIndex];
				UE::UAI::ContentDiscovery::FNameSelectionEntry Entry;
				Entry.Name = Record.Candidate.Name;
				Entry.Identity = Record.Candidate.Identity.CanonicalKey;
				for (const auto& Field : {
					TPair<FString, FString>(TEXT("class"), TEXT("actor_class")),
					TPair<FString, FString>(TEXT("factory"), TEXT("actor_factory")),
					TPair<FString, FString>(TEXT("asset"), TEXT("asset_override"))})
				{
					const FString Value = FPackageName::ObjectPathToObjectName(Record.Candidate.AuthoringSelectors.FindRef(Field.Value));
					if (!Value.IsEmpty()) Entry.Select.Add(Field.Key, Value);
				}
				if (!Record.Candidate.SourceOrigin.IsEmpty())
					Entry.Select.Add(TEXT("source"), Record.Candidate.SourceOrigin);
				for (const auto& Field : {
					TPair<FString, FString>(TEXT("folder"), TEXT("asset_override")),
					TPair<FString, FString>(TEXT("class_module"), TEXT("actor_class")),
					TPair<FString, FString>(TEXT("factory_module"), TEXT("actor_factory"))})
				{
					const FString Value = BuildSelectionNamespace(Records, RecordIndex, Field.Value);
					if (!Value.IsEmpty()) Entry.Select.Add(Field.Key, Value);
				}
				Entries.Add(MoveTemp(Entry));
				EntryRecordIndices.Add(RecordIndex);
			}
			int32 ScopedIndex = INDEX_NONE;
			FDiagnostic ScopedError;
			if (UE::UAI::ContentDiscovery::TryResolveNameSelection(
					Entries, CleanName, Select, ScopedIndex, ScopedError,
					UE::UAI::ContentDiscovery::ENameComparison::IgnoreCase))
			{
				if (!EntryRecordIndices.IsValidIndex(ScopedIndex)
					|| !Records.IsValidIndex(EntryRecordIndices[ScopedIndex]))
				{
					OutError = FDiagnostic::Failure(TEXT("placeable_actor_candidate_resolved_index_invalid"), !EntryRecordIndices.IsValidIndex(ScopedIndex) ? TEXT("The resolved Actor candidate index is outside the semantic selection catalog.") : TEXT("The semantic Actor candidate maps to an entry outside the native placement catalog."), TEXT("candidate_resolve"));
					OutError.CandidateName = Name; OutError.SourceKind = TEXT("native_result"); OutError.ActualValue = MakeShared<FJsonValueNumber>(EntryRecordIndices.IsValidIndex(ScopedIndex) ? EntryRecordIndices[ScopedIndex] : ScopedIndex);
					OutError.ExpectedValue = MakeShared<FJsonValueString>(FString::Printf(TEXT("0 <= index < %d"), EntryRecordIndices.IsValidIndex(ScopedIndex) ? Records.Num() : EntryRecordIndices.Num()));
					return false;
				}
				OutRecord = MoveTemp(Records[EntryRecordIndices[ScopedIndex]]);
				return true;
			}
			OutError = MoveTemp(ScopedError);
			return false;
		}

		TArray<UeAgentAuthoringIdentity::FCanonicalIdentity> Identities;
		for (const FNativePlaceableRecord& Record : Records)
		{
			Identities.Add(Record.Candidate.Identity);
		}
		UeAgentAuthoringTargetLocator::FResolveResult ResolveResult;
		if (!UeAgentAuthoringCatalogCandidates::ResolveExactCandidate(
				CleanName,
				CleanId,
				Identities,
				ResolveResult))
		{
			OutError = FDiagnostic::WrapFailure(
				ResolveResult.Diagnostic.Code == TEXT("target_not_found")
					? TEXT("placeable_actor_candidate_not_found_in_native_context")
					: ResolveResult.Diagnostic.Code == TEXT("target_ambiguous")
						? TEXT("placeable_actor_candidate_ambiguous_in_native_context")
						: TEXT("placeable_actor_candidate_resolve_failed"),
				ResolveResult.Diagnostic, TEXT("candidate_resolve"));
			OutError.CandidateName = CleanName;
			return false;
		}
		if (!Records.IsValidIndex(ResolveResult.ResolvedIndex))
		{
			OutError = FDiagnostic::Failure(TEXT("placeable_actor_candidate_resolved_index_invalid"), TEXT("The resolved Actor identity index is outside the native placement catalog."), TEXT("candidate_resolve")); OutError.CandidateName = Name; OutError.SourceKind = TEXT("native_result"); OutError.ExpectedValue = MakeShared<FJsonValueString>(FString::Printf(TEXT("0 <= index < %d"), Records.Num())); OutError.ActualValue = MakeShared<FJsonValueNumber>(ResolveResult.ResolvedIndex);
			return false;
		}

		OutRecord = MoveTemp(Records[ResolveResult.ResolvedIndex]);
		return true;
	}
}

bool CollectPlaceableActorCandidates(
	const FSearchParams& Params,
	TArray<FCandidate>& OutCandidates,
	TArray<FString>& OutWarnings,
	FDiagnostic& OutError)
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
	FDiagnostic& OutError, const TMap<FString, FString>& Select)
{
	FNativePlaceableRecord Record;
	if (!ResolveRecord(Name, Id, Category, Record, OutError, Select))
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
	FDiagnostic& OutError, const TMap<FString, FString>& Select)
{
	OutActor = nullptr;
	FNativePlaceableRecord Record;
	if (!ResolveRecord(Name, Id, Category, Record, OutError, Select))
	{
		return false;
	}
	OutCandidate = Record.Candidate;
	if (!Record.Candidate.bSupportedInContext)
	{
		OutError = Record.Candidate.SupportDiagnostic;
		return false;
	}

	TScriptInterface<IAssetFactoryInterface> AssetFactory =
		Record.AssetFactory;
	if (!AssetFactory)
	{
		OutError = FDiagnostic::Failure(TEXT("placeable_actor_candidate_factory_unavailable"), TEXT("The selected placeable Actor candidate has no native asset factory."), TEXT("candidate_resolve")); OutError.CandidateName = Record.Candidate.Name; OutError.SourceKind = TEXT("native_result");
		return false;
	}

	if (!AssetFactory->CanPlaceElementsFromAssetData(Record.Item->AssetData))
	{
		OutError = FDiagnostic::Failure(TEXT("placeable_actor_candidate_factory_rejected"), TEXT("The native factory cannot place elements from the selected candidate's asset data."), TEXT("candidate_resolve")); OutError.CandidateName = Record.Candidate.Name; OutError.TargetPath = Record.Item->AssetData.GetObjectPathString(); OutError.ExpectedValue = MakeShared<FJsonValueString>(AssetFactory.GetObject()->GetClass()->GetName()); OutError.SourceKind = TEXT("native_result");
		return false;
	}
	if (SpawnParameters.OverrideActorGuid.IsValid())
	{
		OutError = FDiagnostic::Failure(TEXT("placeable_actor_candidate_explicit_actor_guid_unsupported"), TEXT("Native placement does not support an explicit Actor GUID override."), TEXT("candidate_resolve")); OutError.CandidateName = Record.Candidate.Name; OutError.PropertyPath = TEXT("actor_guid"); OutError.ExpectedValuePresent = false; OutError.ActualValuePresent = true; OutError.ActualEvidence = MakeShared<FJsonValueString>(SpawnParameters.OverrideActorGuid.ToString());
		return false;
	}
	if (!GEditor)
	{
		OutError = FDiagnostic::Failure(TEXT("placeable_actor_candidate_editor_unavailable"), TEXT("The Unreal Editor required for native Actor placement is unavailable."), TEXT("candidate_resolve")); OutError.CandidateName = Record.Candidate.Name; OutError.SourceKind = TEXT("native_result");
		return false;
	}
	UPlacementSubsystem* PlacementSubsystem =
		GEditor->GetEditorSubsystem<UPlacementSubsystem>();
	if (!PlacementSubsystem)
	{
		OutError = FDiagnostic::Failure(TEXT("placeable_actor_candidate_placement_subsystem_unavailable"), TEXT("The editor placement subsystem required to create this Actor is unavailable."), TEXT("candidate_resolve")); OutError.CandidateName = Record.Candidate.Name; OutError.SourceKind = TEXT("native_result");
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
	if (auto* BrushFactory = Cast<UUeAgentBrushBuilderActorFactory>(AssetFactory.GetObject()))
	{
		BrushFactory->ResetCreationDiagnostic();
	}
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
		if (const auto* BrushFactory = Cast<UUeAgentBrushBuilderActorFactory>(AssetFactory.GetObject()))
		{
			if (!BrushFactory->GetCreationDiagnostic().IsEmpty())
			{
				OutError = BrushFactory->GetCreationDiagnostic();
				OutError.CandidateName = Record.Candidate.Name;
				OutError.SourceKind = TEXT("native_result");
				return false;
			}
		}
		OutError = FDiagnostic::Failure(TEXT("placeable_actor_candidate_factory_result_count_invalid"), FString::Printf(TEXT("The native factory returned %d actors; exactly one was required."), PlacedActors.Num()), TEXT("candidate_apply"));
		OutError.CandidateName = Record.Candidate.Name; OutError.SourceKind = TEXT("native_result"); OutError.ExpectedValue = MakeShared<FJsonValueNumber>(1); OutError.ActualValue = MakeShared<FJsonValueNumber>(PlacedActors.Num());
		return false;
	}
	OutActor = PlacedActors[0];

	const FString ExpectedActorClass = Record.Candidate.AuthoringSelectors.FindRef(TEXT("actor_class"));
	if (!ExpectedActorClass.IsEmpty()
		&& !OutActor->GetClass()->GetPathName().Equals(ExpectedActorClass, ESearchCase::CaseSensitive))
	{
		OutError = FDiagnostic::Failure(TEXT("placeable_actor_candidate_factory_result_class_mismatch"), TEXT("The native factory created an Actor of a different class than the selected candidate."), TEXT("candidate_apply")); OutError.CandidateName = Record.Candidate.Name; OutError.SourceKind = TEXT("native_result");
		OutError.ExpectedValue = MakeShared<FJsonValueString>(ExpectedActorClass); OutError.ActualValue = MakeShared<FJsonValueString>(OutActor->GetClass()->GetPathName());
		OutActor->Destroy();
		OutActor = nullptr;
		return false;
	}
	return true;
}
}

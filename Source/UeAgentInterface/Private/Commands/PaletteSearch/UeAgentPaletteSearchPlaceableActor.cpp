// Copyright Epic Games, Inc. All Rights Reserved.

#include "Commands/PaletteSearch/UeAgentPaletteSearchTypes.h"

#include "ActorFactories/ActorFactory.h"
#include "Commands/PaletteSearch/UeAgentPaletteSearchAuthoring.h"
#include "GameFramework/Actor.h"
#include "Internationalization/Text.h"
#include "Misc/App.h"
#include "Misc/PackageName.h"
#include "UObject/UObjectIterator.h"

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
			return ModuleName.Equals(TEXT("Engine"), ESearchCase::IgnoreCase) ? TEXT("engine") : TEXT("plugins");
		}
		return Path.StartsWith(TEXT("/")) ? TEXT("plugins") : TEXT("unknown");
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
			if (NormalizedFilter.Equals(NormalizedOrigin) || (NormalizedFilter.Equals(TEXT("game")) && NormalizedOrigin.Equals(TEXT("project"))))
			{
				return true;
			}
		}
		return false;
	}

	static FString CategoryFromActorClass(UClass* ActorClass)
	{
		if (!ActorClass)
		{
			return TEXT("Miscellaneous");
		}

		const FString ClassName = ActorClass->GetName();
		const FString ClassGroup = ActorClass->GetMetaData(TEXT("ClassGroup"));

		// ClassGroup is the canonical UE category (e.g. "Lights", "Volumes", "VisualEffects")
		if (!ClassGroup.IsEmpty())
		{
			return ClassGroup;
		}

		// Heuristic fallback based on class name
		if (ClassName.Contains(TEXT("Light")) && !ClassName.Contains(TEXT("Lightmass")))
		{
			return TEXT("Lights");
		}
		if (ClassName.Contains(TEXT("Volume")) || ClassName.Contains(TEXT("BlockingVolume")) || ClassName.Contains(TEXT("KillZVolume")))
		{
			return TEXT("Volumes");
		}
		if (ClassName.Contains(TEXT("Trigger")) || ClassName.Contains(TEXT("TriggerBox")) || ClassName.Contains(TEXT("TriggerSphere")) || ClassName.Contains(TEXT("TriggerCapsule")))
		{
			return TEXT("Volumes");
		}
		if (ClassName.Contains(TEXT("Camera")))
		{
			return TEXT("Cinematic");
		}
		if (ClassName.Contains(TEXT("Fog")) || ClassName.Contains(TEXT("Sky")) || ClassName.Contains(TEXT("Atmosphere")) || ClassName.Contains(TEXT("Cloud")))
		{
			return TEXT("VisualEffects");
		}
		if (ClassName.Contains(TEXT("Audio")) || ClassName.Contains(TEXT("Sound")))
		{
			return TEXT("Audio");
		}

		return TEXT("Basic");
	}

	static FString KeywordsFromActorClass(UClass* ActorClass)
	{
		if (!ActorClass)
		{
			return FString();
		}

		const FString ClassName = ActorClass->GetName();
		FString Keywords = ClassName.ToLower();

		// Append parent class names as search keywords
		for (UClass* SuperClass = ActorClass->GetSuperClass(); SuperClass && SuperClass != AActor::StaticClass(); SuperClass = SuperClass->GetSuperClass())
		{
			Keywords += TEXT(" ") + SuperClass->GetName().ToLower();
		}

		return Keywords;
	}

	static void AddPlaceableActorCandidate(const FSearchParams& Params, UClass* ActorClass, TArray<FCandidate>& OutCandidates)
	{
		if (!ActorClass)
		{
			return;
		}

		const FString ClassPath = ActorClass->GetPathName();
		const FString SourceOrigin = SourceOriginFromPath(ClassPath);
		if (!IsSourceAllowed(SourceOrigin, Params.SourceFilters))
		{
			return;
		}

		const FString Name = ActorClass->GetName();
		FString DisplayName = ActorClass->GetDisplayNameText().ToString();
		if (DisplayName.IsEmpty() || DisplayName == Name)
		{
			// Convert CamelCase class name to readable display name
			DisplayName = FName::NameToDisplayString(Name, false);
		}

		const FString Category = CategoryFromActorClass(ActorClass);

		FCandidate Candidate;
		Candidate.Kind = TEXT("placeable_actor");
		Candidate.Type = Candidate.Kind;
		Candidate.Name = Name;
		Candidate.DisplayName = DisplayName;
		Candidate.FullName = ClassPath;
		Candidate.StableId = ClassPath;
		Candidate.CanonicalKey = Candidate.Kind + TEXT(":") + ClassPath + TEXT(":") + Name;
		Candidate.CandidateId = Candidate.CanonicalKey;
		Candidate.CategoryPath = Category;
		Candidate.Section = Category;
		Candidate.SourceOrigin = SourceOrigin;
		Candidate.SourceLabel = SourceOrigin;
		Candidate.PackagePath = FPackageName::ObjectPathToPackageName(ClassPath);
		Candidate.ObjectPath = ClassPath;
		Candidate.Tooltip = ActorClass->GetToolTipText().ToString();
		if (Candidate.Tooltip.IsEmpty())
		{
			Candidate.Tooltip = DisplayName;
		}
		Candidate.Keywords = FString::Printf(TEXT("%s %s %s %s"), *Name, *DisplayName, *Category, *KeywordsFromActorClass(ActorClass));
		Candidate.WriteSupport = TEXT("level_folder.actors.items[].actor.class_path");
		Candidate.SupportReason = TEXT("Use class_path as the Actor class in Level folder package actor.json.");
		Candidate.bInLibrary = true;
		Candidate.bSupportedInContext = true;
		Candidate.JsonAuthoring = BuildJsonAuthoringPatch(Candidate);
		OutCandidates.Add(MoveTemp(Candidate));
	}

	} // anonymous namespace

bool CollectPlaceableActorCandidates(const FSearchParams& Params, TArray<FCandidate>& OutCandidates, TArray<FString>& OutWarnings, FString& OutError)
{
	if (!Params.CatalogType.Equals(TEXT("placeable_actor")))
	{
		OutWarnings.Add(FString::Printf(TEXT("placeable_actor_provider_ignored_catalog_type:%s"), *Params.CatalogType));
		return true;
	}

	TSet<FString> SeenClassPaths;

	for (TObjectIterator<UClass> ClassIt; ClassIt; ++ClassIt)
	{
		UClass* FactoryClass = *ClassIt;
		if (!FactoryClass->IsChildOf(UActorFactory::StaticClass()) || FactoryClass->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
		{
			continue;
		}

		const UActorFactory* FactoryCDO = FactoryClass->GetDefaultObject<UActorFactory>();
		if (!FactoryCDO || !FactoryCDO->NewActorClass || !FactoryCDO->NewActorClass->IsChildOf(AActor::StaticClass()))
		{
			continue;
		}

		UClass* ActorClass = FactoryCDO->NewActorClass;
		if (ActorClass->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists | CLASS_NotPlaceable))
		{
			continue;
		}

		const FString ClassPath = ActorClass->GetPathName();
		if (SeenClassPaths.Contains(ClassPath))
		{
			continue;
		}
		SeenClassPaths.Add(ClassPath);

		AddPlaceableActorCandidate(Params, ActorClass, OutCandidates);
	}

		// Pass 2: discover actors without dedicated UActorFactory (volumes, brushes, etc.)
		// Accept all non-abstract placeable actors; only skip test/internal/debug.
		for (TObjectIterator<UClass> ClassIt; ClassIt; ++ClassIt)
		{
			UClass* ActorClass = *ClassIt;
			if (!ActorClass->IsChildOf(AActor::StaticClass())
				|| ActorClass->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists | CLASS_NotPlaceable)
				|| ActorClass == AActor::StaticClass())
			{
				continue;
			}

			const FString ClassPath = ActorClass->GetPathName();
			if (SeenClassPaths.Contains(ClassPath))
			{
				continue;
			}

			const FString ClassName = ActorClass->GetName();
			if (ClassName.Contains(TEXT("Test")) || ClassName.Contains(TEXT("Debug"))
				|| ClassName.Contains(TEXT("Preview")) || ClassName.Contains(TEXT("Documentation"))
				|| ClassName.Contains(TEXT("Abstract")) || ClassName.StartsWith(TEXT("AutoRTFM")))
			{
				continue;
			}

			if (!ActorClass->GetDefaultObject())
			{
				continue;
			}

			SeenClassPaths.Add(ClassPath);
			AddPlaceableActorCandidate(Params, ActorClass, OutCandidates);
		}

	OutCandidates.Sort([](const FCandidate& A, const FCandidate& B) {
		if (A.CategoryPath != B.CategoryPath)
		{
			return A.CategoryPath < B.CategoryPath;
		}
		return A.Name < B.Name;
	});

	// Assign UiOrder after sorting for stable ordering
	for (int32 Index = 0; Index < OutCandidates.Num(); ++Index)
	{
		OutCandidates[Index].UiOrder = Index;
	}

	if (OutCandidates.IsEmpty())
	{
		OutError = TEXT("placeable_actor_catalog_empty_no_factories_found");
		return false;
	}
	return true;
}
} // namespace UeAgentPaletteSearch

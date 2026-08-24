#include "Core/AssetDurable/UeAgentPersistedWorld.h"

#include "Engine/Level.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "PackageTools.h"
#include "UObject/LinkerInstancingContext.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectHash.h"

namespace UE::UAI::AssetDurable
{
	FPersistedWorldReferenceMap::FPersistedWorldReferenceMap(
		FString InSourceWorldRef,
		FString InSnapshotWorldRef)
		: SourceWorldRef(MoveTemp(InSourceWorldRef))
		, SnapshotWorldRef(MoveTemp(InSnapshotWorldRef))
	{
	}

	bool FPersistedWorldReferenceMap::IsValid() const
	{
		return !SourceWorldRef.IsEmpty()
			&& SourceWorldRef == SourceWorldRef.TrimStartAndEnd()
			&& !SnapshotWorldRef.IsEmpty()
			&& SnapshotWorldRef == SnapshotWorldRef.TrimStartAndEnd();
	}

	bool FPersistedWorldReferenceMap::TryMapReference(
		const FString& SourceRef,
		FString& OutSnapshotRef) const
	{
		OutSnapshotRef.Reset();
		if (!IsValid()
			|| SourceRef.IsEmpty()
			|| SourceRef != SourceRef.TrimStartAndEnd())
		{
			return false;
		}
		if (SourceRef == SourceWorldRef)
		{
			OutSnapshotRef = SnapshotWorldRef;
			return true;
		}
		const FString WorldObjectPrefix = SourceWorldRef + TEXT(":");
		if (!SourceRef.StartsWith(
				WorldObjectPrefix,
				ESearchCase::CaseSensitive))
		{
			return false;
		}
		OutSnapshotRef = SnapshotWorldRef
			+ SourceRef.RightChop(SourceWorldRef.Len());
		return true;
	}

	namespace
	{
		void SetError(
			FPersistedWorldLoadError& OutError,
			FString Code,
			FString Detail = FString())
		{
			OutError.Code = MoveTemp(Code);
			OutError.Detail = MoveTemp(Detail);
		}

		class FScopedPersistedWorldPackages final
		{
		public:
			~FScopedPersistedWorldPackages()
			{
				if (!bCleanupAttempted)
				{
					FString Ignored;
					TryUnload(Ignored);
				}
			}

			void Add(UPackage* Package)
			{
				if (Package)
				{
					Packages.AddUnique(TWeakObjectPtr<UPackage>(Package));
				}
			}

			bool TryUnload(FString& OutDetail)
			{
				bCleanupAttempted = true;
				OutDetail.Reset();
				TArray<UPackage*> Loaded;
				for (const TWeakObjectPtr<UPackage>& Package : Packages)
				{
					if (UPackage* Value = Package.Get())
					{
						Value->SetDirtyFlag(false);
						Loaded.Add(Value);
					}
				}
				if (Loaded.IsEmpty()) return true;
				FText UnloadError;
				if (!UPackageTools::UnloadPackages(Loaded, UnloadError, true))
				{
					OutDetail = UnloadError.ToString();
					return false;
				}
				Packages.Reset();
				return true;
			}

		private:
			TArray<TWeakObjectPtr<UPackage>> Packages;
			bool bCleanupAttempted = false;
		};
	}

	bool TryObservePersistedWorld(
		const TArray<FDurableArtifact>& Artifacts,
		const FPersistedWorldObserver Observer,
		FPersistedWorldLoadError& OutError)
	{
		OutError = {};
		if (Artifacts.IsEmpty())
		{
			SetError(OutError, TEXT("persisted_world_artifacts_missing"));
			return false;
		}

		const FDurableArtifact* WorldHeader = nullptr;
		TArray<const FDurableArtifact*> PackageHeaders;
		TSet<FString> SeenPackages;
		for (const FDurableArtifact& Artifact : Artifacts)
		{
			if (Artifact.GetArtifactKind() != EDurableArtifactKind::Package
				|| Artifact.GetRole() != TEXT("package_header"))
			{
				continue;
			}
			const FString PackageKey =
				Artifact.GetDurableLocation().ToLower();
			if (SeenPackages.Contains(PackageKey)
				|| !FPackageName::IsValidLongPackageName(
					Artifact.GetDurableLocation())
				|| !IFileManager::Get().FileExists(*Artifact.GetFinalPath()))
			{
				SetError(
					OutError,
					TEXT("persisted_world_artifact_invalid"),
					Artifact.GetDurableLocation());
				return false;
			}
			SeenPackages.Add(PackageKey);
			PackageHeaders.Add(&Artifact);
			if (FPaths::GetExtension(Artifact.GetFinalPath()).Equals(
				TEXT("umap"), ESearchCase::IgnoreCase))
			{
				if (WorldHeader)
				{
					SetError(OutError, TEXT("persisted_world_ambiguous"));
					return false;
				}
				WorldHeader = &Artifact;
			}
		}
		if (!WorldHeader || PackageHeaders.IsEmpty())
		{
			SetError(OutError, TEXT("persisted_world_map_missing"));
			return false;
		}

		const FString SourceWorldPackage = WorldHeader->GetDurableLocation();
		const FString SnapshotToken =
			FGuid::NewGuid().ToString(EGuidFormats::Digits);
		const FString SnapshotWorldPackage =
			TEXT("/Temp/UAI/PersistedWorld/") + SnapshotToken + TEXT("/")
			+ FPackageName::GetShortName(SourceWorldPackage);
		FLinkerInstancingContext InstancingContext;
		InstancingContext.AddTag(ULevel::DontLoadExternalObjectsTag);
		InstancingContext.AddTag(ULevel::DontLoadExternalFoldersTag);
		InstancingContext.AddPackageMapping(
			FName(*SourceWorldPackage), FName(*SnapshotWorldPackage));
		TMap<FString, FString> InstancedPackages;
		for (const FDurableArtifact* Artifact : PackageHeaders)
		{
			if (Artifact == WorldHeader) continue;
			const FString& SourcePackage = Artifact->GetDurableLocation();
			const FString InstancedPackage =
				ULevel::GetExternalActorPackageInstanceName(
					SnapshotWorldPackage, SourcePackage);
			InstancingContext.AddPackageMapping(
				FName(*SourcePackage), FName(*InstancedPackage));
			InstancedPackages.Add(SourcePackage, InstancedPackage);
		}

		FScopedPersistedWorldPackages LoadedPackages;
		UPackage* SnapshotPackage = CreatePackage(*SnapshotWorldPackage);
		if (!SnapshotPackage)
		{
			SetError(
				OutError,
				TEXT("persisted_world_package_create_failed"),
				SnapshotWorldPackage);
			return false;
		}
		SnapshotPackage->SetFlags(RF_Transient);
		LoadedPackages.Add(SnapshotPackage);
		const FName SnapshotPackageName(*SnapshotWorldPackage);
		UWorld::WorldTypePreLoadMap.Add(
			SnapshotPackageName, EWorldType::EditorPreview);
		ON_SCOPE_EXIT
		{
			UWorld::WorldTypePreLoadMap.Remove(SnapshotPackageName);
		};
		SnapshotPackage = LoadPackage(
			SnapshotPackage,
			*SourceWorldPackage,
			LOAD_DisableCompileOnLoad,
			nullptr,
			&InstancingContext);
		UWorld* SnapshotWorld = SnapshotPackage
			? UWorld::FindWorldInPackage(SnapshotPackage) : nullptr;
		if (!SnapshotWorld || !SnapshotWorld->PersistentLevel)
		{
			SetError(
				OutError,
				TEXT("persisted_world_load_failed"),
				SourceWorldPackage);
			return false;
		}
		SnapshotWorld->PersistentLevel->OwningWorld = SnapshotWorld;
		SnapshotWorld->PersistentLevel->bIsVisible = true;
		SnapshotWorld->SetCurrentLevel(SnapshotWorld->PersistentLevel);
		const FPersistedWorldReferenceMap ReferenceMap(
			SourceWorldPackage + TEXT(".")
				+ FPackageName::GetLongPackageAssetName(SourceWorldPackage),
			SnapshotWorld->GetPathName());
		if (!ReferenceMap.IsValid())
		{
			SetError(
				OutError,
				TEXT("persisted_world_reference_map_invalid"),
				SourceWorldPackage);
			return false;
		}

		for (const TPair<FString, FString>& PackagePair : InstancedPackages)
		{
			UPackage* InstancedPackage = CreatePackage(*PackagePair.Value);
			if (!InstancedPackage)
			{
				SetError(
					OutError,
					TEXT("persisted_world_package_create_failed"),
					PackagePair.Value);
				return false;
			}
			InstancedPackage->SetFlags(RF_Transient);
			LoadedPackages.Add(InstancedPackage);
			InstancedPackage = LoadPackage(
				InstancedPackage,
				*PackagePair.Key,
				LOAD_DisableCompileOnLoad,
				nullptr,
				&InstancingContext);
			if (!InstancedPackage)
			{
				SetError(
					OutError,
					TEXT("persisted_world_artifact_load_failed"),
					PackagePair.Key);
				return false;
			}
			ForEachObjectWithPackage(
				InstancedPackage,
				[SnapshotWorld](UObject* Object)
				{
					AActor* Actor = Cast<AActor>(Object);
					if (Actor
						&& !Actor->IsTemplate()
						&& Actor->GetLevel() == SnapshotWorld->PersistentLevel)
					{
						SnapshotWorld->PersistentLevel->Actors.AddUnique(Actor);
					}
					return true;
				});
		}

		FPersistedWorldLoadError ObserverError;
		const bool bObserved = Observer(
			*SnapshotWorld,
			ReferenceMap,
			ObserverError);
		FString CleanupError;
		const bool bCleaned = LoadedPackages.TryUnload(CleanupError);
		if (!bObserved)
		{
			OutError = MoveTemp(ObserverError);
			if (!OutError.IsPresent())
			{
				SetError(OutError, TEXT("persisted_world_observer_failed"));
			}
			if (!bCleaned && !CleanupError.IsEmpty())
			{
				OutError.Detail += TEXT("; cleanup=") + CleanupError;
			}
			return false;
		}
		if (!bCleaned)
		{
			SetError(
				OutError,
				TEXT("persisted_world_cleanup_failed"),
				CleanupError);
			return false;
		}
		return true;
	}
}

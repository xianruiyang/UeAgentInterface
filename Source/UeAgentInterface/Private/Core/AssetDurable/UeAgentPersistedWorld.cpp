#include "Core/AssetDurable/UeAgentPersistedWorld.h"

#include "Engine/Level.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "Dom/JsonValue.h"
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
			FString Reason,
			FString Target = FString(),
			FString Detail = FString())
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(MoveTemp(Code), MoveTemp(Reason), TEXT("persisted_world_readback"), MoveTemp(Detail));
			OutError.TargetPath = MoveTemp(Target); OutError.SourceKind = TEXT("native_result");
		}

		class FScopedPersistedWorldPackages final
		{
		public:
			~FScopedPersistedWorldPackages()
			{
				if (!bCleanupAttempted)
				{
					UE::UAI::Contract::FDiagnostic Ignored;
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

			bool TryUnload(UE::UAI::Contract::FDiagnostic& OutError)
			{
				bCleanupAttempted = true;
				OutError.Reset();
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
					OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("persisted_world_cleanup_failed"), UnloadError.IsEmpty() ? TEXT("Unreal could not unload the isolated persisted world packages after readback.") : UnloadError.ToString(), TEXT("native_package_unload"));
					OutError.SourceKind = TEXT("native_result");
					OutError.ActualValue = MakeShared<FJsonValueNumber>(Loaded.Num());
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
			SetError(OutError, TEXT("persisted_world_artifacts_missing"), TEXT("Persisted world readback requires durable package artifacts."));
			OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("at least one package artifact")); OutError.ActualValue = MakeShared<FJsonValueNumber>(0);
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
			if (Artifact.GetIntent() == EAssetDurableIntent::Delete)
			{
				if (IFileManager::Get().FileExists(*Artifact.GetFinalPath()))
				{
					SetError(OutError, TEXT("persisted_world_deleted_package_present"),
						TEXT("A package removed by the world mutation still exists on disk."), Artifact.GetDurableLocation());
					OutError.ActualValue = MakeShared<FJsonValueBoolean>(true);
					OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(false);
					OutError.InternalContext = Artifact.GetFinalPath();
					return false;
				}
				continue;
			}
			if (SeenPackages.Contains(PackageKey)
				|| !FPackageName::IsValidLongPackageName(
					Artifact.GetDurableLocation())
				|| !IFileManager::Get().FileExists(*Artifact.GetFinalPath()))
			{
				SetError(
					OutError,
					TEXT("persisted_world_artifact_invalid"),
					SeenPackages.Contains(PackageKey) ? TEXT("More than one package-header artifact identifies the same package.")
						: !FPackageName::IsValidLongPackageName(Artifact.GetDurableLocation()) ? TEXT("The package-header artifact has an invalid long package name.")
						: TEXT("The package-header artifact file does not exist."),
					Artifact.GetDurableLocation());
				OutError.ActualValue = MakeShared<FJsonValueString>(Artifact.GetDurableLocation()); OutError.InternalContext = Artifact.GetFinalPath();
				return false;
			}
			SeenPackages.Add(PackageKey);
			PackageHeaders.Add(&Artifact);
			if (FPaths::GetExtension(Artifact.GetFinalPath()).Equals(
				TEXT("umap"), ESearchCase::IgnoreCase))
			{
				if (WorldHeader)
				{
					SetError(OutError, TEXT("persisted_world_ambiguous"), TEXT("Persisted world readback requires exactly one map header, but two map headers were found."), Artifact.GetDurableLocation());
					OutError.ExpectedValue = MakeShared<FJsonValueString>(WorldHeader->GetDurableLocation());
					OutError.ActualValue = MakeShared<FJsonValueString>(Artifact.GetDurableLocation());
					return false;
				}
				WorldHeader = &Artifact;
			}
		}
		if (!WorldHeader || PackageHeaders.IsEmpty())
		{
			SetError(OutError, TEXT("persisted_world_map_missing"), TEXT("The durable artifacts contain no .umap package header for world readback."));
			OutError.ExpectedValue = MakeShared<FJsonValueNumber>(1); OutError.ActualValue = MakeShared<FJsonValueNumber>(0);
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
				TEXT("Unreal could not create the isolated world package used for persisted readback."), SourceWorldPackage,
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
				!SnapshotPackage ? TEXT("Unreal could not load the saved world package into the readback snapshot.")
					: !SnapshotWorld ? TEXT("The loaded readback package contains no world object.") : TEXT("The loaded readback world has no persistent level."),
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
				TEXT("The persisted world source and snapshot references do not form a valid readback reference map."),
				SourceWorldPackage);
			OutError.ActualEvidence = MakeShared<FJsonValueString>(SnapshotWorld->GetPathName());
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
					TEXT("Unreal could not create an isolated external-actor package for world readback."), PackagePair.Key,
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
					TEXT("Unreal could not load an external-actor package into the world readback snapshot."),
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
		UE::UAI::Contract::FDiagnostic CleanupError;
		const bool bCleaned = LoadedPackages.TryUnload(CleanupError);
		if (!bObserved)
		{
			OutError = MoveTemp(ObserverError);
			if (!OutError.IsValid())
			{
				SetError(OutError, TEXT("persisted_world_observer_failed"), TEXT("The persisted-world observer rejected the loaded world and reference map without returning a diagnostic."), SourceWorldPackage);
			}
			if (!bCleaned && !CleanupError.IsEmpty())
			{
				OutError.Append(CleanupError);
			}
			return false;
		}
		if (!bCleaned)
		{
			OutError = MoveTemp(CleanupError);
			return false;
		}
		return true;
	}
}

#include "Core/AssetDurable/UeAgentPersistedPackage.h"

#include "HAL/FileManager.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "PackageTools.h"
#include "UObject/LinkerInstancingContext.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace UE::UAI::AssetDurable
{
	namespace
	{
		void SetError(
			FPersistedPackageLoadError& OutError,
			FString Code,
			FString Detail = FString())
		{
			OutError.Code = MoveTemp(Code);
			OutError.Detail = MoveTemp(Detail);
		}

		class FScopedPersistedPackage final
		{
		public:
			~FScopedPersistedPackage()
			{
				if (!bCleanupAttempted)
				{
					FString Ignored;
					TryUnload(Ignored);
				}
			}

			void Set(UPackage* InPackage)
			{
				Package = InPackage;
			}

			bool TryUnload(FString& OutDetail)
			{
				bCleanupAttempted = true;
				OutDetail.Reset();
				UPackage* Loaded = Package.Get();
				if (!Loaded)
				{
					return true;
				}
				Loaded->SetDirtyFlag(false);
				FText UnloadError;
				if (!UPackageTools::UnloadPackages({Loaded}, UnloadError, true))
				{
					OutDetail = UnloadError.ToString();
					return false;
				}
				Package.Reset();
				return true;
			}

		private:
			TWeakObjectPtr<UPackage> Package;
			bool bCleanupAttempted = false;
		};
	}

	bool TryObservePersistedPackage(
		const TArray<FDurableArtifact>& Artifacts,
		const FString& SourcePackageName,
		const FPersistedPackageObserver Observer,
		FPersistedPackageLoadError& OutError)
	{
		OutError = {};
		if (Artifacts.IsEmpty()
			|| !FPackageName::IsValidLongPackageName(SourcePackageName))
		{
			SetError(
				OutError,
				TEXT("persisted_package_request_invalid"),
				SourcePackageName);
			return false;
		}

		const FDurableArtifact* Header = nullptr;
		for (const FDurableArtifact& Artifact : Artifacts)
		{
			if (Artifact.GetArtifactKind() != EDurableArtifactKind::Package
				|| Artifact.GetRole() != TEXT("package_header")
				|| Artifact.GetDurableLocation() != SourcePackageName)
			{
				continue;
			}
			if (Header)
			{
				SetError(
					OutError,
					TEXT("persisted_package_artifact_ambiguous"),
					SourcePackageName);
				return false;
			}
			Header = &Artifact;
		}
		if (!Header
			|| !FPaths::GetExtension(Header->GetFinalPath()).Equals(
				TEXT("uasset"),
				ESearchCase::IgnoreCase)
			|| !IFileManager::Get().FileExists(*Header->GetFinalPath()))
		{
			SetError(
				OutError,
				TEXT("persisted_package_artifact_invalid"),
				SourcePackageName);
			return false;
		}

		const FString SnapshotToken =
			FGuid::NewGuid().ToString(EGuidFormats::Digits);
		const FString SnapshotPackageName =
			TEXT("/Temp/UAI/PersistedPackage/")
			+ SnapshotToken
			+ TEXT("/")
			+ FPackageName::GetShortName(SourcePackageName);
		FLinkerInstancingContext InstancingContext;
		InstancingContext.AddPackageMapping(
			FName(*SourcePackageName),
			FName(*SnapshotPackageName));

		FScopedPersistedPackage LoadedPackage;
		UPackage* SnapshotPackage = CreatePackage(*SnapshotPackageName);
		if (!SnapshotPackage)
		{
			SetError(
				OutError,
				TEXT("persisted_package_create_failed"),
				SnapshotPackageName);
			return false;
		}
		SnapshotPackage->SetFlags(RF_Transient);
		LoadedPackage.Set(SnapshotPackage);
		SnapshotPackage = LoadPackage(
			SnapshotPackage,
			*SourcePackageName,
			LOAD_DisableCompileOnLoad,
			nullptr,
			&InstancingContext);
		if (!SnapshotPackage
			|| SnapshotPackage->GetName() != SnapshotPackageName)
		{
			SetError(
				OutError,
				TEXT("persisted_package_load_failed"),
				SourcePackageName);
			return false;
		}
		LoadedPackage.Set(SnapshotPackage);

		FPersistedPackageLoadError ObserverError;
		const bool bObserved = Observer(*SnapshotPackage, ObserverError);
		FString CleanupError;
		const bool bCleaned = LoadedPackage.TryUnload(CleanupError);
		if (!bObserved)
		{
			OutError = MoveTemp(ObserverError);
			if (!OutError.IsPresent())
			{
				SetError(OutError, TEXT("persisted_package_observer_failed"));
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
				TEXT("persisted_package_cleanup_failed"),
				CleanupError);
			return false;
		}
		return true;
	}
}

#pragma once

#include "DomainAdapters/AssetDataSimpleEffect/UeAgentAssetDataSimpleEffectCapabilities.h"
#include "DomainAdapters/WorldSimpleEffect/UeAgentWorldSimpleEffectProduction.h"
#include "Core/ContextResource/UeAgentOperationContext.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "PackageTools.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

namespace UE::UAI::Tests::SimpleAuthoring::E4
{
	using namespace DomainAdapters::AssetDataSimpleEffect;
	using namespace DomainAdapters::WorldSimpleEffect;
	using namespace ContextResource;
	using namespace ExecutionMutation;

	class FOwnedReloadableWorldPackage final
	{
	public:
		FOwnedReloadableWorldPackage(
			const FString& InScope,
			const FString& InWorldName)
			: Scope(InScope)
		{
			const FString Unique = FGuid::NewGuid().ToString(
				EGuidFormats::Digits);
			Root = FPaths::ConvertRelativePathToFull(
				FPaths::ProjectSavedDir()
					/ TEXT("UeAgentInterface/SA008")
					/ (Scope + TEXT("_") + Unique));
			PackageRoot = Root / TEXT("Package");
			JournalRoot = Root / TEXT("Journal");
			MountPoint = TEXT("/UaiSa008")
				+ Scope
				+ TEXT("_")
				+ Unique
				+ TEXT("/");
			PackageName = MountPoint + InWorldName;
			ContextRevision = TEXT("sa008:")
				+ Scope
				+ TEXT(":")
				+ Unique;

			IFileManager::Get().DeleteDirectory(*Root, false, true);
			if (!IFileManager::Get().MakeDirectory(
					*PackageRoot,
					true))
			{
				return;
			}
			RegisteredRoot = PackageRoot;
			FPaths::NormalizeDirectoryName(RegisteredRoot);
			RegisteredRoot += TEXT("/");
			FPackageName::RegisterMountPoint(
				MountPoint,
				RegisteredRoot);
			bMounted = true;

			Package.Reset(CreatePackage(*PackageName));
			World.Reset(UWorld::CreateWorld(
				EWorldType::EditorPreview,
				false,
				FName(*InWorldName),
				Package.Get(),
				false));
			if (!World.IsValid() || !World->PersistentLevel)
			{
				return;
			}
			World->SetFlags(RF_Public | RF_Standalone);
			World->PersistentLevel->SetUseExternalActors(false);
			World->SetCurrentLevel(World->PersistentLevel);
			WorldIdentity = World->GetPathName();
			LevelIdentity = World->PersistentLevel->GetPathName();
			InitialWorldHandle = World.Get();
			Package->SetDirtyFlag(false);
		}

		~FOwnedReloadableWorldPackage()
		{
			ReleaseLoadedWorld();
			if (bMounted)
			{
				FPackageName::UnRegisterMountPoint(
					MountPoint,
					RegisteredRoot);
			}
			IFileManager::Get().DeleteDirectory(*Root, false, true);
		}

		bool IsValid() const
		{
			return bMounted
				&& Package.IsValid()
				&& World.IsValid()
				&& World->PersistentLevel
				&& !ContextRevision.IsEmpty();
		}

		bool TryReload(FString& OutError)
		{
			OutError.Reset();
			if (bReloaded)
			{
				return World.IsValid() && World->PersistentLevel;
			}
			ReleaseLoadedWorld();
			UPackage* LoadedPackage = LoadPackage(
				nullptr,
				*PackageName,
				LOAD_None);
			UWorld* LoadedWorld = LoadedPackage
				? UWorld::FindWorldInPackage(LoadedPackage)
				: nullptr;
			if (!LoadedPackage
				|| !LoadedWorld
				|| !LoadedWorld->PersistentLevel)
			{
				OutError = TEXT("sa008_e4_world_reload_failed:")
					+ Scope;
				return false;
			}
			Package.Reset(LoadedPackage);
			World.Reset(LoadedWorld);
			const TWeakObjectPtr<UWorld> ReloadedHandle(LoadedWorld);
			bReloaded = !InitialWorldHandle.IsValid()
				&& !InitialWorldHandle.HasSameIndexAndSerialNumber(
					ReloadedHandle);
			if (!bReloaded)
			{
				OutError = TEXT("sa008_e4_world_read_owner_reused:")
					+ Scope;
			}
			return bReloaded;
		}

		FString HeaderPath() const
		{
			return FPackageName::LongPackageNameToFilename(
				PackageName,
				FPackageName::GetMapPackageExtension());
		}

		bool WasReloaded() const
		{
			return bReloaded;
		}

		bool WasOriginalWorldCollected() const
		{
			return !InitialWorldHandle.IsValid();
		}

		void ReleaseLoadedWorld()
		{
			UPackage* PackageToUnload = Package.Get();
			if (Package.IsValid())
			{
				Package->SetDirtyFlag(false);
			}
			World.Reset();
			Package.Reset();
			if (PackageToUnload)
			{
				FText UnloadError;
				UPackageTools::UnloadPackages(
					{PackageToUnload},
					UnloadError,
					true);
			}
			CollectGarbage(RF_NoFlags, true);
		}

		FString Root;
		FString PackageRoot;
		FString JournalRoot;
		FString PackageName;
		FString WorldIdentity;
		FString LevelIdentity;
		FString ContextRevision;
		TStrongObjectPtr<UPackage> Package;
		TStrongObjectPtr<UWorld> World;

	private:
		FString Scope;
		FString MountPoint;
		FString RegisteredRoot;
		TWeakObjectPtr<UWorld> InitialWorldHandle;
		bool bMounted = false;
		bool bReloaded = false;
	};

	class FOwnedReloadableWorldCatalog final
		: public IUeAgentWorldSimpleEffectWorldCatalog
	{
	public:
		FOwnedReloadableWorldCatalog(
			FOwnedReloadableWorldPackage& InOwner,
			const bool bInReloadBeforeRead)
			: Owner(InOwner)
			, bReloadBeforeRead(bInReloadBeforeRead)
		{
		}

		bool TryEnumerate(
			TArray<FUeAgentWorldSimpleEffectWorldCandidate>&
				OutCandidates,
			FExecutionMutationError& OutError) override
		{
			++EnumerateCount;
			OutCandidates.Reset();
			FString ReloadError;
			if ((bReloadBeforeRead
					&& !Owner.TryReload(ReloadError))
				|| !Owner.World.IsValid()
				|| !Owner.World->PersistentLevel)
			{
				OutError = FExecutionMutationError(
					ReloadError.IsEmpty()
						? TEXT("sa008_e4_world_source_missing")
						: ReloadError);
				return false;
			}
			OutCandidates.Emplace(
				Owner.World.Get(),
				Owner.World->GetPathName(),
				Owner.World->PersistentLevel->GetPathName(),
				EUeAgentExecutionWorldType::Preview,
				TOptional<int32>(),
				Owner.ContextRevision);
			return true;
		}

		int32 EnumerateCount = 0;

	private:
		FOwnedReloadableWorldPackage& Owner;
		bool bReloadBeforeRead = false;
	};

	class FOwnedReloadableAssetPackage final
	{
	public:
		explicit FOwnedReloadableAssetPackage(
			const FString& InScope)
			: Scope(InScope)
		{
			const FString Unique = FGuid::NewGuid().ToString(
				EGuidFormats::Digits);
			Root = FPaths::ConvertRelativePathToFull(
				FPaths::ProjectSavedDir()
					/ TEXT("UeAgentInterface/SA008")
					/ (Scope + TEXT("_") + Unique));
			PackageRoot = Root / TEXT("Package");
			JournalRoot = Root / TEXT("Journal");
			MountPoint = TEXT("/UaiSa008")
				+ Scope
				+ TEXT("_")
				+ Unique
				+ TEXT("/");
			PackageName = MountPoint + TEXT("AssetE4");

			IFileManager::Get().DeleteDirectory(*Root, false, true);
			if (!IFileManager::Get().MakeDirectory(
					*PackageRoot,
					true))
			{
				return;
			}
			RegisteredRoot = PackageRoot;
			FPaths::NormalizeDirectoryName(RegisteredRoot);
			RegisteredRoot += TEXT("/");
			FPackageName::RegisterMountPoint(
				MountPoint,
				RegisteredRoot);
			bMounted = true;
			Package.Reset(CreatePackage(*PackageName));
			if (Package.IsValid())
			{
				Package->SetDirtyFlag(false);
			}
		}

		~FOwnedReloadableAssetPackage()
		{
			ReleaseLoadedPackage();
			if (bMounted)
			{
				FPackageName::UnRegisterMountPoint(
					MountPoint,
					RegisteredRoot);
			}
			IFileManager::Get().DeleteDirectory(*Root, false, true);
		}

		bool IsValid() const
		{
			return bMounted && Package.IsValid();
		}

		void BindTarget(
			FString CanonicalRef,
			const FName ObjectName)
		{
			Targets.Add(MoveTemp(CanonicalRef), ObjectName);
		}

		FName FindTargetName(const FString& CanonicalRef) const
		{
			const FName* Name = Targets.Find(CanonicalRef);
			return Name ? *Name : NAME_None;
		}

		UObject* FindTarget(const FString& CanonicalRef) const
		{
			const FName Name = FindTargetName(CanonicalRef);
			return Package.IsValid() && !Name.IsNone()
				? StaticFindObjectFast(
					UObject::StaticClass(),
					Package.Get(),
					Name)
				: nullptr;
		}

		bool TryReload(
			const FString& CanonicalRef,
			FString& OutError)
		{
			OutError.Reset();
			if (bReloaded)
			{
				return IsValid() && ::IsValid(FindTarget(CanonicalRef));
			}
			UObject* Original = FindTarget(CanonicalRef);
			if (!::IsValid(Original))
			{
				OutError = TEXT("sa008_e4_asset_source_missing:")
					+ CanonicalRef;
				return false;
			}
			InitialObjectHandle = Original;
			ReleaseLoadedPackage();
			UPackage* LoadedPackage = LoadPackage(
				nullptr,
				*PackageName,
				LOAD_None);
			if (!LoadedPackage)
			{
				OutError = TEXT("sa008_e4_asset_reload_failed:")
					+ Scope;
				return false;
			}
			Package.Reset(LoadedPackage);
			UObject* Reloaded = FindTarget(CanonicalRef);
			const TWeakObjectPtr<UObject> ReloadedHandle(Reloaded);
			bReloaded = ::IsValid(Reloaded)
				&& !InitialObjectHandle.IsValid()
				&& !InitialObjectHandle.HasSameIndexAndSerialNumber(
					ReloadedHandle);
			if (!bReloaded)
			{
				OutError = TEXT("sa008_e4_asset_read_owner_reused:")
					+ Scope;
			}
			return bReloaded;
		}

		FString HeaderPath() const
		{
			return FPackageName::LongPackageNameToFilename(
				PackageName,
				FPackageName::GetAssetPackageExtension());
		}

		bool WasReloaded() const
		{
			return bReloaded;
		}

		bool WasOriginalObjectCollected() const
		{
			return !InitialObjectHandle.IsValid();
		}

		void ReleaseLoadedPackage()
		{
			UPackage* PackageToUnload = Package.Get();
			if (Package.IsValid())
			{
				Package->SetDirtyFlag(false);
			}
			Package.Reset();
			if (PackageToUnload)
			{
				FText UnloadError;
				UPackageTools::UnloadPackages(
					{PackageToUnload},
					UnloadError,
					true);
			}
			CollectGarbage(RF_NoFlags, true);
		}

		FString Root;
		FString PackageRoot;
		FString JournalRoot;
		FString PackageName;
		TStrongObjectPtr<UPackage> Package;

	private:
		FString Scope;
		FString MountPoint;
		FString RegisteredRoot;
		TMap<FString, FName> Targets;
		TWeakObjectPtr<UObject> InitialObjectHandle;
		bool bMounted = false;
		bool bReloaded = false;
	};

	class FOwnedReloadableAssetResolver final
		: public IUeAgentAssetDataSimpleEffectAssetResolver
	{
	public:
		FOwnedReloadableAssetResolver(
			FOwnedReloadableAssetPackage& InOwner,
			const bool bInReloadBeforeRead)
			: Owner(InOwner)
			, bReloadBeforeRead(bInReloadBeforeRead)
		{
		}

		bool TryResolveExisting(
			const FString& CanonicalRef,
			UObject*& OutObject,
			FExecutionMutationError& OutError) override
		{
			++ResolveExistingCount;
			FString ReloadError;
			if (bReloadBeforeRead
				&& !Owner.TryReload(CanonicalRef, ReloadError))
			{
				OutObject = nullptr;
				OutError = FExecutionMutationError(
					ReloadError,
					CanonicalRef);
				return false;
			}
			OutObject = Owner.FindTarget(CanonicalRef);
			if (!IsValid(OutObject))
			{
				OutObject = nullptr;
				OutError = FExecutionMutationError(
					TEXT("asset_data_reference_unresolved"),
					CanonicalRef);
				return false;
			}
			OutError = FExecutionMutationError();
			return true;
		}

		bool TryResolveCreationTarget(
			const FString& CanonicalRef,
			UPackage*& OutPackage,
			FName& OutObjectName,
			FExecutionMutationError& OutError) override
		{
			++ResolveCreationCount;
			OutPackage = Owner.Package.Get();
			OutObjectName = Owner.FindTargetName(CanonicalRef);
			if (!OutPackage || OutObjectName.IsNone())
			{
				OutPackage = nullptr;
				OutObjectName = NAME_None;
				OutError = FExecutionMutationError(
					TEXT("asset_data_creation_target_unresolved"),
					CanonicalRef);
				return false;
			}
			OutError = FExecutionMutationError();
			return true;
		}

		int32 ResolveExistingCount = 0;
		int32 ResolveCreationCount = 0;

	private:
		FOwnedReloadableAssetPackage& Owner;
		bool bReloadBeforeRead = false;
	};

	inline int32 CountCompletedJournals(const FString& JournalRoot)
	{
		TArray<FString> Files;
		IFileManager::Get().FindFiles(
			Files,
			*(JournalRoot / TEXT("completed/*.json")),
			true,
			false);
		return Files.Num();
	}

	inline int32 CountActiveTransactions(const FString& JournalRoot)
	{
		TArray<FString> Directories;
		IFileManager::Get().FindFiles(
			Directories,
			*(JournalRoot / TEXT("active/*")),
			false,
			true);
		return Directories.Num();
	}
}

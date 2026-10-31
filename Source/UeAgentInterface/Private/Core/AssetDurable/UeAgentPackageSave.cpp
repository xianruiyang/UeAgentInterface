#include "Core/AssetDurable/UeAgentPackageSave.h"

#include "CoreGlobals.h"
#include "Engine/Level.h"
#include "Engine/LevelStreaming.h"
#include "Engine/World.h"
#include "LevelUtils.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "HAL/FileManager.h"
#include "Logging/LogMacros.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "UObject/LinkerLoad.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectGlobals.h"

namespace UE::UAI::AssetDurable
{
	void RefreshPublishedPackageFiles(const TArray<FString>& Filenames)
	{
		TArray<FString> PackageFiles;
		for (const FString& Filename : Filenames)
		{
			const FString Extension = FPaths::GetExtension(Filename, true);
			FString PackageName;
			if ((Extension == FPackageName::GetAssetPackageExtension()
				|| Extension == FPackageName::GetMapPackageExtension())
				&& FPackageName::TryConvertFilenameToLongPackageName(Filename, PackageName))
				PackageFiles.AddUnique(FPaths::ConvertRelativePathToFull(Filename));
		}
		// ScanModifiedAssetFiles removes old registry rows when a file vanished,
		// and discovers new package files without waiting for a directory watcher.
		if (!PackageFiles.IsEmpty()) FAssetRegistryModule::GetRegistry().ScanModifiedAssetFiles(PackageFiles);
	}

	void ReleaseLoadedPackageFileForReplacement(const FString& Filename)
	{
		const FString Extension = FPaths::GetExtension(Filename, true);
		if (Extension != FPackageName::GetAssetPackageExtension()
			&& Extension != FPackageName::GetMapPackageExtension()
			&& Extension != TEXT(".upayload")) return;
		FString PackageName;
		if (FPackageName::TryConvertFilenameToLongPackageName(Filename, PackageName))
			if (UPackage* Package = FindPackage(nullptr, *PackageName)) ResetLoaders(Package);
	}

	bool TrySaveLoadedPackage(
		UPackage& Package,
		UObject* TopLevelAsset,
		const FString& Filename,
		const FSavePackageArgs& Args,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutError.Reset();
		Package.FullyLoad();
		FSavePackageArgs SaveArgs = Args;
		SaveArgs.SaveFlags &= ~SAVE_NoError;
		FStringOutputDevice Diagnostics;
		SaveArgs.Error = &Diagnostics;
		// Scratch publication must preserve dirty state and avoid Editor save UI/GC.
		// Match the native map-save coordinate conversion without the surrounding
		// Editor lifecycle: loaded streaming actors use world coordinates in memory,
		// while the sub-map stores coordinates relative to its streaming transform.
		UWorld* World = Cast<UWorld>(TopLevelAsset);
		ULevelStreaming* Streaming = World && World->PersistentLevel
			? FLevelUtils::FindStreamingLevel(World->PersistentLevel) : nullptr;
		const bool bRemoveTransform = Streaming && World->PersistentLevel->bAlreadyMovedActors;
		if (bRemoveTransform) FLevelUtils::RemoveEditorTransform(Streaming);
		ON_SCOPE_EXIT { if (bRemoveTransform) FLevelUtils::ApplyEditorTransform(Streaming); };
		const FSavePackageResultStruct Result =
			UPackage::Save(&Package, TopLevelAsset, *Filename, SaveArgs);
		if (Result.Result == ESavePackageResult::Success)
		{
			// Current consumers use synchronous native disk writers. Scratch staging
			// deliberately preserves memory dirty through SAVE_KeepDirty.
			if (!IFileManager::Get().FileExists(*Filename))
			{
				OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("package_output_not_persisted"),
					TEXT("Unreal reported a successful save, but the package file was not created."), TEXT("persist"), Filename);
				OutError.TargetPath = Package.GetName();
				return false;
			}
			if (!(SaveArgs.SaveFlags & SAVE_KeepDirty) && Package.IsDirty())
			{
				OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("saved_package_remains_dirty"),
					TEXT("The package still has unsaved changes after the save operation."), TEXT("persist"));
				OutError.TargetPath = Package.GetName();
				return false;
			}
			return true;
		}
		Diagnostics.TrimStartAndEndInline();
		OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("save_asset_failed"),
			Diagnostics.IsEmpty() ? TEXT("Unreal Editor could not save the package.") : FString(Diagnostics),
			TEXT("persist"), FString::Printf(TEXT("filename=%s;save_result=%d"),
				*Filename, static_cast<int32>(Result.Result)));
		OutError.TargetPath = Package.GetName();
		UE_LOG(LogTemp, Warning, TEXT("Package save failed: %s; %s"), *OutError.PublicMessage, *OutError.InternalContext);
		return false;
	}
}

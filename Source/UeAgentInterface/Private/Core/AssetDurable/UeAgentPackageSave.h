#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "Containers/UnrealString.h"
#include "Containers/Array.h"

class UObject;
class UPackage;
struct FSavePackageArgs;

namespace UE::UAI::AssetDurable
{
	// Complete partial packages before UE validates them, and retain native save
	// diagnostics without sending errors to the interactive editor error device.
	// Confirm synchronous disk output and the caller's SAVE_KeepDirty policy.
	// Publication, rollback and file moves remain with callers.
	UEAGENTINTERFACE_API bool TrySaveLoadedPackage(
		UPackage& Package,
		UObject* TopLevelAsset,
		const FString& Filename,
		const FSavePackageArgs& Args,
		UE::UAI::Contract::FDiagnostic& OutError);

	// Stage/save may load the package again. Detach its reader at the actual
	// filesystem replacement boundary, including recovery of package artifacts.
	UEAGENTINTERFACE_API void ReleaseLoadedPackageFileForReplacement(const FString& Filename);
	// Synchronously publish the current disk state (including absence) to native
	// discovery before loading packages. Does not save or load asset objects.
	UEAGENTINTERFACE_API void RefreshPublishedPackageFiles(const TArray<FString>& Filenames);
	// Rescan an incomplete filesystem operation without announcing a confirmed save.
	void RescanPackageFiles(const TArray<FString>& Filenames);
}

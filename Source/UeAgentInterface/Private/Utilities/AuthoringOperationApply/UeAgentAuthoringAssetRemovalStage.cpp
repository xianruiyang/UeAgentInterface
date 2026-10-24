// Copyright Epic Games, Inc. All Rights Reserved.

#include "Utilities/AuthoringOperationApply/UeAgentAuthoringAssetRemovalStage.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "UObject/UObjectGlobals.h"

namespace UeAgentAuthoringOperationApply
{
	bool FAssetRemovalStage::Capture(UObject* InAsset, FDiagnostic& OutError)
	{
		OutError.Reset();
		Reset();
		if (!IsValid(InAsset) || !InAsset->GetOuter())
		{
			OutError = FDiagnostic::Failure(TEXT("asset_removal_capture_target_invalid"),
				!InAsset ? TEXT("No asset was supplied for removal.")
				: !IsValid(InAsset) ? TEXT("The removal asset is no longer a valid native object.")
				: TEXT("The removal asset has no owning outer object."), TEXT("removal_capture"));
			if (IsValid(InAsset)) OutError.TargetPath = InAsset->GetPathName();
			OutError.ActualValuePresent = InAsset != nullptr;
			return false;
		}
		Asset = InAsset;
		OriginalOuter = InAsset->GetOuter();
		OriginalName = InAsset->GetFName();
		OriginalObjectPath = InAsset->GetPathName();
		StagedName = MakeUniqueObjectName(
			OriginalOuter,
			InAsset->GetClass(),
			FName(*FString::Printf(TEXT("__UAI_Removed_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits))));
		return true;
	}

	bool FAssetRemovalStage::Stage(FDiagnostic& OutError)
	{
		OutError.Reset();
		if (!IsCaptured() || bStaged)
		{
			OutError = FDiagnostic::Failure(TEXT("asset_removal_stage_state_invalid"),
				!IsCaptured() ? TEXT("No asset has been captured for staged removal.")
				: TEXT("The captured asset has already been staged for removal."), TEXT("removal_stage"));
			OutError.TargetPath = OriginalObjectPath;
			return false;
		}
		Asset->Modify();
		if (!Asset->Rename(
			*StagedName.ToString(),
			OriginalOuter,
			REN_DontCreateRedirectors | REN_NonTransactional))
		{
			OutError = FDiagnostic::Failure(TEXT("asset_removal_stage_rename_failed"),
				TEXT("Native rename could not move the asset to its temporary removal name."), TEXT("removal_stage"));
			OutError.TargetPath = OriginalObjectPath;
			OutError.InternalContext = StagedName.ToString();
			return false;
		}
		FAssetRegistryModule::AssetRenamed(Asset, OriginalObjectPath);
		Asset->MarkPackageDirty();
		bStaged = true;
		return true;
	}

	bool FAssetRemovalStage::Rollback(FDiagnostic& OutError)
	{
		OutError.Reset();
		if (!IsCaptured() || !bStaged) return true;
		const FString StagedObjectPath = Asset->GetPathName();
		if (!Asset->Rename(
			*OriginalName.ToString(),
			OriginalOuter,
			REN_DontCreateRedirectors | REN_NonTransactional))
		{
			OutError = FDiagnostic::Failure(TEXT("asset_removal_rollback_rename_failed"),
				TEXT("Native rename could not restore the removed asset to its original name and outer."), TEXT("removal_rollback"));
			OutError.TargetPath = OriginalObjectPath;
			OutError.InternalContext = StagedObjectPath;
			OutError.RollbackAttempted = true;
			OutError.RollbackSucceeded = false;
			return false;
		}
		FAssetRegistryModule::AssetRenamed(Asset, StagedObjectPath);
		Asset->MarkPackageDirty();
		bStaged = false;
		return true;
	}

	void FAssetRemovalStage::Reset()
	{
		Asset = nullptr;
		OriginalOuter = nullptr;
		OriginalName = NAME_None;
		OriginalObjectPath.Reset();
		StagedName = NAME_None;
		bStaged = false;
	}
}

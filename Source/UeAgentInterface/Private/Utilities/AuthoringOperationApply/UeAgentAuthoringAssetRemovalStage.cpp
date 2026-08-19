// Copyright Epic Games, Inc. All Rights Reserved.

#include "Utilities/AuthoringOperationApply/UeAgentAuthoringAssetRemovalStage.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "UObject/UObjectGlobals.h"

namespace UeAgentAuthoringOperationApply
{
	bool FAssetRemovalStage::Capture(UObject* InAsset, FString& OutError)
	{
		OutError.Reset();
		Reset();
		if (!IsValid(InAsset) || !InAsset->GetOuter())
		{
			OutError = TEXT("asset_removal_capture_target_invalid");
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

	bool FAssetRemovalStage::Stage(FString& OutError)
	{
		OutError.Reset();
		if (!IsCaptured() || bStaged)
		{
			OutError = TEXT("asset_removal_stage_state_invalid");
			return false;
		}
		Asset->Modify();
		if (!Asset->Rename(
			*StagedName.ToString(),
			OriginalOuter,
			REN_DontCreateRedirectors | REN_NonTransactional))
		{
			OutError = TEXT("asset_removal_stage_rename_failed");
			return false;
		}
		FAssetRegistryModule::AssetRenamed(Asset, OriginalObjectPath);
		Asset->MarkPackageDirty();
		bStaged = true;
		return true;
	}

	bool FAssetRemovalStage::Rollback(FString& OutError)
	{
		OutError.Reset();
		if (!IsCaptured() || !bStaged) return true;
		const FString StagedObjectPath = Asset->GetPathName();
		if (!Asset->Rename(
			*OriginalName.ToString(),
			OriginalOuter,
			REN_DontCreateRedirectors | REN_NonTransactional))
		{
			OutError = TEXT("asset_removal_rollback_rename_failed");
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

// Copyright Epic Games, Inc. All Rights Reserved.

#include "UeAgentHttpServer_Assets.h"
#include "UeAgentHttpServer_AssetsOps.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "CoreGlobals.h"
#include "HAL/FileManager.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "ObjectTools.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"

bool FUeAgentLegacyMechanics::CmdAssetDelete(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentAssetOps;
	using UE::AssetRegistry::EDependencyCategory;

	TArray<FString> RequestedAssetPaths;
	auto AddStringField = [&Ctx, &RequestedAssetPaths](const TCHAR* Key)
	{
		FString Value;
		if (JsonTryGetString(Ctx.Params, Key, Value))
		{
			Value.TrimStartAndEndInline();
			if (!Value.IsEmpty())
			{
				RequestedAssetPaths.Add(Value);
			}
		}
	};
	AddStringField(TEXT("asset_path"));
	AddStringField(TEXT("package_name"));
	AddStringField(TEXT("object_path"));

	TArray<FString> ArrayAssetPaths;
	if (ReadStringArrayParam(Ctx.Params, TEXT("asset_paths"), ArrayAssetPaths))
	{
		RequestedAssetPaths.Append(ArrayAssetPaths);
	}

	TArray<FString> RequestedFolderPaths;
	auto AddFolderField = [&Ctx, &RequestedFolderPaths](const TCHAR* Key)
	{
		FString Value;
		if (JsonTryGetString(Ctx.Params, Key, Value))
		{
			Value.TrimStartAndEndInline();
			if (!Value.IsEmpty())
			{
				Value = NormalizeAssetPath(Value);
				Value.RemoveFromEnd(TEXT("/"));
				RequestedFolderPaths.Add(Value);
			}
		}
	};
	AddFolderField(TEXT("folder_path"));
	AddFolderField(TEXT("folder"));

	TArray<FString> ArrayFolderPaths;
	if (ReadStringArrayParam(Ctx.Params, TEXT("folder_paths"), ArrayFolderPaths))
	{
		for (FString FolderPath : ArrayFolderPaths)
		{
			FolderPath.TrimStartAndEndInline();
			if (!FolderPath.IsEmpty())
			{
				FolderPath = NormalizeAssetPath(FolderPath);
				FolderPath.RemoveFromEnd(TEXT("/"));
				RequestedFolderPaths.Add(FolderPath);
			}
		}
	}

	if (RequestedAssetPaths.IsEmpty() && RequestedFolderPaths.IsEmpty())
	{
		OutError = TEXT("missing_asset_path_or_folder_path");
		return false;
	}

	bool bDryRun = true;
	JsonTryGetBool(Ctx.Params, TEXT("dry_run"), bDryRun);
	bool bRecursive = false;
	JsonTryGetBool(Ctx.Params, TEXT("recursive"), bRecursive);
	bool bFailOnBlocked = true;
	JsonTryGetBool(Ctx.Params, TEXT("fail_on_blocked"), bFailOnBlocked);
	bool bForceScan = true;
	JsonTryGetBool(Ctx.Params, TEXT("force_scan"), bForceScan);
	bool bAllowNonGame = false;
	JsonTryGetBool(Ctx.Params, TEXT("allow_non_game"), bAllowNonGame);
	bool bAllowDirty = false;
	JsonTryGetBool(Ctx.Params, TEXT("allow_dirty"), bAllowDirty);
	bool bCloseAssetEditors = false;
	JsonTryGetBool(Ctx.Params, TEXT("close_asset_editors"), bCloseAssetEditors);
	bool bAllowOpenEditors = false;
	JsonTryGetBool(Ctx.Params, TEXT("allow_open_editors"), bAllowOpenEditors);
	bool bIncludePathStringHits = true;
	JsonTryGetBool(Ctx.Params, TEXT("include_path_string_hits"), bIncludePathStringHits);
	bool bBlockOnPathStringHits = true;
	JsonTryGetBool(Ctx.Params, TEXT("block_on_path_string_hits"), bBlockOnPathStringHits);
	bool bDeleteEmptyFolders = true;
	JsonTryGetBool(Ctx.Params, TEXT("delete_empty_folders"), bDeleteEmptyFolders);
	JsonTryGetBool(Ctx.Params, TEXT("delete_folders"), bDeleteEmptyFolders);

	double MaxPathStringHitsNumber = 1000.0;
	JsonTryGetNumber(Ctx.Params, TEXT("max_path_string_hits"), MaxPathStringHitsNumber);
	const int32 MaxPathStringHits = FMath::Clamp(static_cast<int32>(MaxPathStringHitsNumber), 1, 10000);

	bool bIncludeHard = true;
	bool bIncludeSoft = true;
	bool bIncludeManage = true;
	bool bIncludeSearchableNames = true;
	JsonTryGetBool(Ctx.Params, TEXT("include_hard"), bIncludeHard);
	JsonTryGetBool(Ctx.Params, TEXT("include_soft"), bIncludeSoft);
	JsonTryGetBool(Ctx.Params, TEXT("include_manage"), bIncludeManage);
	JsonTryGetBool(Ctx.Params, TEXT("include_searchable_names"), bIncludeSearchableNames);

	EDependencyCategory CategoryMask = EDependencyCategory::None;
	if (bIncludeHard || bIncludeSoft)
	{
		CategoryMask |= EDependencyCategory::Package;
	}
	if (bIncludeManage)
	{
		CategoryMask |= EDependencyCategory::Manage;
	}
	if (bIncludeSearchableNames)
	{
		CategoryMask |= EDependencyCategory::SearchableName;
	}

	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();
	if (bForceScan)
	{
		AssetRegistry.SearchAllAssets(true);
		AssetRegistry.WaitForCompletion();
	}

	TArray<TSharedPtr<FJsonValue>> FolderExpansionJson;
	for (const FString& FolderPath : RequestedFolderPaths)
	{
		TSharedPtr<FJsonObject> FolderReport = MakeShared<FJsonObject>();
		FolderReport->SetStringField(TEXT("folder_path"), FolderPath);
		FolderReport->SetBoolField(TEXT("recursive"), bRecursive);

		if (FolderPath.IsEmpty())
		{
			FolderReport->SetStringField(TEXT("status"), TEXT("empty_folder_path"));
			FolderReport->SetNumberField(TEXT("asset_count"), 0);
			FolderExpansionJson.Add(MakeShared<FJsonValueObject>(FolderReport));
			continue;
		}

		FARFilter FolderFilter;
		FolderFilter.PackagePaths.Add(*FolderPath);
		FolderFilter.bRecursivePaths = bRecursive;

		TArray<FAssetData> FolderAssets;
		AssetRegistry.GetAssets(FolderFilter, FolderAssets);
		FolderAssets.Sort([](const FAssetData& A, const FAssetData& B)
		{
			return A.PackageName.LexicalLess(B.PackageName);
		});

		TArray<TSharedPtr<FJsonValue>> FolderAssetPathsJson;
		for (const FAssetData& FolderAsset : FolderAssets)
		{
			const FString PackageName = FolderAsset.PackageName.ToString();
			if (PackageName.IsEmpty())
			{
				continue;
			}
			RequestedAssetPaths.Add(PackageName);
			FolderAssetPathsJson.Add(MakeShared<FJsonValueString>(PackageName));
		}

		FolderReport->SetStringField(TEXT("status"), TEXT("expanded"));
		FolderReport->SetNumberField(TEXT("asset_count"), FolderAssetPathsJson.Num());
		FolderReport->SetArrayField(TEXT("asset_paths"), FolderAssetPathsJson);
		FolderExpansionJson.Add(MakeShared<FJsonValueObject>(FolderReport));
	}

	struct FDeleteCandidate
	{
		FString InputPath;
		FString PackageName;
		FString ObjectPath;
		FString FilenameBefore;
		UObject* Asset = nullptr;
	};

	TSet<FString> RequestedDeletePackageNames;
	for (const FString& RequestedPath : RequestedAssetPaths)
	{
		const FString PackageName = NormalizeReferenceInputToPackageName(RequestedPath);
		if (!PackageName.IsEmpty())
		{
			RequestedDeletePackageNames.Add(PackageName);
		}
	}

	TSet<FString> SeenPackageNames;
	TArray<FDeleteCandidate> DeleteCandidates;
	TArray<UObject*> ObjectsToDelete;
	TArray<TSharedPtr<FJsonValue>> AssetsJson;
	TArray<TSharedPtr<FJsonValue>> BlockedJson;
	int32 InvalidCount = 0;
	int32 MissingCount = 0;
	int32 DuplicateCount = 0;
	int32 DeletableCount = 0;
	bool bPathStringHitsTruncated = false;

	auto MakeReason = [](const FString& Reason, const FString& Detail = FString()) -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("reason"), Reason);
		if (!Detail.IsEmpty())
		{
			Obj->SetStringField(TEXT("detail"), Detail);
		}
		return Obj;
	};

	UAssetEditorSubsystem* AssetEditorSubsystem = GEditor ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr;

	for (FString RequestedPath : RequestedAssetPaths)
	{
		RequestedPath.TrimStartAndEndInline();
		const FString PackageName = NormalizeReferenceInputToPackageName(RequestedPath);

		TSharedPtr<FJsonObject> AssetReport = MakeShared<FJsonObject>();
		AssetReport->SetStringField(TEXT("input_asset_path"), RequestedPath);
		AssetReport->SetStringField(TEXT("asset_path"), PackageName);
		AssetReport->SetStringField(TEXT("package_name"), PackageName);

		TArray<TSharedPtr<FJsonValue>> Reasons;
		auto AddBlockedReason = [&Reasons, &MakeReason](const FString& Reason, const FString& Detail = FString())
		{
			Reasons.Add(MakeShared<FJsonValueObject>(MakeReason(Reason, Detail)));
		};

		if (PackageName.IsEmpty())
		{
			++InvalidCount;
			AddBlockedReason(TEXT("invalid_asset_path"));
			AssetReport->SetBoolField(TEXT("deletable"), false);
			AssetReport->SetArrayField(TEXT("blocked_reasons"), Reasons);
			AssetsJson.Add(MakeShared<FJsonValueObject>(AssetReport));
			BlockedJson.Add(MakeShared<FJsonValueObject>(AssetReport));
			continue;
		}

		if (SeenPackageNames.Contains(PackageName))
		{
			++DuplicateCount;
			AddBlockedReason(TEXT("duplicate_request"));
			AssetReport->SetBoolField(TEXT("deletable"), false);
			AssetReport->SetBoolField(TEXT("skipped_duplicate"), true);
			AssetReport->SetArrayField(TEXT("blocked_reasons"), Reasons);
			AssetsJson.Add(MakeShared<FJsonValueObject>(AssetReport));
			continue;
		}
		SeenPackageNames.Add(PackageName);

		if (!bAllowNonGame && !PackageName.StartsWith(TEXT("/Game/")))
		{
			AddBlockedReason(TEXT("non_game_asset"));
		}

		if (bForceScan)
		{
			FString PackageFilename;
			if (FPackageName::TryConvertLongPackageNameToFilename(PackageName, PackageFilename, FPackageName::GetAssetPackageExtension()) && FPaths::FileExists(PackageFilename))
			{
				AssetRegistry.ScanFilesSynchronous({ PackageFilename }, true);
			}
			else if (FPackageName::TryConvertLongPackageNameToFilename(PackageName, PackageFilename, FPackageName::GetMapPackageExtension()) && FPaths::FileExists(PackageFilename))
			{
				AssetRegistry.ScanFilesSynchronous({ PackageFilename }, true);
			}
			AssetRegistry.WaitForCompletion();
		}

		UObject* Asset = LoadAssetObject(PackageName);
		if (!IsValid(Asset))
		{
			++MissingCount;
			AddBlockedReason(TEXT("asset_not_found"));
		}

		TArray<FAssetData> AssetData;
		AssetRegistry.GetAssetsByPackageName(FName(*PackageName), AssetData, false);
		AssetReport->SetNumberField(TEXT("asset_data_count"), AssetData.Num());
		TArray<TSharedPtr<FJsonValue>> AssetDataJson;
		for (const FAssetData& Data : AssetData)
		{
			TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("asset_name"), Data.AssetName.ToString());
			Item->SetStringField(TEXT("package_name"), Data.PackageName.ToString());
			Item->SetStringField(TEXT("object_path"), Data.GetSoftObjectPath().ToString());
			Item->SetStringField(TEXT("asset_class"), AssetDataClassPathString(Data));
			Item->SetBoolField(TEXT("is_redirector"), AssetDataClassPathString(Data).EndsWith(TEXT("ObjectRedirector")));
			AssetDataJson.Add(MakeShared<FJsonValueObject>(Item));
		}
		AssetReport->SetArrayField(TEXT("asset_data"), AssetDataJson);

		const FString ObjectPath = Asset ? Asset->GetPathName() : ToObjectPath(PackageName);
		AssetReport->SetStringField(TEXT("object_path"), ObjectPath);
		AssetReport->SetStringField(TEXT("asset_class"), Asset ? Asset->GetClass()->GetPathName() : FString());
		AssetReport->SetBoolField(TEXT("asset_found"), Asset != nullptr);
		AssetReport->SetBoolField(TEXT("package_dirty"), Asset && Asset->GetOutermost() ? Asset->GetOutermost()->IsDirty() : false);
		if (Asset && Asset->GetOutermost() && Asset->GetOutermost()->IsDirty() && !bAllowDirty)
		{
			AddBlockedReason(TEXT("dirty_package"));
		}

		FString ExistingPackageFilename;
		const bool bPackageExistsOnDisk = FPackageName::DoesPackageExist(PackageName, &ExistingPackageFilename);
		AssetReport->SetBoolField(TEXT("package_exists_on_disk"), bPackageExistsOnDisk);
		AssetReport->SetStringField(TEXT("filename"), ExistingPackageFilename);

		bool bOpenInEditor = false;
		if (AssetEditorSubsystem && Asset)
		{
			TArray<UObject*> EditedAssets = AssetEditorSubsystem->GetAllEditedAssets();
			for (UObject* EditedAsset : EditedAssets)
			{
				if (EditedAsset && EditedAsset->GetOutermost() == Asset->GetOutermost())
				{
					bOpenInEditor = true;
					break;
				}
			}
			if (bOpenInEditor && bCloseAssetEditors)
			{
				AssetEditorSubsystem->CloseAllEditorsForAsset(Asset);
				bOpenInEditor = false;
				EditedAssets = AssetEditorSubsystem->GetAllEditedAssets();
				for (UObject* EditedAsset : EditedAssets)
				{
					if (EditedAsset && EditedAsset->GetOutermost() == Asset->GetOutermost())
					{
						bOpenInEditor = true;
						break;
					}
				}
			}
		}
		AssetReport->SetBoolField(TEXT("open_in_asset_editor"), bOpenInEditor);
		if (bOpenInEditor && !bAllowOpenEditors)
		{
			AddBlockedReason(TEXT("asset_editor_open"));
		}

		const FAssetIdentifier RootIdentifier{ FName(*PackageName) };
		TArray<FAssetDependency> ReferencerEdges;
		if (CategoryMask != EDependencyCategory::None)
		{
			AssetRegistry.GetReferencers(RootIdentifier, ReferencerEdges, CategoryMask);
		}

		TArray<TSharedPtr<FJsonValue>> ReferencersJson;
		int32 ExternalReferencerCount = 0;
		int32 InternalReferencerCount = 0;
		int32 SelfReferencerCount = 0;
		for (const FAssetDependency& ReferencerEdge : ReferencerEdges)
		{
			if (!ShouldIncludeDependencyEdge(ReferencerEdge.Category, ReferencerEdge.Properties, bIncludeHard, bIncludeSoft, bIncludeManage, bIncludeSearchableNames))
			{
				continue;
			}

			TSharedPtr<FJsonObject> EdgeJson = MakeAssetRegistryEdgeJson(ReferencerEdge.AssetId, RootIdentifier, TEXT("referencers"), 1, ReferencerEdge.Category, ReferencerEdge.Properties);
			const FString SourcePackage = EdgeJson->GetStringField(TEXT("source_package"));
			const bool bSelfReference = SourcePackage == PackageName;
			const bool bInternalReference = !bSelfReference && RequestedDeletePackageNames.Contains(SourcePackage);
			EdgeJson->SetBoolField(TEXT("self_reference"), bSelfReference);
			EdgeJson->SetBoolField(TEXT("internal_reference"), bInternalReference);
			ReferencersJson.Add(MakeShared<FJsonValueObject>(EdgeJson));
			if (bSelfReference)
			{
				++SelfReferencerCount;
			}
			else if (bInternalReference)
			{
				++InternalReferencerCount;
			}
			else
			{
				++ExternalReferencerCount;
			}
		}
		AssetReport->SetArrayField(TEXT("referencers"), ReferencersJson);
		AssetReport->SetNumberField(TEXT("referencer_count"), ReferencersJson.Num());
		AssetReport->SetNumberField(TEXT("external_referencer_count"), ExternalReferencerCount);
		AssetReport->SetNumberField(TEXT("internal_referencer_count"), InternalReferencerCount);
		AssetReport->SetNumberField(TEXT("self_referencer_count"), SelfReferencerCount);
		if (ExternalReferencerCount > 0)
		{
			AddBlockedReason(TEXT("asset_still_referenced"), FString::Printf(TEXT("%d external referencer(s)"), ExternalReferencerCount));
		}

		TArray<TSharedPtr<FJsonValue>> PathStringHitsJson;
		int32 ExternalPathStringHitCount = 0;
		int32 InternalPathStringHitCount = 0;
		int32 SelfPathStringHitCount = 0;
		if (bIncludePathStringHits)
		{
			bool bThisPathStringHitsTruncated = false;
			PathStringHitsJson = CollectPackagePathStringHits(PackageName, MaxPathStringHits, bThisPathStringHitsTruncated);
			bPathStringHitsTruncated |= bThisPathStringHitsTruncated;
			for (const TSharedPtr<FJsonValue>& HitValue : PathStringHitsJson)
			{
				const TSharedPtr<FJsonObject>* HitObj = nullptr;
				if (!HitValue.IsValid() || !HitValue->TryGetObject(HitObj) || !HitObj || !HitObj->IsValid())
				{
					continue;
				}
				if ((*HitObj)->GetBoolField(TEXT("self_hit")))
				{
					++SelfPathStringHitCount;
				}
				else
				{
					FString HitPackageName;
					(*HitObj)->TryGetStringField(TEXT("package_name"), HitPackageName);
					const bool bInternalHit = RequestedDeletePackageNames.Contains(NormalizeAssetPath(HitPackageName));
					(*HitObj)->SetBoolField(TEXT("internal_hit"), bInternalHit);
					if (bInternalHit)
					{
						++InternalPathStringHitCount;
					}
					else
					{
						++ExternalPathStringHitCount;
					}
				}
			}
		}
		AssetReport->SetArrayField(TEXT("path_string_hits"), PathStringHitsJson);
		AssetReport->SetNumberField(TEXT("path_string_hit_count"), PathStringHitsJson.Num());
		AssetReport->SetNumberField(TEXT("external_path_string_hit_count"), ExternalPathStringHitCount);
		AssetReport->SetNumberField(TEXT("internal_path_string_hit_count"), InternalPathStringHitCount);
		AssetReport->SetNumberField(TEXT("self_path_string_hit_count"), SelfPathStringHitCount);
		if (bBlockOnPathStringHits && ExternalPathStringHitCount > 0)
		{
			AddBlockedReason(TEXT("path_string_reference_hits"), FString::Printf(TEXT("%d external path string hit(s)"), ExternalPathStringHitCount));
		}

		const bool bDeletable = Reasons.IsEmpty();
		AssetReport->SetBoolField(TEXT("deletable"), bDeletable);
		AssetReport->SetArrayField(TEXT("blocked_reasons"), Reasons);
		AssetsJson.Add(MakeShared<FJsonValueObject>(AssetReport));
		if (!bDeletable)
		{
			BlockedJson.Add(MakeShared<FJsonValueObject>(AssetReport));
			continue;
		}

		++DeletableCount;
		if (Asset)
		{
			DeleteCandidates.Add({ RequestedPath, PackageName, ObjectPath, ExistingPackageFilename, Asset });
			ObjectsToDelete.Add(Asset);
		}
	}

	OutData = MakeShared<FJsonObject>();
	OutData->SetBoolField(TEXT("dry_run"), bDryRun);
	OutData->SetBoolField(TEXT("recursive"), bRecursive);
	OutData->SetBoolField(TEXT("force_scan"), bForceScan);
	OutData->SetBoolField(TEXT("allow_non_game"), bAllowNonGame);
	OutData->SetBoolField(TEXT("allow_dirty"), bAllowDirty);
	OutData->SetBoolField(TEXT("close_asset_editors"), bCloseAssetEditors);
	OutData->SetBoolField(TEXT("allow_open_editors"), bAllowOpenEditors);
	OutData->SetBoolField(TEXT("include_path_string_hits"), bIncludePathStringHits);
	OutData->SetBoolField(TEXT("block_on_path_string_hits"), bBlockOnPathStringHits);
	OutData->SetBoolField(TEXT("delete_empty_folders"), bDeleteEmptyFolders);
	OutData->SetBoolField(TEXT("path_string_hits_truncated"), bPathStringHitsTruncated);
	OutData->SetNumberField(TEXT("requested_folder_count"), RequestedFolderPaths.Num());
	OutData->SetArrayField(TEXT("folder_expansions"), FolderExpansionJson);
	OutData->SetNumberField(TEXT("requested_count"), RequestedAssetPaths.Num());
	OutData->SetNumberField(TEXT("unique_package_count"), SeenPackageNames.Num());
	OutData->SetNumberField(TEXT("invalid_count"), InvalidCount);
	OutData->SetNumberField(TEXT("missing_count"), MissingCount);
	OutData->SetNumberField(TEXT("duplicate_count"), DuplicateCount);
	OutData->SetNumberField(TEXT("deletable_count"), DeletableCount);
	OutData->SetNumberField(TEXT("blocked_count"), BlockedJson.Num());
	OutData->SetArrayField(TEXT("assets"), AssetsJson);
	OutData->SetArrayField(TEXT("blocked"), BlockedJson);

	TSharedPtr<FJsonObject> OptionsJson = MakeShared<FJsonObject>();
	OptionsJson->SetBoolField(TEXT("include_hard"), bIncludeHard);
	OptionsJson->SetBoolField(TEXT("include_soft"), bIncludeSoft);
	OptionsJson->SetBoolField(TEXT("include_manage"), bIncludeManage);
	OptionsJson->SetBoolField(TEXT("include_searchable_names"), bIncludeSearchableNames);
	OptionsJson->SetNumberField(TEXT("max_path_string_hits"), MaxPathStringHits);
	OutData->SetObjectField(TEXT("reference_options"), OptionsJson);

	if (!BlockedJson.IsEmpty() && bFailOnBlocked)
	{
		OutError = TEXT("asset_delete_blocked");
		return false;
	}

	TArray<TSharedPtr<FJsonValue>> DeletedJson;
	TArray<TSharedPtr<FJsonValue>> RemainingPackagesJson;
	int32 DeletedCount = 0;
	int32 ResidualObjectCount = 0;
	int32 RemainingAssetRegistryCount = 0;
	if (!bDryRun && !ObjectsToDelete.IsEmpty())
	{
		TGuardValue<bool> UnattendedScriptGuard(GIsRunningUnattendedScript, true);
		DeletedCount = ObjectTools::DeleteObjectsUnchecked(ObjectsToDelete);
		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	}

	int32 RemainingPackageCount = 0;
	for (const FDeleteCandidate& Candidate : DeleteCandidates)
	{
		bool bFoundResidualObjectAfterDelete = false;
		bool bDetachedResidualObjectAfterDelete = false;
		if (!bDryRun)
		{
			bDetachedResidualObjectAfterDelete = DetachStaleObjectAtPath(Candidate.ObjectPath, &bFoundResidualObjectAfterDelete);
		}

		FString ExistingPackageFilenameAfter;
		const bool bPackageExistsAfter = FPackageName::DoesPackageExist(Candidate.PackageName, &ExistingPackageFilenameAfter);
		if (!bDryRun && bPackageExistsAfter)
		{
			++RemainingPackageCount;
			TSharedPtr<FJsonObject> Remaining = MakeShared<FJsonObject>();
			Remaining->SetStringField(TEXT("asset_path"), Candidate.PackageName);
			Remaining->SetStringField(TEXT("filename"), ExistingPackageFilenameAfter);
			RemainingPackagesJson.Add(MakeShared<FJsonValueObject>(Remaining));
		}

		TArray<FAssetData> AssetDataAfter;
		AssetRegistry.GetAssetsByPackageName(FName(*Candidate.PackageName), AssetDataAfter, false);
		RemainingAssetRegistryCount += AssetDataAfter.Num();
		UObject* ResidualObjectAfterDelete = !bDryRun ? FindObject<UObject>(nullptr, *Candidate.ObjectPath) : nullptr;
		const bool bResidualObjectStillValid = !bDryRun && IsValid(ResidualObjectAfterDelete);
		if (bResidualObjectStillValid)
		{
			++ResidualObjectCount;
		}

		TSharedPtr<FJsonObject> Deleted = MakeShared<FJsonObject>();
		Deleted->SetStringField(TEXT("input_asset_path"), Candidate.InputPath);
		Deleted->SetStringField(TEXT("asset_path"), Candidate.PackageName);
		Deleted->SetStringField(TEXT("object_path"), Candidate.ObjectPath);
		Deleted->SetStringField(TEXT("filename_before"), Candidate.FilenameBefore);
		Deleted->SetStringField(TEXT("filename_after"), ExistingPackageFilenameAfter);
		Deleted->SetBoolField(TEXT("package_exists_after"), bPackageExistsAfter);
		Deleted->SetNumberField(TEXT("asset_registry_after_count"), AssetDataAfter.Num());
		Deleted->SetBoolField(TEXT("found_residual_object_after_delete"), bFoundResidualObjectAfterDelete);
		Deleted->SetBoolField(TEXT("detached_residual_object_after_delete"), bDetachedResidualObjectAfterDelete);
		Deleted->SetBoolField(TEXT("residual_object_still_valid_after_delete"), bResidualObjectStillValid);
		DeletedJson.Add(MakeShared<FJsonValueObject>(Deleted));
	}

	OutData->SetNumberField(TEXT("delete_object_result_count"), DeletedCount);
	OutData->SetNumberField(TEXT("deleted_count"), bDryRun ? 0 : DeletedCount);
	OutData->SetArrayField(TEXT("deleted_assets"), DeletedJson);
	OutData->SetNumberField(TEXT("remaining_asset_registry_count"), RemainingAssetRegistryCount);
	OutData->SetNumberField(TEXT("residual_object_count"), ResidualObjectCount);
	OutData->SetNumberField(TEXT("remaining_package_count"), RemainingPackageCount);
	OutData->SetArrayField(TEXT("remaining_packages"), RemainingPackagesJson);

	auto CountDirectoryEntries = [](const FString& DirectoryPath, int32& OutFileCount, int32& OutDirectoryCount)
	{
		OutFileCount = 0;
		OutDirectoryCount = 0;
		if (DirectoryPath.IsEmpty() || !IFileManager::Get().DirectoryExists(*DirectoryPath))
		{
			return;
		}
		TArray<FString> Files;
		IFileManager::Get().FindFiles(Files, *FPaths::Combine(DirectoryPath, TEXT("*")), true, false);
		OutFileCount = Files.Num();
		TArray<FString> Directories;
		IFileManager::Get().FindFiles(Directories, *FPaths::Combine(DirectoryPath, TEXT("*")), false, true);
		OutDirectoryCount = Directories.Num();
	};

	auto CollectChildDirectoriesRecursive = [](const FString& RootDirectoryPath)
	{
		TArray<FString> Result;
		TArray<FString> Stack;
		Stack.Add(RootDirectoryPath);
		while (!Stack.IsEmpty())
		{
			const FString Current = Stack.Pop(EAllowShrinking::No);
			TArray<FString> ChildDirectoryNames;
			IFileManager::Get().FindFiles(ChildDirectoryNames, *FPaths::Combine(Current, TEXT("*")), false, true);
			for (const FString& ChildName : ChildDirectoryNames)
			{
				const FString ChildPath = FPaths::Combine(Current, ChildName);
				Result.Add(ChildPath);
				Stack.Add(ChildPath);
			}
		}
		Result.Sort([](const FString& A, const FString& B)
		{
			return A.Len() > B.Len();
		});
		return Result;
	};

	TArray<TSharedPtr<FJsonValue>> FolderDeletionJson;
	int32 FolderDeleteCount = 0;
	int32 FolderDeleteBlockedCount = 0;
	int32 FolderWouldDeleteCount = 0;
	const bool bShouldEvaluateFolders = !RequestedFolderPaths.IsEmpty();
	if (bShouldEvaluateFolders)
	{
		TSet<FString> SeenFolderPaths;
		for (const FString& FolderPath : RequestedFolderPaths)
		{
			if (SeenFolderPaths.Contains(FolderPath))
			{
				continue;
			}
			SeenFolderPaths.Add(FolderPath);

			TSharedPtr<FJsonObject> FolderResult = MakeShared<FJsonObject>();
			FolderResult->SetStringField(TEXT("folder_path"), FolderPath);
			FolderResult->SetBoolField(TEXT("recursive"), bRecursive);
			FolderResult->SetBoolField(TEXT("delete_empty_folders"), bDeleteEmptyFolders);

			TArray<TSharedPtr<FJsonValue>> FolderReasons;
			auto AddFolderBlockedReason = [&FolderReasons, &MakeReason](const FString& Reason, const FString& Detail = FString())
			{
				FolderReasons.Add(MakeShared<FJsonValueObject>(MakeReason(Reason, Detail)));
			};

			if (!bDeleteEmptyFolders)
			{
				FolderResult->SetStringField(TEXT("status"), TEXT("disabled"));
				FolderResult->SetBoolField(TEXT("deleted"), false);
				FolderResult->SetBoolField(TEXT("would_delete"), false);
				FolderResult->SetArrayField(TEXT("blocked_reasons"), FolderReasons);
				FolderDeletionJson.Add(MakeShared<FJsonValueObject>(FolderResult));
				continue;
			}

			if (FolderPath.IsEmpty() || !FPackageName::IsValidLongPackageName(FolderPath, false))
			{
				AddFolderBlockedReason(TEXT("invalid_folder_path"));
			}
			if (!bAllowNonGame && !FolderPath.StartsWith(TEXT("/Game/")))
			{
				AddFolderBlockedReason(TEXT("non_game_folder"));
			}
			if (FolderPath == TEXT("/Game") || FolderPath == TEXT("/Engine"))
			{
				AddFolderBlockedReason(TEXT("root_folder_delete_blocked"));
			}

			FString FolderFilename;
			if (FolderReasons.IsEmpty())
			{
				FolderFilename = FPackageName::LongPackageNameToFilename(FolderPath);
				FolderFilename = FPaths::ConvertRelativePathToFull(FolderFilename);
				FPaths::NormalizeDirectoryName(FolderFilename);
			}
			FolderResult->SetStringField(TEXT("filesystem_path"), FolderFilename);

			const bool bDirectoryExists = !FolderFilename.IsEmpty() && IFileManager::Get().DirectoryExists(*FolderFilename);
			FolderResult->SetBoolField(TEXT("directory_exists"), bDirectoryExists);
			if (!bDirectoryExists && FolderReasons.IsEmpty())
			{
				FolderResult->SetStringField(TEXT("status"), TEXT("missing_noop"));
				FolderResult->SetBoolField(TEXT("deleted"), false);
				FolderResult->SetBoolField(TEXT("would_delete"), false);
				FolderResult->SetNumberField(TEXT("remaining_file_count"), 0);
				FolderResult->SetNumberField(TEXT("remaining_directory_count"), 0);
				FolderResult->SetArrayField(TEXT("blocked_reasons"), FolderReasons);
				FolderDeletionJson.Add(MakeShared<FJsonValueObject>(FolderResult));
				continue;
			}

			if (FolderReasons.IsEmpty() && bRecursive && !bDryRun)
			{
				for (const FString& ChildDirectoryPath : CollectChildDirectoriesRecursive(FolderFilename))
				{
					int32 ChildFileCount = 0;
					int32 ChildDirectoryCount = 0;
					CountDirectoryEntries(ChildDirectoryPath, ChildFileCount, ChildDirectoryCount);
					if (ChildFileCount == 0 && ChildDirectoryCount == 0)
					{
						IFileManager::Get().DeleteDirectory(*ChildDirectoryPath, false, false);
					}
				}
			}

			int32 RemainingFileCount = 0;
			int32 RemainingDirectoryCount = 0;
			if (FolderReasons.IsEmpty())
			{
				CountDirectoryEntries(FolderFilename, RemainingFileCount, RemainingDirectoryCount);
				if (RemainingFileCount > 0 || RemainingDirectoryCount > 0)
				{
					AddFolderBlockedReason(TEXT("folder_not_empty"), FString::Printf(TEXT("%d file(s), %d folder(s) remain"), RemainingFileCount, RemainingDirectoryCount));
				}
			}

			const bool bFolderBlocked = !FolderReasons.IsEmpty();
			const bool bWouldDeleteFolder = !bFolderBlocked && bDirectoryExists;
			bool bFolderDeleted = false;
			if (!bDryRun && bWouldDeleteFolder)
			{
				bFolderDeleted = IFileManager::Get().DeleteDirectory(*FolderFilename, false, false);
				if (!bFolderDeleted && IFileManager::Get().DirectoryExists(*FolderFilename))
				{
					AddFolderBlockedReason(TEXT("delete_folder_failed"));
				}
			}

			int32 RemainingFileCountAfter = 0;
			int32 RemainingDirectoryCountAfter = 0;
			const bool bDirectoryExistsAfter = !FolderFilename.IsEmpty() && IFileManager::Get().DirectoryExists(*FolderFilename);
			if (bDirectoryExistsAfter)
			{
				CountDirectoryEntries(FolderFilename, RemainingFileCountAfter, RemainingDirectoryCountAfter);
			}

			if (bDryRun && bWouldDeleteFolder)
			{
				++FolderWouldDeleteCount;
			}
			else if (!bDryRun && bFolderDeleted && !bDirectoryExistsAfter)
			{
				++FolderDeleteCount;
			}
			if (!FolderReasons.IsEmpty())
			{
				++FolderDeleteBlockedCount;
			}

			FolderResult->SetStringField(TEXT("status"), FolderReasons.IsEmpty() ? (bDryRun ? TEXT("would_delete") : TEXT("deleted")) : TEXT("blocked"));
			FolderResult->SetBoolField(TEXT("would_delete"), bWouldDeleteFolder);
			FolderResult->SetBoolField(TEXT("deleted"), bFolderDeleted && !bDirectoryExistsAfter);
			FolderResult->SetBoolField(TEXT("directory_exists_after"), bDirectoryExistsAfter);
			FolderResult->SetNumberField(TEXT("remaining_file_count"), bDirectoryExistsAfter ? RemainingFileCountAfter : RemainingFileCount);
			FolderResult->SetNumberField(TEXT("remaining_directory_count"), bDirectoryExistsAfter ? RemainingDirectoryCountAfter : RemainingDirectoryCount);
			FolderResult->SetArrayField(TEXT("blocked_reasons"), FolderReasons);
			FolderDeletionJson.Add(MakeShared<FJsonValueObject>(FolderResult));
		}
	}

	OutData->SetArrayField(TEXT("folder_deletions"), FolderDeletionJson);
	OutData->SetNumberField(TEXT("folder_delete_count"), FolderDeleteCount);
	OutData->SetNumberField(TEXT("folder_would_delete_count"), FolderWouldDeleteCount);
	OutData->SetNumberField(TEXT("folder_delete_blocked_count"), FolderDeleteBlockedCount);

	if (!bDryRun && DeletedCount < ObjectsToDelete.Num())
	{
		OutError = TEXT("asset_delete_failed");
		return false;
	}

	if (!bDryRun && (RemainingPackageCount > 0 || RemainingAssetRegistryCount > 0 || ResidualObjectCount > 0))
	{
		OutError = TEXT("asset_delete_incomplete");
		return false;
	}

	if (!bDryRun && FolderDeleteBlockedCount > 0 && bDeleteEmptyFolders)
	{
		OutError = TEXT("asset_delete_folder_incomplete");
		return false;
	}

	return true;
}

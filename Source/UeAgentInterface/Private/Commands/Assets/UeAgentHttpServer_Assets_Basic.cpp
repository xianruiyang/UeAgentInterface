// Copyright Epic Games, Inc. All Rights Reserved.

#include "UeAgentHttpServer_Assets.h"
#include "UeAgentHttpServer_AssetsOps.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "CoreGlobals.h"
#include "FileHelpers.h"
#include "Misc/PackageName.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "UObject/Package.h"

bool FUeAgentLegacyMechanics::CmdEditorGetOpenAssets(TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UAssetEditorSubsystem* AssetEditorSubsystem = GEditor ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr;
	if (!AssetEditorSubsystem)
	{
		OutError = TEXT("missing_asset_editor_subsystem");
		return false;
	}
	const TArray<UObject*> OpenAssets = AssetEditorSubsystem->GetAllEditedAssets();
	TArray<TSharedPtr<FJsonValue>> Assets;
	for (UObject* Asset : OpenAssets)
	{
		if (!Asset)
		{
			continue;
		}
		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("name"), Asset->GetName());
		Item->SetStringField(TEXT("object_path"), Asset->GetPathName());
		Item->SetStringField(TEXT("class"), Asset->GetClass() ? Asset->GetClass()->GetPathName() : TEXT(""));
		Assets.Add(MakeShared<FJsonValueObject>(Item));
	}
	OutData->SetArrayField(TEXT("open_assets"), Assets);
	OutData->SetNumberField(TEXT("count"), Assets.Num());
	return true;
}

bool FUeAgentLegacyMechanics::CmdOpenAssetEditor(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	UObject* Asset = UeAgentAssetOps::LoadAssetObject(AssetPathInput);
	if (!Asset)
	{
		OutError = TEXT("asset_not_found");
		return false;
	}

	UAssetEditorSubsystem* AssetEditorSubsystem = GEditor ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr;
	if (!AssetEditorSubsystem)
	{
		OutError = TEXT("missing_asset_editor_subsystem");
		return false;
	}
	AssetEditorSubsystem->OpenEditorForAsset(Asset);

	OutData->SetStringField(TEXT("asset_path"), Asset->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("object_path"), Asset->GetPathName());
	OutData->SetStringField(TEXT("class"), Asset->GetClass() ? Asset->GetClass()->GetPathName() : TEXT(""));
	return true;
}

bool FUeAgentLegacyMechanics::CmdSaveAsset(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	UObject* Asset = UeAgentAssetOps::LoadAssetObject(AssetPathInput);
	if (!Asset)
	{
		OutError = TEXT("asset_not_found");
		return false;
	}

	UPackage* Package = Asset->GetOutermost();
	if (!Package)
	{
		OutError = TEXT("missing_package");
		return false;
	}

	bool bOnlyIfDirty = true;
	JsonTryGetBool(Ctx.Params, TEXT("only_if_dirty"), bOnlyIfDirty);
	const bool bDirty = Package->IsDirty();
	if (bOnlyIfDirty && !bDirty)
	{
		OutData->SetBoolField(TEXT("saved"), false);
		OutData->SetStringField(TEXT("reason"), TEXT("not_dirty"));
		OutData->SetStringField(TEXT("asset_path"), Package->GetName());
		return true;
	}

	TArray<UPackage*> PackagesToSave;
	PackagesToSave.Add(Package);
	const bool bSaved = UEditorLoadingAndSavingUtils::SavePackages(PackagesToSave, false);
	if (!bSaved)
	{
		OutError = TEXT("save_asset_failed");
		return false;
	}

	OutData->SetBoolField(TEXT("saved"), true);
	OutData->SetStringField(TEXT("asset_path"), Package->GetName());
	OutData->SetStringField(TEXT("object_path"), Asset->GetPathName());
	OutData->SetBoolField(TEXT("was_dirty"), bDirty);
	return true;
}

bool FUeAgentLegacyMechanics::CmdAssetDuplicate(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString SourceAssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("source_asset_path"), SourceAssetPath) || SourceAssetPath.IsEmpty())
	{
		OutError = TEXT("missing_source_asset_path");
		return false;
	}

	FString DestinationAssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("destination_asset_path"), DestinationAssetPath) || DestinationAssetPath.IsEmpty())
	{
		OutError = TEXT("missing_destination_asset_path");
		return false;
	}

	UObject* SourceAsset = UeAgentAssetOps::LoadAssetObject(SourceAssetPath);
	if (!SourceAsset)
	{
		OutError = TEXT("source_asset_not_found");
		return false;
	}

	const FString NormalizedDestinationAssetPath = UeAgentAssetOps::NormalizeAssetPath(DestinationAssetPath);
	if (!FPackageName::IsValidLongPackageName(NormalizedDestinationAssetPath))
	{
		OutError = TEXT("invalid_destination_asset_path");
		return false;
	}

	const FString DestinationAssetName = FPackageName::GetLongPackageAssetName(NormalizedDestinationAssetPath);
	const FString DestinationPackagePath = FPackageName::GetLongPackagePath(NormalizedDestinationAssetPath);
	if (DestinationAssetName.IsEmpty() || DestinationPackagePath.IsEmpty())
	{
		OutError = TEXT("invalid_destination_asset_path");
		return false;
	}

	if (UeAgentAssetOps::LoadAssetObject(NormalizedDestinationAssetPath))
	{
		OutError = TEXT("destination_asset_already_exists");
		return false;
	}

	FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
	UObject* DuplicatedAsset = AssetToolsModule.Get().DuplicateAsset(DestinationAssetName, DestinationPackagePath, SourceAsset);
	if (!DuplicatedAsset)
	{
		OutError = TEXT("duplicate_asset_failed");
		return false;
	}
	FAssetRegistryModule::AssetCreated(DuplicatedAsset);

	bool bOpenEditor = false;
	JsonTryGetBool(Ctx.Params, TEXT("open_editor"), bOpenEditor);
	if (bOpenEditor)
	{
		if (UAssetEditorSubsystem* AssetEditorSubsystem = GEditor ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr)
		{
			AssetEditorSubsystem->OpenEditorForAsset(DuplicatedAsset);
		}
	}

	bool bSaveAfterDuplicate = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_duplicate"), bSaveAfterDuplicate);
	if (bSaveAfterDuplicate)
	{
		TArray<UPackage*> PackagesToSave;
		PackagesToSave.Add(DuplicatedAsset->GetOutermost());
		if (!UEditorLoadingAndSavingUtils::SavePackages(PackagesToSave, false))
		{
			OutError = TEXT("save_asset_failed");
			return false;
		}
	}

	OutData->SetStringField(TEXT("source_asset_path"), UeAgentAssetOps::NormalizeAssetPath(SourceAsset->GetOutermost()->GetName()));
	OutData->SetStringField(TEXT("asset_path"), NormalizedDestinationAssetPath);
	OutData->SetStringField(TEXT("object_path"), DuplicatedAsset->GetPathName());
	OutData->SetStringField(TEXT("class"), DuplicatedAsset->GetClass()->GetPathName());
	OutData->SetBoolField(TEXT("saved"), bSaveAfterDuplicate);
	return true;
}

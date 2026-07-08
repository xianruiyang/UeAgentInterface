// Copyright Epic Games, Inc. All Rights Reserved.

#include "UeAgentHttpServer_Assets.h"
#include "UeAgentHttpServer_AssetsOps.h"
#include "Utilities/UeAgentCurveJson.h"
#include "Utilities/UeAgentJsonDiagnostics.h"
#include "Module/UeAgentInterfaceLogger.h"

#include "AssetToolsModule.h"
#include "Animation/AnimSequence.h"
#include "Animation/MorphTarget.h"
#include "Animation/SkinWeightProfile.h"
#include "Animation/Skeleton.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AutomatedAssetImportData.h"
#include "CoreGlobals.h"
#include "Editor.h"
#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Engine/LocalPlayer.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/Texture.h"
#include "Engine/Texture2D.h"
#include "Factories/FbxAnimSequenceImportData.h"
#include "Factories/FbxFactory.h"
#include "Factories/FbxImportUI.h"
#include "Factories/FbxSkeletalMeshImportData.h"
#include "Factories/TextureFactory.h"
#include "FileHelpers.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/GameStateBase.h"
#include "HAL/IConsoleManager.h"
#include "LevelEditor.h"
#include "Misc/FileHelper.h"
#include "Misc/OutputDevice.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "ObjectTools.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "Rendering/SkeletalMeshModel.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Templates/UnrealTemplate.h"
#include "UObject/ObjectRedirector.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectIterator.h"
#include "Widgets/Docking/SDockTab.h"

namespace UeAgentAssetOps
{
	static bool RequestCloseLiveTab(FTabManager& TabManager, const FName& TabId)
	{
		const TSharedPtr<SDockTab> ExistingTab = TabManager.FindExistingLiveTab(FTabId(TabId));
		if (!ExistingTab.IsValid())
		{
			return false;
		}

		ExistingTab->RequestCloseTab();
		return true;
	}

	static int32 CloseWorldBrowserTabsForExit()
	{
		const FName WorldBrowserTabIds[] =
		{
			TEXT("WorldBrowserHierarchy"),
			TEXT("WorldBrowserDetails"),
			TEXT("WorldBrowserComposition"),
			TEXT("WorldBrowserPartitionEditor")
		};

		int32 CloseRequestCount = 0;
		FLevelEditorModule* LevelEditorModule = FModuleManager::GetModulePtr<FLevelEditorModule>(TEXT("LevelEditor"));
		const TSharedPtr<FTabManager> LevelEditorTabManager = LevelEditorModule ? LevelEditorModule->GetLevelEditorTabManager() : nullptr;

		for (const FName& TabId : WorldBrowserTabIds)
		{
			bool bRequestedClose = false;
			if (LevelEditorTabManager.IsValid())
			{
				bRequestedClose = RequestCloseLiveTab(*LevelEditorTabManager, TabId);
			}
			if (!bRequestedClose)
			{
				TSharedRef<FGlobalTabmanager> GlobalTabManager = FGlobalTabmanager::Get();
				bRequestedClose = RequestCloseLiveTab(GlobalTabManager.Get(), TabId);
			}
			if (bRequestedClose)
			{
				++CloseRequestCount;
			}
		}

		if (FSlateApplication::IsInitialized())
		{
			FSlateApplication::Get().DismissAllMenus();
		}

		return CloseRequestCount;
	}

	static int32 UnloadKnownLateShutdownEditorModulesForExit(TArray<TSharedPtr<FJsonValue>>& OutUnloadedModules)
	{
		const FName ModuleNames[] =
		{
			TEXT("PCGEditor")
		};

		int32 UnloadedCount = 0;
		FModuleManager& ModuleManager = FModuleManager::Get();
		for (const FName& ModuleName : ModuleNames)
		{
			if (!ModuleManager.IsModuleLoaded(ModuleName))
			{
				continue;
			}

			const bool bUnloaded = ModuleManager.UnloadModule(ModuleName, false, false);
			TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("name"), ModuleName.ToString());
			Item->SetBoolField(TEXT("unloaded"), bUnloaded);
			OutUnloadedModules.Add(MakeShared<FJsonValueObject>(Item));
			if (bUnloaded)
			{
				++UnloadedCount;
			}
		}
		return UnloadedCount;
	}

	static void ScheduleEditorRequestExit(double DelaySeconds)
	{
		FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateLambda([](float)
			{
				FPlatformMisc::RequestExit(false);
				return false;
			}),
			FMath::Max(0.0f, static_cast<float>(DelaySeconds)));
	}

	static UPackage* ResolvePackageByAssetPath(const FString& InPath)
	{
		const FString NormalizedAssetPath = NormalizeAssetPath(InPath);
		if (NormalizedAssetPath.IsEmpty())
		{
			return nullptr;
		}

		if (UPackage* ExistingPackage = FindPackage(nullptr, *NormalizedAssetPath))
		{
			return ExistingPackage;
		}

		if (UObject* AssetObj = LoadAssetObject(NormalizedAssetPath))
		{
			return AssetObj->GetOutermost();
		}

		return nullptr;
	}

	static bool ShouldSkipDirtyPackage(UPackage* Package)
	{
		if (!Package)
		{
			return true;
		}

		if (Package->HasAnyFlags(RF_Transient) || Package->HasAnyPackageFlags(PKG_CompiledIn))
		{
			return true;
		}

		const FString PackageName = Package->GetName();
		if (PackageName.IsEmpty() ||
			PackageName.StartsWith(TEXT("/Script/")) ||
			PackageName.StartsWith(TEXT("/Engine/Transient")) ||
			PackageName.StartsWith(TEXT("/Temp/")))
		{
			return true;
		}

		return !FPackageName::IsValidLongPackageName(PackageName, false);
	}

	static void AddPackageIfValid(TSet<UPackage*>& OutSet, UPackage* Package)
	{
		if (!Package || ShouldSkipDirtyPackage(Package))
		{
			return;
		}
		OutSet.Add(Package);
	}

	static int32 ClearDirtyFlagForPackages(const TSet<UPackage*>& Packages)
	{
		int32 ClearedCount = 0;
		for (UPackage* Package : Packages)
		{
			if (!Package || !Package->IsDirty())
			{
				continue;
			}

			Package->SetDirtyFlag(false);
			++ClearedCount;
		}
		return ClearedCount;
	}

	struct FDirtyResolutionOptions
	{
		bool bSaveCurrentLevel = false;
		bool bDiscardCurrentLevel = false;
		bool bSaveAllDirty = false;
		bool bDiscardAllDirty = false;
		bool bCloseAllAssetEditors = false;
		bool bOnlySaveDirty = true;
		TArray<FString> SaveResourcePaths;
		TArray<FString> DiscardResourcePaths;
	};

	struct FDirtyResolutionSummary
	{
		int32 SavedPackageCount = 0;
		int32 ExplicitDiscardedPackageCount = 0;
		int32 AutoDiscardedPackageCount = 0;
		TArray<FString> SaveResourceNotFound;
		TArray<FString> DiscardResourceNotFound;
		TArray<UPackage*> DirtyPackagesBefore;
		TArray<UPackage*> DirtyPackagesAfter;
	};

	static void CollectDirtyPackages(TArray<UPackage*>& OutPackages)
	{
		OutPackages.Reset();
		TSet<UPackage*> UniquePackages;
		for (TObjectIterator<UPackage> It; It; ++It)
		{
			UPackage* Package = *It;
			if (!Package || !Package->IsDirty() || ShouldSkipDirtyPackage(Package))
			{
				continue;
			}
			UniquePackages.Add(Package);
		}
		for (UPackage* Package : UniquePackages)
		{
			OutPackages.Add(Package);
		}
		OutPackages.Sort([](const UPackage& A, const UPackage& B)
		{
			return A.GetName() < B.GetName();
		});
	}

	static UObject* FindRepresentativeObjectInPackage(UPackage* Package)
	{
		if (!Package)
		{
			return nullptr;
		}

		const FString PackageName = Package->GetName();
		const FString AssetName = FPackageName::GetLongPackageAssetName(PackageName);
		if (!AssetName.IsEmpty())
		{
			if (UObject* NamedObject = StaticFindObjectFast(UObject::StaticClass(), Package, FName(*AssetName)))
			{
				return NamedObject;
			}
		}

		UObject* FirstTopLevelObject = nullptr;
		for (TObjectIterator<UObject> It; It; ++It)
		{
			UObject* Obj = *It;
			if (!Obj || Obj == Package || Obj->HasAnyFlags(RF_Transient))
			{
				continue;
			}
			if (Obj->GetOutermost() != Package || Obj->GetOuter() != Package)
			{
				continue;
			}

			if (Obj->IsA<UWorld>())
			{
				return Obj;
			}
			if (!FirstTopLevelObject)
			{
				FirstTopLevelObject = Obj;
			}
		}

		return FirstTopLevelObject;
	}

	static bool IsPackageOpenInEditor(UPackage* Package, UAssetEditorSubsystem* AssetEditorSubsystem)
	{
		if (!Package || !AssetEditorSubsystem)
		{
			return false;
		}

		const TArray<UObject*> OpenAssets = AssetEditorSubsystem->GetAllEditedAssets();
		for (UObject* Asset : OpenAssets)
		{
			if (Asset && Asset->GetOutermost() == Package)
			{
				return true;
			}
		}
		return false;
	}

	static TSharedPtr<FJsonObject> MakeDirtyResourceJson(UPackage* Package, UWorld* EditorWorld, UAssetEditorSubsystem* AssetEditorSubsystem)
	{
		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		if (!Package)
		{
			return Item;
		}

		UObject* RepresentativeObject = FindRepresentativeObjectInPackage(Package);
		const FString ResourcePath = Package->GetName();
		const bool bIsCurrentLevel = EditorWorld && EditorWorld->GetOutermost() == Package;
		const bool bIsLevel = bIsCurrentLevel || (RepresentativeObject && RepresentativeObject->IsA<UWorld>());

		Item->SetStringField(TEXT("resource_path"), ResourcePath);
		Item->SetStringField(TEXT("asset_path"), ResourcePath);
		Item->SetStringField(TEXT("package_name"), ResourcePath);
		Item->SetBoolField(TEXT("dirty"), Package->IsDirty());
		Item->SetBoolField(TEXT("decision_required"), Package->IsDirty());
		Item->SetBoolField(TEXT("is_current_level"), bIsCurrentLevel);
		Item->SetBoolField(TEXT("is_level"), bIsLevel);
		Item->SetBoolField(TEXT("is_open_in_editor"), IsPackageOpenInEditor(Package, AssetEditorSubsystem));
		Item->SetStringField(TEXT("kind"), bIsLevel ? TEXT("level") : TEXT("asset"));

		if (RepresentativeObject)
		{
			Item->SetStringField(TEXT("display_name"), RepresentativeObject->GetName());
			Item->SetStringField(TEXT("object_path"), RepresentativeObject->GetPathName());
			Item->SetStringField(TEXT("class"), RepresentativeObject->GetClass() ? RepresentativeObject->GetClass()->GetPathName() : TEXT(""));
		}
		else
		{
			Item->SetStringField(TEXT("display_name"), FPackageName::GetLongPackageAssetName(ResourcePath));
			Item->SetStringField(TEXT("object_path"), TEXT(""));
			Item->SetStringField(TEXT("class"), TEXT(""));
		}

		return Item;
	}

	static void MakeDirtyResourceArrayJson(
		const TArray<UPackage*>& Packages,
		UWorld* EditorWorld,
		UAssetEditorSubsystem* AssetEditorSubsystem,
		TArray<TSharedPtr<FJsonValue>>& OutItems)
	{
		OutItems.Reset();
		OutItems.Reserve(Packages.Num());
		for (UPackage* Package : Packages)
		{
			OutItems.Add(MakeShared<FJsonValueObject>(MakeDirtyResourceJson(Package, EditorWorld, AssetEditorSubsystem)));
		}
	}

	static void ParseResourcePathArrays(const TSharedPtr<FJsonObject>& Params, TArray<FString>& OutSavePaths, TArray<FString>& OutDiscardPaths)
	{
		OutSavePaths.Reset();
		OutDiscardPaths.Reset();

		if (!ReadStringArrayParam(Params, TEXT("save_resource_paths"), OutSavePaths))
		{
			if (!ReadStringArrayParam(Params, TEXT("save_asset_paths"), OutSavePaths))
			{
				ReadStringArrayParam(Params, TEXT("save_resources"), OutSavePaths);
			}
		}
		if (!ReadStringArrayParam(Params, TEXT("discard_resource_paths"), OutDiscardPaths))
		{
			if (!ReadStringArrayParam(Params, TEXT("discard_asset_paths"), OutDiscardPaths))
			{
				ReadStringArrayParam(Params, TEXT("discard_resources"), OutDiscardPaths);
			}
		}
	}

	static bool ResolveDirtyResources(
		const FDirtyResolutionOptions& Options,
		UWorld* EditorWorld,
		FDirtyResolutionSummary& OutSummary,
		FString& OutError)
	{
		OutSummary = FDirtyResolutionSummary();

		if (Options.bSaveCurrentLevel && Options.bDiscardCurrentLevel)
		{
			OutError = TEXT("save_and_discard_current_level_conflict");
			return false;
		}

		TSet<UPackage*> DirtyPackagesBeforeSet;
		for (TObjectIterator<UPackage> It; It; ++It)
		{
			UPackage* Package = *It;
			if (!Package || !Package->IsDirty() || ShouldSkipDirtyPackage(Package))
			{
				continue;
			}
			DirtyPackagesBeforeSet.Add(Package);
		}
		for (UPackage* Package : DirtyPackagesBeforeSet)
		{
			OutSummary.DirtyPackagesBefore.Add(Package);
		}
		OutSummary.DirtyPackagesBefore.Sort([](const UPackage& A, const UPackage& B)
		{
			return A.GetName() < B.GetName();
		});

		TSet<UPackage*> SavePackagesSet;
		TSet<UPackage*> DiscardPackagesSet;

		for (const FString& SavePath : Options.SaveResourcePaths)
		{
			UPackage* Package = ResolvePackageByAssetPath(SavePath);
			if (!Package)
			{
				OutSummary.SaveResourceNotFound.Add(SavePath);
				continue;
			}
			AddPackageIfValid(SavePackagesSet, Package);
		}

		for (const FString& DiscardPath : Options.DiscardResourcePaths)
		{
			UPackage* Package = ResolvePackageByAssetPath(DiscardPath);
			if (!Package)
			{
				OutSummary.DiscardResourceNotFound.Add(DiscardPath);
				continue;
			}
			AddPackageIfValid(DiscardPackagesSet, Package);
		}

		if (EditorWorld && (Options.bSaveCurrentLevel || Options.bDiscardCurrentLevel))
		{
			if (UPackage* WorldPackage = EditorWorld->GetOutermost())
			{
				if (Options.bSaveCurrentLevel)
				{
					SavePackagesSet.Add(WorldPackage);
				}
				if (Options.bDiscardCurrentLevel)
				{
					DiscardPackagesSet.Add(WorldPackage);
				}
			}
		}

		for (UPackage* DiscardPackage : DiscardPackagesSet)
		{
			SavePackagesSet.Remove(DiscardPackage);
		}

		if (Options.bSaveAllDirty)
		{
			for (UPackage* DirtyPackage : DirtyPackagesBeforeSet)
			{
				if (!DiscardPackagesSet.Contains(DirtyPackage))
				{
					SavePackagesSet.Add(DirtyPackage);
				}
			}
		}

		TArray<UPackage*> PackagesToSave;
		for (UPackage* Package : SavePackagesSet)
		{
			if (!Package)
			{
				continue;
			}
			if (Options.bOnlySaveDirty && !Package->IsDirty())
			{
				continue;
			}
			PackagesToSave.Add(Package);
		}

		if (PackagesToSave.Num() > 0)
		{
			if (!UEditorLoadingAndSavingUtils::SavePackages(PackagesToSave, false))
			{
				OutError = TEXT("save_packages_failed");
				return false;
			}
			OutSummary.SavedPackageCount = PackagesToSave.Num();
		}

		OutSummary.ExplicitDiscardedPackageCount = ClearDirtyFlagForPackages(DiscardPackagesSet);

		if (Options.bDiscardAllDirty)
		{
			TSet<UPackage*> RemainingDirty;
			for (TObjectIterator<UPackage> It; It; ++It)
			{
				UPackage* Package = *It;
				if (!Package || !Package->IsDirty() || ShouldSkipDirtyPackage(Package))
				{
					continue;
				}
				if (SavePackagesSet.Contains(Package))
				{
					continue;
				}
				RemainingDirty.Add(Package);
			}
			OutSummary.AutoDiscardedPackageCount = ClearDirtyFlagForPackages(RemainingDirty);
		}

		if (Options.bCloseAllAssetEditors && GEditor)
		{
			if (UAssetEditorSubsystem* AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
			{
				AssetEditorSubsystem->CloseAllAssetEditors();
			}
		}

		CollectDirtyPackages(OutSummary.DirtyPackagesAfter);
		return true;
	}
}
bool FUeAgentHttpServer::CmdEditorListDirtyResources(TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UWorld* EditorWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	UAssetEditorSubsystem* AssetEditorSubsystem = GEditor ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr;

	TArray<UPackage*> DirtyPackages;
	UeAgentAssetOps::CollectDirtyPackages(DirtyPackages);

	TArray<TSharedPtr<FJsonValue>> DirtyResourcesJson;
	UeAgentAssetOps::MakeDirtyResourceArrayJson(DirtyPackages, EditorWorld, AssetEditorSubsystem, DirtyResourcesJson);

	OutData->SetNumberField(TEXT("dirty_resource_count"), DirtyResourcesJson.Num());
	OutData->SetBoolField(TEXT("has_unresolved_dirty_resources"), DirtyResourcesJson.Num() > 0);
	OutData->SetArrayField(TEXT("dirty_resources"), DirtyResourcesJson);
	return true;
}

bool FUeAgentHttpServer::CmdEditorResolveDirtyResources(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UWorld* EditorWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	UAssetEditorSubsystem* AssetEditorSubsystem = GEditor ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr;

	UeAgentAssetOps::FDirtyResolutionOptions Options;
	JsonTryGetBool(Ctx.Params, TEXT("save_current_level"), Options.bSaveCurrentLevel);
	JsonTryGetBool(Ctx.Params, TEXT("discard_current_level"), Options.bDiscardCurrentLevel);
	JsonTryGetBool(Ctx.Params, TEXT("save_all_dirty"), Options.bSaveAllDirty);
	JsonTryGetBool(Ctx.Params, TEXT("discard_all_dirty"), Options.bDiscardAllDirty);
	if (!Options.bSaveAllDirty)
	{
		JsonTryGetBool(Ctx.Params, TEXT("save_all"), Options.bSaveAllDirty);
	}
	if (!Options.bDiscardAllDirty)
	{
		JsonTryGetBool(Ctx.Params, TEXT("discard_all"), Options.bDiscardAllDirty);
	}
	JsonTryGetBool(Ctx.Params, TEXT("close_all_asset_editors"), Options.bCloseAllAssetEditors);
	JsonTryGetBool(Ctx.Params, TEXT("only_save_dirty"), Options.bOnlySaveDirty);
	UeAgentAssetOps::ParseResourcePathArrays(Ctx.Params, Options.SaveResourcePaths, Options.DiscardResourcePaths);

	UeAgentAssetOps::FDirtyResolutionSummary Summary;
	if (!UeAgentAssetOps::ResolveDirtyResources(Options, EditorWorld, Summary, OutError))
	{
		return false;
	}

	TArray<TSharedPtr<FJsonValue>> DirtyBeforeJson;
	TArray<TSharedPtr<FJsonValue>> DirtyAfterJson;
	UeAgentAssetOps::MakeDirtyResourceArrayJson(Summary.DirtyPackagesBefore, EditorWorld, AssetEditorSubsystem, DirtyBeforeJson);
	UeAgentAssetOps::MakeDirtyResourceArrayJson(Summary.DirtyPackagesAfter, EditorWorld, AssetEditorSubsystem, DirtyAfterJson);

	TArray<TSharedPtr<FJsonValue>> SaveNotFoundJson;
	for (const FString& Item : Summary.SaveResourceNotFound)
	{
		SaveNotFoundJson.Add(MakeShared<FJsonValueString>(Item));
	}

	TArray<TSharedPtr<FJsonValue>> DiscardNotFoundJson;
	for (const FString& Item : Summary.DiscardResourceNotFound)
	{
		DiscardNotFoundJson.Add(MakeShared<FJsonValueString>(Item));
	}

	OutData->SetNumberField(TEXT("saved_package_count"), Summary.SavedPackageCount);
	OutData->SetNumberField(TEXT("explicit_discarded_package_count"), Summary.ExplicitDiscardedPackageCount);
	OutData->SetNumberField(TEXT("auto_discarded_package_count"), Summary.AutoDiscardedPackageCount);
	OutData->SetBoolField(TEXT("close_all_asset_editors"), Options.bCloseAllAssetEditors);
	OutData->SetArrayField(TEXT("save_resource_not_found"), SaveNotFoundJson);
	OutData->SetArrayField(TEXT("discard_resource_not_found"), DiscardNotFoundJson);
	OutData->SetArrayField(TEXT("dirty_resources_before"), DirtyBeforeJson);
	OutData->SetArrayField(TEXT("dirty_resources_after"), DirtyAfterJson);
	OutData->SetNumberField(TEXT("remaining_dirty_resource_count"), DirtyAfterJson.Num());
	OutData->SetBoolField(TEXT("all_dirty_resources_resolved"), DirtyAfterJson.Num() == 0);
	return true;
}

bool FUeAgentHttpServer::CmdEditorClose(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	UWorld* EditorWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	UAssetEditorSubsystem* AssetEditorSubsystem = GEditor ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr;

	bool bCloseAllAssetEditors = true;
	JsonTryGetBool(Ctx.Params, TEXT("close_all_asset_editors"), bCloseAllAssetEditors);

	bool bCloseWorldBrowserTabs = true;
	JsonTryGetBool(Ctx.Params, TEXT("close_world_browser_tabs"), bCloseWorldBrowserTabs);

	bool bUnloadLateShutdownEditorModules = true;
	JsonTryGetBool(Ctx.Params, TEXT("unload_late_shutdown_editor_modules"), bUnloadLateShutdownEditorModules);

	bool bRequestExit = true;
	JsonTryGetBool(Ctx.Params, TEXT("request_exit"), bRequestExit);

	double ExitDelaySeconds = 0.25;
	Ctx.Params->TryGetNumberField(TEXT("exit_delay_seconds"), ExitDelaySeconds);
	ExitDelaySeconds = FMath::Clamp(ExitDelaySeconds, 0.0, 10.0);

	TArray<UPackage*> DirtyPackages;
	UeAgentAssetOps::CollectDirtyPackages(DirtyPackages);

	TArray<TSharedPtr<FJsonValue>> DirtyResourcesJson;
	UeAgentAssetOps::MakeDirtyResourceArrayJson(DirtyPackages, EditorWorld, AssetEditorSubsystem, DirtyResourcesJson);

	OutData->SetBoolField(TEXT("request_exit"), bRequestExit);
	OutData->SetBoolField(TEXT("close_all_asset_editors"), bCloseAllAssetEditors);
	OutData->SetBoolField(TEXT("close_world_browser_tabs"), bCloseWorldBrowserTabs);
	OutData->SetBoolField(TEXT("unload_late_shutdown_editor_modules"), bUnloadLateShutdownEditorModules);
	OutData->SetNumberField(TEXT("exit_delay_seconds"), ExitDelaySeconds);
	OutData->SetNumberField(TEXT("dirty_resource_count"), DirtyResourcesJson.Num());
	OutData->SetArrayField(TEXT("dirty_resources"), DirtyResourcesJson);

	if (DirtyResourcesJson.Num() > 0)
	{
		OutData->SetBoolField(TEXT("close_requested"), false);
		OutData->SetBoolField(TEXT("closed"), false);
		OutError = TEXT("editor_has_unresolved_dirty_resources");
		return false;
	}

	if (bCloseAllAssetEditors && GEditor)
	{
		if (UAssetEditorSubsystem* AssetEditorSubsystemLocal = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
		{
			AssetEditorSubsystemLocal->CloseAllAssetEditors();
		}
	}

	int32 ClosedWorldBrowserTabCount = 0;
	if (bCloseWorldBrowserTabs)
	{
		ClosedWorldBrowserTabCount = UeAgentAssetOps::CloseWorldBrowserTabsForExit();
	}
	OutData->SetNumberField(TEXT("closed_world_browser_tab_count"), ClosedWorldBrowserTabCount);

	TArray<TSharedPtr<FJsonValue>> UnloadedModulesJson;
	int32 UnloadedModuleCount = 0;
	if (bUnloadLateShutdownEditorModules && bRequestExit)
	{
		UnloadedModuleCount = UeAgentAssetOps::UnloadKnownLateShutdownEditorModulesForExit(UnloadedModulesJson);
	}
	OutData->SetNumberField(TEXT("unloaded_late_shutdown_editor_module_count"), UnloadedModuleCount);
	OutData->SetArrayField(TEXT("unloaded_late_shutdown_editor_modules"), UnloadedModulesJson);

	if (bRequestExit)
	{
		UeAgentAssetOps::ScheduleEditorRequestExit(ExitDelaySeconds);
	}

	OutData->SetBoolField(TEXT("close_requested"), bRequestExit);
	OutData->SetBoolField(TEXT("closed"), bRequestExit);
	return true;
}

bool FUeAgentHttpServer::CmdEditorPrepareExit(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	bool bSaveCurrentLevel = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_current_level"), bSaveCurrentLevel);

	bool bDiscardCurrentLevel = false;
	JsonTryGetBool(Ctx.Params, TEXT("discard_current_level"), bDiscardCurrentLevel);
	if (bSaveCurrentLevel && bDiscardCurrentLevel)
	{
		OutError = TEXT("save_and_discard_current_level_conflict");
		return false;
	}

	bool bSaveAllDirty = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_all_dirty"), bSaveAllDirty);
	if (!bSaveAllDirty)
	{
		JsonTryGetBool(Ctx.Params, TEXT("save_all"), bSaveAllDirty);
	}

	bool bDiscardAllDirty = true;
	JsonTryGetBool(Ctx.Params, TEXT("discard_all_dirty"), bDiscardAllDirty);
	if (!bDiscardAllDirty)
	{
		JsonTryGetBool(Ctx.Params, TEXT("discard_all"), bDiscardAllDirty);
	}

	bool bCloseAllAssetEditors = true;
	JsonTryGetBool(Ctx.Params, TEXT("close_all_asset_editors"), bCloseAllAssetEditors);

	bool bCloseWorldBrowserTabs = true;
	JsonTryGetBool(Ctx.Params, TEXT("close_world_browser_tabs"), bCloseWorldBrowserTabs);

	bool bUnloadLateShutdownEditorModules = true;
	JsonTryGetBool(Ctx.Params, TEXT("unload_late_shutdown_editor_modules"), bUnloadLateShutdownEditorModules);

	bool bRequestExit = true;
	JsonTryGetBool(Ctx.Params, TEXT("request_exit"), bRequestExit);

	double ExitDelaySeconds = 0.25;
	Ctx.Params->TryGetNumberField(TEXT("exit_delay_seconds"), ExitDelaySeconds);
	ExitDelaySeconds = FMath::Clamp(ExitDelaySeconds, 0.0, 10.0);

	bool bOnlySaveDirty = true;
	JsonTryGetBool(Ctx.Params, TEXT("only_save_dirty"), bOnlySaveDirty);

	TArray<FString> SaveAssetPaths;
	TArray<FString> DiscardAssetPaths;
	UeAgentAssetOps::ParseResourcePathArrays(Ctx.Params, SaveAssetPaths, DiscardAssetPaths);

	TSet<UPackage*> SavePackagesSet;
	TSet<UPackage*> DiscardPackagesSet;
	TArray<FString> SaveAssetNotFound;
	TArray<FString> DiscardAssetNotFound;

	for (const FString& SavePath : SaveAssetPaths)
	{
		UPackage* Package = UeAgentAssetOps::ResolvePackageByAssetPath(SavePath);
		if (!Package)
		{
			SaveAssetNotFound.Add(SavePath);
			continue;
		}
		UeAgentAssetOps::AddPackageIfValid(SavePackagesSet, Package);
	}

	for (const FString& DiscardPath : DiscardAssetPaths)
	{
		UPackage* Package = UeAgentAssetOps::ResolvePackageByAssetPath(DiscardPath);
		if (!Package)
		{
			DiscardAssetNotFound.Add(DiscardPath);
			continue;
		}
		UeAgentAssetOps::AddPackageIfValid(DiscardPackagesSet, Package);
	}

	UWorld* EditorWorld = nullptr;
	if (bSaveCurrentLevel || bDiscardCurrentLevel)
	{
		EditorWorld = GetEditorWorld(OutError);
		if (!EditorWorld)
		{
			return false;
		}
		if (UPackage* WorldPackage = EditorWorld->GetOutermost())
		{
			if (bSaveCurrentLevel)
			{
				SavePackagesSet.Add(WorldPackage);
			}
			if (bDiscardCurrentLevel)
			{
				DiscardPackagesSet.Add(WorldPackage);
			}
		}
	}

	TSet<UPackage*> DirtyPackagesBefore;
	for (TObjectIterator<UPackage> It; It; ++It)
	{
		UPackage* Package = *It;
		if (!Package || !Package->IsDirty() || UeAgentAssetOps::ShouldSkipDirtyPackage(Package))
		{
			continue;
		}
		DirtyPackagesBefore.Add(Package);
	}

	for (UPackage* DiscardPackage : DiscardPackagesSet)
	{
		SavePackagesSet.Remove(DiscardPackage);
	}

	if (bSaveAllDirty)
	{
		for (UPackage* DirtyPackage : DirtyPackagesBefore)
		{
			if (!DiscardPackagesSet.Contains(DirtyPackage))
			{
				SavePackagesSet.Add(DirtyPackage);
			}
		}
	}

	TArray<UPackage*> PackagesToSave;
	for (UPackage* Package : SavePackagesSet)
	{
		if (!Package)
		{
			continue;
		}
		if (bOnlySaveDirty && !Package->IsDirty())
		{
			continue;
		}
		PackagesToSave.Add(Package);
	}

	int32 SavedPackageCount = 0;
	if (PackagesToSave.Num() > 0)
	{
		const bool bSaved = UEditorLoadingAndSavingUtils::SavePackages(PackagesToSave, false);
		if (!bSaved)
		{
			OutError = TEXT("save_packages_failed");
			return false;
		}
		SavedPackageCount = PackagesToSave.Num();
	}

	const int32 ExplicitDiscardedCount = UeAgentAssetOps::ClearDirtyFlagForPackages(DiscardPackagesSet);

	int32 AutoDiscardedCount = 0;
	if (bDiscardAllDirty)
	{
		TSet<UPackage*> RemainingDirty;
		for (TObjectIterator<UPackage> It; It; ++It)
		{
			UPackage* Package = *It;
			if (!Package || !Package->IsDirty() || UeAgentAssetOps::ShouldSkipDirtyPackage(Package))
			{
				continue;
			}
			if (SavePackagesSet.Contains(Package))
			{
				continue;
			}
			RemainingDirty.Add(Package);
		}
		AutoDiscardedCount = UeAgentAssetOps::ClearDirtyFlagForPackages(RemainingDirty);
	}

	if (bCloseAllAssetEditors && GEditor)
	{
		if (UAssetEditorSubsystem* AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
		{
			AssetEditorSubsystem->CloseAllAssetEditors();
		}
	}

	TArray<UPackage*> RemainingDirtyPackages;
	UeAgentAssetOps::CollectDirtyPackages(RemainingDirtyPackages);
	UAssetEditorSubsystem* AssetEditorSubsystem = GEditor ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr;
	TArray<TSharedPtr<FJsonValue>> RemainingDirtyResourcesJson;
	UeAgentAssetOps::MakeDirtyResourceArrayJson(RemainingDirtyPackages, EditorWorld, AssetEditorSubsystem, RemainingDirtyResourcesJson);

	OutData->SetNumberField(TEXT("saved_package_count"), SavedPackageCount);
	OutData->SetNumberField(TEXT("explicit_discarded_package_count"), ExplicitDiscardedCount);
	OutData->SetNumberField(TEXT("auto_discarded_package_count"), AutoDiscardedCount);
	OutData->SetBoolField(TEXT("request_exit"), bRequestExit);
	OutData->SetBoolField(TEXT("close_all_asset_editors"), bCloseAllAssetEditors);
	OutData->SetBoolField(TEXT("close_world_browser_tabs"), bCloseWorldBrowserTabs);
	OutData->SetBoolField(TEXT("unload_late_shutdown_editor_modules"), bUnloadLateShutdownEditorModules);
	OutData->SetNumberField(TEXT("exit_delay_seconds"), ExitDelaySeconds);
	OutData->SetNumberField(TEXT("remaining_dirty_resource_count"), RemainingDirtyResourcesJson.Num());
	OutData->SetArrayField(TEXT("dirty_resources"), RemainingDirtyResourcesJson);

	TArray<TSharedPtr<FJsonValue>> SaveNotFoundJson;
	for (const FString& Item : SaveAssetNotFound)
	{
		SaveNotFoundJson.Add(MakeShared<FJsonValueString>(Item));
	}
	OutData->SetArrayField(TEXT("save_asset_not_found"), SaveNotFoundJson);

	TArray<TSharedPtr<FJsonValue>> DiscardNotFoundJson;
	for (const FString& Item : DiscardAssetNotFound)
	{
		DiscardNotFoundJson.Add(MakeShared<FJsonValueString>(Item));
	}
	OutData->SetArrayField(TEXT("discard_asset_not_found"), DiscardNotFoundJson);

	FUeAgentInterfaceLogger::Log(
		TEXT("EditorPrepareExit: save_paths=%d discard_paths=%d saved=%d discard_explicit=%d discard_auto=%d request_exit=%s"),
		SaveAssetPaths.Num(),
		DiscardAssetPaths.Num(),
		SavedPackageCount,
		ExplicitDiscardedCount,
		AutoDiscardedCount,
		bRequestExit ? TEXT("true") : TEXT("false"));

	if (RemainingDirtyResourcesJson.Num() > 0)
	{
		OutError = TEXT("editor_has_unresolved_dirty_resources");
		return false;
	}

	int32 ClosedWorldBrowserTabCount = 0;
	if (bCloseWorldBrowserTabs)
	{
		ClosedWorldBrowserTabCount = UeAgentAssetOps::CloseWorldBrowserTabsForExit();
	}
	OutData->SetNumberField(TEXT("closed_world_browser_tab_count"), ClosedWorldBrowserTabCount);

	TArray<TSharedPtr<FJsonValue>> UnloadedModulesJson;
	int32 UnloadedModuleCount = 0;
	if (bUnloadLateShutdownEditorModules && bRequestExit)
	{
		UnloadedModuleCount = UeAgentAssetOps::UnloadKnownLateShutdownEditorModulesForExit(UnloadedModulesJson);
	}
	OutData->SetNumberField(TEXT("unloaded_late_shutdown_editor_module_count"), UnloadedModuleCount);
	OutData->SetArrayField(TEXT("unloaded_late_shutdown_editor_modules"), UnloadedModulesJson);

	if (bRequestExit)
	{
		UeAgentAssetOps::ScheduleEditorRequestExit(ExitDelaySeconds);
	}

	return true;
}

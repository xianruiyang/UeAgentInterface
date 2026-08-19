// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringAssets.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringDescriptors.h"
#include "Factories/Factory.h"
#include "Misc/PackageName.h"
#include "ObjectTools.h"
#include "UObject/SavePackage.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UObjectIterator.h"

namespace UeAgentReflectionAuthoring
{
	namespace
	{
		UFactory* FindFactory(UClass* AssetClass)
		{
			UClass* BestFactoryClass = nullptr;
			for (TObjectIterator<UClass> It; It; ++It)
			{
				UClass* FactoryClass = *It;
				if (!FactoryClass->IsChildOf(UFactory::StaticClass())
					|| FactoryClass->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
				{
					continue;
				}
				const UFactory* DefaultFactory = FactoryClass->GetDefaultObject<UFactory>();
				UClass* SupportedClass = DefaultFactory ? DefaultFactory->GetSupportedClass() : nullptr;
				if (!DefaultFactory || !DefaultFactory->CanCreateNew() || !SupportedClass
					|| !AssetClass->IsChildOf(SupportedClass))
				{
					continue;
				}
				if (SupportedClass == AssetClass)
				{
					BestFactoryClass = FactoryClass;
					break;
				}
				if (!BestFactoryClass) BestFactoryClass = FactoryClass;
			}
			return BestFactoryClass ? NewObject<UFactory>(GetTransientPackage(), BestFactoryClass) : nullptr;
		}

		void EnsureAssetFlags(UObject* Asset)
		{
			if (!Asset) return;
			Asset->ClearFlags(RF_Transient);
			Asset->SetFlags(RF_Public | RF_Standalone | RF_Transactional);
		}
	}

	FString NormalizePackagePath(const FString& AssetPath)
	{
		FString Result = AssetPath.TrimStartAndEnd();
		Result.ReplaceInline(TEXT("\\"), TEXT("/"));
		if (Result.Contains(TEXT("."))) Result = FPackageName::ObjectPathToPackageName(Result);
		while (Result.EndsWith(TEXT("/"))) Result.LeftChopInline(1);
		return Result;
	}

	UClass* ResolveAssetClass(const FProfile& Profile, const bool bForCreate, FString& OutError)
	{
		const FString& ClassPath = bForCreate ? Profile.CreateClassPath : Profile.AssetClassPath;
		UClass* Class = FSoftClassPath(ClassPath).TryLoadClass<UObject>();
		if (!Class)
		{
			OutError = TEXT("reflection_asset_class_unavailable:") + Profile.TypeId + TEXT(":") + ClassPath;
			return nullptr;
		}
		if (bForCreate && Class->HasAnyClassFlags(CLASS_Abstract))
		{
			OutError = TEXT("reflection_asset_create_class_abstract:") + ClassPath;
			return nullptr;
		}
		OutError.Reset();
		return Class;
	}

	UObject* LoadAsset(const FProfile& Profile, const FString& AssetPath)
	{
		FString Error;
		UClass* ExpectedClass = ResolveAssetClass(Profile, false, Error);
		if (!ExpectedClass) return nullptr;
		const FString PackagePath = NormalizePackagePath(AssetPath);
		if (PackagePath.IsEmpty()) return nullptr;
		const FString ObjectPath = PackagePath + TEXT(".") + FPackageName::GetLongPackageAssetName(PackagePath);
		if (UObject* Existing = FindObject<UObject>(nullptr, *ObjectPath))
		{
			return Existing->IsA(ExpectedClass) ? Existing : nullptr;
		}
		const FAssetData AssetData = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"))
			.Get()
			.GetAssetByObjectPath(FSoftObjectPath(ObjectPath));
		if (!AssetData.IsValid() || !AssetData.IsInstanceOf(ExpectedClass)) return nullptr;
		return AssetData.GetAsset();
	}

	UObject* CreateFactorySeed(
		const FProfile& Profile,
		UPackage* Outer,
		const FName Name,
		const EObjectFlags Flags,
		FString& OutError,
		UClass* RequestedClass)
	{
		if (!Profile.bAllowCreate)
		{
			OutError = TEXT("reflection_asset_create_requires_domain_hook:") + Profile.TypeId;
			return nullptr;
		}
		UClass* AcceptedClass = ResolveAssetClass(Profile, false, OutError);
		if (!AcceptedClass) return nullptr;
		UClass* AssetClass = RequestedClass ? RequestedClass : ResolveAssetClass(Profile, true, OutError);
		if (!AssetClass || !Outer) return nullptr;
		if (!AssetClass->IsChildOf(AcceptedClass))
		{
			OutError = TEXT("reflection_asset_class_not_allowed:") + AssetClass->GetPathName();
			return nullptr;
		}
		if (AssetClass->HasAnyClassFlags(CLASS_Abstract))
		{
			OutError = TEXT("reflection_asset_create_class_abstract:") + AssetClass->GetPathName();
			return nullptr;
		}
		const FName EffectiveName = Name.IsNone() ? MakeUniqueObjectName(Outer, AssetClass) : Name;
		UObject* Asset = nullptr;
		if (UFactory* Factory = FindFactory(AssetClass))
		{
			Asset = Factory->FactoryCreateNew(AssetClass, Outer, EffectiveName, Flags, nullptr, GWarn);
		}
		if (!Asset) Asset = NewObject<UObject>(Outer, AssetClass, EffectiveName, Flags);
		if (!Asset)
		{
			OutError = TEXT("reflection_asset_factory_seed_failed:") + Profile.TypeId;
			return nullptr;
		}
		EnsureAssetFlags(Asset);
		OutError.Reset();
		return Asset;
	}

	UObject* DuplicateAsset(UObject* Source, UObject* Outer, const FName Name, const EObjectFlags Flags)
	{
		if (!Source || !Outer) return nullptr;
		UObject* Result = StaticDuplicateObject(Source, Outer, Name, Flags);
		EnsureAssetFlags(Result);
		return Result;
	}

	UObject* PublishAsset(UObject* Source, const FString& PackagePath, FString& OutError)
	{
		if (!Source)
		{
			OutError = TEXT("reflection_asset_publish_source_missing");
			return nullptr;
		}
		UPackage* Package = CreatePackage(*PackagePath);
		const FString AssetName = FPackageName::GetLongPackageAssetName(PackagePath);
		UObject* Published = DuplicateAsset(Source, Package, FName(*AssetName), RF_AllFlags);
		if (!Published)
		{
			OutError = TEXT("reflection_asset_publish_failed:") + PackagePath;
			return nullptr;
		}
		FAssetRegistryModule::AssetCreated(Published);
		Published->MarkPackageDirty();
		OutError.Reset();
		return Published;
	}

	bool SaveAsset(UObject& Asset, FString& OutError)
	{
		UPackage* Package = Asset.GetOutermost();
		const FString Filename = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Public | RF_Standalone;
		Args.SaveFlags = SAVE_NoError;
		if (!UPackage::SavePackage(Package, &Asset, *Filename, Args))
		{
			OutError = TEXT("reflection_asset_save_failed:") + Package->GetName();
			return false;
		}
		OutError.Reset();
		return true;
	}

	bool DeleteAsset(UObject* Asset, FString& OutError)
	{
		if (!Asset)
		{
			OutError = TEXT("reflection_asset_delete_target_missing");
			return false;
		}
		TGuardValue<bool> UnattendedGuard(GIsRunningUnattendedScript, true);
		if (!ObjectTools::DeleteSingleObject(Asset, true))
		{
			OutError = TEXT("reflection_asset_delete_blocked_by_references:") + Asset->GetPathName();
			return false;
		}
		OutError.Reset();
		return true;
	}
}

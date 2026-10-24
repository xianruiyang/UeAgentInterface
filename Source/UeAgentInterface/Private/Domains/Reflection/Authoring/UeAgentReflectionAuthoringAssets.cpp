// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringAssets.h"
#include "Dom/JsonValue.h"

#include "Core/Contract/UeAgentDiagnostic.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Domains/Reflection/Authoring/UeAgentReflectionAuthoringDescriptors.h"
#include "Factories/Factory.h"
#include "AssetRegistry/AssetData.h"
#include "UObject/Class.h"
#include "Misc/PackageName.h"
#include "ObjectTools.h"
#include "UObject/SavePackage.h"
#include "Core/AssetDurable/UeAgentPackageSave.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UObjectIterator.h"

namespace UeAgentReflectionAuthoring
{
	namespace
	{
		UE::UAI::Contract::FDiagnostic AssetFailure(const TCHAR* Code, const TCHAR* Reason,
			const TCHAR* Stage, const TCHAR* Field, TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected)
		{
			auto Error = UE::UAI::Contract::FDiagnostic::Failure(Code, Reason, Stage);
			Error.SourceKind = TEXT("native_result");
			Error.PropertyPath = Field;
			Error.ActualValue = MoveTemp(Actual);
			Error.ActualValuePresent = Error.ActualValue.IsValid();
			Error.ExpectedValue = MoveTemp(Expected);
			Error.ExpectedValuePresent = Error.ExpectedValue.IsValid();
			return Error;
		}

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

	UClass* ResolveAssetClass(const FProfile& Profile, const bool bForCreate, UE::UAI::Contract::FDiagnostic& OutError)
	{
		const FString& ClassPath = bForCreate ? Profile.CreateClassPath : Profile.AssetClassPath;
		UClass* Class = FSoftClassPath(ClassPath).TryLoadClass<UObject>();
		if (!Class)
		{
			OutError = AssetFailure(TEXT("reflection_asset_class_unavailable"),
				TEXT("The reflection profile asset class could not be loaded."), TEXT("asset_resolution"),
				TEXT("class_path"), MakeShared<FJsonValueString>(ClassPath),
				MakeShared<FJsonValueString>(TEXT("a loadable UObject class")));
			OutError.SubjectId = Profile.TypeId;
			return nullptr;
		}
		if (bForCreate && Class->HasAnyClassFlags(CLASS_Abstract))
		{
			OutError = AssetFailure(TEXT("reflection_asset_create_class_abstract"),
				TEXT("An abstract asset class cannot be instantiated."), TEXT("asset_create"),
				TEXT("class_path"), MakeShared<FJsonValueString>(ClassPath),
				MakeShared<FJsonValueString>(TEXT("a concrete asset class")));
			OutError.SubjectId = Profile.TypeId;
			return nullptr;
		}
		OutError.Reset();
		return Class;
	}

	UObject* LoadAsset(const FProfile& Profile, const FString& AssetPath, UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutError.Reset();
		UClass* ExpectedClass = ResolveAssetClass(Profile, false, OutError);
		if (!ExpectedClass)
		{
			if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = AssetPath;
			return nullptr;
		}
		const FString PackagePath = NormalizePackagePath(AssetPath);
		if (PackagePath.IsEmpty())
		{
			OutError = AssetFailure(TEXT("reflection_asset_path_empty"),
				TEXT("Resolving a reflection asset requires a non-empty asset path."), TEXT("asset_resolution"),
				TEXT("asset_path"), MakeShared<FJsonValueString>(AssetPath),
				MakeShared<FJsonValueString>(TEXT("a non-empty asset package or object path")));
			return nullptr;
		}
		const auto RejectClass = [&](const FString& ActualClass) -> UObject*
		{
			OutError = AssetFailure(TEXT("reflection_asset_type_invalid"),
				TEXT("The asset class is not accepted by the requested reflection profile."), TEXT("asset_resolution"),
				TEXT("asset_class"), MakeShared<FJsonValueString>(ActualClass),
				MakeShared<FJsonValueString>(ExpectedClass->GetPathName()));
			OutError.SubjectId = Profile.TypeId;
			OutError.TargetPath = PackagePath;
			return nullptr;
		};
		const FString ObjectPath = PackagePath + TEXT(".") + FPackageName::GetLongPackageAssetName(PackagePath);
		if (UObject* Existing = FindObject<UObject>(nullptr, *ObjectPath))
		{
			if (!Existing->IsA(ExpectedClass)
				|| (Profile.AcceptsConcreteClass && !Profile.AcceptsConcreteClass(Existing->GetClass())))
				return RejectClass(Existing->GetClass()->GetPathName());
			return Existing;
		}
		const FAssetData AssetData = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"))
			.Get()
			.GetAssetByObjectPath(FSoftObjectPath(ObjectPath));
		// A missing asset remains an ordinary lookup absence for create/upsert callers.
		if (!AssetData.IsValid()) return nullptr;
		if (!AssetData.IsInstanceOf(ExpectedClass)) return RejectClass(AssetData.AssetClassPath.ToString());
		UObject* Asset = AssetData.GetAsset();
		if (!Asset)
		{
			OutError = AssetFailure(TEXT("reflection_asset_load_failed"),
				TEXT("The asset registry contains the target, but Unreal could not load its object."), TEXT("asset_resolution"),
				TEXT("asset_loaded"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			OutError.TargetPath = PackagePath;
			return nullptr;
		}
		if (Profile.AcceptsConcreteClass && !Profile.AcceptsConcreteClass(Asset->GetClass()))
			return RejectClass(Asset->GetClass()->GetPathName());
		return Asset;
	}

	UObject* CreateFactorySeed(
		const FProfile& Profile,
		UPackage* Outer,
		const FName Name,
		const EObjectFlags Flags,
		UE::UAI::Contract::FDiagnostic& OutError,
		UClass* RequestedClass)
	{
		if (!Profile.bAllowCreate)
		{
			OutError = AssetFailure(TEXT("reflection_asset_create_requires_domain_hook"),
				TEXT("This reflection profile requires its domain-specific creation path."), TEXT("asset_create"),
				TEXT("profile_type"), MakeShared<FJsonValueString>(Profile.TypeId),
				MakeShared<FJsonValueString>(TEXT("a profile that permits generic asset creation")));
			return nullptr;
		}
		UClass* AcceptedClass = ResolveAssetClass(Profile, false, OutError);
		if (!AcceptedClass) return nullptr;
		UClass* AssetClass = RequestedClass ? RequestedClass : ResolveAssetClass(Profile, true, OutError);
		if (!AssetClass) return nullptr;
		if (!Outer)
		{
			OutError = AssetFailure(TEXT("reflection_asset_factory_outer_missing"),
				TEXT("Creating the asset seed requires an owning package."), TEXT("asset_create"),
				TEXT("outer_present"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			OutError.SubjectId = Profile.TypeId;
			return nullptr;
		}
		if (!AssetClass->IsChildOf(AcceptedClass))
		{
			OutError = AssetFailure(TEXT("reflection_asset_class_not_allowed"),
				TEXT("The requested creation class is not derived from the profile asset class."), TEXT("asset_create"),
				TEXT("class_path"), MakeShared<FJsonValueString>(AssetClass->GetPathName()),
				MakeShared<FJsonValueString>(AcceptedClass->GetPathName()));
			return nullptr;
		}
		if (AssetClass->HasAnyClassFlags(CLASS_Abstract))
		{
			OutError = AssetFailure(TEXT("reflection_asset_create_class_abstract"),
				TEXT("An abstract asset class cannot be instantiated."), TEXT("asset_create"),
				TEXT("class_path"), MakeShared<FJsonValueString>(AssetClass->GetPathName()),
				MakeShared<FJsonValueString>(TEXT("a concrete asset class")));
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
			OutError = AssetFailure(TEXT("reflection_asset_factory_seed_failed"),
				TEXT("Unreal could not create an asset seed through the selected factory or object allocation."), TEXT("asset_create"),
				TEXT("class_path"), MakeShared<FJsonValueString>(AssetClass->GetPathName()),
				MakeShared<FJsonValueString>(TEXT("a class whose asset seed can be created")));
			OutError.SubjectId = Profile.TypeId;
			OutError.InternalContext = Outer->GetPathName();
			return nullptr;
		}
		EnsureAssetFlags(Asset);
		OutError.Reset();
		return Asset;
	}

	UObject* DuplicateAsset(UObject* Source, UObject* Outer, const FName Name, const EObjectFlags Flags,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutError.Reset();
		if (!Source || !Outer)
		{
			OutError = AssetFailure(TEXT("reflection_asset_duplicate_input_missing"),
				!Source ? TEXT("Asset duplication requires a source object.") : TEXT("Asset duplication requires an owning object."),
				TEXT("asset_duplicate"), !Source ? TEXT("source_present") : TEXT("outer_present"),
				MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			return nullptr;
		}
		UObject* Result = StaticDuplicateObject(Source, Outer, Name, Flags);
		if (!Result)
		{
			OutError = AssetFailure(TEXT("reflection_asset_duplicate_failed"),
				TEXT("Unreal could not duplicate the asset object."), TEXT("asset_duplicate"),
				TEXT("duplicate_created"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			OutError.InternalContext = Source->GetPathName();
			return nullptr;
		}
		EnsureAssetFlags(Result);
		return Result;
	}

	UObject* PublishAsset(UObject* Source, const FString& PackagePath, UE::UAI::Contract::FDiagnostic& OutError)
	{
		if (!Source)
		{
			OutError = AssetFailure(TEXT("reflection_asset_publish_source_missing"),
				TEXT("Publishing the asset requires a prepared source object."), TEXT("asset_publish"),
				TEXT("source_present"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			OutError.TargetPath = PackagePath;
			return nullptr;
		}
		UPackage* Package = CreatePackage(*PackagePath);
		if (!Package)
		{
			OutError = AssetFailure(TEXT("reflection_asset_publish_package_failed"),
				TEXT("Unreal could not create or retrieve the destination asset package."), TEXT("asset_publish"),
				TEXT("package_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			OutError.TargetPath = PackagePath;
			return nullptr;
		}
		const FString AssetName = FPackageName::GetLongPackageAssetName(PackagePath);
		UObject* Published = DuplicateAsset(Source, Package, FName(*AssetName), RF_AllFlags, OutError);
		if (!Published)
		{
			if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = PackagePath;
			return nullptr;
		}
		FAssetRegistryModule::AssetCreated(Published);
		Published->MarkPackageDirty();
		OutError.Reset();
		return Published;
	}



	bool SaveAsset(UObject& Asset, UE::UAI::Contract::FDiagnostic& OutError)
	{
		UPackage* Package = Asset.GetOutermost();
		const FString Filename = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Public | RF_Standalone;
		UE::UAI::Contract::FDiagnostic SaveDetail;
		if (!UE::UAI::AssetDurable::TrySaveLoadedPackage(*Package, &Asset, Filename, Args, SaveDetail))
		{
			OutError = SaveDetail;
			return false;
		}
		OutError.Reset();
		return true;
	}

	bool DeleteAsset(UObject* Asset, const FString& Target, UE::UAI::Contract::FDiagnostic& OutError)
	{
		if (!Asset) {
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("reflection_asset_delete_target_missing"),
				TEXT("No asset object selected by the reflection profile was supplied for deletion."), TEXT("asset_delete"));
			OutError.SourceKind = TEXT("native_result"); OutError.TargetPath = Target; OutError.PropertyPath = TEXT("asset");
			OutError.ActualValuePresent = true; OutError.ActualValue = MakeShared<FJsonValueNull>();
			OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("asset object selected by the reflection profile"));
			return false;
		}
		TGuardValue<bool> UnattendedGuard(GIsRunningUnattendedScript, true);
		if (!ObjectTools::DeleteSingleObject(Asset, true)) {
			OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("reflection_asset_delete_rejected"),
				TEXT("The editor rejected deletion of the asset object selected by the reflection profile."), TEXT("asset_delete"),
				TEXT("DeleteSingleObject returned false with reference checking enabled; its bool API does not distinguish deletion-policy and reference-check rejection."));
			OutError.SourceKind = TEXT("native_result"); OutError.TargetPath = Target; OutError.PropertyPath = TEXT("deletion_succeeded");
			OutError.ActualValuePresent = true; OutError.ActualValue = MakeShared<FJsonValueBoolean>(false);
			OutError.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
			return false;
		}
		OutError.Reset();
		return true;
	}
}

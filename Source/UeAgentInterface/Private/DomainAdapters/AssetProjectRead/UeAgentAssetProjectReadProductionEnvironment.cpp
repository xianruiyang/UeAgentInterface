#include "DomainAdapters/AssetProjectRead/UeAgentAssetProjectReadProductionEnvironment.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Editor.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "GeneralProjectSettings.h"
#include "Misc/ConfigCacheIni.h"
#include "Modules/ModuleManager.h"
#include "UObject/UObjectIterator.h"

namespace UE::UAI::DomainAdapters::AssetProjectRead
{
	namespace
	{
		bool Fail(
			FUeAgentAssetProjectReadError& OutError,
			FString Code,
			FString SubjectId,
			FString Detail = FString())
		{
			OutError.Code = MoveTemp(Code);
			OutError.SubjectId = MoveTemp(SubjectId);
			OutError.Detail = MoveTemp(Detail);
			return false;
		}

		UObject* ResolveObject(const FString& PublicRef)
		{
			FString ObjectPath = PublicRef;
			ObjectPath.TrimStartAndEndInline();
			if (ObjectPath.IsEmpty())
			{
				return nullptr;
			}
			if (UObject* Existing = StaticFindObject(
				UObject::StaticClass(),
				nullptr,
				*ObjectPath))
			{
				return Existing;
			}
			return StaticLoadObject(
				UObject::StaticClass(),
				nullptr,
				*ObjectPath);
		}

		const FString* ConfigFilename(const FString& Scope)
		{
			if (Scope == TEXT("engine"))
			{
				return &GEngineIni;
			}
			if (Scope == TEXT("game")
				|| Scope == TEXT("platform"))
			{
				return &GGameIni;
			}
			if (Scope == TEXT("editor"))
			{
				return &GEditorIni;
			}
			if (Scope == TEXT("input"))
			{
				return &GInputIni;
			}
			if (Scope == TEXT("device_profiles"))
			{
				return &GDeviceProfilesIni;
			}
			if (Scope == TEXT("scalability"))
			{
				return &GScalabilityIni;
			}
			return nullptr;
		}
	}

	bool FUeAgentAssetProjectReadProductionEnvironment::
		TryResolveAsset(
			const UE::UAI::Discovery::FUeAgentCandidateBinding& Target,
			UObject*& OutAsset,
			FUeAgentAssetProjectReadError& OutError)
	{
		OutError = {};
		OutAsset = ResolveObject(Target.GetPublicRef());
		return IsValid(OutAsset)
			? true
			: Fail(
				OutError,
				TEXT("asset_project_read_asset_unresolved"),
				Target.GetPublicRef());
	}

	bool FUeAgentAssetProjectReadProductionEnvironment::
		TryResolveRegistryRecord(
			const UE::UAI::Discovery::FUeAgentCandidateBinding& Target,
			FAssetData& OutRecord,
			FUeAgentAssetProjectReadError& OutError)
	{
		OutError = {};
		FAssetRegistryModule& Module =
			FModuleManager::LoadModuleChecked<
				FAssetRegistryModule>(TEXT("AssetRegistry"));
		OutRecord = Module.Get().GetAssetByObjectPath(
			FSoftObjectPath(Target.GetPublicRef()));
		return OutRecord.IsValid()
			? true
			: Fail(
				OutError,
				TEXT("asset_project_read_registry_unresolved"),
				Target.GetPublicRef());
	}

	bool FUeAgentAssetProjectReadProductionEnvironment::
		TryResolveRegistryRelations(
			const FAssetData& Record,
			const FString& Direction,
			const TArray<FString>& RelationKinds,
			TArray<FString>& OutDependencies,
			TArray<FString>& OutReferencers,
			FUeAgentAssetProjectReadError& OutError)
	{
		OutDependencies.Reset();
		OutReferencers.Reset();
		OutError = {};
		if (!Record.IsValid()
			|| (Direction != TEXT("dependencies")
				&& Direction != TEXT("referencers")
				&& Direction != TEXT("both")))
		{
			return Fail(
				OutError,
				TEXT("asset_project_read_relation_input_invalid"),
				Record.GetObjectPathString(),
				Direction);
		}

		FAssetRegistryModule& Module =
			FModuleManager::LoadModuleChecked<FAssetRegistryModule>(
				TEXT("AssetRegistry"));
		IAssetRegistry& Registry = Module.Get();
		TSet<FName> Dependencies;
		TSet<FName> Referencers;
		auto Append = [&Record, &Direction, &Registry, &Dependencies, &Referencers](
			const UE::AssetRegistry::EDependencyCategory Category,
			const UE::AssetRegistry::FDependencyQuery& Query)
		{
			TArray<FName> Values;
			if (Direction == TEXT("dependencies") || Direction == TEXT("both"))
			{
				Registry.GetDependencies(
					Record.PackageName,
					Values,
					Category,
					Query);
				Dependencies.Append(Values);
			}
			Values.Reset();
			if (Direction == TEXT("referencers") || Direction == TEXT("both"))
			{
				Registry.GetReferencers(
					Record.PackageName,
					Values,
					Category,
					Query);
				Referencers.Append(Values);
			}
		};

		const TArray<FString> Kinds = RelationKinds.IsEmpty()
			? TArray<FString>{TEXT("hard"), TEXT("soft"), TEXT("manage"), TEXT("searchable_name")}
			: RelationKinds;
		for (const FString& Kind : Kinds)
		{
			if (Kind == TEXT("hard"))
			{
				Append(
					UE::AssetRegistry::EDependencyCategory::Package,
					UE::AssetRegistry::FDependencyQuery(
						UE::AssetRegistry::EDependencyQuery::Hard));
			}
			else if (Kind == TEXT("soft"))
			{
				Append(
					UE::AssetRegistry::EDependencyCategory::Package,
					UE::AssetRegistry::FDependencyQuery(
						UE::AssetRegistry::EDependencyQuery::NotHard));
			}
			else if (Kind == TEXT("manage"))
			{
				Append(
					UE::AssetRegistry::EDependencyCategory::Manage,
					UE::AssetRegistry::FDependencyQuery());
			}
			else if (Kind == TEXT("searchable_name"))
			{
				Append(
					UE::AssetRegistry::EDependencyCategory::SearchableName,
					UE::AssetRegistry::FDependencyQuery());
			}
			else
			{
				return Fail(
					OutError,
					TEXT("asset_project_read_relation_kind_invalid"),
					Record.GetObjectPathString(),
					Kind);
			}
		}
		for (const FName Name : Dependencies)
		{
			OutDependencies.Add(Name.ToString());
		}
		for (const FName Name : Referencers)
		{
			OutReferencers.Add(Name.ToString());
		}
		OutDependencies.Sort();
		OutReferencers.Sort();
		return true;
	}

	bool FUeAgentAssetProjectReadProductionEnvironment::
		TryResolveEditorResources(
			const UE::UAI::ContextResource::
				FUeAgentExecutionContextRef& Context,
			TArray<UPackage*>& OutPackages,
			FUeAgentAssetProjectReadError& OutError)
	{
		OutPackages.Reset();
		OutError = {};
		TSet<UPackage*> Packages;
		if (GEditor)
		{
			if (UAssetEditorSubsystem* Subsystem =
				GEditor->GetEditorSubsystem<
					UAssetEditorSubsystem>())
			{
				for (UObject* Asset
					: Subsystem->GetAllEditedAssets())
				{
					if (IsValid(Asset))
					{
						Packages.Add(Asset->GetOutermost());
					}
				}
			}
		}
		for (TObjectIterator<UPackage> It; It; ++It)
		{
			UPackage* Package = *It;
			if (IsValid(Package) && Package->IsDirty())
			{
				Packages.Add(Package);
			}
		}
		OutPackages = Packages.Array();
		OutPackages.Sort(
			[](const UPackage& Left, const UPackage& Right)
			{
				return Left.GetName() < Right.GetName();
			});
		return true;
	}

	bool FUeAgentAssetProjectReadProductionEnvironment::
		TryResolveProjectObject(
			const UE::UAI::ContextResource::
				FUeAgentExecutionContextRef& Context,
			const FString& SourceId,
			UObject*& OutObject,
			FUeAgentAssetProjectReadError& OutError)
	{
		OutError = {};
		OutObject = GetMutableDefault<
			UGeneralProjectSettings>();
		return IsValid(OutObject)
			? true
			: Fail(
				OutError,
				TEXT("asset_project_read_project_unresolved"),
				Context.GetProjectInstanceId(),
				SourceId);
	}

	bool FUeAgentAssetProjectReadProductionEnvironment::
		TryResolveConfig(
			const UE::UAI::ContextResource::
				FUeAgentExecutionContextRef& Context,
			const FString& ConfigScope,
			const FConfigFile*& OutConfig,
			FUeAgentAssetProjectReadError& OutError)
	{
		OutConfig = nullptr;
		OutError = {};
		const FString* Filename =
			ConfigFilename(ConfigScope);
		if (GConfig && Filename)
		{
			OutConfig = GConfig->FindConfigFile(*Filename);
		}
		return OutConfig
			? true
			: Fail(
				OutError,
				TEXT("asset_project_read_config_unresolved"),
				ConfigScope,
				Context.GetProjectInstanceId());
	}
}

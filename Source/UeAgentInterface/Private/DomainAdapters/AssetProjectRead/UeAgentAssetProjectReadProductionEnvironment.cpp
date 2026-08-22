#include "DomainAdapters/AssetProjectRead/UeAgentAssetProjectReadProductionEnvironment.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Editor.h"
#include "Engine/Level.h"
#include "Engine/MapBuildDataRegistry.h"
#include "Engine/World.h"
#include "GeneralProjectSettings.h"
#include "Interfaces/IProjectManager.h"
#include "Interfaces/ITargetPlatform.h"
#include "Interfaces/ITargetPlatformManagerModule.h"
#include "Misc/ConfigCacheIni.h"
#include "Modules/ModuleManager.h"
#include "ProjectDescriptor.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "UObject/Package.h"
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

		bool IncludesResourceKind(
			const TSet<FString>& ResourceKinds,
			const FString& ResourceKind)
		{
			return ResourceKinds.IsEmpty()
				|| ResourceKinds.Contains(ResourceKind);
		}

		void SortEditorResources(
			TArray<FUeAgentEditorResourceObservation>& Resources)
		{
			Resources.Sort(
				[](const FUeAgentEditorResourceObservation& Left,
					const FUeAgentEditorResourceObservation& Right)
				{
					return Left.ResourceId == Right.ResourceId
						? Left.ObjectPath < Right.ObjectPath
						: Left.ResourceId < Right.ResourceId;
				});
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

	bool FUeAgentAssetProjectReadProductionEnvironment::TryResolveOpenAssets(
			const UE::UAI::ContextResource::
				FUeAgentExecutionContextRef& Context,
			const bool bIncludeEditorIdentity,
			TArray<FUeAgentEditorResourceObservation>& OutResources,
			FUeAgentAssetProjectReadError& OutError)
	{
		OutResources.Reset();
		OutError = {};
		if (GEditor)
		{
			if (UAssetEditorSubsystem* Subsystem =
				GEditor->GetEditorSubsystem<
					UAssetEditorSubsystem>())
			{
				for (UObject* Asset : Subsystem->GetAllEditedAssets())
				{
					if (!IsValid(Asset) || !IsValid(Asset->GetOutermost()))
					{
						continue;
					}
					UPackage* Package = Asset->GetOutermost();
					FUeAgentEditorResourceObservation Observation;
					Observation.ResourceId = Asset->GetPathName();
					Observation.ResourceKind = Package->HasAnyPackageFlags(
						PKG_ContainsMap) ? TEXT("map") : TEXT("asset");
					Observation.PackageName = Package->GetName();
					Observation.ObjectPath = Asset->GetPathName();
					Observation.ClassPath = Asset->GetClass()->GetPathName();
					Observation.EditorIdentity = bIncludeEditorIdentity
						? Context.GetEditorInstanceId() : FString();
					Observation.bDirty = Package->IsDirty();
					OutResources.Add(MoveTemp(Observation));
				}
			}
		}
		SortEditorResources(OutResources);
		return true;
	}

	bool FUeAgentAssetProjectReadProductionEnvironment::TryResolveDirtyResources(
		const UE::UAI::ContextResource::FUeAgentExecutionContextRef& Context,
		const TArray<FString>& ResourceKinds,
		TArray<FUeAgentEditorResourceObservation>& OutResources,
		FUeAgentAssetProjectReadError& OutError)
	{
		OutResources.Reset();
		OutError = {};
		TSet<FString> RequestedKinds;
		RequestedKinds.Append(ResourceKinds);
		for (const FString& Kind : RequestedKinds)
		{
			if (Kind != TEXT("asset") && Kind != TEXT("map")
				&& Kind != TEXT("config"))
			{
				return Fail(
					OutError,
					TEXT("asset_project_read_resource_kind_unsupported"),
					Kind);
			}
		}
		TSet<FString> SeenResourceIds;
		auto AppendDirtyPackage = [
			&OutResources,
			&SeenResourceIds](
			UPackage* Package,
			const FString& Kind,
			bool bAllowTransientPackage)
		{
			if (!IsValid(Package)
				|| !Package->IsDirty()
				|| (!bAllowTransientPackage
					&& Package->HasAnyFlags(RF_Transient))
				|| Package->HasAnyPackageFlags(
					PKG_CompiledIn | PKG_PlayInEditor))
			{
				return;
			}
			const FString PackageName = Package->GetName();
			if (!PackageName.StartsWith(TEXT("/Game/"))
				|| SeenResourceIds.Contains(PackageName))
			{
				return;
			}
			SeenResourceIds.Add(PackageName);
			FUeAgentEditorResourceObservation Observation;
			Observation.ResourceId = PackageName;
			Observation.ResourceKind = Kind;
			Observation.PackageName = PackageName;
			Observation.bDirty = true;
			OutResources.Add(MoveTemp(Observation));
		};

		if (IncludesResourceKind(RequestedKinds, TEXT("map")))
		{
			for (TObjectIterator<UWorld> It; It; ++It)
			{
				UWorld* World = *It;
				if (!IsValid(World))
				{
					continue;
				}
				AppendDirtyPackage(
					World->GetOutermost(),
					TEXT("map"),
					false);
				ULevel* Level = World->PersistentLevel;
				if (!Level)
				{
					continue;
				}
				if (Level->MapBuildData)
				{
					AppendDirtyPackage(
						Level->MapBuildData->GetOutermost(),
						TEXT("map"),
						false);
				}
				for (UPackage* ExternalPackage
					: Level->GetLoadedExternalObjectPackages())
				{
					AppendDirtyPackage(
						ExternalPackage,
						TEXT("map"),
						true);
				}
			}
		}

		if (IncludesResourceKind(RequestedKinds, TEXT("asset")))
		{
			for (TObjectIterator<UPackage> It; It; ++It)
			{
				UPackage* Package = *It;
				if (SeenResourceIds.Contains(Package->GetName())
					|| Package->HasAnyPackageFlags(PKG_ContainsMap))
				{
					continue;
				}
				AppendDirtyPackage(
					Package,
					TEXT("asset"),
					false);
			}
		}

		if (IncludesResourceKind(RequestedKinds, TEXT("config")) && GConfig)
		{
			struct FConfigCandidate
			{
				const TCHAR* Scope;
				const FString* Filename;
			};
			const FConfigCandidate Candidates[] = {
				{TEXT("engine"), &GEngineIni},
				{TEXT("game"), &GGameIni},
				{TEXT("editor"), &GEditorIni},
				{TEXT("input"), &GInputIni},
				{TEXT("device_profiles"), &GDeviceProfilesIni},
				{TEXT("scalability"), &GScalabilityIni},
			};
			for (const FConfigCandidate& Candidate : Candidates)
			{
				const FConfigFile* Config = GConfig->FindConfigFile(
					*Candidate.Filename);
				if (!Config || !Config->Dirty)
				{
					continue;
				}
				FUeAgentEditorResourceObservation Observation;
				Observation.ResourceId = FString(TEXT("config:"))
					+ Candidate.Scope;
				Observation.ResourceKind = TEXT("config");
				Observation.ObjectPath = *Candidate.Filename;
				Observation.bDirty = true;
				OutResources.Add(MoveTemp(Observation));
			}
		}
		SortEditorResources(OutResources);
		return true;
	}

	bool FUeAgentAssetProjectReadProductionEnvironment::TryResolveProjectPlatforms(
		const UE::UAI::ContextResource::FUeAgentExecutionContextRef& Context,
		const TArray<FString>& PlatformIds,
		TArray<FUeAgentProjectPlatformObservation>& OutPlatforms,
		FUeAgentAssetProjectReadError& OutError)
	{
		OutPlatforms.Reset();
		OutError = {};
		ITargetPlatformManagerModule& Manager =
			FModuleManager::LoadModuleChecked<ITargetPlatformManagerModule>(
				TEXT("TargetPlatform"));
		TSet<FString> RequestedIds;
		RequestedIds.Append(PlatformIds);
		TSet<FString> MatchedIds;
		const FProjectDescriptor* Project =
			IProjectManager::Get().GetCurrentProject();
		const bool bProjectHasCode = Project && !Project->Modules.IsEmpty();
		for (ITargetPlatform* Platform : Manager.GetTargetPlatforms())
		{
			if (!Platform)
			{
				continue;
			}
			const FString PlatformName = Platform->PlatformName();
			const FString IniPlatformName = Platform->IniPlatformName();
			bool bRequested = RequestedIds.IsEmpty();
			for (const FString& RequestedId : RequestedIds)
			{
				if (RequestedId.Equals(PlatformName, ESearchCase::IgnoreCase)
					|| RequestedId.Equals(
						IniPlatformName, ESearchCase::IgnoreCase))
				{
					bRequested = true;
					MatchedIds.Add(RequestedId);
				}
			}
			if (!bRequested)
			{
				continue;
			}
			FUeAgentProjectPlatformObservation Observation;
			Observation.PlatformName = PlatformName;
			Observation.IniPlatformName = IniPlatformName;
			Observation.DisplayName = Platform->DisplayName().ToString();
			Observation.bRunningPlatform = Platform->IsRunningPlatform();
			Observation.bSdkInstalled = Platform->IsSdkInstalled(
				bProjectHasCode, Observation.SdkDocumentationPath);
			OutPlatforms.Add(MoveTemp(Observation));
		}
		for (const FString& RequestedId : RequestedIds)
		{
			if (!MatchedIds.Contains(RequestedId))
			{
				return Fail(
					OutError,
					TEXT("asset_project_read_platform_unresolved"),
					RequestedId,
					Context.GetProjectInstanceId());
			}
		}
		OutPlatforms.Sort(
			[](const FUeAgentProjectPlatformObservation& Left,
				const FUeAgentProjectPlatformObservation& Right)
			{
				return Left.PlatformName < Right.PlatformName;
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

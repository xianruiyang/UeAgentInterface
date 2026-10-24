#include "DomainAdapters/AssetProjectRead/UeAgentAssetProjectReadProductionEnvironment.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringAssetIdentity.h"
#include "Utilities/EditorResources/UeAgentEditorResourcePersistence.h"
#include "DomainAdapters/EditorProject/UeAgentSettingsDiscovery.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Dom/JsonValue.h"
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
		UE::UAI::Contract::FDiagnostic LookupFailure(const FString& Code, const FString& Reason,
			const FString& Field, const TSharedPtr<FJsonValue>& Actual,
			const TSharedPtr<FJsonValue>& Expected, const FString& PublicTarget = FString())
		{
			auto Error = UE::UAI::Contract::FDiagnostic::Failure(Code, Reason, TEXT("asset_project_read"));
			Error.SourceKind = TEXT("native_result");
			Error.PropertyPath = Field;
			Error.TargetPath = PublicTarget;
			Error.ActualValue = Actual;
			Error.ExpectedValue = Expected;
			Error.ActualValuePresent = Actual.IsValid();
			Error.ExpectedValuePresent = Expected.IsValid();
			return Error;
		}

        bool Fail(UE::UAI::Contract::FDiagnostic& OutError, const UE::UAI::Contract::FDiagnostic& Diagnostic)
        {
            OutError = Diagnostic;
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
			if (!ObjectPath.Contains(TEXT(".")))
				ObjectPath = UeAgentAuthoringAssetIdentity::ToObjectPath(ObjectPath);
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

		FUeAgentAssetRelationNode ResolveRelationNode(
			IAssetRegistry& Registry,
			const FAssetIdentifier& Identifier)
		{
			FUeAgentAssetRelationNode Node;
			Node.StableIdentifier = Identifier.ToString();
			Node.PackageName = Identifier.PackageName.ToString();
			TArray<FAssetData> Assets;
			if (!Identifier.PackageName.IsNone())
			{
				Registry.GetAssetsByPackageName(
					Identifier.PackageName,
					Assets,
					true);
			}
			const FAssetData* ResolvedAsset = nullptr;
			if (!Identifier.ObjectName.IsNone())
			{
				ResolvedAsset = Assets.FindByPredicate(
					[&Identifier](const FAssetData& Asset)
					{
						return Asset.AssetName == Identifier.ObjectName;
					});
			}
			else if (Assets.Num() == 1)
			{
				ResolvedAsset = &Assets[0];
			}
			Node.bResolved = Identifier.ObjectName.IsNone()
				? !Assets.IsEmpty()
				: ResolvedAsset != nullptr;
			if (ResolvedAsset)
			{
				Node.ObjectPath = ResolvedAsset->GetSoftObjectPath().ToString();
				Node.bRedirector = ResolvedAsset->AssetClassPath.ToString().EndsWith(
					TEXT("ObjectRedirector"));
				if (Node.bRedirector)
				{
					ResolvedAsset->GetTagValue(
						FName(TEXT("DestinationObject")),
						Node.RedirectTarget);
				}
			}
			return Node;
		}

		UE::AssetRegistry::FDependencyQuery MakePackageQuery(
			const EUeAgentAssetRelationKind Kind,
			const EUeAgentAssetRelationPackageScope Scope)
		{
			using UE::AssetRegistry::EDependencyQuery;
			EDependencyQuery Flags = Kind == EUeAgentAssetRelationKind::Hard
				? EDependencyQuery::Hard
				: EDependencyQuery::NotHard;
			switch (Scope)
			{
			case EUeAgentAssetRelationPackageScope::Game:
				Flags |= EDependencyQuery::Game;
				break;
			case EUeAgentAssetRelationPackageScope::EditorOnly:
				Flags |= EDependencyQuery::NotGame;
				break;
			case EUeAgentAssetRelationPackageScope::Build:
				Flags |= EDependencyQuery::Build;
				break;
			default:
				break;
			}
			return UE::AssetRegistry::FDependencyQuery(Flags);
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
			: Fail(OutError, LookupFailure(TEXT("asset_project_read_asset_unresolved"),
				TEXT("The selected asset could not be resolved to a live object."), TEXT("/asset/resolved"),
				MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Target.GetPublicRef()));
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
			: Fail(OutError, LookupFailure(TEXT("asset_project_read_registry_unresolved"),
				TEXT("The asset registry has no valid record for the selected asset."), TEXT("/registry/record/available"),
				MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), Target.GetPublicRef()));
	}

	bool FUeAgentAssetProjectReadProductionEnvironment::
		TryResolveRegistryRelations(
			const FAssetData& Record,
			const FUeAgentAssetRelationGraphRequest& Request,
			FUeAgentAssetRelationGraphResult& OutGraph,
			FUeAgentAssetProjectReadError& OutError)
	{
		OutGraph = {};
		OutError = {};
		const bool bDirectionValid =
			Request.Direction == EUeAgentAssetRelationDirection::Dependencies
			|| Request.Direction == EUeAgentAssetRelationDirection::Referencers
			|| Request.Direction == EUeAgentAssetRelationDirection::Both;
		const bool bPackageScopeValid =
			Request.PackageScope == EUeAgentAssetRelationPackageScope::All
			|| Request.PackageScope == EUeAgentAssetRelationPackageScope::Game
			|| Request.PackageScope == EUeAgentAssetRelationPackageScope::EditorOnly
			|| Request.PackageScope == EUeAgentAssetRelationPackageScope::Build;
		if (!Record.IsValid() || !bDirectionValid || !bPackageScopeValid)
		{
			const bool bRecord = Record.IsValid();
			OutError = LookupFailure(TEXT("asset_project_read_relation_input_invalid"),
				!bRecord ? TEXT("Registry relation traversal requires a valid root asset record.")
					: !bDirectionValid ? TEXT("The registry relation direction is not supported.")
					: TEXT("The registry relation package scope is not supported."),
				!bRecord ? TEXT("/registry/record/valid") : !bDirectionValid ? TEXT("/direction") : TEXT("/package_scope"),
				!bRecord ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueBoolean>(false))
					: TSharedPtr<FJsonValue>(MakeShared<FJsonValueNumber>(!bDirectionValid
						? static_cast<int32>(Request.Direction) : static_cast<int32>(Request.PackageScope))),
				!bRecord ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueBoolean>(true))
					: TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(!bDirectionValid
						? TEXT("dependencies, referencers or both") : TEXT("all, game, editor_only or build"))),
				Record.GetObjectPathString());
			return false;
		}

		FAssetRegistryModule& Module =
			FModuleManager::LoadModuleChecked<FAssetRegistryModule>(
				TEXT("AssetRegistry"));
		IAssetRegistry& Registry = Module.Get();
		OutGraph.bRegistryLoading = Registry.IsLoadingAssets();
		OutGraph.bComplete = !OutGraph.bRegistryLoading;
		const FAssetIdentifier RootIdentifier(Record.PackageName);
		OutGraph.Root = ResolveRelationNode(Registry, RootIdentifier);
		OutGraph.Root.bResolved = true;
		OutGraph.Root.ObjectPath = Record.GetSoftObjectPath().ToString();
		OutGraph.Root.bRedirector = Record.AssetClassPath.ToString().EndsWith(
			TEXT("ObjectRedirector"));
		if (OutGraph.Root.bRedirector)
		{
			Record.GetTagValue(
				FName(TEXT("DestinationObject")),
				OutGraph.Root.RedirectTarget);
		}
		OutGraph.Nodes.Add(OutGraph.Root);

		const TArray<EUeAgentAssetRelationKind> Kinds =
			Request.RelationKinds.IsEmpty()
			? TArray<EUeAgentAssetRelationKind>{
				EUeAgentAssetRelationKind::Hard,
				EUeAgentAssetRelationKind::Soft,
				EUeAgentAssetRelationKind::Manage,
				EUeAgentAssetRelationKind::SearchableName}
			: Request.RelationKinds;
		for (const EUeAgentAssetRelationKind Kind : Kinds)
		{
			if (Kind != EUeAgentAssetRelationKind::Hard
				&& Kind != EUeAgentAssetRelationKind::Soft
				&& Kind != EUeAgentAssetRelationKind::Manage
				&& Kind != EUeAgentAssetRelationKind::SearchableName)
			{
				return Fail(OutError, LookupFailure(TEXT("asset_project_read_relation_kind_invalid"),
					TEXT("The requested native asset relation kind is not supported."), TEXT("/relation_kinds"),
					MakeShared<FJsonValueNumber>(static_cast<int32>(Kind)),
					MakeShared<FJsonValueString>(TEXT("hard, soft, manage or searchable name")), Record.GetObjectPathString()));
			}
		}
		struct FTraversalItem
		{
			FAssetIdentifier Identifier;
			int32 Depth = 0;
			TArray<FString> Path;
		};
		TSet<FString> EdgeKeys;
		TSet<FString> NodeKeys{OutGraph.Root.StableIdentifier};
		auto Traverse = [&](const bool bDependencies)
		{
			TArray<FTraversalItem> Queue;
			Queue.Add({RootIdentifier, 0, {RootIdentifier.ToString()}});
			TSet<FString> Expanded;
			for (int32 QueueIndex = 0; QueueIndex < Queue.Num(); ++QueueIndex)
			{
				const FTraversalItem Current = Queue[QueueIndex];
				if (Expanded.Contains(Current.Identifier.ToString()))
				{
					continue;
				}
				Expanded.Add(Current.Identifier.ToString());
				for (const EUeAgentAssetRelationKind Kind : Kinds)
				{
					using UE::AssetRegistry::EDependencyCategory;
					const EDependencyCategory Category =
						(Kind == EUeAgentAssetRelationKind::Hard
							|| Kind == EUeAgentAssetRelationKind::Soft)
						? EDependencyCategory::Package
						: Kind == EUeAgentAssetRelationKind::Manage
							? EDependencyCategory::Manage
							: EDependencyCategory::SearchableName;
					const UE::AssetRegistry::FDependencyQuery Query =
						Category == EDependencyCategory::Package
						? MakePackageQuery(Kind, Request.PackageScope)
						: UE::AssetRegistry::FDependencyQuery();
					TArray<FAssetDependency> Relations;
					if (bDependencies)
					{
						Registry.GetDependencies(
							Current.Identifier, Relations, Category, Query);
					}
					else
					{
						Registry.GetReferencers(
							Current.Identifier, Relations, Category, Query);
					}
					for (const FAssetDependency& Relation : Relations)
					{
						const FAssetIdentifier Source = bDependencies
							? Current.Identifier : Relation.AssetId;
						const FAssetIdentifier Target = bDependencies
							? Relation.AssetId : Current.Identifier;
						const FAssetIdentifier Next = Relation.AssetId;
						const FString TraversalDirection = bDependencies
							? TEXT("dependencies") : TEXT("referencers");
						const FString EdgeKey = Source.ToString() + TEXT("\n")
							+ Target.ToString() + TEXT("\n")
							+ FString::FromInt(static_cast<int32>(Relation.Category))
							+ TEXT("\n")
							+ FString::FromInt(static_cast<int32>(Relation.Properties))
							+ TEXT("\n") + TraversalDirection;
						TArray<FString> NextPath = Current.Path;
						const bool bCycle = NextPath.Contains(Next.ToString());
						NextPath.Add(Next.ToString());
						const FUeAgentAssetRelationNode SourceNode =
							ResolveRelationNode(Registry, Source);
						const FUeAgentAssetRelationNode TargetNode =
							ResolveRelationNode(Registry, Target);
						if (!NodeKeys.Contains(SourceNode.StableIdentifier))
						{
							NodeKeys.Add(SourceNode.StableIdentifier);
							OutGraph.Nodes.Add(SourceNode);
						}
						if (!NodeKeys.Contains(TargetNode.StableIdentifier))
						{
							NodeKeys.Add(TargetNode.StableIdentifier);
							OutGraph.Nodes.Add(TargetNode);
						}
						if (!EdgeKeys.Contains(EdgeKey))
						{
							EdgeKeys.Add(EdgeKey);
							FUeAgentAssetRelationEdge Edge;
							Edge.SourceIdentifier = SourceNode.StableIdentifier;
							Edge.SourcePackageName = SourceNode.PackageName;
							Edge.SourceObjectPath = SourceNode.ObjectPath;
							Edge.TargetIdentifier = TargetNode.StableIdentifier;
							Edge.TargetPackageName = TargetNode.PackageName;
							Edge.TargetObjectPath = TargetNode.ObjectPath;
							Edge.TraversalDirection = TraversalDirection;
							Edge.Depth = Current.Depth + 1;
							Edge.TraversalPath = NextPath;
							Edge.bCycle = bCycle;
							Edge.Category = Relation.Category == EDependencyCategory::Package
								? TEXT("package")
								: Relation.Category == EDependencyCategory::Manage
									? TEXT("manage") : TEXT("searchable_name");
							Edge.CategoryFlags = static_cast<int32>(Relation.Category);
							Edge.PropertyFlags = static_cast<int32>(Relation.Properties);
							Edge.bHard = EnumHasAnyFlags(
								Relation.Properties,
								UE::AssetRegistry::EDependencyProperty::Hard);
							Edge.bGame = EnumHasAnyFlags(
								Relation.Properties,
								UE::AssetRegistry::EDependencyProperty::Game);
							Edge.bBuild = EnumHasAnyFlags(
								Relation.Properties,
								UE::AssetRegistry::EDependencyProperty::Build);
							Edge.bDirect = EnumHasAnyFlags(
								Relation.Properties,
								UE::AssetRegistry::EDependencyProperty::Direct);
							Edge.bSourceResolved = SourceNode.bResolved;
							Edge.bTargetResolved = TargetNode.bResolved;
							Edge.bSourceRedirector = SourceNode.bRedirector;
							Edge.bTargetRedirector = TargetNode.bRedirector;
							Edge.SourceRedirectTarget = SourceNode.RedirectTarget;
							Edge.TargetRedirectTarget = TargetNode.RedirectTarget;
							OutGraph.Edges.Add(MoveTemp(Edge));
						}
						const FUeAgentAssetRelationNode NextNode =
							bDependencies ? TargetNode : SourceNode;
						if (!bCycle && NextNode.bResolved)
						{
							Queue.Add({Next, Current.Depth + 1, MoveTemp(NextPath)});
						}
					}
				}
			}
		};
		if (Request.Direction != EUeAgentAssetRelationDirection::Referencers)
		{
			Traverse(true);
		}
		if (Request.Direction != EUeAgentAssetRelationDirection::Dependencies)
		{
			Traverse(false);
		}
		OutGraph.Nodes.Sort([](const FUeAgentAssetRelationNode& Left,
			const FUeAgentAssetRelationNode& Right)
		{
			return Left.StableIdentifier < Right.StableIdentifier;
		});
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
				return Fail(OutError, LookupFailure(TEXT("asset_project_read_resource_kind_unsupported"),
					TEXT("The requested dirty resource kind is not supported."), TEXT("/resource_kinds"),
					MakeShared<FJsonValueString>(Kind), MakeShared<FJsonValueString>(TEXT("asset, map or config"))));
			}
		}
		TArray<EditorResources::FDirtyResource> Resources;
		EditorResources::CollectDirtyResources(Resources);
		for (const auto& Resource : Resources)
		{
			if (!IncludesResourceKind(RequestedKinds, Resource.ResourceKind)) continue;
			FUeAgentEditorResourceObservation Observation;
			Observation.ResourceId = Resource.ResourceRef;
			Observation.ResourceKind = Resource.ResourceKind;
			Observation.PackageName = Resource.Package ? Resource.ResourceRef : FString();
			Observation.ObjectPath = Resource.ObjectPath;
			Observation.bDirty = true;
			OutResources.Add(MoveTemp(Observation));
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
				OutError = LookupFailure(TEXT("asset_project_read_platform_unresolved"),
					TEXT("The requested target platform is unavailable."), TEXT("/platform/available"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				OutError.Stage = TEXT("platform_lookup");
				OutError.CandidateName = RequestedId;
				return false;
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
			: [&]()
			{
				OutError = LookupFailure(TEXT("asset_project_read_project_unresolved"),
					TEXT("The general project settings object is unavailable."), TEXT("/project/object/available"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				OutError.CandidateName = SourceId;
				OutError.TargetPath = TEXT("project");
				OutError.SubjectId = Context.GetProjectInstanceId();
				return false;
			}();
	}

	bool FUeAgentAssetProjectReadProductionEnvironment::TryResolveProjectSettingsObject(
		const UE::UAI::ContextResource::FUeAgentExecutionContextRef& Context,
		const FString& SectionId,
		UObject*& OutObject,
		FUeAgentAssetProjectReadError& OutError)
	{
		OutError = {};
		OutObject = nullptr;
		EditorProject::FSettingsPage Page;
		UE::UAI::Contract::FDiagnostic Error;
		if (!EditorProject::FindSettingsPageBySectionId(SectionId, Page, Error))
		{
			OutError = Error;
			if (OutError.SubjectId.IsEmpty()) OutError.SubjectId = SectionId;
			if (OutError.CandidateName.IsEmpty()) OutError.CandidateName = SectionId;
			if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = TEXT("project");
			return false;
		}
		OutObject = Page.SettingsObject;
		return IsValid(OutObject)
			? true
			: [&]()
			{
				OutError = LookupFailure(TEXT("settings_section_object_unavailable"),
					TEXT("The selected settings section has no live settings object."), TEXT("/settings/object/available"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				OutError.CandidateName = SectionId;
				OutError.TargetPath = TEXT("project");
				return false;
			}();
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
			: [&]()
			{
				OutError = LookupFailure(TEXT("asset_project_read_config_unresolved"),
					!GConfig ? TEXT("The configuration cache is unavailable.")
						: !Filename ? TEXT("The requested configuration scope has no configured filename.")
						: TEXT("The configuration cache has no file for the requested scope."),
					!GConfig ? TEXT("/config/cache/available") : !Filename ? TEXT("/config/filename/available") : TEXT("/config/file/available"),
					MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				OutError.CandidateName = ConfigScope;
				if (Filename)
				{
					OutError.Source.SourceFile = *Filename;
					OutError.TargetPath = TEXT("config:") + (ConfigScope == TEXT("platform") ? FString(TEXT("game")) : ConfigScope);
				}
				return false;
			}();
	}
}

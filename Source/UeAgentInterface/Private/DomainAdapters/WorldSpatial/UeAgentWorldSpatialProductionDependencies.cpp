#include "DomainAdapters/WorldSpatial/UeAgentWorldSpatialProductionDependencies.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "DomainAdapters/ContentDiscovery/UeAgentEditorContentDiscovery.h"
#include "Utilities/EditorResources/UeAgentEditorResourcePersistence.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"

#include "BlueprintEditor.h"
#include "BlueprintEditorTabs.h"
#include "Components/ActorComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Core/AssetDurable/UeAgentPackageHeader.h"
#include "Core/EditorViewport/UeAgentViewportRealtimeState.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "Editor.h"
#include "EditorModeManager.h"
#include "EditorViewportClient.h"
#include "Engine/Blueprint.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "FileHelpers.h"
#include "GameFramework/Actor.h"
#include "LevelEditorViewport.h"
#include "MeshDescription.h"
#include "Misc/Crc.h"
#include "ModelingToolsEditorMode.h"
#include "DomainAdapters/ModelingGeometryEffect/UeAgentModelingGeometryEffectCarrierRevision.h"
#include "GraphEditor.h"
#include "HAL/FileManager.h"
#include "Selection/GeometrySelectionManager.h"
#include "Selections/GeometrySelection.h"
#include "SEditorViewport.h"
#include "StaticMeshAttributes.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "UObject/Class.h"
#include "UObject/SoftObjectPath.h"
#include "Widgets/Docking/SDockTab.h"

namespace UE::UAI::DomainAdapters::WorldSpatial
{
	namespace
	{
		bool BindingFailure(FUeAgentWorldSpatialExecutionError& OutError, const TCHAR* Code, const TCHAR* Reason,
			const FString& Path, TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected, bool bPublicValue = true)
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(Code, Reason, TEXT("native_execution"));
			OutError.PropertyPath = Path;
			OutError.SourceKind = TEXT("native_configuration");
			OutError.Source.JsonPath = Path;
			OutError.ActualValuePresent = Actual.IsValid();
			if (bPublicValue) OutError.ActualValue = MoveTemp(Actual);
			else OutError.ActualEvidence = MoveTemp(Actual);
			OutError.ExpectedValue = MoveTemp(Expected);
			return false;
		}

		struct FBlueprintComponentsViewportSession
		{
			TWeakObjectPtr<UBlueprint> Blueprint;
			FEditorViewportClient* Client = nullptr;
			FViewport* Viewport = nullptr;
		};

		TMap<FString, FBlueprintComponentsViewportSession>&
			BlueprintComponentsViewportSessions()
		{
			static TMap<FString, FBlueprintComponentsViewportSession> Sessions;
			return Sessions;
		}

		bool TryParseBlueprintComponentsViewportIdentity(
			const FString& ViewportIdentity,
			FString& OutBlueprintObjectPath)
		{
			static const FString Prefix = TEXT("asset:");
			static const FString Suffix = TEXT("#blueprint_components");
			OutBlueprintObjectPath.Reset();
			if (!ViewportIdentity.StartsWith(Prefix, ESearchCase::CaseSensitive)
				|| !ViewportIdentity.EndsWith(Suffix, ESearchCase::CaseSensitive))
			{
				return false;
			}
			OutBlueprintObjectPath = ViewportIdentity.Mid(
				Prefix.Len(),
				ViewportIdentity.Len() - Prefix.Len() - Suffix.Len());
			return !OutBlueprintObjectPath.IsEmpty();
		}

		bool FindWidgetByTypeRecursive(
			const TSharedRef<SWidget>& Root,
			const FName& TypeName,
			TSharedPtr<SWidget>& OutWidget)
		{
			if (Root->GetType() == TypeName)
			{
				OutWidget = Root;
				return true;
			}
			FChildren* const Children = Root->GetChildren();
			if (!Children)
			{
				return false;
			}
			for (int32 Index = 0; Index < Children->Num(); ++Index)
			{
				if (FindWidgetByTypeRecursive(
						Children->GetChildAt(Index), TypeName, OutWidget))
				{
					return true;
				}
			}
			return false;
		}

		bool TryResolveRegisteredBlueprintComponentsViewport(
			const FString& ViewportIdentity,
			FEditorViewportClient*& OutClient,
			FViewport*& OutViewport)
		{
			FBlueprintComponentsViewportSession* Session =
				BlueprintComponentsViewportSessions().Find(ViewportIdentity);
			if (!Session || !Session->Blueprint.IsValid() || !Session->Client
				|| !Session->Viewport
				|| Session->Client->Viewport != Session->Viewport)
			{
				return false;
			}
			OutClient = Session->Client;
			OutViewport = Session->Viewport;
			return true;
		}

		bool TryResolveOpenBlueprintComponentsViewport(
			const FString& ViewportIdentity,
			FEditorViewportClient*& OutClient,
			FViewport*& OutViewport)
		{
			FString BlueprintObjectPath;
			if (!GEditor || !IsInGameThread()
				|| !TryParseBlueprintComponentsViewportIdentity(
					ViewportIdentity, BlueprintObjectPath))
			{
				return false;
			}
			UBlueprint* const Blueprint = LoadObject<UBlueprint>(
				nullptr, *BlueprintObjectPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
			UAssetEditorSubsystem* const AssetEditors = Blueprint
				? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr;
			IAssetEditorInstance* const Instance = AssetEditors
				? AssetEditors->FindEditorForAsset(Blueprint, false) : nullptr;
			if (!Instance
				|| Instance->GetEditorName() != FName(TEXT("BlueprintEditor")))
			{
				return false;
			}
			FBlueprintEditor* const BlueprintEditor =
				static_cast<FBlueprintEditor*>(Instance);
			const TSharedPtr<FTabManager> TabManager =
				BlueprintEditor->GetAssociatedTabManager();
			const TSharedPtr<SDockTab> ViewportTab = TabManager.IsValid()
				? TabManager->FindExistingLiveTab(
					FTabId(FBlueprintEditorTabs::SCSViewportID))
				: nullptr;
			TSharedPtr<SWidget> FoundWidget;
			if (!ViewportTab.IsValid()
				|| !FindWidgetByTypeRecursive(
					ViewportTab->GetContent(),
					FName(TEXT("SSCSEditorViewport")),
					FoundWidget))
			{
				return false;
			}
			const TSharedPtr<SEditorViewport> ViewportWidget =
				StaticCastSharedPtr<SEditorViewport>(FoundWidget);
			const TSharedPtr<FEditorViewportClient> Client =
				ViewportWidget.IsValid()
					? ViewportWidget->GetViewportClient() : nullptr;
			if (!Client.IsValid() || !Client->Viewport)
			{
				return false;
			}
			OutClient = Client.Get();
			OutViewport = Client->Viewport;
			return true;
		}

		bool TryParseLevelViewportIdentity(
			const FString& ViewportIdentity,
			int32& OutViewportIndex)
		{
			OutViewportIndex = INDEX_NONE;
			if (!ViewportIdentity.StartsWith(TEXT("level:"), ESearchCase::CaseSensitive))
			{
				return false;
			}
			const FString IndexText = ViewportIdentity.RightChop(6);
			return !IndexText.IsEmpty()
				&& IndexText.IsNumeric()
				&& LexTryParseString(OutViewportIndex, *IndexText)
				&& OutViewportIndex >= 0
				&& FString::FromInt(OutViewportIndex) == IndexText;
		}

		bool TryResolveLiveLevelViewport(
			UWorld* ExplicitWorld,
			const FString& ViewportIdentity,
			FEditorViewportClient*& OutClient,
			FViewport*& OutViewport)
		{
			int32 ViewportIndex = INDEX_NONE;
			if (!ExplicitWorld || !GEditor || !IsInGameThread()
				|| !TryParseLevelViewportIdentity(
					ViewportIdentity,
					ViewportIndex))
			{
				return false;
			}

			const TArray<FLevelEditorViewportClient*>& LevelViewports =
				GEditor->GetLevelViewportClients();
			if (!LevelViewports.IsValidIndex(ViewportIndex))
			{
				return false;
			}
			FLevelEditorViewportClient* Client = LevelViewports[ViewportIndex];
			if (!Client || !Client->Viewport || Client->GetWorld() != ExplicitWorld)
			{
				return false;
			}

			OutClient = Client;
			OutViewport = Client->Viewport;
			return true;
		}

		void AppendLiveLevelViewportIdentities(
			UWorld* ExplicitWorld,
			TArray<FString>& OutViewportIdentities)
		{
			if (!ExplicitWorld || !GEditor || !IsInGameThread())
			{
				return;
			}
			const TArray<FLevelEditorViewportClient*>& LevelViewports =
				GEditor->GetLevelViewportClients();
			for (int32 Index = 0; Index < LevelViewports.Num(); ++Index)
			{
				FLevelEditorViewportClient* Client = LevelViewports[Index];
				if (Client && Client->Viewport
					&& Client->GetWorld() == ExplicitWorld)
				{
					OutViewportIdentities.AddUnique(
						FString::Printf(TEXT("level:%d"), Index));
				}
			}
		}

		bool EnvironmentFailure(FUeAgentWorldSpatialExecutionError& OutError,
			const TCHAR* Code, const TCHAR* Reason, const FString& Path,
			TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected, const FString& Target = FString())
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(Code, Reason, TEXT("native_execution"));
			OutError.TargetPath = Target;
			OutError.PropertyPath = Path;
			OutError.Source.JsonPath = Path;
			OutError.SourceKind = TEXT("native_result");
			OutError.ActualValuePresent = Actual.IsValid();
			OutError.ActualValue = MoveTemp(Actual);
			OutError.ExpectedValue = MoveTemp(Expected);
			return false;
		}

		bool EnvironmentWorldMismatch(FUeAgentWorldSpatialExecutionError& OutError,
			const TCHAR* Code, const TCHAR* Reason, const FString& Target,
			const UWorld* ActualWorld, const UWorld* ExpectedWorld)
		{
			EnvironmentFailure(OutError, Code, Reason, TEXT("/world"),
				MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueString>(TEXT("the configured world")), Target);
			if (::IsValid(ActualWorld)) OutError.ActualEvidence = MakeShared<FJsonValueString>(ActualWorld->GetPathName());
			if (::IsValid(ExpectedWorld)) OutError.ExpectedEvidence = MakeShared<FJsonValueString>(ExpectedWorld->GetPathName());
			return false;
		}

		class FProductionEnvironment final
			: public IUeAgentWorldSpatialNativeEnvironment
		{
		public:
			FProductionEnvironment(
				UWorld& InWorld,
				const TArray<FUeAgentWorldSpatialViewportBinding>&
					InViewportBindings,
				const TArray<FUeAgentWorldSpatialMeshSelectionBinding>&
					InMeshSelectionBindings)
				: World(&InWorld)
			{
				for (const FUeAgentWorldSpatialViewportBinding& Binding
					: InViewportBindings)
				{
					ViewportByIdentity.Add(Binding.Identity, Binding.Client);
				}
				for (const FUeAgentWorldSpatialMeshSelectionBinding& Binding
					: InMeshSelectionBindings)
				{
					MeshSelectionByViewport.Add(Binding.ViewportIdentity, Binding);
				}
			}

			bool TryResolveObject(
				const FString& ExplicitRef,
				UObject*& OutObject,
				FUeAgentWorldSpatialExecutionError& OutError) override
			{
				OutObject = nullptr;
				OutError = {};
				if (ExplicitRef.IsEmpty())
				{
					return EnvironmentFailure(OutError, TEXT("world_spatial_explicit_object_ref_missing"),
						TEXT("Object resolution requires an explicit object reference."), TEXT("/explicit_ref"),
						MakeShared<FJsonValueString>(ExplicitRef), MakeShared<FJsonValueString>(TEXT("non-empty object reference")));
				}

				UWorld* ScopedWorld = World.Get();
				if (!ScopedWorld)
				{
					return EnvironmentFailure(OutError, TEXT("world_spatial_explicit_world_expired"),
						TEXT("The configured world is no longer available for object resolution."), TEXT("/world"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueString>(TEXT("available configured world")), ExplicitRef);
				}
				if (ExplicitRef == ScopedWorld->GetPathName())
				{
					OutObject = ScopedWorld;
					return true;
				}

				FString ObjectPath = ExplicitRef;
				UE::UAI::ContentDiscovery::FContentDiscoveryError ResolveError;
				if (!ContentDiscovery::TryResolveEditorContentReference(ObjectPath, FString(), ResolveError))
				{
					OutError = MoveTemp(ResolveError);
					if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = ExplicitRef;
					return false;
				}
				OutObject = StaticFindObject(
					UObject::StaticClass(), nullptr, *ObjectPath, true);
				if (!OutObject)
				{
					const FSoftObjectPath SoftPath(ObjectPath);
					if (SoftPath.IsValid())
					{
						OutObject = SoftPath.TryLoad();
					}
				}
				if (!IsValid(OutObject)
					|| OutObject->GetPathName() != ObjectPath)
				{
					EnvironmentFailure(OutError, TEXT("world_spatial_explicit_object_unresolved"),
						!IsValid(OutObject) ? TEXT("No valid object could be resolved for the requested reference.")
							: TEXT("The resolved object does not match the requested exact reference."), TEXT("/explicit_ref"),
						MakeShared<FJsonValueString>(ExplicitRef), MakeShared<FJsonValueString>(TEXT("an exact reference to a valid object")), ExplicitRef);
					if (IsValid(OutObject)) OutError.ActualEvidence = MakeShared<FJsonValueString>(OutObject->GetPathName());
					OutObject = nullptr;
					return false;
				}

				const UWorld* ObjectWorld = nullptr;
				if (AActor* Actor = Cast<AActor>(OutObject))
				{
					ObjectWorld = Actor->GetWorld();
					if (ObjectWorld && ObjectWorld != ScopedWorld)
					{
						OutObject = nullptr;
						return EnvironmentWorldMismatch(OutError, TEXT("world_spatial_object_world_mismatch"),
							TEXT("The referenced actor belongs to a different world."), ExplicitRef, ObjectWorld, ScopedWorld);
					}
					const bool bBeingDestroyed = Actor->IsActorBeingDestroyed();
					if (bBeingDestroyed
						|| !ScopedWorld->ContainsActor(Actor))
					{
						OutObject = nullptr;
						return EnvironmentFailure(OutError, TEXT("world_spatial_explicit_object_unresolved"),
							bBeingDestroyed ? TEXT("The referenced actor is being destroyed.") : TEXT("The referenced actor is no longer in the configured world."),
							TEXT("/object"), MakeShared<FJsonValueString>(bBeingDestroyed ? TEXT("being_destroyed") : TEXT("outside_configured_world")),
							MakeShared<FJsonValueString>(TEXT("live actor in the configured world")), ExplicitRef);
					}
				}
				else if (UActorComponent* Component =
					Cast<UActorComponent>(OutObject))
				{
					ObjectWorld = Component->GetWorld();
					if (ObjectWorld && ObjectWorld != ScopedWorld)
					{
						OutObject = nullptr;
						return EnvironmentWorldMismatch(OutError, TEXT("world_spatial_object_world_mismatch"),
							TEXT("The referenced component belongs to a different world."), ExplicitRef, ObjectWorld, ScopedWorld);
					}
					AActor* Owner = Component->GetOwner();
					const bool bOwnerBeingDestroyed = Owner && Owner->IsActorBeingDestroyed();
					if (Owner
						&& (bOwnerBeingDestroyed
							|| !ScopedWorld->ContainsActor(Owner)))
					{
						OutObject = nullptr;
						return EnvironmentFailure(OutError, TEXT("world_spatial_explicit_object_unresolved"),
							bOwnerBeingDestroyed ? TEXT("The component's owner is being destroyed.") : TEXT("The component's owner is no longer in the configured world."),
							TEXT("/object/owner"), MakeShared<FJsonValueString>(bOwnerBeingDestroyed ? TEXT("being_destroyed") : TEXT("outside_configured_world")),
							MakeShared<FJsonValueString>(TEXT("live owner in the configured world")), ExplicitRef);
					}
				}
				return true;
			}

			bool TryPersistObject(
				UObject& Object,
				FUeAgentWorldSpatialExecutionError& OutError) override
			{
				OutError = {};
				UPackage* const Package = Object.GetOutermost();
				if (!Package || Object.HasAnyFlags(RF_Transient))
				{
					EnvironmentFailure(OutError, TEXT("world_spatial_persistence_target_invalid"),
						!Package ? TEXT("The object has no package to save.") : TEXT("The object is transient and cannot be persisted."),
						!Package ? TEXT("/package") : TEXT("/object/transient"),
						!Package ? TSharedPtr<FJsonValue>() : MakeShared<FJsonValueBoolean>(true),
						MakeShared<FJsonValueString>(TEXT("persistent object with a saveable package")));
					OutError.ActualEvidence = MakeShared<FJsonValueString>(Object.GetPathName());
					return false;
				}
				FString PackageHeaderFilename;
				UE::UAI::Contract::FDiagnostic PackageHeaderError;
				UObject* PackageTopLevelAsset =
					Package->FindAssetInPackage();
				if (!UE::UAI::AssetDurable::
					TryPrepareLoadedPackageHeaderForSave(
						*Package,
						PackageTopLevelAsset,
						PackageHeaderFilename,
						PackageHeaderError))
				{
					return (OutError = PackageHeaderError, false);
				}
				Object.MarkPackageDirty();
				if (!UE::UAI::EditorResources::SavePackages(
						{Package}, false, OutError))
				{
					return false;
				}
				FString ObservedHeaderFilename;
				PackageHeaderError.Reset();
				if (!UE::UAI::AssetDurable::
						TryResolvePackageHeaderFilename(
							Package->GetName(),
							UE::UAI::AssetDurable::EPackageHeaderKind::EvidenceRequired,
							ObservedHeaderFilename,
							PackageHeaderError))
				{
					return (OutError = PackageHeaderError, false);
				}
				if (!ObservedHeaderFilename.Equals(PackageHeaderFilename, ESearchCase::IgnoreCase))
				{
					EnvironmentFailure(OutError, TEXT("world_spatial_object_persistence_failed"),
						TEXT("The saved package resolves to a different header file than the prepared save target."), TEXT("/package/header"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueString>(TEXT("the prepared package header")));
					OutError.ActualEvidence = MakeShared<FJsonValueString>(ObservedHeaderFilename);
					OutError.ExpectedEvidence = MakeShared<FJsonValueString>(PackageHeaderFilename);
					return false;
				}
				if (!IFileManager::Get().FileExists(*ObservedHeaderFilename))
				{
					EnvironmentFailure(OutError, TEXT("world_spatial_object_persistence_failed"),
						TEXT("The package header file is missing after saving."), TEXT("/package/header/exists"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
					OutError.ActualEvidence = MakeShared<FJsonValueString>(ObservedHeaderFilename);
					return false;
				}
				if (Package->IsDirty())
				{
					return EnvironmentFailure(OutError, TEXT("world_spatial_object_persistence_failed"),
						TEXT("The package still has unsaved changes after saving."), TEXT("/package/dirty"),
						MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false));
				}
				return true;
			}

			bool TryResolveGraphEditor(
				const FString& AssetEditorIdentity,
				TSharedPtr<SGraphEditor>& OutGraphEditor,
				FUeAgentWorldSpatialExecutionError& OutError) override
			{
				OutGraphEditor.Reset();
				UObject* Object = nullptr;
				if (!TryResolveObject(AssetEditorIdentity, Object, OutError))
				{
					return false;
				}
				UEdGraph* Graph = Cast<UEdGraph>(Object);
				if (!Graph)
				{
					return EnvironmentFailure(OutError, TEXT("world_spatial_graph_identity_type_invalid"),
						TEXT("The asset editor reference resolves to an object that is not a graph."), TEXT("/asset_editor_identity"),
						MakeShared<FJsonValueString>(Object->GetClass()->GetPathName()),
						MakeShared<FJsonValueString>(TEXT("UEdGraph")), AssetEditorIdentity);
				}
				OutGraphEditor = SGraphEditor::FindGraphEditorForGraph(Graph);
				return OutGraphEditor.IsValid()
					? true
					: EnvironmentFailure(OutError, TEXT("world_spatial_graph_editor_not_open"),
						TEXT("The graph has no open graph editor."), TEXT("/asset_editor_identity"),
						MakeShared<FJsonValueString>(AssetEditorIdentity), MakeShared<FJsonValueString>(TEXT("graph with an open editor")), AssetEditorIdentity);
			}

			bool TryResolveViewport(
				const FString& ViewportIdentity,
				FEditorViewportClient*& OutClient,
				FViewport*& OutViewport,
				FUeAgentWorldSpatialExecutionError& OutError) override
			{
				OutClient = nullptr;
				OutViewport = nullptr;
				OutError = {};
				if (TryResolveRegisteredBlueprintComponentsViewport(
						ViewportIdentity, OutClient, OutViewport)
					|| TryResolveOpenBlueprintComponentsViewport(
						ViewportIdentity, OutClient, OutViewport))
				{
					return true;
				}
				FEditorViewportClient* const* Found =
					ViewportByIdentity.Find(ViewportIdentity);
				if (Found && *Found && (*Found)->Viewport
					&& (*Found)->GetWorld() == World.Get())
				{
					OutClient = *Found;
					OutViewport = (*Found)->Viewport;
					return true;
				}
				if (!TryResolveLiveLevelViewport(
						World.Get(),
						ViewportIdentity,
						OutClient,
						OutViewport))
				{
					return EnvironmentFailure(OutError, TEXT("world_spatial_explicit_viewport_unresolved"),
						TEXT("No available viewport matches the requested reference in the configured world."), TEXT("/viewport_identity"),
						MakeShared<FJsonValueString>(ViewportIdentity), MakeShared<FJsonValueString>(TEXT("available viewport reference")), ViewportIdentity);
				}
				return true;
			}

			bool TryGetExplicitObjectRef(
				const UObject* Object,
				FString& OutExplicitRef,
				FUeAgentWorldSpatialExecutionError& OutError) override
			{
				OutExplicitRef.Reset();
				OutError = {};
				if (!::IsValid(Object))
				{
					return EnvironmentFailure(OutError, TEXT("world_spatial_reverse_object_invalid"),
						TEXT("An explicit reference cannot be produced for an invalid object."), TEXT("/object"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueString>(TEXT("valid object")));
				}
				const UWorld* ObjectWorld = nullptr;
				if (const AActor* Actor = Cast<AActor>(Object))
				{
					ObjectWorld = Actor->GetWorld();
				}
				else if (const UActorComponent* Component =
					Cast<UActorComponent>(Object))
				{
					ObjectWorld = Component->GetWorld();
				}
				if (ObjectWorld && ObjectWorld != World.Get())
				{
					return EnvironmentWorldMismatch(OutError, TEXT("world_spatial_reverse_object_world_mismatch"),
						TEXT("An explicit reference cannot be produced because the object belongs to a different world."),
						FString(), ObjectWorld, World.Get());
				}
				OutExplicitRef = Object->GetPathName();
				if (OutExplicitRef.IsEmpty())
					return EnvironmentFailure(OutError, TEXT("world_spatial_reverse_object_invalid"),
						TEXT("The object has no path that can be returned as an explicit reference."), TEXT("/explicit_ref"),
						MakeShared<FJsonValueString>(OutExplicitRef), MakeShared<FJsonValueString>(TEXT("non-empty object reference")));
				return true;
			}

			bool TryResolveMeshSelectionSession(
				const FString& ViewportIdentity,
				const TArray<FString>& SelectionIdentities,
				UGeometrySelectionManager*& OutManager,
				TArray<FUeAgentWorldSpatialMeshSelectionTarget>& OutTargets,
				FUeAgentWorldSpatialExecutionError& OutError) override
			{
				OutManager = nullptr;
				OutTargets.Reset();
				FEditorViewportClient* ExactClient = nullptr;
				FViewport* ExactViewport = nullptr;
				if (!TryResolveViewport(
						ViewportIdentity,
						ExactClient,
						ExactViewport,
						OutError))
				{
					return false;
				}
				if (SelectionIdentities.IsEmpty())
				{
					return EnvironmentFailure(OutError, TEXT("world_spatial_mesh_selection_identities_missing"),
						TEXT("Mesh selection requires at least one component reference."), TEXT("/selection_identities"),
						MakeShared<FJsonValueNumber>(0), MakeShared<FJsonValueString>(TEXT("at least one component reference")), ViewportIdentity);
				}

				const FUeAgentWorldSpatialMeshSelectionBinding* Session =
					MeshSelectionByViewport.Find(ViewportIdentity);
				UModelingToolsEditorMode* LiveModelingMode = nullptr;
				if (Session)
				{
					OutManager = Session->Manager;
					if (!OutManager)
					{
						return EnvironmentFailure(OutError, TEXT("world_spatial_explicit_mesh_selection_unavailable"),
							TEXT("The registered mesh selection session has no selection manager."), TEXT("/selection_manager"),
							nullptr, MakeShared<FJsonValueString>(TEXT("available selection manager")), ViewportIdentity);
					}
				}
				else
				{
					LiveModelingMode = Cast<UModelingToolsEditorMode>(
						GLevelEditorModeTools().GetActiveScriptableMode(
							UModelingToolsEditorMode::
								EM_ModelingToolsEditorModeId));
					OutManager = LiveModelingMode
						? LiveModelingMode->GetSelectionManager()
						: nullptr;
				}
				if (!OutManager)
				{
					return EnvironmentFailure(OutError, TEXT("world_spatial_active_mesh_selection_unavailable"),
						!LiveModelingMode ? TEXT("Modeling mode is not active, so no live mesh selection session is available.")
							: TEXT("The active modeling mode has no selection manager."),
						!LiveModelingMode ? TEXT("/modeling_mode") : TEXT("/selection_manager"), nullptr,
						MakeShared<FJsonValueString>(TEXT("active modeling mode with a selection manager")), ViewportIdentity);
				}
				UE::Geometry::EGeometryTopologyType TopologyType =
					UE::Geometry::EGeometryTopologyType::Triangle;
				UE::Geometry::EGeometryElementType ElementType =
					UE::Geometry::EGeometryElementType::Face;
				int32 ActiveTargetCount = 0;
				bool bSelectionIsEmpty = true;
				OutManager->GetActiveSelectionInfo(
					TopologyType,
					ElementType,
					ActiveTargetCount,
					bSelectionIsEmpty);
				if (!OutManager->HasActiveTargets())
					return EnvironmentFailure(OutError, TEXT("world_spatial_mesh_selection_targets_inactive"),
						TEXT("The mesh selection manager has no active targets."), TEXT("/active_targets"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), ViewportIdentity);
				if (OutManager->GetAnyCurrentTargetsLocked())
					return EnvironmentFailure(OutError, TEXT("world_spatial_mesh_selection_targets_inactive"),
						TEXT("One or more current mesh selection targets are locked."), TEXT("/targets_locked"),
						MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false), ViewportIdentity);
				if (ActiveTargetCount < SelectionIdentities.Num())
					return EnvironmentFailure(OutError, TEXT("world_spatial_mesh_selection_targets_inactive"),
						TEXT("There are fewer active mesh targets than requested component references."), TEXT("/active_target_count"),
						MakeShared<FJsonValueNumber>(ActiveTargetCount), MakeShared<FJsonValueNumber>(SelectionIdentities.Num()), ViewportIdentity);

				TSet<FString> Seen;
				for (int32 Index = 0; Index < SelectionIdentities.Num(); ++Index)
				{
					const FString& Identity = SelectionIdentities[Index];
					const FString Path = FString::Printf(TEXT("/selection_identities/%d"), Index);
					if (Seen.Contains(Identity))
					{
						return EnvironmentFailure(OutError, TEXT("world_spatial_mesh_selection_identity_duplicate"),
							TEXT("The mesh selection request repeats a component reference."), Path,
							MakeShared<FJsonValueString>(Identity), MakeShared<FJsonValueString>(TEXT("unique component reference")), Identity);
					}
					Seen.Add(Identity);
					if (Session)
					{
						const FUeAgentWorldSpatialMeshSelectionTarget* Target =
							Session->Targets.FindByPredicate(
								[&Identity](
									const FUeAgentWorldSpatialMeshSelectionTarget& Item)
								{
									return Item.ExplicitRef == Identity;
								});
						if (!Target)
						{
							return EnvironmentFailure(OutError, TEXT("world_spatial_mesh_selection_identity_unresolved"),
								TEXT("The mesh selection session has no target for the requested component reference."), Path,
								MakeShared<FJsonValueString>(Identity), MakeShared<FJsonValueString>(TEXT("registered selection target")), Identity);
						}
						if (!Target->TryValidate(OutError))
						{
							const int32 TargetIndex = static_cast<int32>(Target - Session->Targets.GetData());
							UeAgentAuthoringDiagnostics::PrefixDocumentFailure(OutError, FString::Printf(TEXT("/selection_targets/%d"), TargetIndex));
							return false;
						}
						OutTargets.Add(*Target);
						continue;
					}

					UObject* ResolvedObject = nullptr;
					if (!TryResolveObject(Identity, ResolvedObject, OutError))
					{
						return false;
					}
					UPrimitiveComponent* Component =
						Cast<UPrimitiveComponent>(ResolvedObject);
					if (!Component)
					{
						return EnvironmentFailure(OutError, TEXT("world_spatial_mesh_selection_identity_type_invalid"),
							TEXT("The mesh selection reference resolves to an object that is not a primitive component."), Path,
							MakeShared<FJsonValueString>(ResolvedObject->GetClass()->GetPathName()),
							MakeShared<FJsonValueString>(TEXT("UPrimitiveComponent")), Identity);
					}
					bool bComponentIsActive = false;
					if (bSelectionIsEmpty)
					{
						// UE 5.6 exposes active-target identities only to subclasses.
						// Setting an already-empty, type-matched selection is the public,
						// transaction-free exact-component probe in this state.
						UE::Geometry::FGeometrySelection EmptySelection;
						EmptySelection.InitializeTypes(ElementType, TopologyType);
						bComponentIsActive = OutManager->SetSelectionForComponent(
							Component,
							EmptySelection);
					}
					else
					{
						// UE 5.6's HasSelectionForComponent reports the inverse
						// selection-emptiness predicate. Read the exact value instead.
						UE::Geometry::FGeometrySelection ExistingSelection;
						bComponentIsActive = OutManager->GetSelectionForComponent(
							Component,
							ExistingSelection);
					}
					if (!bComponentIsActive)
					{
						return EnvironmentFailure(OutError, TEXT("world_spatial_mesh_selection_identity_not_active"),
							TEXT("The requested component is not an active target in the mesh selection session."), Path,
							MakeShared<FJsonValueString>(Identity), MakeShared<FJsonValueString>(TEXT("active selection target")), Identity);
					}
					OutTargets.Add({Identity, Component});
				}
				return true;
			}

			void GetExplicitViewportIdentities(
				TArray<FString>& OutViewportIdentities) const override
			{
				ViewportByIdentity.GetKeys(OutViewportIdentities);
				for (const TPair<FString, FBlueprintComponentsViewportSession>& Pair
					: BlueprintComponentsViewportSessions())
				{
					if (Pair.Value.Blueprint.IsValid() && Pair.Value.Client
						&& Pair.Value.Viewport
						&& Pair.Value.Client->Viewport == Pair.Value.Viewport)
					{
						OutViewportIdentities.AddUnique(Pair.Key);
					}
				}
				AppendLiveLevelViewportIdentities(
					World.Get(),
					OutViewportIdentities);
				OutViewportIdentities.Sort();
			}

		private:
			TWeakObjectPtr<UWorld> World;
			TMap<FString, FEditorViewportClient*> ViewportByIdentity;
			TMap<FString, FUeAgentWorldSpatialMeshSelectionBinding>
				MeshSelectionByViewport;
		};

		bool SpaceConversionFailure(FUeAgentWorldSpatialExecutionError& OutError, const TCHAR* Code,
			const TCHAR* Reason, const FString& Subject, const FString& Path,
			TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected, const TCHAR* SourceKind)
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(Code, Reason, TEXT("native_execution"));
			OutError.TargetPath = Subject;
			OutError.PropertyPath = Path;
			OutError.SourceKind = SourceKind;
			OutError.Source.JsonPath = Path;
			OutError.ActualValue = MoveTemp(Actual);
			OutError.ActualValuePresent = OutError.ActualValue.IsValid();
			OutError.ExpectedValue = MoveTemp(Expected);
			return false;
		}

		TSharedPtr<FJsonValue> FrameUnitFact(const FString& Frame, const FString& Unit)
		{
			const TSharedRef<FJsonObject> Fact = MakeShared<FJsonObject>();
			Fact->SetStringField(TEXT("frame_id"), Frame);
			Fact->SetStringField(TEXT("unit"), Unit);
			return MakeShared<FJsonValueObject>(Fact);
		}

		class FProductionSpaceCompiler final
			: public IUeAgentWorldSpatialSpaceCompiler
		{
		public:
			bool TryConvertToNative(
				const FWorldSpatialValue& Value,
				const FString& NativeFrameId,
				const FString& NativeUnit,
				FUeAgentWorldSpatialCompiledValue& OutValue,
				FUeAgentWorldSpatialExecutionError& OutError) override
			{
				OutValue = {};
				OutError = {};
				if (!Value.TryValidate(OutError)) return false;
				if (NativeFrameId.IsEmpty()
					|| NativeUnit.IsEmpty())
				{
					return SpaceConversionFailure(OutError, TEXT("world_spatial_conversion_contract_invalid"),
						TEXT("The native conversion destination requires a frame and unit."), Value.Subject,
						NativeFrameId.IsEmpty() ? TEXT("/native_frame_id") : TEXT("/native_unit"),
						MakeShared<FJsonValueString>(FString()), MakeShared<FJsonValueString>(TEXT("non-empty string")), TEXT("native_contract"));
				}
				if (Value.Frame.FrameId != NativeFrameId
					|| Value.Frame.Unit != NativeUnit)
				{
					return SpaceConversionFailure(OutError, TEXT("world_spatial_conversion_path_unavailable"),
						TEXT("The spatial compiler supports identity conversion only; the source frame or unit differs from the native destination."),
						Value.Subject, TEXT("/frame"), FrameUnitFact(Value.Frame.FrameId, Value.Frame.Unit),
						FrameUnitFact(NativeFrameId, NativeUnit), TEXT("world_spatial_value"));
				}

				TArray<double> Components;
				Components.Reserve(Value.Components.Num());
				for (int32 Index = 0; Index < Value.Components.Num(); ++Index)
				{
					const FString& Lexeme = Value.Components[Index];
					double Parsed = 0.0;
					if (!LexTryParseString(Parsed, *Lexeme)
						|| !FMath::IsFinite(Parsed))
					{
						return SpaceConversionFailure(OutError, TEXT("world_spatial_numeric_component_invalid"),
							TEXT("The spatial component cannot be converted to a finite native number."), Value.Subject,
							FString::Printf(TEXT("/components/%d"), Index), MakeShared<FJsonValueString>(Lexeme),
							MakeShared<FJsonValueString>(TEXT("finite number")), TEXT("world_spatial_value"));
					}
					Components.Add(Parsed);
				}
				OutValue = {
					Value.Subject,
					Value.Quantity,
					NativeFrameId,
					Value.Frame.Dimension,
					NativeUnit,
					MoveTemp(Components),
					TEXT("world-spatial-native-identity:")
						+ NativeFrameId + TEXT(":") + NativeUnit,
				};
				return OutValue.TryValidate(OutError);
			}

			bool TryConvertFromNative(
				const FUeAgentWorldSpatialCompiledValue& Value,
				const FWorldSpatialFrame& SemanticFrame,
				FWorldSpatialValue& OutValue,
				FUeAgentWorldSpatialExecutionError& OutError) override
			{
				OutValue = {};
				OutError = {};
				if (!Value.TryValidate(OutError)) return false;
				if (!SemanticFrame.TryValidate(OutError)) return false;
				if (Value.Dimension != SemanticFrame.Dimension)
				{
					OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("world_spatial_readback_conversion_invalid"),
						TEXT("The native value and semantic frame have different dimensions."), TEXT("native_execution"));
					OutError.SubjectId = Value.Subject;
					OutError.PropertyPath = TEXT("dimension");
					OutError.ActualValue = MakeShared<FJsonValueNumber>(Value.Dimension);
					OutError.ActualValuePresent = true;
					OutError.ExpectedValue = MakeShared<FJsonValueNumber>(SemanticFrame.Dimension);
					return false;
				}
				if (Value.FrameId != SemanticFrame.FrameId
					|| Value.Unit != SemanticFrame.Unit)
				{
					return SpaceConversionFailure(OutError, TEXT("world_spatial_readback_conversion_path_unavailable"),
						TEXT("The spatial compiler supports identity conversion only; the native frame or unit differs from the semantic destination."),
						Value.Subject, TEXT("/"), FrameUnitFact(Value.FrameId, Value.Unit),
						FrameUnitFact(SemanticFrame.FrameId, SemanticFrame.Unit), TEXT("native_result"));
				}

				OutValue.Subject = Value.Subject;
				OutValue.Quantity = Value.Quantity;
				OutValue.Frame = SemanticFrame;
				OutValue.ConversionEvidenceIds.Add(Value.EvidenceId);
				for (const double Component : Value.Components)
				{
					OutValue.Components.Add(
						FString::SanitizeFloat(Component, 9));
				}
				return OutValue.TryValidate(OutError);
			}
		};

		class FProductionRevisionSource final
			: public IUeAgentWorldSpatialRevisionSource
		{
		public:
			FProductionRevisionSource(
				TSharedRef<FProductionEnvironment> InEnvironment,
				FString InGeneration)
				: Environment(MoveTemp(InEnvironment))
				, Generation(MoveTemp(InGeneration))
			{
			}

			bool TryReadRevision(
				const FString& ExplicitRef,
				FWorldSpatialRevision& OutRevision,
				FUeAgentWorldSpatialExecutionError& OutError) override
			{
				OutRevision = {};
				OutError = {};
				if (ExplicitRef.IsEmpty())
				{
					return EnvironmentFailure(OutError, TEXT("world_spatial_revision_ref_missing"),
						TEXT("Revision readback requires an explicit object or viewport reference."), TEXT("/explicit_ref"),
						MakeShared<FJsonValueString>(ExplicitRef), MakeShared<FJsonValueString>(TEXT("non-empty object or viewport reference")));
				}

				uint32 Hash = 0;
				FEditorViewportClient* Client = nullptr;
				FViewport* Viewport = nullptr;
				FUeAgentWorldSpatialExecutionError ViewportError;
				if (Environment->TryResolveViewport(
						ExplicitRef, Client, Viewport, ViewportError))
				{
					const auto Realtime = EditorViewport::ReadRealtimeState(*Client);
					const FString State = FString::Printf(
						TEXT("%s|%s|%.17g|%d|%d|%d|%s|%d|%d|%d"),
						*Client->GetViewLocation().ToString(),
						*Client->GetViewRotation().ToString(),
						Client->ViewFOV,
						static_cast<int32>(Client->GetViewMode()),
						Realtime.bEffective ? 1 : 0,
						Realtime.bConfigured ? 1 : 0,
						*Realtime.OverrideReason,
						Client->IsInGameView() ? 1 : 0,
						Viewport->GetSizeXY().X,
						Viewport->GetSizeXY().Y);
					Hash = FCrc::StrCrc32(*State);
				}
				else
				{
					UObject* Object = nullptr;
					if (!Environment->TryResolveObject(
							ExplicitRef, Object, OutError))
					{
						if (ViewportError.IsValid()) OutError.Diagnostics.Add(MoveTemp(ViewportError));
						return false;
					}
					if (UE::UAI::DomainAdapters::ModelingGeometryEffect::
						IsModelingGeometryCarrier(*Object))
					{
						TOptional<StateSemantic::FUeAgentStateRevision>
							CarrierRevision;
						ExecutionMutation::FExecutionMutationError
							CarrierError;
						if (!UE::UAI::DomainAdapters::ModelingGeometryEffect::
								TryReadModelingGeometryCarrierRevision(
									ExplicitRef,
									*Object,
									CarrierRevision,
									CarrierError))
						{
							OutError = MoveTemp(CarrierError);
							if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = ExplicitRef;
							return false;
						}
						OutRevision.SourceId =
							CarrierRevision->GetSourceId();
						OutRevision.Generation =
							CarrierRevision->GetGeneration();
						OutRevision.Sequence =
							CarrierRevision->GetSequence();
						return OutRevision.TryValidate(OutError);
					}
					Hash = HashObjectSpatialState(*Object);
				}

				OutRevision.SourceId = ExplicitRef;
				OutRevision.Generation = Generation;
				OutRevision.Sequence = static_cast<int64>(Hash);
				return OutRevision.TryValidate(OutError);
			}

		private:
			static uint32 HashObjectSpatialState(UObject& Object)
			{
				uint32 Hash = FCrc::StrCrc32(*Object.GetPathName());
				if (const AActor* Actor = Cast<AActor>(&Object))
				{
					return FCrc::StrCrc32(
						*Actor->GetActorTransform().ToString(), Hash);
				}
				if (const UActorComponent* Component =
					Cast<UActorComponent>(&Object))
				{
					const AActor* Owner = Component->GetOwner();
					return Owner
						? FCrc::StrCrc32(
							*Owner->GetActorTransform().ToString(), Hash)
						: Hash;
				}
				if (const UEdGraph* Graph = Cast<UEdGraph>(&Object))
				{
					TArray<FString> Nodes;
					for (const UEdGraphNode* Node : Graph->Nodes)
					{
						if (Node)
						{
							Nodes.Add(FString::Printf(
								TEXT("%s|%d|%d"),
								*Node->NodeGuid.ToString(),
								Node->NodePosX,
								Node->NodePosY));
						}
					}
					Nodes.Sort();
					for (const FString& Node : Nodes)
					{
						Hash = FCrc::StrCrc32(*Node, Hash);
					}
					return Hash;
				}
				if (UStaticMesh* Mesh = Cast<UStaticMesh>(&Object))
				{
					if (FMeshDescription* Description =
						Mesh->GetMeshDescription(0))
					{
						FStaticMeshAttributes Attributes(*Description);
						const TVertexAttributesConstRef<FVector3f> Positions =
							Attributes.GetVertexPositions();
						for (const FVertexID VertexId
							: Description->Vertices().GetElementIDs())
						{
							const FVector3f Position = Positions[VertexId];
							Hash = FCrc::MemCrc32(
								&Position, sizeof(Position), Hash);
						}
					}
				}
				return Hash;
			}

			TSharedRef<FProductionEnvironment> Environment;
			FString Generation;
		};
	}

	FString MakeBlueprintComponentsViewportIdentity(const UBlueprint& Blueprint)
	{
		return TEXT("asset:") + Blueprint.GetPathName()
			+ TEXT("#blueprint_components");
	}

	bool TryRegisterBlueprintComponentsViewport(
		UBlueprint& Blueprint,
		FEditorViewportClient& Client,
		FViewport& Viewport,
		FString& OutViewportIdentity,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		OutViewportIdentity.Reset();
		OutError.Reset();
		FString PublicTarget;
		auto Reject = [&](const TCHAR* Code, const TCHAR* Reason, const FString& Path,
			TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected)
		{
			OutError = UE::UAI::Contract::FDiagnostic::Failure(Code, Reason, TEXT("native_execution"));
			OutError.SourceKind = TEXT("native_result");
			OutError.PropertyPath = Path;
			OutError.TargetPath = PublicTarget;
			OutError.ActualValue = MoveTemp(Actual);
			OutError.ExpectedValue = MoveTemp(Expected);
			return false;
		};
		if (!IsInGameThread()) return Reject(TEXT("world_spatial_blueprint_preview_session_invalid"),
			TEXT("Blueprint components viewport registration must run on the game thread."), TEXT("/game_thread"),
			MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
		if (!::IsValid(&Blueprint)) return Reject(TEXT("world_spatial_blueprint_preview_session_invalid"),
			TEXT("Blueprint components viewport requires a valid Blueprint."), TEXT("/blueprint"),
			MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
		PublicTarget = Blueprint.GetPathName();
		UWorld* World = Client.GetWorld();
		if (!World) return Reject(TEXT("world_spatial_blueprint_preview_session_invalid"),
			TEXT("Blueprint components viewport has no world."), TEXT("/viewport/world"),
			MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
		if (!World->Scene) return Reject(TEXT("world_spatial_blueprint_preview_session_invalid"),
			TEXT("Blueprint components viewport world has no rendering scene."), TEXT("/viewport/scene"),
			MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
		if (Client.Viewport != &Viewport) return Reject(TEXT("world_spatial_blueprint_preview_session_invalid"),
			TEXT("Viewport client is bound to a different viewport."), TEXT("/viewport/client_binding"),
			MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
		const FIntPoint Size = Viewport.GetSizeXY();
		if (Size.X <= 0) return Reject(TEXT("world_spatial_blueprint_preview_session_invalid"),
			TEXT("Blueprint components viewport width must be positive."), TEXT("/viewport/size/x"),
			MakeShared<FJsonValueNumber>(Size.X), MakeShared<FJsonValueString>(TEXT("width > 0")));
		if (Size.Y <= 0) return Reject(TEXT("world_spatial_blueprint_preview_session_invalid"),
			TEXT("Blueprint components viewport height must be positive."), TEXT("/viewport/size/y"),
			MakeShared<FJsonValueNumber>(Size.Y), MakeShared<FJsonValueString>(TEXT("height > 0")));
		OutViewportIdentity = MakeBlueprintComponentsViewportIdentity(Blueprint);
		TMap<FString, FBlueprintComponentsViewportSession>& Sessions = BlueprintComponentsViewportSessions();
		if (Sessions.Contains(OutViewportIdentity))
		{
			Reject(TEXT("world_spatial_blueprint_preview_session_duplicate"),
				TEXT("Blueprint already has a registered components preview viewport."), TEXT("/viewport/session_registered"),
				MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false));
			OutError.SubjectId = OutViewportIdentity;
			OutViewportIdentity.Reset();
			return false;
		}
		Sessions.Add(
			OutViewportIdentity,
			{&Blueprint, &Client, &Viewport});
		return true;
	}

	void UnregisterBlueprintComponentsViewport(
		const FString& ViewportIdentity,
		FEditorViewportClient& Client)
	{
		if (!IsInGameThread())
		{
			return;
		}
		TMap<FString, FBlueprintComponentsViewportSession>& Sessions =
			BlueprintComponentsViewportSessions();
		const FBlueprintComponentsViewportSession* const Session =
			Sessions.Find(ViewportIdentity);
		if (Session && Session->Client == &Client)
		{
			Sessions.Remove(ViewportIdentity);
		}
	}

	bool FUeAgentWorldSpatialViewportBinding::TryValidateForWorld(
		const UWorld& ExplicitWorld, FUeAgentWorldSpatialExecutionError& OutError) const
	{
		OutError = {};
		if (Identity.IsEmpty())
			return BindingFailure(OutError, TEXT("world_spatial_explicit_viewport_binding_invalid"),
				TEXT("The viewport binding requires a readable viewport identity."), TEXT("/identity"),
				MakeShared<FJsonValueString>(Identity), MakeShared<FJsonValueString>(TEXT("non-empty viewport identity")));
		if (!Client || !Viewport)
			return BindingFailure(OutError, TEXT("world_spatial_explicit_viewport_binding_invalid"),
				TEXT("The viewport binding is missing its client or viewport."), !Client ? TEXT("/client") : TEXT("/viewport"),
				nullptr, MakeShared<FJsonValueString>(TEXT("bound viewport resource")));
		if (Client->Viewport != Viewport)
			return BindingFailure(OutError, TEXT("world_spatial_explicit_viewport_binding_invalid"),
				TEXT("The viewport client is attached to a different viewport."), TEXT("/client/viewport"),
				MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
		if (Client->GetWorld() != &ExplicitWorld)
		{
			const UWorld* ActualWorld = Client->GetWorld();
			BindingFailure(OutError, TEXT("world_spatial_explicit_viewport_binding_invalid"),
				TEXT("The viewport client belongs to a different world."), TEXT("/client/world"),
				MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueString>(TEXT("the configured world")));
			OutError.TargetPath = Identity;
			if (::IsValid(ActualWorld)) OutError.ActualEvidence = MakeShared<FJsonValueString>(ActualWorld->GetPathName());
			if (::IsValid(&ExplicitWorld)) OutError.ExpectedEvidence = MakeShared<FJsonValueString>(ExplicitWorld.GetPathName());
			return false;
		}
		return true;
	}

	bool FUeAgentWorldSpatialViewportBinding::IsValidForWorld(const UWorld& ExplicitWorld) const
	{
		FUeAgentWorldSpatialExecutionError Error;
		return TryValidateForWorld(ExplicitWorld, Error);
	}

	bool FUeAgentWorldSpatialMeshSelectionBinding::TryValidate(FUeAgentWorldSpatialExecutionError& OutError) const
	{
		OutError = {};
		if (ViewportIdentity.IsEmpty())
			return BindingFailure(OutError, TEXT("world_spatial_mesh_selection_binding_invalid"),
				TEXT("The mesh selection binding requires a readable viewport identity."), TEXT("/viewport_identity"),
				MakeShared<FJsonValueString>(ViewportIdentity), MakeShared<FJsonValueString>(TEXT("non-empty viewport identity")));
		if (!::IsValid(Manager))
			return BindingFailure(OutError, TEXT("world_spatial_mesh_selection_binding_invalid"),
				TEXT("The mesh selection manager is missing or no longer valid."), TEXT("/manager"),
				MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
		if (Targets.IsEmpty())
			return BindingFailure(OutError, TEXT("world_spatial_mesh_selection_binding_invalid"),
				TEXT("The mesh selection binding has no targets."), TEXT("/targets"),
				MakeShared<FJsonValueNumber>(0), MakeShared<FJsonValueString>(TEXT("at least one target")));
		TSet<FString> Identities;
		for (int32 Index = 0; Index < Targets.Num(); ++Index)
		{
			const auto& Target = Targets[Index];
			if (!Target.TryValidate(OutError))
			{
				UeAgentAuthoringDiagnostics::PrefixDocumentFailure(OutError, FString::Printf(TEXT("/targets/%d"), Index));
				return false;
			}
			if (Identities.Contains(Target.ExplicitRef))
				return BindingFailure(OutError, TEXT("world_spatial_mesh_selection_binding_invalid"),
					TEXT("The mesh selection binding repeats a target reference."), FString::Printf(TEXT("/targets/%d/explicit_ref"), Index),
					MakeShared<FJsonValueString>(Target.ExplicitRef), MakeShared<FJsonValueString>(TEXT("unique target reference")));
			Identities.Add(Target.ExplicitRef);
		}
		return true;
	}

	bool FUeAgentWorldSpatialMeshSelectionBinding::IsValid() const
	{
		FUeAgentWorldSpatialExecutionError Error;
		return TryValidate(Error);
	}

	bool TryCreateWorldSpatialProductionDependencies(
		UWorld& ExplicitWorld,
		const FString& RuntimeGeneration,
		const FString& ServerSessionId,
		FUeAgentWorldSpatialNativeDependencies& OutDependencies,
		FUeAgentWorldSpatialExecutionError& OutError,
		const TArray<FUeAgentWorldSpatialViewportBinding>& ViewportBindings,
		const TArray<FUeAgentWorldSpatialMeshSelectionBinding>&
			MeshSelectionBindings)
	{
		OutDependencies = {};
		OutError = {};
		if (!::IsValid(&ExplicitWorld) || ExplicitWorld.IsUnreachable())
		{
			return BindingFailure(OutError, TEXT("world_spatial_production_dependencies_invalid"),
				TEXT("The explicit world is invalid or unreachable."), TEXT("/world"),
				MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueString>(TEXT("valid reachable world")));
		}
		if (RuntimeGeneration.IsEmpty() || ServerSessionId.IsEmpty())
		{
			return BindingFailure(OutError, TEXT("world_spatial_production_dependencies_invalid"),
				TEXT("The native dependency factory is missing a runtime generation or server session binding."),
				RuntimeGeneration.IsEmpty() ? TEXT("/runtime_generation") : TEXT("/server_session_id"),
				MakeShared<FJsonValueString>(FString()), MakeShared<FJsonValueString>(TEXT("non-empty binding")), false);
		}
		TSet<FString> ViewportIds;
		for (int32 Index = 0; Index < ViewportBindings.Num(); ++Index)
		{
			const auto& Binding = ViewportBindings[Index];
			const FString Path = FString::Printf(TEXT("/viewport_bindings/%d"), Index);
			if (!Binding.TryValidateForWorld(ExplicitWorld, OutError))
			{
				UeAgentAuthoringDiagnostics::PrefixDocumentFailure(OutError, Path);
				return false;
			}
			if (ViewportIds.Contains(Binding.Identity))
				return BindingFailure(OutError, TEXT("world_spatial_explicit_viewport_binding_invalid"),
					TEXT("The native configuration repeats a viewport identity."), Path + TEXT("/identity"),
					MakeShared<FJsonValueString>(Binding.Identity), MakeShared<FJsonValueString>(TEXT("unique viewport identity")));
			ViewportIds.Add(Binding.Identity);
		}
		TSet<FString> SelectionViewportIds;
		for (int32 Index = 0; Index < MeshSelectionBindings.Num(); ++Index)
		{
			const auto& Binding = MeshSelectionBindings[Index];
			const FString Path = FString::Printf(TEXT("/mesh_selection_bindings/%d"), Index);
			if (!Binding.TryValidate(OutError))
			{
				UeAgentAuthoringDiagnostics::PrefixDocumentFailure(OutError, Path);
				return false;
			}
			for (int32 TargetIndex = 0; TargetIndex < Binding.Targets.Num(); ++TargetIndex)
			{
				const auto& Target = Binding.Targets[TargetIndex];
				const UWorld* TargetWorld = Target.Component->GetWorld();
				if (TargetWorld != &ExplicitWorld)
				{
					BindingFailure(OutError, TEXT("world_spatial_mesh_selection_binding_invalid"),
						TEXT("The mesh selection target belongs to a different world."),
						Path + FString::Printf(TEXT("/targets/%d/component/world"), TargetIndex),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueString>(TEXT("the configured world")));
					OutError.TargetPath = Target.ExplicitRef;
					if (::IsValid(TargetWorld)) OutError.ActualEvidence = MakeShared<FJsonValueString>(TargetWorld->GetPathName());
					OutError.ExpectedEvidence = MakeShared<FJsonValueString>(ExplicitWorld.GetPathName());
					return false;
				}
			}
			if (!ViewportIds.Contains(Binding.ViewportIdentity) || SelectionViewportIds.Contains(Binding.ViewportIdentity))
				return BindingFailure(OutError, TEXT("world_spatial_mesh_selection_binding_invalid"),
					!ViewportIds.Contains(Binding.ViewportIdentity) ? TEXT("The mesh selection binding references an unregistered viewport.")
						: TEXT("The native configuration repeats a mesh selection viewport binding."), Path + TEXT("/viewport_identity"),
					MakeShared<FJsonValueString>(Binding.ViewportIdentity), MakeShared<FJsonValueString>(TEXT("unique registered viewport identity")));
			SelectionViewportIds.Add(Binding.ViewportIdentity);
		}

		const TSharedRef<FProductionEnvironment> Environment =
			MakeShared<FProductionEnvironment>(
				ExplicitWorld, ViewportBindings, MeshSelectionBindings);
		OutDependencies.Environment = Environment;
		OutDependencies.SpaceCompiler = MakeShared<FProductionSpaceCompiler>();
		OutDependencies.RevisionSource =
			MakeShared<FProductionRevisionSource>(Environment, RuntimeGeneration);
		return OutDependencies.TryValidate(OutError);
	}
}

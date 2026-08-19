#include "DomainAdapters/WorldSpatial/UeAgentWorldSpatialProductionDependencies.h"

#include "BlueprintEditor.h"
#include "BlueprintEditorTabs.h"
#include "Components/ActorComponent.h"
#include "Components/PrimitiveComponent.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "Editor.h"
#include "EditorViewportClient.h"
#include "Engine/Blueprint.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "LevelEditorViewport.h"
#include "MeshDescription.h"
#include "Misc/Crc.h"
#include "GraphEditor.h"
#include "Selection/GeometrySelectionManager.h"
#include "Selections/GeometrySelection.h"
#include "SEditorViewport.h"
#include "StaticMeshAttributes.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "UObject/SoftObjectPath.h"
#include "Widgets/Docking/SDockTab.h"

namespace UE::UAI::DomainAdapters::WorldSpatial
{
	namespace
	{
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

		bool Fail(
			FUeAgentWorldSpatialExecutionError& OutError,
			FString Code,
			FString SubjectId,
			FString Detail = FString())
		{
			OutError.Code = MoveTemp(Code);
			OutError.SubjectId = MoveTemp(SubjectId);
			OutError.Detail = MoveTemp(Detail);
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
					return Fail(
						OutError,
						TEXT("world_spatial_explicit_object_ref_missing"),
						ExplicitRef);
				}

				UWorld* ScopedWorld = World.Get();
				if (!ScopedWorld)
				{
					return Fail(
						OutError,
						TEXT("world_spatial_explicit_world_expired"),
						ExplicitRef);
				}
				if (ExplicitRef == ScopedWorld->GetPathName())
				{
					OutObject = ScopedWorld;
					return true;
				}

				OutObject = StaticFindObject(
					UObject::StaticClass(), nullptr, *ExplicitRef, true);
				if (!OutObject)
				{
					const FSoftObjectPath SoftPath(ExplicitRef);
					if (SoftPath.IsValid())
					{
						OutObject = SoftPath.TryLoad();
					}
				}
				if (!OutObject || OutObject->GetPathName() != ExplicitRef)
				{
					OutObject = nullptr;
					return Fail(
						OutError,
						TEXT("world_spatial_explicit_object_unresolved"),
						ExplicitRef);
				}

				const UWorld* ObjectWorld = nullptr;
				if (const AActor* Actor = Cast<AActor>(OutObject))
				{
					ObjectWorld = Actor->GetWorld();
				}
				else if (const UActorComponent* Component =
					Cast<UActorComponent>(OutObject))
				{
					ObjectWorld = Component->GetWorld();
				}
				if (ObjectWorld && ObjectWorld != ScopedWorld)
				{
					OutObject = nullptr;
					return Fail(
						OutError,
						TEXT("world_spatial_object_world_mismatch"),
						ExplicitRef,
						ScopedWorld->GetPathName());
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
					return Fail(
						OutError,
						TEXT("world_spatial_graph_identity_type_invalid"),
						AssetEditorIdentity,
						TEXT("asset_editor_identity must be an exact UEdGraph path"));
				}
				OutGraphEditor = SGraphEditor::FindGraphEditorForGraph(Graph);
				return OutGraphEditor.IsValid()
					? true
					: Fail(
						OutError,
						TEXT("world_spatial_graph_editor_not_open"),
						AssetEditorIdentity);
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
					return Fail(
						OutError,
						TEXT("world_spatial_explicit_viewport_unresolved"),
						ViewportIdentity);
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
					return Fail(
						OutError,
						TEXT("world_spatial_reverse_object_invalid"),
						TEXT("null"));
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
					return Fail(
						OutError,
						TEXT("world_spatial_reverse_object_world_mismatch"),
						Object->GetPathName());
				}
				OutExplicitRef = Object->GetPathName();
				return !OutExplicitRef.IsEmpty();
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
					return Fail(
						OutError,
						TEXT("world_spatial_mesh_selection_identities_missing"),
						ViewportIdentity);
				}

				const FUeAgentWorldSpatialMeshSelectionBinding* Session =
					MeshSelectionByViewport.Find(ViewportIdentity);
				OutManager = Session ? Session->Manager : nullptr;
				if (!Session || !OutManager)
				{
					return Fail(
						OutError,
						TEXT("world_spatial_explicit_mesh_selection_unavailable"),
						ViewportIdentity);
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
				if (!OutManager->HasActiveTargets()
					|| OutManager->GetAnyCurrentTargetsLocked()
					|| ActiveTargetCount < SelectionIdentities.Num())
				{
					return Fail(
						OutError,
						TEXT("world_spatial_mesh_selection_targets_inactive"),
						ViewportIdentity);
				}

				TSet<FString> Seen;
				for (const FString& Identity : SelectionIdentities)
				{
					if (Seen.Contains(Identity))
					{
						return Fail(
							OutError,
							TEXT("world_spatial_mesh_selection_identity_duplicate"),
							Identity);
					}
					Seen.Add(Identity);
					const FUeAgentWorldSpatialMeshSelectionTarget* Target =
						Session->Targets.FindByPredicate(
							[&Identity](
								const FUeAgentWorldSpatialMeshSelectionTarget& Item)
							{
								return Item.ExplicitRef == Identity;
							});
					if (!Target || !Target->IsValid())
					{
						return Fail(
							OutError,
							TEXT("world_spatial_mesh_selection_identity_unresolved"),
							Identity);
					}
					OutTargets.Add(*Target);
				}
				return !OutTargets.IsEmpty();
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
				if (!Value.IsValid() || NativeFrameId.IsEmpty()
					|| NativeUnit.IsEmpty())
				{
					return Fail(
						OutError,
						TEXT("world_spatial_conversion_contract_invalid"),
						Value.Subject);
				}
				if (Value.Frame.FrameId != NativeFrameId
					|| Value.Frame.Unit != NativeUnit)
				{
					return Fail(
						OutError,
						TEXT("world_spatial_conversion_path_unavailable"),
						Value.Subject,
						Value.Frame.FrameId + TEXT("/") + Value.Frame.Unit
							+ TEXT(" -> ") + NativeFrameId + TEXT("/")
							+ NativeUnit);
				}

				TArray<double> Components;
				Components.Reserve(Value.Components.Num());
				for (const FString& Lexeme : Value.Components)
				{
					double Parsed = 0.0;
					if (!LexTryParseString(Parsed, *Lexeme)
						|| !FMath::IsFinite(Parsed))
					{
						return Fail(
							OutError,
							TEXT("world_spatial_numeric_component_invalid"),
							Value.Subject,
							Lexeme);
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
				return OutValue.IsValid();
			}

			bool TryConvertFromNative(
				const FUeAgentWorldSpatialCompiledValue& Value,
				const FWorldSpatialFrame& SemanticFrame,
				FWorldSpatialValue& OutValue,
				FUeAgentWorldSpatialExecutionError& OutError) override
			{
				OutValue = {};
				OutError = {};
				if (!Value.IsValid() || !SemanticFrame.IsValid()
					|| Value.Dimension != SemanticFrame.Dimension)
				{
					return Fail(
						OutError,
						TEXT("world_spatial_readback_conversion_invalid"),
						Value.Subject);
				}
				if (Value.FrameId != SemanticFrame.FrameId
					|| Value.Unit != SemanticFrame.Unit)
				{
					return Fail(
						OutError,
						TEXT("world_spatial_readback_conversion_path_unavailable"),
						Value.Subject,
						Value.FrameId + TEXT("/") + Value.Unit + TEXT(" -> ")
							+ SemanticFrame.FrameId + TEXT("/")
							+ SemanticFrame.Unit);
				}

				OutValue.Subject = Value.Subject;
				OutValue.Quantity = Value.Quantity;
				OutValue.Frame = SemanticFrame;
				for (const double Component : Value.Components)
				{
					if (!FMath::IsFinite(Component))
					{
						return Fail(
							OutError,
							TEXT("world_spatial_readback_component_invalid"),
							Value.Subject);
					}
					OutValue.Components.Add(
						FString::SanitizeFloat(Component, 9));
				}
				return OutValue.IsValid();
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
					return Fail(
						OutError,
						TEXT("world_spatial_revision_ref_missing"),
						ExplicitRef);
				}

				uint32 Hash = 0;
				FEditorViewportClient* Client = nullptr;
				FViewport* Viewport = nullptr;
				FUeAgentWorldSpatialExecutionError ViewportError;
				if (Environment->TryResolveViewport(
						ExplicitRef, Client, Viewport, ViewportError))
				{
					const FString State = FString::Printf(
						TEXT("%s|%s|%.17g|%d|%d|%d|%d|%d"),
						*Client->GetViewLocation().ToString(),
						*Client->GetViewRotation().ToString(),
						Client->ViewFOV,
						static_cast<int32>(Client->GetViewMode()),
						Client->IsRealtime() ? 1 : 0,
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
						return false;
					}
					Hash = HashObjectSpatialState(*Object);
				}

				OutRevision.SourceId = ExplicitRef;
				OutRevision.Generation = Generation;
				OutRevision.Sequence = static_cast<int64>(Hash);
				return OutRevision.IsValid();
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
		FString& OutError)
	{
		OutViewportIdentity.Reset();
		OutError.Reset();
		if (!IsInGameThread() || !::IsValid(&Blueprint)
			|| !Client.GetWorld() || !Client.GetWorld()->Scene
			|| Client.Viewport != &Viewport
			|| Viewport.GetSizeXY().X <= 0 || Viewport.GetSizeXY().Y <= 0)
		{
			OutError = TEXT("world_spatial_blueprint_preview_session_invalid");
			return false;
		}
		OutViewportIdentity = MakeBlueprintComponentsViewportIdentity(Blueprint);
		TMap<FString, FBlueprintComponentsViewportSession>& Sessions =
			BlueprintComponentsViewportSessions();
		if (Sessions.Contains(OutViewportIdentity))
		{
			OutError = TEXT("world_spatial_blueprint_preview_session_duplicate");
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

	bool FUeAgentWorldSpatialViewportBinding::IsValidForWorld(
		const UWorld& ExplicitWorld) const
	{
		return !Identity.IsEmpty() && Client && Viewport
			&& Client->Viewport == Viewport
			&& Client->GetWorld() == &ExplicitWorld;
	}

	bool FUeAgentWorldSpatialMeshSelectionBinding::IsValid() const
	{
		if (ViewportIdentity.IsEmpty() || !::IsValid(Manager)
			|| Targets.IsEmpty())
		{
			return false;
		}
		TSet<FString> Identities;
		for (const FUeAgentWorldSpatialMeshSelectionTarget& Target : Targets)
		{
			if (!Target.IsValid() || Identities.Contains(Target.ExplicitRef))
			{
				return false;
			}
			Identities.Add(Target.ExplicitRef);
		}
		return true;
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
		if (!::IsValid(&ExplicitWorld) || ExplicitWorld.IsUnreachable()
			|| RuntimeGeneration.IsEmpty() || ServerSessionId.IsEmpty())
		{
			return Fail(
				OutError,
				TEXT("world_spatial_production_dependencies_invalid"),
				ExplicitWorld.GetPathName());
		}
		TSet<FString> ViewportIds;
		for (const FUeAgentWorldSpatialViewportBinding& Binding : ViewportBindings)
		{
			if (!Binding.IsValidForWorld(ExplicitWorld)
				|| ViewportIds.Contains(Binding.Identity))
			{
				return Fail(
					OutError,
					TEXT("world_spatial_explicit_viewport_binding_invalid"),
					Binding.Identity);
			}
			ViewportIds.Add(Binding.Identity);
		}
		TSet<FString> SelectionViewportIds;
		for (const FUeAgentWorldSpatialMeshSelectionBinding& Binding
			: MeshSelectionBindings)
		{
			const bool bTargetsMatchWorld = Binding.Targets.ContainsByPredicate(
				[&ExplicitWorld](
					const FUeAgentWorldSpatialMeshSelectionTarget& Target)
				{
					return !Target.Component
						|| Target.Component->GetWorld() != &ExplicitWorld;
				}) == false;
			if (!Binding.IsValid() || !bTargetsMatchWorld
				|| !ViewportIds.Contains(Binding.ViewportIdentity)
				|| SelectionViewportIds.Contains(Binding.ViewportIdentity))
			{
				return Fail(
					OutError,
					TEXT("world_spatial_mesh_selection_binding_invalid"),
					Binding.ViewportIdentity);
			}
			SelectionViewportIds.Add(Binding.ViewportIdentity);
		}

		const TSharedRef<FProductionEnvironment> Environment =
			MakeShared<FProductionEnvironment>(
				ExplicitWorld, ViewportBindings, MeshSelectionBindings);
		OutDependencies.Environment = Environment;
		OutDependencies.SpaceCompiler = MakeShared<FProductionSpaceCompiler>();
		OutDependencies.RevisionSource =
			MakeShared<FProductionRevisionSource>(Environment, RuntimeGeneration);
		return OutDependencies.IsValid();
	}
}

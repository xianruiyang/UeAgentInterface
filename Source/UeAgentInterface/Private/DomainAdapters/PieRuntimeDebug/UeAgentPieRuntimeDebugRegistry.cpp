// Copyright Epic Games, Inc. All Rights Reserved.

#include "DomainAdapters/PieRuntimeDebug/UeAgentPieRuntimeDebugRegistry.h"

#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Camera/PlayerCameraManager.h"
#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "Core/ArtifactPublisher/UeAgentArtifactPublisher.h"
#include "Core/HjsonWorkspace/UeAgentHjsonWorkspace.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/GameViewportClient.h"
#include "Engine/Level.h"
#include "Engine/LocalPlayer.h"
#include "Engine/SceneCapture2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Slate/SceneViewport.h"
#include "Components/SceneCaptureComponent2D.h"
#include "RenderingThread.h"

THIRD_PARTY_INCLUDES_START
#include <openssl/sha.h>
THIRD_PARTY_INCLUDES_END

namespace UE::UAI::DomainAdapters::PieRuntimeDebug
{
	using namespace ArtifactPublisher;
	using namespace ContextResource;
	using namespace HjsonWorkspace;

	FString MakePieCameraSourceIdentity(
		const EUeAgentPieCameraReadSource Source,
		const FString& ExactObjectIdentity)
	{
		return (Source == EUeAgentPieCameraReadSource::PlayerCamera
			? TEXT("pie-player-camera:") : TEXT("scene-camera:"))
			+ ExactObjectIdentity;
	}

	FString MakePieTransientCameraSourceIdentity(const FString& OperationId)
	{
		return TEXT("scene-camera-transient:") + OperationId;
	}

	namespace
	{
		const TCHAR* SchemaId = TEXT("uai.pie.runtime/1");
		const TCHAR* EntryFile = TEXT("current.hjson");
		const TCHAR* PublisherOwnerId = TEXT("pie_runtime_debug_registry");

		bool Fail(FString& OutError, const TCHAR* Code)
		{
			OutError = Code;
			return false;
		}

		FUeAgentResourceTracker MakeResources()
		{
			return FUeAgentResourceTracker(FUeAgentResourceBudget({
				{EUeAgentResourceDimension::InputBytes, 16 * 1024 * 1024},
				{EUeAgentResourceDimension::OutputBytes, 16 * 1024 * 1024},
				{EUeAgentResourceDimension::StringBytes, 16 * 1024 * 1024},
				{EUeAgentResourceDimension::Nodes, 131072},
				{EUeAgentResourceDimension::Items, 131072},
				{EUeAgentResourceDimension::Files, 8},
				{EUeAgentResourceDimension::ReferenceEdges, 131072},
				{EUeAgentResourceDimension::WorkUnits, 524288},
				{EUeAgentResourceDimension::EstimatedMemoryBytes, 256 * 1024 * 1024},
			}));
		}

		FString Sha256(const TArray<uint8>& Bytes)
		{
			uint8 Digest[SHA256_DIGEST_LENGTH];
			SHA256(
				reinterpret_cast<const unsigned char*>(Bytes.GetData()),
				static_cast<size_t>(Bytes.Num()),
				Digest);
			FString Result(TEXT("sha256:"));
			for (const uint8 Byte : Digest)
			{
				Result += FString::Printf(TEXT("%02x"), Byte);
			}
			return Result;
		}

		TSharedRef<FJsonObject> VectorJson(const FVector& Value)
		{
			const TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetNumberField(TEXT("x"), Value.X);
			Result->SetNumberField(TEXT("y"), Value.Y);
			Result->SetNumberField(TEXT("z"), Value.Z);
			return Result;
		}

		TSharedRef<FJsonObject> RotatorJson(const FRotator& Value)
		{
			const TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetNumberField(TEXT("pitch"), Value.Pitch);
			Result->SetNumberField(TEXT("yaw"), Value.Yaw);
			Result->SetNumberField(TEXT("roll"), Value.Roll);
			return Result;
		}

		TSharedRef<FJsonObject> TransformJson(const FTransform& Transform)
		{
			const TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetObjectField(TEXT("location"), VectorJson(Transform.GetLocation()));
			Result->SetObjectField(TEXT("rotation"), RotatorJson(Transform.Rotator()));
			Result->SetObjectField(TEXT("scale"), VectorJson(Transform.GetScale3D()));
			return Result;
		}

		FString ObjectIdentity(const UObject* Object)
		{
			return IsValid(Object) ? Object->GetPathName() : FString();
		}

		TSharedRef<FJsonObject> BuildWorkspaceJson(
			UWorld& World,
			ULocalPlayer& LocalPlayer,
			APlayerController& Controller,
			const FString& SessionRef,
			int32 PieInstance,
			int32 LocalPlayerIndex,
			int64 PauseEpoch,
			int64 WorkspaceRevision,
			const FString& OutputSurfaceIdentity)
		{
			const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
			Root->SetStringField(TEXT("schema"), SchemaId);
			Root->SetStringField(TEXT("session_ref"), SessionRef);
			Root->SetNumberField(TEXT("pie_instance"), PieInstance);
			Root->SetNumberField(TEXT("local_player_index"), LocalPlayerIndex);
			Root->SetNumberField(TEXT("pause_epoch"), PauseEpoch);
			Root->SetNumberField(TEXT("workspace_revision"), WorkspaceRevision);
			Root->SetStringField(TEXT("world"), ObjectIdentity(&World));
			const TSharedRef<FJsonObject> OutputSurface = MakeShared<FJsonObject>();
			OutputSurface->SetStringField(TEXT("identity"), OutputSurfaceIdentity);
			FSceneViewport* CurrentSceneViewport = World.GetGameViewport()
				? World.GetGameViewport()->GetGameViewport() : nullptr;
			const FIntPoint OutputSize = CurrentSceneViewport
				? CurrentSceneViewport->GetSizeXY() : FIntPoint::ZeroValue;
			OutputSurface->SetNumberField(TEXT("width"), OutputSize.X);
			OutputSurface->SetNumberField(TEXT("height"), OutputSize.Y);
			OutputSurface->SetBoolField(TEXT("ready"),
				CurrentSceneViewport && OutputSize.X > 0 && OutputSize.Y > 0);
			Root->SetObjectField(TEXT("pie_output_surface"), OutputSurface);

			TArray<TSharedPtr<FJsonValue>> Levels;
			for (ULevel* Level : World.GetLevels())
			{
				if (IsValid(Level))
				{
					Levels.Add(MakeShared<FJsonValueString>(Level->GetPathName()));
				}
			}
			Root->SetArrayField(TEXT("levels"), MoveTemp(Levels));

			TArray<TSharedPtr<FJsonValue>> Actors;
			TArray<TSharedPtr<FJsonValue>> SceneCameras;
			for (TActorIterator<AActor> It(&World); It; ++It)
			{
				AActor* Actor = *It;
				const TSharedRef<FJsonObject> ActorJson = MakeShared<FJsonObject>();
				ActorJson->SetStringField(TEXT("identity"), ObjectIdentity(Actor));
				ActorJson->SetStringField(TEXT("class"), ObjectIdentity(Actor->GetClass()));
				ActorJson->SetStringField(TEXT("owner"), ObjectIdentity(Actor->GetOwner()));
				ActorJson->SetStringField(TEXT("attached_to"), ObjectIdentity(Actor->GetAttachParentActor()));
				ActorJson->SetObjectField(TEXT("transform"), TransformJson(Actor->GetActorTransform()));
				const FBox Bounds = Actor->GetComponentsBoundingBox(true);
				const TSharedRef<FJsonObject> BoundsJson = MakeShared<FJsonObject>();
				BoundsJson->SetBoolField(TEXT("valid"), Bounds.IsValid != 0);
				BoundsJson->SetObjectField(TEXT("min"), VectorJson(Bounds.Min));
				BoundsJson->SetObjectField(TEXT("max"), VectorJson(Bounds.Max));
				ActorJson->SetObjectField(TEXT("bounds"), BoundsJson);
				ActorJson->SetBoolField(TEXT("hidden"), Actor->IsHidden());

				TArray<TSharedPtr<FJsonValue>> Components;
				for (UActorComponent* Component : Actor->GetComponents())
				{
					if (!IsValid(Component))
					{
						continue;
					}
					const TSharedRef<FJsonObject> ComponentJson = MakeShared<FJsonObject>();
					ComponentJson->SetStringField(TEXT("identity"), ObjectIdentity(Component));
					ComponentJson->SetStringField(TEXT("class"), ObjectIdentity(Component->GetClass()));
					ComponentJson->SetStringField(TEXT("owner"), ObjectIdentity(Component->GetOwner()));
					ComponentJson->SetBoolField(TEXT("active"), Component->IsActive());
					if (const USceneComponent* Scene = Cast<USceneComponent>(Component))
					{
						ComponentJson->SetBoolField(TEXT("visible"), Scene->IsVisible());
						ComponentJson->SetStringField(TEXT("attached_to"), ObjectIdentity(Scene->GetAttachParent()));
						ComponentJson->SetObjectField(TEXT("transform"), TransformJson(Scene->GetComponentTransform()));
					}
					Components.Add(MakeShared<FJsonValueObject>(ComponentJson));
				}
				ActorJson->SetArrayField(TEXT("components"), MoveTemp(Components));
				Actors.Add(MakeShared<FJsonValueObject>(ActorJson));
				if (ACameraActor* CameraActor = Cast<ACameraActor>(Actor))
				{
					UCameraComponent* CameraComponent = CameraActor->GetCameraComponent();
					if (IsValid(CameraComponent))
					{
						FMinimalViewInfo View;
						CameraComponent->GetCameraView(0.0f, View);
						const TSharedRef<FJsonObject> CameraJson = MakeShared<FJsonObject>();
						CameraJson->SetStringField(TEXT("identity"), ObjectIdentity(CameraActor));
						CameraJson->SetObjectField(TEXT("location"), VectorJson(View.Location));
						CameraJson->SetObjectField(TEXT("rotation"), RotatorJson(View.Rotation));
						CameraJson->SetNumberField(TEXT("fov"), View.FOV);
						CameraJson->SetStringField(TEXT("projection"),
							View.ProjectionMode == ECameraProjectionMode::Perspective
								? TEXT("perspective") : TEXT("orthographic"));
						SceneCameras.Add(MakeShared<FJsonValueObject>(CameraJson));
					}
				}
			}
			Root->SetArrayField(TEXT("actors"), MoveTemp(Actors));
			Root->SetArrayField(TEXT("scene_cameras"), MoveTemp(SceneCameras));

			const TSharedRef<FJsonObject> Player = MakeShared<FJsonObject>();
			Player->SetStringField(TEXT("local_player"), ObjectIdentity(&LocalPlayer));
			Player->SetStringField(TEXT("controller"), ObjectIdentity(&Controller));
			Player->SetStringField(TEXT("pawn"), ObjectIdentity(Controller.GetPawn()));
			if (const APlayerCameraManager* Camera = Controller.PlayerCameraManager)
			{
				const FMinimalViewInfo& View = Camera->GetCameraCacheView();
				const TSharedRef<FJsonObject> CameraJson = MakeShared<FJsonObject>();
				CameraJson->SetStringField(TEXT("identity"), MakePieCameraSourceIdentity(
					EUeAgentPieCameraReadSource::PlayerCamera, Camera->GetPathName()));
				CameraJson->SetStringField(TEXT("view_target"), ObjectIdentity(Controller.GetViewTarget()));
				CameraJson->SetObjectField(TEXT("location"), VectorJson(View.Location));
				CameraJson->SetObjectField(TEXT("rotation"), RotatorJson(View.Rotation));
				CameraJson->SetNumberField(TEXT("fov"), View.FOV);
				CameraJson->SetStringField(TEXT("projection"),
					View.ProjectionMode == ECameraProjectionMode::Perspective
						? TEXT("perspective") : TEXT("orthographic"));
				Player->SetObjectField(TEXT("evaluated_camera"), CameraJson);
			}
			Root->SetObjectField(TEXT("player"), Player);

			const TSharedRef<FJsonObject> RegisteredFields = MakeShared<FJsonObject>();
			RegisteredFields->SetStringField(TEXT("extension_contract"), TEXT("registered_fields_only"));
			RegisteredFields->SetArrayField(
				TEXT("values"),
				TArray<TSharedPtr<FJsonValue>>{});
			Root->SetObjectField(TEXT("registered_fields"), RegisteredFields);
			return Root;
		}

		class FWorkspaceVerification final : public IArtifactPublishVerification
		{
		public:
			FWorkspaceVerification(
				TWeakObjectPtr<UWorld> InWorld,
				int32 InPieInstance,
				FString InSessionRef,
				int64 InPauseEpoch,
				int64 InWorkspaceRevision)
				: World(InWorld)
				, PieInstance(InPieInstance)
				, SessionRef(MoveTemp(InSessionRef))
				, PauseEpoch(InPauseEpoch)
				, WorkspaceRevision(InWorkspaceRevision)
			{
			}

			bool TryVerifyStagedWorkspace(
				const FString& StagedWorkspacePath,
				EWorkspaceKind WorkspaceKind,
				const FString& InEntryFile,
				FUeAgentResourceTracker& Resources,
				FArtifactPublishError& OutError) override
			{
				if (WorkspaceKind != EWorkspaceKind::Directory || InEntryFile != EntryFile)
				{
					OutError.Set(TEXT("pie_runtime_workspace_shape_invalid"));
					return false;
				}
				FString Text;
				if (!FFileHelper::LoadFileToString(Text, *(StagedWorkspacePath / EntryFile)))
				{
					OutError.Set(TEXT("pie_runtime_workspace_read_failed"));
					return false;
				}
				FHjsonDocumentPtr HjsonDocument;
				FWorkspaceError HjsonError;
				if (!TryParseHjsonDocument(
						Text,
						StagedWorkspacePath / EntryFile,
						Resources,
						HjsonDocument,
						HjsonError)
					|| !HjsonDocument.IsValid())
				{
					OutError.Set(
						TEXT("pie_runtime_workspace_hjson_parse_failed"),
						StagedWorkspacePath / EntryFile,
						HjsonError.GetCode());
					return false;
				}
				const FHjsonNode* Schema = HjsonDocument->Find(TEXT("/schema"));
				const FHjsonNode* Session = HjsonDocument->Find(TEXT("/session_ref"));
				const FHjsonNode* Epoch = HjsonDocument->Find(TEXT("/pause_epoch"));
				const FHjsonNode* Revision =
					HjsonDocument->Find(TEXT("/workspace_revision"));
				int64 ObservedEpoch = 0;
				int64 ObservedRevision = 0;
				if (!Schema || Schema->GetKind() != EHjsonNodeKind::String
					|| Schema->GetText() != SchemaId
					|| !Session || Session->GetKind() != EHjsonNodeKind::String
					|| Session->GetText() != SessionRef
					|| !Epoch || Epoch->GetKind() != EHjsonNodeKind::Integer
					|| !LexTryParseString(ObservedEpoch, *Epoch->GetLexeme())
					|| ObservedEpoch != PauseEpoch
					|| !Revision || Revision->GetKind() != EHjsonNodeKind::Integer
					|| !LexTryParseString(ObservedRevision, *Revision->GetLexeme())
					|| ObservedRevision != WorkspaceRevision)
				{
					OutError.Set(TEXT("pie_runtime_workspace_identity_mismatch"));
					return false;
				}
				return true;
			}

			bool TryRecheckTargetVersion(FArtifactPublishError& OutError) override
			{
				UEditorEngine* EditorEngine = Cast<UEditorEngine>(GEditor);
				FWorldContext* ExactContext = EditorEngine
					? EditorEngine->GetPIEWorldContext(PieInstance)
					: nullptr;
				if (!IsInGameThread()
					|| !World.IsValid()
					|| !ExactContext
					|| ExactContext->World() != World.Get()
					|| !World->IsPaused())
				{
					OutError.Set(TEXT("pie_runtime_world_changed_or_unpaused"));
					return false;
				}
				return true;
			}

			TMap<FString, FString> GetSemanticVerificationFields() const override
			{
				return {
					{TEXT("schema"), SchemaId},
					{TEXT("session_ref"), SessionRef},
					{TEXT("pause_epoch"), FString::Printf(TEXT("%lld"), PauseEpoch)},
					{TEXT("workspace_revision"), FString::Printf(TEXT("%lld"), WorkspaceRevision)},
				};
			}

		private:
			TWeakObjectPtr<UWorld> World;
			int32 PieInstance = INDEX_NONE;
			FString SessionRef;
			int64 PauseEpoch = 0;
			int64 WorkspaceRevision = 0;
		};
	}

	struct FUeAgentPieRuntimeDebugRegistryState
	{
		FUeAgentPieRuntimeDebugSnapshot Snapshot;
		FString OutputSurfaceIdentity;
		bool bWasPausedOnActivation = false;
	};

	namespace
	{
		bool IsExactWorld(const FUeAgentPieRuntimeDebugRegistryState& State)
		{
			UEditorEngine* EditorEngine = Cast<UEditorEngine>(GEditor);
			FWorldContext* ExactContext = EditorEngine
				? EditorEngine->GetPIEWorldContext(State.Snapshot.PieInstance)
				: nullptr;
			return State.Snapshot.World.IsValid()
				&& ExactContext
				&& ExactContext->World() == State.Snapshot.World.Get();
		}

		bool SnapshotState(
			const FUeAgentPieRuntimeDebugRegistryState& State,
			FUeAgentPieRuntimeDebugSnapshot& OutSnapshot,
			FString& OutError)
		{
			if (!IsExactWorld(State)
				|| !State.Snapshot.LocalPlayer.IsValid()
				|| !State.Snapshot.PlayerController.IsValid()
				|| State.Snapshot.LocalPlayer->GetPlayerController(
					State.Snapshot.World.Get())
					!= State.Snapshot.PlayerController.Get())
			{
				return Fail(OutError, TEXT("pie_runtime_session_objects_invalid"));
			}
			OutSnapshot = State.Snapshot;
			return true;
		}

		bool PublishWorkspace(
			FUeAgentPieRuntimeDebugRegistryState& State,
			int64 NewRevision,
			FString& OutError)
		{
			UWorld* World = State.Snapshot.World.Get();
			ULocalPlayer* LocalPlayer = State.Snapshot.LocalPlayer.Get();
			APlayerController* Controller = State.Snapshot.PlayerController.Get();
			if (!IsInGameThread() || !World || !LocalPlayer || !Controller || !World->IsPaused())
			{
				return Fail(OutError, TEXT("pie_runtime_publish_target_invalid"));
			}
			const TSharedRef<FJsonObject> Root = BuildWorkspaceJson(
				*World,
				*LocalPlayer,
				*Controller,
				State.Snapshot.SessionRef,
				State.Snapshot.PieInstance,
				State.Snapshot.LocalPlayerIndex,
				State.Snapshot.PauseEpoch,
				NewRevision,
				State.OutputSurfaceIdentity);
			FString Text;
			const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Text);
			if (!FJsonSerializer::Serialize(Root, Writer))
			{
				return Fail(OutError, TEXT("pie_runtime_workspace_serialize_failed"));
			}
			FTCHARToUTF8 Utf8(*Text);
			TArray<uint8> Bytes;
			Bytes.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
			const FString ContentHash = Sha256(Bytes);
			TArray<FExportFilePlan> Files;
			Files.Emplace(EntryFile, Bytes, ContentHash);
			const FString Fingerprint = Sha256(Bytes);
			const FExportWorkspacePlanRef Plan = MakeShared<FExportWorkspacePlan>(
				EWorkspaceKind::Directory,
				EntryFile,
				TEXT("pie-runtime:") + State.Snapshot.SessionRef,
				FUeAgentStateRevision(
					State.Snapshot.SessionRef,
					TEXT("workspace-revision"),
					NewRevision),
				TEXT("paused-world-snapshot"),
				MoveTemp(Files),
				Fingerprint,
				(Bytes.Num() + 3ll) / 4ll);
			FUeAgentResourceTracker Resources = MakeResources();
			FArtifactPublishReceipt Receipt;
			FArtifactPublishError Error;
			const TSharedRef<FWorkspaceVerification> Verification =
				MakeShared<FWorkspaceVerification>(
					State.Snapshot.World,
					State.Snapshot.PieInstance,
					State.Snapshot.SessionRef,
					State.Snapshot.PauseEpoch,
					NewRevision);
			if (!TryPublishExportPlan(
					FArtifactPublishRequest(
						Plan,
						State.Snapshot.WorkspacePath,
						PublisherOwnerId,
						Verification),
					Resources,
					Receipt,
					Error))
			{
				OutError = Error.GetCode() + TEXT(":") + Error.GetDetail();
				return false;
			}
			State.Snapshot.WorkspaceRevision = NewRevision;
			return true;
		}
	}

	FUeAgentPieRuntimeDebugRegistry::FUeAgentPieRuntimeDebugRegistry() = default;
	FUeAgentPieRuntimeDebugRegistry::~FUeAgentPieRuntimeDebugRegistry() = default;

	bool FUeAgentPieRuntimeDebugRegistry::TryActivate(
		int32 RequestedPieInstance,
		int32 RequestedLocalPlayerIndex,
		FUeAgentPieRuntimeDebugSnapshot& OutSnapshot,
		FString& OutError)
	{
		FScopeLock Lock(&Mutex);
		if (!IsInGameThread()) return Fail(OutError, TEXT("pie_runtime_game_thread_required"));
		if (State.IsValid()) return Fail(OutError, TEXT("pie_runtime_session_already_active"));
		UEditorEngine* EditorEngine = Cast<UEditorEngine>(GEditor);
		if (!EditorEngine) return Fail(OutError, TEXT("pie_runtime_editor_unavailable"));
		if (!GEngine) return Fail(OutError, TEXT("pie_runtime_engine_unavailable"));
		FWorldContext* ExactContext = EditorEngine->GetPIEWorldContext(RequestedPieInstance);
		if (!ExactContext || !ExactContext->World()) return Fail(OutError, TEXT("pie_runtime_instance_not_found"));
		int32 PieWorldCount = 0;
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			if (Context.WorldType == EWorldType::PIE && Context.World()) ++PieWorldCount;
		}
		if (PieWorldCount != 1) return Fail(OutError, TEXT("pie_runtime_requires_exactly_one_pie_world"));
		UWorld* World = ExactContext->World();
		UGameInstance* GameInstance = World->GetGameInstance();
		if (!GameInstance) return Fail(OutError, TEXT("pie_runtime_game_instance_missing"));
		const TArray<ULocalPlayer*>& LocalPlayers = GameInstance->GetLocalPlayers();
		if (!LocalPlayers.IsValidIndex(RequestedLocalPlayerIndex) || LocalPlayers.Num() != 1)
		{
			return Fail(OutError, TEXT("pie_runtime_requires_exactly_one_local_player"));
		}
		ULocalPlayer* LocalPlayer = LocalPlayers[RequestedLocalPlayerIndex];
		APlayerController* Controller = LocalPlayer
			? LocalPlayer->GetPlayerController(World)
			: nullptr;
		if (!Controller) return Fail(OutError, TEXT("pie_runtime_player_controller_missing"));
		const bool bWasPaused = World->IsPaused();
		if (!EditorEngine->SetPIEWorldsPaused(true) || !World->IsPaused())
			return Fail(OutError, TEXT("pie_runtime_pause_verification_failed"));
		EditorEngine->PlaySessionPaused();

		TUniquePtr<FUeAgentPieRuntimeDebugRegistryState> Candidate =
			MakeUnique<FUeAgentPieRuntimeDebugRegistryState>();
		Candidate->Snapshot.World = World;
		Candidate->Snapshot.LocalPlayer = LocalPlayer;
		Candidate->Snapshot.PlayerController = Controller;
		Candidate->Snapshot.SessionRef = FString::Printf(
			TEXT("runtime-session:pie-debug:%d:%s"),
			RequestedPieInstance,
			*FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower));
		Candidate->Snapshot.PieInstance = RequestedPieInstance;
		Candidate->Snapshot.LocalPlayerIndex = RequestedLocalPlayerIndex;
		Candidate->Snapshot.PauseEpoch = 1;
		Candidate->Snapshot.WorkspaceRevision = 0;
		Candidate->Snapshot.WorkspacePath = FPaths::ProjectSavedDir()
			/ TEXT("UeAgentInterface/RuntimeDebug")
			/ FString::Printf(TEXT("PIE_%d"), RequestedPieInstance)
			/ TEXT("Current");
		Candidate->OutputSurfaceIdentity = TEXT("pie-output-surface:")
			+ ExactContext->ContextHandle.ToString();
		Candidate->bWasPausedOnActivation = bWasPaused;
		if (!PublishWorkspace(*Candidate, 1, OutError))
		{
			if (!EditorEngine->SetPIEWorldsPaused(bWasPaused)
				|| World->IsPaused() != bWasPaused)
			{
				OutError += TEXT(":pause_restore_failed");
			}
			else if (!bWasPaused)
			{
				EditorEngine->PlaySessionResumed();
			}
			return false;
		}
		State = MoveTemp(Candidate);
		return SnapshotState(*State, OutSnapshot, OutError);
	}

	bool FUeAgentPieRuntimeDebugRegistry::TryResolveExact(
		const FString& SessionRef,
		int64 ExpectedPauseEpoch,
		int64 ExpectedWorkspaceRevision,
		FUeAgentPieRuntimeDebugSnapshot& OutSnapshot,
		FString& OutError) const
	{
		FScopeLock Lock(&Mutex);
		if (!IsInGameThread()) return Fail(OutError, TEXT("pie_runtime_game_thread_required"));
		if (!State.IsValid() || State->Snapshot.SessionRef != SessionRef)
			return Fail(OutError, TEXT("pie_runtime_session_stale"));
		if (State->Snapshot.PauseEpoch != ExpectedPauseEpoch
			|| State->Snapshot.WorkspaceRevision != ExpectedWorkspaceRevision)
			return Fail(OutError, TEXT("pie_runtime_snapshot_stale"));
		return SnapshotState(*State, OutSnapshot, OutError);
	}

	bool FUeAgentPieRuntimeDebugRegistry::TryReadExactOutputSurface(
		const FString& SessionRef,
		const int64 ExpectedPauseEpoch,
		const int64 ExpectedWorkspaceRevision,
		TArray<FColor>& OutPixels,
		FIntPoint& OutSize,
		FString& OutSurfaceIdentity,
		FString& OutError) const
	{
		FScopeLock Lock(&Mutex);
		OutPixels.Reset();
		OutSize = FIntPoint::ZeroValue;
		OutSurfaceIdentity.Reset();
		if (!IsInGameThread()) return Fail(OutError, TEXT("pie_runtime_game_thread_required"));
		if (!State.IsValid() || State->Snapshot.SessionRef != SessionRef)
			return Fail(OutError, TEXT("pie_runtime_session_stale"));
		if (State->Snapshot.PauseEpoch != ExpectedPauseEpoch
			|| State->Snapshot.WorkspaceRevision != ExpectedWorkspaceRevision)
			return Fail(OutError, TEXT("pie_runtime_snapshot_stale"));

		FUeAgentPieRuntimeDebugSnapshot Snapshot;
		if (!SnapshotState(*State, Snapshot, OutError)) return false;
		UEditorEngine* EditorEngine = Cast<UEditorEngine>(GEditor);
		FWorldContext* ExactContext = EditorEngine
			? EditorEngine->GetPIEWorldContext(Snapshot.PieInstance)
			: nullptr;
		UWorld* World = Snapshot.World.Get();
		ULocalPlayer* LocalPlayer = Snapshot.LocalPlayer.Get();
		UGameViewportClient* ViewportClient = World ? World->GetGameViewport() : nullptr;
		FSceneViewport* SceneViewport = ViewportClient
			? ViewportClient->GetGameViewport()
			: nullptr;
		const FString ResolvedIdentity = ExactContext
			? TEXT("pie-output-surface:") + ExactContext->ContextHandle.ToString()
			: FString();
		if (!ExactContext
			|| ExactContext->World() != World
			|| !LocalPlayer
			|| LocalPlayer->ViewportClient != ViewportClient
			|| LocalPlayer->GetPlayerController(World) != Snapshot.PlayerController.Get()
			|| ResolvedIdentity != State->OutputSurfaceIdentity
			|| !SceneViewport)
		{
			return Fail(OutError, TEXT("pie_runtime_output_surface_identity_mismatch"));
		}
		const FIntPoint Size = SceneViewport->GetSizeXY();
		if (Size.X <= 0 || Size.Y <= 0)
		{
			return Fail(OutError, TEXT("pie_runtime_output_surface_size_invalid"));
		}
		TArray<FColor> Pixels;
		if (!SceneViewport->ReadPixels(Pixels)
			|| Pixels.Num() != Size.X * Size.Y)
		{
			return Fail(OutError, TEXT("pie_runtime_output_surface_read_failed"));
		}
		OutPixels = MoveTemp(Pixels);
		OutSize = Size;
		OutSurfaceIdentity = ResolvedIdentity;
		return true;
	}

	bool FUeAgentPieRuntimeDebugRegistry::TryReadExactCamera(
		const FString& SessionRef,
		const int64 ExpectedPauseEpoch,
		const int64 ExpectedWorkspaceRevision,
		const FUeAgentPieCameraReadRequest& Request,
		TArray<FColor>& OutPixels,
		FIntPoint& OutSize,
		FString& OutSourceIdentity,
		FString& OutError) const
	{
		FScopeLock Lock(&Mutex);
		OutPixels.Reset();
		OutSize = FIntPoint::ZeroValue;
		OutSourceIdentity.Reset();
		if (!IsInGameThread()) return Fail(OutError, TEXT("pie_runtime_game_thread_required"));
		if (!State.IsValid() || State->Snapshot.SessionRef != SessionRef)
			return Fail(OutError, TEXT("pie_runtime_session_stale"));
		if (State->Snapshot.PauseEpoch != ExpectedPauseEpoch
			|| State->Snapshot.WorkspaceRevision != ExpectedWorkspaceRevision)
			return Fail(OutError, TEXT("pie_runtime_snapshot_stale"));

		FUeAgentPieRuntimeDebugSnapshot Snapshot;
		if (!SnapshotState(*State, Snapshot, OutError)) return false;
		UEditorEngine* EditorEngine = Cast<UEditorEngine>(GEditor);
		FWorldContext* ExactContext = EditorEngine
			? EditorEngine->GetPIEWorldContext(Snapshot.PieInstance) : nullptr;
		UWorld* World = Snapshot.World.Get();
		ULocalPlayer* LocalPlayer = Snapshot.LocalPlayer.Get();
		APlayerController* Controller = Snapshot.PlayerController.Get();
		UGameViewportClient* ViewportClient = World ? World->GetGameViewport() : nullptr;
		FSceneViewport* SceneViewport = ViewportClient ? ViewportClient->GetGameViewport() : nullptr;
		const FString ResolvedSurfaceIdentity = ExactContext
			? TEXT("pie-output-surface:") + ExactContext->ContextHandle.ToString() : FString();
		if (!ExactContext || ExactContext->World() != World || !LocalPlayer || !Controller
			|| LocalPlayer->ViewportClient != ViewportClient
			|| LocalPlayer->GetPlayerController(World) != Controller
			|| ResolvedSurfaceIdentity != State->OutputSurfaceIdentity || !SceneViewport)
		{
			return Fail(OutError, TEXT("pie_runtime_output_surface_identity_mismatch"));
		}
		const FIntPoint Size = SceneViewport->GetSizeXY();
		if (Size.X <= 0 || Size.Y <= 0)
			return Fail(OutError, TEXT("pie_runtime_output_surface_size_invalid"));

		FMinimalViewInfo View;
		FString SourceIdentity;
		if (Request.Source == EUeAgentPieCameraReadSource::PlayerCamera)
		{
			if (!Request.CameraRef.IsEmpty() || Request.OverrideLocation.IsSet()
				|| Request.OverrideRotation.IsSet() || Request.OverrideFovDegrees.IsSet())
				return Fail(OutError, TEXT("pie_runtime_player_camera_override_forbidden"));
			APlayerCameraManager* Manager = Controller->PlayerCameraManager;
			if (!IsValid(Manager))
				return Fail(OutError, TEXT("pie_runtime_player_camera_unavailable"));
			View = Manager->GetCameraCacheView();
			SourceIdentity = MakePieCameraSourceIdentity(
				EUeAgentPieCameraReadSource::PlayerCamera, Manager->GetPathName());
		}
		else
		{
			if (!Request.CameraRef.IsEmpty())
			{
				ACameraActor* Camera = FindObject<ACameraActor>(nullptr, *Request.CameraRef);
				if (!IsValid(Camera) || Camera->GetWorld() != World
					|| Camera->GetPathName() != Request.CameraRef
					|| !IsValid(Camera->GetCameraComponent()))
				{
					return Fail(OutError, TEXT("pie_runtime_scene_camera_identity_mismatch"));
				}
				Camera->GetCameraComponent()->GetCameraView(0.0f, View);
				SourceIdentity = MakePieCameraSourceIdentity(
					EUeAgentPieCameraReadSource::SceneCamera, Camera->GetPathName());
			}
			else
			{
				if (!Request.OverrideLocation.IsSet() || !Request.OverrideRotation.IsSet()
					|| !Request.OverrideFovDegrees.IsSet() || Request.TransientIdentity.IsEmpty())
					return Fail(OutError, TEXT("pie_runtime_transient_camera_incomplete"));
				View.ProjectionMode = ECameraProjectionMode::Perspective;
				SourceIdentity = MakePieTransientCameraSourceIdentity(Request.TransientIdentity);
			}
		}
		if (Request.OverrideLocation.IsSet()) View.Location = Request.OverrideLocation.GetValue();
		if (Request.OverrideRotation.IsSet()) View.Rotation = Request.OverrideRotation.GetValue();
		if (Request.OverrideFovDegrees.IsSet()) View.FOV = Request.OverrideFovDegrees.GetValue();

		FActorSpawnParameters SpawnParameters;
		SpawnParameters.ObjectFlags |= RF_Transient;
		SpawnParameters.SpawnCollisionHandlingOverride =
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		ASceneCapture2D* CaptureActor = World->SpawnActor<ASceneCapture2D>(
			View.Location, View.Rotation, SpawnParameters);
		if (!IsValid(CaptureActor))
			return Fail(OutError, TEXT("pie_runtime_camera_capture_create_failed"));

		UTextureRenderTarget2D* RenderTarget = NewObject<UTextureRenderTarget2D>(
			GetTransientPackage(), NAME_None, RF_Transient);
		USceneCaptureComponent2D* Capture = CaptureActor->GetCaptureComponent2D();
		bool bSucceeded = false;
		if (IsValid(RenderTarget) && IsValid(Capture))
		{
			RenderTarget->InitCustomFormat(Size.X, Size.Y, PF_B8G8R8A8, false);
			RenderTarget->UpdateResourceImmediate(true);
			Capture->TextureTarget = RenderTarget;
			Capture->bCaptureEveryFrame = false;
			Capture->bCaptureOnMovement = false;
			Capture->FOVAngle = View.FOV;
			Capture->ProjectionType = View.ProjectionMode;
			Capture->OrthoWidth = View.OrthoWidth;
			Capture->PostProcessSettings = View.PostProcessSettings;
			Capture->PostProcessBlendWeight = View.PostProcessBlendWeight;
			Capture->SetWorldLocationAndRotation(View.Location, View.Rotation);
			Capture->CaptureScene();
			FlushRenderingCommands();
			FTextureRenderTargetResource* Resource = RenderTarget->GameThread_GetRenderTargetResource();
			bSucceeded = Resource && Resource->ReadPixels(OutPixels)
				&& OutPixels.Num() == Size.X * Size.Y;
		}
		if (IsValid(Capture)) Capture->TextureTarget = nullptr;
		if (IsValid(RenderTarget)) RenderTarget->ReleaseResource();
		CaptureActor->Destroy();
		if (!bSucceeded)
		{
			OutPixels.Reset();
			return Fail(OutError, TEXT("pie_runtime_camera_capture_read_failed"));
		}
		OutSize = Size;
		OutSourceIdentity = MoveTemp(SourceIdentity);
		return true;
	}

	bool FUeAgentPieRuntimeDebugRegistry::TryGetCachedSnapshot(
		const FString& SessionRef,
		FUeAgentPieRuntimeDebugCachedSnapshot& OutSnapshot,
		FString& OutError) const
	{
		FScopeLock Lock(&Mutex);
		if (!State.IsValid() || State->Snapshot.SessionRef != SessionRef)
			return Fail(OutError, TEXT("pie_runtime_session_stale"));
		OutSnapshot.SessionRef = State->Snapshot.SessionRef;
		OutSnapshot.PieInstance = State->Snapshot.PieInstance;
		OutSnapshot.LocalPlayerIndex = State->Snapshot.LocalPlayerIndex;
		OutSnapshot.PauseEpoch = State->Snapshot.PauseEpoch;
		OutSnapshot.WorkspaceRevision = State->Snapshot.WorkspaceRevision;
		OutSnapshot.WorkspacePath = State->Snapshot.WorkspacePath;
		return true;
	}

	bool FUeAgentPieRuntimeDebugRegistry::TryResume(const FString& SessionRef, FString& OutError)
	{
		FScopeLock Lock(&Mutex);
		if (!IsInGameThread()) return Fail(OutError, TEXT("pie_runtime_game_thread_required"));
		if (!State.IsValid() || State->Snapshot.SessionRef != SessionRef)
			return Fail(OutError, TEXT("pie_runtime_session_stale"));
		if (!IsExactWorld(*State)) return Fail(OutError, TEXT("pie_runtime_world_invalid"));
		const bool bPauseCommandSucceeded = GEditor->SetPIEWorldsPaused(false);
		if (!bPauseCommandSucceeded || State->Snapshot.World->IsPaused())
		{
			return Fail(OutError, TEXT("pie_runtime_resume_verification_failed"));
		}
		GEditor->PlaySessionResumed();
		return true;
	}

	bool FUeAgentPieRuntimeDebugRegistry::TryRepauseAndVerify(
		const FString& SessionRef,
		int64& OutPauseEpoch,
		FString& OutError)
	{
		FScopeLock Lock(&Mutex);
		if (!IsInGameThread()) return Fail(OutError, TEXT("pie_runtime_game_thread_required"));
		if (!State.IsValid() || State->Snapshot.SessionRef != SessionRef)
			return Fail(OutError, TEXT("pie_runtime_session_stale"));
		if (!IsExactWorld(*State)) return Fail(OutError, TEXT("pie_runtime_world_invalid"));
		if (!GEditor->SetPIEWorldsPaused(true)
			|| !State->Snapshot.World->IsPaused())
			return Fail(OutError, TEXT("pie_runtime_pause_verification_failed"));
		GEditor->PlaySessionPaused();
		OutPauseEpoch = ++State->Snapshot.PauseEpoch;
		return true;
	}

	bool FUeAgentPieRuntimeDebugRegistry::TryRefreshCurrentWorkspace(
		const FString& SessionRef,
		int64 ExpectedWorkspaceRevision,
		int64& OutWorkspaceRevision,
		FString& OutError)
	{
		FScopeLock Lock(&Mutex);
		if (!IsInGameThread()) return Fail(OutError, TEXT("pie_runtime_game_thread_required"));
		if (!State.IsValid() || State->Snapshot.SessionRef != SessionRef)
			return Fail(OutError, TEXT("pie_runtime_session_stale"));
		if (State->Snapshot.WorkspaceRevision != ExpectedWorkspaceRevision)
			return Fail(OutError, TEXT("pie_runtime_workspace_revision_stale"));
		if (!IsExactWorld(*State) || !State->Snapshot.World->IsPaused())
			return Fail(OutError, TEXT("pie_runtime_refresh_requires_verified_pause"));
		const int64 NewRevision = ExpectedWorkspaceRevision + 1;
		if (!PublishWorkspace(*State, NewRevision, OutError)) return false;
		OutWorkspaceRevision = NewRevision;
		return true;
	}

	bool FUeAgentPieRuntimeDebugRegistry::TryEndSession(
		const FString& SessionRef,
		FString& OutError)
	{
		FScopeLock Lock(&Mutex);
		if (!IsInGameThread()) return Fail(OutError, TEXT("pie_runtime_game_thread_required"));
		if (!State.IsValid() || State->Snapshot.SessionRef != SessionRef)
			return Fail(OutError, TEXT("pie_runtime_session_stale"));
		if (IsExactWorld(*State))
		{
			if (!GEditor->SetPIEWorldsPaused(State->bWasPausedOnActivation)
				|| State->Snapshot.World->IsPaused() != State->bWasPausedOnActivation)
			{
				return Fail(OutError, TEXT("pie_runtime_end_pause_restore_failed"));
			}
			if (!State->bWasPausedOnActivation)
			{
				GEditor->PlaySessionResumed();
			}
		}
		State.Reset();
		return true;
	}
}

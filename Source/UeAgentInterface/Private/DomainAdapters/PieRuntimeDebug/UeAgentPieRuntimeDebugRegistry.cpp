// Copyright Epic Games, Inc. All Rights Reserved.

#include "DomainAdapters/PieRuntimeDebug/UeAgentPieRuntimeDebugRegistry.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "DomainAdapters/PieRuntimeDebug/UeAgentPieRuntimeDiagnostics.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Utilities/UeAgentReadableName.h"
#include "Core/EditorCapture/UeAgentAsyncCapture.h"
#include "DomainAdapters/PieRuntimeDebug/UeAgentPieRuntimeInputState.h"
#include "InputKeyEventArgs.h"
#include "InputAction.h"
#include "Core/EditorCapture/UeAgentSlateWidgetCapture.h"

#include "Animation/AnimInstance.h"
#include "Animation/AnimMontage.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetBlueprintLibrary.h"
#include "Blueprint/WidgetTree.h"
#include "Camera/CameraActor.h"
#include "Camera/CameraTypes.h"
#include "Camera/CameraComponent.h"
#include "Camera/PlayerCameraManager.h"
#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/Widget.h"
#include "Core/ArtifactPublisher/UeAgentArtifactPublisher.h"
#include "Core/HjsonWorkspace/UeAgentHjsonWorkspace.h"
#include "Core/HjsonWorkspace/DomainAuthoring/UeAgentDomainHjsonAuthoringKit.h"
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
#include "EnhancedInputSubsystems.h"
#include "EnhancedPlayerInput.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectIterator.h"
#include "UObject/PropertyOptional.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"
#include "Utilities/UeAgentPropertyValueCodec.h"
#include "Utilities/UeAgentPropertyPath.h"
#include "Slate/SceneViewport.h"
#include "Widgets/SViewport.h"
#include "Components/SceneCaptureComponent2D.h"
#include "RenderingThread.h"
#include "Serialization/JsonSerializer.h"

namespace UE::UAI::DomainAdapters::PieRuntimeDebug
{
	using namespace PieDiagnostics;
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
		const TCHAR* EntryFile = TEXT("current.hjson");
		const TCHAR* PublisherOwnerId = TEXT("pie_runtime_debug_registry");
		const TCHAR* RuntimeSchemaId = TEXT("uai.pie.runtime/2");

		DomainAuthoring::FDomainHjsonDocumentContract RuntimeContract()
		{
			return DomainAuthoring::FDomainHjsonDocumentContract(
				TEXT("pie_runtime_debug"), RuntimeSchemaId, TEXT("pie_runtime_snapshot"));
		}

		FString RuntimeTargetIdentity(const FString& SessionRef)
		{
			return TEXT("pie-runtime:") + SessionRef;
		}

		FString RuntimeBaseRevision(int64 PauseEpoch, int64 WorkspaceRevision)
		{
			return FString::Printf(TEXT("pause-%lld-workspace-%lld"), PauseEpoch, WorkspaceRevision);
		}

		bool Fail(UE::UAI::Contract::FDiagnostic& OutError, UE::UAI::Contract::FDiagnostic Error)
		{
			OutError = MoveTemp(Error);
			return false;
		}

		UE::UAI::Contract::FDiagnostic GameThreadFailure()
		{
			return Native(TEXT("pie_runtime_game_thread_required"), TEXT("PIE runtime access must execute on the game thread."),
				TEXT("pie_runtime"), TEXT("game_thread"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
		}

		UE::UAI::Contract::FDiagnostic ArgumentFailure(const FString& Code, const FString& Reason, const FString& Field,
			const TSharedPtr<FJsonValue>& Actual, const TSharedPtr<FJsonValue>& Expected)
		{
			// These pointers are relative to the registry argument layout, not a command or file.
			auto Error = Input(Code, Reason, TEXT("pie_runtime"), Field.IsEmpty() ? TEXT("/") : TEXT("/") + Field, Actual, Expected);
			Error.PropertyPath = Field;
			return Error;
		}

		TSharedPtr<FJsonValue> EncodeRuntimeReference(UWorld& World,
			const TMap<const UObject*, FString>& AliasesByObject,
			UObject* Object, const FString& Path)
		{
			if (!Object && Path.IsEmpty()) return MakeShared<FJsonValueNull>();
			const FString* Alias = Object && Object->GetWorld() == &World
				? AliasesByObject.Find(Object) : nullptr;
			if (Alias)
			{
				const auto Reference = MakeShared<FJsonObject>();
				Reference->SetStringField(TEXT("alias"), *Alias);
				return MakeShared<FJsonValueObject>(Reference);
			}
			// Referenced assets/classes and unresolved soft paths are values, not
			// additional operation targets. Do not load or expand their object graph.
			if (!Object || (Object->GetPackage() != GetTransientPackage()
				&& !Object->GetPackage()->HasAnyPackageFlags(PKG_PlayInEditor)
				&& (!Object->GetWorld() || Object->IsAsset())))
				return UeAgentPropertyValueCodec::EncodeReferencePath(Path);
			const auto Unavailable = MakeShared<FJsonObject>();
			Unavailable->SetBoolField(TEXT("unavailable"), true);
			return MakeShared<FJsonValueObject>(Unavailable);
		}

		FUeAgentResourceTracker MakeResources()
		{
			return FUeAgentResourceTracker();
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

		FString AliasStem(const UObject& Object)
		{
			FString Stem = Object.GetName();
			for (TCHAR& Character : Stem)
			{
				if (!FChar::IsAlnum(Character) && Character != TEXT('_')) Character = TEXT('_');
			}
			const TCHAR* Prefix = Object.IsA<AActor>() ? TEXT("actor")
				: Object.IsA<UActorComponent>() ? TEXT("component")
				: Object.IsA<UWidget>() ? TEXT("widget") : TEXT("object");
			return FString(Prefix) + TEXT(".") + Stem;
		}

		FString EnsureAlias(
			UObject& Object,
			TMap<FString, TWeakObjectPtr<UObject>>& ObjectsByAlias,
			TMap<const UObject*, FString>& AliasesByObject)
		{
			if (const FString* Existing = AliasesByObject.Find(&Object))
			{
				const TWeakObjectPtr<UObject>* Previous = ObjectsByAlias.Find(*Existing);
				if (Previous && Previous->Get() == &Object) return *Existing;
				AliasesByObject.Remove(&Object);
			}
			const FString Stem = AliasStem(Object);
			const FString Alias = UE::UAI::AllocateReadableName(Stem, [&](const FString& Candidate) {
				return ObjectsByAlias.Contains(Candidate);
			});
			ObjectsByAlias.Add(Alias, &Object);
			AliasesByObject.Add(&Object, Alias);
			return Alias;
		}

		FString AliasOrEmpty(
			UObject* Object,
			TMap<FString, TWeakObjectPtr<UObject>>& ObjectsByAlias,
			TMap<const UObject*, FString>& AliasesByObject)
		{
			return IsValid(Object) ? EnsureAlias(*Object, ObjectsByAlias, AliasesByObject) : FString();
		}

		TSharedRef<FJsonObject> TypedValue(
			const FString& Kind,
			const TSharedPtr<FJsonValue>& Value)
		{
			const TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
			Result->SetStringField(TEXT("kind"), Kind);
			Result->SetField(TEXT("value"), Value);
			return Result;
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
			const FString& OutputSurfaceIdentity,
			TMap<FString, TWeakObjectPtr<UObject>>& ObjectsByAlias,
			TMap<const UObject*, FString>& AliasesByObject)
		{
			const TSharedRef<FJsonObject> Document = MakeShared<FJsonObject>();
			Document->SetStringField(TEXT("schema"), RuntimeSchemaId);
			const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
			Root->SetStringField(TEXT("type"), TEXT("pie_runtime_snapshot"));
			Root->SetStringField(TEXT("world"), World.GetName());
			const TSharedRef<FJsonObject> OutputSurface = MakeShared<FJsonObject>();
			OutputSurface->SetStringField(TEXT("alias"), TEXT("pie_output_surface"));
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
					Levels.Add(MakeShared<FJsonValueString>(Level->GetName()));
				}
			}
			Root->SetArrayField(TEXT("levels"), MoveTemp(Levels));

			TArray<TSharedPtr<FJsonValue>> Actors;
			TArray<TSharedPtr<FJsonValue>> SceneCameras;
			for (TActorIterator<AActor> It(&World); It; ++It)
			{
				AActor* Actor = *It;
				const TSharedRef<FJsonObject> ActorJson = MakeShared<FJsonObject>();
				ActorJson->SetStringField(TEXT("alias"), EnsureAlias(*Actor, ObjectsByAlias, AliasesByObject));
				ActorJson->SetStringField(TEXT("class"), Actor->GetClass()->GetName());
				ActorJson->SetStringField(TEXT("owner"), AliasOrEmpty(Actor->GetOwner(), ObjectsByAlias, AliasesByObject));
				ActorJson->SetStringField(TEXT("attached_to"), AliasOrEmpty(Actor->GetAttachParentActor(), ObjectsByAlias, AliasesByObject));
				const FBox Bounds = Actor->GetComponentsBoundingBox(true);
				const TSharedRef<FJsonObject> BoundsJson = MakeShared<FJsonObject>();
				BoundsJson->SetBoolField(TEXT("valid"), Bounds.IsValid != 0);
				BoundsJson->SetObjectField(TEXT("min"), VectorJson(Bounds.Min));
				BoundsJson->SetObjectField(TEXT("max"), VectorJson(Bounds.Max));
				ActorJson->SetObjectField(TEXT("bounds"), BoundsJson);
				const TSharedRef<FJsonObject> ActorProperties = MakeShared<FJsonObject>();
				ActorProperties->SetObjectField(TEXT("transform"), TypedValue(TEXT("transform"), MakeShared<FJsonValueObject>(TransformJson(Actor->GetActorTransform()))));
				ActorProperties->SetObjectField(TEXT("hidden"), TypedValue(TEXT("bool"), MakeShared<FJsonValueBoolean>(Actor->IsHidden())));
				ActorJson->SetObjectField(TEXT("properties"), ActorProperties);

				TArray<TSharedPtr<FJsonValue>> Components;
				for (UActorComponent* Component : Actor->GetComponents())
				{
					if (!IsValid(Component))
					{
						continue;
					}
					const TSharedRef<FJsonObject> ComponentJson = MakeShared<FJsonObject>();
					ComponentJson->SetStringField(TEXT("alias"), EnsureAlias(*Component, ObjectsByAlias, AliasesByObject));
					ComponentJson->SetStringField(TEXT("class"), Component->GetClass()->GetName());
					ComponentJson->SetStringField(TEXT("owner"), AliasOrEmpty(Component->GetOwner(), ObjectsByAlias, AliasesByObject));
					const TSharedRef<FJsonObject> ComponentProperties = MakeShared<FJsonObject>();
					ComponentProperties->SetObjectField(TEXT("active"), TypedValue(TEXT("bool"), MakeShared<FJsonValueBoolean>(Component->IsActive())));
					if (const USceneComponent* Scene = Cast<USceneComponent>(Component))
					{
						ComponentJson->SetStringField(TEXT("attached_to"), AliasOrEmpty(Scene->GetAttachParent(), ObjectsByAlias, AliasesByObject));
						ComponentProperties->SetObjectField(TEXT("visible"), TypedValue(TEXT("bool"), MakeShared<FJsonValueBoolean>(Scene->IsVisible())));
						ComponentProperties->SetObjectField(TEXT("transform"), TypedValue(TEXT("transform"), MakeShared<FJsonValueObject>(TransformJson(Scene->GetComponentTransform()))));
					}
					ComponentJson->SetObjectField(TEXT("properties"), ComponentProperties);
					if (const USkeletalMeshComponent* Mesh = Cast<USkeletalMeshComponent>(Component))
					{
						if (const UAnimInstance* Animation = Mesh->GetAnimInstance())
						{
							const auto AnimationJson = MakeShared<FJsonObject>();
							AnimationJson->SetStringField(TEXT("anim_instance_class"), Animation->GetClass()->GetPathName());
							if (const FByteProperty* Mode = FindFProperty<FByteProperty>(Animation->GetClass(), TEXT("RootMotionMode")))
							{
								if (Mode->Enum)
								{
									AnimationJson->SetStringField(TEXT("root_motion_mode"),
										UeAgentPropertyValueCodec::EncodeEnumValueName(Mode->Enum, Mode->GetPropertyValue_InContainer(Animation)));
								}
							}
							TArray<TSharedPtr<FJsonValue>> Montages;
							for (const FAnimMontageInstance* Instance : Animation->MontageInstances)
							{
								if (!Instance || !Instance->IsValid()) continue;
								const auto Montage = MakeShared<FJsonObject>();
								Montage->SetStringField(TEXT("asset"), Instance->Montage->GetPathName());
								Montage->SetStringField(TEXT("section"), Instance->GetCurrentSection().ToString());
								Montage->SetNumberField(TEXT("position_seconds"), Instance->GetPosition());
								Montage->SetNumberField(TEXT("play_rate"), Instance->GetPlayRate());
								Montage->SetNumberField(TEXT("weight"), Instance->GetWeight());
								Montage->SetBoolField(TEXT("playing"), Instance->IsPlaying());
								Montage->SetBoolField(TEXT("blending_out"), Instance->IsStopped());
								Montage->SetBoolField(TEXT("root_motion_disabled"), Instance->IsRootMotionDisabled());
								Montage->SetBoolField(TEXT("drives_root_motion"), Animation->GetRootMotionMontageInstance() == Instance);
								Montages.Add(MakeShared<FJsonValueObject>(Montage));
							}
							AnimationJson->SetArrayField(TEXT("montages"), MoveTemp(Montages));
							ComponentJson->SetObjectField(TEXT("animation"), AnimationJson);
						}
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
						CameraJson->SetStringField(TEXT("alias"), EnsureAlias(*CameraActor, ObjectsByAlias, AliasesByObject));
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
			Player->SetStringField(TEXT("local_player"), EnsureAlias(LocalPlayer, ObjectsByAlias, AliasesByObject));
			Player->SetStringField(TEXT("controller"), EnsureAlias(Controller, ObjectsByAlias, AliasesByObject));
			Player->SetStringField(TEXT("pawn"), AliasOrEmpty(Controller.GetPawn(), ObjectsByAlias, AliasesByObject));
			if (const UEnhancedInputLocalPlayerSubsystem* InputSubsystem =
				ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(&LocalPlayer))
			{
				if (UEnhancedPlayerInput* Input = InputSubsystem->GetPlayerInput())
				{
					if (FMapProperty* Contexts = FindFProperty<FMapProperty>(Input->GetClass(), TEXT("AppliedInputContextData")))
					{
						Player->SetField(TEXT("mapping_contexts"), UeAgentPropertyValueCodec::Encode(
							Contexts, Contexts->ContainerPtrToValuePtr<void>(Input), Input,
							UeAgentPropertyValueCodec::FOptions{}));
					}
				}
			}
			if (APlayerCameraManager* Camera = Controller.PlayerCameraManager)
			{
				const FMinimalViewInfo& View = Camera->GetCameraCacheView();
				const TSharedRef<FJsonObject> CameraJson = MakeShared<FJsonObject>();
				CameraJson->SetStringField(TEXT("alias"), EnsureAlias(*Camera, ObjectsByAlias, AliasesByObject));
				CameraJson->SetStringField(TEXT("view_target"), AliasOrEmpty(Controller.GetViewTarget(), ObjectsByAlias, AliasesByObject));
				CameraJson->SetObjectField(TEXT("location"), VectorJson(View.Location));
				CameraJson->SetObjectField(TEXT("rotation"), RotatorJson(View.Rotation));
				CameraJson->SetNumberField(TEXT("fov"), View.FOV);
				CameraJson->SetStringField(TEXT("projection"),
					View.ProjectionMode == ECameraProjectionMode::Perspective
						? TEXT("perspective") : TEXT("orthographic"));
				Player->SetObjectField(TEXT("evaluated_camera"), CameraJson);
			}
			Root->SetObjectField(TEXT("player"), Player);

			// Register the widgets that actually belong to this PIE world. Design
			// previews and widgets from another PIE instance are not observations.
			TArray<UUserWidget*> UserWidgets;
			UWidgetBlueprintLibrary::GetAllWidgetsOfClass(
				&World, UserWidgets, UUserWidget::StaticClass(), false);
			TArray<TSharedPtr<FJsonValue>> Widgets;
			for (UUserWidget* UserWidget : UserWidgets)
			{
				if (!IsValid(UserWidget) || UserWidget->GetWorld() != &World) continue;
				const TSharedRef<FJsonObject> WidgetJson = MakeShared<FJsonObject>();
				WidgetJson->SetStringField(TEXT("alias"), EnsureAlias(*UserWidget, ObjectsByAlias, AliasesByObject));
				WidgetJson->SetStringField(TEXT("name"), UserWidget->GetName());
				WidgetJson->SetStringField(TEXT("class"), UserWidget->GetClass()->GetName());
				WidgetJson->SetBoolField(TEXT("in_viewport"), UserWidget->IsInViewport());
				TArray<TSharedPtr<FJsonValue>> Children;
				if (UserWidget->WidgetTree)
				{
					TArray<UWidget*> TreeWidgets;
					UserWidget->WidgetTree->GetAllWidgets(TreeWidgets);
					for (UWidget* Widget : TreeWidgets)
					{
						if (!IsValid(Widget) || Widget->GetWorld() != &World) continue;
						const TSharedRef<FJsonObject> Child = MakeShared<FJsonObject>();
						Child->SetStringField(TEXT("alias"), EnsureAlias(*Widget, ObjectsByAlias, AliasesByObject));
						Child->SetStringField(TEXT("name"), Widget->GetName());
						Child->SetStringField(TEXT("class"), Widget->GetClass()->GetName());
						Children.Add(MakeShared<FJsonValueObject>(Child));
					}
				}
				WidgetJson->SetArrayField(TEXT("children"), MoveTemp(Children));
				Widgets.Add(MakeShared<FJsonValueObject>(WidgetJson));
			}
			Root->SetArrayField(TEXT("widgets"), MoveTemp(Widgets));

			// Register identity first, then encode references without recursively
			// expanding the UObject graph or loading referenced assets.
			for (TObjectIterator<UObject> It; It; ++It)
				if (IsValid(*It) && !It->IsTemplate() && It->GetWorld() == &World)
					EnsureAlias(**It, ObjectsByAlias, AliasesByObject);
			UeAgentPropertyValueCodec::FOptions Options;
			Options.ScopedReferenceEncoder = [&World, &AliasesByObject](UObject* Object, const FString& Path) -> TSharedPtr<FJsonValue>
			{
				return EncodeRuntimeReference(World, AliasesByObject, Object, Path);
			};
			TArray<FString> Aliases;
			ObjectsByAlias.GetKeys(Aliases);
			Aliases.Sort();
			TArray<TSharedPtr<FJsonValue>> Objects;
			for (const FString& Alias : Aliases)
			{
				UObject* Object = ObjectsByAlias[Alias].Get();
				if (!IsValid(Object) || Object->GetWorld() != &World) continue;
				const TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
				Entry->SetStringField(TEXT("alias"), Alias);
				Entry->SetStringField(TEXT("name"), Object->GetName());
				Entry->SetStringField(TEXT("class"), Object->GetClass()->GetPathName());
				const TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
				for (TFieldIterator<FProperty> It(Object->GetClass()); It; ++It)
				{
					FProperty* Property = *It;
					const TSharedRef<FJsonObject> Ref = MakeShared<FJsonObject>();
					Ref->SetStringField(TEXT("alias"), Alias);
					Ref->SetStringField(TEXT("property"), Property->GetName());
					Ref->SetArrayField(TEXT("path"), {});
					TSharedPtr<FJsonObject> Descriptor = UeAgentPropertyValueCodec::BuildTypeDescriptor(Property);
					if (!Descriptor) Descriptor = MakeShared<FJsonObject>();
					Descriptor->SetObjectField(TEXT("ref"), Ref);
					UE::UAI::Contract::FDiagnostic Reason;
					bool bReadable = !Property->HasAnyPropertyFlags(CPF_Deprecated) && UeAgentPropertyValueCodec::IsSupported(Property, Reason);
					TSharedPtr<FJsonValue> Value;
					if (bReadable) Value = UeAgentPropertyValueCodec::Encode(Property, Property->ContainerPtrToValuePtr<void>(Object), Object, Options);
					bReadable = bReadable && Value.IsValid();
					Descriptor->SetBoolField(TEXT("readable"), bReadable);
					Descriptor->SetBoolField(TEXT("writable"), bReadable && !Property->HasAnyPropertyFlags(CPF_EditConst));
					if (bReadable) Descriptor->SetField(TEXT("value"), Value);
					else Descriptor->SetStringField(TEXT("unavailable_reason"), Reason.IsEmpty() ? TEXT("deprecated_or_encode_unavailable") : Reason.Code);
					if (Property->HasAnyPropertyFlags(CPF_EditConst)) Descriptor->SetStringField(TEXT("write_restriction"), TEXT("read_only"));
					Properties->SetObjectField(Property->GetName(), Descriptor);
				}
				Entry->SetObjectField(TEXT("properties"), Properties);
				Objects.Add(MakeShared<FJsonValueObject>(Entry));
			}
			Root->SetArrayField(TEXT("objects"), Objects);

			Document->SetObjectField(TEXT("root"), Root);
			return Document;
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
					OutError = Native(TEXT("pie_runtime_workspace_shape_invalid"), TEXT("The staged PIE snapshot must be a directory with the registered entry file."),
						TEXT("artifact_publish"), TEXT("workspace_shape"),
						Facts({ { TEXT("kind"), Number(static_cast<int32>(WorkspaceKind)) }, { TEXT("entry_file"), MakeShared<FJsonValueString>(InEntryFile) } }),
						Facts({ { TEXT("kind"), Number(static_cast<int32>(EWorkspaceKind::Directory)) }, { TEXT("entry_file"), MakeShared<FJsonValueString>(EntryFile) } }));
					return false;
				}
				FWorkspaceError DomainError;
				if (!DomainAuthoring::TryVerifyDomainHjsonStagedWorkspace(
						StagedWorkspacePath, RuntimeTargetIdentity(SessionRef), RuntimeContract(),
						RuntimeBaseRevision(PauseEpoch, WorkspaceRevision), EntryFile,
						Resources, DomainError))
				{
					OutError = DomainError.GetDiagnostic();
					return false;
				}
				FString Text;
				if (!FFileHelper::LoadFileToString(Text, *(StagedWorkspacePath / EntryFile)))
				{
					OutError = Native(TEXT("pie_runtime_workspace_read_failed"), TEXT("The staged PIE snapshot entry file could not be read."),
						TEXT("artifact_publish"), TEXT("entry_file_read"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
					OutError.InternalContext = StagedWorkspacePath / EntryFile;
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
					OutError = HjsonError.GetDiagnostic();
					if (OutError.IsEmpty())
					{
						OutError = Native(TEXT("pie_runtime_workspace_hjson_parse_failed"), TEXT("The staged PIE snapshot parser returned no document."),
							TEXT("artifact_publish"), TEXT("parsed_document_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
						OutError.InternalContext = StagedWorkspacePath / EntryFile;
					}
					return false;
				}
				const FHjsonNode* Schema = HjsonDocument->Find(TEXT("/schema"));
				const FHjsonNode* RootType = HjsonDocument->Find(TEXT("/root/type"));
				if (!Schema || Schema->GetKind() != EHjsonNodeKind::String
					|| Schema->GetText() != RuntimeSchemaId
					|| !RootType || RootType->GetKind() != EHjsonNodeKind::String
					|| RootType->GetText() != TEXT("pie_runtime_snapshot"))
				{
					const bool bSchemaInvalid = !Schema || Schema->GetKind() != EHjsonNodeKind::String || Schema->GetText() != RuntimeSchemaId;
					const FHjsonNode* InvalidNode = bSchemaInvalid ? Schema : RootType;
					TSharedPtr<FJsonValue> Actual;
					if (InvalidNode) Actual = InvalidNode->GetKind() == EHjsonNodeKind::String
						? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(InvalidNode->GetText()))
						: Facts({ { TEXT("node_kind"), Number(static_cast<int32>(InvalidNode->GetKind())) } });
					OutError = Native(TEXT("pie_runtime_workspace_identity_mismatch"), TEXT("The staged PIE snapshot has the wrong schema or root type."),
						TEXT("artifact_publish"), bSchemaInvalid ? TEXT("schema") : TEXT("root/type"), Actual,
						MakeShared<FJsonValueString>(bSchemaInvalid ? RuntimeSchemaId : TEXT("pie_runtime_snapshot")));
					OutError.InternalContext = StagedWorkspacePath / EntryFile;
					OutError.Source.InternalJsonPath = bSchemaInvalid ? TEXT("/schema") : TEXT("/root/type");
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
					const TCHAR* Field = !IsInGameThread() ? TEXT("game_thread")
						: !World.IsValid() ? TEXT("world_available") : !ExactContext ? TEXT("pie_context_available")
						: ExactContext->World() != World.Get() ? TEXT("world_matches_session") : TEXT("paused");
					OutError = Native(TEXT("pie_runtime_world_changed_or_unpaused"), TEXT("The PIE snapshot cannot be published because its required world state changed."),
						TEXT("artifact_publish"), Field, MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
					return false;
				}
				return true;
			}

			TMap<FString, FString> GetSemanticVerificationFields() const override
			{
				return {
					{TEXT("schema"), RuntimeSchemaId},
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
		TMap<FString, TWeakObjectPtr<UObject>> ObjectsByAlias;
		TMap<const UObject*, FString> AliasesByObject;
		bool bWasPausedOnActivation = false;
		TSharedPtr<FUeAgentPieRuntimeInputState> Input = MakeShared<FUeAgentPieRuntimeInputState>();
		TMap<FString, TSharedPtr<FJsonObject>> WatchedReferences;
		TMap<FString, FString> ReferenceBaselines;
		TArray<TSharedPtr<FJsonValue>> ImportantUpdates;
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

		UE::UAI::Contract::FDiagnostic SessionFailure(const FUeAgentPieRuntimeDebugRegistryState* State, const FString& SessionRef)
		{
			auto Error = Native(TEXT("pie_runtime_session_stale"),
				State ? TEXT("The requested session no longer matches the active PIE session.") : TEXT("No PIE runtime session is active."),
				TEXT("pie_runtime"), State ? TEXT("session_matches") : TEXT("session_available"),
				MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			Error.ActualEvidence = MakeShared<FJsonValueString>(SessionRef);
			if (State) Error.ExpectedEvidence = MakeShared<FJsonValueString>(State->Snapshot.SessionRef);
			return Error;
		}

		UE::UAI::Contract::FDiagnostic SnapshotFailure(const FUeAgentPieRuntimeDebugRegistryState& State, int64 PauseEpoch, int64 Revision)
		{
			const bool bPauseMismatch = State.Snapshot.PauseEpoch != PauseEpoch;
			return Native(TEXT("pie_runtime_snapshot_stale"), TEXT("The requested PIE snapshot no longer matches the current paused workspace."),
				TEXT("pie_runtime"), bPauseMismatch ? TEXT("pause_epoch") : TEXT("workspace_revision"),
				Number(bPauseMismatch ? State.Snapshot.PauseEpoch : State.Snapshot.WorkspaceRevision), Number(bPauseMismatch ? PauseEpoch : Revision));
		}

		UE::UAI::Contract::FDiagnostic WorldFailure(const FUeAgentPieRuntimeDebugRegistryState& State)
		{
			UEditorEngine* EditorEngine = Cast<UEditorEngine>(GEditor);
			FWorldContext* Context = EditorEngine ? EditorEngine->GetPIEWorldContext(State.Snapshot.PieInstance) : nullptr;
			const TCHAR* Field = !State.Snapshot.World.IsValid() ? TEXT("world_available") : !EditorEngine ? TEXT("editor_available")
				: !Context ? TEXT("pie_context_available") : TEXT("world_matches_session");
			auto Error = Native(TEXT("pie_runtime_world_invalid"), TEXT("The active PIE session no longer has its required world context."),
				TEXT("pie_runtime"), Field, MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			Error.CandidateName = FString::Printf(TEXT("PIE %d"), State.Snapshot.PieInstance);
			return Error;
		}

		UE::UAI::Contract::FDiagnostic PropertyFailure(const FString& Code, const FString& Reason, const FString& Alias,
			const FString& PropertyName, const TSharedPtr<FJsonValue>& Actual, const TSharedPtr<FJsonValue>& Expected)
		{
			auto Error = Native(Code, Reason, TEXT("pie_runtime"), PropertyName, Actual, Expected);
			Error.TargetPath = Alias;
			return Error;
		}

		bool SnapshotState(
			const FUeAgentPieRuntimeDebugRegistryState& State,
			FUeAgentPieRuntimeDebugSnapshot& OutSnapshot,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			if (!IsExactWorld(State)
				|| !State.Snapshot.LocalPlayer.IsValid()
				|| !State.Snapshot.PlayerController.IsValid()
				|| State.Snapshot.LocalPlayer->GetPlayerController(
					State.Snapshot.World.Get())
					!= State.Snapshot.PlayerController.Get())
			{
				if (!IsExactWorld(State)) return Fail(OutError, WorldFailure(State));
				const TCHAR* Field = !State.Snapshot.LocalPlayer.IsValid() ? TEXT("local_player_available")
					: !State.Snapshot.PlayerController.IsValid() ? TEXT("player_controller_available") : TEXT("player_controller_matches");
				return Fail(OutError, Native(TEXT("pie_runtime_session_objects_invalid"), TEXT("The PIE session's local player or controller is no longer valid."),
					TEXT("pie_runtime"), Field, MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)));
			}
			OutSnapshot = State.Snapshot;
			return true;
		}

		bool ResolveAlias(const FUeAgentPieRuntimeDebugRegistryState& State,
			const FString& Alias, UObject*& OutObject, UE::UAI::Contract::FDiagnostic& OutError)
		{
			OutObject = nullptr;
			const TWeakObjectPtr<UObject>* Found = State.ObjectsByAlias.Find(Alias);
			if (!Found || !Found->IsValid())
			{
				OutError = Native(TEXT("pie_runtime_alias_stale"), TEXT("The runtime alias is absent or its object is no longer available."),
					TEXT("pie_runtime"), TEXT("alias"), MakeShared<FJsonValueString>(Alias), MakeShared<FJsonValueString>(TEXT("alias of a live registered object")));
				return false;
			}
			UObject* Object = Found->Get();
			if (!Object || Object->GetWorld() != State.Snapshot.World.Get())
			{
				OutError = Native(TEXT("pie_runtime_alias_world_mismatch"), TEXT("The aliased object does not belong to the current PIE world."),
					TEXT("pie_runtime"), TEXT("object_in_session_world"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				OutError.TargetPath = Alias;
				return false;
			}
			OutObject = Object;
			return true;
		}

		UeAgentPropertyValueCodec::FOptions RuntimeReferenceOptions(const FUeAgentPieRuntimeDebugRegistryState& State)
		{
			UeAgentPropertyValueCodec::FOptions Options;
			Options.ScopedReferenceEncoder = [&State](UObject* Referenced, const FString& Path) -> TSharedPtr<FJsonValue>
			{
				return EncodeRuntimeReference(*State.Snapshot.World.Get(), State.AliasesByObject, Referenced, Path);
			};
			Options.ScopedReferenceDecoder = [&State](const TSharedPtr<FJsonValue>& Value, TSharedPtr<FJsonValue>& OrdinaryValue, UE::UAI::Contract::FDiagnostic& Error)
			{
				if (Value.IsValid() && Value->Type == EJson::Null)
				{
					OrdinaryValue = UeAgentPropertyValueCodec::EncodeReferencePath(FString());
					return true;
				}
				const auto Object = Value.IsValid() && Value->Type == EJson::Object ? Value->AsObject() : nullptr;
				if (Object && Object->HasField(TEXT("alias")))
				{
					FString Alias;
					UObject* Referenced = nullptr;
					if (Object->Values.Num() != 1 || !Object->TryGetStringField(TEXT("alias"), Alias))
						return Fail(Error, ArgumentFailure(TEXT("pie_runtime_reference_alias_invalid"),
							TEXT("A runtime reference object must contain only a string alias."), FString(), Value,
							MakeShared<FJsonValueString>(TEXT("object containing only a string alias"))));
					if (!ResolveAlias(State, Alias, Referenced, Error)) return false;
					OrdinaryValue = UeAgentPropertyValueCodec::EncodeReferencePath(Referenced->GetPathName());
					return true;
				}
				TOptional<FString> Path;
				if (!UeAgentPropertyValueCodec::DecodeReferencePath(Value, Path, Error)) return false;
				if (Path.IsSet())
				{
					// Keep live world objects addressed through the session registry.
					// Unloaded soft asset paths remain lazy and retain their exact path.
					UObject* Referenced = FSoftObjectPath(Path.GetValue()).ResolveObject();
					if (Referenced && Referenced->GetWorld() && !Referenced->IsAsset())
						return Fail(Error, ArgumentFailure(TEXT("pie_runtime_reference_alias_required"),
							TEXT("A live PIE object must be referenced by its session alias."), FString(),
							MakeShared<FJsonValueString>(Path.GetValue()), MakeShared<FJsonValueString>(TEXT("runtime alias reference"))));
				}
				OrdinaryValue = Value;
				return true;
			};
			return Options;
		}

		TSharedPtr<FJsonValue> SortedJson(const TSharedPtr<FJsonValue>& Value)
		{
			if (Value->Type == EJson::Object)
			{
				const auto Result = MakeShared<FJsonObject>();
				TArray<FString> Keys;
				Value->AsObject()->Values.GetKeys(Keys);
				Keys.Sort();
				for (const auto& Key : Keys) Result->SetField(Key, SortedJson(Value->AsObject()->Values[Key]));
				return MakeShared<FJsonValueObject>(Result);
			}
			if (Value->Type == EJson::Array)
			{
				TArray<TSharedPtr<FJsonValue>> Items;
				for (const auto& Item : Value->AsArray()) Items.Add(SortedJson(Item));
				return MakeShared<FJsonValueArray>(Items);
			}
			return Value;
		}

		FString JsonText(const TSharedRef<FJsonObject>& Value)
		{
			FString Text;
			FJsonSerializer::Serialize(SortedJson(MakeShared<FJsonValueObject>(Value))->AsObject().ToSharedRef(), TJsonWriterFactory<>::Create(&Text));
			return Text;
		}

		TSharedPtr<FJsonValue> ReferenceShape(const TSharedPtr<FJsonValue>& Value, const FProperty* Property = nullptr, bool bSingleFixed = false)
		{
			if (!Value || Value->Type == EJson::Null) return MakeShared<FJsonValueNull>();
			if (Property && Value->Type == EJson::Object)
			{
				const auto Object = Value->AsObject();
				const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
				const auto Result = MakeShared<FJsonObject>();
				if (Property->ArrayDim > 1 && !bSingleFixed && Object->TryGetArrayField(TEXT("items"), Items))
				{
					TArray<TSharedPtr<FJsonValue>> Shapes;
					for (const auto& Item : *Items) Shapes.Add(ReferenceShape(Item, Property, true));
					Result->SetArrayField(TEXT("items"), Shapes);
					return MakeShared<FJsonValueObject>(Result);
				}
				if (const auto* Array = CastField<FArrayProperty>(Property); Array && Object->TryGetArrayField(TEXT("items"), Items))
				{
					TArray<TSharedPtr<FJsonValue>> Shapes;
					for (const auto& Item : *Items) Shapes.Add(ReferenceShape(Item, Array->Inner));
					Result->SetArrayField(TEXT("items"), Shapes);
					return MakeShared<FJsonValueObject>(Result);
				}
				if (const auto* Map = CastField<FMapProperty>(Property); Map && Object->TryGetArrayField(TEXT("entries"), Items))
				{
					TArray<TPair<FString, TSharedPtr<FJsonValue>>> Entries;
					for (const auto& Item : *Items)
					{
						const auto Entry = MakeShared<FJsonObject>();
						Entry->SetField(TEXT("key"), Item->AsObject()->TryGetField(TEXT("key")));
						const FString Key = JsonText(Entry);
						Entry->SetField(TEXT("value"), ReferenceShape(Item->AsObject()->TryGetField(TEXT("value")), Map->ValueProp));
						Entries.Emplace(Key, MakeShared<FJsonValueObject>(Entry));
					}
					Entries.Sort([](const auto& A, const auto& B) { return A.Key < B.Key; });
					TArray<TSharedPtr<FJsonValue>> Shapes;
					for (const auto& Entry : Entries) Shapes.Add(Entry.Value);
					Result->SetArrayField(TEXT("entries"), Shapes);
					return MakeShared<FJsonValueObject>(Result);
				}
				if (CastField<FSetProperty>(Property) && Object->TryGetArrayField(TEXT("items"), Items))
				{
					TArray<FString> Identities;
					for (const auto& Item : *Items)
					{
						const auto Entry = MakeShared<FJsonObject>();
						Entry->SetField(TEXT("element"), Item);
						Identities.Add(JsonText(Entry));
					}
					Identities.Sort();
					TArray<TSharedPtr<FJsonValue>> Shapes;
					for (const auto& Identity : Identities) Shapes.Add(MakeShared<FJsonValueString>(Identity));
					Result->SetArrayField(TEXT("items"), Shapes);
					return MakeShared<FJsonValueObject>(Result);
				}
				const TSharedPtr<FJsonObject>* Fields = nullptr;
				if (const auto* Struct = CastField<FStructProperty>(Property); Struct && Object->TryGetObjectField(TEXT("fields"), Fields))
				{
					const auto Shapes = MakeShared<FJsonObject>();
					for (const auto& Field : (*Fields)->Values) Shapes->SetField(Field.Key, ReferenceShape(Field.Value, FindFProperty<FProperty>(Struct->Struct, *Field.Key)));
					Result->SetObjectField(TEXT("fields"), Shapes);
					return MakeShared<FJsonValueObject>(Result);
				}
			}
			if (Value->Type == EJson::Array)
			{
				TArray<TSharedPtr<FJsonValue>> Items;
				for (const auto& Item : Value->AsArray()) Items.Add(ReferenceShape(Item));
				return MakeShared<FJsonValueArray>(Items);
			}
			if (Value->Type != EJson::Object) return MakeShared<FJsonValueString>(TEXT("scalar"));
			const auto Shape = MakeShared<FJsonObject>();
			TArray<FString> Keys;
			Value->AsObject()->Values.GetKeys(Keys);
			Keys.Sort();
			for (const FString& Key : Keys)
				Shape->SetField(Key, Key == TEXT("alias") || Key == TEXT("key") || Key == TEXT("unavailable")
					? Value->AsObject()->Values[Key] : ReferenceShape(Value->AsObject()->Values[Key]));
			return MakeShared<FJsonValueObject>(Shape);
		}

		FString ReferenceSignature(const FUeAgentPieRuntimeDebugRegistryState& State, const TSharedPtr<FJsonObject>& Ref)
		{
			FString Alias, PropertyName; UE::UAI::Contract::FDiagnostic Error;
			UObject* Object = nullptr;
			if (!Ref || !Ref->TryGetStringField(TEXT("alias"), Alias) || !ResolveAlias(State, Alias, Object, Error)) return TEXT("unavailable:") + Error.Code;
			if (!Ref->TryGetStringField(TEXT("property"), PropertyName)) return Object->GetClass()->GetPathName();
			FProperty* Property = FindFProperty<FProperty>(Object->GetClass(), *PropertyName);
			if (!Property) return TEXT("unavailable:property_missing");
			const TArray<TSharedPtr<FJsonValue>>* Path = nullptr;
			Ref->TryGetArrayField(TEXT("path"), Path);
			const auto Options = RuntimeReferenceOptions(State);
			UeAgentPropertyPath::FResolved Target;
			if (!UeAgentPropertyPath::Resolve(Property, Property->ContainerPtrToValuePtr<void>(Object), Path ? *Path : TArray<TSharedPtr<FJsonValue>>{}, Object, Options, Target, Error)) return TEXT("unavailable:") + Error.Code;
			const auto Type = UeAgentPropertyValueCodec::BuildTypeDescriptor(Target.Property, Target.bSingleFixedElement);
			if (!Type) return TEXT("unavailable:type");
			const auto Value = UeAgentPropertyValueCodec::Encode(Target.Property, Target.Address, Object, Options, 0, nullptr, Target.bSingleFixedElement);
			if (!Value) return TEXT("unavailable:encode");
			Type->SetField(TEXT("reference_structure"), ReferenceShape(Value, Target.Property, Target.bSingleFixedElement));
			Type->SetBoolField(TEXT("writable"), !Target.bReadOnly);
			TArray<TSharedPtr<FJsonValue>> Dependencies;
			for (const auto& Dependency : Target.ArrayDependencies)
			{
				const auto ParentValue = UeAgentPropertyValueCodec::Encode(Dependency.Key, Dependency.Value, Object, Options);
				Dependencies.Add(ReferenceShape(ParentValue, Dependency.Key));
			}
			Type->SetArrayField(TEXT("array_dependencies"), Dependencies);
			return JsonText(Type.ToSharedRef());
		}

		bool PublishWorkspace(
			FUeAgentPieRuntimeDebugRegistryState& State,
			int64 NewRevision,
			UE::UAI::Contract::FDiagnostic& OutError)
		{
			UWorld* World = State.Snapshot.World.Get();
			ULocalPlayer* LocalPlayer = State.Snapshot.LocalPlayer.Get();
			APlayerController* Controller = State.Snapshot.PlayerController.Get();
			if (!IsInGameThread() || !World || !LocalPlayer || !Controller || !World->IsPaused())
			{
				const TCHAR* Field = !IsInGameThread() ? TEXT("game_thread") : !World ? TEXT("world_available")
					: !LocalPlayer ? TEXT("local_player_available") : !Controller ? TEXT("player_controller_available") : TEXT("paused");
				return Fail(OutError, Native(TEXT("pie_runtime_publish_target_invalid"),
					TEXT("The PIE snapshot cannot be published because required session state is unavailable."),
					TEXT("pie_runtime"), Field, MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)));
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
				State.OutputSurfaceIdentity,
				State.ObjectsByAlias,
				State.AliasesByObject);
			Root->GetObjectField(TEXT("root"))->SetObjectField(TEXT("input"), State.Input->Describe(State.Snapshot));
			FUeAgentResourceTracker Resources = MakeResources();
			FExportWorkspacePlanPtr Plan;
			FWorkspaceError PlanError;
			if (!DomainAuthoring::TryBuildReadableDomainHjsonDirectoryExportPlan(
					RuntimeTargetIdentity(State.Snapshot.SessionRef), RuntimeContract(),
					RuntimeBaseRevision(State.Snapshot.PauseEpoch, NewRevision),
					TEXT("current"), PublisherOwnerId, EntryFile, Root,
					Resources, Plan, PlanError) || !Plan.IsValid())
			{
				OutError = PlanError.GetDiagnostic();
				return false;
			}
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
						Plan.ToSharedRef(),
						State.Snapshot.WorkspacePath,
						PublisherOwnerId,
						Verification),
					Resources,
					Receipt,
					Error))
			{
				OutError = Error;
				return false;
			}
			State.Snapshot.WorkspaceRevision = NewRevision;
			State.ImportantUpdates.Reset();
			for (const auto& Entry : State.WatchedReferences)
			{
				const FString Signature = ReferenceSignature(State, Entry.Value);
				const FString* Previous = State.ReferenceBaselines.Find(Entry.Key);
				if (!Previous || *Previous != Signature)
				{
					const auto Update = MakeShared<FJsonObject>();
					Update->SetObjectField(TEXT("ref"), Entry.Value);
					Update->SetStringField(TEXT("change"), !Previous ? TEXT("baseline_unknown") : Signature.StartsWith(TEXT("unavailable:")) ? TEXT("invalidated") : TEXT("reference_structure_changed"));
					State.ImportantUpdates.Add(MakeShared<FJsonValueObject>(Update));
				}
				State.ReferenceBaselines.Add(Entry.Key, Signature);
			}
			return true;
		}
	}

	FUeAgentPieRuntimeDebugRegistry::FUeAgentPieRuntimeDebugRegistry() = default;
	FUeAgentPieRuntimeDebugRegistry::~FUeAgentPieRuntimeDebugRegistry() = default;

	bool FUeAgentPieRuntimeDebugRegistry::TryActivate(
		int32 RequestedPieInstance,
		int32 RequestedLocalPlayerIndex,
		FUeAgentPieRuntimeDebugSnapshot& OutSnapshot,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		FScopeLock Lock(&Mutex);
		if (!IsInGameThread()) return Fail(OutError, GameThreadFailure());
		if (State.IsValid()) return Fail(OutError, Native(TEXT("pie_runtime_session_already_active"), TEXT("A PIE runtime session is already active."),
			TEXT("pie_runtime"), TEXT("session_active"), MakeShared<FJsonValueBoolean>(true), MakeShared<FJsonValueBoolean>(false)));
		UEditorEngine* EditorEngine = Cast<UEditorEngine>(GEditor);
		if (!EditorEngine) return Fail(OutError, Native(TEXT("pie_runtime_editor_unavailable"), TEXT("The Unreal Editor is unavailable for PIE activation."),
			TEXT("pie_runtime"), TEXT("editor_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)));
		if (!GEngine) return Fail(OutError, Native(TEXT("pie_runtime_engine_unavailable"), TEXT("The engine is unavailable for PIE activation."),
			TEXT("pie_runtime"), TEXT("engine_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)));
		FWorldContext* ExactContext = EditorEngine->GetPIEWorldContext(RequestedPieInstance);
		if (!ExactContext || !ExactContext->World()) return Fail(OutError, Native(TEXT("pie_runtime_instance_not_found"), TEXT("No live PIE world exists for the requested instance."),
			TEXT("pie_runtime"), TEXT("pie_instance"), Number(RequestedPieInstance), MakeShared<FJsonValueString>(TEXT("existing PIE instance"))));
		int32 PieWorldCount = 0;
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			if (Context.WorldType == EWorldType::PIE && Context.World()) ++PieWorldCount;
		}
		if (PieWorldCount != 1) return Fail(OutError, Native(TEXT("pie_runtime_requires_exactly_one_pie_world"), TEXT("PIE runtime debugging requires exactly one live PIE world."),
			TEXT("pie_runtime"), TEXT("pie_world_count"), Number(PieWorldCount), Number(1)));
		UWorld* World = ExactContext->World();
		UGameInstance* GameInstance = World->GetGameInstance();
		if (!GameInstance) return Fail(OutError, Native(TEXT("pie_runtime_game_instance_missing"), TEXT("The selected PIE world has no game instance."),
			TEXT("pie_runtime"), TEXT("game_instance_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)));
		const TArray<ULocalPlayer*>& LocalPlayers = GameInstance->GetLocalPlayers();
		if (!LocalPlayers.IsValidIndex(RequestedLocalPlayerIndex) || LocalPlayers.Num() != 1)
		{
			return Fail(OutError, Native(TEXT("pie_runtime_requires_exactly_one_local_player"), TEXT("PIE runtime debugging requires one local player and an index selecting that player."),
				TEXT("pie_runtime"), TEXT("local_player"), Facts({ { TEXT("count"), Number(LocalPlayers.Num()) }, { TEXT("index"), Number(RequestedLocalPlayerIndex) } }),
				Facts({ { TEXT("count"), Number(1) }, { TEXT("index"), Number(0) } })));
		}
		ULocalPlayer* LocalPlayer = LocalPlayers[RequestedLocalPlayerIndex];
		APlayerController* Controller = LocalPlayer
			? LocalPlayer->GetPlayerController(World)
			: nullptr;
		if (!Controller) return Fail(OutError, Native(TEXT("pie_runtime_player_controller_missing"), TEXT("The selected local player has no controller in the PIE world."),
			TEXT("pie_runtime"), TEXT("player_controller_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)));
		const bool bWasPaused = World->IsPaused();
		const bool bPauseCommandSucceeded = EditorEngine->SetPIEWorldsPaused(true);
		if (!bPauseCommandSucceeded || !World->IsPaused())
			return Fail(OutError, Native(TEXT("pie_runtime_pause_verification_failed"), TEXT("PIE could not enter the requested paused state."),
				TEXT("pie_runtime"), TEXT("pause"), Facts({ { TEXT("command_succeeded"), MakeShared<FJsonValueBoolean>(bPauseCommandSucceeded) }, { TEXT("paused"), MakeShared<FJsonValueBoolean>(World->IsPaused()) } }),
				Facts({ { TEXT("command_succeeded"), MakeShared<FJsonValueBoolean>(true) }, { TEXT("paused"), MakeShared<FJsonValueBoolean>(true) } })));
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
			const bool bRestoreSucceeded = EditorEngine->SetPIEWorldsPaused(bWasPaused);
			if (!bRestoreSucceeded || World->IsPaused() != bWasPaused)
			{
				OutError.Append(Native(TEXT("pie_runtime_pause_restore_failed"), TEXT("The previous PIE pause state could not be restored."),
					TEXT("cleanup"), TEXT("pause"), Facts({ { TEXT("command_succeeded"), MakeShared<FJsonValueBoolean>(bRestoreSucceeded) }, { TEXT("paused"), MakeShared<FJsonValueBoolean>(World->IsPaused()) } }),
					Facts({ { TEXT("command_succeeded"), MakeShared<FJsonValueBoolean>(true) }, { TEXT("paused"), MakeShared<FJsonValueBoolean>(bWasPaused) } })));
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
		UE::UAI::Contract::FDiagnostic& OutError) const
	{
		FScopeLock Lock(&Mutex);
		if (!IsInGameThread()) return Fail(OutError, GameThreadFailure());
		if (!State.IsValid() || State->Snapshot.SessionRef != SessionRef)
			return Fail(OutError, SessionFailure(State.Get(), SessionRef));
		if (State->Snapshot.PauseEpoch != ExpectedPauseEpoch
			|| State->Snapshot.WorkspaceRevision != ExpectedWorkspaceRevision)
			return Fail(OutError, SnapshotFailure(*State, ExpectedPauseEpoch, ExpectedWorkspaceRevision));
		return SnapshotState(*State, OutSnapshot, OutError);
	}

	bool FUeAgentPieRuntimeDebugRegistry::TryResolveAlias(
		const FString& SessionRef, int64 ExpectedPauseEpoch,
		int64 ExpectedWorkspaceRevision, const FString& Alias,
		UObject*& OutObject, UE::UAI::Contract::FDiagnostic& OutError) const
	{
		FScopeLock Lock(&Mutex);
		OutObject = nullptr;
		if (!IsInGameThread()) return Fail(OutError, GameThreadFailure());
		if (!State.IsValid() || State->Snapshot.SessionRef != SessionRef) return Fail(OutError, SessionFailure(State.Get(), SessionRef));
		if (State->Snapshot.PauseEpoch != ExpectedPauseEpoch || State->Snapshot.WorkspaceRevision != ExpectedWorkspaceRevision)
			return Fail(OutError, SnapshotFailure(*State, ExpectedPauseEpoch, ExpectedWorkspaceRevision));
		if (!IsExactWorld(*State)) return Fail(OutError, WorldFailure(*State));
		return ResolveAlias(*State, Alias, OutObject, OutError);
	}

	bool FUeAgentPieRuntimeDebugRegistry::TryReadProperty(
		const FString& SessionRef, int64 ExpectedPauseEpoch,
		int64 ExpectedWorkspaceRevision, const FString& Alias,
		const FString& PropertyName, TSharedPtr<FJsonValue>& OutValue,
		UE::UAI::Contract::FDiagnostic& OutError, const TArray<TSharedPtr<FJsonValue>>& Path) const
	{
		FScopeLock Lock(&Mutex);
		OutValue.Reset();
		if (!IsInGameThread()) return Fail(OutError, GameThreadFailure());
		if (!State.IsValid() || State->Snapshot.SessionRef != SessionRef) return Fail(OutError, SessionFailure(State.Get(), SessionRef));
		if (State->Snapshot.PauseEpoch != ExpectedPauseEpoch || State->Snapshot.WorkspaceRevision != ExpectedWorkspaceRevision)
			return Fail(OutError, SnapshotFailure(*State, ExpectedPauseEpoch, ExpectedWorkspaceRevision));
		UObject* Object = nullptr;
		if (!IsExactWorld(*State)) return Fail(OutError, WorldFailure(*State));
		if (!ResolveAlias(*State, Alias, Object, OutError)) return false;
		FProperty* Property = FindFProperty<FProperty>(Object->GetClass(), *PropertyName);
		if (!Property || Property->HasAnyPropertyFlags(CPF_Deprecated))
			return Fail(OutError, PropertyFailure(TEXT("pie_runtime_property_unavailable"), TEXT("The requested runtime property is missing or deprecated."), Alias, PropertyName,
				MakeShared<FJsonValueString>(Property ? TEXT("deprecated") : TEXT("missing")), MakeShared<FJsonValueString>(TEXT("existing non-deprecated property"))));
		const auto Options = RuntimeReferenceOptions(*State);
		UeAgentPropertyPath::FResolved Target;
		if (!UeAgentPropertyPath::Resolve(Property, Property->ContainerPtrToValuePtr<void>(Object), Path, Object, Options, Target, OutError))
		{
			BindArgumentInput(OutError, FString());
			return false;
		}
		const TSharedPtr<FJsonValue> Encoded = UeAgentPropertyValueCodec::Encode(Target.Property,
			Target.Address, Object, Options, 0, nullptr, Target.bSingleFixedElement);
		const TSharedPtr<FJsonObject> Descriptor = UeAgentPropertyValueCodec::BuildTypeDescriptor(Target.Property, Target.bSingleFixedElement);
		if (!Encoded.IsValid() || !Descriptor.IsValid()) return Fail(OutError, PropertyFailure(TEXT("pie_runtime_property_encode_failed"),
			TEXT("The runtime property could not produce both an encoded value and a type descriptor."), Alias, PropertyName,
			Facts({ { TEXT("encoded_value_available"), MakeShared<FJsonValueBoolean>(Encoded.IsValid()) }, { TEXT("type_descriptor_available"), MakeShared<FJsonValueBoolean>(Descriptor.IsValid()) } }),
			Facts({ { TEXT("encoded_value_available"), MakeShared<FJsonValueBoolean>(true) }, { TEXT("type_descriptor_available"), MakeShared<FJsonValueBoolean>(true) } })));
		Descriptor->SetField(TEXT("value"), Encoded);
		OutValue = MakeShared<FJsonValueObject>(Descriptor);
		return true;
	}

	bool FUeAgentPieRuntimeDebugRegistry::ValidatePropertyOperation(
		const FString& SessionRef, const FString& Alias, const FString& PropertyName,
		const TArray<TSharedPtr<FJsonValue>>& Path, const TSharedPtr<FJsonValue>& WriteValue,
		bool bRawValue, UE::UAI::Contract::FDiagnostic& OutError) const
	{
		FScopeLock Lock(&Mutex);
		if (!IsInGameThread()) return Fail(OutError, GameThreadFailure());
		if (!State.IsValid() || State->Snapshot.SessionRef != SessionRef) return Fail(OutError, SessionFailure(State.Get(), SessionRef));
		UObject* Object = nullptr;
		if (!IsExactWorld(*State)) return Fail(OutError, WorldFailure(*State));
		if (!ResolveAlias(*State, Alias, Object, OutError)) return false;
		FProperty* Property = FindFProperty<FProperty>(Object->GetClass(), *PropertyName);
		if (!Property || Property->HasAnyPropertyFlags(CPF_Deprecated)) return Fail(OutError, PropertyFailure(TEXT("pie_runtime_property_unavailable"), TEXT("The requested runtime property is missing or deprecated."), Alias, PropertyName,
				MakeShared<FJsonValueString>(Property ? TEXT("deprecated") : TEXT("missing")), MakeShared<FJsonValueString>(TEXT("existing non-deprecated property"))));
		const auto Options = RuntimeReferenceOptions(*State);
		UeAgentPropertyPath::FResolved Target;
		// Validate static types without requiring elements that earlier operations may create.
		if (!UeAgentPropertyPath::Resolve(Property, nullptr, Path, Object, Options, Target, OutError, true))
		{
			BindArgumentInput(OutError, FString());
			return false;
		}
		if (!WriteValue.IsValid()) return true;
		if (Target.bReadOnly) return Fail(OutError, PropertyFailure(TEXT("property_path_not_writable"), TEXT("The selected runtime property path is read-only."), Alias, PropertyName,
			Facts({ { TEXT("read_only"), MakeShared<FJsonValueBoolean>(true) }, { TEXT("path"), MakeShared<FJsonValueArray>(Path) } }),
			Facts({ { TEXT("read_only"), MakeShared<FJsonValueBoolean>(false) } })));
		if (!bRawValue && (WriteValue->Type != EJson::Object || !WriteValue->AsObject().IsValid()))
			return Fail(OutError, ArgumentFailure(TEXT("pie_runtime_property_wrapper_invalid"), TEXT("A typed property value must be an object wrapper."),
				TEXT("value"), WriteValue, MakeShared<FJsonValueString>(TEXT("object with value and type metadata"))));
		const auto Wrapper = bRawValue ? TSharedPtr<FJsonObject>{} : WriteValue->AsObject();
		const auto Encoded = bRawValue ? WriteValue : Wrapper->TryGetField(TEXT("value"));
		if (!Encoded.IsValid()) return Fail(OutError, ArgumentFailure(TEXT("pie_runtime_property_wrapper_invalid"), TEXT("The typed property wrapper has no value field."),
			TEXT("value/value"), nullptr, MakeShared<FJsonValueString>(TEXT("present value field"))));
		if (!bRawValue && !UeAgentPropertyValueCodec::ValidateTypeMetadata(
			Target.Property, Wrapper, OutError, Target.bSingleFixedElement))
		{
			BindArgumentInput(OutError, TEXT("/value"), true);
			return false;
		}
		void* Temporary = FMemory::Malloc(Target.Property->GetSize(), Target.Property->GetMinAlignment());
		Target.Property->InitializeValue(Temporary);
		const bool bValid = UeAgentPropertyValueCodec::Decode(Target.Property, Encoded, Temporary, Object,
			Options, OutError, 0, Target.bSingleFixedElement);
		if (!bValid) BindArgumentInput(OutError, bRawValue ? TEXT("/value") : TEXT("/value/value"));
		Target.Property->DestroyValue(Temporary);
		FMemory::Free(Temporary);
		return bValid;
	}

	bool FUeAgentPieRuntimeDebugRegistry::TryWriteProperty(
		const FString& SessionRef, int64 ExpectedPauseEpoch,
		int64 ExpectedWorkspaceRevision, const FString& Alias,
		const FString& PropertyName, const TSharedPtr<FJsonValue>& Value,
		UE::UAI::Contract::FDiagnostic& OutError, const TArray<TSharedPtr<FJsonValue>>& Path, bool bRawValue)
	{
		FScopeLock Lock(&Mutex);
		if (!IsInGameThread()) return Fail(OutError, GameThreadFailure());
		if (!State.IsValid() || State->Snapshot.SessionRef != SessionRef) return Fail(OutError, SessionFailure(State.Get(), SessionRef));
		if (State->Snapshot.PauseEpoch != ExpectedPauseEpoch || State->Snapshot.WorkspaceRevision != ExpectedWorkspaceRevision)
			return Fail(OutError, SnapshotFailure(*State, ExpectedPauseEpoch, ExpectedWorkspaceRevision));
		UObject* Object = nullptr;
		if (!IsExactWorld(*State)) return Fail(OutError, WorldFailure(*State));
		if (!ResolveAlias(*State, Alias, Object, OutError)) return false;
		FProperty* Property = FindFProperty<FProperty>(Object->GetClass(), *PropertyName);
		if (!Property || Property->HasAnyPropertyFlags(CPF_EditConst | CPF_Deprecated))
			return Fail(OutError, PropertyFailure(TEXT("pie_runtime_property_not_writable"), TEXT("The requested runtime property is missing, deprecated, or read-only."), Alias, PropertyName,
				MakeShared<FJsonValueString>(!Property ? TEXT("missing") : Property->HasAnyPropertyFlags(CPF_EditConst) ? TEXT("read-only") : TEXT("deprecated")),
				MakeShared<FJsonValueString>(TEXT("existing writable property"))));
		if (!Value.IsValid() || (!bRawValue && (Value->Type != EJson::Object || !Value->AsObject().IsValid())))
			return Fail(OutError, ArgumentFailure(TEXT("pie_runtime_property_wrapper_invalid"), TEXT("The property write requires a value, with an object wrapper for typed writes."),
				TEXT("value"), Value, MakeShared<FJsonValueString>(bRawValue ? TEXT("present raw value") : TEXT("object with value and type metadata"))));
		const TSharedPtr<FJsonObject> Wrapper = bRawValue ? nullptr : Value->AsObject();
		const TSharedPtr<FJsonValue> Encoded = bRawValue ? Value : Wrapper->TryGetField(TEXT("value"));
		if (!Encoded.IsValid())
			return Fail(OutError, ArgumentFailure(TEXT("pie_runtime_property_wrapper_invalid"), TEXT("The typed property wrapper has no value field."),
				TEXT("value/value"), nullptr, MakeShared<FJsonValueString>(TEXT("present value field"))));
		void* Temporary = FMemory::Malloc(Property->GetSize(), Property->GetMinAlignment());
		Property->InitializeValue(Temporary);
		Property->CopyCompleteValue(Temporary, Property->ContainerPtrToValuePtr<void>(Object));
		UE::UAI::Contract::FDiagnostic DecodeError;
		UeAgentPropertyPath::FResolved Target;
		const auto Options = RuntimeReferenceOptions(*State);
		bool bDecoded = UeAgentPropertyPath::Resolve(Property, Temporary, Path, Object, Options, Target, DecodeError);
		if (!bDecoded) BindArgumentInput(DecodeError, FString());
		if (bDecoded && Target.bReadOnly)
		{ bDecoded = false; DecodeError = PropertyFailure(TEXT("property_path_not_writable"), TEXT("The selected runtime property path is read-only."), Alias, PropertyName,
			Facts({ { TEXT("read_only"), MakeShared<FJsonValueBoolean>(true) }, { TEXT("path"), MakeShared<FJsonValueArray>(Path) } }),
			Facts({ { TEXT("read_only"), MakeShared<FJsonValueBoolean>(false) } })); }
		const bool bTypeMismatch = bDecoded && !bRawValue
			&& !UeAgentPropertyValueCodec::ValidateTypeMetadata(
				Target.Property, Wrapper, DecodeError, Target.bSingleFixedElement);
		if (bTypeMismatch)
		{
			bDecoded = false;
			BindArgumentInput(DecodeError, TEXT("/value"), true);
		}
		if (bDecoded)
		{
			bDecoded = UeAgentPropertyValueCodec::Decode(Target.Property, Encoded,
				Target.Address, Object, Options, DecodeError, 0, Target.bSingleFixedElement);
			if (!bDecoded) BindArgumentInput(DecodeError, bRawValue ? TEXT("/value") : TEXT("/value/value"));
			else bDecoded = UeAgentPropertyPath::FinishWrite(Target, DecodeError);
		}
		TArray<TSharedPtr<FJsonValue>> ReadbackPath;
		TSharedPtr<FJsonObject> WrittenType;
		if (bDecoded)
		{
			WrittenType = UeAgentPropertyValueCodec::BuildTypeDescriptor(Target.Property, Target.bSingleFixedElement);
			bDecoded = UeAgentPropertyPath::CaptureWritePath(Target, Path, Object, Options, ReadbackPath, DecodeError);
		}
		if (bDecoded)
		{
			Property->CopyCompleteValue(Property->ContainerPtrToValuePtr<void>(Object), Temporary);
			Object->PostEditChange();
		}
		Property->DestroyValue(Temporary);
		FMemory::Free(Temporary);
		if (!bDecoded)
		{
			OutError = MoveTemp(DecodeError);
			if (OutError.PropertyPath.IsEmpty()) OutError.PropertyPath = PropertyName;
			if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Alias;
			return false;
		}
		// PostEditChange can reconstruct owners and relocate container storage.
		// Re-resolve the exact session target; never compare through stale pointers
		// or derive expected values from the post-write snapshot.
		UObject* SettledObject = nullptr;
		UE::UAI::Contract::FDiagnostic ReadbackError;
		const auto FailReadback = [&]()
		{
			MarkNativeSource(ReadbackError);
			OutError = ReadbackError;
			if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = Alias;
            if (OutError.PropertyPath.IsEmpty()) OutError.PropertyPath = PropertyName;
			return false;
		};
		if (!IsExactWorld(*State))
		{ ReadbackError = WorldFailure(*State); return FailReadback(); }
		if (!ResolveAlias(*State, Alias, SettledObject, ReadbackError)) return FailReadback();
		FProperty* SettledProperty = FindFProperty<FProperty>(SettledObject->GetClass(), *PropertyName);
		if (!SettledProperty)
		{ ReadbackError = PropertyFailure(TEXT("pie_runtime_property_unavailable"), TEXT("The runtime property no longer exists after the write."), Alias, PropertyName,
			MakeShared<FJsonValueString>(TEXT("missing")), MakeShared<FJsonValueString>(TEXT("existing property"))); return FailReadback(); }
		UeAgentPropertyPath::FResolved Actual;
		if (!UeAgentPropertyPath::Resolve(SettledProperty,
			SettledProperty->ContainerPtrToValuePtr<void>(SettledObject), ReadbackPath, SettledObject, Options, Actual, ReadbackError)
			|| !UeAgentPropertyValueCodec::ValidateTypeMetadata(Actual.Property, WrittenType, ReadbackError, Actual.bSingleFixedElement))
			return FailReadback();
		auto VerifyOptions = Options;
		VerifyOptions.NativeMemberContainer = Actual.Property->GetOwner<UScriptStruct>() && Actual.Property->ArrayDim == 1
			? static_cast<const uint8*>(Actual.Address) - Actual.Property->GetOffset_ForInternal() : nullptr;
		if (!UeAgentPropertyValueCodec::VerifyValueIntent(Actual.Property, Encoded,
			Actual.Address, SettledObject, VerifyOptions, ReadbackError, Actual.bSingleFixedElement)) {
			MarkNativeSource(ReadbackError);
			auto Warning = UE::UAI::Contract::FDiagnostic::WrapFailure(TEXT("pie_runtime_property_readback_mismatch"),
				ReadbackError, TEXT("pie_property_observation"));
			Warning.PropertyPath = PropertyName;
			if (Warning.TargetPath.IsEmpty()) Warning.TargetPath = Alias;
			UeAgentAuthoringDiagnostics::ReportConsistencyWarning(Warning);
		}
		return true;
	}

	bool FUeAgentPieRuntimeDebugRegistry::TryReadExactOutputSurface(
		const FString& SessionRef,
		const int64 ExpectedPauseEpoch,
		const int64 ExpectedWorkspaceRevision,
		TArray<FColor>& OutPixels,
		FIntPoint& OutSize,
		FString& OutSurfaceIdentity,
		UE::UAI::Contract::FDiagnostic& OutError,
		EditorCapture::FCaptureSurface* AsyncSurface,
		TSharedPtr<EditorCapture::FAsyncCaptureFrame, ESPMode::ThreadSafe>* OutFrame) const
	{
		FScopeLock Lock(&Mutex);
		OutPixels.Reset();
		OutSize = FIntPoint::ZeroValue;
		OutSurfaceIdentity.Reset();
		if (!IsInGameThread()) return Fail(OutError, GameThreadFailure());
		if (!State.IsValid() || State->Snapshot.SessionRef != SessionRef)
			return Fail(OutError, SessionFailure(State.Get(), SessionRef));
		if (State->Snapshot.PauseEpoch != ExpectedPauseEpoch
			|| State->Snapshot.WorkspaceRevision != ExpectedWorkspaceRevision)
			return Fail(OutError, SnapshotFailure(*State, ExpectedPauseEpoch, ExpectedWorkspaceRevision));

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
			const TCHAR* Field = !ExactContext ? TEXT("pie_context_available") : ExactContext->World() != World ? TEXT("world_matches")
				: !LocalPlayer ? TEXT("local_player_available") : LocalPlayer->ViewportClient != ViewportClient ? TEXT("viewport_client_matches")
				: LocalPlayer->GetPlayerController(World) != Snapshot.PlayerController.Get() ? TEXT("player_controller_matches")
				: ResolvedIdentity != State->OutputSurfaceIdentity ? TEXT("surface_identity_matches") : TEXT("scene_viewport_available");
			OutError = Native(TEXT("pie_runtime_output_surface_identity_mismatch"), TEXT("The PIE output surface no longer matches the registered session."),
				TEXT("pie_runtime"), Field, MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			OutError.ActualEvidence = MakeShared<FJsonValueString>(ResolvedIdentity);
			OutError.ExpectedEvidence = MakeShared<FJsonValueString>(State->OutputSurfaceIdentity);
			return false;
		}
		const FIntPoint Size = SceneViewport->GetSizeXY();
		if (Size.X <= 0 || Size.Y <= 0)
		{
			return Fail(OutError, Native(TEXT("pie_runtime_output_surface_size_invalid"), TEXT("The PIE output surface dimensions must be positive."),
				TEXT("pie_runtime"), TEXT("dimensions"), PieDiagnostics::Size(Size), MakeShared<FJsonValueString>(TEXT("positive width and height"))));
		}
		const TSharedPtr<SViewport> ViewportWidget = SceneViewport->GetViewportWidget().Pin();
		if (!ViewportWidget.IsValid() || ViewportWidget->ShouldRenderDirectly())
		{
			return Fail(OutError, Native(TEXT("pie_runtime_slate_output_surface_unavailable"),
				ViewportWidget.IsValid() ? TEXT("The PIE Slate viewport renders directly and cannot be captured as a composed surface.") : TEXT("The PIE output surface has no Slate viewport widget."),
				TEXT("pie_runtime"), ViewportWidget.IsValid() ? TEXT("renders_directly") : TEXT("viewport_widget_available"),
				MakeShared<FJsonValueBoolean>(ViewportWidget.IsValid()), MakeShared<FJsonValueBoolean>(!ViewportWidget.IsValid())));
		}
		// Editor PIE owns a separate scene texture beneath its real Slate game
		// layers. Render that same tree so hidden sessions also include UMG.
		// SceneViewport::ReadPixels alone omits those layers.
		// A prior operation may have changed the world in this same timeline tick.
		// Refresh the scene texture without advancing gameplay before composing UI.
		SceneViewport->Draw(false);
		if (AsyncSurface && OutFrame)
		{
			OutSize = Size;
			OutSurfaceIdentity = ResolvedIdentity;
			return AsyncSurface->Widget(ViewportWidget.ToSharedRef(), Size,
				ViewportWidget->GetCachedGeometry().Scale, *OutFrame, OutError);
		}
		FlushRenderingCommands();
		TArray<FColor> Pixels;
		FIntPoint CapturedSize;
		const float DrawScale = ViewportWidget->GetCachedGeometry().Scale;
		if (!UeAgentSlateWidgetCapture::CaptureWidgetPixels(
			ViewportWidget.ToSharedRef(), FVector2D(Size), DrawScale,
			Pixels, CapturedSize, OutError))
		{
			return false;
		}
		if (CapturedSize != Size || Pixels.Num() != Size.X * Size.Y)
		{
			return Fail(OutError, Native(TEXT("pie_runtime_output_surface_read_failed"), TEXT("The composed PIE capture dimensions or pixel count differ from the output surface."),
				TEXT("pie_runtime"), TEXT("capture"), Facts({ { TEXT("dimensions"), PieDiagnostics::Size(CapturedSize) }, { TEXT("pixel_count"), Number(Pixels.Num()) } }),
				Facts({ { TEXT("dimensions"), PieDiagnostics::Size(Size) }, { TEXT("pixel_count"), Number(static_cast<int64>(Size.X) * Size.Y) } })));
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
		UE::UAI::Contract::FDiagnostic& OutError,
		FMinimalViewInfo* OutView,
		EditorCapture::FCaptureSurface* AsyncSurface,
		TSharedPtr<EditorCapture::FAsyncCaptureFrame, ESPMode::ThreadSafe>* OutFrame) const
	{
		FScopeLock Lock(&Mutex);
		OutPixels.Reset();
		OutSize = FIntPoint::ZeroValue;
		OutSourceIdentity.Reset();
		if (!IsInGameThread()) return Fail(OutError, GameThreadFailure());
		if (!State.IsValid() || State->Snapshot.SessionRef != SessionRef)
			return Fail(OutError, SessionFailure(State.Get(), SessionRef));
		if (State->Snapshot.PauseEpoch != ExpectedPauseEpoch
			|| State->Snapshot.WorkspaceRevision != ExpectedWorkspaceRevision)
			return Fail(OutError, SnapshotFailure(*State, ExpectedPauseEpoch, ExpectedWorkspaceRevision));

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
			const TCHAR* Field = !ExactContext ? TEXT("pie_context_available") : ExactContext->World() != World ? TEXT("world_matches")
				: !LocalPlayer ? TEXT("local_player_available") : !Controller ? TEXT("player_controller_available")
				: LocalPlayer->ViewportClient != ViewportClient ? TEXT("viewport_client_matches")
				: LocalPlayer->GetPlayerController(World) != Controller ? TEXT("player_controller_matches")
				: ResolvedSurfaceIdentity != State->OutputSurfaceIdentity ? TEXT("surface_identity_matches") : TEXT("scene_viewport_available");
			OutError = Native(TEXT("pie_runtime_output_surface_identity_mismatch"), TEXT("The camera capture output surface no longer matches the PIE session."),
				TEXT("pie_runtime"), Field, MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
			OutError.ActualEvidence = MakeShared<FJsonValueString>(ResolvedSurfaceIdentity);
			OutError.ExpectedEvidence = MakeShared<FJsonValueString>(State->OutputSurfaceIdentity);
			return false;
		}
		const FIntPoint Size = SceneViewport->GetSizeXY();
		if (Size.X <= 0 || Size.Y <= 0)
			return Fail(OutError, Native(TEXT("pie_runtime_output_surface_size_invalid"), TEXT("The PIE output surface dimensions must be positive."),
				TEXT("pie_runtime"), TEXT("dimensions"), PieDiagnostics::Size(Size), MakeShared<FJsonValueString>(TEXT("positive width and height"))));

		FMinimalViewInfo View;
		FString SourceIdentity;
		if (Request.Source == EUeAgentPieCameraReadSource::PlayerCamera)
		{
			if (!Request.CameraRef.IsEmpty() || Request.OverrideLocation.IsSet()
				|| Request.OverrideRotation.IsSet() || Request.OverrideFovDegrees.IsSet())
				return Fail(OutError, ArgumentFailure(TEXT("pie_runtime_player_camera_override_forbidden"), TEXT("Player-camera capture does not allow a camera reference or location, rotation, or field-of-view overrides."),
					!Request.CameraRef.IsEmpty() ? TEXT("camera") : Request.OverrideLocation.IsSet() ? TEXT("view_transform/location")
						: Request.OverrideRotation.IsSet() ? TEXT("view_transform/rotation") : TEXT("fov_degrees"),
					Facts({ { TEXT("camera_ref_present"), MakeShared<FJsonValueBoolean>(!Request.CameraRef.IsEmpty()) },
						{ TEXT("location_present"), MakeShared<FJsonValueBoolean>(Request.OverrideLocation.IsSet()) }, { TEXT("rotation_present"), MakeShared<FJsonValueBoolean>(Request.OverrideRotation.IsSet()) },
						{ TEXT("fov_present"), MakeShared<FJsonValueBoolean>(Request.OverrideFovDegrees.IsSet()) } }),
					MakeShared<FJsonValueString>(TEXT("no camera overrides"))));
			APlayerCameraManager* Manager = Controller->PlayerCameraManager;
			if (!IsValid(Manager))
				return Fail(OutError, Native(TEXT("pie_runtime_player_camera_unavailable"), TEXT("The PIE player controller has no live camera manager."),
					TEXT("pie_runtime"), TEXT("player_camera_manager_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)));
			View = Manager->GetCameraCacheView();
			SourceIdentity = MakePieCameraSourceIdentity(
				EUeAgentPieCameraReadSource::PlayerCamera, Manager->GetPathName());
		}
		else
		{
			if (!Request.CameraRef.IsEmpty())
			{
				UObject* ResolvedCamera = nullptr;
				if (!ResolveAlias(*State, Request.CameraRef, ResolvedCamera, OutError)) return false;
				ACameraActor* Camera = Cast<ACameraActor>(ResolvedCamera);
				if (!IsValid(Camera) || Camera->GetWorld() != World
					|| !IsValid(Camera->GetCameraComponent()))
				{
					OutError = Native(TEXT("pie_runtime_scene_camera_identity_mismatch"), TEXT("The camera alias must resolve to a camera actor with a live component in the current PIE world."),
						TEXT("pie_runtime"), TEXT("camera"),
						Facts({ { TEXT("class"), MakeShared<FJsonValueString>(ResolvedCamera->GetClass()->GetName()) },
							{ TEXT("world_matches"), MakeShared<FJsonValueBoolean>(IsValid(Camera) && Camera->GetWorld() == World) },
							{ TEXT("camera_component_available"), MakeShared<FJsonValueBoolean>(IsValid(Camera) && IsValid(Camera->GetCameraComponent())) } }),
						MakeShared<FJsonValueString>(TEXT("camera actor with a live component in the current PIE world")));
					OutError.TargetPath = Request.CameraRef;
					return false;
				}
				Camera->GetCameraComponent()->GetCameraView(0.0f, View);
				SourceIdentity = MakePieCameraSourceIdentity(
					EUeAgentPieCameraReadSource::SceneCamera, Request.CameraRef);
			}
			else
			{
				if (!Request.OverrideLocation.IsSet() || !Request.OverrideRotation.IsSet()
					|| !Request.OverrideFovDegrees.IsSet() || Request.TransientIdentity.IsEmpty())
				{
					const TCHAR* Field = !Request.OverrideLocation.IsSet() ? TEXT("location") : !Request.OverrideRotation.IsSet() ? TEXT("rotation")
						: !Request.OverrideFovDegrees.IsSet() ? TEXT("fov_degrees") : TEXT("transient_identity_available");
					OutError = Native(TEXT("pie_runtime_transient_camera_incomplete"), TEXT("The prepared transient camera is missing a required field."),
						TEXT("pie_runtime"), Field, MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
					return false;
				}
				View.ProjectionMode = ECameraProjectionMode::Perspective;
				SourceIdentity = MakePieTransientCameraSourceIdentity(Request.TransientIdentity);
			}
		}
		if (Request.OverrideLocation.IsSet()) View.Location = Request.OverrideLocation.GetValue();
		if (Request.OverrideRotation.IsSet()) View.Rotation = Request.OverrideRotation.GetValue();
		if (Request.OverrideFovDegrees.IsSet()) View.FOV = Request.OverrideFovDegrees.GetValue();
		if (OutView) *OutView = View;
		if (AsyncSurface && OutFrame)
		{
			OutSize = Size;
			OutSourceIdentity = SourceIdentity;
			return AsyncSurface->Camera(*World, View, Size, *OutFrame, OutError);
		}

		FActorSpawnParameters SpawnParameters;
		SpawnParameters.ObjectFlags |= RF_Transient;
		SpawnParameters.SpawnCollisionHandlingOverride =
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		ASceneCapture2D* CaptureActor = World->SpawnActor<ASceneCapture2D>(
			View.Location, View.Rotation, SpawnParameters);
		if (!IsValid(CaptureActor))
			return Fail(OutError, Native(TEXT("pie_runtime_camera_capture_create_failed"), TEXT("The PIE world could not create a transient scene capture actor."),
				TEXT("pie_runtime"), TEXT("capture_actor_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)));

		UTextureRenderTarget2D* RenderTarget = NewObject<UTextureRenderTarget2D>(
			GetTransientPackage(), NAME_None, RF_Transient);
		USceneCaptureComponent2D* Capture = CaptureActor->GetCaptureComponent2D();
		bool bSucceeded = false;
		UE::UAI::Contract::FDiagnostic CaptureError;
		if (IsValid(RenderTarget) && IsValid(Capture))
		{
			RenderTarget->InitCustomFormat(Size.X, Size.Y, PF_B8G8R8A8, false);
			RenderTarget->UpdateResourceImmediate(true);
			Capture->TextureTarget = RenderTarget;
			// PNG captures need display color, not HDR scene color clamped into an 8-bit target.
			UE::UAI::EditorCapture::ConfigureCameraCapture(*Capture, View);
			Capture->CaptureScene();
			FlushRenderingCommands();
			FTextureRenderTargetResource* Resource = RenderTarget->GameThread_GetRenderTargetResource();
			FReadSurfaceDataFlags ReadFlags(RCM_UNorm);
			ReadFlags.SetLinearToGamma(false);
			const bool bPixelsRead = Resource && Resource->ReadPixels(OutPixels, ReadFlags);
			bSucceeded = bPixelsRead && OutPixels.Num() == Size.X * Size.Y;
			if (!bSucceeded)
			{
				if (!Resource || !bPixelsRead)
					CaptureError = Native(TEXT("pie_runtime_camera_capture_read_failed"),
						!Resource ? TEXT("The camera render target has no resource.") : TEXT("The camera render target could not read its pixels."),
						TEXT("pie_runtime"), !Resource ? TEXT("render_resource_available") : TEXT("pixels_read"),
						MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
				else CaptureError = Native(TEXT("pie_runtime_camera_capture_read_failed"), TEXT("The camera capture pixel count does not match the requested dimensions."),
					TEXT("pie_runtime"), TEXT("pixel_count"), Number(OutPixels.Num()), Number(static_cast<int64>(Size.X) * Size.Y));
			}
		}
		else CaptureError = Native(TEXT("pie_runtime_camera_capture_read_failed"), TEXT("The camera capture could not create a required rendering component."),
			TEXT("pie_runtime"), !IsValid(RenderTarget) ? TEXT("render_target_available") : TEXT("capture_component_available"),
			MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
		if (IsValid(Capture)) Capture->TextureTarget = nullptr;
		if (IsValid(RenderTarget)) RenderTarget->ReleaseResource();
		CaptureActor->Destroy();
		if (!bSucceeded)
		{
			OutPixels.Reset();
			return Fail(OutError, MoveTemp(CaptureError));
		}
		OutSize = Size;
		OutSourceIdentity = MoveTemp(SourceIdentity);
		return true;
	}

	bool FUeAgentPieRuntimeDebugRegistry::TryGetCachedSnapshot(
		const FString& SessionRef,
		FUeAgentPieRuntimeDebugCachedSnapshot& OutSnapshot,
		UE::UAI::Contract::FDiagnostic& OutError) const
	{
		FScopeLock Lock(&Mutex);
		if (!State.IsValid() || State->Snapshot.SessionRef != SessionRef)
			return Fail(OutError, SessionFailure(State.Get(), SessionRef));
		OutSnapshot.SessionRef = State->Snapshot.SessionRef;
		OutSnapshot.PieInstance = State->Snapshot.PieInstance;
		OutSnapshot.LocalPlayerIndex = State->Snapshot.LocalPlayerIndex;
		OutSnapshot.PauseEpoch = State->Snapshot.PauseEpoch;
		OutSnapshot.WorkspaceRevision = State->Snapshot.WorkspaceRevision;
		OutSnapshot.WorkspacePath = State->Snapshot.WorkspacePath;
		return true;
	}

	bool FUeAgentPieRuntimeDebugRegistry::TryResume(const FString& SessionRef, UE::UAI::Contract::FDiagnostic& OutError)
	{
		FScopeLock Lock(&Mutex);
		if (!IsInGameThread()) return Fail(OutError, GameThreadFailure());
		if (!State.IsValid() || State->Snapshot.SessionRef != SessionRef)
			return Fail(OutError, SessionFailure(State.Get(), SessionRef));
		if (!IsExactWorld(*State)) return Fail(OutError, WorldFailure(*State));
		const bool bPauseCommandSucceeded = GEditor->SetPIEWorldsPaused(false);
		if (!bPauseCommandSucceeded || State->Snapshot.World->IsPaused())
		{
			return Fail(OutError, Native(TEXT("pie_runtime_resume_verification_failed"), TEXT("PIE could not enter the requested running state."),
				TEXT("pie_runtime"), TEXT("pause"), Facts({ { TEXT("command_succeeded"), MakeShared<FJsonValueBoolean>(bPauseCommandSucceeded) }, { TEXT("paused"), MakeShared<FJsonValueBoolean>(State->Snapshot.World->IsPaused()) } }),
				Facts({ { TEXT("command_succeeded"), MakeShared<FJsonValueBoolean>(true) }, { TEXT("paused"), MakeShared<FJsonValueBoolean>(false) } })));
		}
		GEditor->PlaySessionResumed();
		return true;
	}

	bool FUeAgentPieRuntimeDebugRegistry::TryRepauseAndVerify(
		const FString& SessionRef,
		int64& OutPauseEpoch,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		FScopeLock Lock(&Mutex);
		if (!IsInGameThread()) return Fail(OutError, GameThreadFailure());
		if (!State.IsValid() || State->Snapshot.SessionRef != SessionRef)
			return Fail(OutError, SessionFailure(State.Get(), SessionRef));
		if (!IsExactWorld(*State)) return Fail(OutError, WorldFailure(*State));
		const bool bPauseCommandSucceeded = GEditor->SetPIEWorldsPaused(true);
		if (!bPauseCommandSucceeded || !State->Snapshot.World->IsPaused())
			return Fail(OutError, Native(TEXT("pie_runtime_pause_verification_failed"), TEXT("PIE could not enter the requested paused state."),
				TEXT("pie_runtime"), TEXT("pause"), Facts({ { TEXT("command_succeeded"), MakeShared<FJsonValueBoolean>(bPauseCommandSucceeded) }, { TEXT("paused"), MakeShared<FJsonValueBoolean>(State->Snapshot.World->IsPaused()) } }),
				Facts({ { TEXT("command_succeeded"), MakeShared<FJsonValueBoolean>(true) }, { TEXT("paused"), MakeShared<FJsonValueBoolean>(true) } })));
		GEditor->PlaySessionPaused();
		OutPauseEpoch = ++State->Snapshot.PauseEpoch;
		return true;
	}

	bool FUeAgentPieRuntimeDebugRegistry::TryRefreshCurrentWorkspace(
		const FString& SessionRef,
		int64 ExpectedWorkspaceRevision,
		int64& OutWorkspaceRevision,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		FScopeLock Lock(&Mutex);
		if (!IsInGameThread()) return Fail(OutError, GameThreadFailure());
		if (!State.IsValid() || State->Snapshot.SessionRef != SessionRef)
			return Fail(OutError, SessionFailure(State.Get(), SessionRef));
		if (State->Snapshot.WorkspaceRevision != ExpectedWorkspaceRevision)
			return Fail(OutError, Native(TEXT("pie_runtime_workspace_revision_stale"), TEXT("The requested workspace revision no longer matches the active PIE workspace."),
				TEXT("pie_runtime"), TEXT("workspace_revision"), Number(State->Snapshot.WorkspaceRevision), Number(ExpectedWorkspaceRevision)));
		if (!IsExactWorld(*State))
			return Fail(OutError, UE::UAI::Contract::FDiagnostic::WrapFailure(TEXT("pie_runtime_refresh_requires_verified_pause"), WorldFailure(*State),
				TEXT("pie_runtime"), TEXT("The workspace cannot be refreshed without a valid paused PIE world.")));
		if (!State->Snapshot.World->IsPaused())
			return Fail(OutError, Native(TEXT("pie_runtime_refresh_requires_verified_pause"), TEXT("The PIE world must be paused before refreshing its workspace."),
				TEXT("pie_runtime"), TEXT("paused"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)));
		const int64 NewRevision = ExpectedWorkspaceRevision + 1;
		if (!PublishWorkspace(*State, NewRevision, OutError)) return false;
		OutWorkspaceRevision = NewRevision;
		return true;
	}

	bool FUeAgentPieRuntimeDebugRegistry::TrackReferences(const FString& SessionRef,
		const TArray<TSharedPtr<FJsonValue>>& Refs, const TArray<TSharedPtr<FJsonValue>>& Forget, UE::UAI::Contract::FDiagnostic& OutError)
	{
		FScopeLock Lock(&Mutex);
		if (!IsInGameThread()) return Fail(OutError, Native(TEXT("pie_runtime_session_stale"), TEXT("Reference tracking must execute on the game thread."),
			TEXT("pie_runtime"), TEXT("game_thread"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)));
		if (!State || State->Snapshot.SessionRef != SessionRef) return Fail(OutError, SessionFailure(State.Get(), SessionRef));
		for (int32 Index = 0; Index < Forget.Num(); ++Index)
		{
			const auto& Item = Forget[Index];
			if (!Item || Item->Type != EJson::Object) return Fail(OutError, ArgumentFailure(TEXT("pie_runtime_watch_ref_invalid"),
				TEXT("A reference to forget must be an object."), FString::Printf(TEXT("forget/%d"), Index), Item, MakeShared<FJsonValueString>(TEXT("object"))));
			const FString Key = JsonText(Item->AsObject().ToSharedRef());
			State->WatchedReferences.Remove(Key);
			State->ReferenceBaselines.Remove(Key);
		}
		for (int32 Index = 0; Index < Refs.Num(); ++Index)
		{
			const auto& Item = Refs[Index];
			FString Alias;
			if (!Item || Item->Type != EJson::Object || !Item->AsObject()->TryGetStringField(TEXT("alias"), Alias))
			{
				const bool bObject = Item && Item->Type == EJson::Object;
				return Fail(OutError, ArgumentFailure(TEXT("pie_runtime_watch_ref_invalid"),
					bObject ? TEXT("A watched reference must contain a string alias.") : TEXT("A watched reference must be an object."),
					FString::Printf(TEXT("refs/%d"), Index) + (bObject ? TEXT("/alias") : TEXT("")),
					bObject ? Item->AsObject()->TryGetField(TEXT("alias")) : Item,
					MakeShared<FJsonValueString>(bObject ? TEXT("string alias") : TEXT("object"))));
			}
			const FString Key = JsonText(Item->AsObject().ToSharedRef());
			if (!State->WatchedReferences.Contains(Key))
			{
				State->WatchedReferences.Add(Key, Item->AsObject());
				State->ReferenceBaselines.Add(Key, ReferenceSignature(*State, Item->AsObject()));
			}
		}
		return true;
	}

	TSharedRef<FJsonObject> FUeAgentPieRuntimeDebugRegistry::ReferenceUpdates(const FString& SessionRef) const
	{
		FScopeLock Lock(&Mutex);
		const auto Result = MakeShared<FJsonObject>();
		const bool bKnown = State && State->Snapshot.SessionRef == SessionRef;
		Result->SetBoolField(TEXT("baseline_known"), bKnown);
		if (bKnown) Result->SetArrayField(TEXT("changes"), State->ImportantUpdates);
		return Result;
	}

	TSharedPtr<FUeAgentPieRuntimeInputState> FUeAgentPieRuntimeDebugRegistry::GetInputState(const FString& SessionRef) const
	{
		FScopeLock Lock(&Mutex);
		return State && State->Snapshot.SessionRef == SessionRef ? State->Input : nullptr;
	}

	bool FUeAgentPieRuntimeDebugRegistry::ReleaseInputs(const FString& SessionRef, UE::UAI::Contract::FDiagnostic& OutError)
	{
		FScopeLock Lock(&Mutex);
		if (!IsInGameThread()) return Fail(OutError, GameThreadFailure());
		if (!State || State->Snapshot.SessionRef != SessionRef) return Fail(OutError, SessionFailure(State.Get(), SessionRef));
		if (APlayerController* Controller = State->Snapshot.PlayerController.Get())
		{
			const auto Pressed = State->Input->Keys.Array();
			for (const FKey& Key : Pressed)
			{
				UE::UAI::Contract::FDiagnostic Error;
				if (!State->Input->Key(State->Snapshot, Key, IE_Released, 0.0, Error))
					Controller->InputKey(FInputKeyEventArgs::CreateSimulated(Key, IE_Released, 0.0f));
			}
			for (const auto& Axis : State->Input->Axes) Controller->InputKey(FInputKeyEventArgs::CreateSimulated(Axis.Key, IE_Axis, 0.0f));
		}
		if (ULocalPlayer* Player = State->Snapshot.LocalPlayer.Get())
			if (auto* Input = ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(Player))
				for (const auto& Action : State->Input->Actions)
					if (Action.Key.IsValid()) Input->InjectInputForAction(Action.Key.Get(), FInputActionValue(Action.Value.GetValueType(), FVector::ZeroVector), {}, {});
		State->Input->Keys.Reset();
		State->Input->Axes.Reset();
		State->Input->Actions.Reset();
		State->Input->ReleaseSlate();
		return true;
	}

	bool FUeAgentPieRuntimeDebugRegistry::TryEndSession(
		const FString& SessionRef,
		UE::UAI::Contract::FDiagnostic& OutError)
	{
		if (!ReleaseInputs(SessionRef, OutError))
		{
			OutError = UE::UAI::Contract::FDiagnostic::WrapFailure(TEXT("pie_runtime_input_release_failed"), OutError,
				TEXT("cleanup"), TEXT("Held session input could not be released."));
			return false;
		}
		FScopeLock Lock(&Mutex);
		if (!IsInGameThread()) return Fail(OutError, GameThreadFailure());
		if (!State.IsValid() || State->Snapshot.SessionRef != SessionRef)
			return Fail(OutError, SessionFailure(State.Get(), SessionRef));
		if (IsExactWorld(*State))
		{
			const bool bRestoreSucceeded = GEditor->SetPIEWorldsPaused(State->bWasPausedOnActivation);
			if (!bRestoreSucceeded || State->Snapshot.World->IsPaused() != State->bWasPausedOnActivation)
			{
				return Fail(OutError, Native(TEXT("pie_runtime_end_pause_restore_failed"), TEXT("The previous PIE pause state could not be restored before ending the session."),
					TEXT("cleanup"), TEXT("pause"), Facts({ { TEXT("command_succeeded"), MakeShared<FJsonValueBoolean>(bRestoreSucceeded) }, { TEXT("paused"), MakeShared<FJsonValueBoolean>(State->Snapshot.World->IsPaused()) } }),
					Facts({ { TEXT("command_succeeded"), MakeShared<FJsonValueBoolean>(true) }, { TEXT("paused"), MakeShared<FJsonValueBoolean>(State->bWasPausedOnActivation) } })));
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

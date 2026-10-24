#include "Core/AssetPreview/UeAgentAssetPreviewState.h"
#include "Core/AssetPreview/UeAgentAssetPreviewTimeline.h"

#include "AdvancedPreviewScene.h"
#include "Animation/DebugSkelMeshComponent.h"
#include "Animation/AnimationAsset.h"
#include "AnimPreviewInstance.h"
#include "Engine/SkeletalMesh.h"
#include "IPersonaPreviewScene.h"
#include "AssetViewerSettings.h"
#include "Core/EditorCapture/UeAgentEditorWidgetTree.h"
#include "Core/EditorViewport/UeAgentViewportRealtimeState.h"
#include "Editor.h"
#include "EditorViewportClient.h"
#include "Engine/Blueprint.h"
#include "Engine/StaticMesh.h"
#include "Engine/TextureCube.h"
#include "Engine/Texture.h"
#include "Framework/Docking/TabManager.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "JsonObjectConverter.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "MaterialEditor/MaterialEditorMeshComponent.h"
#include "ThumbnailRendering/ThumbnailManager.h"
#include "SEditorViewport.h"
#include "Editor/EditorPerProjectUserSettings.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Toolkits/AssetEditorToolkit.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectIterator.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringAssetIdentity.h"
#include "Widgets/Docking/SDockTab.h"

namespace UE::UAI::AssetPreview
{
	UE::UAI::Contract::FDiagnostic PreviewFailure(const TCHAR* Code, const FString& Reason,
		const UObject* Asset, const FString& Target)
	{
		auto Error = UE::UAI::Contract::FDiagnostic::Failure(Code, Reason, TEXT("asset_preview"));
		if (Asset) Error.TargetPath = Asset->GetPathName();
		Error.CandidateName = Target; Error.SourceKind = TEXT("native_result"); return Error;
	}
	UE::UAI::Contract::FDiagnostic PreviewValueFailure(const TCHAR* Code, const FString& Reason,
		const FString& PropertyPath, TSharedPtr<FJsonValue> Expected, TSharedPtr<FJsonValue> Actual,
		const UObject* Asset, const FString& Target)
	{
		auto Error = PreviewFailure(Code, Reason, Asset, Target);
		Error.SourceKind.Reset(); Error.PropertyPath = PropertyPath;
		Error.ExpectedValue = MoveTemp(Expected); Error.ActualValue = MoveTemp(Actual); return Error;
	}
	TSharedPtr<FJsonValue> PreviewNumber(double Value)
	{
		return FMath::IsFinite(Value) ? TSharedPtr<FJsonValue>(MakeShared<FJsonValueNumber>(Value))
			: TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(FString::Printf(TEXT("%.17g"), Value)));
	}

	namespace
	{
		constexpr const TCHAR* Prefix = TEXT("uai.preview.");
		TMap<FString, TSharedPtr<FLocalState>> LocalStates;

		UMaterialEditorMeshComponent* FindMaterialSubject(const FContext& Context, int32* OutCount = nullptr)
		{
			if (OutCount) *OutCount = 0;
			if (!Context.Viewport || !Context.Viewport->GetViewportClient()) return nullptr;
			const UWorld* World = Context.Viewport->GetViewportClient()->GetWorld();
			UMaterialEditorMeshComponent* Found = nullptr;
			int32 Count = 0;
			for (TObjectIterator<UMaterialEditorMeshComponent> It; It; ++It)
			{
				if (!It->IsRegistered() || It->GetWorld() != World) continue;
				++Count;
				Found = *It;
			}
			if (OutCount) *OutCount = Count;
			return Count == 1 ? Found : nullptr;
		}

		UStaticMesh* PrimitiveMesh(EUeAgentAssetPreviewShape Shape)
		{
			auto& Thumbnails = UThumbnailManager::Get();
			switch (Shape)
			{
			case EUeAgentAssetPreviewShape::Sphere: return Thumbnails.EditorSphere;
			case EUeAgentAssetPreviewShape::Cube: return Thumbnails.EditorCube;
			case EUeAgentAssetPreviewShape::Plane: return Thumbnails.EditorPlane;
			case EUeAgentAssetPreviewShape::Cylinder: return Thumbnails.EditorCylinder;
			default: return nullptr;
			}
		}

		void ReadSubject(const FContext& Context, FLocalState& State)
		{
			State.bHasSubject = false;
			State.bSkeletalSubject = false;
			if (auto* Component = FindMaterialSubject(Context))
			{
				State.Subject.Mesh = Component->GetStaticMesh();
				State.Subject.Transform = Component->GetRelativeTransform();
				State.Subject.Shape = EUeAgentAssetPreviewShape::Custom;
				for (auto Shape : {EUeAgentAssetPreviewShape::Sphere, EUeAgentAssetPreviewShape::Cube,
					EUeAgentAssetPreviewShape::Plane, EUeAgentAssetPreviewShape::Cylinder})
					if (PrimitiveMesh(Shape) == Component->GetStaticMesh()) State.Subject.Shape = Shape;
				State.bHasSubject = true;
			}
			else if (Context.ViewportType == TEXT("SAnimationEditorViewport") && Context.AdvancedScene)
			{
				const auto* Scene = static_cast<IPersonaPreviewScene*>(Context.AdvancedScene);
				if (const auto* Mesh = Scene->GetPreviewMeshComponent())
				{
					State.SkeletalSubject.Mesh = Mesh->GetSkeletalMeshAsset();
					State.SkeletalSubject.ReferencePose = Scene->IsShowReferencePoseEnabled();
					State.bHasSubject = State.bSkeletalSubject = true;
				}
			}
		}

		void RefreshCaptureCapabilities(const FContext& Context, FLocalState& State)
		{
			// A live subject change can remove timeline/reset support. Hidden fields
			// must not keep driving capture, but a closed editor is not such a change.
			if (Context.Source != TEXT("live")) return;
			const auto Actions = GetActions(Context.Asset.Get(), Context.Target);
			if (!Actions.Contains(TEXT("reset"))) State.Capture.ResetBeforeCapture = false;
			if (!Actions.Contains(TEXT("seek")))
				State.Capture.Sampling = EUeAgentAssetPreviewSampling::Realtime;
		}

		void ReadViewport(const FContext& Context, FLocalState& State)
		{
			if (Context.bWidgetPreview) { ReadWidgetState(Context, State); return; }
			if (Context.ViewportType == TEXT("STextureEditorViewport"))
			{
				State.bImagePreview = true;
				ReadImageState(Context, State);
				return;
			}
			if (!Context.Viewport) return;
			const auto Client = Context.Viewport->GetViewportClient();
			if (!Client) return;
			State.View.Location = Client->GetViewLocation();
			State.View.Rotation = Client->GetViewRotation();
			State.View.OrbitCamera = Client->bUsingOrbitCamera;
			State.View.LookAtLocation = Client->GetLookAtLocation();
			State.View.CameraLocked = Client->IsCameraLocked();
			State.View.FOV = Client->FOVAngle;
			State.View.Projection = Client->GetViewportType();
			State.View.OrthoZoom = Client->GetOrthoZoom();
			State.Display.ViewMode = Client->GetViewMode();
			State.Display.Realtime = EditorViewport::ReadRealtimeState(*Client).bConfigured;
			State.bHasView = true;
			ReadSubject(Context, State);
			State.bHasPlayback = ReadPlayback(Context, State.Playback);
			State.bHasSimulation = ReadSimulationState(Context.Asset.Get(), State.Simulation);
		}

		struct FNativeViewportBinding
		{
			TArray<FName> Tabs;
			FName Widget;
			bool bAdvancedScene = true;
		};

		bool FindNativeBinding(const UObject* Asset, FNativeViewportBinding& Out, const FString& Target)
		{
			if (!Asset) return false;
			if (Asset->IsA<UTexture>())
			{
				Out = {{TEXT("TextureEditor_Viewport")}, TEXT("STextureEditorViewport"), false};
				return true;
			}
			if (const auto* Material = Cast<UMaterialInterface>(Asset))
			{
				if (Material->GetMaterial()->MaterialDomain == MD_UI) return false;
			}
			for (const UClass* Type = Asset->GetClass(); Type; Type = Type->GetSuperClass())
			{
				const FName Name = Type->GetFName();
				if (Name == TEXT("LevelSequence"))
				{
					Out = {{}, TEXT("SLevelViewport"), false};
					return true;
				}
				if (Name == TEXT("PCGGraph"))
				{
					const auto Targets = GetTargets(Asset);
					const int32 Index = Targets.IndexOfByKey(Target);
					if (Index == INDEX_NONE) return false;
					Out = {{Index == 0 ? FName(TEXT("Viewport")) : FName(*FString::Printf(TEXT("Viewport%d"), Index + 1))}, TEXT("SPCGEditorViewport")};
					return true;
				}
				if (Name == TEXT("PaperTileSet"))
				{
					if (Target == TEXT("texture")) Out = {{TEXT("TextureCanvas")}, TEXT("STileSetSelectorViewport"), false};
					else if (Target == TEXT("tile")) Out = {{TEXT("SingleTileEditor")}, TEXT("SSingleTileEditorViewport"), false};
					else return false;
					return true;
				}
				if (Name == TEXT("MaterialInterface") || Name == TEXT("MaterialFunctionInterface"))
				{
					Out = {{TEXT("MaterialEditor_Preview"), TEXT("MaterialInstanceEditor_Preview")},
						TEXT("SMaterialEditor3DPreviewViewport")};
					return true;
				}
				if (Name == TEXT("StaticMesh"))
				{
					Out = {{TEXT("StaticMeshEditor_Viewport")}, TEXT("SStaticMeshEditorViewport")};
					return true;
				}
				if (Name == TEXT("AnimationAsset") || Name == TEXT("SkeletalMesh")
					|| Name == TEXT("Skeleton") || Name == TEXT("AnimBlueprint")
					|| Name == TEXT("PhysicsAsset") || Name == TEXT("ControlRigBlueprint"))
				{
					Out = {{TEXT("Viewport")}, TEXT("SAnimationEditorViewport")};
					return true;
				}
				if (Name == TEXT("NiagaraSystem") || Name == TEXT("NiagaraEmitter"))
				{
					Out = {{TEXT("NiagaraSystemEditor_Viewport")}, TEXT("SNiagaraSystemViewport")};
					return true;
				}
				if (Name == TEXT("PaperSprite") || Name == TEXT("PaperFlipbook"))
				{
					Out = {{TEXT("Viewport")}, Name == TEXT("PaperSprite")
						? FName(TEXT("SSpriteEditorViewport")) : FName(TEXT("SFlipbookEditorViewport")), false};
					return true;
				}
			}
			if (const auto* Blueprint = Cast<UBlueprint>(Asset))
			{
				if (FBlueprintEditorUtils::DoesSupportComponents(Blueprint))
				{
					Out = {{TEXT("SCSViewport")}, TEXT("SSCSEditorViewport"), false};
					return true;
				}
			}
			return false;
		}

		bool RegisterPropertySemantic(const FString& Group, UE::UAI::Contract::FDiagnostic& OutError)
		{
			auto& Registry = UeAgentAuthoringSemanticRegistry::FRegistry::Get();
			if (!UeAgentAuthoringSemanticRegistry::RegisterBuiltInSemantics(Registry, OutError)) return false;
			UeAgentAuthoringSemanticRegistry::FSemanticDescriptor Descriptor;
			if (!Registry.FindSemantic(TEXT("core.property"), Descriptor))
			{
				OutError = PreviewFailure(TEXT("preview_property_semantic_missing"), TEXT("The base core.property semantic is not registered, so the preview property group cannot be registered."), nullptr, Group);
				OutError.ExpectedValue = MakeShared<FJsonValueString>(TEXT("registered core.property semantic")); OutError.ActualValuePresent = false;
				return false;
			}
			Descriptor.SemanticId = FString(Prefix) + Group + TEXT(".property");
			UeAgentAuthoringSemanticRegistry::ERegistrationResult Result;
			return Registry.RegisterSemantic(Descriptor, Result, OutError);
		}

		FString ProfileGroup(const FProperty* Property)
		{
			const FString Name = Property->GetName();
			if (Name == TEXT("ProfileName") || Name == TEXT("bSharedProfile")) return {};
			if (Name == TEXT("PostProcessingSettings") || Name == TEXT("bPostProcessingEnabled"))
				return TEXT("post_process");
			if (Name == TEXT("bShowGrid") || Name == TEXT("bShowMeshEdges") || Name == TEXT("bEnableToneMapping"))
				return TEXT("display");
			return TEXT("environment");
		}
	}

	bool IsPreviewSemantic(const FString& SemanticId)
	{
		return SemanticId.StartsWith(Prefix) && SemanticId.EndsWith(TEXT(".property"));
	}

	TArray<FString> GetTargets(const UObject* Asset)
	{
		for (const UClass* Type = Asset ? Asset->GetClass() : nullptr; Type; Type = Type->GetSuperClass())
		{
			if (Type->GetFName() == TEXT("PCGGraph"))
				return {TEXT("data_viewport_1"), TEXT("data_viewport_2"), TEXT("data_viewport_3"), TEXT("data_viewport_4")};
			if (Type->GetFName() == TEXT("PaperTileSet")) return {TEXT("tile"), TEXT("texture")};
		}
		return {TEXT("main")};
	}

	void FEditorStateContinuity::Capture()
	{
		Entries.Reset();
		Adjustments.Reset();
		auto* Editors = GEditor ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr;
		if (!Editors) return;
		for (UObject* Asset : Editors->GetAllEditedAssets())
		{
			for (const FString& Target : GetTargets(Asset))
			{
				FContext Context;
				UE::UAI::Contract::FDiagnostic Error;
				if (!ResolveContext(Asset, false, Context, Error, Target) || Context.Source != TEXT("live")) continue;
				// Shared audio playback is not an editor-local viewport preference.
				if (!Context.Viewport && !Context.ImageViewport && !Context.PreviewWidget) continue;
				FEntry Entry;
				Entry.State.Asset = Asset;
				Entry.State.AssetPath = Asset->GetPathName();
				Entry.State.Target = Target;
				ReadViewport(Context, Entry.State);
				Entry.Viewport = Context.Viewport;
				Entry.ImageViewport = Context.ImageViewport;
				Entry.PreviewWidget = Context.PreviewWidget;
				if (Context.AdvancedScene && UAssetViewerSettings::Get()->Profiles.IsValidIndex(Context.ProfileIndex))
				{
					Entry.ProfileIndex = Context.ProfileIndex;
					Entry.ProfileName = UAssetViewerSettings::Get()->Profiles[Context.ProfileIndex].ProfileName;
				}
				auto State = MakeShared<FLocalState>(Entry.State);
				for (const FString Group : {TEXT("subject"), TEXT("view"), TEXT("display"), TEXT("image"), TEXT("widget"), TEXT("playback"), TEXT("simulation")})
				{
					if (State->bWidgetPreview ? Group != TEXT("widget") || !State->bWidgetSizeWritable
						: Group == TEXT("widget") || !State->bHasView) continue;
					if (Group == TEXT("subject") && !State->bHasSubject) continue;
					if (Group == TEXT("playback") && !State->bHasPlayback) continue;
					if (Group == TEXT("simulation") && !State->bHasSimulation) continue;
					if (Group == TEXT("image") && !State->bImagePreview) continue;
					if (Group == TEXT("display") && State->bImagePreview) continue;
					UScriptStruct* Type;
					void* Data;
					if (!ResolveLocalOwner(State, Group, Type, Data)) continue;
					for (TFieldIterator<FProperty> It(Type); It; ++It)
					{
						if (!It->HasAnyPropertyFlags(CPF_Edit)) continue;
						if (State->bImagePreview && (!IsImageFieldSupported(Context, Group, It->GetFName())
							|| ImageFieldScope(Group, It->GetFName()) != TEXT("viewport"))) continue;
						if (Group == TEXT("playback") && !IsPlaybackFieldSupported(Context, It->GetFName())) continue;
						Entry.Fields.FindOrAdd(Group).Add(It->GetFName());
					}
				}
				Entries.Add(MoveTemp(Entry));
			}
		}
	}

	bool FEditorStateContinuity::Restore(UE::UAI::Contract::FDiagnostic& OutError)
	{
		for (auto& Entry : Entries)
		{
			UObject* Asset = FindObject<UObject>(nullptr, *Entry.State.AssetPath);
			if (!Asset) continue; // An intentionally deleted asset has no preview to restore.
			FContext Context;
			if (!ResolveContext(Asset, false, Context, OutError, Entry.State.Target)) return false;
			if (Context.Source == TEXT("live") && Context.Viewport == Entry.Viewport.Pin() && Context.ImageViewport == Entry.ImageViewport.Pin()
				&& Context.PreviewWidget == Entry.PreviewWidget.Pin() && Entry.State.Asset.Get() == Asset) continue;
			if (Context.Source != TEXT("live") && !ResolveContext(Asset, true, Context, OutError, Entry.State.Target)) return false;
			if (Context.AdvancedScene && UAssetViewerSettings::Get()->Profiles.IsValidIndex(Entry.ProfileIndex)
				&& UAssetViewerSettings::Get()->Profiles[Entry.ProfileIndex].ProfileName == Entry.ProfileName)
				Context.AdvancedScene->SetProfileIndex(Entry.ProfileIndex);
			FLocalState Current;
			ReadViewport(Context, Current);
			const auto NoteAdjustment = [&](const TCHAR* Field)
			{
				auto Notice = UE::UAI::Contract::FDiagnostic::Failure(TEXT("preview_selection_reset"),
                    TEXT("The previous preview reference is unavailable; the current native selection was retained."), TEXT("asset_preview"));
                Notice.Severity = UE::UAI::Contract::ESeverity::Warning;
                Notice.TargetPath = Entry.State.AssetPath;
                Notice.SubjectId = Entry.State.Target;
                Notice.PropertyPath = Field;
                if (!Adjustments.ContainsByPredicate([&](const auto& Existing) {
                    return Existing.Code == Notice.Code && Existing.TargetPath == Notice.TargetPath
                        && Existing.SubjectId == Notice.SubjectId && Existing.PropertyPath == Notice.PropertyPath;
                })) Adjustments.Add(MoveTemp(Notice));
			};
			if (Entry.Fields.Contains(TEXT("subject")))
			{
				if (Entry.State.bSkeletalSubject && Current.bSkeletalSubject
					&& !Entry.State.SkeletalSubject.Mesh.IsNull() && !Entry.State.SkeletalSubject.Mesh.LoadSynchronous())
				{
					Entry.State.SkeletalSubject.Mesh = Current.SkeletalSubject.Mesh;
					NoteAdjustment(TEXT("subject.Mesh"));
				}
				else if (!Entry.State.bSkeletalSubject && Current.bHasSubject
					&& Entry.State.Subject.Shape == EUeAgentAssetPreviewShape::Custom
					&& !Entry.State.Subject.Mesh.IsNull() && !Entry.State.Subject.Mesh.LoadSynchronous()
					&& Current.Subject.Mesh.LoadSynchronous())
				{
					Entry.State.Subject.Mesh = Current.Subject.Mesh;
					Entry.State.Subject.Shape = Current.Subject.Shape;
					NoteAdjustment(TEXT("subject.Mesh/Shape"));
				}
			}
			for (const FString Group : {TEXT("subject"), TEXT("view"), TEXT("display"), TEXT("image"), TEXT("widget"), TEXT("playback"), TEXT("simulation")})
			{
				const auto* SavedFields = Entry.Fields.Find(Group);
				if (!SavedFields) continue;
				if (Group == TEXT("widget") && !Current.bWidgetSizeWritable) continue;
				if (Group == TEXT("subject") && (!Current.bHasSubject || Current.bSkeletalSubject != Entry.State.bSkeletalSubject)) continue;
				if (Group == TEXT("playback") && !Current.bHasPlayback) continue;
				if (Group == TEXT("simulation") && !Current.bHasSimulation) continue;
				TSet<FName> Fields = *SavedFields;
				if (Group == TEXT("playback"))
				{
					for (auto It = Fields.CreateIterator(); It; ++It)
						if (!IsPlaybackFieldSupported(Context, *It)) It.RemoveCurrent();
					if ((Fields.Contains(TEXT("StartSection")) || Fields.Contains(TEXT("MontageMode")))
						&& ReconcilePlaybackSelection(Context, Entry.State.Playback))
						NoteAdjustment(TEXT("playback.StartSection"));
				}
				// ApplyLocalState refreshes all groups after a write. Keep the captured
				// values independent so restoring one group cannot erase the next.
				FLocalState Saved = Entry.State;
				if (!Fields.IsEmpty() && !ApplyLocalState(Saved, Group, Fields, OutError)) return false;
			}
			Entry.State.Asset = Asset;
			Entry.Viewport = Context.Viewport;
			Entry.ImageViewport = Context.ImageViewport;
			Entry.PreviewWidget = Context.PreviewWidget;
		}
		return true;
	}

	TSharedPtr<FLocalState> FindLocalState(const FString& AssetPath, const FString& Target)
	{
		const FString ObjectPath = UeAgentAuthoringAssetIdentity::ToObjectPath(AssetPath);
		const FString Selected = Target.IsEmpty() ? GetTargets(FindObject<UObject>(nullptr, *ObjectPath))[0] : Target;
		const auto* Found = LocalStates.Find(ObjectPath + TEXT(":") + Selected);
		if (!Found) return nullptr;
		FContext Context;
		UE::UAI::Contract::FDiagnostic Error;
		if (ResolveContext(FindObject<UObject>(nullptr, *ObjectPath), false, Context, Error, Selected))
			RefreshCaptureCapabilities(Context, **Found);
		return *Found;
	}

	TSharedPtr<FJsonObject> ReadConfiguration(const FContext& Context)
	{
		auto Result = MakeShared<FJsonObject>();
		FLocalState State;
		ReadViewport(Context, State);
		auto Add = [&Result, &Context](const TCHAR* Name, const UStruct* Type, const void* Data)
		{
			auto Value = MakeShared<FJsonObject>();
			FJsonObjectConverter::UStructToJsonObject(Type, Data, Value, CPF_Edit, 0);
			if (Type == FUeAgentAssetPreviewPlayback::StaticStruct())
				for (TFieldIterator<FProperty> It(Type); It; ++It)
					if (!IsPlaybackFieldSupported(Context, It->GetFName())) Value->RemoveField(FJsonObjectConverter::StandardizeCase(It->GetName()));
			if (Type == FUeAgentAssetPreviewImage::StaticStruct() || Type == FUeAgentAssetPreviewImageView::StaticStruct())
				for (TFieldIterator<FProperty> It(Type); It; ++It)
					if (!IsImageFieldSupported(Context, Name, It->GetFName())) Value->RemoveField(FJsonObjectConverter::StandardizeCase(It->GetName()));
			Result->SetObjectField(Name, Value);
		};
		if (State.bHasView && !State.bImagePreview)
		{
			Add(TEXT("view"), FUeAgentAssetPreviewView::StaticStruct(), &State.View);
			Add(TEXT("display"), FUeAgentAssetPreviewDisplay::StaticStruct(), &State.Display);
			if (const auto Client = Context.Viewport ? Context.Viewport->GetViewportClient() : nullptr)
			{
				const auto Realtime = EditorViewport::ReadRealtimeState(*Client);
				auto Status = MakeShared<FJsonObject>();
				Status->SetBoolField(TEXT("configured"), Realtime.bConfigured);
				Status->SetBoolField(TEXT("effective"), Realtime.bEffective);
				if (!Realtime.OverrideReason.IsEmpty()) Status->SetStringField(TEXT("override_reason"), Realtime.OverrideReason);
				Result->SetObjectField(TEXT("realtime_state"), Status);
			}
		}
		if (State.bImagePreview)
		{
			Add(TEXT("view"), FUeAgentAssetPreviewImageView::StaticStruct(), &State.ImageView);
			Add(TEXT("image"), FUeAgentAssetPreviewImage::StaticStruct(), &State.Image);
		}
		if (State.bWidgetPreview) Add(TEXT("widget"), FUeAgentAssetPreviewWidget::StaticStruct(), &State.Widget);
		if (State.bHasPlayback) Add(TEXT("playback"), FUeAgentAssetPreviewPlayback::StaticStruct(), &State.Playback);
		if (const auto Rate = PreviewFrameRate(Context); Rate.IsSet())
		{
			auto Clock = MakeShared<FJsonObject>();
			Clock->SetNumberField(TEXT("frame_rate_numerator"), Rate->Numerator);
			Clock->SetNumberField(TEXT("frame_rate_denominator"), Rate->Denominator);
			Clock->SetBoolField(TEXT("continuous_available"), FindPreviewSequencer(Context.Asset.Get()).IsValid());
			Clock->SetBoolField(TEXT("direct_timeline_available"), Context.ViewportType != TEXT("SNiagaraSystemViewport"));
			Result->SetObjectField(TEXT("timeline_clock"), Clock);
		}
		if (State.bHasSimulation) Add(TEXT("simulation"), FUeAgentAssetPreviewSimulation::StaticStruct(), &State.Simulation);
		if (State.bSkeletalSubject) Add(TEXT("subject"), FUeAgentAssetPreviewSkeletalSubject::StaticStruct(), &State.SkeletalSubject);
		else if (State.bHasSubject) Add(TEXT("subject"), FUeAgentAssetPreviewSubject::StaticStruct(), &State.Subject);
		if (Context.AdvancedScene)
		{
			const auto* Settings = UAssetViewerSettings::Get();
			if (Settings->Profiles.IsValidIndex(Context.ProfileIndex))
			{
				Add(TEXT("shared_profile"), FPreviewSceneProfile::StaticStruct(), &Settings->Profiles[Context.ProfileIndex]);
				Result->SetNumberField(TEXT("profile_index"), Context.ProfileIndex);
			}
		}
		return Result;
	}

	bool ResolveLocalOwner(const TSharedPtr<FLocalState>& State, const FString& Group,
		UScriptStruct*& OutType, void*& OutData)
	{
		OutType = nullptr;
		OutData = nullptr;
		if (!State) return false;
		if (Group == TEXT("subject") && State->bSkeletalSubject)
		{ OutType = FUeAgentAssetPreviewSkeletalSubject::StaticStruct(); OutData = &State->SkeletalSubject; return true; }
		if (Group == TEXT("simulation") && State->bHasSimulation)
		{ OutType = FUeAgentAssetPreviewSimulation::StaticStruct(); OutData = &State->Simulation; return true; }
		if (Group == TEXT("audio") && State->bAudioPreview)
		{ OutType = FUeAgentAssetPreviewAudio::StaticStruct(); OutData = &State->Audio; return true; }
		if (Group == TEXT("widget") && State->bWidgetPreview)
		{ OutType = FUeAgentAssetPreviewWidget::StaticStruct(); OutData = &State->Widget; return true; }
		if (Group == TEXT("view") && State->bImagePreview) { OutType = FUeAgentAssetPreviewImageView::StaticStruct(); OutData = &State->ImageView; }
		else if (Group == TEXT("image") && State->bImagePreview) { OutType = FUeAgentAssetPreviewImage::StaticStruct(); OutData = &State->Image; }
		else if (Group == TEXT("view")) { OutType = FUeAgentAssetPreviewView::StaticStruct(); OutData = &State->View; }
		else if (Group == TEXT("display")) { OutType = FUeAgentAssetPreviewDisplay::StaticStruct(); OutData = &State->Display; }
		else if (Group == TEXT("capture")) { OutType = FUeAgentAssetPreviewCapture::StaticStruct(); OutData = &State->Capture; }
		else if (Group == TEXT("subject")) { OutType = FUeAgentAssetPreviewSubject::StaticStruct(); OutData = &State->Subject; }
		else if (Group == TEXT("playback")) { OutType = FUeAgentAssetPreviewPlayback::StaticStruct(); OutData = &State->Playback; }
		return OutType != nullptr;
	}

	bool ApplyLocalState(FLocalState& State, const FString& Group, const TSet<FName>& Fields, UE::UAI::Contract::FDiagnostic& OutError)
	{
		const auto InvalidValue = [&](const TCHAR* Code, const TCHAR* Field, const TCHAR* Reason, const TCHAR* Expected, TSharedPtr<FJsonValue> Actual) {
			OutError = PreviewValueFailure(Code, Reason, Field, MakeShared<FJsonValueString>(Expected), MoveTemp(Actual), State.Asset.Get(), State.Target);
			OutError.TargetPath = State.AssetPath; return false;
		};
		if (Group == TEXT("capture"))
		{
			const TCHAR* Code = TEXT("preview_capture_parameters_invalid");
			for (const auto& Dimension : TArray<TPair<const TCHAR*, int32>>{{TEXT("RenderWidth"), State.Capture.RenderWidth}, {TEXT("RenderHeight"), State.Capture.RenderHeight}, {TEXT("Width"), State.Capture.Width}, {TEXT("Height"), State.Capture.Height}})
				if (Dimension.Value < 0) return InvalidValue(Code, Dimension.Key, TEXT("A capture dimension must be non-negative; zero uses the configured native extent."), TEXT("integer >= 0"), PreviewNumber(Dimension.Value));
			if (State.Capture.FrameCount < 1) return InvalidValue(Code, TEXT("FrameCount"), TEXT("Capture requires at least one frame."), TEXT("integer >= 1"), PreviewNumber(State.Capture.FrameCount));
			if (!FMath::IsFinite(State.Capture.IntervalSeconds) || State.Capture.IntervalSeconds < 0) return InvalidValue(Code, TEXT("IntervalSeconds"), TEXT("The capture interval must be finite and non-negative in seconds."), TEXT("finite seconds >= 0"), PreviewNumber(State.Capture.IntervalSeconds));
			if (!FMath::IsFinite(State.Capture.WarmupSeconds) || State.Capture.WarmupSeconds < 0) return InvalidValue(Code, TEXT("WarmupSeconds"), TEXT("The capture warmup must be finite and non-negative in seconds."), TEXT("finite seconds >= 0"), PreviewNumber(State.Capture.WarmupSeconds));
			if (!FMath::IsFinite(State.Capture.StartTimeSeconds)) return InvalidValue(Code, TEXT("StartTimeSeconds"), TEXT("The capture start time must be finite."), TEXT("finite seconds"), PreviewNumber(State.Capture.StartTimeSeconds));
			if (!FMath::IsFinite(State.Capture.StepTimeSeconds) || State.Capture.StepTimeSeconds < 0) return InvalidValue(Code, TEXT("StepTimeSeconds"), TEXT("The capture time step must be finite and non-negative in seconds."), TEXT("finite seconds >= 0"), PreviewNumber(State.Capture.StepTimeSeconds));
			if (State.Capture.StepFrames < 0) return InvalidValue(Code, TEXT("StepFrames"), TEXT("The capture frame step must be non-negative."), TEXT("integer >= 0"), MakeShared<FJsonValueString>(LexToString(State.Capture.StepFrames)));
			return true;
		}
		if (Group == TEXT("view") && !State.bImagePreview) {
			const TCHAR* Code = TEXT("preview_view_parameters_invalid");
			if (State.View.Location.ContainsNaN()) return InvalidValue(Code, TEXT("Location"), TEXT("The camera location must have finite X, Y and Z components."), TEXT("finite vector components"), MakeShared<FJsonValueString>(State.View.Location.ToString()));
			if (State.View.Rotation.ContainsNaN()) return InvalidValue(Code, TEXT("Rotation"), TEXT("The camera rotation must have finite pitch, yaw and roll components."), TEXT("finite rotation components"), MakeShared<FJsonValueString>(State.View.Rotation.ToString()));
			if (!FMath::IsFinite(State.View.FOV) || State.View.FOV <= 0 || State.View.FOV >= 180) return InvalidValue(Code, TEXT("FOV"), TEXT("The camera field of view must be finite and strictly between zero and 180 degrees."), TEXT("0 < degrees < 180"), PreviewNumber(State.View.FOV));
			if (!FMath::IsFinite(State.View.OrthoZoom) || State.View.OrthoZoom <= 0) return InvalidValue(Code, TEXT("OrthoZoom"), TEXT("Orthographic zoom must be finite and positive."), TEXT("finite number > 0"), PreviewNumber(State.View.OrthoZoom));
			if (State.View.Projection >= LVT_MAX) return InvalidValue(Code, TEXT("Projection"), TEXT("The viewport projection value is outside the native viewport type range."), TEXT("viewport type below LVT_MAX"), PreviewNumber(static_cast<int32>(State.View.Projection)));
		}
		State.Asset = FindObject<UObject>(nullptr, *State.AssetPath);
		if (State.bAudioPreview && Group == TEXT("audio"))
			return ApplyAudioState(State.Asset.Get(), State.Audio, Fields, OutError);
		FContext Context;
		if (!ResolveContext(State.Asset.Get(), true, Context, OutError, State.Target)) return false;
		State.Asset = Context.Asset;
		if (Group == TEXT("subject") && State.bSkeletalSubject)
		{
			if (Context.ViewportType != TEXT("SAnimationEditorViewport") || !Context.AdvancedScene)
			{ OutError = PreviewFailure(TEXT("preview_skeletal_subject_unavailable"), Context.ViewportType != TEXT("SAnimationEditorViewport") ? TEXT("The preview target is not an animation editor viewport.") : TEXT("The animation viewport has no advanced preview scene."), State.Asset.Get(), State.Target); OutError.ActualValue = MakeShared<FJsonValueString>(Context.ViewportType.ToString()); return false; }
			auto* Scene = static_cast<IPersonaPreviewScene*>(Context.AdvancedScene);
			if (Fields.Contains(TEXT("Mesh")))
			{
				auto* Mesh = State.SkeletalSubject.Mesh.LoadSynchronous();
				if (!Mesh && !State.SkeletalSubject.Mesh.IsNull()) { OutError = PreviewValueFailure(TEXT("preview_subject_mesh_unavailable"), TEXT("The requested skeletal preview mesh could not be loaded."), TEXT("Mesh"), MakeShared<FJsonValueString>(TEXT("loadable skeletal mesh")), MakeShared<FJsonValueString>(State.SkeletalSubject.Mesh.ToSoftObjectPath().ToString()), State.Asset.Get(), State.Target); return false; }
				Scene->SetPreviewMesh(Mesh, false);
			}
			if (Fields.Contains(TEXT("ReferencePose")) && Scene->IsShowReferencePoseEnabled() != State.SkeletalSubject.ReferencePose)
			{
				auto* Component = Scene->GetPreviewMeshComponent();
				auto* Player = Component ? Component->PreviewInstance.Get() : nullptr;
				if (State.SkeletalSubject.ReferencePose)
				{
					State.ReferencePosePlayer = Player;
					State.ReferencePoseAnimation = Scene->GetPreviewAnimationAsset();
					ReadPlayback(Context, State.ReferencePosePlayback);
					if (Player)
					{
						State.ReferencePoseTime = Player->GetCurrentTime();
						State.bReferencePosePlaying = Player->IsPlaying();
					}
					Scene->ShowReferencePose(true);
				}
				else
				{
					// Native ShowReferencePose(false) is a no-op. Re-establish the actual
					// prior animation, or let EnablePreview(false) restore the saved AnimBP.
					const bool bRestore = Player && State.ReferencePosePlayer.Get() == Player && State.ReferencePoseAnimation.IsValid();
					auto* Animation = bRestore ? State.ReferencePoseAnimation.Get() : Cast<UAnimationAsset>(State.Asset.Get());
					Scene->SetPreviewAnimationAsset(Animation, Animation != nullptr);
					if (bRestore && Component->PreviewInstance)
					{
						TSet<FName> RestoreFields;
						for (const FName Name : {FName(TEXT("Loop")), FName(TEXT("MontageMode")), FName(TEXT("StartSection")), FName(TEXT("BlendInputX")), FName(TEXT("BlendInputY"))})
							if (IsPlaybackFieldSupported(Context, Name)) RestoreFields.Add(Name);
						if (!ApplyPlayback(Context, State.ReferencePosePlayback, RestoreFields, OutError)) return false;
						TSharedPtr<FJsonObject> Status;
						if (!ExecuteAction(State.Asset.Get(), State.Target, TEXT("seek"), State.ReferencePoseTime, Status, OutError)
							|| !ExecuteAction(State.Asset.Get(), State.Target, State.bReferencePosePlaying ? TEXT("play") : TEXT("pause"), {}, Status, OutError)) return false;
					}
					State.ReferencePosePlayer.Reset();
					State.ReferencePoseAnimation.Reset();
				}
			}
			return true;
		}
		if (State.bWidgetPreview && Group == TEXT("widget")) return ApplyWidgetState(Context, State.Widget, OutError);
		if (Group == TEXT("simulation")) return ApplySimulationState(State.Asset.Get(), State.Simulation, Fields, OutError);
		if (State.bImagePreview) return ApplyImageState(Context, State, Group, Fields, OutError);
		const auto Client = Context.Viewport ? Context.Viewport->GetViewportClient() : nullptr;
		if (!Client) { OutError = PreviewFailure(TEXT("asset_preview_viewport_unavailable"), TEXT("The resolved asset preview has no native viewport client to apply settings."), State.Asset.Get(), State.Target); return false; }
		if (Group == TEXT("playback"))
		{
			if (!ApplyPlayback(Context, State.Playback, Fields, OutError)) return false;
		}
		else if (Group == TEXT("subject"))
		{
			int32 SubjectCount = 0;
			auto* Component = FindMaterialSubject(Context, &SubjectCount);
			if (!Component) { OutError = PreviewFailure(TEXT("preview_subject_unavailable"), SubjectCount == 0 ? TEXT("The preview world has no material subject mesh component.") : TEXT("The preview world has multiple material subject mesh components, so the subject is ambiguous."), State.Asset.Get(), State.Target); OutError.ExpectedValue = PreviewNumber(1); OutError.ActualValue = PreviewNumber(SubjectCount); return false; }
			if (Fields.Contains(TEXT("Mesh")) || Fields.Contains(TEXT("Shape")))
			{
				UStaticMesh* Mesh = Fields.Contains(TEXT("Shape")) && State.Subject.Shape != EUeAgentAssetPreviewShape::Custom
					? PrimitiveMesh(State.Subject.Shape) : State.Subject.Mesh.LoadSynchronous();
				if (!Mesh) {
					const bool bShape = Fields.Contains(TEXT("Shape")) && State.Subject.Shape != EUeAgentAssetPreviewShape::Custom;
					OutError = PreviewValueFailure(TEXT("preview_subject_mesh_unavailable"), bShape ? TEXT("No native primitive mesh is available for the requested preview shape.") : TEXT("The requested static preview mesh could not be loaded."), bShape ? TEXT("Shape") : TEXT("Mesh"),
					MakeShared<FJsonValueString>(TEXT("available preview mesh")), bShape ? PreviewNumber(static_cast<int32>(State.Subject.Shape)) : TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(State.Subject.Mesh.ToSoftObjectPath().ToString())), State.Asset.Get(), State.Target); return false;
				}
				if (Fields.Contains(TEXT("Shape")) && Fields.Contains(TEXT("Mesh")) && State.Subject.Mesh.LoadSynchronous() != Mesh)
				{ OutError = PreviewValueFailure(TEXT("preview_subject_shape_mesh_conflict"), TEXT("The explicit mesh does not match the mesh selected by the requested preview shape."), TEXT("Mesh"), MakeShared<FJsonValueString>(Mesh->GetPathName()), MakeShared<FJsonValueString>(State.Subject.Mesh.ToSoftObjectPath().ToString()), State.Asset.Get(), State.Target); return false; }
				if (Component->GetStaticMesh() != Mesh)
				{
					if (!Component->SetStaticMesh(Mesh))
					{ OutError = PreviewFailure(TEXT("preview_subject_mesh_apply_failed"), TEXT("The native material preview component rejected the requested static mesh."), State.Asset.Get(), State.Target); OutError.ExpectedValue = MakeShared<FJsonValueString>(Mesh->GetName()); OutError.ActualValue = MakeShared<FJsonValueString>(GetNameSafe(Component->GetStaticMesh())); return false; }
					// Native material preview primitives start in their own default pose. In
					// particular EditorPlane faces away from the default camera without this yaw.
					// Explicit transforms (including continuity snapshots) still take precedence.
					if (!Fields.Contains(TEXT("Transform")))
						Component->SetRelativeTransform(Mesh == PrimitiveMesh(EUeAgentAssetPreviewShape::Plane)
							? FTransform(FRotator(0.0, 180.0, 0.0)) : FTransform::Identity);
				}
			}
			if (Fields.Contains(TEXT("Transform"))) Component->SetRelativeTransform(State.Subject.Transform);
			Component->UpdateBounds();
			if (Context.AdvancedScene) Context.AdvancedScene->SetFloorOffset(-Component->Bounds.Origin.Z + Component->Bounds.BoxExtent.Z);
		}
		else if (Group == TEXT("view"))
		{
			// Select the native transform first. Projection/lock/orbit setters can
			// change the active camera or pose; explicit transform fields follow them.
			if (Fields.Contains(TEXT("Projection")) && Client->GetViewportType() != State.View.Projection) Client->SetViewportType(State.View.Projection);
			if (Fields.Contains(TEXT("CameraLocked")) && Client->IsCameraLocked() != State.View.CameraLocked) Client->SetCameraLock();
			if (Fields.Contains(TEXT("OrbitCamera"))) Client->ToggleOrbitCamera(State.View.OrbitCamera);
			if (Fields.Contains(TEXT("LookAtLocation"))) Client->SetLookAtLocation(State.View.LookAtLocation);
			if (Fields.Contains(TEXT("Location"))) Client->SetViewLocation(State.View.Location);
			if (Fields.Contains(TEXT("Rotation"))) Client->SetViewRotation(State.View.Rotation);
			if (Fields.Contains(TEXT("FOV"))) Client->ViewFOV = Client->FOVAngle = State.View.FOV;
			if (Fields.Contains(TEXT("OrthoZoom"))) Client->SetOrthoZoom(State.View.OrthoZoom);
		}
		else if (Group == TEXT("display"))
		{
			if (Fields.Contains(TEXT("ViewMode"))) Client->SetViewMode(State.Display.ViewMode);
			if (Fields.Contains(TEXT("Realtime"))) EditorViewport::SetRealtimePreference(*Client, State.Display.Realtime);
		}
		Client->Invalidate();
		ReadViewport(Context, State);
		return true;
	}

	FString GroupFromSemantic(const FString& SemanticId)
	{
		return IsPreviewSemantic(SemanticId)
			? SemanticId.Mid(FCString::Strlen(Prefix)).LeftChop(9) : FString();
	}

	bool ResolveContext(UObject* Asset, bool bOpen, FContext& Out, UE::UAI::Contract::FDiagnostic& OutError, const FString& Target)
	{
		Out = {};
		Out.Asset = Asset;
		if (!Asset || !GEditor)
		{
			OutError = PreviewFailure(TEXT("preview_asset_or_editor_unavailable"), !Asset ? TEXT("The asset requested for preview is unavailable.") : TEXT("The Unreal Editor preview service is unavailable."), Asset, Target);
			return false;
		}
		FNativeViewportBinding Binding;
		const auto Targets = GetTargets(Asset);
		Out.Target = Target.IsEmpty() ? Targets[0] : Target;
		if (!Targets.Contains(Out.Target))
		{ OutError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("asset_preview_target_unknown"),
		TEXT("The requested asset preview target is unavailable."), TEXT("asset_preview"));
		OutError.CandidateName = Out.Target;
		TArray<TSharedPtr<FJsonValue>> Choices;
		for (const FString& Choice : Targets) Choices.Add(MakeShared<FJsonValueString>(Choice));
		OutError.CandidateChoices = MakeShared<FJsonValueArray>(Choices); return false; }
		if (IsAudioPreview(Asset))
		{
			FUeAgentAssetPreviewAudio Audio;
			Out.Source = ReadAudioState(Asset, Audio) ? TEXT("live") : TEXT("configured");
			Out.ViewportType = TEXT("AudioPreview");
			return true;
		}
		if (IsWidgetPreview(Asset)) return ResolveWidgetContext(Asset, bOpen, Out, OutError);
		if (!FindNativeBinding(Asset, Binding, Out.Target))
		{
			OutError = PreviewFailure(TEXT("asset_preview_context_unsupported"), TEXT("No supported native viewport binding exists for this asset class and preview target."), Asset, Out.Target);
			OutError.ActualValue = MakeShared<FJsonValueString>(Asset->GetClass()->GetName());
			return false;
		}
		Out.ViewportType = Binding.Widget;
		if (Binding.Widget == TEXT("SLevelViewport")) return ResolveSequenceContext(Asset, bOpen, Out, OutError);
		if (Asset->IsA<UTexture>()) return ResolveImageContext(Asset, bOpen, Out, OutError);
		auto* Subsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
		IAssetEditorInstance* Editor = Subsystem ? Subsystem->FindEditorForAsset(Asset, bOpen) : nullptr;
		if (!Editor && bOpen && Subsystem)
		{
			if (!OpenAssetEditor(Asset))
			{ OutError = PreviewFailure(TEXT("asset_preview_editor_open_failed"), TEXT("Unreal could not open the asset editor required for this preview target."), Asset, Out.Target); return false; }
			Editor = Subsystem->FindEditorForAsset(Asset, false);
		}
		Out.Asset = Asset;
		const auto Tabs = Editor ? Editor->GetAssociatedTabManager() : nullptr;
		if (Tabs)
		{
			for (const FName TabName : Binding.Tabs)
			{
				auto Tab = Tabs->FindExistingLiveTab(FTabId(TabName));
				if (!Tab && bOpen && Tabs->HasTabSpawner(FName(TabName)))
					Tab = Tabs->TryInvokeTab(FTabId(TabName), false);
				if (Tab && bOpen) Tab->ActivateInParent(ETabActivationCause::SetDirectly);
				TSharedPtr<SWidget> Found;
				if (!Tab || !EditorCapture::WidgetTree::FindFirstByType(Tab->GetContent(),
					Binding.Widget, Found)) continue;
				Out.Viewport = StaticCastSharedPtr<SEditorViewport>(Found);
				const auto Client = Out.Viewport->GetViewportClient();
				if (!Client) continue;
				Out.AdvancedScene = Binding.bAdvancedScene
					? static_cast<FAdvancedPreviewScene*>(Client->GetPreviewScene()) : nullptr;
				if (!Binding.bAdvancedScene || Out.AdvancedScene)
				{
					if (Out.AdvancedScene) Out.ProfileIndex = Out.AdvancedScene->GetCurrentProfileIndex();
					Out.Source = TEXT("live");
					return true;
				}
			}
		}
		if (bOpen)
		{
			OutError = PreviewFailure(TEXT("asset_preview_viewport_unavailable"), !Subsystem ? TEXT("The asset editor subsystem is unavailable.") : !Editor ? TEXT("No asset editor is available after the preview open request.") : !Tabs ? TEXT("The asset editor has no tab manager.") : Out.Viewport ? TEXT("The preview viewport has no usable client or required preview scene.") : TEXT("The editor's preview tabs do not contain the required viewport widget."), Asset, Out.Target);
			OutError.ExpectedValue = MakeShared<FJsonValueString>(Binding.Widget.ToString());
			return false;
		}
		Out.Source = TEXT("defaults");
		if (!Binding.bAdvancedScene) return true;
		Out.ProfileIndex = GetDefault<UEditorPerProjectUserSettings>()->AssetViewerProfileIndex;
		const auto* Settings = UAssetViewerSettings::Get();
		if (!Settings->Profiles.IsValidIndex(Out.ProfileIndex)) Out.ProfileIndex = 0;
		return true;
	}

	bool AppendState(const FString& TargetKind,
		TArray<UeAgentAuthoringState::FStateEntry>& Entries, UE::UAI::Contract::FDiagnostic& OutError)
	{
		FString AssetPath;
		for (const auto& Entry : Entries)
		{
			if (const auto* Path = Entry.TargetIdentity.Selectors.Find(TEXT("asset_path")))
			{
				AssetPath = *Path;
				break;
			}
		}
		UObject* Asset = FindObject<UObject>(nullptr, *UeAgentAuthoringAssetIdentity::ToObjectPath(AssetPath));
		for (const FString& Target : GetTargets(Asset))
		{
		FNativeViewportBinding Binding;
		const bool bAudio = IsAudioPreview(Asset);
		const bool bWidget = IsWidgetPreview(Asset);
		if (!bAudio && !bWidget && !FindNativeBinding(Asset, Binding, Target)) continue;
		FContext Context;
		if (!ResolveContext(Asset, false, Context, OutError, Target)) return false;
		auto& Local = LocalStates.FindOrAdd(Asset->GetPathName() + TEXT(":") + Target);
		if (!Local) Local = MakeShared<FLocalState>();
		Local->Asset = Asset;
		Local->AssetPath = Asset->GetPathName();
		Local->Target = Target;
		Local->bAudioPreview = bAudio;
		if (bAudio) ReadAudioState(Asset, Local->Audio);
		ReadViewport(Context, *Local);
		RefreshCaptureCapabilities(Context, *Local);
		if (!bAudio && !bWidget && Binding.bAdvancedScene)
		{
		UAssetViewerSettings* Settings = UAssetViewerSettings::Get();
		if (!Settings->Profiles.IsValidIndex(Context.ProfileIndex))
		{
			OutError = PreviewFailure(TEXT("preview_profile_unavailable"), TEXT("The preview profile index is outside the available asset-viewer profiles."), Asset, Target);
			OutError.ExpectedValue = MakeShared<FJsonValueString>(FString::Printf(TEXT("0 <= profile index < %d"), Settings->Profiles.Num()));
			OutError.ActualValue = PreviewNumber(Context.ProfileIndex);
			return false;
		}
		auto& Profile = Settings->Profiles[Context.ProfileIndex];
		// Native profile loading resolves the stored soft path, including redirects.
		// It does not start a preview or persist the settings.
		Profile.LoadEnvironmentMap();
		const FString Owner = Settings->GetPathName() + TEXT(":") + Target + TEXT(":Profiles[")
			+ FString::FromInt(Context.ProfileIndex) + TEXT("]:") + Profile.ProfileName;
		TArray<UeAgentAuthoringState::FReadRequest> Requests;
		for (TFieldIterator<FProperty> It(FPreviewSceneProfile::StaticStruct()); It; ++It)
		{
			FProperty* Property = *It;
			if (!Property->HasAnyPropertyFlags(CPF_Edit)) continue;
			const FString Group = ProfileGroup(Property);
			if (Group.IsEmpty()) continue;
			if (!RegisterPropertySemantic(Group, OutError)) return false;
			UeAgentAuthoringState::FReadRequest Request;
			UeAgentAuthoringTargetLocator::FResolveResult Resolve;
			if (!UeAgentAuthoringState::BuildStructPropertyReadRequest(FPreviewSceneProfile::StaticStruct(),
				&Profile, Owner, Settings, Property->GetName(), FString(Prefix) + Group + TEXT(".property"), Request, Resolve))
			{
				OutError = !Resolve.Diagnostic.IsEmpty() ? UE::UAI::Contract::FDiagnostic::WrapFailure(TEXT("preview_property_read_request_failed"), Resolve.Diagnostic, TEXT("asset_preview"))
					: PreviewFailure(TEXT("preview_property_read_request_failed"), TEXT("The preview profile property could not be resolved for state readback."), Asset, Target);
				if (OutError.PropertyPath.IsEmpty()) OutError.PropertyPath = Property->GetName();
				if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = AssetPath;
				return false;
			}
			Requests.Add(MoveTemp(Request));
		}
		UeAgentAuthoringState::FCaptureResult Capture;
		if (!UeAgentAuthoringState::CaptureSnapshot(Requests, Capture))
		{
			OutError = Capture.Diagnostic;
			return false;
		}
		for (auto Entry : Capture.Snapshot.CopyEntries())
		{
			Entry.TargetIdentity.Selectors.Add(TEXT("asset_path"), AssetPath);
			Entry.TargetIdentity.Selectors.Add(TEXT("preview_target"), Target);
			Entry.TargetIdentity.Selectors.Add(TEXT("preview_profile"), Profile.ProfileName);
			Entry.TargetIdentity.Selectors.Add(TEXT("preview_profile_index"), FString::FromInt(Context.ProfileIndex));
			Entry.Constraints.Add(TEXT("scope"), TEXT("shared_profile"));
			Entry.Constraints.Add(TEXT("profile"), Profile.ProfileName);
			Entry.Provenance = TEXT("asset_preview.") + Context.Source;
			Entries.Add(MoveTemp(Entry));
		}
		}
		for (const FString Group : {FString(TEXT("view")), FString(TEXT("display")), FString(TEXT("subject")), FString(TEXT("playback")), FString(TEXT("simulation")), FString(TEXT("image")), FString(TEXT("audio")), FString(TEXT("widget")), FString(TEXT("capture"))})
		{
			if (bAudio != (Group == TEXT("audio"))) continue;
			if (Group != TEXT("capture") && bWidget != (Group == TEXT("widget"))) continue;
			if (Group == TEXT("widget") && !Local->bWidgetSizeWritable) continue;
			// Exact camera values require the actual subject and viewport extent.
			// Do not invent a live camera for an unopened asset editor.
			if (Group != TEXT("capture") && !bAudio && !Local->bHasView) continue;
			if (Group == TEXT("subject") && !Local->bHasSubject) continue;
			if (Group == TEXT("playback") && !Local->bHasPlayback) continue;
			if (Group == TEXT("simulation") && !Local->bHasSimulation) continue;
			if (Group == TEXT("image") && !Local->bImagePreview) continue;
			if (Group == TEXT("display") && Local->bImagePreview) continue;
			UScriptStruct* Type;
			void* Data;
			if (!ResolveLocalOwner(Local, Group, Type, Data)) continue;
			if (!RegisterPropertySemantic(Group, OutError)) return false;
			TArray<UeAgentAuthoringState::FReadRequest> LocalRequests;
			for (TFieldIterator<FProperty> It(Type); It; ++It)
			{
				if (!It->HasAnyPropertyFlags(CPF_Edit)) continue;
				if (Local->bImagePreview && Group != TEXT("capture") && !IsImageFieldSupported(Context, Group, It->GetFName())) continue;
				if (Group == TEXT("playback") && !IsPlaybackFieldSupported(Context, It->GetFName())) continue;
				if (Group == TEXT("capture"))
				{
					const FName Field = It->GetFName();
					if (bWidget && (Field == TEXT("RenderWidth") || Field == TEXT("RenderHeight"))) continue;
					const auto Actions = GetActions(Asset, Target);
					if (Field == TEXT("ResetBeforeCapture") && !Actions.Contains(TEXT("reset"))) continue;
					if ((Field == TEXT("Sampling") || Field == TEXT("StartTimeSeconds") || Field == TEXT("StepTimeSeconds")
						|| Field == TEXT("TimeUnit") || Field == TEXT("StartFrame") || Field == TEXT("StepFrames"))
						&& !Actions.Contains(TEXT("seek"))) continue;
				}
				UeAgentAuthoringState::FReadRequest Request;
				UeAgentAuthoringTargetLocator::FResolveResult Resolve;
				if (!UeAgentAuthoringState::BuildStructPropertyReadRequest(Type, Data,
					Asset->GetPathName() + TEXT(":preview:") + Target + TEXT(":") + Group, Asset, It->GetName(),
					FString(Prefix) + Group + TEXT(".property"), Request, Resolve))
				{
					OutError = !Resolve.Diagnostic.IsEmpty() ? UE::UAI::Contract::FDiagnostic::WrapFailure(TEXT("preview_local_property_read_failed"), Resolve.Diagnostic, TEXT("asset_preview"))
						: PreviewFailure(TEXT("preview_local_property_read_failed"), TEXT("The local preview property could not be resolved for state readback."), Asset, Target);
					if (OutError.PropertyPath.IsEmpty()) OutError.PropertyPath = It->GetName();
					if (OutError.TargetPath.IsEmpty()) OutError.TargetPath = AssetPath;
					return false;
				}
				LocalRequests.Add(MoveTemp(Request));
			}
			UeAgentAuthoringState::FCaptureResult LocalCapture;
			if (!UeAgentAuthoringState::CaptureSnapshot(LocalRequests, LocalCapture))
			{
				OutError = LocalCapture.Diagnostic;
				return false;
			}
			for (auto Entry : LocalCapture.Snapshot.CopyEntries())
			{
				Entry.TargetIdentity.Selectors.Add(TEXT("asset_path"), AssetPath);
				Entry.TargetIdentity.Selectors.Add(TEXT("preview_target"), Target);
				Entry.TargetIdentity.Selectors.Add(TEXT("preview_local_group"), Group);
				Entry.Constraints.Add(TEXT("scope"), Group == TEXT("capture") ? TEXT("capture")
					: bAudio ? TEXT("shared_audio_preview")
					: Context.ViewportType == TEXT("SLevelViewport") ? TEXT("shared_level_viewport")
					: Local->bImagePreview ? ImageFieldScope(Group, FName(*Entry.TargetIdentity.Selectors.FindRef(TEXT("property_path")))) : TEXT("viewport"));
				Entry.Provenance = TEXT("asset_preview.") + (Group == TEXT("capture") ? FString(TEXT("configured"))
					: bAudio ? Context.Source : Context.Viewport || Context.ImageViewport || Context.PreviewWidget ? FString(TEXT("live")) : FString(TEXT("last_observed")));
				Entries.Add(MoveTemp(Entry));
			}
		}
		}
		return true;
	}
}

#include "Core/AssetPreview/UeAgentAssetPreviewState.h"

#include "ILevelEditor.h"
#include "ISequencer.h"
#include "LevelEditor.h"
#include "LevelEditorViewport.h"
#include "Modules/ModuleManager.h"
#include "SLevelViewport.h"
#include "Engine/World.h"
#include "Camera/CameraComponent.h"
#include "Dom/JsonValue.h"

namespace UE::UAI::AssetPreview
{
	bool ResolveSequenceContext(UObject* Asset, bool bOpen, FContext& Out, UE::UAI::Contract::FDiagnostic& OutError)
	{
		auto Sequencer = FindPreviewSequencer(Asset);
		if (!Sequencer && bOpen)
		{
			if (!OpenAssetEditor(Asset))
			{ OutError = PreviewFailure(TEXT("sequence_preview_editor_open_failed"), TEXT("The asset editor could not be opened for sequence preview."), Asset); return false; }
			Sequencer = FindPreviewSequencer(Asset);
		}
		Out.Asset = Asset;
		if (!Sequencer)
		{
			if (!bOpen) { Out.Source = TEXT("requires_open_preview"); return true; }
			OutError = PreviewFailure(TEXT("sequence_preview_sequencer_unavailable"), TEXT("The opened asset editor has no registered sequencer for this sequence preview."), Asset); return false;
		}
		auto* Module = FModuleManager::GetModulePtr<FLevelEditorModule>(TEXT("LevelEditor"));
		const auto Editor = Module ? Module->GetFirstLevelEditor() : nullptr;
		const auto Viewport = Editor ? Editor->GetActiveViewportInterface() : nullptr;
		auto* PlaybackContext = Sequencer->GetPlaybackContext();
		if (!Sequencer->IsLevelEditorSequencer() || !Viewport || !PlaybackContext
			|| PlaybackContext->GetWorld() != Viewport->GetLevelViewportClient().GetWorld())
		{
			OutError = PreviewFailure(TEXT("sequence_preview_world_binding_unavailable"), !Sequencer->IsLevelEditorSequencer() ? TEXT("This sequencer is not bound to the level editor.")
				: !Viewport ? TEXT("The level editor has no active viewport for sequence preview.") : !PlaybackContext ? TEXT("The sequencer has no playback context.")
				: TEXT("The sequencer playback world differs from the active level viewport world."), Asset);
			if (Viewport && PlaybackContext) {
				OutError.ExpectedValue = MakeShared<FJsonValueString>(GetNameSafe(Viewport->GetLevelViewportClient().GetWorld()));
				OutError.ActualValue = MakeShared<FJsonValueString>(GetNameSafe(PlaybackContext->GetWorld()));
			}
			return false;
		}
		if (const auto* Camera = Sequencer->GetLastEvaluatedCameraCut().Get(); Camera && Sequencer->IsPerspectiveViewportCameraCutEnabled())
		{
			if (Viewport->GetLevelViewportClient().GetCameraComponentForView() != Camera)
			{
				OutError = PreviewFailure(TEXT("sequence_preview_camera_cut_not_in_active_viewport"), TEXT("The active viewport is not using the camera selected by the sequencer camera cut."), Asset);
				OutError.ExpectedValue = MakeShared<FJsonValueString>(Camera->GetName());
				OutError.ActualValue = MakeShared<FJsonValueString>(GetNameSafe(Viewport->GetLevelViewportClient().GetCameraComponentForView())); return false;
			}
			Out.Source = TEXT("live_sequence_camera_cut");
		}
		else Out.Source = TEXT("live_sequence_world_viewport");
		Out.Viewport = Viewport;
		return true;
	}
}

#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringState.h"
#include "Core/AssetPreview/UeAgentAssetPreviewTypes.h"

class FAdvancedPreviewScene;
class SEditorViewport;
class UObject;
class FJsonObject;
class FJsonValue;
class ISequencer;
class FSceneViewport;
class SWidget;
class UAnimationAsset;
class UDebugSkelMeshComponent;

namespace UE::UAI::AssetPreview
{
	// Producers choose the reason and public facts; consumers only attach source locations.
	UE::UAI::Contract::FDiagnostic PreviewFailure(const TCHAR* Code, const FString& Reason,
		const UObject* Asset = nullptr, const FString& Target = FString());
	UE::UAI::Contract::FDiagnostic PreviewValueFailure(const TCHAR* Code, const FString& Reason,
		const FString& PropertyPath, TSharedPtr<FJsonValue> Expected, TSharedPtr<FJsonValue> Actual,
		const UObject* Asset = nullptr, const FString& Target = FString());
	TSharedPtr<FJsonValue> PreviewNumber(double Value);

	struct FContext
	{
		TSharedPtr<SEditorViewport> Viewport;
		TSharedPtr<FSceneViewport> ImageViewport;
		TSharedPtr<SWidget> PreviewWidget;
		FAdvancedPreviewScene* AdvancedScene = nullptr;
		FString Source;
		FString Target;
		FName ViewportType;
		TWeakObjectPtr<UObject> Asset;
		int32 ProfileIndex = INDEX_NONE;
		bool bWidgetPreview = false;
	};

	struct FLocalState
	{
		TWeakObjectPtr<UObject> Asset;
		FString AssetPath;
		FString Target;
		FUeAgentAssetPreviewView View;
		FUeAgentAssetPreviewDisplay Display;
		FUeAgentAssetPreviewCapture Capture;
		FUeAgentAssetPreviewSubject Subject;
		FUeAgentAssetPreviewSkeletalSubject SkeletalSubject;
		bool bSkeletalSubject = false;
		TWeakObjectPtr<UObject> ReferencePosePlayer;
		TWeakObjectPtr<UAnimationAsset> ReferencePoseAnimation;
		float ReferencePoseTime = 0.f;
		bool bReferencePosePlaying = false;
		FUeAgentAssetPreviewPlayback ReferencePosePlayback;
		FUeAgentAssetPreviewPlayback Playback;
		FUeAgentAssetPreviewImageView ImageView;
		FUeAgentAssetPreviewImage Image;
		FUeAgentAssetPreviewAudio Audio;
		FUeAgentAssetPreviewWidget Widget;
		FUeAgentAssetPreviewSimulation Simulation;
		bool bHasSimulation = false;
		bool bWidgetPreview = false;
		bool bWidgetSizeWritable = false;
		bool bAudioPreview = false;
		bool bImagePreview = false;
		bool bHasView = false;
		bool bHasSubject = false;
		bool bHasPlayback = false;
	};

	// Transient continuity across authoring-driven editor reconstruction. This does
	// not persist preview preferences or keep the old editor/viewport alive.
	class FEditorStateContinuity
	{
	public:
		void Capture();
		bool Restore(UE::UAI::Contract::FDiagnostic& OutError);
		const TArray<UE::UAI::Contract::FDiagnostic>& GetAdjustments() const { return Adjustments; }
	private:
		struct FEntry
		{
			FLocalState State;
			TWeakPtr<SEditorViewport> Viewport;
			TWeakPtr<FSceneViewport> ImageViewport;
			TWeakPtr<SWidget> PreviewWidget;
			int32 ProfileIndex = INDEX_NONE;
			FString ProfileName;
			TMap<FString, TSet<FName>> Fields;
		};
		TArray<FEntry> Entries;
		TArray<UE::UAI::Contract::FDiagnostic> Adjustments;
	};
	TArray<FString> GetTargets(const UObject* Asset);
	TSharedPtr<FJsonObject> ReadConfiguration(const FContext& Context);
	TSharedPtr<FLocalState> FindLocalState(const FString& AssetPath, const FString& Target = FString());
	bool ResolveLocalOwner(const TSharedPtr<FLocalState>& State, const FString& Group,
		UScriptStruct*& OutType, void*& OutData);
	bool ApplyLocalState(FLocalState& State, const FString& Group, const TSet<FName>& Fields, UE::UAI::Contract::FDiagnostic& OutError);
	// Native preview actions are separate from stable workspace configuration.
	// Temporary capture actions preserve the configured speed across native Pause.
	bool ExecuteAction(UObject* Asset, const FString& Target, const FString& Action,
		TOptional<double> TimeSeconds, TSharedPtr<FJsonObject>& OutDetails, UE::UAI::Contract::FDiagnostic& OutError,
		bool bPreservePlaybackRate = false);
	bool ReadPlayback(const FContext& Context, FUeAgentAssetPreviewPlayback& Out);
	// Automatic restoration uses the native current selection if its old identity was removed.
	bool ReconcilePlaybackSelection(const FContext& Context, FUeAgentAssetPreviewPlayback& Value);
	// Evaluates the selected preview pose without advancing its timeline.
	void RefreshSkeletalPreviewPose(UDebugSkelMeshComponent& Mesh);
	bool IsPlaybackFieldSupported(const FContext& Context, FName Field);
	TArray<FString> GetActions(UObject* Asset, const FString& Target);
	// Native opening may destroy/recreate the asset (notably UWorld). Rebind the
	// caller before any subsequent object access; never keep the old object alive.
	// Background preview work reuses an existing toolkit without activating it.
	// Explicit editor opening may request the native bring-to-front behavior.
	UEAGENTINTERFACE_API bool OpenAssetEditor(UObject*& Asset, bool bFocusExistingEditor = false);
	void StartSequencerObservation();
	void StopSequencerObservation();
	TSharedPtr<ISequencer> FindPreviewSequencer(UObject* Asset);
	bool ResetNiagaraPreview(UObject* Asset, UE::UAI::Contract::FDiagnostic& OutError);
	bool ResolveImageContext(UObject* Asset, bool bOpen, FContext& Out, UE::UAI::Contract::FDiagnostic& OutError);
	bool ReadImageState(const FContext& Context, FLocalState& State);
	void ReadImageStatus(const FContext& Context, FJsonObject& Out);
	bool ApplyImageState(const FContext& Context, FLocalState& State, const FString& Group,
		const TSet<FName>& Fields, UE::UAI::Contract::FDiagnostic& OutError);
	bool IsImageFieldSupported(const FContext& Context, const FString& Group, FName Field);
	FString ImageFieldScope(const FString& Group, FName Field);
	bool IsAudioPreview(const UObject* Asset);
	bool IsPhysicsPreview(const UObject* Asset);
	bool ReadSimulationState(UObject* Asset, FUeAgentAssetPreviewSimulation& Out);
	bool ApplySimulationState(UObject* Asset, const FUeAgentAssetPreviewSimulation& Value,
		const TSet<FName>& Fields, UE::UAI::Contract::FDiagnostic& OutError);
	bool ExecuteSimulationAction(UObject* Asset, const FString& Action,
		TSharedPtr<FJsonObject>& OutDetails, UE::UAI::Contract::FDiagnostic& OutError);
	bool IsWidgetPreview(const UObject* Asset);
	bool ResolveWidgetContext(UObject* Asset, bool bOpen, FContext& Out, UE::UAI::Contract::FDiagnostic& OutError);
	bool ResolveSequenceContext(UObject* Asset, bool bOpen, FContext& Out, UE::UAI::Contract::FDiagnostic& OutError);
	bool ReadWidgetState(const FContext& Context, FLocalState& State);
	bool ApplyWidgetState(const FContext& Context, const FUeAgentAssetPreviewWidget& Value, UE::UAI::Contract::FDiagnostic& OutError);
	void ReadWidgetStatus(const FContext& Context, FJsonObject& Out);
	bool ReadAudioState(UObject* Asset, FUeAgentAssetPreviewAudio& Out);
	bool ApplyAudioState(UObject* Asset, const FUeAgentAssetPreviewAudio& Value,
		const TSet<FName>& Fields, UE::UAI::Contract::FDiagnostic& OutError);
	bool ExecuteAudioAction(UObject* Asset, const FString& Action,
		TSharedPtr<FJsonObject>& OutDetails, UE::UAI::Contract::FDiagnostic& OutError);
	bool ApplyPlayback(const FContext& Context, const FUeAgentAssetPreviewPlayback& Value,
		const TSet<FName>& Fields, UE::UAI::Contract::FDiagnostic& OutError);

	// Resolves only the requested asset's preview. Reading does not open a tab.
	bool ResolveContext(UObject* Asset, bool bOpen, FContext& Out, UE::UAI::Contract::FDiagnostic& OutError,
		const FString& Target = FString());
	bool AppendState(const FString& TargetKind,
		TArray<UeAgentAuthoringState::FStateEntry>& Entries, UE::UAI::Contract::FDiagnostic& OutError);
	bool IsPreviewSemantic(const FString& SemanticId);
	FString GroupFromSemantic(const FString& SemanticId);
}

#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"
#include "Editor/UnrealEdTypes.h"
#include "Engine/EngineBaseTypes.h"
#include "TextureEditorSettings.h"
#include "UeAgentAssetPreviewTypes.generated.h"

class UStaticMesh;
class USkeletalMesh;

USTRUCT()
struct FUeAgentAssetPreviewSkeletalSubject
{
	GENERATED_BODY()
	UPROPERTY(EditAnywhere, Category=Subject) TSoftObjectPtr<USkeletalMesh> Mesh;
	UPROPERTY(EditAnywhere, Category=Subject) bool ReferencePose = false;
};

USTRUCT()
struct FUeAgentAssetPreviewSimulation
{
	GENERATED_BODY()
	UPROPERTY(EditAnywhere, Category=Simulation) bool NoGravity = false;
	UPROPERTY(EditAnywhere, Category=Simulation) bool FloorCollision = true;
};

USTRUCT()
struct FUeAgentAssetPreviewWidget
{
	GENERATED_BODY()
	UPROPERTY(EditAnywhere, Category=Preview, meta=(ClampMin="1")) int32 Width = 1920;
	UPROPERTY(EditAnywhere, Category=Preview, meta=(ClampMin="1")) int32 Height = 1080;
};

USTRUCT()
struct FUeAgentAssetPreviewAudio
{
	GENERATED_BODY()
	UPROPERTY(EditAnywhere, Category=Audio, meta=(ClampMin="0")) float Volume = 1.f;
	UPROPERTY(EditAnywhere, Category=Audio, meta=(ClampMin="0.001")) float Pitch = 1.f;
};

USTRUCT()
struct FUeAgentAssetPreviewImageView
{
	GENERATED_BODY()
	UPROPERTY(EditAnywhere, Category=Zoom) ETextureEditorZoomMode ZoomMode = ETextureEditorZoomMode::Fit;
	UPROPERTY(EditAnywhere, Category=Zoom, meta=(ClampMin="0.001")) double CustomZoom = 1.;
	UPROPERTY(EditAnywhere, Category=Orientation) FRotator Orientation = FRotator::ZeroRotator;
};

USTRUCT()
struct FUeAgentAssetPreviewImage
{
	GENERATED_BODY()
	UPROPERTY(EditAnywhere, Category=Channels) bool Red = true;
	UPROPERTY(EditAnywhere, Category=Channels) bool Green = true;
	UPROPERTY(EditAnywhere, Category=Channels) bool Blue = true;
	UPROPERTY(EditAnywhere, Category=Channels) bool Alpha = false;
	UPROPERTY(EditAnywhere, Category=Sampling) TEnumAsByte<ETextureEditorSampling> Sampling = TextureEditorSampling_Default;
	UPROPERTY(EditAnywhere, Category=Volume, meta=(ClampMin="0", ClampMax="1")) float VolumeOpacity = 1.f;
	UPROPERTY(EditAnywhere, Category=Volume) TEnumAsByte<ETextureEditorVolumeViewMode> VolumeViewMode = TextureEditorVolumeViewMode_DepthSlices;
	UPROPERTY(EditAnywhere, Category=Cubemap) TEnumAsByte<ETextureEditorCubemapViewMode> CubemapViewMode = TextureEditorCubemapViewMode_2DView;
};

UENUM()
enum class EUeAgentAssetPreviewShape : uint8
{
	Sphere, Cube, Plane, Cylinder, Custom
};

USTRUCT()
struct FUeAgentAssetPreviewSubject
{
	GENERATED_BODY()
	UPROPERTY(EditAnywhere, Category=Subject) EUeAgentAssetPreviewShape Shape = EUeAgentAssetPreviewShape::Sphere;
	UPROPERTY(EditAnywhere, Category=Subject) TSoftObjectPtr<UStaticMesh> Mesh;
	UPROPERTY(EditAnywhere, Category=Subject) FTransform Transform = FTransform::Identity;
};

USTRUCT()
struct FUeAgentAssetPreviewView
{
	GENERATED_BODY()
	UPROPERTY(EditAnywhere, Category=Camera) FVector Location = FVector::ZeroVector;
	UPROPERTY(EditAnywhere, Category=Camera) FRotator Rotation = FRotator::ZeroRotator;
	UPROPERTY(EditAnywhere, Category=Camera, meta=(ToolTip="Native orbit mode. When enabled, Location and Rotation are orbit transform values around LookAtLocation; disable for a world-space camera pose.")) bool OrbitCamera = false;
	UPROPERTY(EditAnywhere, Category=Camera) FVector LookAtLocation = FVector::ZeroVector;
	UPROPERTY(EditAnywhere, Category=Camera, meta=(ToolTip="Native viewport camera lock. Changing the lock can reset the native camera; explicit pose and orbit fields in the same import take precedence.")) bool CameraLocked = false;
	UPROPERTY(EditAnywhere, Category=Camera, meta=(ClampMin="1", ClampMax="179")) float FOV = 90.f;
	UPROPERTY(EditAnywhere, Category=Projection) TEnumAsByte<ELevelViewportType> Projection = LVT_Perspective;
	UPROPERTY(EditAnywhere, Category=Projection, meta=(ClampMin="0.001")) float OrthoZoom = 10000.f;
};

USTRUCT()
struct FUeAgentAssetPreviewDisplay
{
	GENERATED_BODY()
	UPROPERTY(EditAnywhere, Category=Rendering) TEnumAsByte<EViewModeIndex> ViewMode = VMI_Lit;
	UPROPERTY(EditAnywhere, Category=Rendering) bool Realtime = false;
};

UENUM()
enum class EUeAgentAssetPreviewImageFormat : uint8
{
	Png,
	Jpeg
};

UENUM()
enum class EUeAgentAssetPreviewMontageMode : uint8 { Sections, AllSections };

USTRUCT()
struct FUeAgentAssetPreviewPlayback
{
	GENERATED_BODY()
	UPROPERTY(EditAnywhere, Category=Playback) float Rate = 1.f;
	UPROPERTY(EditAnywhere, Category=Playback) bool Loop = true;
	UPROPERTY(EditAnywhere, Category=Montage) EUeAgentAssetPreviewMontageMode MontageMode = EUeAgentAssetPreviewMontageMode::Sections;
	UPROPERTY(EditAnywhere, Category=Montage) FName StartSection;
	UPROPERTY(EditAnywhere, Category=BlendSpace) double BlendInputX = 0.;
	UPROPERTY(EditAnywhere, Category=BlendSpace) double BlendInputY = 0.;
};

UENUM()
enum class EUeAgentAssetPreviewSampling : uint8
{
	Realtime, Timeline, Continuous
};

UENUM()
enum class EUeAgentAssetPreviewTimeUnit : uint8
{
	Seconds, Frames
};

USTRUCT()
struct FUeAgentAssetPreviewCapture
{
	GENERATED_BODY()
	// Native render extent; one zero preserves the viewport aspect ratio, both
	// zero leave native sizing unchanged. Width/Height below only resize output.
	UPROPERTY(EditAnywhere, Category=Rendering, meta=(ClampMin="0")) int32 RenderWidth = 0;
	UPROPERTY(EditAnywhere, Category=Rendering, meta=(ClampMin="0")) int32 RenderHeight = 0;
	// Zero follows the actual preview viewport extent.
	UPROPERTY(EditAnywhere, Category=Image, meta=(ClampMin="0")) int32 Width = 0;
	UPROPERTY(EditAnywhere, Category=Image, meta=(ClampMin="0")) int32 Height = 0;
	UPROPERTY(EditAnywhere, Category=Image) EUeAgentAssetPreviewImageFormat Format = EUeAgentAssetPreviewImageFormat::Png;
	UPROPERTY(EditAnywhere, Category=Sequence, meta=(ClampMin="1")) int32 FrameCount = 1;
	UPROPERTY(EditAnywhere, Category=Sequence, meta=(ClampMin="0")) float IntervalSeconds = 0.1f;
	UPROPERTY(EditAnywhere, Category=Sequence) EUeAgentAssetPreviewSampling Sampling = EUeAgentAssetPreviewSampling::Realtime;
	UPROPERTY(EditAnywhere, Category=Sequence) EUeAgentAssetPreviewTimeUnit TimeUnit = EUeAgentAssetPreviewTimeUnit::Seconds;
	UPROPERTY(EditAnywhere, Category=Sequence) int32 StartFrame = 0;
	UPROPERTY(EditAnywhere, Category=Sequence, meta=(ClampMin="0")) int32 StepFrames = 1;
	UPROPERTY(EditAnywhere, Category=Sequence) double StartTimeSeconds = 0.;
	UPROPERTY(EditAnywhere, Category=Sequence, meta=(ClampMin="0")) double StepTimeSeconds = 0.1;
	UPROPERTY(EditAnywhere, Category=Preparation) bool ResetBeforeCapture = false;
	UPROPERTY(EditAnywhere, Category=Preparation, meta=(ClampMin="0")) double WarmupSeconds = 0.;
};

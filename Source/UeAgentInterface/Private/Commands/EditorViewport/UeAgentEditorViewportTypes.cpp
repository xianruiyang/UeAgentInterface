// Copyright Epic Games, Inc. All Rights Reserved.

#include "Commands/EditorViewport/UeAgentEditorViewportTypes.h"

namespace UeAgentEditorViewport
{
	const TCHAR* const SchemaState = TEXT("uai.editor_viewport.state.v1");
	const TCHAR* const SchemaTarget = TEXT("uai.editor_viewport.target.v1");
	const TCHAR* const SchemaScreenshot = TEXT("uai.editor_viewport.screenshot.v1");

	FViewportTarget::FViewportTarget()
		: Scope(TEXT("level"))
		, ViewportRole(TEXT("primary"))
		, Viewport(TEXT("active"))
		, bOpenEditorIfNeeded(false)
	{
	}

	FViewportCamera::FViewportCamera()
		: Location(FVector::ZeroVector)
		, Rotation(FRotator::ZeroRotator)
		, Fov(90.0f)
		, Projection(TEXT("perspective"))
		, Space(TEXT("editor_world"))
		, bValid(false)
	{
	}

	FViewportCapabilities::FViewportCapabilities()
		: bCamera(false)
		, bVisualOptions(false)
		, bPreviewState(false)
		, bScreenshot(false)
	{
	}

	FViewportScreenshotResult::FViewportScreenshotResult()
		: Width(0)
		, Height(0)
		, Bytes(0)
	{
	}

	FViewportSnapshot::FViewportSnapshot()
		: bValid(false)
	{
	}

	TSharedPtr<FJsonObject> MakeVectorJson(const FVector& Value)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("x"), Value.X);
		Obj->SetNumberField(TEXT("y"), Value.Y);
		Obj->SetNumberField(TEXT("z"), Value.Z);
		return Obj;
	}

	TSharedPtr<FJsonObject> MakeRotatorJson(const FRotator& Value)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("pitch"), Value.Pitch);
		Obj->SetNumberField(TEXT("yaw"), Value.Yaw);
		Obj->SetNumberField(TEXT("roll"), Value.Roll);
		return Obj;
	}

	TArray<TSharedPtr<FJsonValue>> MakeStringArrayJson(const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> Result;
		Result.Reserve(Values.Num());
		for (const FString& Value : Values)
		{
			Result.Add(MakeShared<FJsonValueString>(Value));
		}
		return Result;
	}

	TSharedPtr<FJsonObject> TargetToJson(const FViewportTarget& Target)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("schema"), SchemaTarget);
		Obj->SetStringField(TEXT("scope"), Target.Scope);
		Obj->SetStringField(TEXT("asset_path"), Target.AssetPath);
		Obj->SetStringField(TEXT("asset_type"), Target.AssetType);
		Obj->SetStringField(TEXT("viewport_role"), Target.ViewportRole);
		Obj->SetStringField(TEXT("viewport"), Target.Viewport);
		Obj->SetStringField(TEXT("viewport_id"), Target.ViewportId);
		Obj->SetBoolField(TEXT("open_editor_if_needed"), Target.bOpenEditorIfNeeded);
		return Obj;
	}

	TSharedPtr<FJsonObject> CameraToJson(const FViewportCamera& Camera)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetBoolField(TEXT("valid"), Camera.bValid);
		Obj->SetStringField(TEXT("space"), Camera.Space);
		Obj->SetStringField(TEXT("projection"), Camera.Projection);
		Obj->SetObjectField(TEXT("location"), MakeVectorJson(Camera.Location));
		Obj->SetObjectField(TEXT("rotation"), MakeRotatorJson(Camera.Rotation));
		Obj->SetNumberField(TEXT("fov"), Camera.Fov);
		return Obj;
	}

	TSharedPtr<FJsonObject> CapabilitiesToJson(const FViewportCapabilities& Capabilities)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetBoolField(TEXT("camera"), Capabilities.bCamera);
		Obj->SetBoolField(TEXT("visual_options"), Capabilities.bVisualOptions);
		Obj->SetBoolField(TEXT("preview_state"), Capabilities.bPreviewState);
		Obj->SetBoolField(TEXT("screenshot"), Capabilities.bScreenshot);
		Obj->SetArrayField(TEXT("focus"), MakeStringArrayJson(Capabilities.FocusModes));
		Obj->SetArrayField(TEXT("screenshot_backends"), MakeStringArrayJson(Capabilities.ScreenshotBackends));
		Obj->SetArrayField(TEXT("visual_option_keys"), MakeStringArrayJson(Capabilities.VisualOptionKeys));
		Obj->SetArrayField(TEXT("preview_state_keys"), MakeStringArrayJson(Capabilities.PreviewStateKeys));
		Obj->SetArrayField(TEXT("unsupported_options"), MakeStringArrayJson(Capabilities.UnsupportedOptions));
		return Obj;
	}

	TSharedPtr<FJsonObject> StateToJson(const FViewportState& State)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("schema"), SchemaState);
		Obj->SetObjectField(TEXT("target"), TargetToJson(State.Target));
		Obj->SetStringField(TEXT("client_class"), State.ClientClass);
		Obj->SetStringField(TEXT("adapter"), State.Adapter);
		Obj->SetObjectField(TEXT("camera"), CameraToJson(State.Camera));
		Obj->SetObjectField(TEXT("capabilities"), CapabilitiesToJson(State.Capabilities));
		Obj->SetObjectField(TEXT("visual_options"), State.VisualOptions.IsValid() ? State.VisualOptions : MakeShared<FJsonObject>());
		Obj->SetObjectField(TEXT("preview_state"), State.PreviewState.IsValid() ? State.PreviewState : MakeShared<FJsonObject>());
		return Obj;
	}

	TSharedPtr<FJsonObject> ScreenshotResultToJson(const FViewportScreenshotResult& Result)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("schema"), SchemaScreenshot);
		Obj->SetStringField(TEXT("file_path"), Result.FilePath);
		Obj->SetStringField(TEXT("format"), Result.Format);
		Obj->SetStringField(TEXT("capture_backend"), Result.CaptureBackend);
		Obj->SetNumberField(TEXT("width"), Result.Width);
		Obj->SetNumberField(TEXT("height"), Result.Height);
		Obj->SetNumberField(TEXT("bytes"), static_cast<double>(Result.Bytes));
		Obj->SetObjectField(TEXT("pixel_signal"), Result.PixelSignal.IsValid() ? Result.PixelSignal : MakeShared<FJsonObject>());
		if (Result.Capture.IsValid())
		{
			Obj->SetObjectField(TEXT("capture"), Result.Capture);
		}
		return Obj;
	}
}

// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace UeAgentEditorViewport
{
	extern const TCHAR* const SchemaState;
	extern const TCHAR* const SchemaTarget;
	extern const TCHAR* const SchemaScreenshot;

	struct FViewportTarget
	{
		FViewportTarget();

		FString Scope;
		FString AssetPath;
		FString AssetType;
		FString ViewportRole;
		FString Viewport;
		FString ViewportId;
		bool bOpenEditorIfNeeded;
	};

	struct FViewportCamera
	{
		FViewportCamera();

		FVector Location;
		FRotator Rotation;
		float Fov;
		FString Projection;
		FString Space;
		bool bValid;
	};

	struct FViewportCapabilities
	{
		FViewportCapabilities();

		bool bCamera;
		bool bVisualOptions;
		bool bPreviewState;
		bool bScreenshot;
		TArray<FString> FocusModes;
		TArray<FString> ScreenshotBackends;
		TArray<FString> VisualOptionKeys;
		TArray<FString> PreviewStateKeys;
		TArray<FString> UnsupportedOptions;
	};

	struct FViewportScreenshotResult
	{
		FViewportScreenshotResult();

		FString FilePath;
		FString Format;
		FString CaptureBackend;
		int32 Width;
		int32 Height;
		int64 Bytes;
		TSharedPtr<FJsonObject> PixelSignal;
		TSharedPtr<FJsonObject> Capture;
	};

	struct FViewportState
	{
		FViewportTarget Target;
		FString ClientClass;
		FString Adapter;
		FViewportCamera Camera;
		FViewportCapabilities Capabilities;
		TSharedPtr<FJsonObject> VisualOptions;
		TSharedPtr<FJsonObject> PreviewState;
	};

	struct FViewportSnapshot
	{
		FViewportSnapshot();

		FViewportState State;
		bool bValid;
	};

	TSharedPtr<FJsonObject> MakeVectorJson(const FVector& Value);
	TSharedPtr<FJsonObject> MakeRotatorJson(const FRotator& Value);
	TArray<TSharedPtr<FJsonValue>> MakeStringArrayJson(const TArray<FString>& Values);
	TSharedPtr<FJsonObject> TargetToJson(const FViewportTarget& Target);
	TSharedPtr<FJsonObject> CameraToJson(const FViewportCamera& Camera);
	TSharedPtr<FJsonObject> CapabilitiesToJson(const FViewportCapabilities& Capabilities);
	TSharedPtr<FJsonObject> StateToJson(const FViewportState& State);
	TSharedPtr<FJsonObject> ScreenshotResultToJson(const FViewportScreenshotResult& Result);
}

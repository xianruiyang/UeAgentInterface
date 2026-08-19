// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Commands/EditorViewport/UeAgentEditorViewportResolver.h"
#include "ShowFlags.h"

class FEditorViewportClient;
class FLevelEditorViewportClient;
class FUeAgentHttpServer;
class SWidget;

namespace UeAgentEditorViewport
{
	bool TryGetObjectField(const TSharedPtr<FJsonObject>& Obj, const FString& Key, TSharedPtr<FJsonObject>& OutObj);
	bool TryGetStringField(const TSharedPtr<FJsonObject>& Obj, const FString& Key, FString& OutValue);
	bool TryGetNumberField(const TSharedPtr<FJsonObject>& Obj, const FString& Key, double& OutValue);
	bool TryGetBoolField(const TSharedPtr<FJsonObject>& Obj, const FString& Key, bool& OutValue);
	void CopyJsonFieldIfPresent(const TSharedPtr<FJsonObject>& Source, const FString& Key, const TSharedPtr<FJsonObject>& Dest);
	bool TryReadVectorField(const TSharedPtr<FJsonObject>& Obj, const FString& Key, FVector& OutValue);
	bool TryReadRotatorField(const TSharedPtr<FJsonObject>& Obj, const FString& Key, FRotator& OutValue);
	bool HasUserFields(const TSharedPtr<FJsonObject>& Obj);
	bool FindWidgetByTypeRecursive(const TSharedRef<SWidget>& RootWidget, const FName& TypeName, TSharedPtr<SWidget>& OutWidget);
	FString NormalizeToken(FString Value);
	FString ViewModeToString(EViewModeIndex ViewMode);
	bool TryParseViewMode(const FString& InValue, EViewModeIndex& OutViewMode);
	FViewportTarget ParseTarget(const TSharedPtr<FJsonObject>& Params);
	TSharedPtr<FJsonObject> MakeLevelVisualOptionsJson(FLevelEditorViewportClient* Client);
	bool TryReadBoolOption(const TSharedPtr<FJsonObject>& Options, const FString& Key, bool& OutValue);
	bool ParseCameraRequest(const TSharedPtr<FJsonObject>& Params, const FViewportCamera& CurrentCamera, FViewportCamera& OutCamera, FString& OutError);

	TSharedRef<IViewportAdapter> MakePersonaViewportAdapter();
	TSharedRef<IViewportAdapter> MakeControlRigPersonaViewportAdapter();
	TSharedRef<IViewportAdapter> MakeLevelViewportAdapter(const FUeAgentHttpServer& Server);
}

// Copyright Epic Games, Inc. All Rights Reserved.

#include "Commands/EditorViewport/UeAgentEditorViewportLegacy.h"

namespace UeAgentEditorViewportLegacy
{
	namespace
	{
		static bool TryGetObjectField(const TSharedPtr<FJsonObject>& Obj, const FString& Key, TSharedPtr<FJsonObject>& OutObj)
		{
			const TSharedPtr<FJsonObject>* Found = nullptr;
			if (Obj.IsValid() && Obj->TryGetObjectField(Key, Found) && Found && Found->IsValid())
			{
				OutObj = *Found;
				return true;
			}
			return false;
		}

		static void CopyFieldIfPresent(const TSharedPtr<FJsonObject>& Source, const FString& Key, const TSharedPtr<FJsonObject>& Dest)
		{
			if (!Source.IsValid() || !Dest.IsValid())
			{
				return;
			}
			const TSharedPtr<FJsonValue>* Value = Source->Values.Find(Key);
			if (Value && Value->IsValid())
			{
				Dest->Values.Add(Key, *Value);
			}
		}

		static void CopyMissingFields(const TSharedPtr<FJsonObject>& Source, const TSharedPtr<FJsonObject>& Dest)
		{
			if (!Source.IsValid() || !Dest.IsValid())
			{
				return;
			}
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Source->Values)
			{
				if (!Dest->HasField(Pair.Key))
				{
					Dest->Values.Add(Pair.Key, Pair.Value);
				}
			}
		}

		static TSharedPtr<FJsonObject> MakeProxyParams(
			const TSharedPtr<FJsonObject>& SourceParams,
			const TSharedPtr<FJsonObject>& Target,
			const TSharedPtr<FJsonObject>& Focus)
		{
			TSharedPtr<FJsonObject> Params = MakeParamsWithTarget(SourceParams, Target);
			if (Focus.IsValid())
			{
				Params->SetObjectField(TEXT("focus"), Focus);
			}
			return Params;
		}

		static bool ExecuteCompatCommand(
			const FUeAgentHttpServer& Server,
			const FUeAgentRequestContext& Ctx,
			const FString& ReplacementCommand,
			const TSharedPtr<FJsonObject>& Params,
			TSharedPtr<FJsonObject>& OutData,
			FString& OutError)
		{
			FUeAgentRequestContext ProxyCtx = Ctx;
			ProxyCtx.Command = ReplacementCommand;
			ProxyCtx.Params = Params;
			return Server.ExecuteEditorViewportCommand(ReplacementCommand, ProxyCtx, OutData, OutError);
		}

		static TSharedPtr<FJsonObject> FindCameraObject(const TSharedPtr<FJsonObject>& Data)
		{
			TSharedPtr<FJsonObject> Camera;
			if (TryGetObjectField(Data, TEXT("camera"), Camera))
			{
				return Camera;
			}
			if (TryGetObjectField(Data, TEXT("camera_after_apply"), Camera))
			{
				return Camera;
			}
			return nullptr;
		}
	}

	TSharedPtr<FJsonObject> MakeLevelTarget()
	{
		TSharedPtr<FJsonObject> Target = MakeShared<FJsonObject>();
		Target->SetStringField(TEXT("scope"), TEXT("level"));
		Target->SetStringField(TEXT("viewport"), TEXT("active"));
		Target->SetStringField(TEXT("viewport_role"), TEXT("primary"));
		return Target;
	}

	TSharedPtr<FJsonObject> MakeAssetTarget(const FString& AssetPath, const FString& AssetType, const FString& ViewportRole, bool bOpenEditorIfNeeded)
	{
		TSharedPtr<FJsonObject> Target = MakeShared<FJsonObject>();
		Target->SetStringField(TEXT("scope"), TEXT("asset_editor"));
		Target->SetStringField(TEXT("asset_path"), AssetPath);
		Target->SetStringField(TEXT("asset_type"), AssetType);
		Target->SetStringField(TEXT("viewport_role"), ViewportRole.IsEmpty() ? TEXT("primary") : ViewportRole);
		Target->SetStringField(TEXT("viewport"), TEXT("active"));
		Target->SetBoolField(TEXT("open_editor_if_needed"), bOpenEditorIfNeeded);
		return Target;
	}

	TSharedPtr<FJsonObject> MakeParamsWithTarget(const TSharedPtr<FJsonObject>& SourceParams, const TSharedPtr<FJsonObject>& Target)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		if (SourceParams.IsValid())
		{
			Params->Values = SourceParams->Values;
		}
		if (Target.IsValid())
		{
			Params->SetObjectField(TEXT("target"), Target);
		}
		return Params;
	}

	void AddCompatMetadata(const TSharedPtr<FJsonObject>& Data, const FString& LegacyCommand, const FString& ReplacementCommand)
	{
		if (!Data.IsValid())
		{
			return;
		}
		Data->SetBoolField(TEXT("compat_wrapper"), true);
		Data->SetStringField(TEXT("legacy_command"), LegacyCommand);
		Data->SetStringField(TEXT("replacement_command"), ReplacementCommand);
		Data->SetBoolField(TEXT("deprecated_for_new_authoring"), true);
	}

	void AddLegacyCameraFields(const TSharedPtr<FJsonObject>& Data)
	{
		if (!Data.IsValid())
		{
			return;
		}
		TSharedPtr<FJsonObject> Camera = FindCameraObject(Data);
		if (Camera.IsValid())
		{
			CopyFieldIfPresent(Camera, TEXT("location"), Data);
			CopyFieldIfPresent(Camera, TEXT("rotation"), Data);
			CopyFieldIfPresent(Camera, TEXT("fov"), Data);
			CopyFieldIfPresent(Camera, TEXT("projection"), Data);
		}

		TSharedPtr<FJsonObject> VisualOptions;
		if (TryGetObjectField(Data, TEXT("visual_options"), VisualOptions))
		{
			CopyFieldIfPresent(VisualOptions, TEXT("realtime"), Data);
			CopyFieldIfPresent(VisualOptions, TEXT("game_view"), Data);
		}

		TSharedPtr<FJsonObject> Target;
		if (TryGetObjectField(Data, TEXT("target"), Target))
		{
			CopyFieldIfPresent(Target, TEXT("asset_path"), Data);
			if (!Data->HasField(TEXT("viewport")))
			{
				TSharedPtr<FJsonObject> Viewport = MakeShared<FJsonObject>();
				Viewport->SetBoolField(TEXT("valid"), true);
				CopyFieldIfPresent(Data, TEXT("adapter"), Viewport);
				CopyFieldIfPresent(Target, TEXT("viewport_id"), Viewport);
				CopyFieldIfPresent(Target, TEXT("viewport"), Viewport);
				CopyFieldIfPresent(Target, TEXT("viewport_role"), Viewport);
				Data->SetObjectField(TEXT("viewport"), Viewport);
			}
		}
	}

	void MergeFocusResultIntoLegacyFields(const TSharedPtr<FJsonObject>& Data)
	{
		TSharedPtr<FJsonObject> FocusResult;
		if (!TryGetObjectField(Data, TEXT("focus_result"), FocusResult))
		{
			return;
		}
		CopyMissingFields(FocusResult, Data);

		TSharedPtr<FJsonObject> Actor;
		if (TryGetObjectField(FocusResult, TEXT("actor"), Actor))
		{
			FString ActorName;
			if (Actor->TryGetStringField(TEXT("name"), ActorName) && !Data->HasField(TEXT("focused_name")))
			{
				Data->SetStringField(TEXT("focused_name"), ActorName);
			}
			FString ActorLabel;
			if (Actor->TryGetStringField(TEXT("label"), ActorLabel) && !Data->HasField(TEXT("focused_label")))
			{
				Data->SetStringField(TEXT("focused_label"), ActorLabel);
			}
		}
	}

	bool ExecuteCameraGet(
		const FUeAgentHttpServer& Server,
		const FUeAgentRequestContext& Ctx,
		const TSharedPtr<FJsonObject>& Target,
		TSharedPtr<FJsonObject>& OutData,
		FString& OutError)
	{
		const FString Replacement = TEXT("editor_viewport_get_state");
		if (!ExecuteCompatCommand(Server, Ctx, Replacement, MakeParamsWithTarget(Ctx.Params, Target), OutData, OutError))
		{
			return false;
		}
		AddCompatMetadata(OutData, Ctx.Command, Replacement);
		AddLegacyCameraFields(OutData);
		return true;
	}

	bool ExecuteCameraSet(
		const FUeAgentHttpServer& Server,
		const FUeAgentRequestContext& Ctx,
		const TSharedPtr<FJsonObject>& Target,
		TSharedPtr<FJsonObject>& OutData,
		FString& OutError)
	{
		const FString Replacement = TEXT("editor_viewport_set_camera");
		if (!ExecuteCompatCommand(Server, Ctx, Replacement, MakeParamsWithTarget(Ctx.Params, Target), OutData, OutError))
		{
			return false;
		}
		AddCompatMetadata(OutData, Ctx.Command, Replacement);
		AddLegacyCameraFields(OutData);
		return true;
	}

	bool ExecuteFocus(
		const FUeAgentHttpServer& Server,
		const FUeAgentRequestContext& Ctx,
		const TSharedPtr<FJsonObject>& Target,
		const TSharedPtr<FJsonObject>& Focus,
		TSharedPtr<FJsonObject>& OutData,
		FString& OutError)
	{
		const FString Replacement = TEXT("editor_viewport_focus");
		if (!ExecuteCompatCommand(Server, Ctx, Replacement, MakeProxyParams(Ctx.Params, Target, Focus), OutData, OutError))
		{
			return false;
		}
		AddCompatMetadata(OutData, Ctx.Command, Replacement);
		AddLegacyCameraFields(OutData);
		MergeFocusResultIntoLegacyFields(OutData);
		return true;
	}
}

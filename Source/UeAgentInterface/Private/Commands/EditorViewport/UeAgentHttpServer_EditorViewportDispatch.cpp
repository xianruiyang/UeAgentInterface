// Copyright Epic Games, Inc. All Rights Reserved.

#include "UeAgentHttpServer_EditorViewport.h"

#include "Commands/EditorViewport/UeAgentEditorViewportBlueprint.h"
#include "Commands/EditorViewport/UeAgentEditorViewportBoundaries.h"
#include "Commands/EditorViewport/UeAgentEditorViewportGroom.h"
#include "Commands/EditorViewport/UeAgentEditorViewportIK.h"
#include "Commands/EditorViewport/UeAgentEditorViewportMaterial.h"
#include "Commands/EditorViewport/UeAgentEditorViewportNiagara.h"
#include "Commands/EditorViewport/UeAgentEditorViewportPhysicsAsset.h"
#include "Commands/EditorViewport/UeAgentEditorViewportSequence.h"
#include "Commands/EditorViewport/UeAgentEditorViewportStaticMesh.h"
#include "Commands/EditorViewport/UeAgentHttpServer_EditorViewportInternal.h"

#include "Editor.h"
#include "LevelEditorViewport.h"
#include "Subsystems/AssetEditorSubsystem.h"

namespace UeAgentEditorViewport
{
	void MakeAdapters(const FUeAgentHttpServer& Server, TArray<TSharedRef<IViewportAdapter>>& OutAdapters)
	{
		OutAdapters.Add(MakeLevelViewportAdapter(Server));
		OutAdapters.Add(MakeStaticMeshViewportAdapter());
		OutAdapters.Add(MakeIKViewportAdapter());
		OutAdapters.Add(MakePhysicsAssetViewportAdapter());
		OutAdapters.Add(MakeGroomViewportAdapter());
		OutAdapters.Add(MakeLevelSequenceViewportAdapter());
		OutAdapters.Add(MakeControlRigPersonaViewportAdapter());
		OutAdapters.Add(MakePersonaViewportAdapter());
		OutAdapters.Add(MakeBlueprintSCSViewportAdapter());
		OutAdapters.Add(MakeNiagaraViewportAdapter());
		OutAdapters.Add(MakeMaterialViewportAdapter());
	}

	bool ResolveAdapter(const FUeAgentHttpServer& Server, const TSharedPtr<FJsonObject>& Params, TSharedRef<IViewportAdapter>& OutAdapter, FResolvedViewport& OutViewport, FString& OutError)
	{
		const FViewportTarget Target = ParseTarget(Params);
		FResolveRequest Request;
		Request.Target = Target;
		Request.Params = Params;

		FSpecialPreviewBoundary RoleBoundary;
		if (Target.Scope == TEXT("asset_editor") && TryGetExcludedEditorWindowBoundaryForViewportRole(Target.ViewportRole, Target.AssetType, RoleBoundary))
		{
			OutError = RoleBoundary.ErrorCode;
			return false;
		}

		TArray<TSharedRef<IViewportAdapter>> Adapters;
		MakeAdapters(Server, Adapters);
		TArray<FString> CandidateNames;
		for (const TSharedRef<IViewportAdapter>& Adapter : Adapters)
		{
			if (!Adapter->CanResolve(Request))
			{
				continue;
			}
			CandidateNames.Add(Adapter->GetName());
			FString ResolveError;
			if (Adapter->Resolve(Request, OutViewport, ResolveError))
			{
				OutAdapter = Adapter;
				return true;
			}
			if (OutError.IsEmpty())
			{
				OutError = ResolveError;
			}
		}
		FSpecialPreviewBoundary Boundary;
		if (Target.Scope == TEXT("asset_editor") && TryGetEditorViewportBoundaryForAssetType(Target.AssetType, Boundary))
		{
			OutError = Boundary.ErrorCode;
		}
		else if (CandidateNames.Num() > 1)
		{
			OutError = TEXT("ambiguous_viewport_target");
		}
		else if (CandidateNames.Num() == 0)
		{
			OutError = Target.Scope == TEXT("asset_editor") ? TEXT("unsupported_asset_editor_viewport") : TEXT("unsupported_editor_viewport_target");
		}
		return false;
	}

	TSharedPtr<FJsonObject> MakeUnsupportedAssetCandidate(UObject* Asset)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		FViewportTarget Target;
		Target.Scope = TEXT("asset_editor");
		Target.AssetPath = Asset ? Asset->GetPathName() : FString();
		Target.AssetType = Asset && Asset->GetClass() ? NormalizeToken(Asset->GetClass()->GetName()) : FString();
		Target.ViewportRole = TEXT("primary");
		Target.ViewportId = FString::Printf(TEXT("asset:%s#primary"), *Target.AssetPath);
		Obj->SetStringField(TEXT("viewport_id"), Target.ViewportId);
		Obj->SetObjectField(TEXT("target"), TargetToJson(Target));
		Obj->SetStringField(TEXT("adapter"), TEXT(""));
		Obj->SetStringField(TEXT("status"), TEXT("unsupported_asset_editor_viewport"));
		Obj->SetObjectField(TEXT("capabilities"), CapabilitiesToJson(FViewportCapabilities()));
		FSpecialPreviewBoundary Boundary;
		if (TryGetEditorViewportBoundaryForAssetType(Target.AssetType, Boundary))
		{
			ApplyEditorViewportBoundaryToCandidate(Obj, Boundary);
		}
		return Obj;
	}

	bool TryMakeSupportedAssetCandidate(UObject* Asset, const TArray<TSharedRef<IViewportAdapter>>& Adapters, TSharedPtr<FJsonObject>& OutCandidate)
	{
		if (!Asset)
		{
			return false;
		}

		FResolveRequest Request;
		Request.Target.Scope = TEXT("asset_editor");
		Request.Target.AssetPath = Asset->GetPathName();
		Request.Target.AssetType = NormalizeToken(Asset->GetClass() ? Asset->GetClass()->GetName() : FString());
		Request.Target.ViewportRole = TEXT("primary");
		Request.Target.Viewport = TEXT("active");
		Request.Target.bOpenEditorIfNeeded = false;

		for (const TSharedRef<IViewportAdapter>& Adapter : Adapters)
		{
			if (!Adapter->CanResolve(Request))
			{
				continue;
			}

			FResolvedViewport Resolved;
			FString ResolveError;
			if (!Adapter->Resolve(Request, Resolved, ResolveError))
			{
				continue;
			}

			FViewportState State;
			FString StateError;
			if (!Adapter->ReadState(Resolved, State, StateError))
			{
				continue;
			}

			OutCandidate = StateToJson(State);
			OutCandidate->SetStringField(TEXT("viewport_id"), Resolved.Target.ViewportId);
			OutCandidate->SetStringField(TEXT("status"), TEXT("supported"));
			return true;
		}

		return false;
	}
}

bool FUeAgentHttpServer::ExecuteEditorViewportCommand(const FString& CommandLower, const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentEditorViewport;

	auto ReadResolvedState = [](const TSharedRef<IViewportAdapter>& Adapter, const FResolvedViewport& Viewport, TSharedPtr<FJsonObject>& OutStateData, FString& Error) -> bool
	{
		FViewportState State;
		if (!Adapter->ReadState(Viewport, State, Error))
		{
			return false;
		}
		OutStateData = StateToJson(State);
		return true;
	};

	if (CommandLower == TEXT("editor_viewport_list"))
	{
		OutData = MakeShared<FJsonObject>();
		OutData->SetStringField(TEXT("schema"), TEXT("uai.editor_viewport.list.v1"));
		TArray<TSharedPtr<FJsonValue>> Viewports;

		TArray<TSharedRef<IViewportAdapter>> Adapters;
		MakeAdapters(*this, Adapters);
		if (GEditor)
		{
			int32 Index = 0;
			for (FLevelEditorViewportClient* Client : GEditor->GetLevelViewportClients())
			{
				if (!Client || !Client->Viewport)
				{
					++Index;
					continue;
				}
				FResolvedViewport Resolved;
				Resolved.Target.Scope = TEXT("level");
				Resolved.Target.Viewport = Index == 0 ? TEXT("active") : FString::Printf(TEXT("index_%d"), Index);
				Resolved.Target.ViewportRole = TEXT("primary");
				Resolved.Target.ViewportId = FString::Printf(TEXT("level:%d"), Index);
				Resolved.AdapterName = TEXT("level");
				Resolved.ClientClass = TEXT("FLevelEditorViewportClient");
				Resolved.ViewportClient = Client;

				FViewportState State;
				FString StateError;
				if (Adapters[0]->ReadState(Resolved, State, StateError))
				{
					TSharedPtr<FJsonObject> Item = StateToJson(State);
					Item->SetStringField(TEXT("viewport_id"), Resolved.Target.ViewportId);
					Viewports.Add(MakeShared<FJsonValueObject>(Item));
				}
				++Index;
			}

			bool bIncludeOpenEditors = true;
			TryGetBoolField(Ctx.Params, TEXT("include_open_editors"), bIncludeOpenEditors);
			if (bIncludeOpenEditors)
			{
				if (UAssetEditorSubsystem* AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
				{
					for (UObject* Asset : AssetEditorSubsystem->GetAllEditedAssets())
					{
						TSharedPtr<FJsonObject> Candidate;
						if (!TryMakeSupportedAssetCandidate(Asset, Adapters, Candidate))
						{
							Candidate = MakeUnsupportedAssetCandidate(Asset);
						}
						Viewports.Add(MakeShared<FJsonValueObject>(Candidate));
					}
				}
			}
		}

		OutData->SetArrayField(TEXT("viewports"), Viewports);
		OutData->SetNumberField(TEXT("viewport_count"), Viewports.Num());
		return true;
	}

	TSharedRef<IViewportAdapter> Adapter = MakeLevelViewportAdapter(*this);
	FResolvedViewport Viewport;
	if (!ResolveAdapter(*this, Ctx.Params, Adapter, Viewport, OutError))
	{
		return false;
	}

	if (CommandLower == TEXT("editor_viewport_get_state"))
	{
		return ReadResolvedState(Adapter, Viewport, OutData, OutError);
	}

	if (CommandLower == TEXT("editor_viewport_set_camera"))
	{
		FViewportState CurrentState;
		if (!Adapter->ReadState(Viewport, CurrentState, OutError))
		{
			return false;
		}
		FViewportCamera Camera;
		if (!ParseCameraRequest(Ctx.Params, CurrentState.Camera, Camera, OutError))
		{
			return false;
		}
		if (!Adapter->WriteCamera(Viewport, Camera, OutError))
		{
			return false;
		}
		return ReadResolvedState(Adapter, Viewport, OutData, OutError);
	}

	if (CommandLower == TEXT("editor_viewport_focus"))
	{
		TSharedPtr<FJsonObject> FocusObj;
		if (!TryGetObjectField(Ctx.Params, TEXT("focus"), FocusObj))
		{
			FocusObj = Ctx.Params;
		}
		TSharedPtr<FJsonObject> FocusData;
		if (!Adapter->Focus(Viewport, FocusObj, FocusData, OutError))
		{
			return false;
		}
		if (!ReadResolvedState(Adapter, Viewport, OutData, OutError))
		{
			return false;
		}
		OutData->SetObjectField(TEXT("focus_result"), FocusData.IsValid() ? FocusData : MakeShared<FJsonObject>());
		return true;
	}

	if (CommandLower == TEXT("editor_viewport_set_visual_options"))
	{
		TSharedPtr<FJsonObject> Options;
		if (!TryGetObjectField(Ctx.Params, TEXT("visual_options"), Options))
		{
			Options = Ctx.Params;
		}
		TSharedPtr<FJsonObject> Applied;
		if (!Adapter->ApplyVisualOptions(Viewport, Options, Applied, OutError))
		{
			return false;
		}
		if (!ReadResolvedState(Adapter, Viewport, OutData, OutError))
		{
			return false;
		}
		OutData->SetObjectField(TEXT("visual_options_applied"), Applied.IsValid() ? Applied : MakeShared<FJsonObject>());
		return true;
	}

	if (CommandLower == TEXT("editor_viewport_set_preview_state"))
	{
		TSharedPtr<FJsonObject> PreviewState;
		if (!TryGetObjectField(Ctx.Params, TEXT("preview_state"), PreviewState))
		{
			PreviewState = Ctx.Params;
		}
		TSharedPtr<FJsonObject> Applied;
		if (!Adapter->ApplyPreviewState(Viewport, PreviewState, Applied, OutError))
		{
			return false;
		}
		if (!ReadResolvedState(Adapter, Viewport, OutData, OutError))
		{
			return false;
		}
		OutData->SetObjectField(TEXT("preview_state_applied"), Applied.IsValid() ? Applied : MakeShared<FJsonObject>());
		return true;
	}

	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("domain"), TEXT("editor_viewport"));
	OutData->SetStringField(TEXT("command"), CommandLower);
	OutError = TEXT("unsupported_editor_viewport_command");
	return false;
}

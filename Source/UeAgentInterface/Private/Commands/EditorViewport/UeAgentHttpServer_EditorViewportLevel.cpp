// Copyright Epic Games, Inc. All Rights Reserved.

#include "Commands/EditorViewport/UeAgentHttpServer_EditorViewportInternal.h"

#include "Server/UeAgentHttpServer.h"
#include "UeAgentInterfaceSettings.h"

#include "Editor.h"
#include "EditorViewportClient.h"
#include "HAL/FileManager.h"
#include "LevelEditorViewport.h"

namespace UeAgentEditorViewport
{
	class FLevelViewportAdapter final : public IViewportAdapter
	{
	public:
		explicit FLevelViewportAdapter(const FUeAgentHttpServer& InServer)
			: Server(InServer)
		{
		}

		virtual FString GetName() const override
		{
			return TEXT("level");
		}

		virtual bool CanResolve(const FResolveRequest& Request) const override
		{
			return Request.Target.Scope.IsEmpty() || Request.Target.Scope == TEXT("level");
		}

		virtual bool Resolve(const FResolveRequest& Request, FResolvedViewport& OutViewport, FString& OutError) const override
		{
			FString SelectionReason;
			FLevelEditorViewportClient* Client = GetRequestedOrPreferredClient(Request.Target, &SelectionReason);
			if (!Client)
			{
				OutError = TEXT("no_level_editor_viewport");
				return false;
			}

			OutViewport.Target = Request.Target;
			OutViewport.Target.Scope = TEXT("level");
			const int32 ClientIndex = GetClientIndex(Client);
			OutViewport.Target.Viewport = OutViewport.Target.Viewport.IsEmpty() ? TEXT("auto") : OutViewport.Target.Viewport;
			OutViewport.Target.ViewportId = OutViewport.Target.ViewportId.IsEmpty()
				? (ClientIndex >= 0 ? FString::Printf(TEXT("level:%d"), ClientIndex) : TEXT("level:auto"))
				: OutViewport.Target.ViewportId;
			OutViewport.Target.ViewportRole = TEXT("primary");
			OutViewport.AdapterName = GetName();
			OutViewport.ClientClass = TEXT("FLevelEditorViewportClient");
			OutViewport.ViewportClient = Client;
			return true;
		}

		virtual bool ReadState(const FResolvedViewport& Viewport, FViewportState& OutState, FString& OutError) const override
		{
			FLevelEditorViewportClient* Client = AsLevelClient(Viewport);
			if (!Client)
			{
				OutError = TEXT("invalid_level_viewport");
				return false;
			}

			OutState.Target = Viewport.Target;
			OutState.ClientClass = TEXT("FLevelEditorViewportClient");
			OutState.Adapter = GetName();
			OutState.Camera.Location = Client->GetViewLocation();
			OutState.Camera.Rotation = Client->GetViewRotation();
			OutState.Camera.Fov = Client->ViewFOV;
			OutState.Camera.Space = TEXT("editor_world");
			OutState.Camera.Projection = Client->ViewportType == LVT_Perspective ? TEXT("perspective") : TEXT("orthographic");
			OutState.Camera.bValid = true;
			OutState.Capabilities.bCamera = true;
			OutState.Capabilities.bVisualOptions = true;
			OutState.Capabilities.bPreviewState = false;
			OutState.Capabilities.bScreenshot = true;
			OutState.Capabilities.FocusModes = { TEXT("selection"), TEXT("actor"), TEXT("actors"), TEXT("custom_bounds"), TEXT("custom_point") }; OutState.Capabilities.ScreenshotBackends = { TEXT("level_viewport_backbuffer") }; OutState.Capabilities.VisualOptionKeys = { TEXT("view_mode"), TEXT("grid"), TEXT("bounds"), TEXT("pivot"), TEXT("selection"), TEXT("mode_widgets"), TEXT("collision") };
			OutState.VisualOptions = MakeLevelVisualOptionsJson(Client);
			OutState.PreviewState = MakeShared<FJsonObject>();
			return true;
		}

		virtual bool WriteCamera(const FResolvedViewport& Viewport, const FViewportCamera& Camera, FString& OutError) const override
		{
			FLevelEditorViewportClient* Client = AsLevelClient(Viewport);
			if (!Client)
			{
				OutError = TEXT("invalid_level_viewport");
				return false;
			}
			Client->SetViewLocation(Camera.Location);
			Client->SetViewRotation(Camera.Rotation);
			Client->ViewFOV = FMath::Clamp(Camera.Fov, 5.0f, 170.0f);
			Client->Invalidate();
			if (Client->Viewport)
			{
				Client->Viewport->Invalidate();
			}
			return true;
		}

		virtual bool Focus(const FResolvedViewport& Viewport, const TSharedPtr<FJsonObject>& FocusObj, TSharedPtr<FJsonObject>& OutData, FString& OutError) const override
		{
			FLevelEditorViewportClient* Client = AsLevelClient(Viewport);
			if (!Client)
			{
				OutError = TEXT("invalid_level_viewport");
				return false;
			}
			if (!FocusObj.IsValid())
			{
				OutError = TEXT("missing_focus");
				return false;
			}

			FString Mode = TEXT("selection");
			TryGetStringField(FocusObj, TEXT("mode"), Mode);
			Mode = NormalizeToken(Mode);
			OutData = MakeShared<FJsonObject>();
			OutData->SetStringField(TEXT("mode"), Mode);

			if (Mode == TEXT("actor"))
			{
				FString Id;
				if (!TryGetStringField(FocusObj, TEXT("id"), Id)
					&& !TryGetStringField(FocusObj, TEXT("actor"), Id)
					&& !TryGetStringField(FocusObj, TEXT("actor_name"), Id)
					&& !TryGetStringField(FocusObj, TEXT("actor_label"), Id))
				{
					OutError = TEXT("missing_actor_id");
					return false;
				}

				FUeAgentRequestContext ProxyCtx;
				ProxyCtx.RequestId = TEXT("editor_viewport_focus_actor");
				ProxyCtx.Command = TEXT("viewport_frame_actor");
				ProxyCtx.Params = MakeShared<FJsonObject>();
				ProxyCtx.Params->SetStringField(TEXT("id"), Id);
				double Padding = 1.1;
				if (TryGetNumberField(FocusObj, TEXT("padding"), Padding))
				{
					ProxyCtx.Params->SetNumberField(TEXT("padding"), Padding);
				}
				bool bCollisionAware = false;
				if (TryGetBoolField(FocusObj, TEXT("collision_aware"), bCollisionAware))
				{
					ProxyCtx.Params->SetBoolField(TEXT("collision_aware"), bCollisionAware);
				}
				bool bLookAt = true;
				if (TryGetBoolField(FocusObj, TEXT("look_at"), bLookAt))
				{
					ProxyCtx.Params->SetBoolField(TEXT("look_at"), bLookAt);
				}
				CopyJsonFieldIfPresent(FocusObj, TEXT("auto_fallback"), ProxyCtx.Params);
				CopyJsonFieldIfPresent(FocusObj, TEXT("trace_channel"), ProxyCtx.Params);
				CopyJsonFieldIfPresent(FocusObj, TEXT("trace_complex"), ProxyCtx.Params);
				CopyJsonFieldIfPresent(FocusObj, TEXT("safety_offset_cm"), ProxyCtx.Params);
				CopyJsonFieldIfPresent(FocusObj, TEXT("fallback_step_cm"), ProxyCtx.Params);
				CopyJsonFieldIfPresent(FocusObj, TEXT("fallback_offsets_cm"), ProxyCtx.Params);
				return Server.CmdViewportFrameActorCore(ProxyCtx, OutData, OutError);
			}

			if (Mode == TEXT("selection"))
			{
				FUeAgentRequestContext ProxyCtx;
				ProxyCtx.RequestId = TEXT("editor_viewport_focus_selection");
				ProxyCtx.Command = TEXT("viewport_frame_selection");
				ProxyCtx.Params = MakeShared<FJsonObject>();
				double Padding = 1.1;
				if (TryGetNumberField(FocusObj, TEXT("padding"), Padding))
				{
					ProxyCtx.Params->SetNumberField(TEXT("padding"), Padding);
				}
				bool bInstant = true;
				if (TryGetBoolField(FocusObj, TEXT("instant"), bInstant))
				{
					ProxyCtx.Params->SetBoolField(TEXT("instant"), bInstant);
				}
				return Server.CmdViewportFrameSelectionCore(ProxyCtx, OutData, OutError);
			}

			if (Mode == TEXT("actors"))
			{
				FUeAgentRequestContext ProxyCtx;
				ProxyCtx.RequestId = TEXT("editor_viewport_focus_actors");
				ProxyCtx.Command = TEXT("viewport_frame_actors");
				ProxyCtx.Params = MakeShared<FJsonObject>();
				const TArray<TSharedPtr<FJsonValue>>* ActorIds = nullptr;
				if (!FocusObj->TryGetArrayField(TEXT("actor_ids"), ActorIds) || !ActorIds)
				{
					OutError = TEXT("missing_actor_ids");
					return false;
				}
				ProxyCtx.Params->SetArrayField(TEXT("actor_ids"), *ActorIds);
				double Padding = 1.1;
				if (TryGetNumberField(FocusObj, TEXT("padding"), Padding))
				{
					ProxyCtx.Params->SetNumberField(TEXT("padding"), Padding);
				}
				CopyJsonFieldIfPresent(FocusObj, TEXT("instant"), ProxyCtx.Params);
				CopyJsonFieldIfPresent(FocusObj, TEXT("collision_aware"), ProxyCtx.Params);
				CopyJsonFieldIfPresent(FocusObj, TEXT("auto_fallback"), ProxyCtx.Params);
				CopyJsonFieldIfPresent(FocusObj, TEXT("trace_channel"), ProxyCtx.Params);
				CopyJsonFieldIfPresent(FocusObj, TEXT("trace_complex"), ProxyCtx.Params);
				CopyJsonFieldIfPresent(FocusObj, TEXT("safety_offset_cm"), ProxyCtx.Params);
				CopyJsonFieldIfPresent(FocusObj, TEXT("fallback_step_cm"), ProxyCtx.Params);
				CopyJsonFieldIfPresent(FocusObj, TEXT("fallback_offsets_cm"), ProxyCtx.Params);
				return Server.CmdViewportFrameActorsCore(ProxyCtx, OutData, OutError);
			}

			if (Mode == TEXT("custom_bounds"))
			{
				FVector Min = FVector::ZeroVector;
				FVector Max = FVector::ZeroVector;
				if (!TryReadVectorField(FocusObj, TEXT("min"), Min) || !TryReadVectorField(FocusObj, TEXT("max"), Max))
				{
					TSharedPtr<FJsonObject> BoundsObj;
					if (!TryGetObjectField(FocusObj, TEXT("bounds"), BoundsObj)
						|| !TryReadVectorField(BoundsObj, TEXT("min"), Min)
						|| !TryReadVectorField(BoundsObj, TEXT("max"), Max))
					{
						OutError = TEXT("missing_custom_bounds");
						return false;
					}
				}
				const FBox Box(Min, Max);
				if (!Box.IsValid)
				{
					OutError = TEXT("invalid_custom_bounds");
					return false;
				}
				bool bInstant = true;
				TryGetBoolField(FocusObj, TEXT("instant"), bInstant);
				Client->FocusViewportOnBox(Box, bInstant);
				Client->Invalidate();
				TSharedPtr<FJsonObject> BoundsJson = MakeShared<FJsonObject>();
				BoundsJson->SetObjectField(TEXT("min"), MakeVectorJson(Box.Min));
				BoundsJson->SetObjectField(TEXT("max"), MakeVectorJson(Box.Max));
				BoundsJson->SetObjectField(TEXT("center"), MakeVectorJson(Box.GetCenter()));
				BoundsJson->SetObjectField(TEXT("extent"), MakeVectorJson(Box.GetExtent()));
				OutData->SetObjectField(TEXT("bounds"), BoundsJson);
				OutData->SetBoolField(TEXT("instant"), bInstant);
				return true;
			}

			if (Mode == TEXT("custom_point"))
			{
				FVector Point = FVector::ZeroVector;
				if (!TryReadVectorField(FocusObj, TEXT("point"), Point))
				{
					OutError = TEXT("missing_custom_point");
					return false;
				}
				double Radius = 50.0;
				TryGetNumberField(FocusObj, TEXT("radius"), Radius);
				const FBox Box(Point - FVector(Radius), Point + FVector(Radius));
				Client->FocusViewportOnBox(Box, true);
				Client->Invalidate();
				OutData->SetObjectField(TEXT("point"), MakeVectorJson(Point));
				OutData->SetNumberField(TEXT("radius"), Radius);
				return true;
			}

			OutError = TEXT("unsupported_focus_mode");
			return false;
		}

		virtual bool ApplyVisualOptions(const FResolvedViewport& Viewport, const TSharedPtr<FJsonObject>& Options, TSharedPtr<FJsonObject>& OutData, FString& OutError) const override
		{
			FLevelEditorViewportClient* Client = AsLevelClient(Viewport);
			if (!Client)
			{
				OutError = TEXT("invalid_level_viewport");
				return false;
			}
			if (!Options.IsValid())
			{
				OutError = TEXT("missing_visual_options");
				return false;
			}

			TArray<FString> Unsupported;
			FString ViewModeText;
			if (TryGetStringField(Options, TEXT("view_mode"), ViewModeText))
			{
				EViewModeIndex ViewMode = VMI_Lit;
				if (!TryParseViewMode(ViewModeText, ViewMode))
				{
					Unsupported.Add(TEXT("view_mode"));
				}
				else
				{
					Client->SetViewMode(ViewMode);
				}
			}

			auto ApplyBoolFlag = [Client, Options](const TCHAR* Key, auto Setter)
			{
				bool bValue = false;
				if (TryReadBoolOption(Options, Key, bValue))
				{
					Setter(bValue);
				}
			};

			ApplyBoolFlag(TEXT("grid"), [Client](bool bValue) { Client->EngineShowFlags.SetGrid(bValue); });
			ApplyBoolFlag(TEXT("bounds"), [Client](bool bValue) { Client->EngineShowFlags.SetBounds(bValue); });
			ApplyBoolFlag(TEXT("pivot"), [Client](bool bValue) { Client->EngineShowFlags.SetPivot(bValue); });
			ApplyBoolFlag(TEXT("selection"), [Client](bool bValue) { Client->EngineShowFlags.SetSelection(bValue); });
			ApplyBoolFlag(TEXT("mode_widgets"), [Client](bool bValue) { Client->EngineShowFlags.SetModeWidgets(bValue); });
			ApplyBoolFlag(TEXT("collision"), [Client](bool bValue) { Client->EngineShowFlags.SetCollision(bValue); });

			bool bRealtime = Client->IsRealtime();
			if (TryGetBoolField(Options, TEXT("realtime"), bRealtime))
			{
				Client->SetRealtime(bRealtime);
			}
			bool bGameView = Client->IsInGameView();
			if (TryGetBoolField(Options, TEXT("game_view"), bGameView))
			{
				Client->SetGameView(bGameView);
			}

			Client->Invalidate();
			if (Client->Viewport)
			{
				Client->Viewport->Invalidate();
			}

			OutData = MakeLevelVisualOptionsJson(Client);
			if (Unsupported.Num() > 0)
			{
				TArray<TSharedPtr<FJsonValue>> UnsupportedJson;
				for (const FString& Key : Unsupported)
				{
					UnsupportedJson.Add(MakeShared<FJsonValueString>(Key));
				}
				OutData->SetArrayField(TEXT("unsupported_options"), UnsupportedJson);
				bool bIgnoreUnsupported = false;
				TryGetBoolField(Options, TEXT("ignore_unsupported"), bIgnoreUnsupported);
				if (!bIgnoreUnsupported)
				{
					OutError = TEXT("unsupported_visual_options");
					return false;
				}
			}
			return true;
		}

		virtual bool ApplyPreviewState(const FResolvedViewport&, const TSharedPtr<FJsonObject>& PreviewState, TSharedPtr<FJsonObject>& OutData, FString& OutError) const override
		{
			OutData = MakeShared<FJsonObject>();
			if (!HasUserFields(PreviewState))
			{
				OutData->SetBoolField(TEXT("applied"), false);
				OutData->SetStringField(TEXT("reason"), TEXT("empty_preview_state"));
				return true;
			}
			OutError = TEXT("editor_viewport_preview_state_not_supported");
			return false;
		}

		virtual bool CaptureScreenshot(const FResolvedViewport& Viewport, const TSharedPtr<FJsonObject>& Params, FViewportScreenshotResult& OutResult, FString& OutError) const override
		{
			FLevelEditorViewportClient* Client = AsLevelClient(Viewport);
			if (!Client)
			{
				OutError = TEXT("invalid_level_viewport");
				return false;
			}
			const UUeAgentInterfaceSettings* Settings = UUeAgentInterfaceSettings::Get();
			if (!Settings || !Settings->bEnableScreenshots)
			{
				OutError = TEXT("screenshots_disabled");
				return false;
			}

			FResolvedScreenshotOptions Options;
			if (!ParseScreenshotOptions(Params, Options, OutError))
			{
				return false;
			}
			TArray<FColor> Pixels;
			FIntPoint Size(0, 0);
			TSharedPtr<FJsonObject> CaptureInfo;
			if (!CaptureEditorViewportClientBackbuffer(Client, GetName(), Options, Pixels, Size, CaptureInfo, OutError))
			{
				return false;
			}

			TArray<FColor> Resized;
			FIntPoint ResizedSize(0, 0);
			if (!FUeAgentHttpServer::ResizePixelsMaxSize(Pixels, Size, Options.MaxSize, Resized, ResizedSize, OutError))
			{
				return false;
			}
			if (!FUeAgentHttpServer::WriteCompressedImage(Resized, ResizedSize, Options.Format, Options.Quality, Options.OutputPath, OutError))
			{
				return false;
			}

			OutResult.FilePath = Options.OutputPath;
			OutResult.Format = Options.Format;
			OutResult.Width = ResizedSize.X;
			OutResult.Height = ResizedSize.Y;
			OutResult.Bytes = IFileManager::Get().FileSize(*Options.OutputPath);
			OutResult.CaptureBackend = TEXT("level_viewport_backbuffer");
			OutResult.PixelSignal = MakePixelSignalJson(Resized, ResizedSize);
			OutResult.Capture = CaptureInfo;
			return true;
		}

		virtual bool SnapshotState(const FResolvedViewport& Viewport, FViewportSnapshot& OutSnapshot, FString& OutError) const override
		{
			OutSnapshot = FViewportSnapshot();
			if (!ReadState(Viewport, OutSnapshot.State, OutError))
			{
				return false;
			}
			OutSnapshot.bValid = true;
			return true;
		}

		virtual bool RestoreState(const FResolvedViewport& Viewport, const FViewportSnapshot& Snapshot, FString& OutError) const override
		{
			if (!Snapshot.bValid)
			{
				OutError = TEXT("invalid_snapshot");
				return false;
			}
			if (!WriteCamera(Viewport, Snapshot.State.Camera, OutError))
			{
				return false;
			}
			TSharedPtr<FJsonObject> Applied;
			return ApplyVisualOptions(Viewport, Snapshot.State.VisualOptions, Applied, OutError);
		}

	private:
		const FUeAgentHttpServer& Server;

		static FLevelEditorViewportClient* AsLevelClient(const FResolvedViewport& Viewport)
		{
			return static_cast<FLevelEditorViewportClient*>(Viewport.ViewportClient);
		}

		static bool TryParseViewportIndex(const FString& Token, int32& OutIndex)
		{
			const FString Normalized = NormalizeToken(Token);
			if (Normalized.IsEmpty())
			{
				return false;
			}
			FString Tail = Normalized;
			if (Normalized.StartsWith(TEXT("index_")))
			{
				Tail = Normalized.RightChop(6);
			}
			else if (Normalized.StartsWith(TEXT("level:")))
			{
				Tail = Normalized.RightChop(6);
			}
			return LexTryParseString(OutIndex, *Tail) && OutIndex >= 0;
		}

		static int32 GetClientIndex(const FLevelEditorViewportClient* TargetClient)
		{
			if (!GEditor || !TargetClient)
			{
				return INDEX_NONE;
			}
			int32 Index = 0;
			for (FLevelEditorViewportClient* Client : GEditor->GetLevelViewportClients())
			{
				if (Client == TargetClient)
				{
					return Index;
				}
				++Index;
			}
			return INDEX_NONE;
		}

		static FLevelEditorViewportClient* GetClientByIndex(const int32 TargetIndex, FString* OutSelectionReason = nullptr)
		{
			if (!GEditor || TargetIndex < 0)
			{
				return nullptr;
			}
			int32 Index = 0;
			for (FLevelEditorViewportClient* Client : GEditor->GetLevelViewportClients())
			{
				if (Index == TargetIndex)
				{
					if (OutSelectionReason)
					{
						*OutSelectionReason = FString::Printf(TEXT("requested_level_viewport_%d"), Index);
					}
					return Client && Client->Viewport ? Client : nullptr;
				}
				++Index;
			}
			return nullptr;
		}

		static FLevelEditorViewportClient* GetActiveLevelClient(FString* OutSelectionReason = nullptr)
		{
			if (GEditor)
			{
				if (FViewport* ActiveViewport = GEditor->GetActiveViewport())
				{
					for (FLevelEditorViewportClient* Client : GEditor->GetLevelViewportClients())
					{
						if (Client && Client->Viewport == ActiveViewport)
						{
							if (OutSelectionReason)
							{
								*OutSelectionReason = TEXT("active_viewport");
							}
							return Client;
						}
					}
				}
			}
			return nullptr;
		}

		static FLevelEditorViewportClient* GetRequestedOrPreferredClient(const FViewportTarget& Target, FString* OutSelectionReason = nullptr)
		{
			int32 RequestedIndex = INDEX_NONE;
			if (TryParseViewportIndex(Target.ViewportId, RequestedIndex) || TryParseViewportIndex(Target.Viewport, RequestedIndex))
			{
				return GetClientByIndex(RequestedIndex, OutSelectionReason);
			}
			if (Target.Viewport == TEXT("active"))
			{
				return GetActiveLevelClient(OutSelectionReason);
			}
			return GetPreferredClient(OutSelectionReason);
		}

		static FLevelEditorViewportClient* GetPreferredClient(FString* OutSelectionReason = nullptr)
		{
			auto SetReason = [OutSelectionReason](const TCHAR* Reason)
			{
				if (OutSelectionReason)
				{
					*OutSelectionReason = Reason;
				}
			};

			if (GEditor)
			{
				if (FLevelEditorViewportClient* ActiveClient = GetActiveLevelClient(nullptr))
				{
					if (ActiveClient->ViewportType == LVT_Perspective)
					{
						SetReason(TEXT("active_perspective_viewport"));
						return ActiveClient;
					}
				}

				int32 Index = 0;
				for (FLevelEditorViewportClient* Client : GEditor->GetLevelViewportClients())
				{
					if (Client && Client->Viewport && Client->ViewportType == LVT_Perspective)
					{
						SetReason(*FString::Printf(TEXT("perspective_level_viewport_client_%d"), Index));
						return Client;
					}
					++Index;
				}

				if (FLevelEditorViewportClient* ActiveClient = GetActiveLevelClient(nullptr))
				{
					SetReason(TEXT("active_viewport"));
					return ActiveClient;
				}

				Index = 0;
				for (FLevelEditorViewportClient* Client : GEditor->GetLevelViewportClients())
				{
					if (Client && Client->Viewport)
					{
						SetReason(*FString::Printf(TEXT("level_viewport_client_%d"), Index));
						return Client;
					}
					++Index;
				}
			}

			if (GCurrentLevelEditingViewportClient && GCurrentLevelEditingViewportClient->Viewport)
			{
				SetReason(TEXT("current_level_editing_viewport"));
				return GCurrentLevelEditingViewportClient;
			}

			SetReason(TEXT("none"));
			return nullptr;
		}
	};

	TSharedRef<IViewportAdapter> MakeLevelViewportAdapter(const FUeAgentHttpServer& Server)
	{
		return MakeShared<FLevelViewportAdapter>(Server);
	}
}

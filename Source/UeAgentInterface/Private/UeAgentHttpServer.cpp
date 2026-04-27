// Copyright Epic Games, Inc. All Rights Reserved.

#include "UeAgentHttpServer.h"

#include "UeAgentJsonDiagnostics.h"
#include "UeAgentInterfaceLogger.h"
#include "UeAgentInterfaceSettings.h"

#include "Async/Async.h"
#include "HttpServerModule.h"
#include "IHttpRouter.h"
#include "IPAddress.h"
#include "Misc/Guid.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace UeAgentHttpServer
{
	static const FString ContentTypeJson = TEXT("application/json; charset=utf-8");
	static const FString HeaderTokenLower = TEXT("x-ueagentinterface-token");

	static const TCHAR* VerbToString(EHttpServerRequestVerbs Verb)
	{
		switch (Verb)
		{
		case EHttpServerRequestVerbs::VERB_GET: return TEXT("GET");
		case EHttpServerRequestVerbs::VERB_POST: return TEXT("POST");
		case EHttpServerRequestVerbs::VERB_PUT: return TEXT("PUT");
		case EHttpServerRequestVerbs::VERB_PATCH: return TEXT("PATCH");
		case EHttpServerRequestVerbs::VERB_DELETE: return TEXT("DELETE");
		case EHttpServerRequestVerbs::VERB_OPTIONS: return TEXT("OPTIONS");
		default: return TEXT("NONE");
		}
	}

	static EHttpServerResponseCodes ToResponseCode(int32 Code)
	{
		switch (Code)
		{
		case 200: return EHttpServerResponseCodes::Ok;
		case 204: return EHttpServerResponseCodes::NoContent;
		case 400: return EHttpServerResponseCodes::BadRequest;
		case 401: return EHttpServerResponseCodes::Denied;
		case 404: return EHttpServerResponseCodes::NotFound;
		case 408: return EHttpServerResponseCodes::RequestTimeout;
		default:  return EHttpServerResponseCodes::ServerError;
		}
	}

	static bool IsLoopbackPeer(const TSharedPtr<FInternetAddr>& Addr)
	{
		if (!Addr.IsValid() || !Addr->IsValid())
		{
			return false;
		}

		const TArray<uint8> Raw = Addr->GetRawIp();
		if (Raw.Num() == 4)
		{
			return Raw[0] == 127;
		}
		if (Raw.Num() == 16)
		{
			for (int32 i = 0; i < 15; i++)
			{
				if (Raw[i] != 0)
				{
					return false;
				}
			}
			return Raw[15] == 1;
		}
		return false;
	}

	static FString MaskToken(const FString& Token)
	{
		if (Token.Len() <= 8)
		{
			return TEXT("********");
		}
		return Token.Left(4) + TEXT("...") + Token.Right(4);
	}

	static FString GetHeaderFirst(const TMap<FString, TArray<FString>>& Headers, const FString& KeyLower)
	{
		for (const auto& Pair : Headers)
		{
			if (Pair.Key.ToLower() == KeyLower)
			{
				return Pair.Value.Num() > 0 ? Pair.Value[0] : FString();
			}
		}
		return FString();
	}

	static FString NowIso()
	{
		return FDateTime::Now().ToString(TEXT("%Y-%m-%dT%H:%M:%S.%s"));
	}
}

namespace
{
	using FExactCommandInvoker = bool (*)(const FUeAgentHttpServer*, const FUeAgentRequestContext&, TSharedPtr<FJsonObject>&, TArray<TSharedPtr<FJsonValue>>&, FString&);
	using FPrefixedCommandInvoker = bool (*)(const FUeAgentHttpServer*, const FString&, const FUeAgentRequestContext&, TSharedPtr<FJsonObject>&, FString&);
	using FExactNoCtxConstMethod = bool (FUeAgentHttpServer::*)(TSharedPtr<FJsonObject>&, FString&) const;
	using FExactWithCtxConstMethod = bool (FUeAgentHttpServer::*)(const FUeAgentRequestContext&, TSharedPtr<FJsonObject>&, FString&) const;
	using FExactWithCtxArtifactsConstMethod = bool (FUeAgentHttpServer::*)(const FUeAgentRequestContext&, TSharedPtr<FJsonObject>&, TArray<TSharedPtr<FJsonValue>>&, FString&) const;
	using FExactWithCtxMutableMethod = bool (FUeAgentHttpServer::*)(const FUeAgentRequestContext&, TSharedPtr<FJsonObject>&, FString&);
	using FPrefixedConstMethod = bool (FUeAgentHttpServer::*)(const FString&, const FUeAgentRequestContext&, TSharedPtr<FJsonObject>&, FString&) const;

	struct FExactCommandEntry
	{
		const TCHAR* Name;
		FExactCommandInvoker Invoker;
	};

	struct FPrefixedCommandEntry
	{
		const TCHAR* Prefix;
		FPrefixedCommandInvoker Invoker;
	};

	struct FUeAgentHttpServerCommandAccess
	{
		template<FExactNoCtxConstMethod Method>
		static bool InvokeExactNoCtx(const FUeAgentHttpServer* Server, const FUeAgentRequestContext&, TSharedPtr<FJsonObject>& OutDataRef, TArray<TSharedPtr<FJsonValue>>&, FString& OutErrRef)
		{
			return (Server->*Method)(OutDataRef, OutErrRef);
		}

		template<FExactWithCtxConstMethod Method>
		static bool InvokeExactWithCtx(const FUeAgentHttpServer* Server, const FUeAgentRequestContext& CommandCtx, TSharedPtr<FJsonObject>& OutDataRef, TArray<TSharedPtr<FJsonValue>>&, FString& OutErrRef)
		{
			return (Server->*Method)(CommandCtx, OutDataRef, OutErrRef);
		}

		template<FExactWithCtxArtifactsConstMethod Method>
		static bool InvokeExactWithCtxArtifacts(const FUeAgentHttpServer* Server, const FUeAgentRequestContext& CommandCtx, TSharedPtr<FJsonObject>& OutDataRef, TArray<TSharedPtr<FJsonValue>>& OutArtifactsRef, FString& OutErrRef)
		{
			return (Server->*Method)(CommandCtx, OutDataRef, OutArtifactsRef, OutErrRef);
		}

		template<FExactWithCtxMutableMethod Method>
		static bool InvokeExactMutableWithCtx(const FUeAgentHttpServer* Server, const FUeAgentRequestContext& CommandCtx, TSharedPtr<FJsonObject>& OutDataRef, TArray<TSharedPtr<FJsonValue>>&, FString& OutErrRef)
		{
			return (const_cast<FUeAgentHttpServer*>(Server)->*Method)(CommandCtx, OutDataRef, OutErrRef);
		}

		template<FPrefixedConstMethod Method>
		static bool InvokePrefixed(const FUeAgentHttpServer* Server, const FString& CommandLower, const FUeAgentRequestContext& CommandCtx, TSharedPtr<FJsonObject>& OutDataRef, FString& OutErrRef)
		{
			return (Server->*Method)(CommandLower, CommandCtx, OutDataRef, OutErrRef);
		}

		static FExactCommandInvoker FindExactCommandInvoker(const FString& CommandLower)
		{
			static const FExactCommandEntry GExactCommandEntries[] = {
				{ TEXT("exec_batch"), &InvokeExactWithCtxArtifacts<&FUeAgentHttpServer::ExecuteBatchCommands> },
				{ TEXT("get_world_state"), &InvokeExactNoCtx<&FUeAgentHttpServer::CmdGetWorldState> },
				{ TEXT("list_actors"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdListActors> },
				{ TEXT("actor_list_components"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdActorListComponents> },
				{ TEXT("level_list_actor_components"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdActorListComponents> },
				{ TEXT("level_get_actor_property"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdLevelGetActorProperty> },
				{ TEXT("actor_set_property"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdActorSetProperty> },
				{ TEXT("level_set_actor_property"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdActorSetProperty> },
				{ TEXT("level_get_component_property"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdLevelGetComponentProperty> },
				{ TEXT("component_set_property"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdComponentSetProperty> },
				{ TEXT("level_set_component_property"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdComponentSetProperty> },
				{ TEXT("spawn_actor"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdSpawnActor> },
				{ TEXT("level_spawn_wall_with_opening"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdLevelSpawnWallWithOpening> },
				{ TEXT("destroy_actor"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdDestroyActor> },
				{ TEXT("level_mark_probe"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdLevelMarkProbe> },
				{ TEXT("level_generate_probes"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdLevelGenerateProbes> },
				{ TEXT("level_destroy_folder_actors"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdLevelDestroyFolderActors> },
				{ TEXT("level_cleanup_empty_folders"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdLevelCleanupEmptyFolders> },
				{ TEXT("level_attach_actor"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdLevelAttachActor> },
				{ TEXT("level_detach_actor"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdLevelDetachActor> },
				{ TEXT("level_get_selection"), &InvokeExactNoCtx<&FUeAgentHttpServer::CmdLevelGetSelection> },
				{ TEXT("level_set_selection"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdLevelSetSelection> },
				{ TEXT("level_duplicate_actor"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdLevelDuplicateActor> },
				{ TEXT("level_set_actor_folder"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdLevelSetActorFolder> },
				{ TEXT("level_add_actor_tag"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdLevelAddActorTag> },
				{ TEXT("level_get_actor_transform"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdLevelGetActorTransform> },
				{ TEXT("level_set_actor_transform"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdLevelSetActorTransform> },
				{ TEXT("level_set_actor_location"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdLevelSetActorLocation> },
				{ TEXT("level_set_actor_rotation"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdLevelSetActorRotation> },
				{ TEXT("level_set_actor_scale"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdLevelSetActorScale> },
				{ TEXT("viewport_get_camera"), &InvokeExactNoCtx<&FUeAgentHttpServer::CmdViewportGetCamera> },
				{ TEXT("viewport_set_camera"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdViewportSetCamera> },
				{ TEXT("viewport_set_realtime"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdViewportSetRealtime> },
				{ TEXT("viewport_set_game_view"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdViewportSetGameView> },
				{ TEXT("viewport_focus_actor"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdViewportFocusActor> },
				{ TEXT("viewport_focus_actor_safe"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdViewportFocusActorSafe> },
				{ TEXT("viewport_frame_actor"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdViewportFrameActor> },
				{ TEXT("viewport_frame_selection"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdViewportFrameSelection> },
				{ TEXT("viewport_frame_actors"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdViewportFrameActors> },
				{ TEXT("viewport_frame_folder"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdViewportFrameFolder> },
				{ TEXT("viewport_deproject_screen_to_world"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdViewportDeprojectScreenToWorld> },
				{ TEXT("viewport_trace_screen_point"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdViewportTraceScreenPoint> },
				{ TEXT("viewport_pick_actor_at_screen"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdViewportPickActorAtScreen> },
				{ TEXT("viewport_select_actor_at_screen"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdViewportSelectActorAtScreen> },
				{ TEXT("level_get_nearby_actor_obbs"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdLevelGetNearbyActorObbs> },
				{ TEXT("level_validate_connectivity"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdLevelValidateConnectivity> },
				{ TEXT("navmesh_build"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdNavmeshBuild> },
				{ TEXT("navmesh_project_point"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdNavmeshProjectPoint> },
				{ TEXT("navmesh_find_path"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdNavmeshFindPath> },
				{ TEXT("navmesh_spawn_bounds_volume"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdNavmeshSpawnBoundsVolume> },
				{ TEXT("level_trace_world_ray"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdLevelTraceWorldRay> },
				{ TEXT("level_sweep_capsule"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdLevelSweepCapsule> },
				{ TEXT("level_sweep_capsule_path"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdLevelSweepCapsulePath> },
				{ TEXT("level_check_overlaps"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdLevelCheckOverlaps> },
				{ TEXT("level_snap_to_surface"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdLevelSnapToSurface> },
				{ TEXT("mesh_get_closest_vertex"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdMeshGetClosestVertex> },
				{ TEXT("mesh_get_vertex_world_position"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdMeshGetVertexWorldPosition> },
				{ TEXT("level_align_actor_vertex_to_vertex"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdLevelAlignActorVertexToVertex> },
				{ TEXT("level_align_actor_by_bounds"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdLevelAlignActorByBounds> },
				{ TEXT("level_align_face_to_face"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdLevelAlignFaceToFace> },
				{ TEXT("screenshot_viewport"), &InvokeExactWithCtxArtifacts<&FUeAgentHttpServer::CmdScreenshotViewport> },
				{ TEXT("screenshot_viewport_buffer"), &InvokeExactWithCtxArtifacts<&FUeAgentHttpServer::CmdScreenshotViewportBuffer> },
				{ TEXT("save_current_level"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdSaveCurrentLevel> },
				{ TEXT("begin_transaction"), &InvokeExactMutableWithCtx<&FUeAgentHttpServer::CmdBeginTransaction> },
				{ TEXT("end_transaction"), &InvokeExactMutableWithCtx<&FUeAgentHttpServer::CmdEndTransaction> },
				{ TEXT("undo"), &InvokeExactNoCtx<&FUeAgentHttpServer::CmdUndo> },
				{ TEXT("redo"), &InvokeExactNoCtx<&FUeAgentHttpServer::CmdRedo> },
				{ TEXT("editor_get_open_assets"), &InvokeExactNoCtx<&FUeAgentHttpServer::CmdEditorGetOpenAssets> },
				{ TEXT("open_asset_editor"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdOpenAssetEditor> },
				{ TEXT("save_asset"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdSaveAsset> },
				{ TEXT("asset_duplicate"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdAssetDuplicate> },
				{ TEXT("asset_import_texture"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdAssetImportTexture> },
				{ TEXT("asset_import_fbx_skeletal_mesh"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdAssetImportFbxSkeletalMesh> },
				{ TEXT("asset_import_fbx_animation"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdAssetImportFbxAnimation> },
				{ TEXT("asset_export_property_json"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdAssetExportPropertyJson> },
				{ TEXT("asset_apply_property_json"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdAssetApplyPropertyJson> },
				{ TEXT("curve_export_json"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdCurveExportJson> },
				{ TEXT("curve_apply_json"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdCurveApplyJson> },
				{ TEXT("editor_list_dirty_resources"), &InvokeExactNoCtx<&FUeAgentHttpServer::CmdEditorListDirtyResources> },
				{ TEXT("editor_resolve_dirty_resources"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdEditorResolveDirtyResources> },
				{ TEXT("editor_close"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdEditorClose> },
				{ TEXT("editor_prepare_exit"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdEditorPrepareExit> },
				{ TEXT("landscape_create"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdLandscapeCreate> },
				{ TEXT("landscape_raise_circle"), &InvokeExactWithCtx<&FUeAgentHttpServer::CmdLandscapeRaiseCircle> },
			};

			for (const FExactCommandEntry& Entry : GExactCommandEntries)
			{
				if (CommandLower == Entry.Name)
				{
					return Entry.Invoker;
				}
			}
			return nullptr;
		}

		static FPrefixedCommandInvoker FindPrefixedCommandInvoker(const FString& CommandLower)
		{
			static const FPrefixedCommandEntry GPrefixedCommandEntries[] = {
				{ TEXT("blueprint_"), &InvokePrefixed<&FUeAgentHttpServer::ExecuteBlueprintCommand> },
				{ TEXT("anim_blueprint_"), &InvokePrefixed<&FUeAgentHttpServer::ExecuteAnimBlueprintCommand> },
				{ TEXT("montage_"), &InvokePrefixed<&FUeAgentHttpServer::ExecuteMontageCommand> },
				{ TEXT("umg_"), &InvokePrefixed<&FUeAgentHttpServer::ExecuteUmgCommand> },
				{ TEXT("static_mesh_"), &InvokePrefixed<&FUeAgentHttpServer::ExecuteStaticMeshCommand> },
				{ TEXT("enhanced_input_"), &InvokePrefixed<&FUeAgentHttpServer::ExecuteEnhancedInputCommand> },
				{ TEXT("material_"), &InvokePrefixed<&FUeAgentHttpServer::ExecuteMaterialCommand> },
				{ TEXT("sequence_"), &InvokePrefixed<&FUeAgentHttpServer::ExecuteSequenceCommand> },
				{ TEXT("anim_sequence_"), &InvokePrefixed<&FUeAgentHttpServer::ExecuteAnimSequenceCommand> },
				{ TEXT("skeleton_"), &InvokePrefixed<&FUeAgentHttpServer::ExecuteSkeletonCommand> },
				{ TEXT("ik_rig_"), &InvokePrefixed<&FUeAgentHttpServer::ExecuteIKRigCommand> },
				{ TEXT("ik_retargeter_"), &InvokePrefixed<&FUeAgentHttpServer::ExecuteIKRetargeterCommand> },
				{ TEXT("niagara_"), &InvokePrefixed<&FUeAgentHttpServer::ExecuteNiagaraCommand> },
				{ TEXT("modeling_"), &InvokePrefixed<&FUeAgentHttpServer::ExecuteModelingCommand> },
			};

			for (const FPrefixedCommandEntry& Entry : GPrefixedCommandEntries)
			{
				if (CommandLower.StartsWith(Entry.Prefix))
				{
					return Entry.Invoker;
				}
			}
			return nullptr;
		}
	};
}

FUeAgentHttpServer::FUeAgentHttpServer() = default;

FUeAgentHttpServer::~FUeAgentHttpServer()
{
	Stop();
}

bool FUeAgentHttpServer::Start()
{
	if (bRunning)
	{
		return true;
	}

	FUeAgentInterfaceLogger::Init();

	const UUeAgentInterfaceSettings* Settings = UUeAgentInterfaceSettings::Get();
	if (!Settings)
	{
		return false;
	}

	Router = FHttpServerModule::Get().GetHttpRouter((uint32)Settings->Port, /*bFailOnBindFailure*/ true);
	if (!Router.IsValid())
	{
		FUeAgentInterfaceLogger::Log(TEXT("Start: failed to bind http router (port=%d)."), Settings->Port);
		return false;
	}

	if (!BindRoutes())
	{
		UnbindRoutes();
		Router.Reset();
		return false;
	}

	FHttpServerModule::Get().StartAllListeners();
	bRunning = true;

	FUeAgentInterfaceLogger::Log(TEXT("Server started. listen=%s port=%d token=%s version=%s"),
		*Settings->ListenAddress,
		Settings->Port,
		*UeAgentHttpServer::MaskToken(Settings->AuthToken),
		*FUeAgentInterfaceLogger::GetVersionTag());

	return true;
}

void FUeAgentHttpServer::Stop()
{
	if (!bRunning && !Router.IsValid())
	{
		return;
	}

	UnbindRoutes();
	Router.Reset();
	// HTTPServer's public module API only exposes a module-wide listener stop.
	FHttpServerModule::Get().StopAllListeners();
	bRunning = false;
	FUeAgentInterfaceLogger::Log(TEXT("Server stopped (routes unbound, listeners stopped)."));
}

bool FUeAgentHttpServer::IsRunning() const
{
	return bRunning;
}

int32 FUeAgentHttpServer::GetPort() const
{
	const UUeAgentInterfaceSettings* Settings = UUeAgentInterfaceSettings::Get();
	return Settings ? Settings->Port : 0;
}

FString FUeAgentHttpServer::GetListenAddress() const
{
	const UUeAgentInterfaceSettings* Settings = UUeAgentInterfaceSettings::Get();
	return Settings ? Settings->ListenAddress : TEXT("127.0.0.1");
}

FString FUeAgentHttpServer::GetAuthTokenMasked() const
{
	const UUeAgentInterfaceSettings* Settings = UUeAgentInterfaceSettings::Get();
	return Settings ? UeAgentHttpServer::MaskToken(Settings->AuthToken) : TEXT("********");
}

FString FUeAgentHttpServer::GetAuthToken() const
{
	const UUeAgentInterfaceSettings* Settings = UUeAgentInterfaceSettings::Get();
	return Settings ? Settings->AuthToken : FString();
}

bool FUeAgentHttpServer::BindRoutes()
{
	if (!Router.IsValid())
	{
		return false;
	}

	auto BindCheckedRoute = [this](const TCHAR* RoutePath, EHttpServerRequestVerbs Verbs, const FHttpRequestHandler& Handler) -> bool
	{
		const FHttpPath Path(RoutePath);
		const FHttpRouteHandle RouteHandle = Router->BindRoute(Path, Verbs, Handler);
		if (!RouteHandle.IsValid())
		{
			FUeAgentInterfaceLogger::Log(TEXT("BindRoutes: failed to bind route path=%s verbs=%u"), *Path.GetPath(), static_cast<uint32>(Verbs));
			return false;
		}

		RouteHandles.Add(RouteHandle);
		return true;
	};

	if (!BindCheckedRoute(TEXT("/api/ping"), EHttpServerRequestVerbs::VERB_GET, FHttpRequestHandler::CreateRaw(this, &FUeAgentHttpServer::HandlePing)))
	{
		return false;
	}

	if (!BindCheckedRoute(TEXT("/api/status"), EHttpServerRequestVerbs::VERB_GET, FHttpRequestHandler::CreateRaw(this, &FUeAgentHttpServer::HandleStatus)))
	{
		return false;
	}

	if (!BindCheckedRoute(TEXT("/api/exec"), EHttpServerRequestVerbs::VERB_POST, FHttpRequestHandler::CreateRaw(this, &FUeAgentHttpServer::HandleExec)))
	{
		return false;
	}

	RequestLogPreprocessorHandle = Router->RegisterRequestPreprocessor(FHttpRequestHandler::CreateLambda(
		[](const FHttpServerRequest& Request, const FHttpResultCallback&)
		{
			FUeAgentInterfaceLogger::Log(
				TEXT("Preprocess request verb=%s path=%s body_bytes=%d"),
				UeAgentHttpServer::VerbToString(Request.Verb),
				*Request.RelativePath.GetPath(),
				Request.Body.Num());
			return false;
		}));

	return true;
}

void FUeAgentHttpServer::UnbindRoutes()
{
	if (!Router.IsValid())
	{
		RequestLogPreprocessorHandle.Reset();
		RouteHandles.Reset();
		return;
	}

	if (RequestLogPreprocessorHandle.IsValid())
	{
		Router->UnregisterRequestPreprocessor(RequestLogPreprocessorHandle);
		RequestLogPreprocessorHandle.Reset();
	}

	for (FHttpRouteHandle& Handle : RouteHandles)
	{
		if (Handle.IsValid())
		{
			Router->UnbindRoute(Handle);
		}
	}
	RouteHandles.Reset();
}

bool FUeAgentHttpServer::HandlePing(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	FUeAgentInterfaceLogger::Log(TEXT("HandlePing verb=%s path=%s"), UeAgentHttpServer::VerbToString(Request.Verb), *Request.RelativePath.GetPath());

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetBoolField(TEXT("ok"), true);
	Root->SetStringField(TEXT("timestamp"), UeAgentHttpServer::NowIso());

	FString Out;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);

	OnComplete(MakeJsonResponse(200, Out));
	return true;
}

bool FUeAgentHttpServer::HandleStatus(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	FUeAgentInterfaceLogger::Log(TEXT("HandleStatus verb=%s path=%s"), UeAgentHttpServer::VerbToString(Request.Verb), *Request.RelativePath.GetPath());

	FString Reason;
	if (!IsAuthorized(Request, Reason))
	{
		FUeAgentInterfaceLogger::Log(TEXT("HandleStatus unauthorized reason=%s"), *Reason);
		OnComplete(MakeErrorResponse(401, TEXT("unauthorized"), Reason));
		return true;
	}

	const UUeAgentInterfaceSettings* Settings = UUeAgentInterfaceSettings::Get();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetBoolField(TEXT("running"), bRunning);
	Data->SetStringField(TEXT("listen_address"), Settings ? Settings->ListenAddress : TEXT(""));
	Data->SetNumberField(TEXT("port"), Settings ? Settings->Port : 0);
	Data->SetStringField(TEXT("token_masked"), Settings ? UeAgentHttpServer::MaskToken(Settings->AuthToken) : TEXT("********"));
	Data->SetStringField(TEXT("version_tag"), FUeAgentInterfaceLogger::GetVersionTag());
	Data->SetBoolField(TEXT("screenshots_enabled"), Settings ? Settings->bEnableScreenshots : false);

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetBoolField(TEXT("ok"), true);
	Root->SetStringField(TEXT("error"), TEXT(""));
	Root->SetStringField(TEXT("timestamp"), UeAgentHttpServer::NowIso());
	Root->SetObjectField(TEXT("data"), Data);
	Root->SetArrayField(TEXT("artifacts"), TArray<TSharedPtr<FJsonValue>>());

	FString Out;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);

	OnComplete(MakeJsonResponse(200, Out));
	return true;
}

bool FUeAgentHttpServer::HandleExec(const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	FUeAgentInterfaceLogger::Log(TEXT("HandleExec verb=%s path=%s body_bytes=%d"), UeAgentHttpServer::VerbToString(Request.Verb), *Request.RelativePath.GetPath(), Request.Body.Num());

	FString Reason;
	if (!IsAuthorized(Request, Reason))
	{
		FUeAgentInterfaceLogger::Log(TEXT("HandleExec unauthorized reason=%s"), *Reason);
		OnComplete(MakeErrorResponse(401, TEXT("unauthorized"), Reason));
		return true;
	}

	FUeAgentRequestContext Ctx;
	FString ParseError;
	if (!ParseExecBody(Request, Ctx, ParseError))
	{
		FUeAgentInterfaceLogger::Log(TEXT("HandleExec parse_failed error=%s"), *ParseError);
		OnComplete(MakeErrorResponse(400, TEXT("invalid_request"), ParseError));
		return true;
	}

	TSharedPtr<FJsonObject> Data;
	TArray<TSharedPtr<FJsonValue>> Artifacts;
	FString ExecError;
	const bool bOk = ExecuteCommandOnGameThread(Ctx, Data, Artifacts, ExecError);
	FUeAgentInterfaceLogger::Log(TEXT("HandleExec command=%s request_id=%s ok=%s error=%s"), *Ctx.Command, *Ctx.RequestId, bOk ? TEXT("true") : TEXT("false"), *ExecError);

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetBoolField(TEXT("ok"), bOk);
	Root->SetStringField(TEXT("request_id"), Ctx.RequestId);
	Root->SetStringField(TEXT("error"), bOk ? TEXT("") : ExecError);
	Root->SetStringField(TEXT("timestamp"), UeAgentHttpServer::NowIso());

	if (Data.IsValid())
	{
		Root->SetObjectField(TEXT("data"), Data);
	}
	else
	{
		Root->SetObjectField(TEXT("data"), MakeShared<FJsonObject>());
	}
	Root->SetArrayField(TEXT("artifacts"), Artifacts);

	FString Out;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);

	OnComplete(MakeJsonResponse(bOk ? 200 : 400, Out));
	return true;
}

bool FUeAgentHttpServer::IsAuthorized(const FHttpServerRequest& Request, FString& OutReason) const
{
	const UUeAgentInterfaceSettings* Settings = UUeAgentInterfaceSettings::Get();
	if (!Settings)
	{
		OutReason = TEXT("missing_settings");
		return false;
	}

	// Best-effort: if listening on loopback only, reject non-loopback peers.
	if (Settings->ListenAddress == TEXT("127.0.0.1") || Settings->ListenAddress == TEXT("localhost"))
	{
		if (!UeAgentHttpServer::IsLoopbackPeer(Request.PeerAddress))
		{
			OutReason = TEXT("non_loopback_peer");
			return false;
		}
	}

	const FString TokenHeader = UeAgentHttpServer::GetHeaderFirst(Request.Headers, UeAgentHttpServer::HeaderTokenLower);
	if (TokenHeader.IsEmpty())
	{
		OutReason = TEXT("missing_token_header");
		return false;
	}

	if (Settings->AuthToken.IsEmpty())
	{
		OutReason = TEXT("missing_server_token");
		return false;
	}

	if (TokenHeader != Settings->AuthToken)
	{
		OutReason = TEXT("token_mismatch");
		return false;
	}

	return true;
}

TUniquePtr<FHttpServerResponse> FUeAgentHttpServer::MakeJsonResponse(int32 Code, const FString& JsonText)
{
	TUniquePtr<FHttpServerResponse> Resp = FHttpServerResponse::Create(JsonText, UeAgentHttpServer::ContentTypeJson);
	Resp->Code = UeAgentHttpServer::ToResponseCode(Code);
	Resp->Headers.Add(TEXT("Cache-Control"), { TEXT("no-store") });
	return Resp;
}

TUniquePtr<FHttpServerResponse> FUeAgentHttpServer::MakeErrorResponse(int32 Code, const FString& ErrorCode, const FString& ErrorMessage)
{
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetBoolField(TEXT("ok"), false);
	Root->SetStringField(TEXT("error"), ErrorCode);
	Root->SetStringField(TEXT("timestamp"), UeAgentHttpServer::NowIso());

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("error_code"), ErrorCode);
	Data->SetStringField(TEXT("error_message"), ErrorMessage);
	Data->SetStringField(TEXT("timestamp"), UeAgentHttpServer::NowIso());
	Root->SetObjectField(TEXT("data"), Data);
	Root->SetArrayField(TEXT("artifacts"), TArray<TSharedPtr<FJsonValue>>());

	FString Out;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);

	return MakeJsonResponse(Code, Out);
}

bool FUeAgentHttpServer::ParseExecBody(const FHttpServerRequest& Request, FUeAgentRequestContext& OutCtx, FString& OutError) const
{
	const FUTF8ToTCHAR BodyConv(reinterpret_cast<const ANSICHAR*>(Request.Body.GetData()), Request.Body.Num());
	const FString BodyStr(BodyConv.Length(), BodyConv.Get());
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(BodyStr);

	TSharedPtr<FJsonObject> Root;
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		OutError = TEXT("invalid_json");
		return false;
	}

	if (!JsonTryGetString(Root, TEXT("request_id"), OutCtx.RequestId))
	{
		OutCtx.RequestId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
	}

	if (!JsonTryGetString(Root, TEXT("command"), OutCtx.Command) || OutCtx.Command.IsEmpty())
	{
		OutError = TEXT("missing_command");
		return false;
	}

	const TSharedPtr<FJsonObject>* PParams = nullptr;
	if (Root->TryGetObjectField(TEXT("params"), PParams) && PParams && PParams->IsValid())
	{
		OutCtx.Params = *PParams;
	}
	else
	{
		OutCtx.Params = MakeShared<FJsonObject>();
	}

	return true;
}

bool FUeAgentHttpServer::ParseBatchCommands(const FUeAgentRequestContext& BatchCtx, TArray<FUeAgentRequestContext>& OutCommands, bool& bOutStopOnError, FString& OutError) const
{
	OutCommands.Reset();
	bOutStopOnError = true;

	if (!BatchCtx.Params.IsValid())
	{
		OutError = TEXT("missing_params");
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* CommandValues = nullptr;
	if (!BatchCtx.Params->TryGetArrayField(TEXT("commands"), CommandValues) || !CommandValues)
	{
		OutError = TEXT("missing_commands");
		return false;
	}

	if (CommandValues->Num() <= 0)
	{
		OutError = TEXT("commands_empty");
		return false;
	}

	bool bStopOnError = true;
	BatchCtx.Params->TryGetBoolField(TEXT("stop_on_error"), bStopOnError);
	bOutStopOnError = bStopOnError;

	OutCommands.Reserve(CommandValues->Num());
	for (int32 Index = 0; Index < CommandValues->Num(); ++Index)
	{
		const TSharedPtr<FJsonValue>& CommandValue = (*CommandValues)[Index];
		if (!CommandValue.IsValid() || CommandValue->Type != EJson::Object)
		{
			OutError = FString::Printf(TEXT("invalid_command_item_%d"), Index);
			return false;
		}

		const TSharedPtr<FJsonObject> CommandObj = CommandValue->AsObject();
		if (!CommandObj.IsValid())
		{
			OutError = FString::Printf(TEXT("invalid_command_item_object_%d"), Index);
			return false;
		}

		FString CommandName;
		if (!JsonTryGetString(CommandObj, TEXT("command"), CommandName) || CommandName.IsEmpty())
		{
			OutError = FString::Printf(TEXT("missing_command_%d"), Index);
			return false;
		}

		if (CommandName.Equals(TEXT("exec_batch"), ESearchCase::IgnoreCase))
		{
			OutError = FString::Printf(TEXT("nested_exec_batch_not_allowed_%d"), Index);
			return false;
		}

		FString RequestId;
		if (!JsonTryGetString(CommandObj, TEXT("request_id"), RequestId) || RequestId.IsEmpty())
		{
			RequestId = FString::Printf(TEXT("%s-%d"), *BatchCtx.RequestId, Index);
		}

		const TSharedPtr<FJsonObject>* ParamsObj = nullptr;
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		if (CommandObj->TryGetObjectField(TEXT("params"), ParamsObj) && ParamsObj && ParamsObj->IsValid())
		{
			Params = *ParamsObj;
		}

		FUeAgentRequestContext SubCommand;
		SubCommand.RequestId = RequestId;
		SubCommand.Command = CommandName;
		SubCommand.Params = Params;
		OutCommands.Add(SubCommand);
	}

	return true;
}

bool FUeAgentHttpServer::ExecuteBatchCommands(const FUeAgentRequestContext& BatchCtx, TSharedPtr<FJsonObject>& OutData, TArray<TSharedPtr<FJsonValue>>& OutArtifacts, FString& OutError) const
{
	TArray<FUeAgentRequestContext> Commands;
	bool bStopOnError = true;
	FString ParseError;
	if (!ParseBatchCommands(BatchCtx, Commands, bStopOnError, ParseError))
	{
		OutError = ParseError;
		return false;
	}

	FUeAgentInterfaceLogger::Log(TEXT("ExecBatch start request_id=%s count=%d stop_on_error=%s"), *BatchCtx.RequestId, Commands.Num(), bStopOnError ? TEXT("true") : TEXT("false"));

	OutData = MakeShared<FJsonObject>();
	OutArtifacts.Reset();

	TArray<TSharedPtr<FJsonValue>> Results;
	Results.Reserve(Commands.Num());

	int32 Succeeded = 0;
	int32 FailedIndex = INDEX_NONE;
	FString FailedCommand;
	FString FailedRequestId;
	FString FailedError;

	for (int32 Index = 0; Index < Commands.Num(); ++Index)
	{
		const FUeAgentRequestContext& SubCommand = Commands[Index];
		TSharedPtr<FJsonObject> SubData;
		TArray<TSharedPtr<FJsonValue>> SubArtifacts;
		FString SubError;
		const bool bSubOk = ExecuteCommandOnGameThread(SubCommand, SubData, SubArtifacts, SubError);

		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		Item->SetNumberField(TEXT("index"), Index);
		Item->SetStringField(TEXT("request_id"), SubCommand.RequestId);
		Item->SetStringField(TEXT("command"), SubCommand.Command);
		Item->SetBoolField(TEXT("ok"), bSubOk);
		Item->SetStringField(TEXT("error"), bSubOk ? TEXT("") : SubError);
		Item->SetObjectField(TEXT("data"), SubData.IsValid() ? SubData : MakeShared<FJsonObject>());
		Item->SetArrayField(TEXT("artifacts"), SubArtifacts);
		Results.Add(MakeShared<FJsonValueObject>(Item));

		if (bSubOk)
		{
			++Succeeded;
		}
		else if (FailedIndex == INDEX_NONE)
		{
			FailedIndex = Index;
			FailedCommand = SubCommand.Command;
			FailedRequestId = SubCommand.RequestId;
			FailedError = SubError;

			FUeAgentInterfaceLogger::Log(TEXT("ExecBatch interrupted request_id=%s index=%d command=%s error=%s"),
				*BatchCtx.RequestId, FailedIndex, *FailedCommand, *FailedError);

			if (bStopOnError)
			{
				break;
			}
		}
	}

	const int32 Executed = Results.Num();
	const bool bInterrupted = (FailedIndex != INDEX_NONE) && bStopOnError;

	OutData->SetStringField(TEXT("mode"), TEXT("exec_batch"));
	OutData->SetNumberField(TEXT("total"), Commands.Num());
	OutData->SetNumberField(TEXT("executed"), Executed);
	OutData->SetNumberField(TEXT("succeeded"), Succeeded);
	OutData->SetNumberField(TEXT("failed"), Executed - Succeeded);
	OutData->SetBoolField(TEXT("stop_on_error"), bStopOnError);
	OutData->SetBoolField(TEXT("interrupted"), bInterrupted);
	OutData->SetNumberField(TEXT("failed_index"), FailedIndex);
	OutData->SetStringField(TEXT("failed_command"), FailedCommand);
	OutData->SetStringField(TEXT("failed_request_id"), FailedRequestId);
	OutData->SetStringField(TEXT("failed_error"), FailedError);
	OutData->SetArrayField(TEXT("results"), Results);

	if (FailedIndex != INDEX_NONE)
	{
		OutError = FString::Printf(TEXT("batch_interrupted_at_%d:%s"), FailedIndex, *FailedError);
		return false;
	}

	FUeAgentInterfaceLogger::Log(TEXT("ExecBatch done request_id=%s total=%d succeeded=%d"), *BatchCtx.RequestId, Commands.Num(), Succeeded);
	return true;
}

bool FUeAgentHttpServer::ExecuteCommandOnGameThread(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, TArray<TSharedPtr<FJsonValue>>& OutArtifacts, FString& OutError) const
{
	auto ExecOnGT = [this](const FUeAgentRequestContext& InCtx, TSharedPtr<FJsonObject>& Data, TArray<TSharedPtr<FJsonValue>>& Artifacts, FString& Err) -> bool
	{
		Data = MakeShared<FJsonObject>();
		Artifacts.Reset();
		Err.Reset();

		const FString Cmd = InCtx.Command.ToLower();

		if (const FExactCommandInvoker ExactInvoker = FUeAgentHttpServerCommandAccess::FindExactCommandInvoker(Cmd))
		{
			return ExactInvoker(this, InCtx, Data, Artifacts, Err);
		}

		if (const FPrefixedCommandInvoker PrefixedInvoker = FUeAgentHttpServerCommandAccess::FindPrefixedCommandInvoker(Cmd))
		{
			return PrefixedInvoker(this, Cmd, InCtx, Data, Err);
		}

		Err = TEXT("unknown_command");
		return false;
	};

	if (IsInGameThread())
	{
		TSharedPtr<FJsonObject> Data;
		TArray<TSharedPtr<FJsonValue>> Artifacts;
		FString Err;
		const bool bOk = ExecOnGT(Ctx, Data, Artifacts, Err);
		OutData = Data;
		OutArtifacts = Artifacts;
		if (!bOk)
		{
			OutError = Err;
			return false;
		}
		return true;
	}

	struct FExecResult
	{
		bool bOk = false;
		TSharedPtr<FJsonObject> Data;
		TArray<TSharedPtr<FJsonValue>> Artifacts;
		FString Error;
	};

	TSharedRef<FExecResult, ESPMode::ThreadSafe> Result = MakeShared<FExecResult, ESPMode::ThreadSafe>();
	FEvent* Done = FPlatformProcess::GetSynchEventFromPool(true);

	AsyncTask(ENamedThreads::GameThread, [ExecOnGT, Ctx, Result, Done]()
	{
		FString Err;
		TSharedPtr<FJsonObject> Data;
		TArray<TSharedPtr<FJsonValue>> Artifacts;
		const bool bOk = ExecOnGT(Ctx, Data, Artifacts, Err);

		Result->bOk = bOk;
		Result->Data = Data;
		Result->Artifacts = Artifacts;
		Result->Error = Err;
		Done->Trigger();
	});

	const bool bDone = Done->Wait(FTimespan::FromSeconds(30));
	FPlatformProcess::ReturnSynchEventToPool(Done);

	if (!bDone)
	{
		OutError = TEXT("timeout_waiting_gamethread");
		return false;
	}

	OutData = Result->Data;
	OutArtifacts = Result->Artifacts;
	if (!Result->bOk)
	{
		OutError = Result->Error;
		return false;
	}

	return true;
}

bool FUeAgentHttpServer::JsonTryGetObject(const TSharedPtr<FJsonObject>& Obj, const FString& Key, TSharedPtr<FJsonObject>& OutObj)
{
	if (!Obj.IsValid())
	{
		return false;
	}
	const TSharedPtr<FJsonObject>* P = nullptr;
	if (Obj->TryGetObjectField(Key, P) && P && P->IsValid())
	{
		OutObj = *P;
		return true;
	}
	return false;
}

bool FUeAgentHttpServer::JsonTryGetString(const TSharedPtr<FJsonObject>& Obj, const FString& Key, FString& OutValue)
{
	return Obj.IsValid() && Obj->TryGetStringField(Key, OutValue);
}

bool FUeAgentHttpServer::JsonTryGetNumber(const TSharedPtr<FJsonObject>& Obj, const FString& Key, double& OutValue)
{
	return UeAgentJsonDiagnostics::TryReadNumberFieldByAliases(Obj, { Key }, OutValue);
}

bool FUeAgentHttpServer::JsonTryGetBool(const TSharedPtr<FJsonObject>& Obj, const FString& Key, bool& OutValue)
{
	return Obj.IsValid() && Obj->TryGetBoolField(Key, OutValue);
}

bool FUeAgentHttpServer::JsonTryGetVector(const TSharedPtr<FJsonObject>& Obj, const FString& Key, FVector& OutValue)
{
	const TSharedPtr<FJsonValue>* FieldValue = UeAgentJsonDiagnostics::FindFieldValue(Obj, Key);
	return FieldValue && UeAgentJsonDiagnostics::TryReadVectorValue(*FieldValue, OutValue);
}

bool FUeAgentHttpServer::JsonTryGetRotator(const TSharedPtr<FJsonObject>& Obj, const FString& Key, FRotator& OutValue)
{
	const TSharedPtr<FJsonValue>* FieldValue = UeAgentJsonDiagnostics::FindFieldValue(Obj, Key);
	return FieldValue && UeAgentJsonDiagnostics::TryReadRotatorValue(*FieldValue, OutValue);
}

TSharedPtr<FJsonObject> FUeAgentHttpServer::VecToJson(const FVector& V)
{
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetNumberField(TEXT("x"), V.X);
	Obj->SetNumberField(TEXT("y"), V.Y);
	Obj->SetNumberField(TEXT("z"), V.Z);
	return Obj;
}

TSharedPtr<FJsonObject> FUeAgentHttpServer::RotToJson(const FRotator& R)
{
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetNumberField(TEXT("pitch"), R.Pitch);
	Obj->SetNumberField(TEXT("yaw"), R.Yaw);
	Obj->SetNumberField(TEXT("roll"), R.Roll);
	return Obj;
}

void FUeAgentHttpServer::SetPropertyImportResultFields(
	const TSharedPtr<FJsonObject>& Obj,
	FProperty* Property,
	const FString& RequestedValueText,
	const FString& AppliedValueText,
	const FString& ImportStatus,
	const FString& ImportError)
{
	if (!Obj.IsValid())
	{
		return;
	}

	const bool bReadBackValue = ImportStatus.Equals(TEXT("imported_and_read_back"), ESearchCase::CaseSensitive)
		|| ImportStatus.Equals(TEXT("assigned_object_reference"), ESearchCase::CaseSensitive);
	const bool bExactMatch = bReadBackValue && AppliedValueText.Equals(RequestedValueText, ESearchCase::CaseSensitive);
	Obj->SetStringField(TEXT("requested_value_text"), RequestedValueText);
	Obj->SetStringField(TEXT("applied_value_text"), AppliedValueText);
	Obj->SetBoolField(TEXT("property_value_read_back"), bReadBackValue);
	Obj->SetBoolField(TEXT("value_text_exact_match"), bExactMatch);
	Obj->SetBoolField(TEXT("value_text_changed_after_import"), bReadBackValue && !bExactMatch);
	Obj->SetStringField(TEXT("property_import_status"), ImportStatus);
	Obj->SetBoolField(TEXT("property_import_verified"), bReadBackValue);
	Obj->SetStringField(TEXT("property_import_error"), bReadBackValue ? TEXT("") : (ImportError.IsEmpty() ? ImportStatus : ImportError));
	Obj->SetStringField(TEXT("cpp_type"), Property ? Property->GetCPPType() : TEXT(""));
}

TSharedPtr<FJsonObject> FUeAgentHttpServer::MakePropertyImportResultJson(
	const FString& PropertyName,
	FProperty* Property,
	const FString& RequestedValueText,
	const FString& AppliedValueText,
	const FString& ImportStatus,
	const FString& ImportError)
{
	TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetStringField(TEXT("property_name"), PropertyName);
	SetPropertyImportResultFields(Obj, Property, RequestedValueText, AppliedValueText, ImportStatus, ImportError);
	return Obj;
}

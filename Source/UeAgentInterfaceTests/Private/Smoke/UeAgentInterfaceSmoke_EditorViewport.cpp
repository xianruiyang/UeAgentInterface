#if WITH_DEV_AUTOMATION_TESTS

#include "UeAgentSmokeTestSharedIncludes.inl"

#include "Animation/AnimBlueprint.h"
#include "Animation/AnimationAsset.h"
#include "Animation/Skeleton.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "ControlRigBlueprint.h"
#include "Engine/Blueprint.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/SkeletalMesh.h"
#include "Framework/Docking/TabManager.h"
#include "HAL/FileManager.h"
#include "GroomAsset.h"
#include "LevelSequence.h"
#include "Materials/Material.h"
#include "MovieScene.h"
#include "NiagaraComponent.h"
#include "NiagaraSimCache.h"
#include "NiagaraSystem.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "Rigs/RigHierarchy.h"
#include "Subsystems/AssetEditorSubsystem.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentInterfaceEditorViewportSmokeTest,
	"GptProjectTest.UeAgentInterface.Smoke.EditorViewport",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUeAgentInterfaceEditorViewportSmokeTest::RunTest(const FString& Parameters)
{
	if (FApp::IsUnattended())
	{
		FGlobalTabmanager::Get()->SetCanSavePersistentLayouts(false);
	}

	FScopedUeAgentHttpServer ServerScope;
	FString InitError;
	if (!ServerScope.Initialize(InitError))
	{
		AddError(FString::Printf(TEXT("Initialize failed: %s"), *InitError));
		return false;
	}

	auto ExecJson = [this, &ServerScope](const FString& RequestId, const FString& Command, const FString& ParamsJson, int32 ExpectedStatus, TSharedPtr<FJsonObject>& OutJson) -> bool
	{
		const FString Body = MakeExecRequestBody(RequestId, Command, ParamsJson);
		FHttpSmokeResult Result;
		TestTrue(FString::Printf(TEXT("%s request completed"), *RequestId), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, Body, Result, 45.0));
		TestEqual(FString::Printf(TEXT("%s status code"), *RequestId), Result.StatusCode, ExpectedStatus);
		TestTrue(FString::Printf(TEXT("%s response parses as JSON"), *RequestId), ParseJson(Result.Body, OutJson));
		return OutJson.IsValid();
	};

	auto ExecJsonExpectFailure = [this, &ServerScope](const FString& RequestId, const FString& Command, const FString& ParamsJson, const FString& ExpectedToken, TSharedPtr<FJsonObject>& OutJson) -> bool
	{
		const FString Body = MakeExecRequestBody(RequestId, Command, ParamsJson);
		FHttpSmokeResult Result;
		if (!TestTrue(FString::Printf(TEXT("%s request completed"), *RequestId), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, Body, Result, 45.0)))
		{
			return false;
		}
		if (!TestTrue(FString::Printf(TEXT("%s status is command failure"), *RequestId), Result.StatusCode == 200 || Result.StatusCode == 400))
		{
			AddError(Result.Body.Left(1024));
			return false;
		}
		if (!TestTrue(FString::Printf(TEXT("%s response parses as JSON"), *RequestId), ParseJson(Result.Body, OutJson)) || !OutJson.IsValid())
		{
			return false;
		}

		bool bOk = true;
		OutJson->TryGetBoolField(TEXT("ok"), bOk);
		if (!TestFalse(FString::Printf(TEXT("%s ok=false"), *RequestId), bOk))
		{
			AddError(Result.Body.Left(1024));
			return false;
		}

		FString Error;
		OutJson->TryGetStringField(TEXT("error"), Error);
		const bool bMatched = ExpectedToken.IsEmpty() || Error.Contains(ExpectedToken) || Result.Body.Contains(ExpectedToken);
		if (!TestTrue(FString::Printf(TEXT("%s expected token %s"), *RequestId, *ExpectedToken), bMatched))
		{
			AddError(Result.Body.Left(2048));
			return false;
		}
		return true;
	};

	auto GetData = [this](const FString& Label, const TSharedPtr<FJsonObject>& Json) -> TSharedPtr<FJsonObject>
	{
		const TSharedPtr<FJsonObject>* Data = nullptr;
		TestTrue(FString::Printf(TEXT("%s contains data object"), *Label), Json.IsValid() && Json->TryGetObjectField(TEXT("data"), Data) && Data && Data->IsValid());
		return (Data && Data->IsValid()) ? *Data : nullptr;
	};

	auto TryGetNestedObject = [](const TSharedPtr<FJsonObject>& Owner, const TCHAR* FieldName, TSharedPtr<FJsonObject>& OutObject) -> bool
	{
		const TSharedPtr<FJsonObject>* ObjectPtr = nullptr;
		if (Owner.IsValid() && Owner->TryGetObjectField(FieldName, ObjectPtr) && ObjectPtr && ObjectPtr->IsValid())
		{
			OutObject = *ObjectPtr;
			return true;
		}
		return false;
	};

	auto BoolJson = [](bool bValue) -> const TCHAR*
	{
		return bValue ? TEXT("true") : TEXT("false");
	};

	auto EscapeJsonString = [](const FString& Value) -> FString
	{
		FString Out;
		Out.Reserve(Value.Len());
		for (int32 Index = 0; Index < Value.Len(); ++Index)
		{
			const TCHAR Ch = Value[Index];
			switch (Ch)
			{
			case TCHAR('\"'):
				Out += TEXT("\\\"");
				break;
			case TCHAR('\\'):
				Out += TEXT("\\\\");
				break;
			case TCHAR('\b'):
				Out += TEXT("\\b");
				break;
			case TCHAR('\f'):
				Out += TEXT("\\f");
				break;
			case TCHAR('\n'):
				Out += TEXT("\\n");
				break;
			case TCHAR('\r'):
				Out += TEXT("\\r");
				break;
			case TCHAR('\t'):
				Out += TEXT("\\t");
				break;
			default:
				if (Ch < TCHAR(0x20))
				{
					Out += FString::Printf(TEXT("\\u%04x"), static_cast<uint32>(Ch));
				}
				else
				{
					Out.AppendChar(Ch);
				}
				break;
			}
		}
		return Out;
	};

	auto ArrayHasString = [](const TSharedPtr<FJsonObject>& Owner, const TCHAR* FieldName, const FString& Expected) -> bool
	{
		const TArray<TSharedPtr<FJsonValue>>* ArrayPtr = nullptr;
		if (!Owner.IsValid() || !Owner->TryGetArrayField(FieldName, ArrayPtr) || !ArrayPtr)
		{
			return false;
		}
		for (const TSharedPtr<FJsonValue>& Value : *ArrayPtr)
		{
			if (Value.IsValid() && Value->AsString() == Expected)
			{
				return true;
			}
		}
		return false;
	};

	auto ReadVector = [](const TSharedPtr<FJsonObject>& Owner, const TCHAR* FieldName, FVector& OutVector) -> bool
	{
		const TSharedPtr<FJsonObject>* ObjectPtr = nullptr;
		if (!Owner.IsValid() || !Owner->TryGetObjectField(FieldName, ObjectPtr) || !ObjectPtr || !ObjectPtr->IsValid())
		{
			return false;
		}
		OutVector.X = (*ObjectPtr)->GetNumberField(TEXT("x"));
		OutVector.Y = (*ObjectPtr)->GetNumberField(TEXT("y"));
		OutVector.Z = (*ObjectPtr)->GetNumberField(TEXT("z"));
		return true;
	};

	auto ReadRotator = [](const TSharedPtr<FJsonObject>& Owner, const TCHAR* FieldName, FRotator& OutRotator) -> bool
	{
		const TSharedPtr<FJsonObject>* ObjectPtr = nullptr;
		if (!Owner.IsValid() || !Owner->TryGetObjectField(FieldName, ObjectPtr) || !ObjectPtr || !ObjectPtr->IsValid())
		{
			return false;
		}
		OutRotator.Pitch = (*ObjectPtr)->GetNumberField(TEXT("pitch"));
		OutRotator.Yaw = (*ObjectPtr)->GetNumberField(TEXT("yaw"));
		OutRotator.Roll = (*ObjectPtr)->GetNumberField(TEXT("roll"));
		return true;
	};

	auto ReadCamera = [&](const TSharedPtr<FJsonObject>& StateData, FVector& OutLocation, FRotator& OutRotation, double& OutFov) -> bool
	{
		TSharedPtr<FJsonObject> CameraObj;
		if (!TryGetNestedObject(StateData, TEXT("camera"), CameraObj))
		{
			return false;
		}
		if (!ReadVector(CameraObj, TEXT("location"), OutLocation) || !ReadRotator(CameraObj, TEXT("rotation"), OutRotation))
		{
			return false;
		}
		OutFov = CameraObj->GetNumberField(TEXT("fov"));
		return true;
	};

	FVector OriginalCameraLocation = FVector::ZeroVector;
	FRotator OriginalCameraRotation = FRotator::ZeroRotator;
	double OriginalCameraFov = 90.0;
	bool bHasOriginalCamera = false;
	FString OriginalViewMode = TEXT("lit");
	bool bOriginalGrid = true;
	bool bOriginalBounds = false;
	bool bOriginalPivot = true;
	bool bOriginalSelection = true;
	bool bOriginalModeWidgets = true;
	bool bOriginalCollision = false;
	bool bOriginalRealtime = false;
	bool bOriginalGameView = false;
	const bool bCanRenderViewport = FApp::CanEverRender();

	TSharedPtr<FJsonObject> ListJson;
	if (ExecJson(TEXT("smoke-editor-viewport-list"), TEXT("editor_viewport_list"), TEXT("{}"), 200, ListJson))
	{
		TestTrue(TEXT("Editor viewport list ok"), ListJson->GetBoolField(TEXT("ok")));
		const TSharedPtr<FJsonObject> Data = GetData(TEXT("Editor viewport list"), ListJson);
		if (Data.IsValid())
		{
			TestEqual(TEXT("Editor viewport list schema"), Data->GetStringField(TEXT("schema")), FString(TEXT("uai.editor_viewport.list.v1")));
			TestTrue(TEXT("Editor viewport list has at least one viewport"), Data->GetNumberField(TEXT("viewport_count")) > 0.0);
			const TArray<TSharedPtr<FJsonValue>>* Viewports = nullptr;
			TestTrue(TEXT("Editor viewport list viewports array"), Data->TryGetArrayField(TEXT("viewports"), Viewports) && Viewports && Viewports->Num() > 0);
		}
	}

	const TArray<TPair<FString, FString>> SpecialPreviewBoundaryCases = {
		{ TEXT("texture2d"), TEXT("special_texture_preview_not_3d_editor_viewport") },
		{ TEXT("texture_render_target_2d"), TEXT("special_render_target_preview_not_3d_editor_viewport") },
		{ TEXT("media_texture"), TEXT("special_media_preview_not_3d_editor_viewport") },
		{ TEXT("paper_sprite"), TEXT("special_paper2d_preview_not_3d_editor_viewport") },
		{ TEXT("sparse_volume_texture"), TEXT("special_sparse_volume_texture_preview_not_3d_editor_viewport") },
		{ TEXT("subuv_animation"), TEXT("special_subuv_preview_not_3d_editor_viewport") },
	};
	for (const TPair<FString, FString>& BoundaryCase : SpecialPreviewBoundaryCases)
	{
		TSharedPtr<FJsonObject> SpecialBoundaryJson;
		ExecJsonExpectFailure(
			FString::Printf(TEXT("smoke-editor-viewport-special-boundary-%s"), *BoundaryCase.Key),
			TEXT("editor_viewport_get_state"),
			FString::Printf(TEXT("{\"target\":{\"scope\":\"asset_editor\",\"asset_type\":\"%s\"}}"), *BoundaryCase.Key),
			BoundaryCase.Value,
			SpecialBoundaryJson);
	}

	const TArray<TPair<FString, FString>> ExcludedWindowBoundaryCases = {
		{ TEXT("\"asset_type\":\"widget_blueprint\""), TEXT("excluded_umg_designer_not_3d_editor_viewport") },
		{ TEXT("\"asset_type\":\"blueprint\",\"viewport_role\":\"graph\""), TEXT("excluded_graph_window_not_3d_editor_viewport") },
		{ TEXT("\"asset_type\":\"pcg_graph\""), TEXT("excluded_pcg_graph_not_3d_editor_viewport") },
		{ TEXT("\"asset_type\":\"behavior_tree\""), TEXT("excluded_behavior_tree_not_3d_editor_viewport") },
	};
	for (const TPair<FString, FString>& BoundaryCase : ExcludedWindowBoundaryCases)
	{
		TSharedPtr<FJsonObject> ExcludedBoundaryJson;
		ExecJsonExpectFailure(
			FString::Printf(TEXT("smoke-editor-viewport-excluded-window-%s"), *BoundaryCase.Value),
			TEXT("editor_viewport_get_state"),
			FString::Printf(TEXT("{\"target\":{\"scope\":\"asset_editor\",%s}}"), *BoundaryCase.Key),
			BoundaryCase.Value,
			ExcludedBoundaryJson);
	}

	TSharedPtr<FJsonObject> InitialStateJson;
	if (ExecJson(TEXT("smoke-editor-viewport-get-state-initial"), TEXT("editor_viewport_get_state"), TEXT("{}"), 200, InitialStateJson))
	{
		TestTrue(TEXT("Editor viewport get_state ok"), InitialStateJson->GetBoolField(TEXT("ok")));
		const TSharedPtr<FJsonObject> Data = GetData(TEXT("Editor viewport get_state"), InitialStateJson);
		if (Data.IsValid())
		{
			TestEqual(TEXT("Editor viewport state schema"), Data->GetStringField(TEXT("schema")), FString(TEXT("uai.editor_viewport.state.v1")));

			TSharedPtr<FJsonObject> CameraObj;
			TestTrue(TEXT("Editor viewport state camera object"), TryGetNestedObject(Data, TEXT("camera"), CameraObj));
			if (CameraObj.IsValid())
			{
				TestEqual(TEXT("Editor viewport camera space"), CameraObj->GetStringField(TEXT("space")), FString(TEXT("editor_world")));
				bHasOriginalCamera = ReadCamera(Data, OriginalCameraLocation, OriginalCameraRotation, OriginalCameraFov);
				TestTrue(TEXT("Editor viewport original camera readable"), bHasOriginalCamera);
			}

			TSharedPtr<FJsonObject> CapabilitiesObj;
			TestTrue(TEXT("Editor viewport state capabilities object"), TryGetNestedObject(Data, TEXT("capabilities"), CapabilitiesObj));
			if (CapabilitiesObj.IsValid())
			{
				TestTrue(TEXT("Editor viewport supports camera"), CapabilitiesObj->GetBoolField(TEXT("camera")));
				TestTrue(TEXT("Editor viewport supports visual options"), CapabilitiesObj->GetBoolField(TEXT("visual_options")));
				TestTrue(TEXT("Editor viewport supports screenshot"), CapabilitiesObj->GetBoolField(TEXT("screenshot")));
			}

			TSharedPtr<FJsonObject> VisualOptionsObj;
			TestTrue(TEXT("Editor viewport state visual options object"), TryGetNestedObject(Data, TEXT("visual_options"), VisualOptionsObj));
			if (VisualOptionsObj.IsValid())
			{
				VisualOptionsObj->TryGetStringField(TEXT("view_mode"), OriginalViewMode);
				VisualOptionsObj->TryGetBoolField(TEXT("realtime"), bOriginalRealtime);
				VisualOptionsObj->TryGetBoolField(TEXT("game_view"), bOriginalGameView);

				TSharedPtr<FJsonObject> OverlaysObj;
				if (TryGetNestedObject(VisualOptionsObj, TEXT("overlays"), OverlaysObj))
				{
					OverlaysObj->TryGetBoolField(TEXT("grid"), bOriginalGrid);
					OverlaysObj->TryGetBoolField(TEXT("bounds"), bOriginalBounds);
					OverlaysObj->TryGetBoolField(TEXT("pivot"), bOriginalPivot);
					OverlaysObj->TryGetBoolField(TEXT("selection"), bOriginalSelection);
					OverlaysObj->TryGetBoolField(TEXT("mode_widgets"), bOriginalModeWidgets);
					OverlaysObj->TryGetBoolField(TEXT("collision"), bOriginalCollision);
				}
			}
		}
	}

	const FVector RequestedCameraLocation(23500.0, 24500.0, 380.0);
	const FRotator RequestedCameraRotation(-22.0, 36.0, 0.0);
	const double RequestedCameraFov = 38.0;
	TSharedPtr<FJsonObject> SetCameraJson;
	if (ExecJson(
		TEXT("smoke-editor-viewport-set-camera"),
		TEXT("editor_viewport_set_camera"),
		FString::Printf(
			TEXT("{\"camera\":{\"location\":{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f},\"rotation\":{\"pitch\":%.3f,\"yaw\":%.3f,\"roll\":%.3f},\"fov\":%.3f}}"),
			RequestedCameraLocation.X,
			RequestedCameraLocation.Y,
			RequestedCameraLocation.Z,
			RequestedCameraRotation.Pitch,
			RequestedCameraRotation.Yaw,
			RequestedCameraRotation.Roll,
			RequestedCameraFov),
		200,
		SetCameraJson))
	{
		TestTrue(TEXT("Editor viewport set_camera ok"), SetCameraJson->GetBoolField(TEXT("ok")));
		const TSharedPtr<FJsonObject> Data = GetData(TEXT("Editor viewport set_camera"), SetCameraJson);
		FVector ReadLocation = FVector::ZeroVector;
		FRotator ReadRotation = FRotator::ZeroRotator;
		double ReadFov = 0.0;
		if (Data.IsValid() && ReadCamera(Data, ReadLocation, ReadRotation, ReadFov))
		{
			TestTrue(TEXT("Editor viewport set_camera location x"), FMath::IsNearlyEqual(ReadLocation.X, RequestedCameraLocation.X, 0.1));
			TestTrue(TEXT("Editor viewport set_camera location y"), FMath::IsNearlyEqual(ReadLocation.Y, RequestedCameraLocation.Y, 0.1));
			TestTrue(TEXT("Editor viewport set_camera location z"), FMath::IsNearlyEqual(ReadLocation.Z, RequestedCameraLocation.Z, 0.1));
			TestTrue(TEXT("Editor viewport set_camera pitch"), FMath::IsNearlyEqual(ReadRotation.Pitch, RequestedCameraRotation.Pitch, 0.1));
			TestTrue(TEXT("Editor viewport set_camera yaw"), FMath::IsNearlyEqual(ReadRotation.Yaw, RequestedCameraRotation.Yaw, 0.1));
			TestTrue(TEXT("Editor viewport set_camera fov"), FMath::IsNearlyEqual(ReadFov, RequestedCameraFov, 0.1));
		}
	}

	UWorld* EditorWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	UStaticMesh* CubeMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	const FString ProbeLabel = FString::Printf(TEXT("UAI_EditorViewport_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	AStaticMeshActor* ProbeActor = (EditorWorld && CubeMesh) ? EditorWorld->SpawnActor<AStaticMeshActor>(FVector(24150.0, 24450.0, 100.0), FRotator::ZeroRotator) : nullptr;
	TestNotNull(TEXT("Editor viewport probe actor spawned"), ProbeActor);
	if (ProbeActor && CubeMesh)
	{
		ProbeActor->SetActorLabel(ProbeLabel);
		ProbeActor->GetStaticMeshComponent()->SetStaticMesh(CubeMesh);
		ProbeActor->SetActorScale3D(FVector(0.4, 0.4, 0.4));
		ProbeActor->GetStaticMeshComponent()->UpdateBounds();
	}

	if (bCanRenderViewport && ProbeActor)
	{
		TSharedPtr<FJsonObject> FocusJson;
		if (ExecJson(
			TEXT("smoke-editor-viewport-focus-actor"),
			TEXT("editor_viewport_focus"),
			FString::Printf(TEXT("{\"focus\":{\"mode\":\"actor\",\"actor_label\":\"%s\",\"padding\":1.2,\"look_at\":true}}"), *ProbeLabel),
			200,
			FocusJson))
		{
			TestTrue(TEXT("Editor viewport focus ok"), FocusJson->GetBoolField(TEXT("ok")));
			const TSharedPtr<FJsonObject> Data = GetData(TEXT("Editor viewport focus"), FocusJson);
			if (Data.IsValid())
			{
				TestTrue(TEXT("Editor viewport focus_result object"), Data->HasTypedField<EJson::Object>(TEXT("focus_result")));
				TSharedPtr<FJsonObject> CameraObj;
				TestTrue(TEXT("Editor viewport focus camera object"), TryGetNestedObject(Data, TEXT("camera"), CameraObj));
				if (CameraObj.IsValid())
				{
					TestEqual(TEXT("Editor viewport focus camera space"), CameraObj->GetStringField(TEXT("space")), FString(TEXT("editor_world")));
				}
			}
		}
	}
	else
	{
		TSharedPtr<FJsonObject> FocusJson;
		if (ExecJson(
			TEXT("smoke-editor-viewport-focus-custom-point"),
			TEXT("editor_viewport_focus"),
			TEXT("{\"focus\":{\"mode\":\"custom_point\",\"point\":{\"x\":24150,\"y\":24450,\"z\":100},\"radius\":120}}"),
			200,
			FocusJson))
		{
			TestTrue(TEXT("Editor viewport headless focus ok"), FocusJson->GetBoolField(TEXT("ok")));
			const TSharedPtr<FJsonObject> Data = GetData(TEXT("Editor viewport headless focus"), FocusJson);
			if (Data.IsValid())
			{
				TestTrue(TEXT("Editor viewport headless focus_result object"), Data->HasTypedField<EJson::Object>(TEXT("focus_result")));
			}
		}
	}

	TSharedPtr<FJsonObject> LegacyViewportSetCameraJson;
	if (ExecJson(
		TEXT("smoke-editor-viewport-legacy-set-camera"),
		TEXT("viewport_set_camera"),
		TEXT("{\"location\":{\"x\":23650,\"y\":24650,\"z\":420},\"rotation\":{\"pitch\":-24,\"yaw\":42,\"roll\":0},\"fov\":41}"),
		200,
		LegacyViewportSetCameraJson))
	{
		TestTrue(TEXT("Legacy viewport set_camera ok"), LegacyViewportSetCameraJson->GetBoolField(TEXT("ok")));
		const TSharedPtr<FJsonObject> Data = GetData(TEXT("Legacy viewport set_camera"), LegacyViewportSetCameraJson);
		if (Data.IsValid())
		{
			TestTrue(TEXT("Legacy viewport set_camera wrapper"), Data->GetBoolField(TEXT("compat_wrapper")));
			TestEqual(TEXT("Legacy viewport set_camera replacement"), Data->GetStringField(TEXT("replacement_command")), FString(TEXT("editor_viewport_set_camera")));
			TestTrue(TEXT("Legacy viewport set_camera location field"), Data->HasTypedField<EJson::Object>(TEXT("location")));
			TestTrue(TEXT("Legacy viewport set_camera rotation field"), Data->HasTypedField<EJson::Object>(TEXT("rotation")));
			TestTrue(TEXT("Legacy viewport set_camera fov field"), Data->HasField(TEXT("fov")));
		}
	}

	TSharedPtr<FJsonObject> LegacyViewportGetCameraJson;
	if (ExecJson(TEXT("smoke-editor-viewport-legacy-get-camera"), TEXT("viewport_get_camera"), TEXT("{}"), 200, LegacyViewportGetCameraJson))
	{
		TestTrue(TEXT("Legacy viewport get_camera ok"), LegacyViewportGetCameraJson->GetBoolField(TEXT("ok")));
		const TSharedPtr<FJsonObject> Data = GetData(TEXT("Legacy viewport get_camera"), LegacyViewportGetCameraJson);
		if (Data.IsValid())
		{
			TestTrue(TEXT("Legacy viewport get_camera wrapper"), Data->GetBoolField(TEXT("compat_wrapper")));
			TestEqual(TEXT("Legacy viewport get_camera replacement"), Data->GetStringField(TEXT("replacement_command")), FString(TEXT("editor_viewport_get_state")));
			TestTrue(TEXT("Legacy viewport get_camera location field"), Data->HasTypedField<EJson::Object>(TEXT("location")));
			TestTrue(TEXT("Legacy viewport get_camera viewport field"), Data->HasTypedField<EJson::Object>(TEXT("viewport")));
		}
	}

	if (bCanRenderViewport && ProbeActor)
	{
		TSharedPtr<FJsonObject> LegacyFrameActorJson;
		if (ExecJson(
			TEXT("smoke-editor-viewport-legacy-frame-actor"),
			TEXT("viewport_frame_actor"),
			FString::Printf(TEXT("{\"id\":\"%s\",\"padding\":1.2,\"look_at\":true}"), *ProbeLabel),
			200,
			LegacyFrameActorJson))
		{
			TestTrue(TEXT("Legacy viewport frame_actor ok"), LegacyFrameActorJson->GetBoolField(TEXT("ok")));
			const TSharedPtr<FJsonObject> Data = GetData(TEXT("Legacy viewport frame_actor"), LegacyFrameActorJson);
			if (Data.IsValid())
			{
				TestTrue(TEXT("Legacy viewport frame_actor wrapper"), Data->GetBoolField(TEXT("compat_wrapper")));
				TestEqual(TEXT("Legacy viewport frame_actor replacement"), Data->GetStringField(TEXT("replacement_command")), FString(TEXT("editor_viewport_focus")));
				TestTrue(TEXT("Legacy viewport frame_actor focus result"), Data->HasTypedField<EJson::Object>(TEXT("focus_result")));
				TestTrue(TEXT("Legacy viewport frame_actor bounds"), Data->HasTypedField<EJson::Object>(TEXT("bounds")));
			}
		}
	}

	TSharedPtr<FJsonObject> VisualOptionsJson;
	if (ExecJson(
		TEXT("smoke-editor-viewport-visual-options"),
		TEXT("editor_viewport_set_visual_options"),
		TEXT("{\"visual_options\":{\"view_mode\":\"lit\",\"overlays\":{\"grid\":false,\"bounds\":true,\"pivot\":true,\"selection\":true,\"mode_widgets\":true},\"collision\":false}}"),
		200,
		VisualOptionsJson))
	{
		TestTrue(TEXT("Editor viewport visual options ok"), VisualOptionsJson->GetBoolField(TEXT("ok")));
		const TSharedPtr<FJsonObject> Data = GetData(TEXT("Editor viewport visual options"), VisualOptionsJson);
		TSharedPtr<FJsonObject> VisualObj;
		TSharedPtr<FJsonObject> OverlaysObj;
		if (Data.IsValid())
		{
			TestTrue(TEXT("Editor viewport visual_options object"), TryGetNestedObject(Data, TEXT("visual_options"), VisualObj));
		}
		if (TryGetNestedObject(VisualObj, TEXT("overlays"), OverlaysObj))
		{
			TestFalse(TEXT("Editor viewport visual grid disabled"), OverlaysObj->GetBoolField(TEXT("grid")));
			TestTrue(TEXT("Editor viewport visual bounds enabled"), OverlaysObj->GetBoolField(TEXT("bounds")));
			TestTrue(TEXT("Editor viewport visual pivot enabled"), OverlaysObj->GetBoolField(TEXT("pivot")));
		}
	}

	TSharedPtr<FJsonObject> PreviewStateJson;
	if (ExecJson(
		TEXT("smoke-editor-viewport-empty-preview-state"),
		TEXT("editor_viewport_set_preview_state"),
		TEXT("{\"preview_state\":{}}"),
		200,
		PreviewStateJson))
	{
		TestTrue(TEXT("Editor viewport empty preview state ok"), PreviewStateJson->GetBoolField(TEXT("ok")));
		const TSharedPtr<FJsonObject> Data = GetData(TEXT("Editor viewport empty preview state"), PreviewStateJson);
		TSharedPtr<FJsonObject> AppliedObj;
		TestTrue(TEXT("Editor viewport preview_state_applied object"), Data.IsValid() && TryGetNestedObject(Data, TEXT("preview_state_applied"), AppliedObj));
		if (AppliedObj.IsValid())
		{
			TestFalse(TEXT("Editor viewport empty preview state not applied"), AppliedObj->GetBoolField(TEXT("applied")));
			TestEqual(TEXT("Editor viewport empty preview state reason"), AppliedObj->GetStringField(TEXT("reason")), FString(TEXT("empty_preview_state")));
		}
	}

	TSharedPtr<FJsonObject> PreShotStateJson;
	FVector PreShotCameraLocation = FVector::ZeroVector;
	FRotator PreShotCameraRotation = FRotator::ZeroRotator;
	double PreShotCameraFov = 0.0;
	bool bHasPreShotCamera = false;
	if (ExecJson(TEXT("smoke-editor-viewport-get-state-before-shot"), TEXT("editor_viewport_get_state"), TEXT("{}"), 200, PreShotStateJson))
	{
		const TSharedPtr<FJsonObject> Data = GetData(TEXT("Editor viewport pre-shot state"), PreShotStateJson);
		bHasPreShotCamera = Data.IsValid() && ReadCamera(Data, PreShotCameraLocation, PreShotCameraRotation, PreShotCameraFov);
		TestTrue(TEXT("Editor viewport pre-shot camera readable"), bHasPreShotCamera);
	}

	const FString ScreenshotOutputFile = TEXT("runtimeLogs/editor_viewport_level_smoke.png");
	const FString ScreenshotParams = FString::Printf(
		TEXT("{\"output_file\":\"%s\",\"format\":\"png\",\"max_size\":128,\"warmup_frames\":0,\"restore_after_capture\":true,\"visual_options\":{\"view_mode\":\"unlit\",\"overlays\":{\"grid\":false,\"bounds\":true,\"pivot\":true}},\"camera\":{\"orbit\":{\"target\":{\"x\":24150,\"y\":24450,\"z\":100},\"yaw\":45,\"pitch\":-25,\"distance\":650,\"fov\":42}}}"),
		*ScreenshotOutputFile);
	TSharedPtr<FJsonObject> ScreenshotJson;
	if (ExecJson(
		TEXT("smoke-editor-viewport-screenshot"),
		TEXT("editor_viewport_screenshot"),
		ScreenshotParams,
		bCanRenderViewport ? 200 : 400,
		ScreenshotJson))
	{
		TestEqual(TEXT("Editor viewport screenshot ok matches render capability"), ScreenshotJson->GetBoolField(TEXT("ok")), bCanRenderViewport);
		const TSharedPtr<FJsonObject> Data = GetData(TEXT("Editor viewport screenshot"), ScreenshotJson);
		if (Data.IsValid() && bCanRenderViewport)
		{
			TestEqual(TEXT("Editor viewport screenshot schema"), Data->GetStringField(TEXT("schema")), FString(TEXT("uai.editor_viewport.screenshot.v1")));
			TestEqual(TEXT("Editor viewport screenshot backend"), Data->GetStringField(TEXT("capture_backend")), FString(TEXT("level_viewport_backbuffer")));
			TestTrue(TEXT("Editor viewport screenshot width positive"), Data->GetNumberField(TEXT("width")) > 0.0);
			TestTrue(TEXT("Editor viewport screenshot height positive"), Data->GetNumberField(TEXT("height")) > 0.0);
			const FString ScreenshotPath = Data->GetStringField(TEXT("file_path"));
			TestTrue(TEXT("Editor viewport screenshot file exists"), !ScreenshotPath.IsEmpty() && IFileManager::Get().FileExists(*ScreenshotPath));

			TSharedPtr<FJsonObject> PixelSignalObj;
			TestTrue(TEXT("Editor viewport screenshot pixel signal object"), TryGetNestedObject(Data, TEXT("pixel_signal"), PixelSignalObj));
			if (PixelSignalObj.IsValid())
			{
				TestTrue(TEXT("Editor viewport screenshot nonblank"), PixelSignalObj->GetBoolField(TEXT("nonblank")));
			}

			TSharedPtr<FJsonObject> RestoreObj;
			TestTrue(TEXT("Editor viewport screenshot restore_result object"), TryGetNestedObject(Data, TEXT("restore_result"), RestoreObj));
			if (RestoreObj.IsValid())
			{
				TestTrue(TEXT("Editor viewport screenshot restored"), RestoreObj->GetBoolField(TEXT("restored")));
				TestTrue(TEXT("Editor viewport screenshot restore succeeded"), RestoreObj->GetBoolField(TEXT("success")));
			}
		}
		else if (Data.IsValid())
		{
			const FString ErrorText = ScreenshotJson->GetStringField(TEXT("error"));
			TestTrue(TEXT("Editor viewport headless screenshot reports viewport size"), ErrorText.Contains(TEXT("invalid_viewport_size")));
			TSharedPtr<FJsonObject> RestoreObj;
			TestTrue(TEXT("Editor viewport headless screenshot restore_result object"), TryGetNestedObject(Data, TEXT("restore_result"), RestoreObj));
			if (RestoreObj.IsValid())
			{
				TestTrue(TEXT("Editor viewport headless screenshot restored after failure"), RestoreObj->GetBoolField(TEXT("restored")));
				TestTrue(TEXT("Editor viewport headless screenshot restore succeeded"), RestoreObj->GetBoolField(TEXT("success")));
			}
		}
	}

	if (bCanRenderViewport)
	{
		TSharedPtr<FJsonObject> LegacyScreenshotJson;
		if (ExecJson(
			TEXT("smoke-editor-viewport-legacy-screenshot-basic"),
			TEXT("screenshot_viewport"),
			TEXT("{\"format\":\"png\",\"max_size\":256,\"warmup_frames\":1}"),
			200,
			LegacyScreenshotJson))
		{
			TestTrue(TEXT("Legacy screenshot_viewport ok"), LegacyScreenshotJson->GetBoolField(TEXT("ok")));
			const TSharedPtr<FJsonObject> Data = GetData(TEXT("Legacy screenshot_viewport"), LegacyScreenshotJson);
			if (Data.IsValid())
			{
				TestTrue(TEXT("Legacy screenshot_viewport wrapper"), Data->GetBoolField(TEXT("compat_wrapper")));
				TestTrue(TEXT("Legacy screenshot_viewport partial migration"), Data->GetBoolField(TEXT("partial_migration")));
				TestEqual(TEXT("Legacy screenshot_viewport replacement"), Data->GetStringField(TEXT("replacement_command")), FString(TEXT("editor_viewport_screenshot")));
				TestEqual(TEXT("Legacy screenshot_viewport subpath"), Data->GetStringField(TEXT("compat_subpath")), FString(TEXT("basic_level_viewport_screenshot")));
				TestTrue(TEXT("Legacy screenshot_viewport file exists"), IFileManager::Get().FileExists(*Data->GetStringField(TEXT("file_path"))));
			}
		}
	}

	if (bHasPreShotCamera)
	{
		TSharedPtr<FJsonObject> PostShotStateJson;
		if (ExecJson(TEXT("smoke-editor-viewport-get-state-after-shot"), TEXT("editor_viewport_get_state"), TEXT("{}"), 200, PostShotStateJson))
		{
			const TSharedPtr<FJsonObject> Data = GetData(TEXT("Editor viewport post-shot state"), PostShotStateJson);
			FVector PostShotCameraLocation = FVector::ZeroVector;
			FRotator PostShotCameraRotation = FRotator::ZeroRotator;
			double PostShotCameraFov = 0.0;
			if (Data.IsValid() && ReadCamera(Data, PostShotCameraLocation, PostShotCameraRotation, PostShotCameraFov))
			{
				TestTrue(TEXT("Editor viewport screenshot restored camera x"), FMath::IsNearlyEqual(PostShotCameraLocation.X, PreShotCameraLocation.X, 0.1));
				TestTrue(TEXT("Editor viewport screenshot restored camera y"), FMath::IsNearlyEqual(PostShotCameraLocation.Y, PreShotCameraLocation.Y, 0.1));
				TestTrue(TEXT("Editor viewport screenshot restored camera z"), FMath::IsNearlyEqual(PostShotCameraLocation.Z, PreShotCameraLocation.Z, 0.1));
				TestTrue(TEXT("Editor viewport screenshot restored fov"), FMath::IsNearlyEqual(PostShotCameraFov, PreShotCameraFov, 0.1));
			}
		}
	}

	if (bHasOriginalCamera)
	{
		TSharedPtr<FJsonObject> RestoreCameraJson;
		ExecJson(
			TEXT("smoke-editor-viewport-restore-camera"),
			TEXT("editor_viewport_set_camera"),
			FString::Printf(
				TEXT("{\"camera\":{\"location\":{\"x\":%.6f,\"y\":%.6f,\"z\":%.6f},\"rotation\":{\"pitch\":%.6f,\"yaw\":%.6f,\"roll\":%.6f},\"fov\":%.6f}}"),
				OriginalCameraLocation.X,
				OriginalCameraLocation.Y,
				OriginalCameraLocation.Z,
				OriginalCameraRotation.Pitch,
				OriginalCameraRotation.Yaw,
				OriginalCameraRotation.Roll,
				OriginalCameraFov),
			200,
			RestoreCameraJson);
	}

	TSharedPtr<FJsonObject> RestoreVisualJson;
	ExecJson(
		TEXT("smoke-editor-viewport-restore-visual-options"),
		TEXT("editor_viewport_set_visual_options"),
		FString::Printf(
			TEXT("{\"visual_options\":{\"view_mode\":\"%s\",\"overlays\":{\"grid\":%s,\"bounds\":%s,\"pivot\":%s,\"selection\":%s,\"mode_widgets\":%s,\"collision\":%s},\"realtime\":%s,\"game_view\":%s}}"),
			*OriginalViewMode,
			BoolJson(bOriginalGrid),
			BoolJson(bOriginalBounds),
			BoolJson(bOriginalPivot),
			BoolJson(bOriginalSelection),
			BoolJson(bOriginalModeWidgets),
			BoolJson(bOriginalCollision),
			BoolJson(bOriginalRealtime),
			BoolJson(bOriginalGameView)),
		200,
		RestoreVisualJson);

	const FString StaticMeshAssetPath = TEXT("/Engine/BasicShapes/Cube.Cube");
	const FString StaticMeshTargetParams = FString::Printf(
		TEXT("\"target\":{\"scope\":\"asset_editor\",\"asset_type\":\"static_mesh\",\"asset_path\":\"%s\",\"open_editor_if_needed\":true}"),
		*StaticMeshAssetPath);
	FVector StaticMeshOriginalCameraLocation = FVector::ZeroVector;
	FRotator StaticMeshOriginalCameraRotation = FRotator::ZeroRotator;
	double StaticMeshOriginalCameraFov = 90.0;
	bool bHasStaticMeshOriginalCamera = false;
	FString StaticMeshOriginalViewMode = TEXT("lit");
	bool bStaticMeshOriginalGrid = true;
	bool bStaticMeshOriginalBounds = false;
	bool bStaticMeshOriginalPivot = false;
	bool bStaticMeshOriginalNormals = false;
	bool bStaticMeshOriginalTangents = false;
	bool bStaticMeshOriginalBinormals = false;
	bool bStaticMeshOriginalVertices = false;
	bool bStaticMeshOriginalSimpleCollision = false;
	bool bStaticMeshOriginalComplexCollision = false;
	bool bStaticMeshOriginalRealtime = false;

	if (bCanRenderViewport)
	{
		TSharedPtr<FJsonObject> StaticMeshInitialStateJson;
		if (ExecJson(
			TEXT("smoke-editor-viewport-staticmesh-get-state"),
			TEXT("editor_viewport_get_state"),
			FString::Printf(TEXT("{%s}"), *StaticMeshTargetParams),
			200,
			StaticMeshInitialStateJson))
		{
			TestTrue(TEXT("StaticMesh viewport get_state ok"), StaticMeshInitialStateJson->GetBoolField(TEXT("ok")));
			const TSharedPtr<FJsonObject> Data = GetData(TEXT("StaticMesh viewport get_state"), StaticMeshInitialStateJson);
			if (Data.IsValid())
			{
				TestEqual(TEXT("StaticMesh viewport state schema"), Data->GetStringField(TEXT("schema")), FString(TEXT("uai.editor_viewport.state.v1")));
				TestEqual(TEXT("StaticMesh viewport adapter"), Data->GetStringField(TEXT("adapter")), FString(TEXT("static_mesh")));

				TSharedPtr<FJsonObject> CameraObj;
				TestTrue(TEXT("StaticMesh viewport camera object"), TryGetNestedObject(Data, TEXT("camera"), CameraObj));
				if (CameraObj.IsValid())
				{
					TestEqual(TEXT("StaticMesh viewport camera space"), CameraObj->GetStringField(TEXT("space")), FString(TEXT("asset_preview_world")));
					bHasStaticMeshOriginalCamera = ReadCamera(Data, StaticMeshOriginalCameraLocation, StaticMeshOriginalCameraRotation, StaticMeshOriginalCameraFov);
					TestTrue(TEXT("StaticMesh viewport original camera readable"), bHasStaticMeshOriginalCamera);
				}

				TSharedPtr<FJsonObject> CapabilitiesObj;
				TestTrue(TEXT("StaticMesh viewport capabilities object"), TryGetNestedObject(Data, TEXT("capabilities"), CapabilitiesObj));
				if (CapabilitiesObj.IsValid())
				{
					TestTrue(TEXT("StaticMesh viewport supports camera"), CapabilitiesObj->GetBoolField(TEXT("camera")));
					TestTrue(TEXT("StaticMesh viewport supports visual options"), CapabilitiesObj->GetBoolField(TEXT("visual_options")));
					TestTrue(TEXT("StaticMesh viewport supports screenshot"), CapabilitiesObj->GetBoolField(TEXT("screenshot")));
					TestTrue(TEXT("StaticMesh viewport supports focus pivot"), ArrayHasString(CapabilitiesObj, TEXT("focus"), TEXT("pivot")));
					TestTrue(TEXT("StaticMesh viewport supports focus asset_bounds"), ArrayHasString(CapabilitiesObj, TEXT("focus"), TEXT("asset_bounds")));
				}

				TSharedPtr<FJsonObject> PreviewStateObj;
				TestTrue(TEXT("StaticMesh viewport preview state object"), TryGetNestedObject(Data, TEXT("preview_state"), PreviewStateObj));
				if (PreviewStateObj.IsValid())
				{
					TestEqual(TEXT("StaticMesh viewport preview asset path"), PreviewStateObj->GetStringField(TEXT("asset_path")), StaticMeshAssetPath);
					TestTrue(TEXT("StaticMesh viewport lod index nonnegative"), PreviewStateObj->GetNumberField(TEXT("lod_index")) >= 0.0);
				}

				TSharedPtr<FJsonObject> VisualOptionsObj;
				TestTrue(TEXT("StaticMesh viewport visual options object"), TryGetNestedObject(Data, TEXT("visual_options"), VisualOptionsObj));
				if (VisualOptionsObj.IsValid())
				{
					VisualOptionsObj->TryGetStringField(TEXT("view_mode"), StaticMeshOriginalViewMode);
					VisualOptionsObj->TryGetBoolField(TEXT("realtime"), bStaticMeshOriginalRealtime);

					TSharedPtr<FJsonObject> OverlaysObj;
					if (TryGetNestedObject(VisualOptionsObj, TEXT("overlays"), OverlaysObj))
					{
						OverlaysObj->TryGetBoolField(TEXT("grid"), bStaticMeshOriginalGrid);
						OverlaysObj->TryGetBoolField(TEXT("bounds"), bStaticMeshOriginalBounds);
						OverlaysObj->TryGetBoolField(TEXT("pivot"), bStaticMeshOriginalPivot);
						OverlaysObj->TryGetBoolField(TEXT("normals"), bStaticMeshOriginalNormals);
						OverlaysObj->TryGetBoolField(TEXT("tangents"), bStaticMeshOriginalTangents);
						OverlaysObj->TryGetBoolField(TEXT("binormals"), bStaticMeshOriginalBinormals);
						OverlaysObj->TryGetBoolField(TEXT("vertices"), bStaticMeshOriginalVertices);
						OverlaysObj->TryGetBoolField(TEXT("simple_collision"), bStaticMeshOriginalSimpleCollision);
						OverlaysObj->TryGetBoolField(TEXT("complex_collision"), bStaticMeshOriginalComplexCollision);
					}
				}
			}
		}

		TSharedPtr<FJsonObject> StaticMeshCameraJson;
		if (ExecJson(
			TEXT("smoke-editor-viewport-staticmesh-set-camera"),
			TEXT("editor_viewport_set_camera"),
			FString::Printf(
				TEXT("{%s,\"camera\":{\"orbit\":{\"target\":{\"x\":0,\"y\":0,\"z\":0},\"yaw\":35,\"pitch\":-22,\"distance\":420,\"fov\":40}}}"),
				*StaticMeshTargetParams),
			200,
			StaticMeshCameraJson))
		{
			TestTrue(TEXT("StaticMesh viewport set_camera ok"), StaticMeshCameraJson->GetBoolField(TEXT("ok")));
			const TSharedPtr<FJsonObject> Data = GetData(TEXT("StaticMesh viewport set_camera"), StaticMeshCameraJson);
			TSharedPtr<FJsonObject> CameraObj;
			if (Data.IsValid() && TryGetNestedObject(Data, TEXT("camera"), CameraObj))
			{
				TestEqual(TEXT("StaticMesh viewport set_camera space"), CameraObj->GetStringField(TEXT("space")), FString(TEXT("asset_preview_world")));
				TestTrue(TEXT("StaticMesh viewport set_camera fov"), FMath::IsNearlyEqual(CameraObj->GetNumberField(TEXT("fov")), 40.0, 0.1));
			}
		}

		TSharedPtr<FJsonObject> StaticMeshLegacyPreviewJson;
		if (ExecJson(
			TEXT("smoke-editor-viewport-staticmesh-legacy-preview-view"),
			TEXT("static_mesh_set_preview_view"),
			FString::Printf(TEXT("{\"asset_path\":\"%s\",\"yaw\":45,\"pitch\":-24,\"distance\":430,\"fov\":42,\"open_editor_if_needed\":true}"), *StaticMeshAssetPath),
			200,
			StaticMeshLegacyPreviewJson))
		{
			TestTrue(TEXT("StaticMesh legacy preview view ok"), StaticMeshLegacyPreviewJson->GetBoolField(TEXT("ok")));
			const TSharedPtr<FJsonObject> Data = GetData(TEXT("StaticMesh legacy preview view"), StaticMeshLegacyPreviewJson);
			if (Data.IsValid())
			{
				TestTrue(TEXT("StaticMesh legacy preview wrapper"), Data->GetBoolField(TEXT("compat_wrapper")));
				TestEqual(TEXT("StaticMesh legacy preview replacement"), Data->GetStringField(TEXT("replacement_command")), FString(TEXT("editor_viewport_set_camera")));
				TestTrue(TEXT("StaticMesh legacy preview target field"), Data->HasTypedField<EJson::Object>(TEXT("target")));
				TestTrue(TEXT("StaticMesh legacy preview location field"), Data->HasTypedField<EJson::Object>(TEXT("location")));
				TestTrue(TEXT("StaticMesh legacy preview fov"), FMath::IsNearlyEqual(Data->GetNumberField(TEXT("fov")), 42.0, 0.1));
			}
		}

		TSharedPtr<FJsonObject> StaticMeshFocusJson;
		if (ExecJson(
			TEXT("smoke-editor-viewport-staticmesh-focus-pivot"),
			TEXT("editor_viewport_focus"),
			FString::Printf(
				TEXT("{%s,\"focus\":{\"mode\":\"pivot\",\"radius\":25}}"),
				*StaticMeshTargetParams),
			200,
			StaticMeshFocusJson))
		{
			TestTrue(TEXT("StaticMesh viewport focus ok"), StaticMeshFocusJson->GetBoolField(TEXT("ok")));
			const TSharedPtr<FJsonObject> Data = GetData(TEXT("StaticMesh viewport focus"), StaticMeshFocusJson);
			TSharedPtr<FJsonObject> FocusResultObj;
			TestTrue(TEXT("StaticMesh viewport focus result object"), Data.IsValid() && TryGetNestedObject(Data, TEXT("focus_result"), FocusResultObj));
			if (FocusResultObj.IsValid())
			{
				TestEqual(TEXT("StaticMesh viewport focus mode"), FocusResultObj->GetStringField(TEXT("mode")), FString(TEXT("pivot")));
				TestEqual(TEXT("StaticMesh viewport focus bounds space"), FocusResultObj->GetStringField(TEXT("bounds_space")), FString(TEXT("asset_local")));
				TestTrue(TEXT("StaticMesh viewport focus bounds object"), FocusResultObj->HasTypedField<EJson::Object>(TEXT("bounds")));
			}
		}

		TSharedPtr<FJsonObject> StaticMeshVisualJson;
		if (ExecJson(
			TEXT("smoke-editor-viewport-staticmesh-visual-options"),
			TEXT("editor_viewport_set_visual_options"),
			FString::Printf(
				TEXT("{%s,\"visual_options\":{\"view_mode\":\"lit\",\"overlays\":{\"grid\":false,\"bounds\":true,\"pivot\":true,\"normals\":true,\"tangents\":true,\"binormals\":true},\"collision\":false}}"),
				*StaticMeshTargetParams),
			200,
			StaticMeshVisualJson))
		{
			TestTrue(TEXT("StaticMesh viewport visual options ok"), StaticMeshVisualJson->GetBoolField(TEXT("ok")));
			const TSharedPtr<FJsonObject> Data = GetData(TEXT("StaticMesh viewport visual options"), StaticMeshVisualJson);
			TSharedPtr<FJsonObject> VisualObj;
			TSharedPtr<FJsonObject> OverlaysObj;
			if (Data.IsValid())
			{
				TestTrue(TEXT("StaticMesh viewport visual_options object"), TryGetNestedObject(Data, TEXT("visual_options"), VisualObj));
			}
			if (TryGetNestedObject(VisualObj, TEXT("overlays"), OverlaysObj))
			{
				TestFalse(TEXT("StaticMesh viewport visual grid disabled"), OverlaysObj->GetBoolField(TEXT("grid")));
				TestTrue(TEXT("StaticMesh viewport visual bounds enabled"), OverlaysObj->GetBoolField(TEXT("bounds")));
				TestTrue(TEXT("StaticMesh viewport visual pivot enabled"), OverlaysObj->GetBoolField(TEXT("pivot")));
				TestTrue(TEXT("StaticMesh viewport visual normals enabled"), OverlaysObj->GetBoolField(TEXT("normals")));
				TestTrue(TEXT("StaticMesh viewport visual tangents enabled"), OverlaysObj->GetBoolField(TEXT("tangents")));
				TestTrue(TEXT("StaticMesh viewport visual binormals enabled"), OverlaysObj->GetBoolField(TEXT("binormals")));
				TestFalse(TEXT("StaticMesh viewport visual collision disabled"), OverlaysObj->GetBoolField(TEXT("collision")));
			}
		}

		TSharedPtr<FJsonObject> StaticMeshPreviewStateJson;
		if (ExecJson(
			TEXT("smoke-editor-viewport-staticmesh-empty-preview-state"),
			TEXT("editor_viewport_set_preview_state"),
			FString::Printf(TEXT("{%s,\"preview_state\":{}}"), *StaticMeshTargetParams),
			200,
			StaticMeshPreviewStateJson))
		{
			TestTrue(TEXT("StaticMesh viewport empty preview state ok"), StaticMeshPreviewStateJson->GetBoolField(TEXT("ok")));
			const TSharedPtr<FJsonObject> Data = GetData(TEXT("StaticMesh viewport empty preview state"), StaticMeshPreviewStateJson);
			TSharedPtr<FJsonObject> AppliedObj;
			TestTrue(TEXT("StaticMesh viewport preview_state_applied object"), Data.IsValid() && TryGetNestedObject(Data, TEXT("preview_state_applied"), AppliedObj));
			if (AppliedObj.IsValid())
			{
				TestFalse(TEXT("StaticMesh viewport empty preview state not applied"), AppliedObj->GetBoolField(TEXT("applied")));
				TestEqual(TEXT("StaticMesh viewport empty preview state reason"), AppliedObj->GetStringField(TEXT("reason")), FString(TEXT("empty_preview_state")));
			}
		}

		TSharedPtr<FJsonObject> StaticMeshPreShotStateJson;
		FVector StaticMeshPreShotCameraLocation = FVector::ZeroVector;
		FRotator StaticMeshPreShotCameraRotation = FRotator::ZeroRotator;
		double StaticMeshPreShotCameraFov = 0.0;
		bool bHasStaticMeshPreShotCamera = false;
		if (ExecJson(
			TEXT("smoke-editor-viewport-staticmesh-get-state-before-shot"),
			TEXT("editor_viewport_get_state"),
			FString::Printf(TEXT("{%s}"), *StaticMeshTargetParams),
			200,
			StaticMeshPreShotStateJson))
		{
			const TSharedPtr<FJsonObject> Data = GetData(TEXT("StaticMesh viewport pre-shot state"), StaticMeshPreShotStateJson);
			bHasStaticMeshPreShotCamera = Data.IsValid() && ReadCamera(Data, StaticMeshPreShotCameraLocation, StaticMeshPreShotCameraRotation, StaticMeshPreShotCameraFov);
			TestTrue(TEXT("StaticMesh viewport pre-shot camera readable"), bHasStaticMeshPreShotCamera);
		}

		const FString StaticMeshScreenshotOutputFile = TEXT("runtimeLogs/editor_viewport_static_mesh_smoke.png");
		TSharedPtr<FJsonObject> StaticMeshScreenshotJson;
		if (ExecJson(
			TEXT("smoke-editor-viewport-staticmesh-screenshot"),
			TEXT("editor_viewport_screenshot"),
			FString::Printf(
				TEXT("{%s,\"output_file\":\"%s\",\"format\":\"png\",\"max_size\":128,\"warmup_frames\":0,\"restore_after_capture\":true,\"visual_options\":{\"view_mode\":\"unlit\",\"overlays\":{\"grid\":false,\"bounds\":true,\"pivot\":true,\"normals\":false,\"tangents\":false,\"binormals\":false},\"collision\":false},\"focus\":{\"mode\":\"asset_bounds\"},\"camera\":{\"orbit\":{\"target\":{\"x\":0,\"y\":0,\"z\":0},\"yaw\":45,\"pitch\":-25,\"distance\":460,\"fov\":42}}}"),
				*StaticMeshTargetParams,
				*StaticMeshScreenshotOutputFile),
			200,
			StaticMeshScreenshotJson))
		{
			TestTrue(TEXT("StaticMesh viewport screenshot ok"), StaticMeshScreenshotJson->GetBoolField(TEXT("ok")));
			const TSharedPtr<FJsonObject> Data = GetData(TEXT("StaticMesh viewport screenshot"), StaticMeshScreenshotJson);
			if (Data.IsValid())
			{
				TestEqual(TEXT("StaticMesh viewport screenshot schema"), Data->GetStringField(TEXT("schema")), FString(TEXT("uai.editor_viewport.screenshot.v1")));
				TestEqual(TEXT("StaticMesh viewport screenshot backend"), Data->GetStringField(TEXT("capture_backend")), FString(TEXT("static_mesh_editor_backbuffer")));
				TestTrue(TEXT("StaticMesh viewport screenshot width positive"), Data->GetNumberField(TEXT("width")) > 0.0);
				TestTrue(TEXT("StaticMesh viewport screenshot height positive"), Data->GetNumberField(TEXT("height")) > 0.0);
				const FString ScreenshotPath = Data->GetStringField(TEXT("file_path"));
				TestTrue(TEXT("StaticMesh viewport screenshot file exists"), !ScreenshotPath.IsEmpty() && IFileManager::Get().FileExists(*ScreenshotPath));

				TSharedPtr<FJsonObject> PixelSignalObj;
				TestTrue(TEXT("StaticMesh viewport screenshot pixel signal object"), TryGetNestedObject(Data, TEXT("pixel_signal"), PixelSignalObj));
				if (PixelSignalObj.IsValid())
				{
					TestTrue(TEXT("StaticMesh viewport screenshot nonblank"), PixelSignalObj->GetBoolField(TEXT("nonblank")));
				}

				TSharedPtr<FJsonObject> RestoreObj;
				TestTrue(TEXT("StaticMesh viewport screenshot restore_result object"), TryGetNestedObject(Data, TEXT("restore_result"), RestoreObj));
				if (RestoreObj.IsValid())
				{
					TestTrue(TEXT("StaticMesh viewport screenshot restored"), RestoreObj->GetBoolField(TEXT("restored")));
					TestTrue(TEXT("StaticMesh viewport screenshot restore succeeded"), RestoreObj->GetBoolField(TEXT("success")));
				}
			}
		}

		if (bHasStaticMeshPreShotCamera)
		{
			TSharedPtr<FJsonObject> StaticMeshPostShotStateJson;
			if (ExecJson(
				TEXT("smoke-editor-viewport-staticmesh-get-state-after-shot"),
				TEXT("editor_viewport_get_state"),
				FString::Printf(TEXT("{%s}"), *StaticMeshTargetParams),
				200,
				StaticMeshPostShotStateJson))
			{
				const TSharedPtr<FJsonObject> Data = GetData(TEXT("StaticMesh viewport post-shot state"), StaticMeshPostShotStateJson);
				FVector StaticMeshPostShotCameraLocation = FVector::ZeroVector;
				FRotator StaticMeshPostShotCameraRotation = FRotator::ZeroRotator;
				double StaticMeshPostShotCameraFov = 0.0;
				if (Data.IsValid() && ReadCamera(Data, StaticMeshPostShotCameraLocation, StaticMeshPostShotCameraRotation, StaticMeshPostShotCameraFov))
				{
					TestTrue(TEXT("StaticMesh viewport screenshot restored camera x"), FMath::IsNearlyEqual(StaticMeshPostShotCameraLocation.X, StaticMeshPreShotCameraLocation.X, 0.1));
					TestTrue(TEXT("StaticMesh viewport screenshot restored camera y"), FMath::IsNearlyEqual(StaticMeshPostShotCameraLocation.Y, StaticMeshPreShotCameraLocation.Y, 0.1));
					TestTrue(TEXT("StaticMesh viewport screenshot restored camera z"), FMath::IsNearlyEqual(StaticMeshPostShotCameraLocation.Z, StaticMeshPreShotCameraLocation.Z, 0.1));
					TestTrue(TEXT("StaticMesh viewport screenshot restored fov"), FMath::IsNearlyEqual(StaticMeshPostShotCameraFov, StaticMeshPreShotCameraFov, 0.1));
				}
			}
		}

		if (bHasStaticMeshOriginalCamera)
		{
			TSharedPtr<FJsonObject> StaticMeshRestoreCameraJson;
			ExecJson(
				TEXT("smoke-editor-viewport-staticmesh-restore-camera"),
				TEXT("editor_viewport_set_camera"),
				FString::Printf(
					TEXT("{%s,\"camera\":{\"location\":{\"x\":%.6f,\"y\":%.6f,\"z\":%.6f},\"rotation\":{\"pitch\":%.6f,\"yaw\":%.6f,\"roll\":%.6f},\"fov\":%.6f}}"),
					*StaticMeshTargetParams,
					StaticMeshOriginalCameraLocation.X,
					StaticMeshOriginalCameraLocation.Y,
					StaticMeshOriginalCameraLocation.Z,
					StaticMeshOriginalCameraRotation.Pitch,
					StaticMeshOriginalCameraRotation.Yaw,
					StaticMeshOriginalCameraRotation.Roll,
					StaticMeshOriginalCameraFov),
				200,
				StaticMeshRestoreCameraJson);
		}

		TSharedPtr<FJsonObject> StaticMeshRestoreVisualJson;
		ExecJson(
			TEXT("smoke-editor-viewport-staticmesh-restore-visual-options"),
			TEXT("editor_viewport_set_visual_options"),
			FString::Printf(
				TEXT("{%s,\"visual_options\":{\"view_mode\":\"%s\",\"overlays\":{\"grid\":%s,\"bounds\":%s,\"pivot\":%s,\"normals\":%s,\"tangents\":%s,\"binormals\":%s,\"vertices\":%s,\"simple_collision\":%s,\"complex_collision\":%s},\"realtime\":%s}}"),
				*StaticMeshTargetParams,
				*StaticMeshOriginalViewMode,
				BoolJson(bStaticMeshOriginalGrid),
				BoolJson(bStaticMeshOriginalBounds),
				BoolJson(bStaticMeshOriginalPivot),
				BoolJson(bStaticMeshOriginalNormals),
				BoolJson(bStaticMeshOriginalTangents),
				BoolJson(bStaticMeshOriginalBinormals),
				BoolJson(bStaticMeshOriginalVertices),
				BoolJson(bStaticMeshOriginalSimpleCollision),
				BoolJson(bStaticMeshOriginalComplexCollision),
				BoolJson(bStaticMeshOriginalRealtime)),
			200,
			StaticMeshRestoreVisualJson);
	}

	const FString MaterialAssetPath = TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial");
	UMaterial* SmokeMaterial = LoadObject<UMaterial>(nullptr, *MaterialAssetPath);
	TestNotNull(TEXT("Material viewport smoke asset"), SmokeMaterial);

	if (bCanRenderViewport && SmokeMaterial)
	{
		const FString MaterialTargetParams = FString::Printf(
			TEXT("\"target\":{\"scope\":\"asset_editor\",\"asset_type\":\"material\",\"asset_path\":\"%s\",\"open_editor_if_needed\":true}"),
			*MaterialAssetPath);
		FVector MaterialOriginalCameraLocation = FVector::ZeroVector;
		FRotator MaterialOriginalCameraRotation = FRotator::ZeroRotator;
		double MaterialOriginalCameraFov = 90.0;
		bool bHasMaterialOriginalCamera = false;
		FString MaterialOriginalViewMode = TEXT("lit");
		bool bMaterialOriginalGrid = false;
		bool bMaterialOriginalBounds = false;
		bool bMaterialOriginalRealtime = false;
		FString MaterialOriginalPreviewPrimitive = TEXT("sphere");
		bool bMaterialOriginalBackground = true;
		bool bMaterialOriginalPostProcess = false;

		TSharedPtr<FJsonObject> MaterialStateJson;
		if (ExecJson(
			TEXT("smoke-editor-viewport-material-get-state"),
			TEXT("editor_viewport_get_state"),
			FString::Printf(TEXT("{%s}"), *MaterialTargetParams),
			200,
			MaterialStateJson))
		{
			TestTrue(TEXT("Material viewport get_state ok"), MaterialStateJson->GetBoolField(TEXT("ok")));
			const TSharedPtr<FJsonObject> Data = GetData(TEXT("Material viewport get_state"), MaterialStateJson);
			if (Data.IsValid())
			{
				TestEqual(TEXT("Material viewport adapter"), Data->GetStringField(TEXT("adapter")), FString(TEXT("material")));
				TSharedPtr<FJsonObject> CameraObj;
				TestTrue(TEXT("Material viewport camera object"), TryGetNestedObject(Data, TEXT("camera"), CameraObj));
				if (CameraObj.IsValid())
				{
					TestEqual(TEXT("Material viewport camera space"), CameraObj->GetStringField(TEXT("space")), FString(TEXT("material_preview_world")));
					bHasMaterialOriginalCamera = ReadCamera(Data, MaterialOriginalCameraLocation, MaterialOriginalCameraRotation, MaterialOriginalCameraFov);
					TestTrue(TEXT("Material viewport original camera readable"), bHasMaterialOriginalCamera);
				}

				TSharedPtr<FJsonObject> CapabilitiesObj;
				TestTrue(TEXT("Material viewport capabilities object"), TryGetNestedObject(Data, TEXT("capabilities"), CapabilitiesObj));
				if (CapabilitiesObj.IsValid())
				{
					TestTrue(TEXT("Material viewport supports camera"), CapabilitiesObj->GetBoolField(TEXT("camera")));
					TestTrue(TEXT("Material viewport supports visual options"), CapabilitiesObj->GetBoolField(TEXT("visual_options")));
					TestTrue(TEXT("Material viewport supports preview state"), CapabilitiesObj->GetBoolField(TEXT("preview_state")));
					TestTrue(TEXT("Material viewport supports screenshot"), CapabilitiesObj->GetBoolField(TEXT("screenshot")));
					TestTrue(TEXT("Material viewport supports preview bounds focus"), ArrayHasString(CapabilitiesObj, TEXT("focus"), TEXT("preview_component_bounds")));
					TestTrue(TEXT("Material viewport supports preview primitive"), ArrayHasString(CapabilitiesObj, TEXT("preview_state_keys"), TEXT("preview_primitive")));
					TestTrue(TEXT("Material viewport supports background state"), ArrayHasString(CapabilitiesObj, TEXT("preview_state_keys"), TEXT("background")));
				}

				TSharedPtr<FJsonObject> PreviewStateObj;
				TestTrue(TEXT("Material viewport preview_state object"), TryGetNestedObject(Data, TEXT("preview_state"), PreviewStateObj));
				if (PreviewStateObj.IsValid())
				{
					TestEqual(TEXT("Material viewport preview context"), PreviewStateObj->GetStringField(TEXT("context")), FString(TEXT("material_editor")));
					TestEqual(TEXT("Material viewport asset path"), PreviewStateObj->GetStringField(TEXT("asset_path")), MaterialAssetPath);
					TestTrue(TEXT("Material viewport preview component present"), PreviewStateObj->GetBoolField(TEXT("has_preview_component")));
					TestTrue(TEXT("Material viewport preview component bounds object"), PreviewStateObj->HasTypedField<EJson::Object>(TEXT("preview_component_bounds")));
					PreviewStateObj->TryGetStringField(TEXT("preview_primitive"), MaterialOriginalPreviewPrimitive);
					PreviewStateObj->TryGetBoolField(TEXT("background"), bMaterialOriginalBackground);
					PreviewStateObj->TryGetBoolField(TEXT("post_process_material_preview"), bMaterialOriginalPostProcess);
				}

				TSharedPtr<FJsonObject> VisualOptionsObj;
				TestTrue(TEXT("Material viewport visual_options object"), TryGetNestedObject(Data, TEXT("visual_options"), VisualOptionsObj));
				if (VisualOptionsObj.IsValid())
				{
					VisualOptionsObj->TryGetStringField(TEXT("view_mode"), MaterialOriginalViewMode);
					VisualOptionsObj->TryGetBoolField(TEXT("realtime"), bMaterialOriginalRealtime);
					TSharedPtr<FJsonObject> OverlaysObj;
					if (TryGetNestedObject(VisualOptionsObj, TEXT("overlays"), OverlaysObj))
					{
						OverlaysObj->TryGetBoolField(TEXT("grid"), bMaterialOriginalGrid);
						OverlaysObj->TryGetBoolField(TEXT("bounds"), bMaterialOriginalBounds);
					}
				}
			}
		}

		TSharedPtr<FJsonObject> MaterialCameraJson;
		if (ExecJson(
			TEXT("smoke-editor-viewport-material-set-camera"),
			TEXT("editor_viewport_set_camera"),
			FString::Printf(TEXT("{%s,\"camera\":{\"orbit\":{\"target\":{\"x\":0,\"y\":0,\"z\":0},\"yaw\":35,\"pitch\":-20,\"distance\":280,\"fov\":38}}}"), *MaterialTargetParams),
			200,
			MaterialCameraJson))
		{
			TestTrue(TEXT("Material viewport set_camera ok"), MaterialCameraJson->GetBoolField(TEXT("ok")));
			const TSharedPtr<FJsonObject> Data = GetData(TEXT("Material viewport set_camera"), MaterialCameraJson);
			TSharedPtr<FJsonObject> CameraObj;
			if (Data.IsValid() && TryGetNestedObject(Data, TEXT("camera"), CameraObj))
			{
				TestEqual(TEXT("Material viewport set_camera space"), CameraObj->GetStringField(TEXT("space")), FString(TEXT("material_preview_world")));
				TestTrue(TEXT("Material viewport set_camera fov"), FMath::IsNearlyEqual(CameraObj->GetNumberField(TEXT("fov")), 38.0, 0.1));
			}
		}

		TSharedPtr<FJsonObject> MaterialFocusJson;
		if (ExecJson(
			TEXT("smoke-editor-viewport-material-focus-bounds"),
			TEXT("editor_viewport_focus"),
			FString::Printf(TEXT("{%s,\"focus\":{\"mode\":\"preview_component_bounds\",\"padding\":1.1,\"instant\":true}}"), *MaterialTargetParams),
			200,
			MaterialFocusJson))
		{
			TestTrue(TEXT("Material viewport focus ok"), MaterialFocusJson->GetBoolField(TEXT("ok")));
			const TSharedPtr<FJsonObject> Data = GetData(TEXT("Material viewport focus"), MaterialFocusJson);
			TSharedPtr<FJsonObject> FocusResultObj;
			TestTrue(TEXT("Material viewport focus result object"), Data.IsValid() && TryGetNestedObject(Data, TEXT("focus_result"), FocusResultObj));
			if (FocusResultObj.IsValid())
			{
				TestEqual(TEXT("Material viewport focus mode"), FocusResultObj->GetStringField(TEXT("mode")), FString(TEXT("preview_component_bounds")));
				TestEqual(TEXT("Material viewport focus bounds space"), FocusResultObj->GetStringField(TEXT("bounds_space")), FString(TEXT("material_preview_world")));
				TestTrue(TEXT("Material viewport focus bounds object"), FocusResultObj->HasTypedField<EJson::Object>(TEXT("bounds")));
			}
		}

		TSharedPtr<FJsonObject> MaterialVisualJson;
		if (ExecJson(
			TEXT("smoke-editor-viewport-material-visual-options"),
			TEXT("editor_viewport_set_visual_options"),
			FString::Printf(TEXT("{%s,\"visual_options\":{\"view_mode\":\"lit\",\"overlays\":{\"grid\":false,\"bounds\":true},\"realtime\":false}}"), *MaterialTargetParams),
			200,
			MaterialVisualJson))
		{
			TestTrue(TEXT("Material viewport visual options ok"), MaterialVisualJson->GetBoolField(TEXT("ok")));
			const TSharedPtr<FJsonObject> Data = GetData(TEXT("Material viewport visual options"), MaterialVisualJson);
			TSharedPtr<FJsonObject> VisualObj;
			TSharedPtr<FJsonObject> OverlaysObj;
			if (Data.IsValid() && TryGetNestedObject(Data, TEXT("visual_options"), VisualObj) && TryGetNestedObject(VisualObj, TEXT("overlays"), OverlaysObj))
			{
				TestFalse(TEXT("Material viewport visual grid disabled"), OverlaysObj->GetBoolField(TEXT("grid")));
				TestTrue(TEXT("Material viewport visual bounds enabled"), OverlaysObj->GetBoolField(TEXT("bounds")));
			}
		}

		TSharedPtr<FJsonObject> MaterialPreviewStateJson;
		if (ExecJson(
			TEXT("smoke-editor-viewport-material-preview-state"),
			TEXT("editor_viewport_set_preview_state"),
			FString::Printf(TEXT("{%s,\"preview_state\":{\"preview_primitive\":\"cube\",\"background\":false,\"post_process_material_preview\":false}}"), *MaterialTargetParams),
			200,
			MaterialPreviewStateJson))
		{
			TestTrue(TEXT("Material viewport preview state ok"), MaterialPreviewStateJson->GetBoolField(TEXT("ok")));
			const TSharedPtr<FJsonObject> Data = GetData(TEXT("Material viewport preview state"), MaterialPreviewStateJson);
			TSharedPtr<FJsonObject> AppliedObj;
			TestTrue(TEXT("Material viewport preview_state_applied object"), Data.IsValid() && TryGetNestedObject(Data, TEXT("preview_state_applied"), AppliedObj));
			if (AppliedObj.IsValid())
			{
				TestTrue(TEXT("Material viewport preview state applied"), AppliedObj->GetBoolField(TEXT("applied")));
			}
			TSharedPtr<FJsonObject> PreviewStateObj;
			TestTrue(TEXT("Material viewport preview_state readback object"), Data.IsValid() && TryGetNestedObject(Data, TEXT("preview_state"), PreviewStateObj));
			if (PreviewStateObj.IsValid())
			{
				TestEqual(TEXT("Material viewport preview primitive cube"), PreviewStateObj->GetStringField(TEXT("preview_primitive")), FString(TEXT("cube")));
				TestFalse(TEXT("Material viewport background disabled"), PreviewStateObj->GetBoolField(TEXT("background")));
				TestFalse(TEXT("Material viewport post process disabled"), PreviewStateObj->GetBoolField(TEXT("post_process_material_preview")));
			}
		}

		TSharedPtr<FJsonObject> MaterialPreShotStateJson;
		FVector MaterialPreShotCameraLocation = FVector::ZeroVector;
		FRotator MaterialPreShotCameraRotation = FRotator::ZeroRotator;
		double MaterialPreShotCameraFov = 0.0;
		bool bHasMaterialPreShotCamera = false;
		if (ExecJson(TEXT("smoke-editor-viewport-material-pre-shot-state"), TEXT("editor_viewport_get_state"), FString::Printf(TEXT("{%s}"), *MaterialTargetParams), 200, MaterialPreShotStateJson))
		{
			const TSharedPtr<FJsonObject> Data = GetData(TEXT("Material viewport pre-shot state"), MaterialPreShotStateJson);
			bHasMaterialPreShotCamera = Data.IsValid() && ReadCamera(Data, MaterialPreShotCameraLocation, MaterialPreShotCameraRotation, MaterialPreShotCameraFov);
			TestTrue(TEXT("Material viewport pre-shot camera readable"), bHasMaterialPreShotCamera);
		}

		const FString MaterialScreenshotOutputFile = TEXT("runtimeLogs/editor_viewport_material_smoke.png");
		TSharedPtr<FJsonObject> MaterialScreenshotJson;
		if (ExecJson(
			TEXT("smoke-editor-viewport-material-screenshot"),
			TEXT("editor_viewport_screenshot"),
			FString::Printf(
				TEXT("{%s,\"output_file\":\"%s\",\"format\":\"png\",\"max_size\":128,\"warmup_frames\":0,\"restore_after_capture\":true,\"visual_options\":{\"view_mode\":\"lit\",\"overlays\":{\"grid\":false,\"bounds\":true}},\"preview_state\":{\"preview_primitive\":\"sphere\",\"background\":false},\"focus\":{\"mode\":\"preview_component_bounds\"},\"camera\":{\"orbit\":{\"target\":{\"x\":0,\"y\":0,\"z\":0},\"yaw\":40,\"pitch\":-20,\"distance\":300,\"fov\":40}}}"),
				*MaterialTargetParams,
				*MaterialScreenshotOutputFile),
			200,
			MaterialScreenshotJson))
		{
			TestTrue(TEXT("Material viewport screenshot ok"), MaterialScreenshotJson->GetBoolField(TEXT("ok")));
			const TSharedPtr<FJsonObject> Data = GetData(TEXT("Material viewport screenshot"), MaterialScreenshotJson);
			if (Data.IsValid())
			{
				TestEqual(TEXT("Material viewport screenshot schema"), Data->GetStringField(TEXT("schema")), FString(TEXT("uai.editor_viewport.screenshot.v1")));
				TestEqual(TEXT("Material viewport screenshot backend"), Data->GetStringField(TEXT("capture_backend")), FString(TEXT("material_editor_backbuffer")));
				TestTrue(TEXT("Material viewport screenshot width positive"), Data->GetNumberField(TEXT("width")) > 0.0);
				TestTrue(TEXT("Material viewport screenshot height positive"), Data->GetNumberField(TEXT("height")) > 0.0);
				const FString ScreenshotPath = Data->GetStringField(TEXT("file_path"));
				TestTrue(TEXT("Material viewport screenshot file exists"), !ScreenshotPath.IsEmpty() && IFileManager::Get().FileExists(*ScreenshotPath));
				TSharedPtr<FJsonObject> PixelSignalObj;
				TestTrue(TEXT("Material viewport screenshot pixel signal object"), TryGetNestedObject(Data, TEXT("pixel_signal"), PixelSignalObj));
				if (PixelSignalObj.IsValid())
				{
					TestTrue(TEXT("Material viewport screenshot nonblank"), PixelSignalObj->GetBoolField(TEXT("nonblank")));
				}
				TSharedPtr<FJsonObject> RestoreObj;
				TestTrue(TEXT("Material viewport screenshot restore_result object"), TryGetNestedObject(Data, TEXT("restore_result"), RestoreObj));
				if (RestoreObj.IsValid())
				{
					TestTrue(TEXT("Material viewport screenshot restored"), RestoreObj->GetBoolField(TEXT("restored")));
					TestTrue(TEXT("Material viewport screenshot restore succeeded"), RestoreObj->GetBoolField(TEXT("success")));
				}
			}
		}

		if (bHasMaterialPreShotCamera)
		{
			TSharedPtr<FJsonObject> MaterialPostShotStateJson;
			if (ExecJson(TEXT("smoke-editor-viewport-material-post-shot-state"), TEXT("editor_viewport_get_state"), FString::Printf(TEXT("{%s}"), *MaterialTargetParams), 200, MaterialPostShotStateJson))
			{
				const TSharedPtr<FJsonObject> Data = GetData(TEXT("Material viewport post-shot state"), MaterialPostShotStateJson);
				FVector MaterialPostShotCameraLocation = FVector::ZeroVector;
				FRotator MaterialPostShotCameraRotation = FRotator::ZeroRotator;
				double MaterialPostShotCameraFov = 0.0;
				if (Data.IsValid() && ReadCamera(Data, MaterialPostShotCameraLocation, MaterialPostShotCameraRotation, MaterialPostShotCameraFov))
				{
					TestTrue(TEXT("Material viewport screenshot restored camera x"), FMath::IsNearlyEqual(MaterialPostShotCameraLocation.X, MaterialPreShotCameraLocation.X, 0.1));
					TestTrue(TEXT("Material viewport screenshot restored camera y"), FMath::IsNearlyEqual(MaterialPostShotCameraLocation.Y, MaterialPreShotCameraLocation.Y, 0.1));
					TestTrue(TEXT("Material viewport screenshot restored camera z"), FMath::IsNearlyEqual(MaterialPostShotCameraLocation.Z, MaterialPreShotCameraLocation.Z, 0.1));
					TestTrue(TEXT("Material viewport screenshot restored fov"), FMath::IsNearlyEqual(MaterialPostShotCameraFov, MaterialPreShotCameraFov, 0.1));
				}
			}
		}

		TSharedPtr<FJsonObject> MaterialRestorePreviewJson;
		ExecJson(
			TEXT("smoke-editor-viewport-material-restore-preview-state"),
			TEXT("editor_viewport_set_preview_state"),
			FString::Printf(
				TEXT("{%s,\"preview_state\":{\"preview_primitive\":\"%s\",\"background\":%s,\"post_process_material_preview\":%s}}"),
				*MaterialTargetParams,
				*MaterialOriginalPreviewPrimitive,
				BoolJson(bMaterialOriginalBackground),
				BoolJson(bMaterialOriginalPostProcess)),
			200,
			MaterialRestorePreviewJson);

		if (bHasMaterialOriginalCamera)
		{
			TSharedPtr<FJsonObject> MaterialRestoreCameraJson;
			ExecJson(
				TEXT("smoke-editor-viewport-material-restore-camera"),
				TEXT("editor_viewport_set_camera"),
				FString::Printf(
					TEXT("{%s,\"camera\":{\"location\":{\"x\":%.6f,\"y\":%.6f,\"z\":%.6f},\"rotation\":{\"pitch\":%.6f,\"yaw\":%.6f,\"roll\":%.6f},\"fov\":%.6f}}"),
					*MaterialTargetParams,
					MaterialOriginalCameraLocation.X,
					MaterialOriginalCameraLocation.Y,
					MaterialOriginalCameraLocation.Z,
					MaterialOriginalCameraRotation.Pitch,
					MaterialOriginalCameraRotation.Yaw,
					MaterialOriginalCameraRotation.Roll,
					MaterialOriginalCameraFov),
				200,
				MaterialRestoreCameraJson);
		}

		TSharedPtr<FJsonObject> MaterialRestoreVisualJson;
		ExecJson(
			TEXT("smoke-editor-viewport-material-restore-visual-options"),
			TEXT("editor_viewport_set_visual_options"),
			FString::Printf(
				TEXT("{%s,\"visual_options\":{\"view_mode\":\"%s\",\"overlays\":{\"grid\":%s,\"bounds\":%s},\"realtime\":%s}}"),
				*MaterialTargetParams,
				*MaterialOriginalViewMode,
				BoolJson(bMaterialOriginalGrid),
				BoolJson(bMaterialOriginalBounds),
				BoolJson(bMaterialOriginalRealtime)),
			200,
			MaterialRestoreVisualJson);
	}

	const FString SkeletalMeshAssetPath = TEXT("/Game/Characters/Mannequins/Meshes/SKM_Manny_Simple.SKM_Manny_Simple");
	USkeletalMesh* SmokeSkeletalMesh = LoadObject<USkeletalMesh>(nullptr, *SkeletalMeshAssetPath);
	TestNotNull(TEXT("Persona viewport smoke skeletal mesh asset"), SmokeSkeletalMesh);
	FString RootBoneName = TEXT("root");
	FString SecondaryBoneName = TEXT("root");
	if (SmokeSkeletalMesh && SmokeSkeletalMesh->GetRefSkeleton().GetNum() > 0)
	{
		RootBoneName = SmokeSkeletalMesh->GetRefSkeleton().GetBoneName(0).ToString();
		if (SmokeSkeletalMesh->GetRefSkeleton().GetNum() > 1)
		{
			SecondaryBoneName = SmokeSkeletalMesh->GetRefSkeleton().GetBoneName(1).ToString();
		}
	}
	USkeleton* SmokeSkeleton = SmokeSkeletalMesh ? SmokeSkeletalMesh->GetSkeleton() : nullptr;
	TestNotNull(TEXT("Persona viewport smoke skeleton asset"), SmokeSkeleton);
	const FString AnimationAssetPath = TEXT("/Game/Characters/Mannequins/Anims/Unarmed/MM_Idle.MM_Idle");
	UAnimationAsset* SmokeAnimationAsset = LoadObject<UAnimationAsset>(nullptr, *AnimationAssetPath);
	TestNotNull(TEXT("Persona viewport smoke animation asset"), SmokeAnimationAsset);
	const FString AnimBlueprintAssetPath = TEXT("/Game/Characters/Mannequins/Anims/Unarmed/ABP_Unarmed.ABP_Unarmed");
	UAnimBlueprint* SmokeAnimBlueprint = LoadObject<UAnimBlueprint>(nullptr, *AnimBlueprintAssetPath);
	TestNotNull(TEXT("Persona viewport smoke anim blueprint asset"), SmokeAnimBlueprint);
	const FString ControlRigAssetPath = TEXT("/Game/Characters/Mannequins/Rigs/CR_Mannequin_Body.CR_Mannequin_Body");
	UControlRigBlueprint* SmokeControlRig = LoadObject<UControlRigBlueprint>(nullptr, *ControlRigAssetPath);
	TestNotNull(TEXT("ControlRig viewport smoke asset"), SmokeControlRig);
	USkeletalMesh* SmokeControlRigPreviewMesh = SmokeControlRig ? SmokeControlRig->GetPreviewMesh() : nullptr;
	TestNotNull(TEXT("ControlRig viewport smoke preview mesh"), SmokeControlRigPreviewMesh);
	FString SmokeControlRigControlName;
	FString SmokeControlRigNullName;
	if (URigHierarchy* SmokeControlRigHierarchy = SmokeControlRig ? SmokeControlRig->GetHierarchy() : nullptr)
	{
		const TArray<FRigElementKey> ControlKeys = SmokeControlRigHierarchy->GetControlKeys(true);
		if (ControlKeys.Num() > 0)
		{
			SmokeControlRigControlName = ControlKeys[0].Name.ToString();
		}
		const TArray<FRigElementKey> NullKeys = SmokeControlRigHierarchy->GetNullKeys(true);
		if (NullKeys.Num() > 0)
		{
			SmokeControlRigNullName = NullKeys[0].Name.ToString();
		}
	}
	TestTrue(TEXT("ControlRig viewport smoke has control element"), !SmokeControlRigControlName.IsEmpty());
	TestTrue(TEXT("ControlRig viewport smoke has null/space element"), !SmokeControlRigNullName.IsEmpty());
	const FString BlueprintSCSAssetPath = TEXT("/Game/Blueprints/BP_FloatingRotatingCube.BP_FloatingRotatingCube");
	UBlueprint* SmokeBlueprintSCS = LoadObject<UBlueprint>(nullptr, *BlueprintSCSAssetPath);
	TestNotNull(TEXT("Blueprint SCS viewport smoke asset"), SmokeBlueprintSCS);
	const FString NiagaraSystemAssetPath = TEXT("/Game/Niagara/OfficialTemplateStudy/Systems/NS_DirectionalBurst.NS_DirectionalBurst");
	UNiagaraSystem* SmokeNiagaraSystem = LoadObject<UNiagaraSystem>(nullptr, *NiagaraSystemAssetPath);
	TestNotNull(TEXT("Niagara viewport smoke system asset"), SmokeNiagaraSystem);
	UNiagaraSimCache* SmokeNiagaraSimCache = nullptr;
	if (bCanRenderViewport && SmokeNiagaraSystem && GEditor)
	{
		UWorld* SmokeWorld = GEditor->GetEditorWorldContext().World();
		TestNotNull(TEXT("Niagara SimCache smoke editor world"), SmokeWorld);
		if (SmokeWorld)
		{
			if (SmokeNiagaraSystem->HasOutstandingCompilationRequests(true))
			{
				SmokeNiagaraSystem->WaitForCompilationComplete(true, false);
				SmokeNiagaraSystem->PollForCompilationComplete(true);
			}
			SmokeNiagaraSystem->CacheFromCompiledData();

			UNiagaraComponent* SimCacheComponent = NewObject<UNiagaraComponent>(SmokeWorld, TEXT("UAI_NiagaraSimCacheSmokeComponent"), RF_Transient);
			TestNotNull(TEXT("Niagara SimCache smoke component"), SimCacheComponent);
			if (SimCacheComponent)
			{
				SimCacheComponent->SetAsset(SmokeNiagaraSystem, false);
				SimCacheComponent->SetForceSolo(true);
				SimCacheComponent->SetAgeUpdateMode(ENiagaraAgeUpdateMode::TickDeltaTime);
				SimCacheComponent->SetSeekDelta(1.0f / 30.0f);
				SimCacheComponent->SetCanRenderWhileSeeking(true);
				SimCacheComponent->SetAllowScalability(false);
				SimCacheComponent->SetRenderingEnabled(true);
				SimCacheComponent->RegisterComponentWithWorld(SmokeWorld);
				SimCacheComponent->Activate(true);
				SimCacheComponent->InitializeSystem();
				SmokeWorld->SendAllEndOfFrameUpdates();

				const FString SimCacheAssetPath = MakeAutomationAssetPath(TEXT("NiagaraSimCacheViewport"));
				UPackage* SimCachePackage = CreatePackage(*SimCacheAssetPath);
				const FName SimCacheAssetName(*FPackageName::GetLongPackageAssetName(SimCacheAssetPath));
				SmokeNiagaraSimCache = NewObject<UNiagaraSimCache>(
					SimCachePackage,
					SimCacheAssetName,
					RF_Public | RF_Standalone | RF_Transactional);
				TestNotNull(TEXT("Niagara SimCache smoke asset"), SmokeNiagaraSimCache);
				if (SmokeNiagaraSimCache)
				{
					FNiagaraSimCacheCreateParameters CreateParameters;
					CreateParameters.AttributeCaptureMode = ENiagaraSimCacheAttributeCaptureMode::RenderingOnly;
					CreateParameters.bAllowDataInterfaceCaching = false;
					const bool bBeginWrite = SmokeNiagaraSimCache->BeginWrite(CreateParameters, SimCacheComponent);
					TestTrue(TEXT("Niagara SimCache begin write"), bBeginWrite);
					if (bBeginWrite)
					{
						const float FrameDelta = 1.0f / 30.0f;
						for (int32 FrameIndex = 0; FrameIndex < 4; ++FrameIndex)
						{
							SimCacheComponent->TickComponent(FrameDelta, ELevelTick::LEVELTICK_All, nullptr);
							SmokeWorld->SendAllEndOfFrameUpdates();
							TestTrue(FString::Printf(TEXT("Niagara SimCache write frame %d"), FrameIndex), SmokeNiagaraSimCache->WriteFrame(SimCacheComponent));
						}
						const bool bEndWrite = SmokeNiagaraSimCache->EndWrite(false);
						TestTrue(TEXT("Niagara SimCache end write"), bEndWrite);
					}
					TestTrue(TEXT("Niagara SimCache valid"), SmokeNiagaraSimCache->IsCacheValid());
					TestTrue(TEXT("Niagara SimCache has frames"), SmokeNiagaraSimCache->GetNumFrames() > 0);
					if (SmokeNiagaraSimCache->IsCacheValid() && SmokeNiagaraSimCache->GetNumFrames() > 0)
					{
						FAssetRegistryModule::AssetCreated(SmokeNiagaraSimCache);
						SimCachePackage->SetDirtyFlag(false);
					}
					else
					{
						SmokeNiagaraSimCache = nullptr;
					}
				}
				SimCacheComponent->DeactivateImmediate();
				SimCacheComponent->DestroyComponent();
			}
		}
	}
	ULevelSequence* SmokeLevelSequence = nullptr;
	if (GEditor)
	{
		const FString LevelSequenceAssetPath = MakeAutomationAssetPath(TEXT("LevelSequenceViewport"));
		const FString LevelSequenceAssetName = FPackageName::GetLongPackageAssetName(LevelSequenceAssetPath);
		UPackage* LevelSequencePackage = CreatePackage(*LevelSequenceAssetPath);
		TestNotNull(TEXT("LevelSequence viewport smoke package"), LevelSequencePackage);
		if (LevelSequencePackage && !LevelSequenceAssetName.IsEmpty())
		{
			SmokeLevelSequence = NewObject<ULevelSequence>(
				LevelSequencePackage,
				*LevelSequenceAssetName,
				RF_Public | RF_Standalone | RF_Transactional);
			TestNotNull(TEXT("LevelSequence viewport smoke asset"), SmokeLevelSequence);
			if (SmokeLevelSequence)
			{
				SmokeLevelSequence->Initialize();
				UMovieScene* MovieScene = SmokeLevelSequence->GetMovieScene();
				TestNotNull(TEXT("LevelSequence viewport smoke movie scene"), MovieScene);
				if (MovieScene)
				{
					MovieScene->SetDisplayRate(FFrameRate(30, 1));
					const FFrameRate TickResolution = MovieScene->GetTickResolution();
					MovieScene->SetPlaybackRange(TRange<FFrameNumber>(
						TickResolution.AsFrameNumber(0.0),
						TickResolution.AsFrameNumber(3.0)));
				}
				FAssetRegistryModule::AssetCreated(SmokeLevelSequence);
				LevelSequencePackage->SetDirtyFlag(false);
			}
		}
	}
	UGroomAsset* SmokeGroomAsset = nullptr;
	UPhysicsAsset* SmokePhysicsAsset = nullptr;
	if (bCanRenderViewport && SmokeSkeletalMesh && SmokeSkeletalMesh->GetRefSkeleton().GetNum() > 1)
	{
		const FString PhysicsAssetPath = MakeAutomationAssetPath(TEXT("PAViewport"));
		const FString PhysicsAssetObjectPath = FString::Printf(
			TEXT("%s.%s"),
			*PhysicsAssetPath,
			*FPackageName::GetLongPackageAssetName(PhysicsAssetPath));
		const FString PhysicsAssetFolder = MakeSmokeAssetFolderPath(TEXT("physics_asset_viewport"), PhysicsAssetPath);

		auto SavePhysicsAssetProfileFile = [this](const FString& FilePath, const FString& Contents, const TCHAR* Label) -> bool
		{
			IFileManager::Get().MakeDirectory(*FPaths::GetPath(FilePath), true);
			const bool bSaved = FFileHelper::SaveStringToFile(
				Contents,
				*FilePath,
				FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
			TestTrue(FString::Printf(TEXT("%s saved"), Label), bSaved);
			return bSaved;
		};

		const FString PhysicsAssetJson = FString::Printf(
			TEXT("{\"schema\":\"uai.physics_asset.folder.v1\",\"asset_path\":\"%s\",\"skeletal_mesh\":\"%s\"}"),
			*EscapeJsonString(PhysicsAssetPath),
			*EscapeJsonString(SkeletalMeshAssetPath));
		const FString PhysicsBodiesJson = FString::Printf(
			TEXT("{\"replace_all\":true,\"bodies\":[")
			TEXT("{\"bone_name\":\"%s\",\"physics_type\":\"Kinematic\",\"primitive_shapes\":[{\"type\":\"box\",\"x\":36.0,\"y\":28.0,\"z\":32.0,\"transform\":{\"location\":{\"x\":0.0,\"y\":0.0,\"z\":0.0},\"rotation\":{\"x\":0.0,\"y\":0.0,\"z\":0.0}}}]},")
			TEXT("{\"bone_name\":\"%s\",\"physics_type\":\"Simulated\",\"primitive_shapes\":[{\"type\":\"sphere\",\"radius\":18.0,\"transform\":{\"location\":{\"x\":0.0,\"y\":0.0,\"z\":0.0},\"rotation\":{\"x\":0.0,\"y\":0.0,\"z\":0.0}}}]}")
			TEXT("]}"),
			*EscapeJsonString(RootBoneName),
			*EscapeJsonString(SecondaryBoneName));
		const FString PhysicsConstraintsJson = FString::Printf(
			TEXT("{\"replace_all\":true,\"constraints\":[{\"name\":\"ViewportConstraint\",\"parent_body\":\"%s\",\"child_body\":\"%s\",\"disable_collision\":true,")
			TEXT("\"linear\":{\"x_motion\":\"Locked\",\"y_motion\":\"Locked\",\"z_motion\":\"Locked\",\"limit\":0.0},")
			TEXT("\"angular\":{\"swing1_motion\":\"Limited\",\"swing1_limit_degrees\":25.0,\"swing2_motion\":\"Limited\",\"swing2_limit_degrees\":25.0,\"twist_motion\":\"Limited\",\"twist_limit_degrees\":15.0}}]}"),
			*EscapeJsonString(RootBoneName),
			*EscapeJsonString(SecondaryBoneName));
		const FString PhysicsProfilesJson = TEXT("{\"constraint_profiles\":[],\"physical_animation_profiles\":[]}");
		const bool bPhysicsProfileSaved =
			SavePhysicsAssetProfileFile(FPaths::Combine(PhysicsAssetFolder, TEXT("asset.json")), PhysicsAssetJson, TEXT("PhysicsAsset viewport asset profile")) &&
			SavePhysicsAssetProfileFile(FPaths::Combine(PhysicsAssetFolder, TEXT("bodies.json")), PhysicsBodiesJson, TEXT("PhysicsAsset viewport body profile")) &&
			SavePhysicsAssetProfileFile(FPaths::Combine(PhysicsAssetFolder, TEXT("constraints.json")), PhysicsConstraintsJson, TEXT("PhysicsAsset viewport constraint profile")) &&
			SavePhysicsAssetProfileFile(FPaths::Combine(PhysicsAssetFolder, TEXT("profiles.json")), PhysicsProfilesJson, TEXT("PhysicsAsset viewport profiles profile"));

		if (bPhysicsProfileSaved)
		{
			TSharedPtr<FJsonObject> PhysicsApplyJson;
			if (ExecJson(
				TEXT("smoke-editor-viewport-physics-asset-apply-folder"),
				TEXT("physics_asset_apply_folder"),
				FString::Printf(
					TEXT("{\"folder_path\":\"%s\",\"create_if_missing\":true,\"allow_destructive_replace\":true,\"backup\":false,\"save_after_apply\":false}"),
					*EscapeJsonString(ToSmokeJsonPath(PhysicsAssetFolder))),
				200,
				PhysicsApplyJson))
			{
				TestTrue(TEXT("PhysicsAsset viewport apply folder ok"), PhysicsApplyJson->GetBoolField(TEXT("ok")));
				const TSharedPtr<FJsonObject> Data = GetData(TEXT("PhysicsAsset viewport apply folder"), PhysicsApplyJson);
				if (Data.IsValid())
				{
					TSharedPtr<FJsonObject> ReadbackObj;
					TestTrue(TEXT("PhysicsAsset viewport apply readback object"), TryGetNestedObject(Data, TEXT("readback"), ReadbackObj));
					if (ReadbackObj.IsValid())
					{
						TestTrue(TEXT("PhysicsAsset viewport apply bodies"), ReadbackObj->GetNumberField(TEXT("body_count")) >= 2.0);
						TestTrue(TEXT("PhysicsAsset viewport apply constraints"), ReadbackObj->GetNumberField(TEXT("constraint_count")) >= 1.0);
					}
				}
			}
		}

		SmokePhysicsAsset = LoadObject<UPhysicsAsset>(nullptr, *PhysicsAssetObjectPath);
		TestNotNull(TEXT("PhysicsAsset viewport smoke asset"), SmokePhysicsAsset);
		if (SmokePhysicsAsset)
		{
			const FString PhysicsTargetParams = FString::Printf(
				TEXT("\"target\":{\"scope\":\"asset_editor\",\"asset_type\":\"physics_asset\",\"asset_path\":\"%s\",\"open_editor_if_needed\":true}"),
				*PhysicsAssetObjectPath);

			TSharedPtr<FJsonObject> PhysicsStateJson;
			if (ExecJson(
				TEXT("smoke-editor-viewport-physics-asset-get-state"),
				TEXT("editor_viewport_get_state"),
				FString::Printf(TEXT("{%s}"), *PhysicsTargetParams),
				200,
				PhysicsStateJson))
			{
				TestTrue(TEXT("PhysicsAsset viewport get_state ok"), PhysicsStateJson->GetBoolField(TEXT("ok")));
				const TSharedPtr<FJsonObject> Data = GetData(TEXT("PhysicsAsset viewport get_state"), PhysicsStateJson);
				if (Data.IsValid())
				{
					TestEqual(TEXT("PhysicsAsset viewport adapter"), Data->GetStringField(TEXT("adapter")), FString(TEXT("physics_asset")));
					TSharedPtr<FJsonObject> CameraObj;
					TestTrue(TEXT("PhysicsAsset viewport camera object"), TryGetNestedObject(Data, TEXT("camera"), CameraObj));
					if (CameraObj.IsValid())
					{
						TestEqual(TEXT("PhysicsAsset viewport camera space"), CameraObj->GetStringField(TEXT("space")), FString(TEXT("physics_asset_preview_world")));
					}
					TSharedPtr<FJsonObject> CapabilitiesObj;
					TestTrue(TEXT("PhysicsAsset viewport capabilities object"), TryGetNestedObject(Data, TEXT("capabilities"), CapabilitiesObj));
					if (CapabilitiesObj.IsValid())
					{
						TestTrue(TEXT("PhysicsAsset viewport supports body focus"), ArrayHasString(CapabilitiesObj, TEXT("focus"), TEXT("body")));
						TestTrue(TEXT("PhysicsAsset viewport supports constraint focus"), ArrayHasString(CapabilitiesObj, TEXT("focus"), TEXT("constraint")));
						TestTrue(TEXT("PhysicsAsset viewport supports screenshot"), CapabilitiesObj->GetBoolField(TEXT("screenshot")));
					}
					TSharedPtr<FJsonObject> PreviewStateObj;
					TestTrue(TEXT("PhysicsAsset viewport preview_state object"), TryGetNestedObject(Data, TEXT("preview_state"), PreviewStateObj));
					if (PreviewStateObj.IsValid())
					{
						TestEqual(TEXT("PhysicsAsset viewport context"), PreviewStateObj->GetStringField(TEXT("context")), FString(TEXT("physics_asset")));
						TestTrue(TEXT("PhysicsAsset viewport body_count"), PreviewStateObj->GetNumberField(TEXT("body_count")) >= 2.0);
						TestTrue(TEXT("PhysicsAsset viewport constraint_count"), PreviewStateObj->GetNumberField(TEXT("constraint_count")) >= 1.0);
					}
				}
			}

			TSharedPtr<FJsonObject> PhysicsBodyFocusJson;
			if (ExecJson(
				TEXT("smoke-editor-viewport-physics-asset-focus-body"),
				TEXT("editor_viewport_focus"),
				FString::Printf(
					TEXT("{%s,\"focus\":{\"mode\":\"body\",\"bone_name\":\"%s\",\"instant\":true,\"radius\":40}}"),
					*PhysicsTargetParams,
					*EscapeJsonString(RootBoneName)),
				200,
				PhysicsBodyFocusJson))
			{
				TestTrue(TEXT("PhysicsAsset viewport body focus ok"), PhysicsBodyFocusJson->GetBoolField(TEXT("ok")));
				const TSharedPtr<FJsonObject> Data = GetData(TEXT("PhysicsAsset viewport body focus"), PhysicsBodyFocusJson);
				TSharedPtr<FJsonObject> FocusResultObj;
				TestTrue(TEXT("PhysicsAsset viewport body focus result object"), Data.IsValid() && TryGetNestedObject(Data, TEXT("focus_result"), FocusResultObj));
				if (FocusResultObj.IsValid())
				{
					TestEqual(TEXT("PhysicsAsset viewport body focus mode"), FocusResultObj->GetStringField(TEXT("mode")), FString(TEXT("body")));
					TestEqual(TEXT("PhysicsAsset viewport body focus bounds space"), FocusResultObj->GetStringField(TEXT("bounds_space")), FString(TEXT("physics_asset_preview_world")));
					TSharedPtr<FJsonObject> BodyObj;
					TestTrue(TEXT("PhysicsAsset viewport body focus body object"), TryGetNestedObject(FocusResultObj, TEXT("body"), BodyObj));
					if (BodyObj.IsValid())
					{
						TestEqual(TEXT("PhysicsAsset viewport body focus bone"), BodyObj->GetStringField(TEXT("bone_name")), RootBoneName);
					}
				}
			}

			TSharedPtr<FJsonObject> PhysicsConstraintFocusJson;
			if (ExecJson(
				TEXT("smoke-editor-viewport-physics-asset-focus-constraint"),
				TEXT("editor_viewport_focus"),
				FString::Printf(
					TEXT("{%s,\"focus\":{\"mode\":\"constraint\",\"constraint_index\":0,\"instant\":true,\"radius\":35}}"),
					*PhysicsTargetParams),
				200,
				PhysicsConstraintFocusJson))
			{
				TestTrue(TEXT("PhysicsAsset viewport constraint focus ok"), PhysicsConstraintFocusJson->GetBoolField(TEXT("ok")));
				const TSharedPtr<FJsonObject> Data = GetData(TEXT("PhysicsAsset viewport constraint focus"), PhysicsConstraintFocusJson);
				TSharedPtr<FJsonObject> FocusResultObj;
				TestTrue(TEXT("PhysicsAsset viewport constraint focus result object"), Data.IsValid() && TryGetNestedObject(Data, TEXT("focus_result"), FocusResultObj));
				if (FocusResultObj.IsValid())
				{
					TestEqual(TEXT("PhysicsAsset viewport constraint focus mode"), FocusResultObj->GetStringField(TEXT("mode")), FString(TEXT("constraint")));
					TSharedPtr<FJsonObject> ConstraintObj;
					TestTrue(TEXT("PhysicsAsset viewport constraint focus constraint object"), TryGetNestedObject(FocusResultObj, TEXT("constraint"), ConstraintObj));
					if (ConstraintObj.IsValid())
					{
						const FString ConstraintName = ConstraintObj->GetStringField(TEXT("name"));
						TestTrue(TEXT("PhysicsAsset viewport constraint focus name"), ConstraintName.StartsWith(TEXT("ViewportConstraint")));
						TestTrue(TEXT("PhysicsAsset viewport constraint child frame"), ConstraintObj->HasTypedField<EJson::Object>(TEXT("child_frame_location")));
						TestTrue(TEXT("PhysicsAsset viewport constraint parent frame"), ConstraintObj->HasTypedField<EJson::Object>(TEXT("parent_frame_location")));
					}
				}
			}

			TSharedPtr<FJsonObject> PhysicsVisualJson;
			if (ExecJson(
				TEXT("smoke-editor-viewport-physics-asset-visual-options"),
				TEXT("editor_viewport_set_visual_options"),
				FString::Printf(
					TEXT("{%s,\"visual_options\":{\"view_mode\":\"lit\",\"overlays\":{\"grid\":false,\"bounds\":true,\"bones\":true,\"collision\":true,\"bodies\":true,\"constraints\":true,\"center_of_mass\":true},\"collision_view_mode\":\"solid_wireframe\",\"constraint_view_mode\":\"all_limits\",\"realtime\":false}}"),
					*PhysicsTargetParams),
				200,
				PhysicsVisualJson))
			{
				TestTrue(TEXT("PhysicsAsset viewport visual options ok"), PhysicsVisualJson->GetBoolField(TEXT("ok")));
				const TSharedPtr<FJsonObject> Data = GetData(TEXT("PhysicsAsset viewport visual options"), PhysicsVisualJson);
				TSharedPtr<FJsonObject> VisualObj;
				TSharedPtr<FJsonObject> OverlaysObj;
				if (Data.IsValid() && TryGetNestedObject(Data, TEXT("visual_options"), VisualObj) && TryGetNestedObject(VisualObj, TEXT("overlays"), OverlaysObj))
				{
					TestFalse(TEXT("PhysicsAsset viewport visual grid disabled"), OverlaysObj->GetBoolField(TEXT("grid")));
					TestTrue(TEXT("PhysicsAsset viewport visual bounds enabled"), OverlaysObj->GetBoolField(TEXT("bounds")));
					TestTrue(TEXT("PhysicsAsset viewport visual collision enabled"), OverlaysObj->GetBoolField(TEXT("collision")));
					TestTrue(TEXT("PhysicsAsset viewport visual bodies enabled"), OverlaysObj->GetBoolField(TEXT("bodies")));
					TestTrue(TEXT("PhysicsAsset viewport visual constraints enabled"), OverlaysObj->GetBoolField(TEXT("constraints")));
					TestTrue(TEXT("PhysicsAsset viewport visual center of mass enabled"), OverlaysObj->GetBoolField(TEXT("center_of_mass")));
				}
			}

			const FString PhysicsScreenshotOutputFile = TEXT("runtimeLogs/editor_viewport_physics_asset_smoke.png");
			TSharedPtr<FJsonObject> PhysicsScreenshotJson;
			if (ExecJson(
				TEXT("smoke-editor-viewport-physics-asset-screenshot"),
				TEXT("editor_viewport_screenshot"),
				FString::Printf(
					TEXT("{%s,\"output_file\":\"%s\",\"format\":\"png\",\"max_size\":128,\"warmup_frames\":0,\"restore_after_capture\":true,\"visual_options\":{\"view_mode\":\"lit\",\"overlays\":{\"grid\":false,\"bounds\":true,\"bones\":true,\"collision\":true,\"bodies\":true,\"constraints\":true}},\"focus\":{\"mode\":\"constraint\",\"constraint_index\":0,\"radius\":35},\"camera\":{\"orbit\":{\"target\":{\"x\":0,\"y\":0,\"z\":90},\"yaw\":35,\"pitch\":-12,\"distance\":430,\"fov\":42}}}"),
					*PhysicsTargetParams,
					*PhysicsScreenshotOutputFile),
				200,
				PhysicsScreenshotJson))
			{
				TestTrue(TEXT("PhysicsAsset viewport screenshot ok"), PhysicsScreenshotJson->GetBoolField(TEXT("ok")));
				const TSharedPtr<FJsonObject> Data = GetData(TEXT("PhysicsAsset viewport screenshot"), PhysicsScreenshotJson);
				if (Data.IsValid())
				{
					TestEqual(TEXT("PhysicsAsset viewport screenshot backend"), Data->GetStringField(TEXT("capture_backend")), FString(TEXT("physics_asset_editor_backbuffer")));
					TestTrue(TEXT("PhysicsAsset viewport screenshot width positive"), Data->GetNumberField(TEXT("width")) > 0.0);
					const FString ScreenshotPath = Data->GetStringField(TEXT("file_path"));
					TestTrue(TEXT("PhysicsAsset viewport screenshot file exists"), !ScreenshotPath.IsEmpty() && IFileManager::Get().FileExists(*ScreenshotPath));
					TSharedPtr<FJsonObject> PixelSignalObj;
					TestTrue(TEXT("PhysicsAsset viewport screenshot pixel signal object"), TryGetNestedObject(Data, TEXT("pixel_signal"), PixelSignalObj));
					if (PixelSignalObj.IsValid())
					{
						TestTrue(TEXT("PhysicsAsset viewport screenshot nonblank"), PixelSignalObj->GetBoolField(TEXT("nonblank")));
					}
					TSharedPtr<FJsonObject> RestoreObj;
					TestTrue(TEXT("PhysicsAsset viewport screenshot restore_result object"), TryGetNestedObject(Data, TEXT("restore_result"), RestoreObj));
					if (RestoreObj.IsValid())
					{
						TestTrue(TEXT("PhysicsAsset viewport screenshot restored"), RestoreObj->GetBoolField(TEXT("restored")));
						TestTrue(TEXT("PhysicsAsset viewport screenshot restore succeeded"), RestoreObj->GetBoolField(TEXT("success")));
					}
				}
			}
		}
	}

	if (bCanRenderViewport)
	{
		const FString GroomAssetPath = MakeAutomationAssetPath(TEXT("GroomViewport"));
		const FString GroomAssetName = FPackageName::GetLongPackageAssetName(GroomAssetPath);
		const FString GroomAssetObjectPath = FString::Printf(TEXT("%s.%s"), *GroomAssetPath, *GroomAssetName);
		UPackage* GroomPackage = CreatePackage(*GroomAssetPath);
		TestNotNull(TEXT("Groom viewport package"), GroomPackage);
		if (GroomPackage)
		{
			SmokeGroomAsset = FindObject<UGroomAsset>(GroomPackage, *GroomAssetName);
			if (!SmokeGroomAsset)
			{
				SmokeGroomAsset = NewObject<UGroomAsset>(
					GroomPackage,
					*GroomAssetName,
					RF_Public | RF_Standalone | RF_Transactional);
				FAssetRegistryModule::AssetCreated(SmokeGroomAsset);
				GroomPackage->MarkPackageDirty();
			}
		}
		TestNotNull(TEXT("Groom viewport smoke asset"), SmokeGroomAsset);

		if (SmokeGroomAsset)
		{
			const FString GroomTargetParams = FString::Printf(
				TEXT("\"target\":{\"scope\":\"asset_editor\",\"asset_type\":\"groom\",\"asset_path\":\"%s\",\"open_editor_if_needed\":true}"),
				*GroomAssetObjectPath);

			TSharedPtr<FJsonObject> GroomStateJson;
			if (ExecJson(
				TEXT("smoke-editor-viewport-groom-get-state"),
				TEXT("editor_viewport_get_state"),
				FString::Printf(TEXT("{%s}"), *GroomTargetParams),
				200,
				GroomStateJson))
			{
				TestTrue(TEXT("Groom viewport get_state ok"), GroomStateJson->GetBoolField(TEXT("ok")));
				const TSharedPtr<FJsonObject> Data = GetData(TEXT("Groom viewport get_state"), GroomStateJson);
				if (Data.IsValid())
				{
					TestEqual(TEXT("Groom viewport adapter"), Data->GetStringField(TEXT("adapter")), FString(TEXT("groom")));
					TSharedPtr<FJsonObject> CameraObj;
					TestTrue(TEXT("Groom viewport camera object"), TryGetNestedObject(Data, TEXT("camera"), CameraObj));
					if (CameraObj.IsValid())
					{
						TestEqual(TEXT("Groom viewport camera space"), CameraObj->GetStringField(TEXT("space")), FString(TEXT("groom_preview_world")));
					}
					TSharedPtr<FJsonObject> CapabilitiesObj;
					TestTrue(TEXT("Groom viewport capabilities object"), TryGetNestedObject(Data, TEXT("capabilities"), CapabilitiesObj));
					if (CapabilitiesObj.IsValid())
					{
						TestTrue(TEXT("Groom viewport supports groom bounds focus"), ArrayHasString(CapabilitiesObj, TEXT("focus"), TEXT("groom_bounds")));
						TestTrue(TEXT("Groom viewport supports custom bounds focus"), ArrayHasString(CapabilitiesObj, TEXT("focus"), TEXT("custom_bounds")));
						TestTrue(TEXT("Groom viewport supports screenshot"), CapabilitiesObj->GetBoolField(TEXT("screenshot")));
						TestTrue(TEXT("Groom viewport supports groom visualization option"), ArrayHasString(CapabilitiesObj, TEXT("visual_option_keys"), TEXT("groom_visualization")));
					}
					TSharedPtr<FJsonObject> PreviewStateObj;
					TestTrue(TEXT("Groom viewport preview_state object"), TryGetNestedObject(Data, TEXT("preview_state"), PreviewStateObj));
					if (PreviewStateObj.IsValid())
					{
						TestEqual(TEXT("Groom viewport preview context"), PreviewStateObj->GetStringField(TEXT("context")), FString(TEXT("groom")));
						TestEqual(TEXT("Groom viewport asset path"), PreviewStateObj->GetStringField(TEXT("asset_path")), SmokeGroomAsset->GetPathName());
						TestTrue(TEXT("Groom viewport bounds object"), PreviewStateObj->HasTypedField<EJson::Object>(TEXT("bounds")));
						TestTrue(TEXT("Groom viewport empty asset uses explicit bounds source"), PreviewStateObj->GetStringField(TEXT("bounds_source")).Len() > 0);
					}
				}
			}

			TSharedPtr<FJsonObject> GroomFocusJson;
			if (ExecJson(
				TEXT("smoke-editor-viewport-groom-focus"),
				TEXT("editor_viewport_focus"),
				FString::Printf(
					TEXT("{%s,\"focus\":{\"mode\":\"groom_bounds\",\"instant\":true,\"radius\":50}}"),
					*GroomTargetParams),
				200,
				GroomFocusJson))
			{
				TestTrue(TEXT("Groom viewport focus ok"), GroomFocusJson->GetBoolField(TEXT("ok")));
				const TSharedPtr<FJsonObject> Data = GetData(TEXT("Groom viewport focus"), GroomFocusJson);
				TSharedPtr<FJsonObject> FocusResultObj;
				TestTrue(TEXT("Groom viewport focus result object"), Data.IsValid() && TryGetNestedObject(Data, TEXT("focus_result"), FocusResultObj));
				if (FocusResultObj.IsValid())
				{
					TestEqual(TEXT("Groom viewport focus mode"), FocusResultObj->GetStringField(TEXT("mode")), FString(TEXT("groom_bounds")));
					TestEqual(TEXT("Groom viewport focus bounds space"), FocusResultObj->GetStringField(TEXT("bounds_space")), FString(TEXT("groom_preview_world")));
					TestTrue(TEXT("Groom viewport focus bounds object"), FocusResultObj->HasTypedField<EJson::Object>(TEXT("bounds")));
				}
			}

			TSharedPtr<FJsonObject> GroomVisualJson;
			if (ExecJson(
				TEXT("smoke-editor-viewport-groom-visual-options"),
				TEXT("editor_viewport_set_visual_options"),
				FString::Printf(
					TEXT("{%s,\"visual_options\":{\"view_mode\":\"lit\",\"overlays\":{\"grid\":true,\"bounds\":true},\"groom_visualization\":\"guides\",\"realtime\":false}}"),
					*GroomTargetParams),
				200,
				GroomVisualJson))
			{
				TestTrue(TEXT("Groom viewport visual options ok"), GroomVisualJson->GetBoolField(TEXT("ok")));
				const TSharedPtr<FJsonObject> Data = GetData(TEXT("Groom viewport visual options"), GroomVisualJson);
				TSharedPtr<FJsonObject> VisualObj;
				TSharedPtr<FJsonObject> OverlaysObj;
				if (Data.IsValid() && TryGetNestedObject(Data, TEXT("visual_options"), VisualObj) && TryGetNestedObject(VisualObj, TEXT("overlays"), OverlaysObj))
				{
					TestTrue(TEXT("Groom viewport visual grid enabled"), OverlaysObj->GetBoolField(TEXT("grid")));
					TestTrue(TEXT("Groom viewport visual bounds enabled"), OverlaysObj->GetBoolField(TEXT("bounds")));
					TestTrue(TEXT("Groom viewport visualization enabled"), VisualObj->GetBoolField(TEXT("groom_visualization_enabled")));
				}
			}

			TSharedPtr<FJsonObject> GroomPreviewJson;
			if (ExecJson(
				TEXT("smoke-editor-viewport-groom-preview-state"),
				TEXT("editor_viewport_set_preview_state"),
				FString::Printf(
					TEXT("{%s,\"preview_state\":{\"reset_lod\":true,\"forced_lod\":-1}}"),
					*GroomTargetParams),
				200,
				GroomPreviewJson))
			{
				TestTrue(TEXT("Groom viewport preview state ok"), GroomPreviewJson->GetBoolField(TEXT("ok")));
				const TSharedPtr<FJsonObject> Data = GetData(TEXT("Groom viewport preview state"), GroomPreviewJson);
				TSharedPtr<FJsonObject> PreviewStateObj;
				if (Data.IsValid() && TryGetNestedObject(Data, TEXT("preview_state"), PreviewStateObj))
				{
					TestEqual(TEXT("Groom viewport preview state context"), PreviewStateObj->GetStringField(TEXT("context")), FString(TEXT("groom")));
					TestTrue(TEXT("Groom viewport preview state lod count reported"), PreviewStateObj->HasField(TEXT("lod_count")));
				}
			}

			const FString GroomScreenshotOutputFile = TEXT("runtimeLogs/editor_viewport_groom_smoke.png");
			TSharedPtr<FJsonObject> GroomScreenshotJson;
			if (ExecJson(
				TEXT("smoke-editor-viewport-groom-screenshot"),
				TEXT("editor_viewport_screenshot"),
				FString::Printf(
					TEXT("{%s,\"output_file\":\"%s\",\"format\":\"png\",\"max_size\":128,\"warmup_frames\":0,\"restore_after_capture\":true,\"visual_options\":{\"view_mode\":\"lit\",\"overlays\":{\"grid\":true,\"bounds\":true},\"groom_visualization\":\"guides\"},\"focus\":{\"mode\":\"groom_bounds\",\"radius\":50},\"camera\":{\"orbit\":{\"target\":{\"x\":0,\"y\":0,\"z\":0},\"yaw\":35,\"pitch\":-18,\"distance\":220,\"fov\":42}}}"),
					*GroomTargetParams,
					*GroomScreenshotOutputFile),
				200,
				GroomScreenshotJson))
			{
				TestTrue(TEXT("Groom viewport screenshot ok"), GroomScreenshotJson->GetBoolField(TEXT("ok")));
				const TSharedPtr<FJsonObject> Data = GetData(TEXT("Groom viewport screenshot"), GroomScreenshotJson);
				if (Data.IsValid())
				{
					TestEqual(TEXT("Groom viewport screenshot backend"), Data->GetStringField(TEXT("capture_backend")), FString(TEXT("groom_editor_backbuffer")));
					TestTrue(TEXT("Groom viewport screenshot width positive"), Data->GetNumberField(TEXT("width")) > 0.0);
					const FString ScreenshotPath = Data->GetStringField(TEXT("file_path"));
					TestTrue(TEXT("Groom viewport screenshot file exists"), !ScreenshotPath.IsEmpty() && IFileManager::Get().FileExists(*ScreenshotPath));
					TSharedPtr<FJsonObject> PixelSignalObj;
					TestTrue(TEXT("Groom viewport screenshot pixel signal object"), TryGetNestedObject(Data, TEXT("pixel_signal"), PixelSignalObj));
					if (PixelSignalObj.IsValid())
					{
						TestTrue(TEXT("Groom viewport screenshot nonblank"), PixelSignalObj->GetBoolField(TEXT("nonblank")));
					}
					TSharedPtr<FJsonObject> RestoreObj;
					TestTrue(TEXT("Groom viewport screenshot restore_result object"), TryGetNestedObject(Data, TEXT("restore_result"), RestoreObj));
					if (RestoreObj.IsValid())
					{
						TestTrue(TEXT("Groom viewport screenshot restored"), RestoreObj->GetBoolField(TEXT("restored")));
						TestTrue(TEXT("Groom viewport screenshot restore succeeded"), RestoreObj->GetBoolField(TEXT("success")));
					}
				}
			}
		}
	}
	const FString PersonaTargetParams = FString::Printf(
		TEXT("\"target\":{\"scope\":\"asset_editor\",\"asset_type\":\"skeletal_mesh\",\"asset_path\":\"%s\",\"open_editor_if_needed\":true}"),
		*SkeletalMeshAssetPath);
	FVector PersonaOriginalCameraLocation = FVector::ZeroVector;
	FRotator PersonaOriginalCameraRotation = FRotator::ZeroRotator;
	double PersonaOriginalCameraFov = 90.0;
	bool bHasPersonaOriginalCamera = false;
	FString PersonaOriginalViewMode = TEXT("lit");
	bool bPersonaOriginalGrid = true;
	bool bPersonaOriginalBounds = false;
	bool bPersonaOriginalBones = false;
	FString PersonaOriginalBoneDrawMode = TEXT("none");
	bool bPersonaOriginalLocalAxes = false;
	FString PersonaOriginalLocalAxesMode = TEXT("none");
	bool bPersonaOriginalSockets = false;
	bool bPersonaOriginalMeshSockets = false;
	bool bPersonaOriginalSkeletonSockets = false;
	bool bPersonaOriginalNormals = false;
	bool bPersonaOriginalTangents = false;
	bool bPersonaOriginalBinormals = false;
	bool bPersonaOriginalRealtime = false;
	bool bPersonaOriginalReferencePose = false;

	if (bCanRenderViewport && SmokeSkeletalMesh)
	{
		TSharedPtr<FJsonObject> PersonaInitialStateJson;
		if (ExecJson(
			TEXT("smoke-editor-viewport-persona-get-state"),
			TEXT("editor_viewport_get_state"),
			FString::Printf(TEXT("{%s}"), *PersonaTargetParams),
			200,
			PersonaInitialStateJson))
		{
			TestTrue(TEXT("Persona viewport get_state ok"), PersonaInitialStateJson->GetBoolField(TEXT("ok")));
			const TSharedPtr<FJsonObject> Data = GetData(TEXT("Persona viewport get_state"), PersonaInitialStateJson);
			if (Data.IsValid())
			{
				TestEqual(TEXT("Persona viewport state schema"), Data->GetStringField(TEXT("schema")), FString(TEXT("uai.editor_viewport.state.v1")));
				TestEqual(TEXT("Persona viewport adapter"), Data->GetStringField(TEXT("adapter")), FString(TEXT("persona")));

				TSharedPtr<FJsonObject> CameraObj;
				TestTrue(TEXT("Persona viewport camera object"), TryGetNestedObject(Data, TEXT("camera"), CameraObj));
				if (CameraObj.IsValid())
				{
					TestEqual(TEXT("Persona viewport camera space"), CameraObj->GetStringField(TEXT("space")), FString(TEXT("persona_preview_world")));
					bHasPersonaOriginalCamera = ReadCamera(Data, PersonaOriginalCameraLocation, PersonaOriginalCameraRotation, PersonaOriginalCameraFov);
					TestTrue(TEXT("Persona viewport original camera readable"), bHasPersonaOriginalCamera);
				}

				TSharedPtr<FJsonObject> CapabilitiesObj;
				TestTrue(TEXT("Persona viewport capabilities object"), TryGetNestedObject(Data, TEXT("capabilities"), CapabilitiesObj));
				if (CapabilitiesObj.IsValid())
				{
					TestTrue(TEXT("Persona viewport supports camera"), CapabilitiesObj->GetBoolField(TEXT("camera")));
					TestTrue(TEXT("Persona viewport supports visual options"), CapabilitiesObj->GetBoolField(TEXT("visual_options")));
					TestTrue(TEXT("Persona viewport supports preview state"), CapabilitiesObj->GetBoolField(TEXT("preview_state")));
					TestTrue(TEXT("Persona viewport supports screenshot"), CapabilitiesObj->GetBoolField(TEXT("screenshot")));
					TestTrue(TEXT("Persona viewport supports focus bone"), ArrayHasString(CapabilitiesObj, TEXT("focus"), TEXT("bone")));
					TestTrue(TEXT("Persona viewport supports focus socket"), ArrayHasString(CapabilitiesObj, TEXT("focus"), TEXT("socket")));
				}

				TSharedPtr<FJsonObject> PreviewStateObj;
				TestTrue(TEXT("Persona viewport preview state object"), TryGetNestedObject(Data, TEXT("preview_state"), PreviewStateObj));
				if (PreviewStateObj.IsValid())
				{
					TestEqual(TEXT("Persona viewport preview mesh path"), PreviewStateObj->GetStringField(TEXT("preview_mesh_path")), SkeletalMeshAssetPath);
					PreviewStateObj->TryGetBoolField(TEXT("show_reference_pose"), bPersonaOriginalReferencePose);
				}

				TSharedPtr<FJsonObject> VisualOptionsObj;
				TestTrue(TEXT("Persona viewport visual options object"), TryGetNestedObject(Data, TEXT("visual_options"), VisualOptionsObj));
				if (VisualOptionsObj.IsValid())
				{
					VisualOptionsObj->TryGetStringField(TEXT("view_mode"), PersonaOriginalViewMode);
					VisualOptionsObj->TryGetBoolField(TEXT("realtime"), bPersonaOriginalRealtime);

					TSharedPtr<FJsonObject> OverlaysObj;
					if (TryGetNestedObject(VisualOptionsObj, TEXT("overlays"), OverlaysObj))
					{
						OverlaysObj->TryGetBoolField(TEXT("grid"), bPersonaOriginalGrid);
						OverlaysObj->TryGetBoolField(TEXT("bounds"), bPersonaOriginalBounds);
						OverlaysObj->TryGetBoolField(TEXT("bones"), bPersonaOriginalBones);
						OverlaysObj->TryGetStringField(TEXT("bone_draw_mode"), PersonaOriginalBoneDrawMode);
						OverlaysObj->TryGetBoolField(TEXT("local_axes"), bPersonaOriginalLocalAxes);
						OverlaysObj->TryGetStringField(TEXT("local_axes_mode"), PersonaOriginalLocalAxesMode);
						OverlaysObj->TryGetBoolField(TEXT("sockets"), bPersonaOriginalSockets);
						OverlaysObj->TryGetBoolField(TEXT("mesh_sockets"), bPersonaOriginalMeshSockets);
						OverlaysObj->TryGetBoolField(TEXT("skeleton_sockets"), bPersonaOriginalSkeletonSockets);
						OverlaysObj->TryGetBoolField(TEXT("normals"), bPersonaOriginalNormals);
						OverlaysObj->TryGetBoolField(TEXT("tangents"), bPersonaOriginalTangents);
						OverlaysObj->TryGetBoolField(TEXT("binormals"), bPersonaOriginalBinormals);
					}
				}
			}
		}

		TSharedPtr<FJsonObject> PersonaCameraJson;
		if (ExecJson(
			TEXT("smoke-editor-viewport-persona-set-camera"),
			TEXT("editor_viewport_set_camera"),
			FString::Printf(
				TEXT("{%s,\"camera\":{\"orbit\":{\"target\":{\"x\":0,\"y\":0,\"z\":90},\"yaw\":35,\"pitch\":-12,\"distance\":360,\"fov\":35}}}"),
				*PersonaTargetParams),
			200,
			PersonaCameraJson))
		{
			TestTrue(TEXT("Persona viewport set_camera ok"), PersonaCameraJson->GetBoolField(TEXT("ok")));
			const TSharedPtr<FJsonObject> Data = GetData(TEXT("Persona viewport set_camera"), PersonaCameraJson);
			TSharedPtr<FJsonObject> CameraObj;
			if (Data.IsValid() && TryGetNestedObject(Data, TEXT("camera"), CameraObj))
			{
				TestEqual(TEXT("Persona viewport set_camera space"), CameraObj->GetStringField(TEXT("space")), FString(TEXT("persona_preview_world")));
				TestTrue(TEXT("Persona viewport set_camera fov"), FMath::IsNearlyEqual(CameraObj->GetNumberField(TEXT("fov")), 35.0, 0.1));
			}
		}

		TSharedPtr<FJsonObject> PersonaFocusJson;
		if (ExecJson(
			TEXT("smoke-editor-viewport-persona-focus-bone"),
			TEXT("editor_viewport_focus"),
			FString::Printf(
				TEXT("{%s,\"focus\":{\"mode\":\"bone\",\"bone_name\":\"%s\",\"radius\":35}}"),
				*PersonaTargetParams,
				*RootBoneName),
			200,
			PersonaFocusJson))
		{
			TestTrue(TEXT("Persona viewport focus ok"), PersonaFocusJson->GetBoolField(TEXT("ok")));
			const TSharedPtr<FJsonObject> Data = GetData(TEXT("Persona viewport focus"), PersonaFocusJson);
			TSharedPtr<FJsonObject> FocusResultObj;
			TestTrue(TEXT("Persona viewport focus result object"), Data.IsValid() && TryGetNestedObject(Data, TEXT("focus_result"), FocusResultObj));
			if (FocusResultObj.IsValid())
			{
				TestEqual(TEXT("Persona viewport focus mode"), FocusResultObj->GetStringField(TEXT("mode")), FString(TEXT("bone")));
				TestEqual(TEXT("Persona viewport focus bounds space"), FocusResultObj->GetStringField(TEXT("bounds_space")), FString(TEXT("persona_preview_world")));
				TestTrue(TEXT("Persona viewport focus bounds object"), FocusResultObj->HasTypedField<EJson::Object>(TEXT("bounds")));
			}
		}

		TSharedPtr<FJsonObject> PersonaVisualJson;
		if (ExecJson(
			TEXT("smoke-editor-viewport-persona-visual-options"),
			TEXT("editor_viewport_set_visual_options"),
			FString::Printf(
				TEXT("{%s,\"visual_options\":{\"view_mode\":\"lit\",\"overlays\":{\"grid\":false,\"bounds\":true,\"bones\":true,\"bone_draw_mode\":\"all\",\"local_axes\":true,\"sockets\":true,\"mesh_sockets\":true,\"skeleton_sockets\":true,\"normals\":true,\"tangents\":true,\"binormals\":true}}}"),
				*PersonaTargetParams),
			200,
			PersonaVisualJson))
		{
			TestTrue(TEXT("Persona viewport visual options ok"), PersonaVisualJson->GetBoolField(TEXT("ok")));
			const TSharedPtr<FJsonObject> Data = GetData(TEXT("Persona viewport visual options"), PersonaVisualJson);
			TSharedPtr<FJsonObject> VisualObj;
			TSharedPtr<FJsonObject> OverlaysObj;
			if (Data.IsValid())
			{
				TestTrue(TEXT("Persona viewport visual_options object"), TryGetNestedObject(Data, TEXT("visual_options"), VisualObj));
			}
			if (TryGetNestedObject(VisualObj, TEXT("overlays"), OverlaysObj))
			{
				TestFalse(TEXT("Persona viewport visual grid disabled"), OverlaysObj->GetBoolField(TEXT("grid")));
				TestTrue(TEXT("Persona viewport visual bounds enabled"), OverlaysObj->GetBoolField(TEXT("bounds")));
				TestTrue(TEXT("Persona viewport visual bones enabled"), OverlaysObj->GetBoolField(TEXT("bones")));
				TestEqual(TEXT("Persona viewport visual bone draw mode"), OverlaysObj->GetStringField(TEXT("bone_draw_mode")), FString(TEXT("all")));
				TestTrue(TEXT("Persona viewport visual local axes enabled"), OverlaysObj->GetBoolField(TEXT("local_axes")));
				TestTrue(TEXT("Persona viewport visual sockets enabled"), OverlaysObj->GetBoolField(TEXT("sockets")));
				TestTrue(TEXT("Persona viewport visual normals enabled"), OverlaysObj->GetBoolField(TEXT("normals")));
				TestTrue(TEXT("Persona viewport visual tangents enabled"), OverlaysObj->GetBoolField(TEXT("tangents")));
				TestTrue(TEXT("Persona viewport visual binormals enabled"), OverlaysObj->GetBoolField(TEXT("binormals")));
			}
		}

		TSharedPtr<FJsonObject> PersonaPreviewStateJson;
		if (ExecJson(
			TEXT("smoke-editor-viewport-persona-reference-pose"),
			TEXT("editor_viewport_set_preview_state"),
			FString::Printf(TEXT("{%s,\"preview_state\":{\"show_reference_pose\":true}}"), *PersonaTargetParams),
			200,
			PersonaPreviewStateJson))
		{
			TestTrue(TEXT("Persona viewport preview state ok"), PersonaPreviewStateJson->GetBoolField(TEXT("ok")));
			const TSharedPtr<FJsonObject> Data = GetData(TEXT("Persona viewport preview state"), PersonaPreviewStateJson);
			TSharedPtr<FJsonObject> AppliedObj;
			TestTrue(TEXT("Persona viewport preview_state_applied object"), Data.IsValid() && TryGetNestedObject(Data, TEXT("preview_state_applied"), AppliedObj));
			if (AppliedObj.IsValid())
			{
				TestTrue(TEXT("Persona viewport reference pose applied"), AppliedObj->GetBoolField(TEXT("applied")));
			}
			TSharedPtr<FJsonObject> PreviewStateObj;
			TestTrue(TEXT("Persona viewport preview_state object after apply"), Data.IsValid() && TryGetNestedObject(Data, TEXT("preview_state"), PreviewStateObj));
			if (PreviewStateObj.IsValid())
			{
				TestTrue(TEXT("Persona viewport reference pose enabled"), PreviewStateObj->GetBoolField(TEXT("show_reference_pose")));
			}
		}

		TSharedPtr<FJsonObject> PersonaPreShotStateJson;
		FVector PersonaPreShotCameraLocation = FVector::ZeroVector;
		FRotator PersonaPreShotCameraRotation = FRotator::ZeroRotator;
		double PersonaPreShotCameraFov = 0.0;
		bool bHasPersonaPreShotCamera = false;
		if (ExecJson(
			TEXT("smoke-editor-viewport-persona-get-state-before-shot"),
			TEXT("editor_viewport_get_state"),
			FString::Printf(TEXT("{%s}"), *PersonaTargetParams),
			200,
			PersonaPreShotStateJson))
		{
			const TSharedPtr<FJsonObject> Data = GetData(TEXT("Persona viewport pre-shot state"), PersonaPreShotStateJson);
			bHasPersonaPreShotCamera = Data.IsValid() && ReadCamera(Data, PersonaPreShotCameraLocation, PersonaPreShotCameraRotation, PersonaPreShotCameraFov);
			TestTrue(TEXT("Persona viewport pre-shot camera readable"), bHasPersonaPreShotCamera);
		}

		const FString PersonaScreenshotOutputFile = TEXT("runtimeLogs/editor_viewport_persona_skeletal_mesh_smoke.png");
		TSharedPtr<FJsonObject> PersonaScreenshotJson;
		if (ExecJson(
			TEXT("smoke-editor-viewport-persona-screenshot"),
			TEXT("editor_viewport_screenshot"),
			FString::Printf(
				TEXT("{%s,\"output_file\":\"%s\",\"format\":\"png\",\"max_size\":128,\"warmup_frames\":0,\"restore_after_capture\":true,\"visual_options\":{\"view_mode\":\"lit\",\"overlays\":{\"grid\":false,\"bounds\":true,\"bones\":true,\"bone_draw_mode\":\"all\",\"sockets\":true,\"normals\":false,\"tangents\":false,\"binormals\":false}},\"preview_state\":{\"show_reference_pose\":true},\"focus\":{\"mode\":\"preview_component_bounds\"},\"camera\":{\"orbit\":{\"target\":{\"x\":0,\"y\":0,\"z\":90},\"yaw\":40,\"pitch\":-12,\"distance\":420,\"fov\":38}}}"),
				*PersonaTargetParams,
				*PersonaScreenshotOutputFile),
			bCanRenderViewport ? 200 : 400,
			PersonaScreenshotJson))
		{
			TestEqual(TEXT("Persona viewport screenshot ok matches render capability"), PersonaScreenshotJson->GetBoolField(TEXT("ok")), bCanRenderViewport);
			const TSharedPtr<FJsonObject> Data = GetData(TEXT("Persona viewport screenshot"), PersonaScreenshotJson);
			if (Data.IsValid() && bCanRenderViewport)
			{
				TestEqual(TEXT("Persona viewport screenshot schema"), Data->GetStringField(TEXT("schema")), FString(TEXT("uai.editor_viewport.screenshot.v1")));
				TestEqual(TEXT("Persona viewport screenshot backend"), Data->GetStringField(TEXT("capture_backend")), FString(TEXT("persona_editor_backbuffer")));
				TestTrue(TEXT("Persona viewport screenshot width positive"), Data->GetNumberField(TEXT("width")) > 0.0);
				TestTrue(TEXT("Persona viewport screenshot height positive"), Data->GetNumberField(TEXT("height")) > 0.0);
				const FString ScreenshotPath = Data->GetStringField(TEXT("file_path"));
				TestTrue(TEXT("Persona viewport screenshot file exists"), !ScreenshotPath.IsEmpty() && IFileManager::Get().FileExists(*ScreenshotPath));

				TSharedPtr<FJsonObject> PixelSignalObj;
				TestTrue(TEXT("Persona viewport screenshot pixel signal object"), TryGetNestedObject(Data, TEXT("pixel_signal"), PixelSignalObj));
				if (PixelSignalObj.IsValid())
				{
					TestTrue(TEXT("Persona viewport screenshot nonblank"), PixelSignalObj->GetBoolField(TEXT("nonblank")));
				}

				TSharedPtr<FJsonObject> RestoreObj;
				TestTrue(TEXT("Persona viewport screenshot restore_result object"), TryGetNestedObject(Data, TEXT("restore_result"), RestoreObj));
				if (RestoreObj.IsValid())
				{
					TestTrue(TEXT("Persona viewport screenshot restored"), RestoreObj->GetBoolField(TEXT("restored")));
					TestTrue(TEXT("Persona viewport screenshot restore succeeded"), RestoreObj->GetBoolField(TEXT("success")));
				}
			}
			else if (Data.IsValid())
			{
				const FString ErrorText = PersonaScreenshotJson->GetStringField(TEXT("error"));
				TestTrue(TEXT("Persona viewport headless screenshot reports capture limitation"), ErrorText.Contains(TEXT("invalid_viewport_size")) || ErrorText.Contains(TEXT("read_pixels_failed")));
				TSharedPtr<FJsonObject> RestoreObj;
				TestTrue(TEXT("Persona viewport headless screenshot restore_result object"), TryGetNestedObject(Data, TEXT("restore_result"), RestoreObj));
				if (RestoreObj.IsValid())
				{
					TestTrue(TEXT("Persona viewport headless screenshot restored after failure"), RestoreObj->GetBoolField(TEXT("restored")));
					TestTrue(TEXT("Persona viewport headless screenshot restore succeeded"), RestoreObj->GetBoolField(TEXT("success")));
				}
			}
		}

		if (bHasPersonaPreShotCamera)
		{
			TSharedPtr<FJsonObject> PersonaPostShotStateJson;
			if (ExecJson(
				TEXT("smoke-editor-viewport-persona-get-state-after-shot"),
				TEXT("editor_viewport_get_state"),
				FString::Printf(TEXT("{%s}"), *PersonaTargetParams),
				200,
				PersonaPostShotStateJson))
			{
				const TSharedPtr<FJsonObject> Data = GetData(TEXT("Persona viewport post-shot state"), PersonaPostShotStateJson);
				FVector PersonaPostShotCameraLocation = FVector::ZeroVector;
				FRotator PersonaPostShotCameraRotation = FRotator::ZeroRotator;
				double PersonaPostShotCameraFov = 0.0;
				if (Data.IsValid() && ReadCamera(Data, PersonaPostShotCameraLocation, PersonaPostShotCameraRotation, PersonaPostShotCameraFov))
				{
					TestTrue(TEXT("Persona viewport screenshot restored camera x"), FMath::IsNearlyEqual(PersonaPostShotCameraLocation.X, PersonaPreShotCameraLocation.X, 0.1));
					TestTrue(TEXT("Persona viewport screenshot restored camera y"), FMath::IsNearlyEqual(PersonaPostShotCameraLocation.Y, PersonaPreShotCameraLocation.Y, 0.1));
					TestTrue(TEXT("Persona viewport screenshot restored camera z"), FMath::IsNearlyEqual(PersonaPostShotCameraLocation.Z, PersonaPreShotCameraLocation.Z, 0.1));
					TestTrue(TEXT("Persona viewport screenshot restored fov"), FMath::IsNearlyEqual(PersonaPostShotCameraFov, PersonaPreShotCameraFov, 0.1));
				}
			}
		}

		TSharedPtr<FJsonObject> PersonaRestorePreviewJson;
		ExecJson(
			TEXT("smoke-editor-viewport-persona-restore-preview-state"),
			TEXT("editor_viewport_set_preview_state"),
			FString::Printf(
				TEXT("{%s,\"preview_state\":{\"show_reference_pose\":%s}}"),
				*PersonaTargetParams,
				BoolJson(bPersonaOriginalReferencePose)),
			200,
			PersonaRestorePreviewJson);

		if (bHasPersonaOriginalCamera)
		{
			TSharedPtr<FJsonObject> PersonaRestoreCameraJson;
			ExecJson(
				TEXT("smoke-editor-viewport-persona-restore-camera"),
				TEXT("editor_viewport_set_camera"),
				FString::Printf(
					TEXT("{%s,\"camera\":{\"location\":{\"x\":%.6f,\"y\":%.6f,\"z\":%.6f},\"rotation\":{\"pitch\":%.6f,\"yaw\":%.6f,\"roll\":%.6f},\"fov\":%.6f}}"),
					*PersonaTargetParams,
					PersonaOriginalCameraLocation.X,
					PersonaOriginalCameraLocation.Y,
					PersonaOriginalCameraLocation.Z,
					PersonaOriginalCameraRotation.Pitch,
					PersonaOriginalCameraRotation.Yaw,
					PersonaOriginalCameraRotation.Roll,
					PersonaOriginalCameraFov),
				200,
				PersonaRestoreCameraJson);
		}

		TSharedPtr<FJsonObject> PersonaRestoreVisualJson;
		ExecJson(
			TEXT("smoke-editor-viewport-persona-restore-visual-options"),
			TEXT("editor_viewport_set_visual_options"),
			FString::Printf(
				TEXT("{%s,\"visual_options\":{\"view_mode\":\"%s\",\"overlays\":{\"grid\":%s,\"bounds\":%s,\"bones\":%s,\"bone_draw_mode\":\"%s\",\"local_axes\":%s,\"local_axes_mode\":\"%s\",\"sockets\":%s,\"mesh_sockets\":%s,\"skeleton_sockets\":%s,\"normals\":%s,\"tangents\":%s,\"binormals\":%s},\"realtime\":%s}}"),
				*PersonaTargetParams,
				*PersonaOriginalViewMode,
				BoolJson(bPersonaOriginalGrid),
				BoolJson(bPersonaOriginalBounds),
				BoolJson(bPersonaOriginalBones),
				*PersonaOriginalBoneDrawMode,
				BoolJson(bPersonaOriginalLocalAxes),
				*PersonaOriginalLocalAxesMode,
				BoolJson(bPersonaOriginalSockets),
				BoolJson(bPersonaOriginalMeshSockets),
				BoolJson(bPersonaOriginalSkeletonSockets),
				BoolJson(bPersonaOriginalNormals),
				BoolJson(bPersonaOriginalTangents),
				BoolJson(bPersonaOriginalBinormals),
				BoolJson(bPersonaOriginalRealtime)),
			200,
			PersonaRestoreVisualJson);
	}

	auto VerifyPersonaTarget = [&](const FString& Label, const FString& RequestPrefix, const FString& TargetParams, const FString& ExpectedAssetType, const FString& PreviewPathField, const FString& ExpectedPreviewPath)
	{
		TSharedPtr<FJsonObject> StateJson;
		if (ExecJson(
			RequestPrefix + TEXT("-get-state"),
			TEXT("editor_viewport_get_state"),
			FString::Printf(TEXT("{%s}"), *TargetParams),
			200,
			StateJson))
		{
			TestTrue(Label + TEXT(" get_state ok"), StateJson->GetBoolField(TEXT("ok")));
			const TSharedPtr<FJsonObject> Data = GetData(Label + TEXT(" get_state"), StateJson);
			if (Data.IsValid())
			{
				TestEqual(Label + TEXT(" adapter"), Data->GetStringField(TEXT("adapter")), FString(TEXT("persona")));

				TSharedPtr<FJsonObject> TargetObj;
				TestTrue(Label + TEXT(" target object"), TryGetNestedObject(Data, TEXT("target"), TargetObj));
				if (TargetObj.IsValid())
				{
					TestEqual(Label + TEXT(" target asset_type"), TargetObj->GetStringField(TEXT("asset_type")), ExpectedAssetType);
				}

				TSharedPtr<FJsonObject> PreviewStateObj;
				TestTrue(Label + TEXT(" preview_state object"), TryGetNestedObject(Data, TEXT("preview_state"), PreviewStateObj));
				if (PreviewStateObj.IsValid())
				{
					TestTrue(Label + TEXT(" preview path present"), PreviewStateObj->HasField(PreviewPathField));
					if (PreviewStateObj->HasField(PreviewPathField))
					{
						TestEqual(Label + TEXT(" preview path"), PreviewStateObj->GetStringField(PreviewPathField), ExpectedPreviewPath);
					}
				}

				TSharedPtr<FJsonObject> CapabilitiesObj;
				TestTrue(Label + TEXT(" capabilities object"), TryGetNestedObject(Data, TEXT("capabilities"), CapabilitiesObj));
				if (CapabilitiesObj.IsValid())
				{
					TestTrue(Label + TEXT(" supports camera"), CapabilitiesObj->GetBoolField(TEXT("camera")));
					TestTrue(Label + TEXT(" supports visual options"), CapabilitiesObj->GetBoolField(TEXT("visual_options")));
					TestTrue(Label + TEXT(" supports focus bounds"), ArrayHasString(CapabilitiesObj, TEXT("focus"), TEXT("preview_component_bounds")));
				}
			}
		}

		TSharedPtr<FJsonObject> CameraJson;
		if (ExecJson(
			RequestPrefix + TEXT("-set-camera"),
			TEXT("editor_viewport_set_camera"),
			FString::Printf(
				TEXT("{%s,\"camera\":{\"orbit\":{\"target\":{\"x\":0,\"y\":0,\"z\":90},\"yaw\":20,\"pitch\":-8,\"distance\":380,\"fov\":42}}}"),
				*TargetParams),
			200,
			CameraJson))
		{
			TestTrue(Label + TEXT(" set_camera ok"), CameraJson->GetBoolField(TEXT("ok")));
			const TSharedPtr<FJsonObject> Data = GetData(Label + TEXT(" set_camera"), CameraJson);
			TSharedPtr<FJsonObject> CameraObj;
			if (Data.IsValid() && TryGetNestedObject(Data, TEXT("camera"), CameraObj))
			{
				TestEqual(Label + TEXT(" camera space"), CameraObj->GetStringField(TEXT("space")), FString(TEXT("persona_preview_world")));
			}
		}

		TSharedPtr<FJsonObject> FocusJson;
		if (ExecJson(
			RequestPrefix + TEXT("-focus-bounds"),
			TEXT("editor_viewport_focus"),
			FString::Printf(TEXT("{%s,\"focus\":{\"mode\":\"preview_component_bounds\",\"instant\":true}}"), *TargetParams),
			200,
			FocusJson))
		{
			TestTrue(Label + TEXT(" focus ok"), FocusJson->GetBoolField(TEXT("ok")));
			const TSharedPtr<FJsonObject> Data = GetData(Label + TEXT(" focus"), FocusJson);
			TSharedPtr<FJsonObject> FocusResultObj;
			TestTrue(Label + TEXT(" focus result object"), Data.IsValid() && TryGetNestedObject(Data, TEXT("focus_result"), FocusResultObj));
			if (FocusResultObj.IsValid())
			{
				TestEqual(Label + TEXT(" focus bounds space"), FocusResultObj->GetStringField(TEXT("bounds_space")), FString(TEXT("persona_preview_world")));
			}
		}
	};

	if (bCanRenderViewport && SmokeSkeleton)
	{
		const FString SkeletonTargetParams = FString::Printf(
			TEXT("\"target\":{\"scope\":\"asset_editor\",\"asset_type\":\"skeleton\",\"asset_path\":\"%s\",\"open_editor_if_needed\":true}"),
			*SmokeSkeleton->GetPathName());
		VerifyPersonaTarget(
			TEXT("Persona skeleton viewport"),
			TEXT("smoke-editor-viewport-persona-skeleton"),
			SkeletonTargetParams,
			TEXT("skeleton"),
			TEXT("skeleton_path"),
			SmokeSkeleton->GetPathName());
	}

	if (bCanRenderViewport && SmokeAnimationAsset)
	{
		const FString AnimationTargetParams = FString::Printf(
			TEXT("\"target\":{\"scope\":\"asset_editor\",\"asset_type\":\"animation_asset\",\"asset_path\":\"%s\",\"open_editor_if_needed\":true}"),
			*SmokeAnimationAsset->GetPathName());
		VerifyPersonaTarget(
			TEXT("Persona animation viewport"),
			TEXT("smoke-editor-viewport-persona-animation"),
			AnimationTargetParams,
			TEXT("animation_asset"),
			TEXT("animation_asset_path"),
			SmokeAnimationAsset->GetPathName());
	}

	if (bCanRenderViewport && SmokeAnimBlueprint)
	{
		const FString AnimBlueprintTargetParams = FString::Printf(
			TEXT("\"target\":{\"scope\":\"asset_editor\",\"asset_type\":\"anim_blueprint\",\"asset_path\":\"%s\",\"open_editor_if_needed\":true}"),
			*SmokeAnimBlueprint->GetPathName());
		VerifyPersonaTarget(
			TEXT("Persona anim blueprint viewport"),
			TEXT("smoke-editor-viewport-persona-anim-blueprint"),
			AnimBlueprintTargetParams,
			TEXT("anim_blueprint"),
			TEXT("anim_blueprint_path"),
			SmokeAnimBlueprint->GetPathName());

		TSharedPtr<FJsonObject> AnimBlueprintLegacyCameraJson;
		if (ExecJson(
			TEXT("smoke-editor-viewport-anim-blueprint-legacy-get-camera"),
			TEXT("anim_blueprint_viewport_get_camera"),
			FString::Printf(TEXT("{\"asset_path\":\"%s\",\"open_editor_if_needed\":true}"), *SmokeAnimBlueprint->GetPathName()),
			200,
			AnimBlueprintLegacyCameraJson))
		{
			TestTrue(TEXT("AnimBlueprint legacy viewport get_camera ok"), AnimBlueprintLegacyCameraJson->GetBoolField(TEXT("ok")));
			const TSharedPtr<FJsonObject> Data = GetData(TEXT("AnimBlueprint legacy viewport get_camera"), AnimBlueprintLegacyCameraJson);
			if (Data.IsValid())
			{
				TestTrue(TEXT("AnimBlueprint legacy viewport wrapper"), Data->GetBoolField(TEXT("compat_wrapper")));
				TestEqual(TEXT("AnimBlueprint legacy viewport replacement"), Data->GetStringField(TEXT("replacement_command")), FString(TEXT("editor_viewport_get_state")));
				TestEqual(TEXT("AnimBlueprint legacy viewport asset kind"), Data->GetStringField(TEXT("asset_kind")), FString(TEXT("anim_blueprint")));
				TestTrue(TEXT("AnimBlueprint legacy viewport location field"), Data->HasTypedField<EJson::Object>(TEXT("location")));
			}
		}
	}

	if (bCanRenderViewport && SmokeBlueprintSCS)
	{
		const FString BlueprintSCSTargetParams = FString::Printf(
			TEXT("\"target\":{\"scope\":\"asset_editor\",\"asset_type\":\"blueprint\",\"asset_path\":\"%s\",\"open_editor_if_needed\":true}"),
			*SmokeBlueprintSCS->GetPathName());
		FVector BlueprintOriginalCameraLocation = FVector::ZeroVector;
		FRotator BlueprintOriginalCameraRotation = FRotator::ZeroRotator;
		double BlueprintOriginalCameraFov = 90.0;
		bool bHasBlueprintOriginalCamera = false;
		FString BlueprintOriginalViewMode = TEXT("lit");
		bool bBlueprintOriginalGrid = true;
		bool bBlueprintOriginalFloor = true;
		bool bBlueprintOriginalBounds = false;
		bool bBlueprintOriginalRealtime = false;
		FString FirstBlueprintComponentName;

		TSharedPtr<FJsonObject> BlueprintStateJson;
		if (ExecJson(TEXT("smoke-editor-viewport-blueprint-scs-get-state"), TEXT("editor_viewport_get_state"), FString::Printf(TEXT("{%s}"), *BlueprintSCSTargetParams), 200, BlueprintStateJson))
		{
			TestTrue(TEXT("Blueprint SCS viewport get_state ok"), BlueprintStateJson->GetBoolField(TEXT("ok")));
			const TSharedPtr<FJsonObject> Data = GetData(TEXT("Blueprint SCS viewport get_state"), BlueprintStateJson);
			if (Data.IsValid())
			{
				TestEqual(TEXT("Blueprint SCS viewport adapter"), Data->GetStringField(TEXT("adapter")), FString(TEXT("blueprint_scs")));
				TSharedPtr<FJsonObject> CameraObj;
				TestTrue(TEXT("Blueprint SCS viewport camera object"), TryGetNestedObject(Data, TEXT("camera"), CameraObj));
				if (CameraObj.IsValid())
				{
					TestEqual(TEXT("Blueprint SCS viewport camera space"), CameraObj->GetStringField(TEXT("space")), FString(TEXT("blueprint_preview_world")));
					bHasBlueprintOriginalCamera = ReadCamera(Data, BlueprintOriginalCameraLocation, BlueprintOriginalCameraRotation, BlueprintOriginalCameraFov);
					TestTrue(TEXT("Blueprint SCS viewport original camera readable"), bHasBlueprintOriginalCamera);
				}

				TSharedPtr<FJsonObject> CapabilitiesObj;
				TestTrue(TEXT("Blueprint SCS viewport capabilities object"), TryGetNestedObject(Data, TEXT("capabilities"), CapabilitiesObj));
				if (CapabilitiesObj.IsValid())
				{
					TestTrue(TEXT("Blueprint SCS viewport supports camera"), CapabilitiesObj->GetBoolField(TEXT("camera")));
					TestTrue(TEXT("Blueprint SCS viewport supports visual options"), CapabilitiesObj->GetBoolField(TEXT("visual_options")));
					TestTrue(TEXT("Blueprint SCS viewport supports screenshot"), CapabilitiesObj->GetBoolField(TEXT("screenshot")));
					TestTrue(TEXT("Blueprint SCS viewport supports component focus"), ArrayHasString(CapabilitiesObj, TEXT("focus"), TEXT("component")));
					TestTrue(TEXT("Blueprint SCS viewport supports selection focus"), ArrayHasString(CapabilitiesObj, TEXT("focus"), TEXT("selection")));
					TestTrue(TEXT("Blueprint SCS viewport supports grid option"), ArrayHasString(CapabilitiesObj, TEXT("visual_option_keys"), TEXT("grid")));
					TestTrue(TEXT("Blueprint SCS viewport supports floor option"), ArrayHasString(CapabilitiesObj, TEXT("visual_option_keys"), TEXT("floor")));
				}

				TSharedPtr<FJsonObject> PreviewStateObj;
				TestTrue(TEXT("Blueprint SCS viewport preview_state object"), TryGetNestedObject(Data, TEXT("preview_state"), PreviewStateObj));
				if (PreviewStateObj.IsValid())
				{
					TestEqual(TEXT("Blueprint SCS viewport preview context"), PreviewStateObj->GetStringField(TEXT("context")), FString(TEXT("blueprint_scs")));
					TestEqual(TEXT("Blueprint SCS viewport blueprint path"), PreviewStateObj->GetStringField(TEXT("blueprint_path")), SmokeBlueprintSCS->GetPathName());
					const TArray<TSharedPtr<FJsonValue>>* Components = nullptr;
					if (PreviewStateObj->TryGetArrayField(TEXT("components"), Components) && Components && Components->Num() > 0)
					{
						FirstBlueprintComponentName = (*Components)[0]->AsString();
					}
					TestFalse(TEXT("Blueprint SCS viewport component list not empty"), FirstBlueprintComponentName.IsEmpty());
				}

				TSharedPtr<FJsonObject> VisualOptionsObj;
				TestTrue(TEXT("Blueprint SCS viewport visual options object"), TryGetNestedObject(Data, TEXT("visual_options"), VisualOptionsObj));
				if (VisualOptionsObj.IsValid())
				{
					VisualOptionsObj->TryGetStringField(TEXT("view_mode"), BlueprintOriginalViewMode);
					VisualOptionsObj->TryGetBoolField(TEXT("realtime"), bBlueprintOriginalRealtime);
					TSharedPtr<FJsonObject> OverlaysObj;
					if (TryGetNestedObject(VisualOptionsObj, TEXT("overlays"), OverlaysObj))
					{
						OverlaysObj->TryGetBoolField(TEXT("grid"), bBlueprintOriginalGrid);
						OverlaysObj->TryGetBoolField(TEXT("floor"), bBlueprintOriginalFloor);
						OverlaysObj->TryGetBoolField(TEXT("bounds"), bBlueprintOriginalBounds);
					}
				}
			}
		}

		TSharedPtr<FJsonObject> BlueprintCameraJson;
		if (ExecJson(
			TEXT("smoke-editor-viewport-blueprint-scs-set-camera"),
			TEXT("editor_viewport_set_camera"),
			FString::Printf(TEXT("{%s,\"camera\":{\"orbit\":{\"target\":{\"x\":0,\"y\":0,\"z\":80},\"yaw\":32,\"pitch\":-18,\"distance\":420,\"fov\":41}}}"), *BlueprintSCSTargetParams),
			200,
			BlueprintCameraJson))
		{
			TestTrue(TEXT("Blueprint SCS viewport set_camera ok"), BlueprintCameraJson->GetBoolField(TEXT("ok")));
			const TSharedPtr<FJsonObject> Data = GetData(TEXT("Blueprint SCS viewport set_camera"), BlueprintCameraJson);
			TSharedPtr<FJsonObject> CameraObj;
			if (Data.IsValid() && TryGetNestedObject(Data, TEXT("camera"), CameraObj))
			{
				TestEqual(TEXT("Blueprint SCS viewport set_camera space"), CameraObj->GetStringField(TEXT("space")), FString(TEXT("blueprint_preview_world")));
				TestTrue(TEXT("Blueprint SCS viewport set_camera fov"), FMath::IsNearlyEqual(CameraObj->GetNumberField(TEXT("fov")), 41.0, 0.1));
			}
		}

		TSharedPtr<FJsonObject> BlueprintLegacyCameraJson;
		if (ExecJson(
			TEXT("smoke-editor-viewport-blueprint-scs-legacy-set-camera"),
			TEXT("blueprint_viewport_set_camera"),
			FString::Printf(
				TEXT("{\"asset_path\":\"%s\",\"location\":{\"x\":-260,\"y\":-320,\"z\":180},\"rotation\":{\"pitch\":-18,\"yaw\":35,\"roll\":0},\"fov\":43,\"open_editor_if_needed\":true}"),
				*SmokeBlueprintSCS->GetPathName()),
			200,
			BlueprintLegacyCameraJson))
		{
			TestTrue(TEXT("Blueprint legacy viewport set_camera ok"), BlueprintLegacyCameraJson->GetBoolField(TEXT("ok")));
			const TSharedPtr<FJsonObject> Data = GetData(TEXT("Blueprint legacy viewport set_camera"), BlueprintLegacyCameraJson);
			if (Data.IsValid())
			{
				TestTrue(TEXT("Blueprint legacy viewport wrapper"), Data->GetBoolField(TEXT("compat_wrapper")));
				TestEqual(TEXT("Blueprint legacy viewport replacement"), Data->GetStringField(TEXT("replacement_command")), FString(TEXT("editor_viewport_set_camera")));
				TestTrue(TEXT("Blueprint legacy viewport location field"), Data->HasTypedField<EJson::Object>(TEXT("location")));
				TestTrue(TEXT("Blueprint legacy viewport fov"), FMath::IsNearlyEqual(Data->GetNumberField(TEXT("fov")), 43.0, 0.1));
			}
		}

		TSharedPtr<FJsonObject> BlueprintActorFocusJson;
		if (ExecJson(
			TEXT("smoke-editor-viewport-blueprint-scs-focus-actor"),
			TEXT("editor_viewport_focus"),
			FString::Printf(TEXT("{%s,\"focus\":{\"mode\":\"actor_bounds\",\"instant\":true}}"), *BlueprintSCSTargetParams),
			200,
			BlueprintActorFocusJson))
		{
			TestTrue(TEXT("Blueprint SCS viewport actor focus ok"), BlueprintActorFocusJson->GetBoolField(TEXT("ok")));
			const TSharedPtr<FJsonObject> Data = GetData(TEXT("Blueprint SCS viewport actor focus"), BlueprintActorFocusJson);
			TSharedPtr<FJsonObject> FocusResultObj;
			TestTrue(TEXT("Blueprint SCS viewport actor focus result object"), Data.IsValid() && TryGetNestedObject(Data, TEXT("focus_result"), FocusResultObj));
			if (FocusResultObj.IsValid())
			{
				TestEqual(TEXT("Blueprint SCS viewport actor focus mode"), FocusResultObj->GetStringField(TEXT("mode")), FString(TEXT("actor_bounds")));
				TestEqual(TEXT("Blueprint SCS viewport actor focus bounds space"), FocusResultObj->GetStringField(TEXT("bounds_space")), FString(TEXT("blueprint_preview_world")));
			}
		}

		if (!FirstBlueprintComponentName.IsEmpty())
		{
			TSharedPtr<FJsonObject> BlueprintComponentFocusJson;
			if (ExecJson(
				TEXT("smoke-editor-viewport-blueprint-scs-focus-component"),
				TEXT("editor_viewport_focus"),
				FString::Printf(TEXT("{%s,\"focus\":{\"mode\":\"component\",\"component_name\":\"%s\",\"instant\":true}}"), *BlueprintSCSTargetParams, *FirstBlueprintComponentName),
				200,
				BlueprintComponentFocusJson))
			{
				TestTrue(TEXT("Blueprint SCS viewport component focus ok"), BlueprintComponentFocusJson->GetBoolField(TEXT("ok")));
				const TSharedPtr<FJsonObject> Data = GetData(TEXT("Blueprint SCS viewport component focus"), BlueprintComponentFocusJson);
				TSharedPtr<FJsonObject> FocusResultObj;
				TestTrue(TEXT("Blueprint SCS viewport component focus result object"), Data.IsValid() && TryGetNestedObject(Data, TEXT("focus_result"), FocusResultObj));
				if (FocusResultObj.IsValid())
				{
					TestEqual(TEXT("Blueprint SCS viewport component focus mode"), FocusResultObj->GetStringField(TEXT("mode")), FString(TEXT("component")));
				}
			}
		}

		TSharedPtr<FJsonObject> BlueprintSelectionFocusJson;
		if (ExecJson(
			TEXT("smoke-editor-viewport-blueprint-scs-focus-selection"),
			TEXT("editor_viewport_focus"),
			FString::Printf(TEXT("{%s,\"focus\":{\"mode\":\"selection\",\"instant\":true}}"), *BlueprintSCSTargetParams),
			200,
			BlueprintSelectionFocusJson))
		{
			TestTrue(TEXT("Blueprint SCS viewport selection focus ok"), BlueprintSelectionFocusJson->GetBoolField(TEXT("ok")));
			const TSharedPtr<FJsonObject> Data = GetData(TEXT("Blueprint SCS viewport selection focus"), BlueprintSelectionFocusJson);
			TSharedPtr<FJsonObject> FocusResultObj;
			TestTrue(TEXT("Blueprint SCS viewport selection focus result object"), Data.IsValid() && TryGetNestedObject(Data, TEXT("focus_result"), FocusResultObj));
			if (FocusResultObj.IsValid())
			{
				TestEqual(TEXT("Blueprint SCS viewport selection focus mode"), FocusResultObj->GetStringField(TEXT("mode")), FString(TEXT("selection")));
				TestTrue(TEXT("Blueprint SCS viewport selection focus bounds object"), FocusResultObj->HasTypedField<EJson::Object>(TEXT("bounds")));
			}
		}

		TSharedPtr<FJsonObject> BlueprintVisualJson;
		if (ExecJson(
			TEXT("smoke-editor-viewport-blueprint-scs-visual-options"),
			TEXT("editor_viewport_set_visual_options"),
			FString::Printf(TEXT("{%s,\"visual_options\":{\"view_mode\":\"lit\",\"overlays\":{\"grid\":false,\"floor\":false,\"bounds\":true},\"realtime\":false}}"), *BlueprintSCSTargetParams),
			200,
			BlueprintVisualJson))
		{
			TestTrue(TEXT("Blueprint SCS viewport visual options ok"), BlueprintVisualJson->GetBoolField(TEXT("ok")));
			const TSharedPtr<FJsonObject> Data = GetData(TEXT("Blueprint SCS viewport visual options"), BlueprintVisualJson);
			TSharedPtr<FJsonObject> VisualObj;
			TSharedPtr<FJsonObject> OverlaysObj;
			if (Data.IsValid() && TryGetNestedObject(Data, TEXT("visual_options"), VisualObj) && TryGetNestedObject(VisualObj, TEXT("overlays"), OverlaysObj))
			{
				TestFalse(TEXT("Blueprint SCS viewport visual grid disabled"), OverlaysObj->GetBoolField(TEXT("grid")));
				TestFalse(TEXT("Blueprint SCS viewport visual floor disabled"), OverlaysObj->GetBoolField(TEXT("floor")));
				TestTrue(TEXT("Blueprint SCS viewport visual bounds enabled"), OverlaysObj->GetBoolField(TEXT("bounds")));
			}
		}

		TSharedPtr<FJsonObject> BlueprintPreShotStateJson;
		FVector BlueprintPreShotCameraLocation = FVector::ZeroVector;
		FRotator BlueprintPreShotCameraRotation = FRotator::ZeroRotator;
		double BlueprintPreShotCameraFov = 0.0;
		bool bHasBlueprintPreShotCamera = false;
		if (ExecJson(TEXT("smoke-editor-viewport-blueprint-scs-pre-shot-state"), TEXT("editor_viewport_get_state"), FString::Printf(TEXT("{%s}"), *BlueprintSCSTargetParams), 200, BlueprintPreShotStateJson))
		{
			const TSharedPtr<FJsonObject> Data = GetData(TEXT("Blueprint SCS viewport pre-shot state"), BlueprintPreShotStateJson);
			bHasBlueprintPreShotCamera = Data.IsValid() && ReadCamera(Data, BlueprintPreShotCameraLocation, BlueprintPreShotCameraRotation, BlueprintPreShotCameraFov);
			TestTrue(TEXT("Blueprint SCS viewport pre-shot camera readable"), bHasBlueprintPreShotCamera);
		}

		const FString BlueprintScreenshotOutputFile = TEXT("runtimeLogs/editor_viewport_blueprint_scs_smoke.png");
		TSharedPtr<FJsonObject> BlueprintScreenshotJson;
		if (ExecJson(
			TEXT("smoke-editor-viewport-blueprint-scs-screenshot"),
			TEXT("editor_viewport_screenshot"),
			FString::Printf(
				TEXT("{%s,\"output_file\":\"%s\",\"format\":\"png\",\"max_size\":128,\"warmup_frames\":0,\"restore_after_capture\":true,\"visual_options\":{\"view_mode\":\"lit\",\"overlays\":{\"grid\":false,\"floor\":false,\"bounds\":true}},\"focus\":{\"mode\":\"actor_bounds\"},\"camera\":{\"orbit\":{\"target\":{\"x\":0,\"y\":0,\"z\":80},\"yaw\":35,\"pitch\":-16,\"distance\":430,\"fov\":42}}}"),
				*BlueprintSCSTargetParams,
				*BlueprintScreenshotOutputFile),
			200,
			BlueprintScreenshotJson))
		{
			TestTrue(TEXT("Blueprint SCS viewport screenshot ok"), BlueprintScreenshotJson->GetBoolField(TEXT("ok")));
			const TSharedPtr<FJsonObject> Data = GetData(TEXT("Blueprint SCS viewport screenshot"), BlueprintScreenshotJson);
			if (Data.IsValid())
			{
				TestEqual(TEXT("Blueprint SCS viewport screenshot schema"), Data->GetStringField(TEXT("schema")), FString(TEXT("uai.editor_viewport.screenshot.v1")));
				TestEqual(TEXT("Blueprint SCS viewport screenshot backend"), Data->GetStringField(TEXT("capture_backend")), FString(TEXT("blueprint_scs_backbuffer")));
				TestTrue(TEXT("Blueprint SCS viewport screenshot width positive"), Data->GetNumberField(TEXT("width")) > 0.0);
				TestTrue(TEXT("Blueprint SCS viewport screenshot height positive"), Data->GetNumberField(TEXT("height")) > 0.0);
				const FString ScreenshotPath = Data->GetStringField(TEXT("file_path"));
				TestTrue(TEXT("Blueprint SCS viewport screenshot file exists"), !ScreenshotPath.IsEmpty() && IFileManager::Get().FileExists(*ScreenshotPath));
				TSharedPtr<FJsonObject> PixelSignalObj;
				TestTrue(TEXT("Blueprint SCS viewport screenshot pixel signal object"), TryGetNestedObject(Data, TEXT("pixel_signal"), PixelSignalObj));
				if (PixelSignalObj.IsValid())
				{
					TestTrue(TEXT("Blueprint SCS viewport screenshot nonblank"), PixelSignalObj->GetBoolField(TEXT("nonblank")));
				}
				TSharedPtr<FJsonObject> RestoreObj;
				TestTrue(TEXT("Blueprint SCS viewport screenshot restore_result object"), TryGetNestedObject(Data, TEXT("restore_result"), RestoreObj));
				if (RestoreObj.IsValid())
				{
					TestTrue(TEXT("Blueprint SCS viewport screenshot restored"), RestoreObj->GetBoolField(TEXT("restored")));
					TestTrue(TEXT("Blueprint SCS viewport screenshot restore succeeded"), RestoreObj->GetBoolField(TEXT("success")));
				}
			}
		}

		if (bHasBlueprintPreShotCamera)
		{
			TSharedPtr<FJsonObject> BlueprintPostShotStateJson;
			if (ExecJson(TEXT("smoke-editor-viewport-blueprint-scs-post-shot-state"), TEXT("editor_viewport_get_state"), FString::Printf(TEXT("{%s}"), *BlueprintSCSTargetParams), 200, BlueprintPostShotStateJson))
			{
				const TSharedPtr<FJsonObject> Data = GetData(TEXT("Blueprint SCS viewport post-shot state"), BlueprintPostShotStateJson);
				FVector BlueprintPostShotCameraLocation = FVector::ZeroVector;
				FRotator BlueprintPostShotCameraRotation = FRotator::ZeroRotator;
				double BlueprintPostShotCameraFov = 0.0;
				if (Data.IsValid() && ReadCamera(Data, BlueprintPostShotCameraLocation, BlueprintPostShotCameraRotation, BlueprintPostShotCameraFov))
				{
					TestTrue(TEXT("Blueprint SCS viewport screenshot restored camera x"), FMath::IsNearlyEqual(BlueprintPostShotCameraLocation.X, BlueprintPreShotCameraLocation.X, 0.1));
					TestTrue(TEXT("Blueprint SCS viewport screenshot restored camera y"), FMath::IsNearlyEqual(BlueprintPostShotCameraLocation.Y, BlueprintPreShotCameraLocation.Y, 0.1));
					TestTrue(TEXT("Blueprint SCS viewport screenshot restored camera z"), FMath::IsNearlyEqual(BlueprintPostShotCameraLocation.Z, BlueprintPreShotCameraLocation.Z, 0.1));
					TestTrue(TEXT("Blueprint SCS viewport screenshot restored fov"), FMath::IsNearlyEqual(BlueprintPostShotCameraFov, BlueprintPreShotCameraFov, 0.1));
				}
			}
		}

		if (bHasBlueprintOriginalCamera)
		{
			TSharedPtr<FJsonObject> BlueprintRestoreCameraJson;
			ExecJson(
				TEXT("smoke-editor-viewport-blueprint-scs-restore-camera"),
				TEXT("editor_viewport_set_camera"),
				FString::Printf(
					TEXT("{%s,\"camera\":{\"location\":{\"x\":%.6f,\"y\":%.6f,\"z\":%.6f},\"rotation\":{\"pitch\":%.6f,\"yaw\":%.6f,\"roll\":%.6f},\"fov\":%.6f}}"),
					*BlueprintSCSTargetParams,
					BlueprintOriginalCameraLocation.X,
					BlueprintOriginalCameraLocation.Y,
					BlueprintOriginalCameraLocation.Z,
					BlueprintOriginalCameraRotation.Pitch,
					BlueprintOriginalCameraRotation.Yaw,
					BlueprintOriginalCameraRotation.Roll,
					BlueprintOriginalCameraFov),
				200,
				BlueprintRestoreCameraJson);
		}

		TSharedPtr<FJsonObject> BlueprintRestoreVisualJson;
		ExecJson(
			TEXT("smoke-editor-viewport-blueprint-scs-restore-visual-options"),
			TEXT("editor_viewport_set_visual_options"),
			FString::Printf(
				TEXT("{%s,\"visual_options\":{\"view_mode\":\"%s\",\"overlays\":{\"grid\":%s,\"floor\":%s,\"bounds\":%s},\"realtime\":%s}}"),
				*BlueprintSCSTargetParams,
				*BlueprintOriginalViewMode,
				BoolJson(bBlueprintOriginalGrid),
				BoolJson(bBlueprintOriginalFloor),
				BoolJson(bBlueprintOriginalBounds),
				BoolJson(bBlueprintOriginalRealtime)),
			200,
			BlueprintRestoreVisualJson);
	}

	if (bCanRenderViewport && SmokeNiagaraSystem)
	{
		const FString NiagaraTargetParams = FString::Printf(
			TEXT("\"target\":{\"scope\":\"asset_editor\",\"asset_type\":\"niagara_system\",\"asset_path\":\"%s\",\"open_editor_if_needed\":true}"),
			*SmokeNiagaraSystem->GetPathName());
		FVector NiagaraOriginalCameraLocation = FVector::ZeroVector;
		FRotator NiagaraOriginalCameraRotation = FRotator::ZeroRotator;
		double NiagaraOriginalCameraFov = 90.0;
		bool bHasNiagaraOriginalCamera = false;
		FString NiagaraOriginalViewMode = TEXT("lit");
		bool bNiagaraOriginalGrid = false;
		bool bNiagaraOriginalBounds = false;
		bool bNiagaraOriginalOriginAxis = false;
		bool bNiagaraOriginalParticleCounts = false;
		bool bNiagaraOriginalRealtime = false;

		TSharedPtr<FJsonObject> NiagaraStateJson;
		if (ExecJson(
			TEXT("smoke-editor-viewport-niagara-get-state"),
			TEXT("editor_viewport_get_state"),
			FString::Printf(TEXT("{%s}"), *NiagaraTargetParams),
			200,
			NiagaraStateJson))
		{
			TestTrue(TEXT("Niagara viewport get_state ok"), NiagaraStateJson->GetBoolField(TEXT("ok")));
			const TSharedPtr<FJsonObject> Data = GetData(TEXT("Niagara viewport get_state"), NiagaraStateJson);
			if (Data.IsValid())
			{
				TestEqual(TEXT("Niagara viewport adapter"), Data->GetStringField(TEXT("adapter")), FString(TEXT("niagara_system")));
				TSharedPtr<FJsonObject> CameraObj;
				TestTrue(TEXT("Niagara viewport camera object"), TryGetNestedObject(Data, TEXT("camera"), CameraObj));
				if (CameraObj.IsValid())
				{
					TestEqual(TEXT("Niagara viewport camera space"), CameraObj->GetStringField(TEXT("space")), FString(TEXT("niagara_preview_world")));
					bHasNiagaraOriginalCamera = ReadCamera(Data, NiagaraOriginalCameraLocation, NiagaraOriginalCameraRotation, NiagaraOriginalCameraFov);
					TestTrue(TEXT("Niagara viewport original camera readable"), bHasNiagaraOriginalCamera);
				}

				TSharedPtr<FJsonObject> CapabilitiesObj;
				TestTrue(TEXT("Niagara viewport capabilities object"), TryGetNestedObject(Data, TEXT("capabilities"), CapabilitiesObj));
				if (CapabilitiesObj.IsValid())
				{
					TestTrue(TEXT("Niagara viewport supports camera"), CapabilitiesObj->GetBoolField(TEXT("camera")));
					TestTrue(TEXT("Niagara viewport supports visual options"), CapabilitiesObj->GetBoolField(TEXT("visual_options")));
					TestTrue(TEXT("Niagara viewport supports preview state"), CapabilitiesObj->GetBoolField(TEXT("preview_state")));
					TestTrue(TEXT("Niagara viewport supports screenshot"), CapabilitiesObj->GetBoolField(TEXT("screenshot")));
					TestTrue(TEXT("Niagara viewport supports focus bounds"), ArrayHasString(CapabilitiesObj, TEXT("focus"), TEXT("preview_component_bounds")));
					TestTrue(TEXT("Niagara viewport supports origin axis option"), ArrayHasString(CapabilitiesObj, TEXT("visual_option_keys"), TEXT("origin_axis")));
					TestTrue(TEXT("Niagara viewport supports particle counts option"), ArrayHasString(CapabilitiesObj, TEXT("visual_option_keys"), TEXT("particle_counts")));
					TestTrue(TEXT("Niagara viewport supports pause_at_frame state"), ArrayHasString(CapabilitiesObj, TEXT("preview_state_keys"), TEXT("pause_at_frame")));
				}

				TSharedPtr<FJsonObject> PreviewStateObj;
				TestTrue(TEXT("Niagara viewport preview_state object"), TryGetNestedObject(Data, TEXT("preview_state"), PreviewStateObj));
				if (PreviewStateObj.IsValid())
				{
					TestEqual(TEXT("Niagara viewport preview context"), PreviewStateObj->GetStringField(TEXT("context")), FString(TEXT("niagara_system")));
					TestEqual(TEXT("Niagara viewport asset path"), PreviewStateObj->GetStringField(TEXT("asset_path")), SmokeNiagaraSystem->GetOutermost()->GetName());
					TestTrue(TEXT("Niagara viewport preview component present"), PreviewStateObj->GetBoolField(TEXT("has_preview_component")));
					TestTrue(TEXT("Niagara viewport particle stats object"), PreviewStateObj->HasTypedField<EJson::Object>(TEXT("particle_stats")));
				}

				TSharedPtr<FJsonObject> VisualOptionsObj;
				TestTrue(TEXT("Niagara viewport visual options object"), TryGetNestedObject(Data, TEXT("visual_options"), VisualOptionsObj));
				if (VisualOptionsObj.IsValid())
				{
					VisualOptionsObj->TryGetStringField(TEXT("view_mode"), NiagaraOriginalViewMode);
					VisualOptionsObj->TryGetBoolField(TEXT("realtime"), bNiagaraOriginalRealtime);
					TSharedPtr<FJsonObject> OverlaysObj;
					if (TryGetNestedObject(VisualOptionsObj, TEXT("overlays"), OverlaysObj))
					{
						OverlaysObj->TryGetBoolField(TEXT("grid"), bNiagaraOriginalGrid);
						OverlaysObj->TryGetBoolField(TEXT("bounds"), bNiagaraOriginalBounds);
						OverlaysObj->TryGetBoolField(TEXT("origin_axis"), bNiagaraOriginalOriginAxis);
						OverlaysObj->TryGetBoolField(TEXT("particle_counts"), bNiagaraOriginalParticleCounts);
					}
				}
			}
		}

		TSharedPtr<FJsonObject> NiagaraCameraJson;
		if (ExecJson(
			TEXT("smoke-editor-viewport-niagara-set-camera"),
			TEXT("editor_viewport_set_camera"),
			FString::Printf(TEXT("{%s,\"camera\":{\"orbit\":{\"target\":{\"x\":0,\"y\":0,\"z\":40},\"yaw\":35,\"pitch\":-14,\"distance\":360,\"fov\":39}}}"), *NiagaraTargetParams),
			200,
			NiagaraCameraJson))
		{
			TestTrue(TEXT("Niagara viewport set_camera ok"), NiagaraCameraJson->GetBoolField(TEXT("ok")));
			const TSharedPtr<FJsonObject> Data = GetData(TEXT("Niagara viewport set_camera"), NiagaraCameraJson);
			TSharedPtr<FJsonObject> CameraObj;
			if (Data.IsValid() && TryGetNestedObject(Data, TEXT("camera"), CameraObj))
			{
				TestEqual(TEXT("Niagara viewport set_camera space"), CameraObj->GetStringField(TEXT("space")), FString(TEXT("niagara_preview_world")));
				TestTrue(TEXT("Niagara viewport set_camera fov"), FMath::IsNearlyEqual(CameraObj->GetNumberField(TEXT("fov")), 39.0, 0.1));
			}
		}

		TSharedPtr<FJsonObject> NiagaraFocusJson;
		if (ExecJson(
			TEXT("smoke-editor-viewport-niagara-focus-bounds"),
			TEXT("editor_viewport_focus"),
			FString::Printf(TEXT("{%s,\"focus\":{\"mode\":\"preview_component_bounds\",\"instant\":true}}"), *NiagaraTargetParams),
			200,
			NiagaraFocusJson))
		{
			TestTrue(TEXT("Niagara viewport focus ok"), NiagaraFocusJson->GetBoolField(TEXT("ok")));
			const TSharedPtr<FJsonObject> Data = GetData(TEXT("Niagara viewport focus"), NiagaraFocusJson);
			TSharedPtr<FJsonObject> FocusResultObj;
			TestTrue(TEXT("Niagara viewport focus result object"), Data.IsValid() && TryGetNestedObject(Data, TEXT("focus_result"), FocusResultObj));
			if (FocusResultObj.IsValid())
			{
				TestEqual(TEXT("Niagara viewport focus mode"), FocusResultObj->GetStringField(TEXT("mode")), FString(TEXT("preview_component_bounds")));
				TestEqual(TEXT("Niagara viewport focus bounds space"), FocusResultObj->GetStringField(TEXT("bounds_space")), FString(TEXT("niagara_preview_world")));
				TestTrue(TEXT("Niagara viewport focus bounds object"), FocusResultObj->HasTypedField<EJson::Object>(TEXT("bounds")));
			}
		}

		TSharedPtr<FJsonObject> NiagaraVisualJson;
		if (ExecJson(
			TEXT("smoke-editor-viewport-niagara-visual-options"),
			TEXT("editor_viewport_set_visual_options"),
			FString::Printf(TEXT("{%s,\"visual_options\":{\"view_mode\":\"lit\",\"overlays\":{\"grid\":false,\"bounds\":true,\"origin_axis\":true,\"particle_counts\":true},\"realtime\":false}}"), *NiagaraTargetParams),
			200,
			NiagaraVisualJson))
		{
			TestTrue(TEXT("Niagara viewport visual options ok"), NiagaraVisualJson->GetBoolField(TEXT("ok")));
			const TSharedPtr<FJsonObject> Data = GetData(TEXT("Niagara viewport visual options"), NiagaraVisualJson);
			TSharedPtr<FJsonObject> VisualObj;
			TSharedPtr<FJsonObject> OverlaysObj;
			if (Data.IsValid() && TryGetNestedObject(Data, TEXT("visual_options"), VisualObj) && TryGetNestedObject(VisualObj, TEXT("overlays"), OverlaysObj))
			{
				TestFalse(TEXT("Niagara viewport visual grid disabled"), OverlaysObj->GetBoolField(TEXT("grid")));
				TestTrue(TEXT("Niagara viewport visual bounds enabled"), OverlaysObj->GetBoolField(TEXT("bounds")));
				TestTrue(TEXT("Niagara viewport visual origin axis enabled"), OverlaysObj->GetBoolField(TEXT("origin_axis")));
				TestTrue(TEXT("Niagara viewport visual particle counts enabled"), OverlaysObj->GetBoolField(TEXT("particle_counts")));
			}
		}

		TSharedPtr<FJsonObject> NiagaraPreviewStateJson;
		if (ExecJson(
			TEXT("smoke-editor-viewport-niagara-pause-at-frame"),
			TEXT("editor_viewport_set_preview_state"),
			FString::Printf(TEXT("{%s,\"preview_state\":{\"pause_at_frame\":3,\"tick_delta_seconds\":0.033333333,\"advance_mode\":\"tick_component\",\"pause\":true}}"), *NiagaraTargetParams),
			200,
			NiagaraPreviewStateJson))
		{
			TestTrue(TEXT("Niagara viewport preview state ok"), NiagaraPreviewStateJson->GetBoolField(TEXT("ok")));
			const TSharedPtr<FJsonObject> Data = GetData(TEXT("Niagara viewport preview state"), NiagaraPreviewStateJson);
			TSharedPtr<FJsonObject> AppliedObj;
			TestTrue(TEXT("Niagara viewport preview_state_applied object"), Data.IsValid() && TryGetNestedObject(Data, TEXT("preview_state_applied"), AppliedObj));
			if (AppliedObj.IsValid())
			{
				TestTrue(TEXT("Niagara viewport pause_at_frame applied"), AppliedObj->GetBoolField(TEXT("applied")));
				TestFalse(TEXT("Niagara viewport preview state token not empty"), AppliedObj->GetStringField(TEXT("preview_state_token")).IsEmpty());
			}
			TSharedPtr<FJsonObject> PreviewStateObj;
			TestTrue(TEXT("Niagara viewport preview_state object after apply"), Data.IsValid() && TryGetNestedObject(Data, TEXT("preview_state"), PreviewStateObj));
			if (PreviewStateObj.IsValid())
			{
				TestTrue(TEXT("Niagara viewport component paused"), PreviewStateObj->GetBoolField(TEXT("component_paused")));
				TestFalse(TEXT("Niagara viewport readback token not empty"), PreviewStateObj->GetStringField(TEXT("preview_state_token")).IsEmpty());
			}
		}

		const FString NiagaraBakerTargetParams = FString::Printf(
			TEXT("\"target\":{\"scope\":\"asset_editor\",\"asset_type\":\"niagara_system\",\"asset_path\":\"%s\",\"viewport_role\":\"baker\",\"open_editor_if_needed\":true}"),
			*SmokeNiagaraSystem->GetPathName());
		TSharedPtr<FJsonObject> NiagaraBakerStateJson;
		if (ExecJson(
			TEXT("smoke-editor-viewport-niagara-baker-get-state"),
			TEXT("editor_viewport_get_state"),
			FString::Printf(TEXT("{%s}"), *NiagaraBakerTargetParams),
			200,
			NiagaraBakerStateJson))
		{
			TestTrue(TEXT("Niagara Baker viewport get_state ok"), NiagaraBakerStateJson->GetBoolField(TEXT("ok")));
			const TSharedPtr<FJsonObject> Data = GetData(TEXT("Niagara Baker viewport get_state"), NiagaraBakerStateJson);
			TSharedPtr<FJsonObject> TargetObj;
			TSharedPtr<FJsonObject> PreviewStateObj;
			TSharedPtr<FJsonObject> CapabilitiesObj;
			if (Data.IsValid())
			{
				TestTrue(TEXT("Niagara Baker target object"), TryGetNestedObject(Data, TEXT("target"), TargetObj));
				if (TargetObj.IsValid())
				{
					TestEqual(TEXT("Niagara Baker target role"), TargetObj->GetStringField(TEXT("viewport_role")), FString(TEXT("baker")));
				}
				TestTrue(TEXT("Niagara Baker preview_state object"), TryGetNestedObject(Data, TEXT("preview_state"), PreviewStateObj));
				if (PreviewStateObj.IsValid())
				{
					TestEqual(TEXT("Niagara Baker preview context"), PreviewStateObj->GetStringField(TEXT("context")), FString(TEXT("niagara_baker")));
					TestTrue(TEXT("Niagara Baker settings object"), PreviewStateObj->HasTypedField<EJson::Object>(TEXT("baker")));
				}
				TestTrue(TEXT("Niagara Baker capabilities object"), TryGetNestedObject(Data, TEXT("capabilities"), CapabilitiesObj));
				if (CapabilitiesObj.IsValid())
				{
					TestTrue(TEXT("Niagara Baker supports baker_frame"), ArrayHasString(CapabilitiesObj, TEXT("preview_state_keys"), TEXT("baker_frame")));
					TestTrue(TEXT("Niagara Baker supports normalized_time"), ArrayHasString(CapabilitiesObj, TEXT("preview_state_keys"), TEXT("normalized_time")));
					TestTrue(TEXT("Niagara Baker screenshot backend listed"), ArrayHasString(CapabilitiesObj, TEXT("screenshot_backends"), TEXT("niagara_baker_editor_backbuffer")));
				}
			}
		}

		TSharedPtr<FJsonObject> NiagaraBakerPreviewJson;
		if (ExecJson(
			TEXT("smoke-editor-viewport-niagara-baker-preview-state"),
			TEXT("editor_viewport_set_preview_state"),
			FString::Printf(TEXT("{%s,\"preview_state\":{\"baker_frame\":2,\"pause\":true,\"reset\":true}}"), *NiagaraBakerTargetParams),
			200,
			NiagaraBakerPreviewJson))
		{
			TestTrue(TEXT("Niagara Baker preview state ok"), NiagaraBakerPreviewJson->GetBoolField(TEXT("ok")));
			const TSharedPtr<FJsonObject> Data = GetData(TEXT("Niagara Baker preview state"), NiagaraBakerPreviewJson);
			TSharedPtr<FJsonObject> AppliedObj;
			TestTrue(TEXT("Niagara Baker preview_state_applied object"), Data.IsValid() && TryGetNestedObject(Data, TEXT("preview_state_applied"), AppliedObj));
			if (AppliedObj.IsValid())
			{
				TestTrue(TEXT("Niagara Baker preview applied"), AppliedObj->GetBoolField(TEXT("applied")));
				TestEqual(TEXT("Niagara Baker preview mode"), AppliedObj->GetStringField(TEXT("mode")), FString(TEXT("seek_absolute_time")));
				TestEqual(TEXT("Niagara Baker time source"), AppliedObj->GetStringField(TEXT("time_source")), FString(TEXT("baker_frame")));
				TestFalse(TEXT("Niagara Baker preview state token not empty"), AppliedObj->GetStringField(TEXT("preview_state_token")).IsEmpty());
			}
		}

		const FString NiagaraBakerScreenshotOutputFile = TEXT("runtimeLogs/editor_viewport_niagara_baker_smoke.png");
		TSharedPtr<FJsonObject> NiagaraBakerScreenshotJson;
		if (ExecJson(
			TEXT("smoke-editor-viewport-niagara-baker-screenshot"),
			TEXT("editor_viewport_screenshot"),
			FString::Printf(
				TEXT("{%s,\"output_file\":\"%s\",\"format\":\"png\",\"max_size\":128,\"warmup_frames\":0,\"restore_after_capture\":true,\"preview_state\":{\"baker_frame\":3,\"pause\":true,\"reset\":true},\"focus\":{\"mode\":\"preview_component_bounds\"},\"camera\":{\"orbit\":{\"target\":{\"x\":0,\"y\":0,\"z\":40},\"yaw\":35,\"pitch\":-14,\"distance\":380,\"fov\":40}}}"),
				*NiagaraBakerTargetParams,
				*NiagaraBakerScreenshotOutputFile),
			200,
			NiagaraBakerScreenshotJson))
		{
			TestTrue(TEXT("Niagara Baker screenshot ok"), NiagaraBakerScreenshotJson->GetBoolField(TEXT("ok")));
			const TSharedPtr<FJsonObject> Data = GetData(TEXT("Niagara Baker screenshot"), NiagaraBakerScreenshotJson);
			if (Data.IsValid())
			{
				TestEqual(TEXT("Niagara Baker screenshot backend"), Data->GetStringField(TEXT("capture_backend")), FString(TEXT("niagara_baker_editor_backbuffer")));
				const FString ScreenshotPath = Data->GetStringField(TEXT("file_path"));
				TestTrue(TEXT("Niagara Baker screenshot file exists"), !ScreenshotPath.IsEmpty() && IFileManager::Get().FileExists(*ScreenshotPath));
				TSharedPtr<FJsonObject> PixelSignalObj;
				TestTrue(TEXT("Niagara Baker screenshot pixel signal object"), TryGetNestedObject(Data, TEXT("pixel_signal"), PixelSignalObj));
				if (PixelSignalObj.IsValid())
				{
					TestTrue(TEXT("Niagara Baker screenshot nonblank"), PixelSignalObj->GetBoolField(TEXT("nonblank")));
				}
			}
		}

		if (SmokeNiagaraSimCache)
		{
			const FString NiagaraSimCacheTargetParams = FString::Printf(
				TEXT("\"target\":{\"scope\":\"asset_editor\",\"asset_type\":\"niagara_sim_cache\",\"asset_path\":\"%s\",\"viewport_role\":\"sim_cache\",\"open_editor_if_needed\":true}"),
				*SmokeNiagaraSimCache->GetPathName());
			TSharedPtr<FJsonObject> NiagaraSimCacheStateJson;
			if (ExecJson(
				TEXT("smoke-editor-viewport-niagara-sim-cache-get-state"),
				TEXT("editor_viewport_get_state"),
				FString::Printf(TEXT("{%s}"), *NiagaraSimCacheTargetParams),
				200,
				NiagaraSimCacheStateJson))
			{
				TestTrue(TEXT("Niagara SimCache viewport get_state ok"), NiagaraSimCacheStateJson->GetBoolField(TEXT("ok")));
				const TSharedPtr<FJsonObject> Data = GetData(TEXT("Niagara SimCache viewport get_state"), NiagaraSimCacheStateJson);
				TSharedPtr<FJsonObject> PreviewStateObj;
				TSharedPtr<FJsonObject> CapabilitiesObj;
				if (Data.IsValid())
				{
					TestTrue(TEXT("Niagara SimCache preview_state object"), TryGetNestedObject(Data, TEXT("preview_state"), PreviewStateObj));
					if (PreviewStateObj.IsValid())
					{
						TestEqual(TEXT("Niagara SimCache preview context"), PreviewStateObj->GetStringField(TEXT("context")), FString(TEXT("niagara_sim_cache")));
						TestTrue(TEXT("Niagara SimCache state object"), PreviewStateObj->HasTypedField<EJson::Object>(TEXT("sim_cache")));
						TestEqual(TEXT("Niagara SimCache active path"), PreviewStateObj->GetStringField(TEXT("active_sim_cache_object_path")), SmokeNiagaraSimCache->GetPathName());
					}
					TestTrue(TEXT("Niagara SimCache capabilities object"), TryGetNestedObject(Data, TEXT("capabilities"), CapabilitiesObj));
					if (CapabilitiesObj.IsValid())
					{
						TestTrue(TEXT("Niagara SimCache supports cache_frame"), ArrayHasString(CapabilitiesObj, TEXT("preview_state_keys"), TEXT("cache_frame")));
						TestTrue(TEXT("Niagara SimCache screenshot backend listed"), ArrayHasString(CapabilitiesObj, TEXT("screenshot_backends"), TEXT("niagara_sim_cache_editor_backbuffer")));
					}
				}
			}

			TSharedPtr<FJsonObject> NiagaraSimCachePreviewJson;
			if (ExecJson(
				TEXT("smoke-editor-viewport-niagara-sim-cache-preview-state"),
				TEXT("editor_viewport_set_preview_state"),
				FString::Printf(TEXT("{%s,\"preview_state\":{\"cache_frame\":1,\"pause\":true,\"reset\":true}}"), *NiagaraSimCacheTargetParams),
				200,
				NiagaraSimCachePreviewJson))
			{
				TestTrue(TEXT("Niagara SimCache preview state ok"), NiagaraSimCachePreviewJson->GetBoolField(TEXT("ok")));
				const TSharedPtr<FJsonObject> Data = GetData(TEXT("Niagara SimCache preview state"), NiagaraSimCachePreviewJson);
				TSharedPtr<FJsonObject> AppliedObj;
				TestTrue(TEXT("Niagara SimCache preview_state_applied object"), Data.IsValid() && TryGetNestedObject(Data, TEXT("preview_state_applied"), AppliedObj));
				if (AppliedObj.IsValid())
				{
					TestTrue(TEXT("Niagara SimCache preview applied"), AppliedObj->GetBoolField(TEXT("applied")));
					TestEqual(TEXT("Niagara SimCache preview mode"), AppliedObj->GetStringField(TEXT("mode")), FString(TEXT("seek_absolute_time")));
					TestEqual(TEXT("Niagara SimCache time source"), AppliedObj->GetStringField(TEXT("time_source")), FString(TEXT("sim_cache_frame")));
				}
			}

			const FString NiagaraSimCacheScreenshotOutputFile = TEXT("runtimeLogs/editor_viewport_niagara_sim_cache_smoke.png");
			TSharedPtr<FJsonObject> NiagaraSimCacheScreenshotJson;
			if (ExecJson(
				TEXT("smoke-editor-viewport-niagara-sim-cache-screenshot"),
				TEXT("editor_viewport_screenshot"),
				FString::Printf(
					TEXT("{%s,\"output_file\":\"%s\",\"format\":\"png\",\"max_size\":128,\"warmup_frames\":0,\"restore_after_capture\":true,\"preview_state\":{\"cache_frame\":2,\"pause\":true,\"reset\":true},\"focus\":{\"mode\":\"preview_component_bounds\"},\"camera\":{\"orbit\":{\"target\":{\"x\":0,\"y\":0,\"z\":40},\"yaw\":35,\"pitch\":-14,\"distance\":380,\"fov\":40}}}"),
					*NiagaraSimCacheTargetParams,
					*NiagaraSimCacheScreenshotOutputFile),
				200,
				NiagaraSimCacheScreenshotJson))
			{
				TestTrue(TEXT("Niagara SimCache screenshot ok"), NiagaraSimCacheScreenshotJson->GetBoolField(TEXT("ok")));
				const TSharedPtr<FJsonObject> Data = GetData(TEXT("Niagara SimCache screenshot"), NiagaraSimCacheScreenshotJson);
				if (Data.IsValid())
				{
					TestEqual(TEXT("Niagara SimCache screenshot backend"), Data->GetStringField(TEXT("capture_backend")), FString(TEXT("niagara_sim_cache_editor_backbuffer")));
					const FString ScreenshotPath = Data->GetStringField(TEXT("file_path"));
					TestTrue(TEXT("Niagara SimCache screenshot file exists"), !ScreenshotPath.IsEmpty() && IFileManager::Get().FileExists(*ScreenshotPath));
					TSharedPtr<FJsonObject> PixelSignalObj;
					TestTrue(TEXT("Niagara SimCache screenshot pixel signal object"), TryGetNestedObject(Data, TEXT("pixel_signal"), PixelSignalObj));
					if (PixelSignalObj.IsValid())
					{
						TestTrue(TEXT("Niagara SimCache screenshot nonblank"), PixelSignalObj->GetBoolField(TEXT("nonblank")));
					}
				}
			}
		}

		TSharedPtr<FJsonObject> NiagaraPreShotStateJson;
		FVector NiagaraPreShotCameraLocation = FVector::ZeroVector;
		FRotator NiagaraPreShotCameraRotation = FRotator::ZeroRotator;
		double NiagaraPreShotCameraFov = 0.0;
		bool bHasNiagaraPreShotCamera = false;
		if (ExecJson(TEXT("smoke-editor-viewport-niagara-pre-shot-state"), TEXT("editor_viewport_get_state"), FString::Printf(TEXT("{%s}"), *NiagaraTargetParams), 200, NiagaraPreShotStateJson))
		{
			const TSharedPtr<FJsonObject> Data = GetData(TEXT("Niagara viewport pre-shot state"), NiagaraPreShotStateJson);
			bHasNiagaraPreShotCamera = Data.IsValid() && ReadCamera(Data, NiagaraPreShotCameraLocation, NiagaraPreShotCameraRotation, NiagaraPreShotCameraFov);
			TestTrue(TEXT("Niagara viewport pre-shot camera readable"), bHasNiagaraPreShotCamera);
		}

		const FString NiagaraScreenshotOutputFile = TEXT("runtimeLogs/editor_viewport_niagara_system_smoke.png");
		TSharedPtr<FJsonObject> NiagaraScreenshotJson;
		if (ExecJson(
			TEXT("smoke-editor-viewport-niagara-screenshot"),
			TEXT("editor_viewport_screenshot"),
			FString::Printf(
				TEXT("{%s,\"output_file\":\"%s\",\"format\":\"png\",\"max_size\":128,\"warmup_frames\":0,\"restore_after_capture\":true,\"visual_options\":{\"view_mode\":\"lit\",\"overlays\":{\"grid\":false,\"bounds\":true,\"origin_axis\":true,\"particle_counts\":true}},\"preview_state\":{\"pause_at_frame\":4,\"tick_delta_seconds\":0.033333333,\"advance_mode\":\"tick_component\",\"pause\":true},\"focus\":{\"mode\":\"preview_component_bounds\"},\"camera\":{\"orbit\":{\"target\":{\"x\":0,\"y\":0,\"z\":40},\"yaw\":35,\"pitch\":-14,\"distance\":380,\"fov\":40}}}"),
				*NiagaraTargetParams,
				*NiagaraScreenshotOutputFile),
			200,
			NiagaraScreenshotJson))
		{
			TestTrue(TEXT("Niagara viewport screenshot ok"), NiagaraScreenshotJson->GetBoolField(TEXT("ok")));
			const TSharedPtr<FJsonObject> Data = GetData(TEXT("Niagara viewport screenshot"), NiagaraScreenshotJson);
			if (Data.IsValid())
			{
				TestEqual(TEXT("Niagara viewport screenshot schema"), Data->GetStringField(TEXT("schema")), FString(TEXT("uai.editor_viewport.screenshot.v1")));
				TestEqual(TEXT("Niagara viewport screenshot backend"), Data->GetStringField(TEXT("capture_backend")), FString(TEXT("niagara_editor_backbuffer")));
				TestTrue(TEXT("Niagara viewport screenshot width positive"), Data->GetNumberField(TEXT("width")) > 0.0);
				TestTrue(TEXT("Niagara viewport screenshot height positive"), Data->GetNumberField(TEXT("height")) > 0.0);
				const FString ScreenshotPath = Data->GetStringField(TEXT("file_path"));
				TestTrue(TEXT("Niagara viewport screenshot file exists"), !ScreenshotPath.IsEmpty() && IFileManager::Get().FileExists(*ScreenshotPath));
				TSharedPtr<FJsonObject> PixelSignalObj;
				TestTrue(TEXT("Niagara viewport screenshot pixel signal object"), TryGetNestedObject(Data, TEXT("pixel_signal"), PixelSignalObj));
				if (PixelSignalObj.IsValid())
				{
					TestTrue(TEXT("Niagara viewport screenshot nonblank"), PixelSignalObj->GetBoolField(TEXT("nonblank")));
				}
				TSharedPtr<FJsonObject> RestoreObj;
				TestTrue(TEXT("Niagara viewport screenshot restore_result object"), TryGetNestedObject(Data, TEXT("restore_result"), RestoreObj));
				if (RestoreObj.IsValid())
				{
					TestTrue(TEXT("Niagara viewport screenshot restored"), RestoreObj->GetBoolField(TEXT("restored")));
					TestTrue(TEXT("Niagara viewport screenshot restore succeeded"), RestoreObj->GetBoolField(TEXT("success")));
				}
			}
		}

		if (bHasNiagaraPreShotCamera)
		{
			TSharedPtr<FJsonObject> NiagaraPostShotStateJson;
			if (ExecJson(TEXT("smoke-editor-viewport-niagara-post-shot-state"), TEXT("editor_viewport_get_state"), FString::Printf(TEXT("{%s}"), *NiagaraTargetParams), 200, NiagaraPostShotStateJson))
			{
				const TSharedPtr<FJsonObject> Data = GetData(TEXT("Niagara viewport post-shot state"), NiagaraPostShotStateJson);
				FVector NiagaraPostShotCameraLocation = FVector::ZeroVector;
				FRotator NiagaraPostShotCameraRotation = FRotator::ZeroRotator;
				double NiagaraPostShotCameraFov = 0.0;
				if (Data.IsValid() && ReadCamera(Data, NiagaraPostShotCameraLocation, NiagaraPostShotCameraRotation, NiagaraPostShotCameraFov))
				{
					TestTrue(TEXT("Niagara viewport screenshot restored camera x"), FMath::IsNearlyEqual(NiagaraPostShotCameraLocation.X, NiagaraPreShotCameraLocation.X, 0.1));
					TestTrue(TEXT("Niagara viewport screenshot restored camera y"), FMath::IsNearlyEqual(NiagaraPostShotCameraLocation.Y, NiagaraPreShotCameraLocation.Y, 0.1));
					TestTrue(TEXT("Niagara viewport screenshot restored camera z"), FMath::IsNearlyEqual(NiagaraPostShotCameraLocation.Z, NiagaraPreShotCameraLocation.Z, 0.1));
					TestTrue(TEXT("Niagara viewport screenshot restored fov"), FMath::IsNearlyEqual(NiagaraPostShotCameraFov, NiagaraPreShotCameraFov, 0.1));
				}
			}
		}

		TSharedPtr<FJsonObject> NiagaraRestorePreviewJson;
		ExecJson(
			TEXT("smoke-editor-viewport-niagara-restore-preview-state"),
			TEXT("editor_viewport_set_preview_state"),
			FString::Printf(TEXT("{%s,\"preview_state\":{\"restore_interactive_preview\":true}}"), *NiagaraTargetParams),
			200,
			NiagaraRestorePreviewJson);
		if (bHasNiagaraOriginalCamera)
		{
			TSharedPtr<FJsonObject> NiagaraRestoreCameraJson;
			ExecJson(
				TEXT("smoke-editor-viewport-niagara-restore-camera"),
				TEXT("editor_viewport_set_camera"),
				FString::Printf(
					TEXT("{%s,\"camera\":{\"location\":{\"x\":%.6f,\"y\":%.6f,\"z\":%.6f},\"rotation\":{\"pitch\":%.6f,\"yaw\":%.6f,\"roll\":%.6f},\"fov\":%.6f}}"),
					*NiagaraTargetParams,
					NiagaraOriginalCameraLocation.X,
					NiagaraOriginalCameraLocation.Y,
					NiagaraOriginalCameraLocation.Z,
					NiagaraOriginalCameraRotation.Pitch,
					NiagaraOriginalCameraRotation.Yaw,
					NiagaraOriginalCameraRotation.Roll,
					NiagaraOriginalCameraFov),
				200,
				NiagaraRestoreCameraJson);
		}
		TSharedPtr<FJsonObject> NiagaraRestoreVisualJson;
		ExecJson(
			TEXT("smoke-editor-viewport-niagara-restore-visual-options"),
			TEXT("editor_viewport_set_visual_options"),
			FString::Printf(
				TEXT("{%s,\"visual_options\":{\"view_mode\":\"%s\",\"overlays\":{\"grid\":%s,\"bounds\":%s,\"origin_axis\":%s,\"particle_counts\":%s},\"realtime\":%s}}"),
				*NiagaraTargetParams,
				*NiagaraOriginalViewMode,
				BoolJson(bNiagaraOriginalGrid),
				BoolJson(bNiagaraOriginalBounds),
				BoolJson(bNiagaraOriginalOriginAxis),
				BoolJson(bNiagaraOriginalParticleCounts),
				BoolJson(bNiagaraOriginalRealtime)),
			200,
		NiagaraRestoreVisualJson);
	}

	if (bCanRenderViewport && SmokeLevelSequence)
	{
		const FString LevelSequenceTargetParams = FString::Printf(
			TEXT("\"target\":{\"scope\":\"asset_editor\",\"asset_path\":\"%s\",\"asset_type\":\"level_sequence\",\"viewport_role\":\"sequence\",\"open_editor_if_needed\":true}"),
			*SmokeLevelSequence->GetPathName());

		TSharedPtr<FJsonObject> LevelSequenceStateJson;
		if (ExecJson(
			TEXT("smoke-editor-viewport-level-sequence-get-state"),
			TEXT("editor_viewport_get_state"),
			FString::Printf(TEXT("{%s}"), *LevelSequenceTargetParams),
			200,
			LevelSequenceStateJson))
		{
			TestTrue(TEXT("LevelSequence viewport get_state ok"), LevelSequenceStateJson->GetBoolField(TEXT("ok")));
			const TSharedPtr<FJsonObject> Data = GetData(TEXT("LevelSequence viewport get_state"), LevelSequenceStateJson);
			if (Data.IsValid())
			{
				TestEqual(TEXT("LevelSequence viewport adapter"), Data->GetStringField(TEXT("adapter")), FString(TEXT("level_sequence")));
				TSharedPtr<FJsonObject> CameraObj;
				TestTrue(TEXT("LevelSequence viewport camera object"), TryGetNestedObject(Data, TEXT("camera"), CameraObj));
				if (CameraObj.IsValid())
				{
					TestEqual(TEXT("LevelSequence viewport camera space"), CameraObj->GetStringField(TEXT("space")), FString(TEXT("editor_world")));
				}
				TSharedPtr<FJsonObject> CapabilitiesObj;
				TestTrue(TEXT("LevelSequence viewport capabilities object"), TryGetNestedObject(Data, TEXT("capabilities"), CapabilitiesObj));
				if (CapabilitiesObj.IsValid())
				{
					TestTrue(TEXT("LevelSequence viewport supports camera"), CapabilitiesObj->GetBoolField(TEXT("camera")));
					TestTrue(TEXT("LevelSequence viewport supports preview state"), CapabilitiesObj->GetBoolField(TEXT("preview_state")));
					TestTrue(TEXT("LevelSequence viewport supports screenshot"), CapabilitiesObj->GetBoolField(TEXT("screenshot")));
					TestTrue(TEXT("LevelSequence viewport supports frame state"), ArrayHasString(CapabilitiesObj, TEXT("preview_state_keys"), TEXT("frame")));
					TestTrue(TEXT("LevelSequence viewport screenshot backend listed"), ArrayHasString(CapabilitiesObj, TEXT("screenshot_backends"), TEXT("level_sequence_level_viewport_backbuffer")));
				}
				TSharedPtr<FJsonObject> PreviewStateObj;
				TestTrue(TEXT("LevelSequence viewport preview_state object"), TryGetNestedObject(Data, TEXT("preview_state"), PreviewStateObj));
				if (PreviewStateObj.IsValid())
				{
					TestEqual(TEXT("LevelSequence viewport preview context"), PreviewStateObj->GetStringField(TEXT("context")), FString(TEXT("level_sequence_editor")));
					TestEqual(TEXT("LevelSequence viewport camera source"), PreviewStateObj->GetStringField(TEXT("camera_source")), FString(TEXT("level_viewport")));
					TestEqual(TEXT("LevelSequence viewport sequence path"), PreviewStateObj->GetStringField(TEXT("sequence_path")), SmokeLevelSequence->GetPathName());
				}
			}
		}

		TSharedPtr<FJsonObject> LevelSequencePreviewJson;
		if (ExecJson(
			TEXT("smoke-editor-viewport-level-sequence-preview-state"),
			TEXT("editor_viewport_set_preview_state"),
			FString::Printf(TEXT("{%s,\"preview_state\":{\"frame\":3,\"camera_source\":\"level_viewport\"}}"), *LevelSequenceTargetParams),
			200,
			LevelSequencePreviewJson))
		{
			TestTrue(TEXT("LevelSequence viewport preview state ok"), LevelSequencePreviewJson->GetBoolField(TEXT("ok")));
			const TSharedPtr<FJsonObject> Data = GetData(TEXT("LevelSequence viewport preview state"), LevelSequencePreviewJson);
			TSharedPtr<FJsonObject> AppliedObj;
			TestTrue(TEXT("LevelSequence viewport preview_state_applied object"), Data.IsValid() && TryGetNestedObject(Data, TEXT("preview_state_applied"), AppliedObj));
			if (AppliedObj.IsValid())
			{
				TestTrue(TEXT("LevelSequence viewport preview applied"), AppliedObj->GetBoolField(TEXT("applied")));
				TestEqual(TEXT("LevelSequence viewport preview mode"), AppliedObj->GetStringField(TEXT("mode")), FString(TEXT("set_local_time")));
				TestEqual(TEXT("LevelSequence viewport time source"), AppliedObj->GetStringField(TEXT("time_source")), FString(TEXT("frame_index")));
				TestEqual(TEXT("LevelSequence viewport applied frame"), static_cast<int32>(AppliedObj->GetNumberField(TEXT("frame_index"))), 3);
			}
			TSharedPtr<FJsonObject> PreviewStateObj;
			TestTrue(TEXT("LevelSequence viewport preview_state readback object"), Data.IsValid() && TryGetNestedObject(Data, TEXT("preview_state"), PreviewStateObj));
			if (PreviewStateObj.IsValid())
			{
				TestEqual(TEXT("LevelSequence viewport readback frame"), static_cast<int32>(PreviewStateObj->GetNumberField(TEXT("frame_index"))), 3);
			}
		}

		const FString LevelSequenceScreenshotOutputFile = TEXT("runtimeLogs/editor_viewport_level_sequence_smoke.png");
		const FVector SequenceShotTarget = ProbeActor ? ProbeActor->GetActorLocation() : FVector(24150.0, 24450.0, 100.0);
		TSharedPtr<FJsonObject> LevelSequenceScreenshotJson;
		if (ExecJson(
			TEXT("smoke-editor-viewport-level-sequence-screenshot"),
			TEXT("editor_viewport_screenshot"),
			FString::Printf(
				TEXT("{%s,\"output_file\":\"%s\",\"format\":\"png\",\"max_size\":128,\"warmup_frames\":0,\"restore_after_capture\":true,\"preview_state\":{\"frame\":4,\"camera_source\":\"level_viewport\"},\"visual_options\":{\"view_mode\":\"lit\",\"overlays\":{\"grid\":false,\"bounds\":true}},\"focus\":{\"mode\":\"custom_bounds\",\"bounds\":{\"min\":{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f},\"max\":{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f}},\"instant\":true},\"camera\":{\"orbit\":{\"target\":{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f},\"yaw\":35,\"pitch\":-18,\"distance\":360,\"fov\":42}}}"),
				*LevelSequenceTargetParams,
				*LevelSequenceScreenshotOutputFile,
				SequenceShotTarget.X - 80.0,
				SequenceShotTarget.Y - 80.0,
				SequenceShotTarget.Z - 80.0,
				SequenceShotTarget.X + 80.0,
				SequenceShotTarget.Y + 80.0,
				SequenceShotTarget.Z + 80.0,
				SequenceShotTarget.X,
				SequenceShotTarget.Y,
				SequenceShotTarget.Z),
			200,
			LevelSequenceScreenshotJson))
		{
			TestTrue(TEXT("LevelSequence viewport screenshot ok"), LevelSequenceScreenshotJson->GetBoolField(TEXT("ok")));
			const TSharedPtr<FJsonObject> Data = GetData(TEXT("LevelSequence viewport screenshot"), LevelSequenceScreenshotJson);
			if (Data.IsValid())
			{
				TestEqual(TEXT("LevelSequence viewport screenshot schema"), Data->GetStringField(TEXT("schema")), FString(TEXT("uai.editor_viewport.screenshot.v1")));
				TestEqual(TEXT("LevelSequence viewport screenshot backend"), Data->GetStringField(TEXT("capture_backend")), FString(TEXT("level_sequence_level_viewport_backbuffer")));
				const FString ScreenshotPath = Data->GetStringField(TEXT("file_path"));
				TestTrue(TEXT("LevelSequence viewport screenshot file exists"), !ScreenshotPath.IsEmpty() && IFileManager::Get().FileExists(*ScreenshotPath));
				TSharedPtr<FJsonObject> PixelSignalObj;
				TestTrue(TEXT("LevelSequence viewport screenshot pixel signal object"), TryGetNestedObject(Data, TEXT("pixel_signal"), PixelSignalObj));
				if (PixelSignalObj.IsValid())
				{
					TestTrue(TEXT("LevelSequence viewport screenshot nonblank"), PixelSignalObj->GetBoolField(TEXT("nonblank")));
				}
				TSharedPtr<FJsonObject> AppliedObj;
				TestTrue(TEXT("LevelSequence viewport screenshot preview_state_applied object"), TryGetNestedObject(Data, TEXT("preview_state_applied"), AppliedObj));
				if (AppliedObj.IsValid())
				{
					TestEqual(TEXT("LevelSequence viewport screenshot applied frame"), static_cast<int32>(AppliedObj->GetNumberField(TEXT("frame_index"))), 4);
				}
				TSharedPtr<FJsonObject> RestoreObj;
				TestTrue(TEXT("LevelSequence viewport screenshot restore_result object"), TryGetNestedObject(Data, TEXT("restore_result"), RestoreObj));
				if (RestoreObj.IsValid())
				{
					TestTrue(TEXT("LevelSequence viewport screenshot restored"), RestoreObj->GetBoolField(TEXT("restored")));
					TestTrue(TEXT("LevelSequence viewport screenshot restore succeeded"), RestoreObj->GetBoolField(TEXT("success")));
				}
			}
		}
	}

	if (bCanRenderViewport && SmokeSkeletalMesh)
	{
		const FString IKSourceRigPath = MakeAutomationAssetPath(TEXT("IKViewportSrc"));
		const FString IKTargetRigPath = MakeAutomationAssetPath(TEXT("IKViewportDst"));
		const FString IKRetargeterPath = MakeAutomationAssetPath(TEXT("IKViewportRTG"));
		const FString IKGoalName = TEXT("ViewportGoal");

		TSharedPtr<FJsonObject> IKCreateSourceRigJson;
		if (ExecJson(
			TEXT("smoke-editor-viewport-ik-create-source-rig"),
			TEXT("ik_rig_create"),
			FString::Printf(TEXT("{\"asset_path\":\"%s\",\"preview_skeletal_mesh\":\"%s\",\"save_after_create\":false}"), *IKSourceRigPath, *SkeletalMeshAssetPath),
			200,
			IKCreateSourceRigJson))
		{
			TestTrue(TEXT("IK viewport create source rig ok"), IKCreateSourceRigJson->GetBoolField(TEXT("ok")));
		}

		TSharedPtr<FJsonObject> IKCreateTargetRigJson;
		if (ExecJson(
			TEXT("smoke-editor-viewport-ik-create-target-rig"),
			TEXT("ik_rig_create"),
			FString::Printf(TEXT("{\"asset_path\":\"%s\",\"preview_skeletal_mesh\":\"%s\",\"save_after_create\":false}"), *IKTargetRigPath, *SkeletalMeshAssetPath),
			200,
			IKCreateTargetRigJson))
		{
			TestTrue(TEXT("IK viewport create target rig ok"), IKCreateTargetRigJson->GetBoolField(TEXT("ok")));
		}

		TSharedPtr<FJsonObject> IKSetSourceRootJson;
		ExecJson(
			TEXT("smoke-editor-viewport-ik-source-root"),
			TEXT("ik_rig_set_retarget_root"),
			FString::Printf(TEXT("{\"asset_path\":\"%s\",\"root_bone_name\":\"%s\",\"save_after_set\":false}"), *IKSourceRigPath, *RootBoneName),
			200,
			IKSetSourceRootJson);

		TSharedPtr<FJsonObject> IKSetTargetRootJson;
		ExecJson(
			TEXT("smoke-editor-viewport-ik-target-root"),
			TEXT("ik_rig_set_retarget_root"),
			FString::Printf(TEXT("{\"asset_path\":\"%s\",\"root_bone_name\":\"%s\",\"save_after_set\":false}"), *IKTargetRigPath, *RootBoneName),
			200,
			IKSetTargetRootJson);

		TSharedPtr<FJsonObject> IKSetSourceGoalJson;
		if (ExecJson(
			TEXT("smoke-editor-viewport-ik-source-goal"),
			TEXT("ik_rig_set_goal"),
			FString::Printf(TEXT("{\"asset_path\":\"%s\",\"goal_name\":\"%s\",\"bone_name\":\"%s\",\"position\":{\"x\":11.0,\"y\":22.0,\"z\":33.0},\"save_after_set\":false}"), *IKSourceRigPath, *IKGoalName, *SecondaryBoneName),
			200,
			IKSetSourceGoalJson))
		{
			TestTrue(TEXT("IK viewport set source goal ok"), IKSetSourceGoalJson->GetBoolField(TEXT("ok")));
		}

		TSharedPtr<FJsonObject> IKSetTargetGoalJson;
		ExecJson(
			TEXT("smoke-editor-viewport-ik-target-goal"),
			TEXT("ik_rig_set_goal"),
			FString::Printf(TEXT("{\"asset_path\":\"%s\",\"goal_name\":\"%s\",\"bone_name\":\"%s\",\"position\":{\"x\":12.0,\"y\":24.0,\"z\":36.0},\"save_after_set\":false}"), *IKTargetRigPath, *IKGoalName, *SecondaryBoneName),
			200,
			IKSetTargetGoalJson);

		TSharedPtr<FJsonObject> IKSetSourceChainJson;
		ExecJson(
			TEXT("smoke-editor-viewport-ik-source-chain"),
			TEXT("ik_rig_set_retarget_chain"),
			FString::Printf(TEXT("{\"asset_path\":\"%s\",\"chain_name\":\"ViewportChain\",\"start_bone_name\":\"%s\",\"end_bone_name\":\"%s\",\"goal_name\":\"%s\",\"save_after_set\":false}"), *IKSourceRigPath, *RootBoneName, *SecondaryBoneName, *IKGoalName),
			200,
			IKSetSourceChainJson);

		TSharedPtr<FJsonObject> IKSetTargetChainJson;
		ExecJson(
			TEXT("smoke-editor-viewport-ik-target-chain"),
			TEXT("ik_rig_set_retarget_chain"),
			FString::Printf(TEXT("{\"asset_path\":\"%s\",\"chain_name\":\"ViewportChain\",\"start_bone_name\":\"%s\",\"end_bone_name\":\"%s\",\"goal_name\":\"%s\",\"save_after_set\":false}"), *IKTargetRigPath, *RootBoneName, *SecondaryBoneName, *IKGoalName),
			200,
			IKSetTargetChainJson);

		const FString IKRigTargetParams = FString::Printf(
			TEXT("\"target\":{\"scope\":\"asset_editor\",\"asset_type\":\"ik_rig\",\"asset_path\":\"%s\",\"open_editor_if_needed\":true}"),
			*IKSourceRigPath);

		TSharedPtr<FJsonObject> IKRigStateJson;
		if (ExecJson(
			TEXT("smoke-editor-viewport-ik-rig-get-state"),
			TEXT("editor_viewport_get_state"),
			FString::Printf(TEXT("{%s}"), *IKRigTargetParams),
			200,
			IKRigStateJson))
		{
			TestTrue(TEXT("IKRig viewport get_state ok"), IKRigStateJson->GetBoolField(TEXT("ok")));
			const TSharedPtr<FJsonObject> Data = GetData(TEXT("IKRig viewport get_state"), IKRigStateJson);
			if (Data.IsValid())
			{
				TestEqual(TEXT("IKRig viewport adapter"), Data->GetStringField(TEXT("adapter")), FString(TEXT("ik")));
				TSharedPtr<FJsonObject> CameraObj;
				TestTrue(TEXT("IKRig viewport camera object"), TryGetNestedObject(Data, TEXT("camera"), CameraObj));
				if (CameraObj.IsValid())
				{
					TestEqual(TEXT("IKRig viewport camera space"), CameraObj->GetStringField(TEXT("space")), FString(TEXT("ik_preview_world")));
				}
				TSharedPtr<FJsonObject> CapabilitiesObj;
				TestTrue(TEXT("IKRig viewport capabilities object"), TryGetNestedObject(Data, TEXT("capabilities"), CapabilitiesObj));
				if (CapabilitiesObj.IsValid())
				{
					TestTrue(TEXT("IKRig viewport supports goal focus"), ArrayHasString(CapabilitiesObj, TEXT("focus"), TEXT("goal")));
					TestTrue(TEXT("IKRig viewport supports screenshot"), CapabilitiesObj->GetBoolField(TEXT("screenshot")));
				}
				TSharedPtr<FJsonObject> PreviewStateObj;
				TestTrue(TEXT("IKRig viewport preview_state object"), TryGetNestedObject(Data, TEXT("preview_state"), PreviewStateObj));
				if (PreviewStateObj.IsValid())
				{
					TestEqual(TEXT("IKRig viewport context"), PreviewStateObj->GetStringField(TEXT("context")), FString(TEXT("ik_rig")));
					TestTrue(TEXT("IKRig viewport has goals"), PreviewStateObj->GetNumberField(TEXT("goals_count")) >= 1.0);
				}
			}
		}

		TSharedPtr<FJsonObject> IKRigCameraJson;
		if (ExecJson(
			TEXT("smoke-editor-viewport-ik-rig-set-camera"),
			TEXT("editor_viewport_set_camera"),
			FString::Printf(
				TEXT("{%s,\"camera\":{\"orbit\":{\"target\":{\"x\":0,\"y\":0,\"z\":90},\"yaw\":28,\"pitch\":-12,\"distance\":430,\"fov\":43}}}"),
				*IKRigTargetParams),
			200,
			IKRigCameraJson))
		{
			TestTrue(TEXT("IKRig viewport set_camera ok"), IKRigCameraJson->GetBoolField(TEXT("ok")));
			const TSharedPtr<FJsonObject> Data = GetData(TEXT("IKRig viewport set_camera"), IKRigCameraJson);
			TSharedPtr<FJsonObject> CameraObj;
			if (Data.IsValid() && TryGetNestedObject(Data, TEXT("camera"), CameraObj))
			{
				TestEqual(TEXT("IKRig viewport set_camera space"), CameraObj->GetStringField(TEXT("space")), FString(TEXT("ik_preview_world")));
				TestTrue(TEXT("IKRig viewport set_camera fov"), FMath::IsNearlyEqual(CameraObj->GetNumberField(TEXT("fov")), 43.0, 0.1));
			}
		}

		TSharedPtr<FJsonObject> IKBoneFocusJson;
		if (ExecJson(
			TEXT("smoke-editor-viewport-ik-rig-focus-bone"),
			TEXT("editor_viewport_focus"),
			FString::Printf(
				TEXT("{%s,\"focus\":{\"mode\":\"bone\",\"bone\":\"%s\",\"instant\":true,\"radius\":35}}"),
				*IKRigTargetParams,
				*EscapeJsonString(RootBoneName)),
			200,
			IKBoneFocusJson))
		{
			TestTrue(TEXT("IKRig viewport bone focus ok"), IKBoneFocusJson->GetBoolField(TEXT("ok")));
			const TSharedPtr<FJsonObject> Data = GetData(TEXT("IKRig viewport bone focus"), IKBoneFocusJson);
			TSharedPtr<FJsonObject> FocusResultObj;
			TestTrue(TEXT("IKRig viewport bone focus result object"), Data.IsValid() && TryGetNestedObject(Data, TEXT("focus_result"), FocusResultObj));
			if (FocusResultObj.IsValid())
			{
				TestEqual(TEXT("IKRig viewport bone focus mode"), FocusResultObj->GetStringField(TEXT("mode")), FString(TEXT("bone")));
				TestEqual(TEXT("IKRig viewport bone focus bounds space"), FocusResultObj->GetStringField(TEXT("bounds_space")), FString(TEXT("ik_preview_world")));
			}
		}

		TSharedPtr<FJsonObject> IKGoalFocusJson;
		if (ExecJson(
			TEXT("smoke-editor-viewport-ik-rig-focus-goal"),
			TEXT("editor_viewport_focus"),
			FString::Printf(
				TEXT("{%s,\"focus\":{\"mode\":\"goal\",\"goal\":\"%s\",\"instant\":true,\"radius\":35}}"),
				*IKRigTargetParams,
				*IKGoalName),
			200,
			IKGoalFocusJson))
		{
			TestTrue(TEXT("IKRig viewport goal focus ok"), IKGoalFocusJson->GetBoolField(TEXT("ok")));
			const TSharedPtr<FJsonObject> Data = GetData(TEXT("IKRig viewport goal focus"), IKGoalFocusJson);
			TSharedPtr<FJsonObject> FocusResultObj;
			TestTrue(TEXT("IKRig viewport goal focus result object"), Data.IsValid() && TryGetNestedObject(Data, TEXT("focus_result"), FocusResultObj));
			if (FocusResultObj.IsValid())
			{
				TestEqual(TEXT("IKRig viewport goal focus mode"), FocusResultObj->GetStringField(TEXT("mode")), FString(TEXT("goal")));
				TSharedPtr<FJsonObject> GoalObj;
				TestTrue(TEXT("IKRig viewport goal focus data object"), TryGetNestedObject(FocusResultObj, TEXT("goal"), GoalObj));
				if (GoalObj.IsValid())
				{
					TestEqual(TEXT("IKRig viewport goal focus name"), GoalObj->GetStringField(TEXT("goal_name")), IKGoalName);
					TestTrue(TEXT("IKRig viewport goal focus preview location"), GoalObj->HasTypedField<EJson::Object>(TEXT("preview_world_location")));
				}
			}
		}

		TSharedPtr<FJsonObject> IKVisualJson;
		if (ExecJson(
			TEXT("smoke-editor-viewport-ik-rig-visual-options"),
			TEXT("editor_viewport_set_visual_options"),
			FString::Printf(
				TEXT("{%s,\"visual_options\":{\"view_mode\":\"lit\",\"overlays\":{\"grid\":false,\"bounds\":true,\"bones\":true,\"local_axes\":true,\"sockets\":true}}}"),
				*IKRigTargetParams),
			200,
			IKVisualJson))
		{
			TestTrue(TEXT("IKRig viewport visual options ok"), IKVisualJson->GetBoolField(TEXT("ok")));
			const TSharedPtr<FJsonObject> Data = GetData(TEXT("IKRig viewport visual options"), IKVisualJson);
			TSharedPtr<FJsonObject> VisualObj;
			TSharedPtr<FJsonObject> OverlaysObj;
			if (Data.IsValid() && TryGetNestedObject(Data, TEXT("visual_options"), VisualObj) && TryGetNestedObject(VisualObj, TEXT("overlays"), OverlaysObj))
			{
				TestFalse(TEXT("IKRig viewport visual grid disabled"), OverlaysObj->GetBoolField(TEXT("grid")));
				TestTrue(TEXT("IKRig viewport visual bounds enabled"), OverlaysObj->GetBoolField(TEXT("bounds")));
				TestTrue(TEXT("IKRig viewport visual bones enabled"), OverlaysObj->GetBoolField(TEXT("bones")));
			}
		}

		const FString IKRigScreenshotOutputFile = TEXT("runtimeLogs/editor_viewport_ik_rig_smoke.png");
		TSharedPtr<FJsonObject> IKRigScreenshotJson;
		if (ExecJson(
			TEXT("smoke-editor-viewport-ik-rig-screenshot"),
			TEXT("editor_viewport_screenshot"),
			FString::Printf(
				TEXT("{%s,\"output_file\":\"%s\",\"format\":\"png\",\"max_size\":128,\"warmup_frames\":0,\"restore_after_capture\":true,\"visual_options\":{\"view_mode\":\"lit\",\"overlays\":{\"grid\":false,\"bounds\":true,\"bones\":true}},\"focus\":{\"mode\":\"goal\",\"goal\":\"%s\"},\"camera\":{\"orbit\":{\"target\":{\"x\":0,\"y\":0,\"z\":90},\"yaw\":35,\"pitch\":-12,\"distance\":460,\"fov\":42}}}"),
				*IKRigTargetParams,
				*IKRigScreenshotOutputFile,
				*IKGoalName),
			200,
			IKRigScreenshotJson))
		{
			TestTrue(TEXT("IKRig viewport screenshot ok"), IKRigScreenshotJson->GetBoolField(TEXT("ok")));
			const TSharedPtr<FJsonObject> Data = GetData(TEXT("IKRig viewport screenshot"), IKRigScreenshotJson);
			if (Data.IsValid())
			{
				TestEqual(TEXT("IKRig viewport screenshot backend"), Data->GetStringField(TEXT("capture_backend")), FString(TEXT("ik_editor_backbuffer")));
				TestTrue(TEXT("IKRig viewport screenshot width positive"), Data->GetNumberField(TEXT("width")) > 0.0);
				const FString ScreenshotPath = Data->GetStringField(TEXT("file_path"));
				TestTrue(TEXT("IKRig viewport screenshot file exists"), !ScreenshotPath.IsEmpty() && IFileManager::Get().FileExists(*ScreenshotPath));
				TSharedPtr<FJsonObject> PixelSignalObj;
				TestTrue(TEXT("IKRig viewport screenshot pixel signal object"), TryGetNestedObject(Data, TEXT("pixel_signal"), PixelSignalObj));
				if (PixelSignalObj.IsValid())
				{
					TestTrue(TEXT("IKRig viewport screenshot nonblank"), PixelSignalObj->GetBoolField(TEXT("nonblank")));
				}
			}
		}

		TSharedPtr<FJsonObject> IKCreateRetargeterJson;
		if (ExecJson(
			TEXT("smoke-editor-viewport-ik-retargeter-create"),
			TEXT("ik_retargeter_create"),
			FString::Printf(TEXT("{\"asset_path\":\"%s\",\"save_after_create\":false}"), *IKRetargeterPath),
			200,
			IKCreateRetargeterJson))
		{
			TestTrue(TEXT("IKRetargeter viewport create ok"), IKCreateRetargeterJson->GetBoolField(TEXT("ok")));
		}
		TSharedPtr<FJsonObject> IKRetargeterSourceRigJson;
		ExecJson(
			TEXT("smoke-editor-viewport-ik-retargeter-source-rig"),
			TEXT("ik_retargeter_set_ik_rig"),
			FString::Printf(TEXT("{\"asset_path\":\"%s\",\"source_or_target\":\"source\",\"ik_rig_path\":\"%s\",\"save_after_set\":false}"), *IKRetargeterPath, *IKSourceRigPath),
			200,
			IKRetargeterSourceRigJson);
		TSharedPtr<FJsonObject> IKRetargeterTargetRigJson;
		ExecJson(
			TEXT("smoke-editor-viewport-ik-retargeter-target-rig"),
			TEXT("ik_retargeter_set_ik_rig"),
			FString::Printf(TEXT("{\"asset_path\":\"%s\",\"source_or_target\":\"target\",\"ik_rig_path\":\"%s\",\"save_after_set\":false}"), *IKRetargeterPath, *IKTargetRigPath),
			200,
			IKRetargeterTargetRigJson);
		TSharedPtr<FJsonObject> IKRetargeterSourceMeshJson;
		ExecJson(
			TEXT("smoke-editor-viewport-ik-retargeter-source-mesh"),
			TEXT("ik_retargeter_set_preview_mesh"),
			FString::Printf(TEXT("{\"asset_path\":\"%s\",\"source_or_target\":\"source\",\"skeletal_mesh_path\":\"%s\",\"save_after_set\":false}"), *IKRetargeterPath, *SkeletalMeshAssetPath),
			200,
			IKRetargeterSourceMeshJson);
		TSharedPtr<FJsonObject> IKRetargeterTargetMeshJson;
		ExecJson(
			TEXT("smoke-editor-viewport-ik-retargeter-target-mesh"),
			TEXT("ik_retargeter_set_preview_mesh"),
			FString::Printf(TEXT("{\"asset_path\":\"%s\",\"source_or_target\":\"target\",\"skeletal_mesh_path\":\"%s\",\"save_after_set\":false}"), *IKRetargeterPath, *SkeletalMeshAssetPath),
			200,
			IKRetargeterTargetMeshJson);

		TSharedPtr<FJsonObject> IKRetargeterAutoMapJson;
		ExecJson(
			TEXT("smoke-editor-viewport-ik-retargeter-auto-map"),
			TEXT("ik_retargeter_auto_map_chains"),
			FString::Printf(TEXT("{\"asset_path\":\"%s\",\"auto_map_type\":\"exact\",\"force_remap\":true,\"save_after_set\":false}"), *IKRetargeterPath),
			200,
			IKRetargeterAutoMapJson);

		const FString IKRetargeterTargetParams = FString::Printf(
			TEXT("\"target\":{\"scope\":\"asset_editor\",\"asset_type\":\"ik_retargeter\",\"asset_path\":\"%s\",\"viewport_role\":\"target\",\"open_editor_if_needed\":true}"),
			*IKRetargeterPath);

		TSharedPtr<FJsonObject> IKRetargeterOpenGuardJson;
		const FString ExpectedRetargeterOpenError = FApp::IsUnattended()
			? FString(TEXT("ik_retargeter_editor_open_unsupported_unattended"))
			: FString(TEXT("ik_retargeter_editor_open_requires_existing_editor"));
		if (ExecJsonExpectFailure(
			TEXT("smoke-editor-viewport-ik-retargeter-open-guard"),
			TEXT("editor_viewport_get_state"),
			FString::Printf(TEXT("{%s}"), *IKRetargeterTargetParams),
			ExpectedRetargeterOpenError,
			IKRetargeterOpenGuardJson))
		{
			AddInfo(TEXT("IKRetargeter viewport auto-open guard validated."));
		}
	}

	if (bCanRenderViewport && SmokeControlRig)
	{
		const FString ControlRigTargetParams = FString::Printf(
			TEXT("\"target\":{\"scope\":\"asset_editor\",\"asset_type\":\"control_rig\",\"asset_path\":\"%s\",\"open_editor_if_needed\":true}"),
			*SmokeControlRig->GetPathName());

		TSharedPtr<FJsonObject> ControlRigStateJson;
		if (ExecJson(
			TEXT("smoke-editor-viewport-control-rig-get-state"),
			TEXT("editor_viewport_get_state"),
			FString::Printf(TEXT("{%s}"), *ControlRigTargetParams),
			200,
			ControlRigStateJson))
		{
			TestTrue(TEXT("ControlRig viewport get_state ok"), ControlRigStateJson->GetBoolField(TEXT("ok")));
			const TSharedPtr<FJsonObject> Data = GetData(TEXT("ControlRig viewport get_state"), ControlRigStateJson);
			if (Data.IsValid())
			{
				TestEqual(TEXT("ControlRig viewport adapter"), Data->GetStringField(TEXT("adapter")), FString(TEXT("control_rig")));

				TSharedPtr<FJsonObject> TargetObj;
				TestTrue(TEXT("ControlRig viewport target object"), TryGetNestedObject(Data, TEXT("target"), TargetObj));
				if (TargetObj.IsValid())
				{
					TestEqual(TEXT("ControlRig viewport target asset_type"), TargetObj->GetStringField(TEXT("asset_type")), FString(TEXT("control_rig")));
				}

				TSharedPtr<FJsonObject> CameraObj;
				TestTrue(TEXT("ControlRig viewport camera object"), TryGetNestedObject(Data, TEXT("camera"), CameraObj));
				if (CameraObj.IsValid())
				{
					TestEqual(TEXT("ControlRig viewport camera space"), CameraObj->GetStringField(TEXT("space")), FString(TEXT("persona_preview_world")));
				}

				TSharedPtr<FJsonObject> CapabilitiesObj;
				TestTrue(TEXT("ControlRig viewport capabilities object"), TryGetNestedObject(Data, TEXT("capabilities"), CapabilitiesObj));
				if (CapabilitiesObj.IsValid())
				{
					TestTrue(TEXT("ControlRig viewport supports camera"), CapabilitiesObj->GetBoolField(TEXT("camera")));
					TestTrue(TEXT("ControlRig viewport supports visual options"), CapabilitiesObj->GetBoolField(TEXT("visual_options")));
					TestTrue(TEXT("ControlRig viewport supports screenshot"), CapabilitiesObj->GetBoolField(TEXT("screenshot")));
					TestTrue(TEXT("ControlRig viewport supports focus bounds"), ArrayHasString(CapabilitiesObj, TEXT("focus"), TEXT("preview_component_bounds")));
					TestTrue(TEXT("ControlRig viewport supports focus control"), ArrayHasString(CapabilitiesObj, TEXT("focus"), TEXT("control")));
					TestTrue(TEXT("ControlRig viewport supports focus null"), ArrayHasString(CapabilitiesObj, TEXT("focus"), TEXT("null")));
					TestTrue(TEXT("ControlRig viewport supports focus space"), ArrayHasString(CapabilitiesObj, TEXT("focus"), TEXT("space")));
				}

				TSharedPtr<FJsonObject> PreviewStateObj;
				TestTrue(TEXT("ControlRig viewport preview_state object"), TryGetNestedObject(Data, TEXT("preview_state"), PreviewStateObj));
				if (PreviewStateObj.IsValid())
				{
					TestEqual(TEXT("ControlRig viewport context"), PreviewStateObj->GetStringField(TEXT("context")), FString(TEXT("ControlRigBlueprint")));
					if (SmokeControlRigPreviewMesh)
					{
						TestEqual(TEXT("ControlRig viewport preview mesh path"), PreviewStateObj->GetStringField(TEXT("preview_mesh_path")), SmokeControlRigPreviewMesh->GetPathName());
					}
				}
			}
		}

		TSharedPtr<FJsonObject> ControlRigCameraJson;
		if (ExecJson(
			TEXT("smoke-editor-viewport-control-rig-set-camera"),
			TEXT("editor_viewport_set_camera"),
			FString::Printf(
				TEXT("{%s,\"camera\":{\"orbit\":{\"target\":{\"x\":0,\"y\":0,\"z\":90},\"yaw\":30,\"pitch\":-10,\"distance\":420,\"fov\":40}}}"),
				*ControlRigTargetParams),
			200,
			ControlRigCameraJson))
		{
			TestTrue(TEXT("ControlRig viewport set_camera ok"), ControlRigCameraJson->GetBoolField(TEXT("ok")));
			const TSharedPtr<FJsonObject> Data = GetData(TEXT("ControlRig viewport set_camera"), ControlRigCameraJson);
			TSharedPtr<FJsonObject> CameraObj;
			if (Data.IsValid() && TryGetNestedObject(Data, TEXT("camera"), CameraObj))
			{
				TestEqual(TEXT("ControlRig viewport set_camera space"), CameraObj->GetStringField(TEXT("space")), FString(TEXT("persona_preview_world")));
				TestTrue(TEXT("ControlRig viewport set_camera fov"), FMath::IsNearlyEqual(CameraObj->GetNumberField(TEXT("fov")), 40.0, 0.1));
			}
		}

		TSharedPtr<FJsonObject> ControlRigLegacyCameraJson;
		if (ExecJson(
			TEXT("smoke-editor-viewport-control-rig-legacy-set-camera"),
			TEXT("control_rig_viewport_set_camera"),
			FString::Printf(
				TEXT("{\"asset_path\":\"%s\",\"location\":{\"x\":-260,\"y\":-360,\"z\":180},\"rotation\":{\"pitch\":-12,\"yaw\":35,\"roll\":0},\"fov\":44,\"open_editor_if_needed\":true}"),
				*SmokeControlRig->GetPathName()),
			200,
			ControlRigLegacyCameraJson))
		{
			TestTrue(TEXT("ControlRig legacy viewport set_camera ok"), ControlRigLegacyCameraJson->GetBoolField(TEXT("ok")));
			const TSharedPtr<FJsonObject> Data = GetData(TEXT("ControlRig legacy viewport set_camera"), ControlRigLegacyCameraJson);
			if (Data.IsValid())
			{
				TestTrue(TEXT("ControlRig legacy viewport wrapper"), Data->GetBoolField(TEXT("compat_wrapper")));
				TestEqual(TEXT("ControlRig legacy viewport replacement"), Data->GetStringField(TEXT("replacement_command")), FString(TEXT("editor_viewport_set_camera")));
				TestEqual(TEXT("ControlRig legacy viewport asset kind"), Data->GetStringField(TEXT("asset_kind")), FString(TEXT("control_rig")));
				TestTrue(TEXT("ControlRig legacy viewport fov"), FMath::IsNearlyEqual(Data->GetNumberField(TEXT("fov")), 44.0, 0.1));
			}
		}

		TSharedPtr<FJsonObject> ControlRigFocusJson;
		if (ExecJson(
			TEXT("smoke-editor-viewport-control-rig-focus-bounds"),
			TEXT("editor_viewport_focus"),
			FString::Printf(TEXT("{%s,\"focus\":{\"mode\":\"preview_component_bounds\",\"instant\":true}}"), *ControlRigTargetParams),
			200,
			ControlRigFocusJson))
		{
			TestTrue(TEXT("ControlRig viewport focus ok"), ControlRigFocusJson->GetBoolField(TEXT("ok")));
			const TSharedPtr<FJsonObject> Data = GetData(TEXT("ControlRig viewport focus"), ControlRigFocusJson);
			TSharedPtr<FJsonObject> FocusResultObj;
			TestTrue(TEXT("ControlRig viewport focus result object"), Data.IsValid() && TryGetNestedObject(Data, TEXT("focus_result"), FocusResultObj));
			if (FocusResultObj.IsValid())
			{
				TestEqual(TEXT("ControlRig viewport focus mode"), FocusResultObj->GetStringField(TEXT("mode")), FString(TEXT("preview_component_bounds")));
				TestEqual(TEXT("ControlRig viewport focus bounds space"), FocusResultObj->GetStringField(TEXT("bounds_space")), FString(TEXT("persona_preview_world")));
			}
		}

		if (!SmokeControlRigControlName.IsEmpty())
		{
			TSharedPtr<FJsonObject> ControlRigControlFocusJson;
			if (ExecJson(
				TEXT("smoke-editor-viewport-control-rig-focus-control"),
				TEXT("editor_viewport_focus"),
				FString::Printf(
					TEXT("{%s,\"focus\":{\"mode\":\"control\",\"control\":\"%s\",\"instant\":true,\"radius\":35}}"),
					*ControlRigTargetParams,
					*EscapeJsonString(SmokeControlRigControlName)),
				200,
				ControlRigControlFocusJson))
			{
				TestTrue(TEXT("ControlRig viewport control focus ok"), ControlRigControlFocusJson->GetBoolField(TEXT("ok")));
				const TSharedPtr<FJsonObject> Data = GetData(TEXT("ControlRig viewport control focus"), ControlRigControlFocusJson);
				TSharedPtr<FJsonObject> FocusResultObj;
				TestTrue(TEXT("ControlRig viewport control focus result object"), Data.IsValid() && TryGetNestedObject(Data, TEXT("focus_result"), FocusResultObj));
				if (FocusResultObj.IsValid())
				{
					TestEqual(TEXT("ControlRig viewport control focus mode"), FocusResultObj->GetStringField(TEXT("mode")), FString(TEXT("control")));
					TestEqual(TEXT("ControlRig viewport control focus element"), FocusResultObj->GetStringField(TEXT("element_name")), SmokeControlRigControlName);
					TestEqual(TEXT("ControlRig viewport control focus element type"), FocusResultObj->GetStringField(TEXT("element_type")), FString(TEXT("control")));
					TestEqual(TEXT("ControlRig viewport control focus bounds space"), FocusResultObj->GetStringField(TEXT("bounds_space")), FString(TEXT("persona_preview_world")));
					TestTrue(TEXT("ControlRig viewport control focus rig location"), FocusResultObj->HasTypedField<EJson::Object>(TEXT("rig_global_location")));
					TestTrue(TEXT("ControlRig viewport control focus preview location"), FocusResultObj->HasTypedField<EJson::Object>(TEXT("preview_world_location")));
				}
			}
		}

		if (!SmokeControlRigNullName.IsEmpty())
		{
			TSharedPtr<FJsonObject> ControlRigNullFocusJson;
			if (ExecJson(
				TEXT("smoke-editor-viewport-control-rig-focus-null"),
				TEXT("editor_viewport_focus"),
				FString::Printf(
					TEXT("{%s,\"focus\":{\"mode\":\"null\",\"null\":\"%s\",\"instant\":true,\"radius\":35}}"),
					*ControlRigTargetParams,
					*EscapeJsonString(SmokeControlRigNullName)),
				200,
				ControlRigNullFocusJson))
			{
				TestTrue(TEXT("ControlRig viewport null focus ok"), ControlRigNullFocusJson->GetBoolField(TEXT("ok")));
				const TSharedPtr<FJsonObject> Data = GetData(TEXT("ControlRig viewport null focus"), ControlRigNullFocusJson);
				TSharedPtr<FJsonObject> FocusResultObj;
				TestTrue(TEXT("ControlRig viewport null focus result object"), Data.IsValid() && TryGetNestedObject(Data, TEXT("focus_result"), FocusResultObj));
				if (FocusResultObj.IsValid())
				{
					TestEqual(TEXT("ControlRig viewport null focus mode"), FocusResultObj->GetStringField(TEXT("mode")), FString(TEXT("null")));
					TestEqual(TEXT("ControlRig viewport null focus element"), FocusResultObj->GetStringField(TEXT("element_name")), SmokeControlRigNullName);
					TestEqual(TEXT("ControlRig viewport null focus element type"), FocusResultObj->GetStringField(TEXT("element_type")), FString(TEXT("null")));
					TestEqual(TEXT("ControlRig viewport null focus bounds space"), FocusResultObj->GetStringField(TEXT("bounds_space")), FString(TEXT("persona_preview_world")));
				}
			}

			TSharedPtr<FJsonObject> ControlRigSpaceFocusJson;
			if (ExecJson(
				TEXT("smoke-editor-viewport-control-rig-focus-space"),
				TEXT("editor_viewport_focus"),
				FString::Printf(
					TEXT("{%s,\"focus\":{\"mode\":\"space\",\"space\":\"%s\",\"instant\":true,\"radius\":35}}"),
					*ControlRigTargetParams,
					*EscapeJsonString(SmokeControlRigNullName)),
				200,
				ControlRigSpaceFocusJson))
			{
				TestTrue(TEXT("ControlRig viewport space focus ok"), ControlRigSpaceFocusJson->GetBoolField(TEXT("ok")));
				const TSharedPtr<FJsonObject> Data = GetData(TEXT("ControlRig viewport space focus"), ControlRigSpaceFocusJson);
				TSharedPtr<FJsonObject> FocusResultObj;
				TestTrue(TEXT("ControlRig viewport space focus result object"), Data.IsValid() && TryGetNestedObject(Data, TEXT("focus_result"), FocusResultObj));
				if (FocusResultObj.IsValid())
				{
					TestEqual(TEXT("ControlRig viewport space focus mode"), FocusResultObj->GetStringField(TEXT("mode")), FString(TEXT("space")));
					TestEqual(TEXT("ControlRig viewport space focus element"), FocusResultObj->GetStringField(TEXT("element_name")), SmokeControlRigNullName);
					TestEqual(TEXT("ControlRig viewport space focus element type"), FocusResultObj->GetStringField(TEXT("element_type")), FString(TEXT("space")));
					TestEqual(TEXT("ControlRig viewport space focus rig element type"), FocusResultObj->GetStringField(TEXT("rig_element_type")), FString(TEXT("null")));
					TestEqual(TEXT("ControlRig viewport space focus bounds space"), FocusResultObj->GetStringField(TEXT("bounds_space")), FString(TEXT("persona_preview_world")));
				}
			}
		}

		TSharedPtr<FJsonObject> ControlRigVisualJson;
		if (ExecJson(
			TEXT("smoke-editor-viewport-control-rig-visual-options"),
			TEXT("editor_viewport_set_visual_options"),
			FString::Printf(
				TEXT("{%s,\"visual_options\":{\"view_mode\":\"lit\",\"overlays\":{\"grid\":false,\"bounds\":true,\"bones\":true,\"local_axes\":true,\"sockets\":true}}}"),
				*ControlRigTargetParams),
			200,
			ControlRigVisualJson))
		{
			TestTrue(TEXT("ControlRig viewport visual options ok"), ControlRigVisualJson->GetBoolField(TEXT("ok")));
			const TSharedPtr<FJsonObject> Data = GetData(TEXT("ControlRig viewport visual options"), ControlRigVisualJson);
			TSharedPtr<FJsonObject> VisualObj;
			TSharedPtr<FJsonObject> OverlaysObj;
			if (Data.IsValid() && TryGetNestedObject(Data, TEXT("visual_options"), VisualObj) && TryGetNestedObject(VisualObj, TEXT("overlays"), OverlaysObj))
			{
				TestFalse(TEXT("ControlRig viewport visual grid disabled"), OverlaysObj->GetBoolField(TEXT("grid")));
				TestTrue(TEXT("ControlRig viewport visual bounds enabled"), OverlaysObj->GetBoolField(TEXT("bounds")));
				TestTrue(TEXT("ControlRig viewport visual bones enabled"), OverlaysObj->GetBoolField(TEXT("bones")));
				TestTrue(TEXT("ControlRig viewport visual local axes enabled"), OverlaysObj->GetBoolField(TEXT("local_axes")));
				TestTrue(TEXT("ControlRig viewport visual sockets enabled"), OverlaysObj->GetBoolField(TEXT("sockets")));
			}
		}

		const FString ControlRigScreenshotOutputFile = TEXT("runtimeLogs/editor_viewport_control_rig_smoke.png");
		TSharedPtr<FJsonObject> ControlRigScreenshotJson;
		if (ExecJson(
			TEXT("smoke-editor-viewport-control-rig-screenshot"),
			TEXT("editor_viewport_screenshot"),
			FString::Printf(
				TEXT("{%s,\"output_file\":\"%s\",\"format\":\"png\",\"max_size\":128,\"warmup_frames\":0,\"restore_after_capture\":true,\"visual_options\":{\"view_mode\":\"lit\",\"overlays\":{\"grid\":false,\"bounds\":true,\"bones\":true,\"local_axes\":true}},\"focus\":{\"mode\":\"preview_component_bounds\"},\"camera\":{\"orbit\":{\"target\":{\"x\":0,\"y\":0,\"z\":90},\"yaw\":35,\"pitch\":-12,\"distance\":460,\"fov\":42}}}"),
				*ControlRigTargetParams,
				*ControlRigScreenshotOutputFile),
			200,
			ControlRigScreenshotJson))
		{
			TestTrue(TEXT("ControlRig viewport screenshot ok"), ControlRigScreenshotJson->GetBoolField(TEXT("ok")));
			const TSharedPtr<FJsonObject> Data = GetData(TEXT("ControlRig viewport screenshot"), ControlRigScreenshotJson);
			if (Data.IsValid())
			{
				TestEqual(TEXT("ControlRig viewport screenshot schema"), Data->GetStringField(TEXT("schema")), FString(TEXT("uai.editor_viewport.screenshot.v1")));
				TestEqual(TEXT("ControlRig viewport screenshot backend"), Data->GetStringField(TEXT("capture_backend")), FString(TEXT("persona_editor_backbuffer")));
				TestTrue(TEXT("ControlRig viewport screenshot width positive"), Data->GetNumberField(TEXT("width")) > 0.0);
				TestTrue(TEXT("ControlRig viewport screenshot height positive"), Data->GetNumberField(TEXT("height")) > 0.0);
				const FString ScreenshotPath = Data->GetStringField(TEXT("file_path"));
				TestTrue(TEXT("ControlRig viewport screenshot file exists"), !ScreenshotPath.IsEmpty() && IFileManager::Get().FileExists(*ScreenshotPath));
				TSharedPtr<FJsonObject> PixelSignalObj;
				TestTrue(TEXT("ControlRig viewport screenshot pixel signal object"), TryGetNestedObject(Data, TEXT("pixel_signal"), PixelSignalObj));
				if (PixelSignalObj.IsValid())
				{
					TestTrue(TEXT("ControlRig viewport screenshot nonblank"), PixelSignalObj->GetBoolField(TEXT("nonblank")));
				}
				TSharedPtr<FJsonObject> RestoreObj;
				TestTrue(TEXT("ControlRig viewport screenshot restore_result object"), TryGetNestedObject(Data, TEXT("restore_result"), RestoreObj));
				if (RestoreObj.IsValid())
				{
					TestTrue(TEXT("ControlRig viewport screenshot restored"), RestoreObj->GetBoolField(TEXT("restored")));
					TestTrue(TEXT("ControlRig viewport screenshot restore succeeded"), RestoreObj->GetBoolField(TEXT("success")));
				}
			}
		}
	}
	if (!bCanRenderViewport)
	{
		AddInfo(TEXT("Skipping asset editor viewport targets under NullRHI; standalone asset editor layout persistence is not safe in headless windows."));
	}

	if (GEditor && !FApp::IsUnattended() && (CubeMesh || SmokeSkeletalMesh || SmokeSkeleton || SmokeAnimationAsset || SmokeAnimBlueprint || SmokeControlRig || SmokeBlueprintSCS || SmokeNiagaraSystem || SmokeMaterial || SmokePhysicsAsset || SmokeGroomAsset || SmokeLevelSequence))
	{
		if (UAssetEditorSubsystem* AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
		{
			if (CubeMesh)
			{
				AssetEditorSubsystem->CloseAllEditorsForAsset(CubeMesh);
			}
			if (SmokeSkeletalMesh)
			{
				AssetEditorSubsystem->CloseAllEditorsForAsset(SmokeSkeletalMesh);
			}
			if (SmokeSkeleton)
			{
				AssetEditorSubsystem->CloseAllEditorsForAsset(SmokeSkeleton);
			}
			if (SmokeAnimationAsset)
			{
				AssetEditorSubsystem->CloseAllEditorsForAsset(SmokeAnimationAsset);
			}
			if (SmokeAnimBlueprint)
			{
				AssetEditorSubsystem->CloseAllEditorsForAsset(SmokeAnimBlueprint);
			}
			if (SmokeControlRig)
			{
				AssetEditorSubsystem->CloseAllEditorsForAsset(SmokeControlRig);
			}
			if (SmokeBlueprintSCS)
			{
				AssetEditorSubsystem->CloseAllEditorsForAsset(SmokeBlueprintSCS);
			}
			if (SmokeNiagaraSystem)
			{
				AssetEditorSubsystem->CloseAllEditorsForAsset(SmokeNiagaraSystem);
			}
			if (SmokeMaterial)
			{
				AssetEditorSubsystem->CloseAllEditorsForAsset(SmokeMaterial);
			}
			if (SmokePhysicsAsset)
			{
				AssetEditorSubsystem->CloseAllEditorsForAsset(SmokePhysicsAsset);
			}
			if (SmokeGroomAsset)
			{
				AssetEditorSubsystem->CloseAllEditorsForAsset(SmokeGroomAsset);
			}
			if (SmokeLevelSequence)
			{
				AssetEditorSubsystem->CloseAllEditorsForAsset(SmokeLevelSequence);
			}
			if (FSlateApplication::IsInitialized())
			{
				FSlateApplication::Get().Tick();
			}
		}
	}

	if (ProbeActor && EditorWorld && !ProbeActor->IsActorBeingDestroyed())
	{
		EditorWorld->DestroyActor(ProbeActor);
	}

	return !HasAnyErrors();
}

#endif

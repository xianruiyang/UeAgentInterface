#if WITH_DEV_AUTOMATION_TESTS

#include "UeAgentSmokeTestSharedIncludes.inl"

#include "Components/BoxComponent.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/DynamicMeshComponent.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "DynamicMesh/DynamicMeshOverlay.h"
#include "DynamicMeshActor.h"
#include "Engine/StaticMeshActor.h"
#include "Materials/MaterialInterface.h"
#include "UDynamicMesh.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentInterfaceLevelReadonlySmokeTest,
	"GptProjectTest.UeAgentInterface.Smoke.LevelReadonlyQueries",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUeAgentInterfaceLevelReadonlySmokeTest::RunTest(const FString& Parameters)
{
	FScopedUeAgentHttpServer ServerScope;
	FString InitError;
	if (!ServerScope.Initialize(InitError))
	{
		AddError(FString::Printf(TEXT("Initialize failed: %s"), *InitError));
		return false;
	}

	const FString WorldStateBody = MakeExecRequestBody(TEXT("smoke-world-001"), TEXT("get_world_state"));
	FHttpSmokeResult WorldStateResult;
	TestTrue(TEXT("World state request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, WorldStateBody, WorldStateResult));
	TestEqual(TEXT("World state status code"), WorldStateResult.StatusCode, 200);

	TSharedPtr<FJsonObject> WorldStateJson;
	TestTrue(TEXT("World state response parses as JSON"), ParseJson(WorldStateResult.Body, WorldStateJson));
	TestTrue(TEXT("World state root ok=true"), WorldStateJson.IsValid() && WorldStateJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* WorldStateData = nullptr;
	TestTrue(TEXT("World state contains data object"), WorldStateJson.IsValid() && WorldStateJson->TryGetObjectField(TEXT("data"), WorldStateData) && WorldStateData && WorldStateData->IsValid());
	if (WorldStateData && WorldStateData->IsValid())
	{
		TestTrue(TEXT("World state contains map_name"), (*WorldStateData)->HasTypedField<EJson::String>(TEXT("map_name")));
		TestTrue(TEXT("World state contains package_name"), (*WorldStateData)->HasTypedField<EJson::String>(TEXT("package_name")));
		TestTrue(TEXT("World state contains dirty flag"), (*WorldStateData)->HasTypedField<EJson::Boolean>(TEXT("dirty")));
	}

	const FString ViewportBody = MakeExecRequestBody(TEXT("smoke-viewport-001"), TEXT("viewport_get_camera"));
	FHttpSmokeResult ViewportResult;
	TestTrue(TEXT("Viewport camera request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ViewportBody, ViewportResult));
	TestEqual(TEXT("Viewport camera status code"), ViewportResult.StatusCode, 200);

	TSharedPtr<FJsonObject> ViewportJson;
	TestTrue(TEXT("Viewport camera response parses as JSON"), ParseJson(ViewportResult.Body, ViewportJson));
	TestTrue(TEXT("Viewport camera root ok=true"), ViewportJson.IsValid() && ViewportJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* ViewportData = nullptr;
	TestTrue(TEXT("Viewport camera contains data object"), ViewportJson.IsValid() && ViewportJson->TryGetObjectField(TEXT("data"), ViewportData) && ViewportData && ViewportData->IsValid());
	if (ViewportData && ViewportData->IsValid())
	{
		TestTrue(TEXT("Viewport camera contains location"), (*ViewportData)->HasTypedField<EJson::Object>(TEXT("location")));
		TestTrue(TEXT("Viewport camera contains rotation"), (*ViewportData)->HasTypedField<EJson::Object>(TEXT("rotation")));
		TestTrue(TEXT("Viewport camera contains fov"), (*ViewportData)->HasTypedField<EJson::Number>(TEXT("fov")));
		TestTrue(TEXT("Viewport camera contains realtime"), (*ViewportData)->HasTypedField<EJson::Boolean>(TEXT("realtime")));
		const TSharedPtr<FJsonObject>* ViewportInfo = nullptr;
		TestTrue(TEXT("Viewport camera contains viewport info"), (*ViewportData)->TryGetObjectField(TEXT("viewport"), ViewportInfo) && ViewportInfo && ViewportInfo->IsValid());
		if (ViewportInfo && ViewportInfo->IsValid())
		{
			TestTrue(TEXT("Viewport info contains selection reason"), (*ViewportInfo)->HasTypedField<EJson::String>(TEXT("selection_reason")));
			TestTrue(TEXT("Viewport info contains client index"), (*ViewportInfo)->HasTypedField<EJson::Number>(TEXT("viewport_client_index")));
			TestTrue(TEXT("Viewport info contains size"), (*ViewportInfo)->HasTypedField<EJson::Object>(TEXT("size")));
			TestTrue(TEXT("Viewport info contains widget mode"), (*ViewportInfo)->HasTypedField<EJson::String>(TEXT("widget_mode")));
			TestTrue(TEXT("Viewport info contains show flags"), (*ViewportInfo)->HasTypedField<EJson::Object>(TEXT("show_flags")));
			TestTrue(TEXT("Viewport info contains stable view mode name"), (*ViewportInfo)->HasTypedField<EJson::String>(TEXT("view_mode_name")));
			TestTrue(TEXT("Viewport info contains view mode state"), (*ViewportInfo)->HasTypedField<EJson::Object>(TEXT("view_mode_state")));
		}
	}

	FString OriginalViewModeName = TEXT("lit");
	if (ViewportData && ViewportData->IsValid())
	{
		const TSharedPtr<FJsonObject>* ViewportInfo = nullptr;
		if ((*ViewportData)->TryGetObjectField(TEXT("viewport"), ViewportInfo) && ViewportInfo && ViewportInfo->IsValid())
		{
			(*ViewportInfo)->TryGetStringField(TEXT("view_mode_name"), OriginalViewModeName);
		}
	}

	const FString ViewModeCatalogBody = MakeExecRequestBody(TEXT("smoke-viewport-view-modes-list-001"), TEXT("viewport_list_view_modes"));
	FHttpSmokeResult ViewModeCatalogResult;
	TestTrue(TEXT("Viewport list view modes request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ViewModeCatalogBody, ViewModeCatalogResult));
	TestEqual(TEXT("Viewport list view modes status code"), ViewModeCatalogResult.StatusCode, 200);

	TSharedPtr<FJsonObject> ViewModeCatalogJson;
	TestTrue(TEXT("Viewport list view modes parses JSON"), ParseJson(ViewModeCatalogResult.Body, ViewModeCatalogJson));
	TestTrue(TEXT("Viewport list view modes ok"), ViewModeCatalogJson.IsValid() && ViewModeCatalogJson->GetBoolField(TEXT("ok")));
	const TSharedPtr<FJsonObject>* ViewModeCatalogData = nullptr;
	TestTrue(TEXT("Viewport list view modes contains data object"), ViewModeCatalogJson.IsValid() && ViewModeCatalogJson->TryGetObjectField(TEXT("data"), ViewModeCatalogData) && ViewModeCatalogData && ViewModeCatalogData->IsValid());
	if (ViewModeCatalogData && ViewModeCatalogData->IsValid())
	{
		TestTrue(TEXT("Viewport view modes catalog has mode count"), (*ViewModeCatalogData)->GetNumberField(TEXT("mode_count")) > 10.0);
		const TArray<TSharedPtr<FJsonValue>>* ModeValues = nullptr;
		TestTrue(TEXT("Viewport view modes catalog has modes array"), (*ViewModeCatalogData)->TryGetArrayField(TEXT("modes"), ModeValues) && ModeValues);
		bool bFoundLumenOverview = false;
		bool bFoundShaderComplexity = false;
		if (ModeValues)
		{
			for (const TSharedPtr<FJsonValue>& ModeValue : *ModeValues)
			{
				const TSharedPtr<FJsonObject>* ModeObj = nullptr;
				if (!ModeValue.IsValid() || !ModeValue->TryGetObject(ModeObj) || !ModeObj || !ModeObj->IsValid())
				{
					continue;
				}
				FString Key;
				(*ModeObj)->TryGetStringField(TEXT("key"), Key);
				FString Family;
				(*ModeObj)->TryGetStringField(TEXT("visualization_family"), Family);
				bFoundLumenOverview |= (Family == TEXT("lumen") && Key == TEXT("lumen_overview"));
				bFoundShaderComplexity |= Key == TEXT("shader_complexity");
			}
		}
		TestTrue(TEXT("Viewport view modes catalog includes shader complexity"), bFoundShaderComplexity);
		TestTrue(TEXT("Viewport view modes catalog includes lumen overview"), bFoundLumenOverview);
	}

	const FString ViewModeScreenshotBody = MakeExecRequestBody(
		TEXT("smoke-viewport-view-mode-shot-001"),
		TEXT("screenshot_viewport"),
		TEXT("{\"view_mode\":\"lumen:overview\",\"restore_view_mode\":true,\"format\":\"jpg\",\"max_size\":64,\"warmup_frames\":0}"));
	FHttpSmokeResult ViewModeScreenshotResult;
	TestTrue(TEXT("Viewport view mode screenshot request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ViewModeScreenshotBody, ViewModeScreenshotResult, 30.0));
	TestEqual(TEXT("Viewport view mode screenshot status code"), ViewModeScreenshotResult.StatusCode, 200);

	TSharedPtr<FJsonObject> ViewModeScreenshotJson;
	TestTrue(TEXT("Viewport view mode screenshot parses JSON"), ParseJson(ViewModeScreenshotResult.Body, ViewModeScreenshotJson));
	TestTrue(TEXT("Viewport view mode screenshot ok"), ViewModeScreenshotJson.IsValid() && ViewModeScreenshotJson->GetBoolField(TEXT("ok")));
	const TSharedPtr<FJsonObject>* ViewModeScreenshotData = nullptr;
	TestTrue(TEXT("Viewport view mode screenshot contains data object"), ViewModeScreenshotJson.IsValid() && ViewModeScreenshotJson->TryGetObjectField(TEXT("data"), ViewModeScreenshotData) && ViewModeScreenshotData && ViewModeScreenshotData->IsValid());
	if (ViewModeScreenshotData && ViewModeScreenshotData->IsValid())
	{
		const TSharedPtr<FJsonObject>* RequestedMode = nullptr;
		TestTrue(TEXT("Viewport view mode screenshot contains requested mode"), (*ViewModeScreenshotData)->TryGetObjectField(TEXT("requested_view_mode"), RequestedMode) && RequestedMode && RequestedMode->IsValid());
		if (RequestedMode && RequestedMode->IsValid())
		{
			TestEqual(TEXT("Viewport view mode screenshot requested lumen family"), (*RequestedMode)->GetStringField(TEXT("visualization_family")), FString(TEXT("lumen")));
			TestEqual(TEXT("Viewport view mode screenshot requested overview mode"), (*RequestedMode)->GetStringField(TEXT("visualization_mode")), FString(TEXT("Overview")));
		}

		const TSharedPtr<FJsonObject>* AppliedMode = nullptr;
		TestTrue(TEXT("Viewport view mode screenshot contains applied mode"), (*ViewModeScreenshotData)->TryGetObjectField(TEXT("view_mode_applied"), AppliedMode) && AppliedMode && AppliedMode->IsValid());
		if (AppliedMode && AppliedMode->IsValid())
		{
			TestEqual(TEXT("Viewport view mode screenshot applied lumen mode"), (*AppliedMode)->GetStringField(TEXT("view_mode_name")), FString(TEXT("lumen_visualization")));
		}

		const TSharedPtr<FJsonObject>* RestoredMode = nullptr;
		TestTrue(TEXT("Viewport view mode screenshot contains restored mode"), (*ViewModeScreenshotData)->TryGetObjectField(TEXT("view_mode_after_restore"), RestoredMode) && RestoredMode && RestoredMode->IsValid());
		if (RestoredMode && RestoredMode->IsValid())
		{
			TestEqual(TEXT("Viewport view mode screenshot restores previous mode"), (*RestoredMode)->GetStringField(TEXT("view_mode_name")), OriginalViewModeName);
		}
	}

	const FVector ProbeBaseLocation(24000.0f, 24000.0f, 80.0f);
	const FString ProbeLabel = FString::Printf(TEXT("UAI_Smoke_ProbeTransform_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	UWorld* EditorWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	AStaticMeshActor* ProbeActor = EditorWorld ? EditorWorld->SpawnActor<AStaticMeshActor>(ProbeBaseLocation, FRotator::ZeroRotator) : nullptr;
	TestNotNull(TEXT("Probe transform smoke spawned actor"), ProbeActor);
	if (ProbeActor)
	{
		ProbeActor->SetActorLabel(ProbeLabel);
		ProbeActor->SetActorScale3D(FVector(0.35f, 0.35f, 0.35f));
		if (UStaticMesh* CubeMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube")))
		{
			ProbeActor->GetStaticMeshComponent()->SetStaticMesh(CubeMesh);
		}
	}

	const FString ProbeCameraBody = MakeExecRequestBody(
		TEXT("smoke-probe-transform-camera-001"),
		TEXT("viewport_set_camera"),
		TEXT("{\"location\":{\"x\":23720,\"y\":23640,\"z\":210},\"rotation\":{\"pitch\":-18,\"yaw\":52,\"roll\":0},\"fov\":35}"));
	FHttpSmokeResult ProbeCameraResult;
	TestTrue(TEXT("Probe transform viewport camera request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ProbeCameraBody, ProbeCameraResult));
	TestEqual(TEXT("Probe transform viewport camera status code"), ProbeCameraResult.StatusCode, 200);

	const FString ProbeParams = FString::Printf(
		TEXT("{\"id\":\"%s\",\"format\":\"png\",\"max_size\":64,\"warmup_frames\":0,\"write_individual_frames\":false,\"frame_sheet_max_size\":256,\"candidates\":[{\"label\":\"x25\",\"location\":{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f}},{\"label\":\"rot45_scale\",\"rotation\":{\"pitch\":0,\"yaw\":45,\"roll\":0},\"scale\":{\"x\":0.5,\"y\":0.5,\"z\":0.5}}]}"),
		*ProbeLabel,
		ProbeBaseLocation.X + 25.0f,
		ProbeBaseLocation.Y,
		ProbeBaseLocation.Z);
	const FString ProbeBody = MakeExecRequestBody(
		TEXT("smoke-probe-transform-candidates-001"),
		TEXT("probe_actor_transform_candidates"),
		ProbeParams);
	FHttpSmokeResult ProbeResult;
	TestTrue(TEXT("Probe transform candidates request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ProbeBody, ProbeResult, 30.0));
	TestEqual(TEXT("Probe transform candidates status code"), ProbeResult.StatusCode, 200);

	TSharedPtr<FJsonObject> ProbeJson;
	TestTrue(TEXT("Probe transform candidates parses JSON"), ParseJson(ProbeResult.Body, ProbeJson));
	TestTrue(TEXT("Probe transform candidates ok"), ProbeJson.IsValid() && ProbeJson->GetBoolField(TEXT("ok")));
	const TSharedPtr<FJsonObject>* ProbeData = nullptr;
	TestTrue(TEXT("Probe transform candidates contains data"), ProbeJson.IsValid() && ProbeJson->TryGetObjectField(TEXT("data"), ProbeData) && ProbeData && ProbeData->IsValid());
	if (ProbeData && ProbeData->IsValid())
	{
		TestEqual(TEXT("Probe transform candidate_count"), static_cast<int32>(FMath::RoundToInt((*ProbeData)->GetNumberField(TEXT("candidate_count")))), 2);
		TestEqual(TEXT("Probe transform frame_count includes original"), static_cast<int32>(FMath::RoundToInt((*ProbeData)->GetNumberField(TEXT("frame_count")))), 3);
		TestTrue(TEXT("Probe transform restored flag"), (*ProbeData)->GetBoolField(TEXT("restored")));

		const TSharedPtr<FJsonObject>* FinalTransform = nullptr;
		TestTrue(TEXT("Probe transform final transform exists"), (*ProbeData)->TryGetObjectField(TEXT("final_transform"), FinalTransform) && FinalTransform && FinalTransform->IsValid());
		if (FinalTransform && FinalTransform->IsValid())
		{
			const TSharedPtr<FJsonObject>* FinalLocation = nullptr;
			TestTrue(TEXT("Probe transform final location exists"), (*FinalTransform)->TryGetObjectField(TEXT("location"), FinalLocation) && FinalLocation && FinalLocation->IsValid());
			if (FinalLocation && FinalLocation->IsValid())
			{
				TestTrue(TEXT("Probe transform restored location x"), FMath::IsNearlyEqual(static_cast<float>((*FinalLocation)->GetNumberField(TEXT("x"))), ProbeBaseLocation.X, 0.1f));
				TestTrue(TEXT("Probe transform restored location y"), FMath::IsNearlyEqual(static_cast<float>((*FinalLocation)->GetNumberField(TEXT("y"))), ProbeBaseLocation.Y, 0.1f));
			}
		}

		const TSharedPtr<FJsonObject>* FrameSheet = nullptr;
		TestTrue(TEXT("Probe transform frame sheet exists"), (*ProbeData)->TryGetObjectField(TEXT("frame_sheet"), FrameSheet) && FrameSheet && FrameSheet->IsValid());
		if (FrameSheet && FrameSheet->IsValid())
		{
			FString SheetPath;
			TestTrue(TEXT("Probe transform frame sheet has path"), (*FrameSheet)->TryGetStringField(TEXT("file_path"), SheetPath) && !SheetPath.IsEmpty());
			TestTrue(TEXT("Probe transform frame sheet file exists"), !SheetPath.IsEmpty() && IFileManager::Get().FileExists(*SheetPath));
		}
	}

	const FString PixelScaleParams = FString::Printf(
		TEXT("{\"id\":\"%s\",\"target_point\":\"bounds_center\"}"),
		*ProbeLabel);
	const FString PixelScaleBody = MakeExecRequestBody(
		TEXT("smoke-probe-camera-pixel-scale-001"),
		TEXT("probe_camera_pixel_scale"),
		PixelScaleParams);
	FHttpSmokeResult PixelScaleResult;
	TestTrue(TEXT("Probe camera pixel scale request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, PixelScaleBody, PixelScaleResult));
	TestEqual(TEXT("Probe camera pixel scale status code"), PixelScaleResult.StatusCode, 200);

	TSharedPtr<FJsonObject> PixelScaleJson;
	TestTrue(TEXT("Probe camera pixel scale parses JSON"), ParseJson(PixelScaleResult.Body, PixelScaleJson));
	TestTrue(TEXT("Probe camera pixel scale ok"), PixelScaleJson.IsValid() && PixelScaleJson->GetBoolField(TEXT("ok")));
	const TSharedPtr<FJsonObject>* PixelScaleData = nullptr;
	TestTrue(TEXT("Probe camera pixel scale contains data"), PixelScaleJson.IsValid() && PixelScaleJson->TryGetObjectField(TEXT("data"), PixelScaleData) && PixelScaleData && PixelScaleData->IsValid());
	if (PixelScaleData && PixelScaleData->IsValid())
	{
		TestEqual(TEXT("Probe camera pixel scale schema"), (*PixelScaleData)->GetStringField(TEXT("schema")), FString(TEXT("uai.probe.camera_pixel_scale.v1")));
		TestTrue(TEXT("Probe camera pixel scale target is in front alias"), (*PixelScaleData)->GetBoolField(TEXT("in_front")));
		TestTrue(TEXT("Probe camera pixel scale target is in front"), (*PixelScaleData)->GetBoolField(TEXT("in_front_of_camera")));
		TestTrue(TEXT("Probe camera pixel scale valid"), (*PixelScaleData)->GetBoolField(TEXT("pixel_scale_valid")));
		TestTrue(TEXT("Probe camera pixel scale has camera-space location"), (*PixelScaleData)->HasTypedField<EJson::Object>(TEXT("camera_space_location")));

		const TSharedPtr<FJsonObject>* Units = nullptr;
		TestTrue(TEXT("Probe camera pixel scale has units"), (*PixelScaleData)->TryGetObjectField(TEXT("camera_space_units_per_pixel"), Units) && Units && Units->IsValid());
		if (Units && Units->IsValid())
		{
			TestTrue(TEXT("Probe camera pixel scale x positive"), (*Units)->GetNumberField(TEXT("x")) > 0.0);
			TestTrue(TEXT("Probe camera pixel scale y positive"), (*Units)->GetNumberField(TEXT("y")) > 0.0);
		}
	}
	if (ProbeActor && !ProbeActor->IsActorBeingDestroyed())
	{
		ProbeActor->Destroy();
	}

	const FString InteractionStateBody = MakeExecRequestBody(TEXT("smoke-viewport-interaction-get-001"), TEXT("viewport_get_editor_interaction_state"));
	FHttpSmokeResult InteractionStateResult;
	TestTrue(TEXT("Viewport interaction state request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, InteractionStateBody, InteractionStateResult));
	TestEqual(TEXT("Viewport interaction state status code"), InteractionStateResult.StatusCode, 200);

	TSharedPtr<FJsonObject> InteractionStateJson;
	TestTrue(TEXT("Viewport interaction state parses JSON"), ParseJson(InteractionStateResult.Body, InteractionStateJson));
	const TSharedPtr<FJsonObject>* InteractionStateData = nullptr;
	TestTrue(TEXT("Viewport interaction state contains data object"), InteractionStateJson.IsValid() && InteractionStateJson->TryGetObjectField(TEXT("data"), InteractionStateData) && InteractionStateData && InteractionStateData->IsValid());
	if (InteractionStateData && InteractionStateData->IsValid())
	{
		TestTrue(TEXT("Viewport interaction state contains viewport"), (*InteractionStateData)->HasTypedField<EJson::Object>(TEXT("viewport")));
		TestTrue(TEXT("Viewport interaction state contains viewports"), (*InteractionStateData)->HasTypedField<EJson::Array>(TEXT("viewports")));
	}

	const FString GameViewBody = MakeExecRequestBody(TEXT("smoke-viewport-interaction-game-001"), TEXT("viewport_set_game_view"), TEXT("{\"game_view\":true}"));
	FHttpSmokeResult GameViewResult;
	TestTrue(TEXT("Viewport game view enable request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, GameViewBody, GameViewResult));
	TestEqual(TEXT("Viewport game view enable status code"), GameViewResult.StatusCode, 200);

	const FString RestoreInteractionBody = MakeExecRequestBody(TEXT("smoke-viewport-interaction-restore-001"), TEXT("viewport_restore_editor_interaction_state"), TEXT("{\"all_viewports\":true,\"game_view\":false,\"mode_widgets\":true,\"selection\":true,\"pivot\":true,\"widget_mode\":\"translate_if_none\"}"));
	FHttpSmokeResult RestoreInteractionResult;
	TestTrue(TEXT("Viewport interaction restore request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, RestoreInteractionBody, RestoreInteractionResult));
	TestEqual(TEXT("Viewport interaction restore status code"), RestoreInteractionResult.StatusCode, 200);

	TSharedPtr<FJsonObject> RestoreInteractionJson;
	TestTrue(TEXT("Viewport interaction restore parses JSON"), ParseJson(RestoreInteractionResult.Body, RestoreInteractionJson));
	const TSharedPtr<FJsonObject>* RestoreInteractionData = nullptr;
	TestTrue(TEXT("Viewport interaction restore contains data object"), RestoreInteractionJson.IsValid() && RestoreInteractionJson->TryGetObjectField(TEXT("data"), RestoreInteractionData) && RestoreInteractionData && RestoreInteractionData->IsValid());
	if (RestoreInteractionData && RestoreInteractionData->IsValid())
	{
		const TSharedPtr<FJsonObject>* RestoredViewport = nullptr;
		TestTrue(TEXT("Viewport interaction restore contains viewport"), (*RestoreInteractionData)->TryGetObjectField(TEXT("viewport"), RestoredViewport) && RestoredViewport && RestoredViewport->IsValid());
		if (RestoredViewport && RestoredViewport->IsValid())
		{
			TestFalse(TEXT("Viewport interaction restore leaves game_view false"), (*RestoredViewport)->GetBoolField(TEXT("game_view")));
			const TSharedPtr<FJsonObject>* ShowFlags = nullptr;
			TestTrue(TEXT("Viewport interaction restore contains show flags"), (*RestoredViewport)->TryGetObjectField(TEXT("show_flags"), ShowFlags) && ShowFlags && ShowFlags->IsValid());
			if (ShowFlags && ShowFlags->IsValid())
			{
				TestTrue(TEXT("Viewport interaction restore enables mode widgets"), (*ShowFlags)->GetBoolField(TEXT("mode_widgets")));
				TestTrue(TEXT("Viewport interaction restore enables selection"), (*ShowFlags)->GetBoolField(TEXT("selection")));
				TestTrue(TEXT("Viewport interaction restore enables pivot"), (*ShowFlags)->GetBoolField(TEXT("pivot")));
			}
		}
	}

	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentInterfaceViewportWarningsSmokeTest,
	"GptProjectTest.UeAgentInterface.Smoke.ViewportWarnings",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUeAgentInterfaceViewportWarningsSmokeTest::RunTest(const FString& Parameters)
{
	FScopedUeAgentHttpServer ServerScope;
	FString InitError;
	if (!ServerScope.Initialize(InitError))
	{
		AddError(FString::Printf(TEXT("Initialize failed: %s"), *InitError));
		return false;
	}

	const FString FolderPath = FString::Printf(TEXT("UAI/Smoke/ViewportWarnings/%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	auto ExecJson = [this, &ServerScope](const FString& RequestId, const FString& Command, const FString& ParamsJson, FHttpSmokeResult& OutResult, double TimeoutSeconds = 5.0) -> bool
	{
		const FString Body = MakeExecRequestBody(RequestId, Command, ParamsJson);
		const bool bCompleted = ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, Body, OutResult, TimeoutSeconds);
		TestTrue(FString::Printf(TEXT("%s request completed"), *Command), bCompleted);
		return bCompleted;
	};

	FHttpSmokeResult CleanupBeforeResult;
	ExecJson(
		TEXT("smoke-viewport-warnings-clean-before-001"),
		TEXT("level_destroy_folder_actors"),
		FString::Printf(TEXT("{\"folder_path\":\"%s\",\"include_child_folders\":true}"), *FolderPath),
		CleanupBeforeResult);

	auto SpawnActor = [this, &ExecJson, &FolderPath](const FString& RequestId, const FString& ClassPath, const FString& Label, const FVector& Location) -> bool
	{
		FHttpSmokeResult SpawnResult;
		if (!ExecJson(
			RequestId,
			TEXT("spawn_actor"),
			FString::Printf(
				TEXT("{\"class_path\":\"%s\",\"label\":\"%s\",\"folder_path\":\"%s\",\"tags\":[\"UAI.Smoke.ViewportWarnings\"],\"location\":{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f}}"),
				*ClassPath,
				*Label,
				*FolderPath,
				Location.X,
				Location.Y,
				Location.Z),
			SpawnResult,
			10.0))
		{
			return false;
		}
		TestEqual(FString::Printf(TEXT("%s spawn status code"), *Label), SpawnResult.StatusCode, 200);

		TSharedPtr<FJsonObject> SpawnJson;
		TestTrue(FString::Printf(TEXT("%s spawn parses"), *Label), ParseJson(SpawnResult.Body, SpawnJson));
		TestTrue(FString::Printf(TEXT("%s spawn ok"), *Label), SpawnJson.IsValid() && SpawnJson->GetBoolField(TEXT("ok")));
		return SpawnJson.IsValid() && SpawnJson->GetBoolField(TEXT("ok"));
	};

	const FString PlanarLabel = TEXT("UAI_Smoke_ViewportWarnings_PlanarReflection");
	const FString DirALabel = TEXT("UAI_Smoke_ViewportWarnings_DirectionalA");
	const FString DirBLabel = TEXT("UAI_Smoke_ViewportWarnings_DirectionalB");
	SpawnActor(TEXT("smoke-viewport-warnings-planar-001"), TEXT("/Script/Engine.PlanarReflection"), PlanarLabel, FVector(90000.0, 90000.0, 250.0));
	SpawnActor(TEXT("smoke-viewport-warnings-dir-a-001"), TEXT("/Script/Engine.DirectionalLight"), DirALabel, FVector(90200.0, 90000.0, 350.0));
	SpawnActor(TEXT("smoke-viewport-warnings-dir-b-001"), TEXT("/Script/Engine.DirectionalLight"), DirBLabel, FVector(90400.0, 90000.0, 350.0));

	auto SetDirectionalPriority = [this](const FString& ActorLabel, int32 Priority) -> bool
	{
		if (!GEditor)
		{
			AddError(TEXT("Viewport warning smoke missing GEditor"));
			return false;
		}
		UWorld* EditorWorld = GEditor->GetEditorWorldContext().World();
		if (!EditorWorld)
		{
			AddError(TEXT("Viewport warning smoke missing editor world"));
			return false;
		}
		for (TActorIterator<AActor> It(EditorWorld); It; ++It)
		{
			AActor* Actor = *It;
			if (!Actor || !Actor->GetActorLabel().Equals(ActorLabel, ESearchCase::IgnoreCase))
			{
				continue;
			}
			UDirectionalLightComponent* Component = Actor->FindComponentByClass<UDirectionalLightComponent>();
			if (!Component)
			{
				AddError(FString::Printf(TEXT("Directional light component missing: %s"), *ActorLabel));
				return false;
			}
			Actor->Modify();
			Component->Modify();
			Component->SetForwardShadingPriority(Priority);
			Component->MarkRenderStateDirty();
			Actor->MarkPackageDirty();
			return true;
		}
		AddError(FString::Printf(TEXT("Directional actor not found: %s"), *ActorLabel));
		return false;
	};

	TestTrue(TEXT("Directional A priority set"), SetDirectionalPriority(DirALabel, 999));
	TestTrue(TEXT("Directional B priority set"), SetDirectionalPriority(DirBLabel, 999));

	FHttpSmokeResult WarningsResult;
	ExecJson(
		TEXT("smoke-viewport-warnings-read-001"),
		TEXT("viewport_get_warnings"),
		TEXT("{\"include_suppressed\":true}"),
		WarningsResult,
		10.0);
	TestEqual(TEXT("Viewport warnings status code"), WarningsResult.StatusCode, 200);

	TSharedPtr<FJsonObject> WarningsJson;
	TestTrue(TEXT("Viewport warnings parses"), ParseJson(WarningsResult.Body, WarningsJson));
	TestTrue(TEXT("Viewport warnings ok"), WarningsJson.IsValid() && WarningsJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* WarningsData = nullptr;
	TestTrue(TEXT("Viewport warnings data exists"), WarningsJson.IsValid() && WarningsJson->TryGetObjectField(TEXT("data"), WarningsData) && WarningsData && WarningsData->IsValid());
	if (WarningsData && WarningsData->IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* WarningValues = nullptr;
		TestTrue(TEXT("Viewport warnings array exists"), (*WarningsData)->TryGetArrayField(TEXT("warnings"), WarningValues) && WarningValues);
		bool bFoundForwardWarning = false;
		bool bFoundPlanarWarning = false;
		if (WarningValues)
		{
			for (const TSharedPtr<FJsonValue>& WarningValue : *WarningValues)
			{
				const TSharedPtr<FJsonObject>* WarningObj = nullptr;
				if (!WarningValue.IsValid() || !WarningValue->TryGetObject(WarningObj) || !WarningObj || !WarningObj->IsValid())
				{
					continue;
				}
				const FString Code = (*WarningObj)->GetStringField(TEXT("code"));
				const FString Text = (*WarningObj)->GetStringField(TEXT("text"));
				bFoundForwardWarning |= Code == TEXT("renderer.multiple_directional_lights_forward_shading") && Text.Contains(TEXT("ForwardShadingPriority"));
				bFoundPlanarWarning |= Code == TEXT("renderer.no_global_clip_plane");
			}
		}
		TestTrue(TEXT("Viewport warnings include directional conflict"), bFoundForwardWarning);

		const TSharedPtr<FJsonObject>* RendererProbe = nullptr;
		if ((*WarningsData)->TryGetObjectField(TEXT("renderer_probe"), RendererProbe) && RendererProbe && RendererProbe->IsValid())
		{
			const int32 ClipPlaneValue = (*RendererProbe)->GetIntegerField(TEXT("r.AllowGlobalClipPlane"));
			if (ClipPlaneValue == 0)
			{
				TestTrue(TEXT("Viewport warnings include planar clip plane warning when cvar disabled"), bFoundPlanarWarning);
			}
		}
	}

	FHttpSmokeResult CleanupAfterResult;
	ExecJson(
		TEXT("smoke-viewport-warnings-clean-after-001"),
		TEXT("level_destroy_folder_actors"),
		FString::Printf(TEXT("{\"folder_path\":\"%s\",\"include_child_folders\":true}"), *FolderPath),
		CleanupAfterResult,
		10.0);
	TestEqual(TEXT("Viewport warnings cleanup status code"), CleanupAfterResult.StatusCode, 200);

	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentInterfaceLevelCollisionQueriesSmokeTest,
	"GptProjectTest.UeAgentInterface.Smoke.LevelCollisionQueries",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUeAgentInterfaceLevelCollisionQueriesSmokeTest::RunTest(const FString& Parameters)
{
	FScopedUeAgentHttpServer ServerScope;
	FString InitError;
	if (!ServerScope.Initialize(InitError))
	{
		AddError(FString::Printf(TEXT("Initialize failed: %s"), *InitError));
		return false;
	}

	const FString UniqueSuffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString ActorLabel = FString::Printf(TEXT("SmokeCollisionCube_%s"), *UniqueSuffix);
	const FVector ActorLocation(50000.0f, 50000.0f, 100.0f);
	const FVector ActorScale(5.0f, 5.0f, 5.0f);

	const FString SpawnBody = MakeExecRequestBody(
		TEXT("smoke-collision-spawn-001"),
		TEXT("spawn_actor"),
		FString::Printf(
			TEXT("{\"class_path\":\"/Script/Engine.StaticMeshActor\",\"label\":\"%s\",\"static_mesh\":\"/Engine/BasicShapes/Cube.Cube\",\"location\":{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f},\"scale\":{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f}}"),
			*ActorLabel,
			ActorLocation.X,
			ActorLocation.Y,
			ActorLocation.Z,
			ActorScale.X,
			ActorScale.Y,
			ActorScale.Z));
	FHttpSmokeResult SpawnResult;
	TestTrue(TEXT("Collision spawn_actor request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SpawnBody, SpawnResult));
	TestEqual(TEXT("Collision spawn_actor status code"), SpawnResult.StatusCode, 200);

	TSharedPtr<FJsonObject> SpawnJson;
	TestTrue(TEXT("Collision spawn_actor response parses as JSON"), ParseJson(SpawnResult.Body, SpawnJson));
	TestTrue(TEXT("Collision spawn_actor root ok=true"), SpawnJson.IsValid() && SpawnJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* SpawnData = nullptr;
	TestTrue(TEXT("Collision spawn_actor contains data object"), SpawnJson.IsValid() && SpawnJson->TryGetObjectField(TEXT("data"), SpawnData) && SpawnData && SpawnData->IsValid());
	const FString ActorName = (SpawnData && SpawnData->IsValid()) ? (*SpawnData)->GetStringField(TEXT("name")) : FString();
	TestFalse(TEXT("Collision spawn_actor returned name"), ActorName.IsEmpty());
	if (ActorName.IsEmpty())
	{
		return false;
	}

	auto DestroyActorByName = [this, &ServerScope](const FString& RequestId, const FString& InActorName) -> void
	{
		if (InActorName.IsEmpty())
		{
			return;
		}

		const FString DestroyBody = MakeExecRequestBody(
			RequestId,
			TEXT("destroy_actor"),
			FString::Printf(TEXT("{\"id\":\"%s\"}"), *InActorName));
		FHttpSmokeResult DestroyResult;
		ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, DestroyBody, DestroyResult, 5.0, false);
	};

	TArray<AActor*> DirectActorsToDestroy;
	auto TrackDirectActor = [&DirectActorsToDestroy](AActor* Actor) -> AActor*
	{
		if (Actor)
		{
			DirectActorsToDestroy.Add(Actor);
		}
		return Actor;
	};
	auto DestroyDirectActors = [&DirectActorsToDestroy]() -> void
	{
		for (AActor* Actor : DirectActorsToDestroy)
		{
			if (IsValid(Actor) && !Actor->IsActorBeingDestroyed())
			{
				Actor->Destroy();
			}
		}
		DirectActorsToDestroy.Reset();
	};
	auto ExecMeshShapes = [this, &ServerScope](const FString& RequestId, const FString& Params, TSharedPtr<FJsonObject>& OutData, bool bExpectedOk = true) -> bool
	{
		OutData.Reset();
		const FString Body = MakeExecRequestBody(RequestId, TEXT("level_get_nearby_mesh_shapes"), Params);
		FHttpSmokeResult Result;
		if (!ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, Body, Result))
		{
			AddError(FString::Printf(TEXT("%s request failed"), *RequestId));
			return false;
		}
		if (Result.StatusCode != 200)
		{
			AddError(FString::Printf(TEXT("%s unexpected status code: %d"), *RequestId, Result.StatusCode));
			return false;
		}

		TSharedPtr<FJsonObject> Json;
		if (!ParseJson(Result.Body, Json) || !Json.IsValid())
		{
			AddError(FString::Printf(TEXT("%s returned invalid JSON"), *RequestId));
			return false;
		}

		bool bOk = false;
		Json->TryGetBoolField(TEXT("ok"), bOk);
		if (bOk != bExpectedOk)
		{
			AddError(FString::Printf(TEXT("%s ok mismatch: expected %s, got %s"), *RequestId, bExpectedOk ? TEXT("true") : TEXT("false"), bOk ? TEXT("true") : TEXT("false")));
			return false;
		}
		if (!bExpectedOk)
		{
			return true;
		}

		const TSharedPtr<FJsonObject>* Data = nullptr;
		if (!Json->TryGetObjectField(TEXT("data"), Data) || !Data || !Data->IsValid())
		{
			AddError(FString::Printf(TEXT("%s missing data object"), *RequestId));
			return false;
		}
		OutData = *Data;
		return true;
	};
	auto MeshShapesContainsActor = [](const TSharedPtr<FJsonObject>& Data, const FString& ExpectedActorLabel) -> bool
	{
		const TArray<TSharedPtr<FJsonValue>>* Actors = nullptr;
		if (!Data.IsValid() || !Data->TryGetArrayField(TEXT("actors"), Actors) || !Actors)
		{
			return false;
		}
		for (const TSharedPtr<FJsonValue>& ActorValue : *Actors)
		{
			const TSharedPtr<FJsonObject> ActorObj = ActorValue.IsValid() ? ActorValue->AsObject() : nullptr;
			if (ActorObj.IsValid() && ActorObj->GetStringField(TEXT("name")) == ExpectedActorLabel)
			{
				return true;
			}
		}
		return false;
	};
	auto MeshShapesOwnerCountForActor = [](const TSharedPtr<FJsonObject>& Data, const FString& ExpectedActorLabel) -> int32
	{
		const TArray<TSharedPtr<FJsonValue>>* Actors = nullptr;
		if (!Data.IsValid() || !Data->TryGetArrayField(TEXT("actors"), Actors) || !Actors)
		{
			return 0;
		}
		for (const TSharedPtr<FJsonValue>& ActorValue : *Actors)
		{
			const TSharedPtr<FJsonObject> ActorObj = ActorValue.IsValid() ? ActorValue->AsObject() : nullptr;
			const TArray<TSharedPtr<FJsonValue>>* Components = nullptr;
			if (ActorObj.IsValid() && ActorObj->GetStringField(TEXT("name")) == ExpectedActorLabel && ActorObj->TryGetArrayField(TEXT("components"), Components) && Components)
			{
				return Components->Num();
			}
		}
		return 0;
	};
	auto MeshShapesContainsActorShape = [](const TSharedPtr<FJsonObject>& Data, const FString& ExpectedActorLabel, const FString& ExpectedSrc, int32 ExpectedInstanceIndex = INDEX_NONE) -> bool
	{
		const TArray<TSharedPtr<FJsonValue>>* Actors = nullptr;
		if (!Data.IsValid() || !Data->TryGetArrayField(TEXT("actors"), Actors) || !Actors)
		{
			return false;
		}
		for (const TSharedPtr<FJsonValue>& ActorValue : *Actors)
		{
			const TSharedPtr<FJsonObject> ActorObj = ActorValue.IsValid() ? ActorValue->AsObject() : nullptr;
			const TArray<TSharedPtr<FJsonValue>>* Components = nullptr;
			if (!ActorObj.IsValid() || ActorObj->GetStringField(TEXT("name")) != ExpectedActorLabel || !ActorObj->TryGetArrayField(TEXT("components"), Components) || !Components)
			{
				continue;
			}
			for (const TSharedPtr<FJsonValue>& ComponentValue : *Components)
			{
				const TSharedPtr<FJsonObject> ComponentObj = ComponentValue.IsValid() ? ComponentValue->AsObject() : nullptr;
				if (!ComponentObj.IsValid())
				{
					continue;
				}
				if (ExpectedInstanceIndex != INDEX_NONE)
				{
					double InstanceIndexNumber = -1.0;
					if (!ComponentObj->TryGetNumberField(TEXT("instance_index"), InstanceIndexNumber) || FMath::RoundToInt(InstanceIndexNumber) != ExpectedInstanceIndex)
					{
						continue;
					}
				}

				const TArray<TSharedPtr<FJsonValue>>* Shapes = nullptr;
				if (!ComponentObj->TryGetArrayField(TEXT("shapes"), Shapes) || !Shapes)
				{
					continue;
				}
				for (const TSharedPtr<FJsonValue>& ShapeValue : *Shapes)
				{
					const TSharedPtr<FJsonObject> ShapeObj = ShapeValue.IsValid() ? ShapeValue->AsObject() : nullptr;
					if (ShapeObj.IsValid() && ShapeObj->GetStringField(TEXT("src")) == ExpectedSrc)
					{
						return true;
					}
				}
			}
		}
		return false;
	};

	{
		const FVector TraceStart = ActorLocation + FVector(0.0f, 0.0f, 2000.0f);
		const FString TraceBody = MakeExecRequestBody(
			TEXT("smoke-collision-trace-001"),
			TEXT("level_trace_world_ray"),
			FString::Printf(
				TEXT("{\"start\":{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f},\"direction\":{\"x\":0.0,\"y\":0.0,\"z\":-1.0},\"trace_distance\":5000.0,\"trace_channel\":\"Visibility\",\"trace_complex\":true}"),
				TraceStart.X,
				TraceStart.Y,
				TraceStart.Z));
		FHttpSmokeResult TraceResult;
		TestTrue(TEXT("Collision level_trace_world_ray request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, TraceBody, TraceResult));
		TestEqual(TEXT("Collision level_trace_world_ray status code"), TraceResult.StatusCode, 200);

		TSharedPtr<FJsonObject> TraceJson;
		TestTrue(TEXT("Collision level_trace_world_ray parses JSON"), ParseJson(TraceResult.Body, TraceJson));
		TestTrue(TEXT("Collision level_trace_world_ray root ok=true"), TraceJson.IsValid() && TraceJson->GetBoolField(TEXT("ok")));

		const TSharedPtr<FJsonObject>* TraceData = nullptr;
		TestTrue(TEXT("Collision level_trace_world_ray contains data"), TraceJson.IsValid() && TraceJson->TryGetObjectField(TEXT("data"), TraceData) && TraceData && TraceData->IsValid());
		if (TraceData && TraceData->IsValid())
		{
			TestTrue(TEXT("Collision level_trace_world_ray hit=true"), (*TraceData)->GetBoolField(TEXT("hit")));
			TestEqual(TEXT("Collision level_trace_world_ray actor_name matches"), (*TraceData)->GetStringField(TEXT("actor_name")), ActorName);
			TestEqual(TEXT("Collision level_trace_world_ray actor_id matches label"), (*TraceData)->GetStringField(TEXT("actor_id")), ActorLabel);
		}
	}

	{
		const FVector SweepStart = ActorLocation + FVector(0.0f, -2000.0f, 0.0f);
		const FVector SweepEnd = ActorLocation + FVector(0.0f, 2000.0f, 0.0f);
		const FString SweepBody = MakeExecRequestBody(
			TEXT("smoke-collision-sweep-001"),
			TEXT("level_sweep_capsule"),
			FString::Printf(
				TEXT("{\"start\":{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f},\"end\":{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f},\"radius_cm\":34.0,\"half_height_cm\":88.0,\"trace_channel\":\"Pawn\",\"trace_complex\":false,\"find_initial_overlaps\":true}"),
				SweepStart.X,
				SweepStart.Y,
				SweepStart.Z,
				SweepEnd.X,
				SweepEnd.Y,
				SweepEnd.Z));
		FHttpSmokeResult SweepResult;
		TestTrue(TEXT("Collision level_sweep_capsule request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SweepBody, SweepResult));
		TestEqual(TEXT("Collision level_sweep_capsule status code"), SweepResult.StatusCode, 200);

		TSharedPtr<FJsonObject> SweepJson;
		TestTrue(TEXT("Collision level_sweep_capsule parses JSON"), ParseJson(SweepResult.Body, SweepJson));
		TestTrue(TEXT("Collision level_sweep_capsule root ok=true"), SweepJson.IsValid() && SweepJson->GetBoolField(TEXT("ok")));

		const TSharedPtr<FJsonObject>* SweepData = nullptr;
		TestTrue(TEXT("Collision level_sweep_capsule contains data"), SweepJson.IsValid() && SweepJson->TryGetObjectField(TEXT("data"), SweepData) && SweepData && SweepData->IsValid());
		if (SweepData && SweepData->IsValid())
		{
			TestTrue(TEXT("Collision level_sweep_capsule blocking_hit=true"), (*SweepData)->GetBoolField(TEXT("blocking_hit")));
			TestEqual(TEXT("Collision level_sweep_capsule actor_name matches"), (*SweepData)->GetStringField(TEXT("actor_name")), ActorName);
		}
	}

	{
		const FVector PathStart = ActorLocation + FVector(0.0f, -2000.0f, 0.0f);
		const FVector PathEnd = ActorLocation + FVector(0.0f, 2000.0f, 0.0f);
		const FString PathBody = MakeExecRequestBody(
			TEXT("smoke-collision-sweep-path-001"),
			TEXT("level_sweep_capsule_path"),
			FString::Printf(
				TEXT("{\"points_mode\":\"center\",\"points\":[{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f},{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f}],\"radius_cm\":34.0,\"half_height_cm\":88.0,\"step_cm\":200.0,\"trace_channel\":\"Pawn\",\"trace_complex\":false,\"find_initial_overlaps\":true,\"stop_on_blocking_hit\":true}"),
				PathStart.X,
				PathStart.Y,
				PathStart.Z,
				PathEnd.X,
				PathEnd.Y,
				PathEnd.Z));
		FHttpSmokeResult PathResult;
		TestTrue(TEXT("Collision level_sweep_capsule_path request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, PathBody, PathResult));
		TestEqual(TEXT("Collision level_sweep_capsule_path status code"), PathResult.StatusCode, 200);

		TSharedPtr<FJsonObject> PathJson;
		TestTrue(TEXT("Collision level_sweep_capsule_path parses JSON"), ParseJson(PathResult.Body, PathJson));
		TestTrue(TEXT("Collision level_sweep_capsule_path root ok=true"), PathJson.IsValid() && PathJson->GetBoolField(TEXT("ok")));

		const TSharedPtr<FJsonObject>* PathData = nullptr;
		TestTrue(TEXT("Collision level_sweep_capsule_path contains data"), PathJson.IsValid() && PathJson->TryGetObjectField(TEXT("data"), PathData) && PathData && PathData->IsValid());
		if (PathData && PathData->IsValid())
		{
			TestTrue(TEXT("Collision level_sweep_capsule_path blocking_hit=true"), (*PathData)->GetBoolField(TEXT("blocking_hit")));
			TestTrue(TEXT("Collision level_sweep_capsule_path first_blocking_segment_index>=0"), (*PathData)->GetIntegerField(TEXT("first_blocking_segment_index")) >= 0);
			TestEqual(TEXT("Collision level_sweep_capsule_path actor_name matches"), (*PathData)->GetStringField(TEXT("actor_name")), ActorName);
		}
	}

	{
		const FVector ProbeLocation = ActorLocation + FVector(0.0f, 0.0f, 2000.0f);
		const FString ProbeBody = MakeExecRequestBody(
			TEXT("smoke-collision-probe-001"),
			TEXT("level_mark_probe"),
			FString::Printf(TEXT("{\"label\":\"SmokeProbe_%s\",\"location\":{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f}}"), *UniqueSuffix, ProbeLocation.X, ProbeLocation.Y, ProbeLocation.Z));
		FHttpSmokeResult ProbeResult;
		TestTrue(TEXT("Collision level_mark_probe request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ProbeBody, ProbeResult));
		TestEqual(TEXT("Collision level_mark_probe status code"), ProbeResult.StatusCode, 200);

		TSharedPtr<FJsonObject> ProbeJson;
		TestTrue(TEXT("Collision level_mark_probe parses JSON"), ParseJson(ProbeResult.Body, ProbeJson));
		TestTrue(TEXT("Collision level_mark_probe root ok=true"), ProbeJson.IsValid() && ProbeJson->GetBoolField(TEXT("ok")));

		FString ProbeActorName;
		const TSharedPtr<FJsonObject>* ProbeData = nullptr;
		TestTrue(TEXT("Collision level_mark_probe contains data"), ProbeJson.IsValid() && ProbeJson->TryGetObjectField(TEXT("data"), ProbeData) && ProbeData && ProbeData->IsValid());
		if (ProbeData && ProbeData->IsValid())
		{
			const TSharedPtr<FJsonObject>* ProbeInfo = nullptr;
			if ((*ProbeData)->TryGetObjectField(TEXT("probe"), ProbeInfo) && ProbeInfo && ProbeInfo->IsValid())
			{
				ProbeActorName = (*ProbeInfo)->GetStringField(TEXT("name"));
			}
		}
		TestFalse(TEXT("Collision level_mark_probe returned probe actor name"), ProbeActorName.IsEmpty());

		if (!ProbeActorName.IsEmpty())
		{
			const FString SnapBody = MakeExecRequestBody(
				TEXT("smoke-collision-snap-001"),
				TEXT("level_snap_to_surface"),
				FString::Printf(
					TEXT("{\"id\":\"%s\",\"direction\":{\"x\":0.0,\"y\":0.0,\"z\":-1.0},\"trace_distance\":5000.0,\"trace_channel\":\"Visibility\",\"trace_complex\":true}"),
					*ProbeActorName));
			FHttpSmokeResult SnapResult;
			TestTrue(TEXT("Collision level_snap_to_surface request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SnapBody, SnapResult));
			TestEqual(TEXT("Collision level_snap_to_surface status code"), SnapResult.StatusCode, 200);

			TSharedPtr<FJsonObject> SnapJson;
			TestTrue(TEXT("Collision level_snap_to_surface parses JSON"), ParseJson(SnapResult.Body, SnapJson));
			TestTrue(TEXT("Collision level_snap_to_surface root ok=true"), SnapJson.IsValid() && SnapJson->GetBoolField(TEXT("ok")));

			const TSharedPtr<FJsonObject>* SnapData = nullptr;
			TestTrue(TEXT("Collision level_snap_to_surface contains data"), SnapJson.IsValid() && SnapJson->TryGetObjectField(TEXT("data"), SnapData) && SnapData && SnapData->IsValid());
			if (SnapData && SnapData->IsValid())
			{
				TestTrue(TEXT("Collision level_snap_to_surface snapped=true"), (*SnapData)->GetBoolField(TEXT("snapped")));
				TestEqual(TEXT("Collision level_snap_to_surface hit actor matches cube"), (*SnapData)->GetStringField(TEXT("actor_name")), ActorName);
			}

			DestroyActorByName(TEXT("smoke-collision-destroy-probe-001"), ProbeActorName);
		}
	}

	{
		const FString OverlapBody = MakeExecRequestBody(
			TEXT("smoke-collision-overlap-001"),
			TEXT("level_check_overlaps"),
			FString::Printf(
				TEXT("{\"shape\":\"box\",\"center\":{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f},\"box_extent\":{\"x\":300.0,\"y\":300.0,\"z\":300.0},\"trace_channel\":\"Visibility\",\"limit\":50}"),
				ActorLocation.X,
				ActorLocation.Y,
				ActorLocation.Z));
		FHttpSmokeResult OverlapResult;
		TestTrue(TEXT("Collision level_check_overlaps request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, OverlapBody, OverlapResult));
		TestEqual(TEXT("Collision level_check_overlaps status code"), OverlapResult.StatusCode, 200);

		TSharedPtr<FJsonObject> OverlapJson;
		TestTrue(TEXT("Collision level_check_overlaps parses JSON"), ParseJson(OverlapResult.Body, OverlapJson));
		TestTrue(TEXT("Collision level_check_overlaps root ok=true"), OverlapJson.IsValid() && OverlapJson->GetBoolField(TEXT("ok")));

		const TSharedPtr<FJsonObject>* OverlapData = nullptr;
		TestTrue(TEXT("Collision level_check_overlaps contains data"), OverlapJson.IsValid() && OverlapJson->TryGetObjectField(TEXT("data"), OverlapData) && OverlapData && OverlapData->IsValid());
		if (OverlapData && OverlapData->IsValid())
		{
			TestTrue(TEXT("Collision level_check_overlaps overlap_count>=1"), static_cast<int32>((*OverlapData)->GetIntegerField(TEXT("overlap_count"))) >= 1);

			const TArray<TSharedPtr<FJsonValue>>* Overlaps = nullptr;
			TestTrue(TEXT("Collision level_check_overlaps contains overlaps array"), (*OverlapData)->TryGetArrayField(TEXT("overlaps"), Overlaps) && Overlaps);
			bool bFound = false;
			if (Overlaps)
			{
				for (const TSharedPtr<FJsonValue>& ItemValue : *Overlaps)
				{
					const TSharedPtr<FJsonObject> ItemObj = ItemValue.IsValid() ? ItemValue->AsObject() : nullptr;
					if (!ItemObj.IsValid())
					{
						continue;
					}

					const TSharedPtr<FJsonObject>* ActorSummary = nullptr;
					if (ItemObj->TryGetObjectField(TEXT("actor"), ActorSummary) && ActorSummary && ActorSummary->IsValid())
					{
						if ((*ActorSummary)->GetStringField(TEXT("name")) == ActorName)
						{
							bFound = true;
							break;
						}
					}
				}
			}
			TestTrue(TEXT("Collision level_check_overlaps includes spawned actor"), bFound);
		}
	}

	{
		const FVector QueryMin = ActorLocation - FVector(400.0f, 400.0f, 400.0f);
		const FVector QueryMax = ActorLocation + FVector(400.0f, 400.0f, 400.0f);
		const FString MeshShapesBody = MakeExecRequestBody(
			TEXT("smoke-collision-mesh-shapes-static-001"),
			TEXT("level_get_nearby_mesh_shapes"),
			FString::Printf(
				TEXT("{\"query\":{\"type\":\"aabb\",\"min\":{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f},\"max\":{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f}},\"filters\":{\"types\":[\"static_mesh\"]},\"max_shapes_per_actor\":8}"),
				QueryMin.X,
				QueryMin.Y,
				QueryMin.Z,
				QueryMax.X,
				QueryMax.Y,
				QueryMax.Z));
		FHttpSmokeResult MeshShapesResult;
		TestTrue(TEXT("Collision mesh shapes static request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, MeshShapesBody, MeshShapesResult));
		TestEqual(TEXT("Collision mesh shapes static status code"), MeshShapesResult.StatusCode, 200);

		TSharedPtr<FJsonObject> MeshShapesJson;
		TestTrue(TEXT("Collision mesh shapes static parses JSON"), ParseJson(MeshShapesResult.Body, MeshShapesJson));
		TestTrue(TEXT("Collision mesh shapes static root ok=true"), MeshShapesJson.IsValid() && MeshShapesJson->GetBoolField(TEXT("ok")));

		const TSharedPtr<FJsonObject>* MeshShapesData = nullptr;
		TestTrue(TEXT("Collision mesh shapes static contains data"), MeshShapesJson.IsValid() && MeshShapesJson->TryGetObjectField(TEXT("data"), MeshShapesData) && MeshShapesData && MeshShapesData->IsValid());
		if (MeshShapesData && MeshShapesData->IsValid())
		{
			const TSharedPtr<FJsonObject>* ShapeSpace = nullptr;
			TestTrue(TEXT("Collision mesh shapes shape_space exists"), (*MeshShapesData)->TryGetObjectField(TEXT("shape_space"), ShapeSpace) && ShapeSpace && ShapeSpace->IsValid());
			if (ShapeSpace && ShapeSpace->IsValid())
			{
				TestEqual(TEXT("Collision mesh shapes frame"), (*ShapeSpace)->GetStringField(TEXT("frame")), FString(TEXT("owner_local")));
				TestEqual(TEXT("Collision mesh shapes repr"), (*ShapeSpace)->GetStringField(TEXT("repr")), FString(TEXT("aabb")));
			}

			const TArray<TSharedPtr<FJsonValue>>* Actors = nullptr;
			TestTrue(TEXT("Collision mesh shapes static actors exists"), (*MeshShapesData)->TryGetArrayField(TEXT("actors"), Actors) && Actors);
			bool bFoundActor = false;
			bool bFoundBlockingShape = false;
			bool bSummaryShapeIsCompact = false;
			if (Actors)
			{
				for (const TSharedPtr<FJsonValue>& ActorValue : *Actors)
				{
					const TSharedPtr<FJsonObject> ActorObj = ActorValue.IsValid() ? ActorValue->AsObject() : nullptr;
					if (!ActorObj.IsValid() || ActorObj->GetStringField(TEXT("name")) != ActorLabel)
					{
						continue;
					}

					bFoundActor = true;
					const TArray<TSharedPtr<FJsonValue>>* Components = nullptr;
					if (!ActorObj->TryGetArrayField(TEXT("components"), Components) || !Components)
					{
						continue;
					}

					for (const TSharedPtr<FJsonValue>& ComponentValue : *Components)
					{
						const TSharedPtr<FJsonObject> ComponentObj = ComponentValue.IsValid() ? ComponentValue->AsObject() : nullptr;
						if (!ComponentObj.IsValid())
						{
							continue;
						}

						TestTrue(TEXT("Collision mesh shapes component class is UE class"), ComponentObj->GetStringField(TEXT("class")).Contains(TEXT("StaticMeshComponent")));
						const TArray<TSharedPtr<FJsonValue>>* Shapes = nullptr;
						if (!ComponentObj->TryGetArrayField(TEXT("shapes"), Shapes) || !Shapes)
						{
							continue;
						}

						for (const TSharedPtr<FJsonValue>& ShapeValue : *Shapes)
						{
							const TSharedPtr<FJsonObject> ShapeObj = ShapeValue.IsValid() ? ShapeValue->AsObject() : nullptr;
							if (!ShapeObj.IsValid())
							{
								continue;
							}

							const FString Src = ShapeObj->GetStringField(TEXT("src"));
							bFoundBlockingShape |= Src == TEXT("collision_box/block");
							bSummaryShapeIsCompact |= ShapeObj->HasField(TEXT("src"))
								&& ShapeObj->HasField(TEXT("center"))
								&& ShapeObj->HasField(TEXT("extent"))
								&& !ShapeObj->HasField(TEXT("id"))
								&& !ShapeObj->HasField(TEXT("n"))
								&& !ShapeObj->HasField(TEXT("warn"))
								&& !ShapeObj->HasField(TEXT("world_bounds"));
						}
					}
				}
			}
			TestTrue(TEXT("Collision mesh shapes static includes spawned actor label"), bFoundActor);
			TestTrue(TEXT("Collision mesh shapes static includes blocking box collision"), bFoundBlockingShape);
			TestTrue(TEXT("Collision mesh shapes summary shape remains compact"), bSummaryShapeIsCompact);
		}

		const FString MeshShapesStandardBody = MakeExecRequestBody(
			TEXT("smoke-collision-mesh-shapes-standard-001"),
			TEXT("level_get_nearby_mesh_shapes"),
			FString::Printf(
				TEXT("{\"query\":{\"type\":\"aabb\",\"min\":{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f},\"max\":{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f}},\"filters\":{\"types\":[\"static_mesh\"]},\"detail_level\":\"standard\",\"max_shapes_per_actor\":8}"),
				QueryMin.X,
				QueryMin.Y,
				QueryMin.Z,
				QueryMax.X,
				QueryMax.Y,
				QueryMax.Z));
		FHttpSmokeResult MeshShapesStandardResult;
		TestTrue(TEXT("Collision mesh shapes standard request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, MeshShapesStandardBody, MeshShapesStandardResult));
		TestEqual(TEXT("Collision mesh shapes standard status code"), MeshShapesStandardResult.StatusCode, 200);

		TSharedPtr<FJsonObject> MeshShapesStandardJson;
		TestTrue(TEXT("Collision mesh shapes standard parses JSON"), ParseJson(MeshShapesStandardResult.Body, MeshShapesStandardJson));
		TestTrue(TEXT("Collision mesh shapes standard root ok=true"), MeshShapesStandardJson.IsValid() && MeshShapesStandardJson->GetBoolField(TEXT("ok")));
		const TSharedPtr<FJsonObject>* MeshShapesStandardData = nullptr;
		TestTrue(TEXT("Collision mesh shapes standard contains data"), MeshShapesStandardJson.IsValid() && MeshShapesStandardJson->TryGetObjectField(TEXT("data"), MeshShapesStandardData) && MeshShapesStandardData && MeshShapesStandardData->IsValid());
		if (MeshShapesStandardData && MeshShapesStandardData->IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* Actors = nullptr;
			bool bFoundStandardWorldBounds = false;
			if ((*MeshShapesStandardData)->TryGetArrayField(TEXT("actors"), Actors) && Actors)
			{
				for (const TSharedPtr<FJsonValue>& ActorValue : *Actors)
				{
					const TSharedPtr<FJsonObject> ActorObj = ActorValue.IsValid() ? ActorValue->AsObject() : nullptr;
					const TArray<TSharedPtr<FJsonValue>>* Components = nullptr;
					if (!ActorObj.IsValid() || ActorObj->GetStringField(TEXT("name")) != ActorLabel || !ActorObj->TryGetArrayField(TEXT("components"), Components) || !Components)
					{
						continue;
					}
					for (const TSharedPtr<FJsonValue>& ComponentValue : *Components)
					{
						const TSharedPtr<FJsonObject> ComponentObj = ComponentValue.IsValid() ? ComponentValue->AsObject() : nullptr;
						const TArray<TSharedPtr<FJsonValue>>* Shapes = nullptr;
						if (!ComponentObj.IsValid() || !ComponentObj->TryGetArrayField(TEXT("shapes"), Shapes) || !Shapes)
						{
							continue;
						}
						for (const TSharedPtr<FJsonValue>& ShapeValue : *Shapes)
						{
							const TSharedPtr<FJsonObject> ShapeObj = ShapeValue.IsValid() ? ShapeValue->AsObject() : nullptr;
							bFoundStandardWorldBounds |= ShapeObj.IsValid() && ShapeObj->HasTypedField<EJson::Object>(TEXT("world_bounds")) && ShapeObj->HasTypedField<EJson::String>(TEXT("authority"));
						}
					}
				}
			}
			TestTrue(TEXT("Collision mesh shapes standard expands diagnostics"), bFoundStandardWorldBounds);
		}

		const FString MeshShapesBadTypeBody = MakeExecRequestBody(
			TEXT("smoke-collision-mesh-shapes-bad-type-001"),
			TEXT("level_get_nearby_mesh_shapes"),
			FString::Printf(
				TEXT("{\"query\":{\"type\":\"aabb\",\"min\":{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f},\"max\":{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f}},\"filters\":{\"types\":[\"not_a_supported_type\"]}}"),
				QueryMin.X,
				QueryMin.Y,
				QueryMin.Z,
				QueryMax.X,
				QueryMax.Y,
				QueryMax.Z));
		FHttpSmokeResult MeshShapesBadTypeResult;
		TestTrue(TEXT("Collision mesh shapes bad type request completed"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, MeshShapesBadTypeBody, MeshShapesBadTypeResult));
		TestEqual(TEXT("Collision mesh shapes bad type status code"), MeshShapesBadTypeResult.StatusCode, 400);
		TSharedPtr<FJsonObject> MeshShapesBadTypeJson;
		TestTrue(TEXT("Collision mesh shapes bad type parses JSON"), ParseJson(MeshShapesBadTypeResult.Body, MeshShapesBadTypeJson));
		TestFalse(TEXT("Collision mesh shapes bad type root ok=false"), MeshShapesBadTypeJson.IsValid() && MeshShapesBadTypeJson->GetBoolField(TEXT("ok")));
	}

	{
		TSharedPtr<FJsonObject> SphereData;
		const FString SphereParams = FString::Printf(
			TEXT("{\"query\":{\"type\":\"sphere\",\"center\":{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f},\"radius\":700.0},\"filters\":{\"types\":[\"static_mesh\"]},\"max_shapes_per_actor\":8}"),
			ActorLocation.X,
			ActorLocation.Y,
			ActorLocation.Z);
		if (ExecMeshShapes(TEXT("smoke-collision-mesh-shapes-sphere-001"), SphereParams, SphereData))
		{
			TestTrue(TEXT("Collision mesh shapes sphere query includes spawned actor"), MeshShapesContainsActorShape(SphereData, ActorLabel, TEXT("collision_box/block")));
		}

		TSharedPtr<FJsonObject> FarAabbData;
		const FVector FarMin = ActorLocation + FVector(10000.0f, 10000.0f, 10000.0f);
		const FVector FarMax = FarMin + FVector(100.0f, 100.0f, 100.0f);
		const FString FarAabbParams = FString::Printf(
			TEXT("{\"query\":{\"type\":\"aabb\",\"min\":{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f},\"max\":{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f}},\"filters\":{\"types\":[\"static_mesh\"]}}"),
			FarMin.X,
			FarMin.Y,
			FarMin.Z,
			FarMax.X,
			FarMax.Y,
			FarMax.Z);
		if (ExecMeshShapes(TEXT("smoke-collision-mesh-shapes-far-aabb-001"), FarAabbParams, FarAabbData))
		{
			TestFalse(TEXT("Collision mesh shapes far AABB excludes spawned actor"), MeshShapesContainsActor(FarAabbData, ActorLabel));
		}
	}

	{
		UWorld* EditorWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		UStaticMesh* CubeMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
		TestNotNull(TEXT("Collision mesh shapes direct world exists"), EditorWorld);
		TestNotNull(TEXT("Collision mesh shapes cube mesh loaded"), CubeMesh);
		if (EditorWorld && CubeMesh)
		{
			const FVector VisualLocation = ActorLocation + FVector(3000.0f, 0.0f, 100.0f);
			const FString VisualLabel = FString::Printf(TEXT("SmokeVisualFallback_%s"), *UniqueSuffix);
			AActor* VisualActor = TrackDirectActor(EditorWorld->SpawnActor<AActor>(VisualLocation, FRotator::ZeroRotator));
			TestNotNull(TEXT("Collision mesh shapes visual fallback actor spawned"), VisualActor);
			if (VisualActor)
			{
				VisualActor->SetActorLabel(VisualLabel);
				UStaticMeshComponent* VisualComponent = NewObject<UStaticMeshComponent>(VisualActor, TEXT("SmokeVisualFallbackStaticMesh"));
				TestNotNull(TEXT("Collision mesh shapes visual fallback component created"), VisualComponent);
				if (VisualComponent)
				{
					VisualComponent->CreationMethod = EComponentCreationMethod::Instance;
					VisualComponent->SetStaticMesh(CubeMesh);
					VisualComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
					VisualComponent->SetWorldScale3D(FVector(2.0f, 1.0f, 1.0f));
					VisualActor->AddInstanceComponent(VisualComponent);
					VisualActor->SetRootComponent(VisualComponent);
					VisualComponent->RegisterComponent();
					VisualComponent->SetWorldLocation(VisualLocation);
				}

				TSharedPtr<FJsonObject> VisualData;
				const FVector QueryMin = VisualLocation - FVector(250.0f, 250.0f, 250.0f);
				const FVector QueryMax = VisualLocation + FVector(250.0f, 250.0f, 250.0f);
				const FString VisualParams = FString::Printf(
					TEXT("{\"query\":{\"type\":\"aabb\",\"min\":{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f},\"max\":{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f}},\"filters\":{\"types\":[\"static_mesh\"]},\"max_shapes_per_actor\":8}"),
					QueryMin.X,
					QueryMin.Y,
					QueryMin.Z,
					QueryMax.X,
					QueryMax.Y,
					QueryMax.Z);
				if (ExecMeshShapes(TEXT("smoke-collision-mesh-shapes-visual-fallback-001"), VisualParams, VisualData))
				{
					TestTrue(TEXT("Collision mesh shapes no-collision static mesh uses visual fallback"), MeshShapesContainsActorShape(VisualData, VisualLabel, TEXT("visual_aabb/visual")));
					TestFalse(TEXT("Collision mesh shapes visual fallback is not reported as blocking collision"), MeshShapesContainsActorShape(VisualData, VisualLabel, TEXT("collision_box/block")));
				}
			}
		}
	}

	{
		UWorld* EditorWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		UStaticMesh* CubeMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
		if (EditorWorld && CubeMesh)
		{
			const FVector IsmLocation = ActorLocation + FVector(5000.0f, 0.0f, 100.0f);
			const FString IsmLabel = FString::Printf(TEXT("SmokeISM_%s"), *UniqueSuffix);
			AActor* IsmActor = TrackDirectActor(EditorWorld->SpawnActor<AActor>(IsmLocation, FRotator::ZeroRotator));
			TestNotNull(TEXT("Collision mesh shapes ISM actor spawned"), IsmActor);
			if (IsmActor)
			{
				IsmActor->SetActorLabel(IsmLabel);
				UInstancedStaticMeshComponent* IsmComponent = NewObject<UInstancedStaticMeshComponent>(IsmActor, TEXT("SmokeInstancedStaticMesh"));
				TestNotNull(TEXT("Collision mesh shapes ISM component created"), IsmComponent);
				if (IsmComponent)
				{
					IsmComponent->CreationMethod = EComponentCreationMethod::Instance;
					IsmComponent->SetStaticMesh(CubeMesh);
					IsmComponent->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
					IsmComponent->SetCollisionObjectType(ECC_WorldStatic);
					IsmComponent->SetCollisionResponseToAllChannels(ECR_Block);
					IsmActor->AddInstanceComponent(IsmComponent);
					IsmActor->SetRootComponent(IsmComponent);
					IsmComponent->RegisterComponent();
					IsmComponent->SetWorldLocation(IsmLocation);
					IsmComponent->AddInstance(FTransform(FRotator::ZeroRotator, FVector::ZeroVector, FVector::OneVector), false);
					IsmComponent->AddInstance(FTransform(FRotator::ZeroRotator, FVector(2000.0f, 0.0f, 0.0f), FVector::OneVector), false);
				}

				TSharedPtr<FJsonObject> IsmData;
				const FVector QueryMin = IsmLocation - FVector(250.0f, 250.0f, 250.0f);
				const FVector QueryMax = IsmLocation + FVector(250.0f, 250.0f, 250.0f);
				const FString IsmParams = FString::Printf(
					TEXT("{\"query\":{\"type\":\"aabb\",\"min\":{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f},\"max\":{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f}},\"filters\":{\"types\":[\"instanced_static_mesh\"]},\"max_shapes_per_actor\":8}"),
					QueryMin.X,
					QueryMin.Y,
					QueryMin.Z,
					QueryMax.X,
					QueryMax.Y,
					QueryMax.Z);
				if (ExecMeshShapes(TEXT("smoke-collision-mesh-shapes-ism-001"), IsmParams, IsmData))
				{
					TestTrue(TEXT("Collision mesh shapes ISM returns matching instance"), MeshShapesContainsActorShape(IsmData, IsmLabel, TEXT("collision_box/block"), 0));
					TestEqual(TEXT("Collision mesh shapes ISM excludes far instance owner"), MeshShapesOwnerCountForActor(IsmData, IsmLabel), 1);
				}
			}

			const FVector HismLocation = ActorLocation + FVector(7000.0f, 0.0f, 100.0f);
			const FString HismLabel = FString::Printf(TEXT("SmokeHISM_%s"), *UniqueSuffix);
			AActor* HismActor = TrackDirectActor(EditorWorld->SpawnActor<AActor>(HismLocation, FRotator::ZeroRotator));
			TestNotNull(TEXT("Collision mesh shapes HISM actor spawned"), HismActor);
			if (HismActor)
			{
				HismActor->SetActorLabel(HismLabel);
				UHierarchicalInstancedStaticMeshComponent* HismComponent = NewObject<UHierarchicalInstancedStaticMeshComponent>(HismActor, TEXT("SmokeHierarchicalInstancedStaticMesh"));
				TestNotNull(TEXT("Collision mesh shapes HISM component created"), HismComponent);
				if (HismComponent)
				{
					HismComponent->CreationMethod = EComponentCreationMethod::Instance;
					HismComponent->SetStaticMesh(CubeMesh);
					HismComponent->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
					HismComponent->SetCollisionObjectType(ECC_WorldStatic);
					HismComponent->SetCollisionResponseToAllChannels(ECR_Block);
					HismActor->AddInstanceComponent(HismComponent);
					HismActor->SetRootComponent(HismComponent);
					HismComponent->RegisterComponent();
					HismComponent->SetWorldLocation(HismLocation);
					HismComponent->AddInstance(FTransform(FRotator::ZeroRotator, FVector::ZeroVector, FVector::OneVector), false);
					HismComponent->AddInstance(FTransform(FRotator::ZeroRotator, FVector(2000.0f, 0.0f, 0.0f), FVector::OneVector), false);
				}

				TSharedPtr<FJsonObject> HismData;
				const FVector QueryMin = HismLocation - FVector(250.0f, 250.0f, 250.0f);
				const FVector QueryMax = HismLocation + FVector(250.0f, 250.0f, 250.0f);
				const FString HismParams = FString::Printf(
					TEXT("{\"query\":{\"type\":\"aabb\",\"min\":{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f},\"max\":{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f}},\"filters\":{\"types\":[\"hierarchical_instanced_static_mesh\"]},\"max_shapes_per_actor\":8}"),
					QueryMin.X,
					QueryMin.Y,
					QueryMin.Z,
					QueryMax.X,
					QueryMax.Y,
					QueryMax.Z);
				if (ExecMeshShapes(TEXT("smoke-collision-mesh-shapes-hism-001"), HismParams, HismData))
				{
					TestTrue(TEXT("Collision mesh shapes HISM returns matching instance"), MeshShapesContainsActorShape(HismData, HismLabel, TEXT("collision_box/block"), 0));
					TestEqual(TEXT("Collision mesh shapes HISM excludes far instance owner"), MeshShapesOwnerCountForActor(HismData, HismLabel), 1);
				}
			}
		}
	}

	{
		UWorld* EditorWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		const FVector VoxelLocation = ActorLocation + FVector(1500.0f, 0.0f, 0.0f);
		const FString VoxelLabel = FString::Printf(TEXT("SmokeVoxelCollision_%s"), *UniqueSuffix);
		AActor* VoxelActor = EditorWorld ? EditorWorld->SpawnActor<AActor>(VoxelLocation, FRotator::ZeroRotator) : nullptr;
		TestNotNull(TEXT("Collision mesh shapes voxel actor spawned"), VoxelActor);
		if (VoxelActor)
		{
			VoxelActor->SetActorLabel(VoxelLabel);
			UBoxComponent* VoxelBox = NewObject<UBoxComponent>(VoxelActor, *FString::Printf(TEXT("UAI_VoxelCollision_%s"), *UniqueSuffix.Left(8)));
			TestNotNull(TEXT("Collision mesh shapes voxel box component created"), VoxelBox);
			if (VoxelBox)
			{
				VoxelBox->CreationMethod = EComponentCreationMethod::Instance;
				VoxelBox->SetBoxExtent(FVector(120.0f, 80.0f, 60.0f));
				VoxelBox->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
				VoxelBox->SetCollisionObjectType(ECC_WorldStatic);
				VoxelBox->SetCollisionResponseToAllChannels(ECR_Block);
				VoxelBox->ComponentTags.Add(FName(*FString::Printf(TEXT("UAIEditableVoxelGridCollision:%s"), *UniqueSuffix.Left(8))));
				VoxelActor->AddInstanceComponent(VoxelBox);
				VoxelActor->SetRootComponent(VoxelBox);
				VoxelBox->RegisterComponent();
				VoxelBox->SetWorldLocation(VoxelLocation);
			}
		}

		if (VoxelActor)
		{
			const FVector QueryMin = VoxelLocation - FVector(200.0f, 200.0f, 200.0f);
			const FVector QueryMax = VoxelLocation + FVector(200.0f, 200.0f, 200.0f);
			const FString VoxelShapesBody = MakeExecRequestBody(
				TEXT("smoke-collision-mesh-shapes-voxel-001"),
				TEXT("level_get_nearby_mesh_shapes"),
				FString::Printf(
					TEXT("{\"query\":{\"type\":\"aabb\",\"min\":{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f},\"max\":{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f}},\"filters\":{\"types\":[\"voxel_collision\"]},\"max_shapes_per_actor\":8}"),
					QueryMin.X,
					QueryMin.Y,
					QueryMin.Z,
					QueryMax.X,
					QueryMax.Y,
					QueryMax.Z));
			FHttpSmokeResult VoxelShapesResult;
			TestTrue(TEXT("Collision mesh shapes voxel request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, VoxelShapesBody, VoxelShapesResult));
			TestEqual(TEXT("Collision mesh shapes voxel status code"), VoxelShapesResult.StatusCode, 200);

			TSharedPtr<FJsonObject> VoxelShapesJson;
			TestTrue(TEXT("Collision mesh shapes voxel parses JSON"), ParseJson(VoxelShapesResult.Body, VoxelShapesJson));
			TestTrue(TEXT("Collision mesh shapes voxel root ok=true"), VoxelShapesJson.IsValid() && VoxelShapesJson->GetBoolField(TEXT("ok")));
			const TSharedPtr<FJsonObject>* VoxelShapesData = nullptr;
			TestTrue(TEXT("Collision mesh shapes voxel contains data"), VoxelShapesJson.IsValid() && VoxelShapesJson->TryGetObjectField(TEXT("data"), VoxelShapesData) && VoxelShapesData && VoxelShapesData->IsValid());
			if (VoxelShapesData && VoxelShapesData->IsValid())
			{
				const TArray<TSharedPtr<FJsonValue>>* Actors = nullptr;
				bool bFoundVoxelActor = false;
				bool bFoundVoxelShape = false;
				if ((*VoxelShapesData)->TryGetArrayField(TEXT("actors"), Actors) && Actors)
				{
					for (const TSharedPtr<FJsonValue>& ActorValue : *Actors)
					{
						const TSharedPtr<FJsonObject> ActorObj = ActorValue.IsValid() ? ActorValue->AsObject() : nullptr;
						const TArray<TSharedPtr<FJsonValue>>* Components = nullptr;
						if (!ActorObj.IsValid() || ActorObj->GetStringField(TEXT("name")) != VoxelLabel || !ActorObj->TryGetArrayField(TEXT("components"), Components) || !Components)
						{
							continue;
						}
						bFoundVoxelActor = true;
						for (const TSharedPtr<FJsonValue>& ComponentValue : *Components)
						{
							const TSharedPtr<FJsonObject> ComponentObj = ComponentValue.IsValid() ? ComponentValue->AsObject() : nullptr;
							const TArray<TSharedPtr<FJsonValue>>* Shapes = nullptr;
							if (!ComponentObj.IsValid() || !ComponentObj->TryGetArrayField(TEXT("shapes"), Shapes) || !Shapes)
							{
								continue;
							}
							for (const TSharedPtr<FJsonValue>& ShapeValue : *Shapes)
							{
								const TSharedPtr<FJsonObject> ShapeObj = ShapeValue.IsValid() ? ShapeValue->AsObject() : nullptr;
								bFoundVoxelShape |= ShapeObj.IsValid() && ShapeObj->GetStringField(TEXT("src")) == TEXT("collision_box/block");
							}
						}
					}
				}
				TestTrue(TEXT("Collision mesh shapes voxel includes voxel actor only when queried"), bFoundVoxelActor);
				TestTrue(TEXT("Collision mesh shapes voxel returns exact box collision"), bFoundVoxelShape);
			}
		}

		if (VoxelActor && !VoxelActor->IsActorBeingDestroyed())
		{
			VoxelActor->Destroy();
		}
	}

	// Validate OBB math under non-uniform scale + rotation (common in whitebox blockout).
	{
		const FString RotLabel = FString::Printf(TEXT("SmokeObbRot_%s"), *UniqueSuffix);
		const FVector RotLocation = ActorLocation + FVector(0.0f, 2500.0f, 100.0f);
		const FVector RotScale(4.0f, 8.0f, 0.4f);

		const FString RotSpawnBody = MakeExecRequestBody(
			TEXT("smoke-collision-obb-rot-spawn-001"),
			TEXT("spawn_actor"),
			FString::Printf(
				TEXT("{\"class_path\":\"/Script/Engine.StaticMeshActor\",\"label\":\"%s\",\"static_mesh\":\"/Engine/BasicShapes/Cube.Cube\",\"location\":{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f},\"scale\":{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f}}"),
				*RotLabel,
				RotLocation.X,
				RotLocation.Y,
				RotLocation.Z,
				RotScale.X,
				RotScale.Y,
				RotScale.Z));
		FHttpSmokeResult RotSpawnResult;
		TestTrue(TEXT("OBB rot spawn_actor request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, RotSpawnBody, RotSpawnResult));
		TestEqual(TEXT("OBB rot spawn_actor status code"), RotSpawnResult.StatusCode, 200);

		TSharedPtr<FJsonObject> RotSpawnJson;
		TestTrue(TEXT("OBB rot spawn_actor parses JSON"), ParseJson(RotSpawnResult.Body, RotSpawnJson));
		TestTrue(TEXT("OBB rot spawn_actor root ok=true"), RotSpawnJson.IsValid() && RotSpawnJson->GetBoolField(TEXT("ok")));

		const TSharedPtr<FJsonObject>* RotSpawnData = nullptr;
		TestTrue(TEXT("OBB rot spawn_actor contains data"), RotSpawnJson.IsValid() && RotSpawnJson->TryGetObjectField(TEXT("data"), RotSpawnData) && RotSpawnData && RotSpawnData->IsValid());
		const FString RotActorName = (RotSpawnData && RotSpawnData->IsValid()) ? (*RotSpawnData)->GetStringField(TEXT("name")) : FString();
		TestFalse(TEXT("OBB rot spawn_actor returned name"), RotActorName.IsEmpty());

		if (!RotActorName.IsEmpty())
		{
			const FString RotSetBody = MakeExecRequestBody(
				TEXT("smoke-collision-obb-rot-set-rot-001"),
				TEXT("level_set_actor_rotation"),
				FString::Printf(TEXT("{\"id\":\"%s\",\"rotation\":{\"pitch\":0.0,\"yaw\":0.0,\"roll\":-30.0}}"), *RotActorName));
			FHttpSmokeResult RotSetResult;
			TestTrue(TEXT("OBB rot set_actor_rotation request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, RotSetBody, RotSetResult));
			TestEqual(TEXT("OBB rot set_actor_rotation status code"), RotSetResult.StatusCode, 200);

			TSharedPtr<FJsonObject> RotMeshShapesData;
			const FString RotMeshShapesParams = FString::Printf(
				TEXT("{\"query\":{\"type\":\"obb\",\"center\":{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f},\"axis_x\":{\"x\":1.0,\"y\":0.0,\"z\":0.0},\"axis_y\":{\"x\":0.0,\"y\":1.0,\"z\":0.0},\"axis_z\":{\"x\":0.0,\"y\":0.0,\"z\":1.0},\"half_extent\":{\"x\":700.0,\"y\":700.0,\"z\":300.0}},\"filters\":{\"types\":[\"static_mesh\"]},\"detail_level\":\"standard\",\"max_shapes_per_actor\":8}"),
				RotLocation.X,
				RotLocation.Y,
				RotLocation.Z);
			if (ExecMeshShapes(TEXT("smoke-collision-mesh-shapes-rot-obb-001"), RotMeshShapesParams, RotMeshShapesData))
			{
				TestTrue(TEXT("Collision mesh shapes OBB query includes rotated non-uniform actor"), MeshShapesContainsActorShape(RotMeshShapesData, RotLabel, TEXT("collision_box/block")));
			}

			const FString RotObbBody = MakeExecRequestBody(
				TEXT("smoke-collision-obb-rot-query-001"),
				TEXT("level_get_nearby_actor_obbs"),
				FString::Printf(TEXT("{\"id\":\"%s\",\"radius\":10.0,\"include_self\":true}"), *RotActorName));
			FHttpSmokeResult RotObbResult;
			TestTrue(TEXT("OBB rot nearby obbs request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, RotObbBody, RotObbResult));
			TestEqual(TEXT("OBB rot nearby obbs status code"), RotObbResult.StatusCode, 200);

			TSharedPtr<FJsonObject> RotObbJson;
			TestTrue(TEXT("OBB rot nearby obbs parses JSON"), ParseJson(RotObbResult.Body, RotObbJson));
			TestTrue(TEXT("OBB rot nearby obbs root ok=true"), RotObbJson.IsValid() && RotObbJson->GetBoolField(TEXT("ok")));

			const TSharedPtr<FJsonObject>* RotObbData = nullptr;
			TestTrue(TEXT("OBB rot nearby obbs contains data"), RotObbJson.IsValid() && RotObbJson->TryGetObjectField(TEXT("data"), RotObbData) && RotObbData && RotObbData->IsValid());
			if (RotObbData && RotObbData->IsValid())
			{
				const TSharedPtr<FJsonObject>* SourceObb = nullptr;
				TestTrue(TEXT("OBB rot nearby obbs returns source_obb"), (*RotObbData)->TryGetObjectField(TEXT("source_obb"), SourceObb) && SourceObb && SourceObb->IsValid());
				if (SourceObb && SourceObb->IsValid())
				{
					const TSharedPtr<FJsonObject>* HalfLengths = nullptr;
					const TSharedPtr<FJsonObject>* LocalExtent = nullptr;
					TestTrue(TEXT("OBB rot has half_lengths"), (*SourceObb)->TryGetObjectField(TEXT("half_lengths"), HalfLengths) && HalfLengths && HalfLengths->IsValid());
					TestTrue(TEXT("OBB rot has local_extent"), (*SourceObb)->TryGetObjectField(TEXT("local_extent"), LocalExtent) && LocalExtent && LocalExtent->IsValid());
					if ((HalfLengths && HalfLengths->IsValid()) && (LocalExtent && LocalExtent->IsValid()))
					{
						TestTrue(TEXT("OBB rot local_extent.x ~= 50"), FMath::IsNearlyEqual((*LocalExtent)->GetNumberField(TEXT("x")), 50.0, 0.25));
						TestTrue(TEXT("OBB rot local_extent.y ~= 50"), FMath::IsNearlyEqual((*LocalExtent)->GetNumberField(TEXT("y")), 50.0, 0.25));
						TestTrue(TEXT("OBB rot local_extent.z ~= 50"), FMath::IsNearlyEqual((*LocalExtent)->GetNumberField(TEXT("z")), 50.0, 0.25));
						TestTrue(TEXT("OBB rot half_lengths.x ~= 200"), FMath::IsNearlyEqual((*HalfLengths)->GetNumberField(TEXT("x")), 200.0, 0.5));
						TestTrue(TEXT("OBB rot half_lengths.y ~= 400"), FMath::IsNearlyEqual((*HalfLengths)->GetNumberField(TEXT("y")), 400.0, 0.5));
						TestTrue(TEXT("OBB rot half_lengths.z ~= 20"), FMath::IsNearlyEqual((*HalfLengths)->GetNumberField(TEXT("z")), 20.0, 0.25));
					}
				}
			}
		}

		DestroyActorByName(TEXT("smoke-collision-obb-rot-destroy-001"), RotActorName);
	}

	DestroyDirectActors();
	DestroyActorByName(TEXT("smoke-collision-destroy-001"), ActorName);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentInterfaceNavmeshQueriesSmokeTest,
	"GptProjectTest.UeAgentInterface.Smoke.NavmeshQueries",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUeAgentInterfaceNavmeshQueriesSmokeTest::RunTest(const FString& Parameters)
{
	FScopedUeAgentHttpServer ServerScope;
	FString InitError;
	if (!ServerScope.Initialize(InitError))
	{
		AddError(FString::Printf(TEXT("Initialize failed: %s"), *InitError));
		return false;
	}

	const FString UniqueSuffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FVector BaseLocation(60000.0f, 60000.0f, 0.0f);

	auto SpawnActor = [this, &ServerScope](const FString& RequestId, const FString& ClassPath, const FString& Label, const FVector& Location, const FVector& Scale, const FString& StaticMeshPath, FString& OutActorName) -> bool
	{
		const FString Params = StaticMeshPath.IsEmpty()
			? FString::Printf(
				TEXT("{\"class_path\":\"%s\",\"label\":\"%s\",\"location\":{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f},\"scale\":{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f}}"),
				*ClassPath,
				*Label,
				Location.X,
				Location.Y,
				Location.Z,
				Scale.X,
				Scale.Y,
				Scale.Z)
			: FString::Printf(
				TEXT("{\"class_path\":\"%s\",\"label\":\"%s\",\"static_mesh\":\"%s\",\"location\":{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f},\"scale\":{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f}}"),
				*ClassPath,
				*Label,
				*StaticMeshPath,
				Location.X,
				Location.Y,
				Location.Z,
				Scale.X,
				Scale.Y,
				Scale.Z);

		const FString SpawnBody = MakeExecRequestBody(RequestId, TEXT("spawn_actor"), Params);
		FHttpSmokeResult SpawnResult;
		TestTrue(TEXT("Navmesh spawn_actor request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SpawnBody, SpawnResult, 10.0));
		TestEqual(TEXT("Navmesh spawn_actor status code"), SpawnResult.StatusCode, 200);

		TSharedPtr<FJsonObject> SpawnJson;
		TestTrue(TEXT("Navmesh spawn_actor parses JSON"), ParseJson(SpawnResult.Body, SpawnJson));
		TestTrue(TEXT("Navmesh spawn_actor root ok=true"), SpawnJson.IsValid() && SpawnJson->GetBoolField(TEXT("ok")));

		const TSharedPtr<FJsonObject>* SpawnData = nullptr;
		TestTrue(TEXT("Navmesh spawn_actor contains data"), SpawnJson.IsValid() && SpawnJson->TryGetObjectField(TEXT("data"), SpawnData) && SpawnData && SpawnData->IsValid());
		if (!(SpawnData && SpawnData->IsValid()))
		{
			return false;
		}

		OutActorName = (*SpawnData)->GetStringField(TEXT("name"));
		TestFalse(TEXT("Navmesh spawn_actor returned name"), OutActorName.IsEmpty());
		return !OutActorName.IsEmpty();
	};

	auto DestroyActorByName = [this, &ServerScope](const FString& RequestId, const FString& InActorName) -> void
	{
		if (InActorName.IsEmpty())
		{
			return;
		}

		const FString DestroyBody = MakeExecRequestBody(
			RequestId,
			TEXT("destroy_actor"),
			FString::Printf(TEXT("{\"id\":\"%s\"}"), *InActorName));
		FHttpSmokeResult DestroyResult;
		ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, DestroyBody, DestroyResult, 10.0, false);
	};

	FString FloorActorName;
	const FString FloorLabel = FString::Printf(TEXT("SmokeNavFloor_%s"), *UniqueSuffix);
	const FString BoundsLabel = FString::Printf(TEXT("SmokeNavBounds_%s"), *UniqueSuffix);
	ANavMeshBoundsVolume* NavBoundsVolume = nullptr;

	if (!SpawnActor(
			TEXT("smoke-navmesh-spawn-floor-001"),
			TEXT("/Script/Engine.StaticMeshActor"),
			FloorLabel,
			BaseLocation,
			FVector(50.0f, 50.0f, 1.0f),
			TEXT("/Engine/BasicShapes/Cube.Cube"),
			FloorActorName))
	{
		return false;
	}

	{
		auto SpawnNavBoundsVolume = [this, &BoundsLabel, BaseLocation]() -> ANavMeshBoundsVolume*
		{
			if (!GEditor)
			{
				AddError(TEXT("Navmesh test missing GEditor"));
				return nullptr;
			}

			UWorld* EditorWorld = GEditor->GetEditorWorldContext().World();
			if (!EditorWorld)
			{
				AddError(TEXT("Navmesh test missing editor world"));
				return nullptr;
			}

			FActorSpawnParameters SpawnParams;
			SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			SpawnParams.ObjectFlags |= RF_Transactional;

			ANavMeshBoundsVolume* Volume = EditorWorld->SpawnActor<ANavMeshBoundsVolume>(BaseLocation, FRotator::ZeroRotator, SpawnParams);
			if (!Volume)
			{
				AddError(TEXT("Navmesh test failed to spawn NavMeshBoundsVolume"));
				return nullptr;
			}

			Volume->SetActorLabel(BoundsLabel);
			Volume->SetActorLocation(BaseLocation);

			UCubeBuilder* Builder = NewObject<UCubeBuilder>();
			Builder->X = 20000.0f;
			Builder->Y = 20000.0f;
			Builder->Z = 8000.0f;
			UActorFactory::CreateBrushForVolumeActor(Volume, Builder);
			Volume->SetActorLocation(BaseLocation);

			if (UNavigationSystemV1* NavSys = FNavigationSystem::GetCurrent<UNavigationSystemV1>(EditorWorld))
			{
				NavSys->OnNavigationBoundsUpdated(Volume);
			}

			return Volume;
		};

		if (IsInGameThread())
		{
			NavBoundsVolume = SpawnNavBoundsVolume();
		}
		else
		{
			FEvent* Done = FPlatformProcess::GetSynchEventFromPool(true);
			AsyncTask(ENamedThreads::GameThread, [&NavBoundsVolume, SpawnNavBoundsVolume, Done]()
			{
				NavBoundsVolume = SpawnNavBoundsVolume();
				Done->Trigger();
			});

			const bool bDone = Done->Wait(FTimespan::FromSeconds(10.0));
			FPlatformProcess::ReturnSynchEventToPool(Done);
			if (!bDone)
			{
				AddError(TEXT("Navmesh test timed out waiting for nav bounds spawn"));
				NavBoundsVolume = nullptr;
			}
		}

		if (!NavBoundsVolume)
		{
			DestroyActorByName(TEXT("smoke-navmesh-destroy-floor-early"), FloorActorName);
			return false;
		}
	}

	auto RequestBuild = [this, &ServerScope](const FString& RequestId) -> bool
	{
		const FString BuildBody = MakeExecRequestBody(RequestId, TEXT("navmesh_build"), TEXT("{\"wait_for_finish\":true,\"timeout_seconds\":30}"));
		FHttpSmokeResult BuildResult;
		TestTrue(TEXT("Navmesh navmesh_build request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, BuildBody, BuildResult, 40.0));
		TestEqual(TEXT("Navmesh navmesh_build status code"), BuildResult.StatusCode, 200);

		TSharedPtr<FJsonObject> BuildJson;
		TestTrue(TEXT("Navmesh navmesh_build parses JSON"), ParseJson(BuildResult.Body, BuildJson));
		TestTrue(TEXT("Navmesh navmesh_build root ok=true"), BuildJson.IsValid() && BuildJson->GetBoolField(TEXT("ok")));
		return BuildResult.StatusCode == 200;
	};

	auto RequestProject = [this, &ServerScope](const FString& RequestId, const FVector& Point, bool& OutProjected) -> bool
	{
		const FString ProjectBody = MakeExecRequestBody(
			RequestId,
			TEXT("navmesh_project_point"),
			FString::Printf(
				TEXT("{\"point\":{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f},\"project_query_extent\":{\"x\":50.0,\"y\":50.0,\"z\":200.0}}"),
				Point.X,
				Point.Y,
				Point.Z));
		FHttpSmokeResult ProjectResult;
		TestTrue(TEXT("Navmesh navmesh_project_point request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ProjectBody, ProjectResult, 30.0));
		TestEqual(TEXT("Navmesh navmesh_project_point status code"), ProjectResult.StatusCode, 200);

		TSharedPtr<FJsonObject> ProjectJson;
		TestTrue(TEXT("Navmesh navmesh_project_point parses JSON"), ParseJson(ProjectResult.Body, ProjectJson));
		TestTrue(TEXT("Navmesh navmesh_project_point root ok=true"), ProjectJson.IsValid() && ProjectJson->GetBoolField(TEXT("ok")));

		const TSharedPtr<FJsonObject>* ProjectData = nullptr;
		TestTrue(TEXT("Navmesh navmesh_project_point contains data"), ProjectJson.IsValid() && ProjectJson->TryGetObjectField(TEXT("data"), ProjectData) && ProjectData && ProjectData->IsValid());
		OutProjected = (ProjectData && ProjectData->IsValid()) ? (*ProjectData)->GetBoolField(TEXT("projected")) : false;
		return ProjectResult.StatusCode == 200;
	};

	auto RequestFindPath = [this, &ServerScope](const FString& RequestId, const FVector& Start, const FVector& End, bool& OutPathFound) -> bool
	{
		const FString FindBody = MakeExecRequestBody(
			RequestId,
			TEXT("navmesh_find_path"),
			FString::Printf(
				TEXT("{\"start\":{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f},\"end\":{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f},\"allow_partial\":false,\"project_to_nav\":true}"),
				Start.X,
				Start.Y,
				Start.Z,
				End.X,
				End.Y,
				End.Z));
		FHttpSmokeResult FindResult;
		TestTrue(TEXT("Navmesh navmesh_find_path request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, FindBody, FindResult, 30.0));
		TestEqual(TEXT("Navmesh navmesh_find_path status code"), FindResult.StatusCode, 200);

		TSharedPtr<FJsonObject> FindJson;
		TestTrue(TEXT("Navmesh navmesh_find_path parses JSON"), ParseJson(FindResult.Body, FindJson));
		TestTrue(TEXT("Navmesh navmesh_find_path root ok=true"), FindJson.IsValid() && FindJson->GetBoolField(TEXT("ok")));

		const TSharedPtr<FJsonObject>* FindData = nullptr;
		TestTrue(TEXT("Navmesh navmesh_find_path contains data"), FindJson.IsValid() && FindJson->TryGetObjectField(TEXT("data"), FindData) && FindData && FindData->IsValid());
		OutPathFound = (FindData && FindData->IsValid()) ? (*FindData)->GetBoolField(TEXT("path_found")) : false;
		return FindResult.StatusCode == 200;
	};

	bool bPathFound = false;
	const FVector QueryStart = BaseLocation + FVector(-1500.0f, 0.0f, 200.0f);
	const FVector QueryEnd = BaseLocation + FVector(1500.0f, 0.0f, 200.0f);

	RequestBuild(TEXT("smoke-navmesh-build-001"));

	bool bProjectedStart = false;
	RequestProject(TEXT("smoke-navmesh-project-001"), QueryStart, bProjectedStart);
	TestTrue(TEXT("Navmesh projected start point"), bProjectedStart);

	{
		const FString ValidateBody = MakeExecRequestBody(
			TEXT("smoke-navmesh-validate-001"),
			TEXT("level_validate_connectivity"),
			FString::Printf(
				TEXT("{\"points\":[{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f},{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f}],\"allow_partial\":false,\"project_to_nav\":true}"),
				QueryStart.X,
				QueryStart.Y,
				QueryStart.Z,
				QueryEnd.X,
				QueryEnd.Y,
				QueryEnd.Z));
		FHttpSmokeResult ValidateResult;
		TestTrue(TEXT("Navmesh level_validate_connectivity request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ValidateBody, ValidateResult, 30.0));
		TestEqual(TEXT("Navmesh level_validate_connectivity status code"), ValidateResult.StatusCode, 200);

		TSharedPtr<FJsonObject> ValidateJson;
		TestTrue(TEXT("Navmesh level_validate_connectivity parses JSON"), ParseJson(ValidateResult.Body, ValidateJson));
		TestTrue(TEXT("Navmesh level_validate_connectivity root ok=true"), ValidateJson.IsValid() && ValidateJson->GetBoolField(TEXT("ok")));

		const TSharedPtr<FJsonObject>* ValidateData = nullptr;
		TestTrue(TEXT("Navmesh level_validate_connectivity contains data"), ValidateJson.IsValid() && ValidateJson->TryGetObjectField(TEXT("data"), ValidateData) && ValidateData && ValidateData->IsValid());
		if (ValidateData && ValidateData->IsValid())
		{
			TestTrue(TEXT("Navmesh level_validate_connectivity all_connected=true"), (*ValidateData)->GetBoolField(TEXT("all_connected")));
		}
	}

	for (int32 Attempt = 0; Attempt < 10 && !bPathFound; ++Attempt)
	{
		bool bAttemptFound = false;
		RequestFindPath(FString::Printf(TEXT("smoke-navmesh-find-%03d"), Attempt + 1), QueryStart, QueryEnd, bAttemptFound);
		bPathFound = bAttemptFound;
		if (!bPathFound)
		{
			FPlatformProcess::Sleep(0.05f);
		}
	}

	TestTrue(TEXT("Navmesh path_found=true"), bPathFound);

	DestroyActorByName(TEXT("smoke-navmesh-destroy-floor-001"), FloorActorName);
	if (NavBoundsVolume)
	{
		ANavMeshBoundsVolume* VolumeToDestroy = NavBoundsVolume;
		if (IsInGameThread())
		{
			VolumeToDestroy->Destroy();
		}
		else
		{
			FEvent* Done = FPlatformProcess::GetSynchEventFromPool(true);
			AsyncTask(ENamedThreads::GameThread, [VolumeToDestroy, Done]()
			{
				if (IsValid(VolumeToDestroy))
				{
					VolumeToDestroy->Destroy();
				}
				Done->Trigger();
			});
			Done->Wait(FTimespan::FromSeconds(10.0));
			FPlatformProcess::ReturnSynchEventToPool(Done);
		}
		NavBoundsVolume = nullptr;
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentInterfaceViewportFrameFolderSmokeTest,
	"GptProjectTest.UeAgentInterface.Smoke.ViewportFrameFolder",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUeAgentInterfaceViewportFrameFolderSmokeTest::RunTest(const FString& Parameters)
{
	if (!FApp::CanEverRender())
	{
		AddInfo(TEXT("Skipping viewport frame folder smoke because rendering is disabled in the current automation environment."));
		return true;
	}

	FScopedUeAgentHttpServer ServerScope;
	FString InitError;
	if (!ServerScope.Initialize(InitError))
	{
		AddError(FString::Printf(TEXT("Initialize failed: %s"), *InitError));
		return false;
	}

	const FString UniqueSuffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString FolderPath = FString::Printf(TEXT("__UeAgentInterfaceSmoke/ViewportFrameFolder_%s"), *UniqueSuffix);
	const FString ChildFolderPath = FolderPath + TEXT("/Child");
	const FString ActorLabelA = FString::Printf(TEXT("SmokeViewportFrameFolderA_%s"), *UniqueSuffix);
	const FString ActorLabelB = FString::Printf(TEXT("SmokeViewportFrameFolderB_%s"), *UniqueSuffix);
	const FVector ExpectedCameraLocation(-2400.0f, -1800.0f, 1100.0f);
	const FRotator ExpectedCameraRotation(-18.0f, 42.0f, 0.0f);
	const double ExpectedFov = 60.0;

	const FString SetCameraBody = MakeExecRequestBody(
		TEXT("smoke-viewport-folder-set-camera-001"),
		TEXT("viewport_set_camera"),
		FString::Printf(
			TEXT("{\"location\":{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f},\"rotation\":{\"pitch\":%.3f,\"yaw\":%.3f,\"roll\":%.3f},\"fov\":%.3f}"),
			ExpectedCameraLocation.X,
			ExpectedCameraLocation.Y,
			ExpectedCameraLocation.Z,
			ExpectedCameraRotation.Pitch,
			ExpectedCameraRotation.Yaw,
			ExpectedCameraRotation.Roll,
			ExpectedFov));
	FHttpSmokeResult SetCameraResult;
	TestTrue(TEXT("Viewport set_camera request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SetCameraBody, SetCameraResult));
	TestEqual(TEXT("Viewport set_camera status code"), SetCameraResult.StatusCode, 200);

	auto SpawnCubeActor = [this, &ServerScope](const FString& RequestId, const FString& ActorLabel, const FVector& Location, FString& OutActorName) -> bool
	{
		const FString SpawnBody = MakeExecRequestBody(
			RequestId,
			TEXT("spawn_actor"),
			FString::Printf(
				TEXT("{\"class_path\":\"/Script/Engine.StaticMeshActor\",\"label\":\"%s\",\"static_mesh\":\"/Engine/BasicShapes/Cube.Cube\",\"location\":{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f}}"),
				*ActorLabel,
				Location.X,
				Location.Y,
				Location.Z));
		FHttpSmokeResult SpawnResult;
		TestTrue(TEXT("Spawn actor request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SpawnBody, SpawnResult));
		TestEqual(TEXT("Spawn actor status code"), SpawnResult.StatusCode, 200);

		TSharedPtr<FJsonObject> SpawnJson;
		TestTrue(TEXT("Spawn actor response parses as JSON"), ParseJson(SpawnResult.Body, SpawnJson));
		TestTrue(TEXT("Spawn actor root ok=true"), SpawnJson.IsValid() && SpawnJson->GetBoolField(TEXT("ok")));

		const TSharedPtr<FJsonObject>* SpawnData = nullptr;
		TestTrue(TEXT("Spawn actor contains data object"), SpawnJson.IsValid() && SpawnJson->TryGetObjectField(TEXT("data"), SpawnData) && SpawnData && SpawnData->IsValid());
		if (!(SpawnData && SpawnData->IsValid()))
		{
			return false;
		}

		OutActorName = (*SpawnData)->GetStringField(TEXT("name"));
		TestFalse(TEXT("Spawn actor returned name"), OutActorName.IsEmpty());
		return !OutActorName.IsEmpty();
	};

	auto SetActorFolder = [this, &ServerScope](const FString& RequestId, const FString& ActorName, const FString& TargetFolderPath) -> bool
	{
		const FString FolderBody = MakeExecRequestBody(
			RequestId,
			TEXT("level_set_actor_folder"),
			FString::Printf(TEXT("{\"id\":\"%s\",\"folder_path\":\"%s\"}"), *ActorName, *TargetFolderPath));
		FHttpSmokeResult FolderResult;
		TestTrue(TEXT("Set actor folder request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, FolderBody, FolderResult));
		TestEqual(TEXT("Set actor folder status code"), FolderResult.StatusCode, 200);
		return FolderResult.StatusCode == 200;
	};

	auto DestroyActorByName = [this, &ServerScope](const FString& RequestId, const FString& ActorName) -> void
	{
		if (ActorName.IsEmpty())
		{
			return;
		}

		const FString DestroyBody = MakeExecRequestBody(
			RequestId,
			TEXT("destroy_actor"),
			FString::Printf(TEXT("{\"id\":\"%s\"}"), *ActorName));
		FHttpSmokeResult DestroyResult;
		TestTrue(TEXT("Destroy actor request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, DestroyBody, DestroyResult));
		if (DestroyResult.StatusCode == 400)
		{
			TSharedPtr<FJsonObject> DestroyJson;
			if (ParseJson(DestroyResult.Body, DestroyJson) && DestroyJson.IsValid())
			{
				const FString ErrorCode = DestroyJson->GetStringField(TEXT("error"));
				if (ErrorCode.Equals(TEXT("actor_not_found")))
				{
					return;
				}
			}
		}
		TestEqual(TEXT("Destroy actor status code"), DestroyResult.StatusCode, 200);
	};

	FString ActorNameA;
	FString ActorNameB;
	if (!SpawnCubeActor(TEXT("smoke-viewport-folder-spawn-001"), ActorLabelA, FVector(-400.0f, -100.0f, 50.0f), ActorNameA))
	{
		return false;
	}
	if (!SpawnCubeActor(TEXT("smoke-viewport-folder-spawn-002"), ActorLabelB, FVector(900.0f, 500.0f, 50.0f), ActorNameB))
	{
		DestroyActorByName(TEXT("smoke-viewport-folder-destroy-a-early"), ActorNameA);
		return false;
	}

	SetActorFolder(TEXT("smoke-viewport-folder-folder-001"), ActorNameA, FolderPath);
	SetActorFolder(TEXT("smoke-viewport-folder-folder-002"), ActorNameB, ChildFolderPath);

	const FString FrameFolderBody = MakeExecRequestBody(
		TEXT("smoke-viewport-folder-frame-001"),
		TEXT("viewport_frame_folder"),
		FString::Printf(TEXT("{\"folder_path\":\"%s\"}"), *FolderPath));
	FHttpSmokeResult FrameFolderResult;
	TestTrue(TEXT("Viewport frame_folder request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, FrameFolderBody, FrameFolderResult));
	TestEqual(TEXT("Viewport frame_folder status code"), FrameFolderResult.StatusCode, 200);

	TSharedPtr<FJsonObject> FrameFolderJson;
	TestTrue(TEXT("Viewport frame_folder response parses as JSON"), ParseJson(FrameFolderResult.Body, FrameFolderJson));
	TestTrue(TEXT("Viewport frame_folder root ok=true"), FrameFolderJson.IsValid() && FrameFolderJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* FrameFolderData = nullptr;
	TestTrue(TEXT("Viewport frame_folder contains data object"), FrameFolderJson.IsValid() && FrameFolderJson->TryGetObjectField(TEXT("data"), FrameFolderData) && FrameFolderData && FrameFolderData->IsValid());
	if (FrameFolderData && FrameFolderData->IsValid())
	{
		TestEqual(TEXT("Viewport frame_folder focused_count"), static_cast<int32>((*FrameFolderData)->GetIntegerField(TEXT("focused_count"))), 2);
		TestTrue(TEXT("Viewport frame_folder include_child_folders=true"), (*FrameFolderData)->GetBoolField(TEXT("include_child_folders")));
		TestTrue(TEXT("Viewport frame_folder contains bounds"), (*FrameFolderData)->HasTypedField<EJson::Object>(TEXT("bounds")));

		const TSharedPtr<FJsonObject>* PreviousLocation = nullptr;
		const TSharedPtr<FJsonObject>* NewLocation = nullptr;
		const TSharedPtr<FJsonObject>* Rotation = nullptr;
		TestTrue(TEXT("Viewport frame_folder contains previous_location"), (*FrameFolderData)->TryGetObjectField(TEXT("previous_location"), PreviousLocation) && PreviousLocation && PreviousLocation->IsValid());
		TestTrue(TEXT("Viewport frame_folder contains new_location"), (*FrameFolderData)->TryGetObjectField(TEXT("new_location"), NewLocation) && NewLocation && NewLocation->IsValid());
		TestTrue(TEXT("Viewport frame_folder contains rotation"), (*FrameFolderData)->TryGetObjectField(TEXT("rotation"), Rotation) && Rotation && Rotation->IsValid());

		if (PreviousLocation && PreviousLocation->IsValid())
		{
			TestTrue(TEXT("Viewport frame_folder previous location x matches"), FMath::IsNearlyEqual(static_cast<float>((*PreviousLocation)->GetNumberField(TEXT("x"))), ExpectedCameraLocation.X, 0.5f));
			TestTrue(TEXT("Viewport frame_folder previous location y matches"), FMath::IsNearlyEqual(static_cast<float>((*PreviousLocation)->GetNumberField(TEXT("y"))), ExpectedCameraLocation.Y, 0.5f));
			TestTrue(TEXT("Viewport frame_folder previous location z matches"), FMath::IsNearlyEqual(static_cast<float>((*PreviousLocation)->GetNumberField(TEXT("z"))), ExpectedCameraLocation.Z, 0.5f));
		}

		if (Rotation && Rotation->IsValid())
		{
			TestTrue(TEXT("Viewport frame_folder rotation pitch preserved"), FMath::IsNearlyEqual(static_cast<float>((*Rotation)->GetNumberField(TEXT("pitch"))), ExpectedCameraRotation.Pitch, 0.1f));
			TestTrue(TEXT("Viewport frame_folder rotation yaw preserved"), FMath::IsNearlyEqual(static_cast<float>((*Rotation)->GetNumberField(TEXT("yaw"))), ExpectedCameraRotation.Yaw, 0.1f));
			TestTrue(TEXT("Viewport frame_folder rotation roll preserved"), FMath::IsNearlyEqual(static_cast<float>((*Rotation)->GetNumberField(TEXT("roll"))), ExpectedCameraRotation.Roll, 0.1f));
		}

		TestTrue(TEXT("Viewport frame_folder fov preserved"), FMath::IsNearlyEqual(static_cast<float>((*FrameFolderData)->GetNumberField(TEXT("fov"))), static_cast<float>(ExpectedFov), 0.1f));

		if (PreviousLocation && PreviousLocation->IsValid() && NewLocation && NewLocation->IsValid())
		{
			const FVector PreviousVector(
				static_cast<float>((*PreviousLocation)->GetNumberField(TEXT("x"))),
				static_cast<float>((*PreviousLocation)->GetNumberField(TEXT("y"))),
				static_cast<float>((*PreviousLocation)->GetNumberField(TEXT("z"))));
			const FVector NewVector(
				static_cast<float>((*NewLocation)->GetNumberField(TEXT("x"))),
				static_cast<float>((*NewLocation)->GetNumberField(TEXT("y"))),
				static_cast<float>((*NewLocation)->GetNumberField(TEXT("z"))));
			TestTrue(TEXT("Viewport frame_folder moved camera"), !PreviousVector.Equals(NewVector, 0.1f));
		}
	}

	const FString DestroyFolderBody = MakeExecRequestBody(
		TEXT("smoke-viewport-folder-destroy-folder-001"),
		TEXT("level_destroy_folder_actors"),
		FString::Printf(TEXT("{\"folder_path\":\"%s\"}"), *FolderPath));
	FHttpSmokeResult DestroyFolderResult;
	TestTrue(TEXT("Level destroy_folder_actors request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, DestroyFolderBody, DestroyFolderResult));
	TestEqual(TEXT("Level destroy_folder_actors status code"), DestroyFolderResult.StatusCode, 200);

	TSharedPtr<FJsonObject> DestroyFolderJson;
	TestTrue(TEXT("Level destroy_folder_actors response parses as JSON"), ParseJson(DestroyFolderResult.Body, DestroyFolderJson));
	TestTrue(TEXT("Level destroy_folder_actors root ok=true"), DestroyFolderJson.IsValid() && DestroyFolderJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* DestroyFolderData = nullptr;
	TestTrue(TEXT("Level destroy_folder_actors contains data object"), DestroyFolderJson.IsValid() && DestroyFolderJson->TryGetObjectField(TEXT("data"), DestroyFolderData) && DestroyFolderData && DestroyFolderData->IsValid());
	if (DestroyFolderData && DestroyFolderData->IsValid())
	{
		TestEqual(TEXT("Level destroy_folder_actors deleted_count"), static_cast<int32>((*DestroyFolderData)->GetIntegerField(TEXT("deleted_count"))), 2);
		TestEqual(TEXT("Level destroy_folder_actors matched_count"), static_cast<int32>((*DestroyFolderData)->GetIntegerField(TEXT("matched_count"))), 2);
	}

	const FString DestroyFolderAgainBody = MakeExecRequestBody(
		TEXT("smoke-viewport-folder-destroy-folder-002"),
		TEXT("level_destroy_folder_actors"),
		FString::Printf(TEXT("{\"folder_path\":\"%s\"}"), *FolderPath));
	FHttpSmokeResult DestroyFolderAgainResult;
	TestTrue(TEXT("Level destroy_folder_actors second request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, DestroyFolderAgainBody, DestroyFolderAgainResult));
	TestEqual(TEXT("Level destroy_folder_actors second status code"), DestroyFolderAgainResult.StatusCode, 200);

	TSharedPtr<FJsonObject> DestroyFolderAgainJson;
	TestTrue(TEXT("Level destroy_folder_actors second response parses as JSON"), ParseJson(DestroyFolderAgainResult.Body, DestroyFolderAgainJson));
	TestTrue(TEXT("Level destroy_folder_actors second root ok=true"), DestroyFolderAgainJson.IsValid() && DestroyFolderAgainJson->GetBoolField(TEXT("ok")));
	if (DestroyFolderAgainJson.IsValid())
	{
		const TSharedPtr<FJsonObject>* DestroyFolderAgainData = nullptr;
		if (DestroyFolderAgainJson->TryGetObjectField(TEXT("data"), DestroyFolderAgainData) && DestroyFolderAgainData && DestroyFolderAgainData->IsValid())
		{
			TestEqual(TEXT("Level destroy_folder_actors second deleted_count"), static_cast<int32>((*DestroyFolderAgainData)->GetIntegerField(TEXT("deleted_count"))), 0);
		}
	}

	DestroyActorByName(TEXT("smoke-viewport-folder-destroy-a-fallback"), ActorNameA);
	DestroyActorByName(TEXT("smoke-viewport-folder-destroy-b-fallback"), ActorNameB);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentInterfaceViewportActorFrameAndPickSmokeTest,
	"GptProjectTest.UeAgentInterface.Smoke.ViewportActorFrameAndPick",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUeAgentInterfaceViewportActorFrameAndPickSmokeTest::RunTest(const FString& Parameters)
{
	if (!FApp::CanEverRender())
	{
		AddInfo(TEXT("Skipping viewport frame/pick smoke because rendering is disabled in the current automation environment."));
		return true;
	}

	FScopedUeAgentHttpServer ServerScope;
	FString InitError;
	if (!ServerScope.Initialize(InitError))
	{
		AddError(FString::Printf(TEXT("Initialize failed: %s"), *InitError));
		return false;
	}

	const FString UniqueSuffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString ActorLabel = FString::Printf(TEXT("SmokeViewportFrameActor_%s"), *UniqueSuffix);
	const FVector ExpectedCameraLocation(-3600.0f, -1800.0f, 1400.0f);
	const FRotator ExpectedCameraRotation(-14.0f, 28.0f, 0.0f);
	const double ExpectedFov = 62.0;

	const FString SetCameraBody = MakeExecRequestBody(
		TEXT("smoke-viewport-actor-set-camera-001"),
		TEXT("viewport_set_camera"),
		FString::Printf(
			TEXT("{\"location\":{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f},\"rotation\":{\"pitch\":%.3f,\"yaw\":%.3f,\"roll\":%.3f},\"fov\":%.3f}"),
			ExpectedCameraLocation.X,
			ExpectedCameraLocation.Y,
			ExpectedCameraLocation.Z,
			ExpectedCameraRotation.Pitch,
			ExpectedCameraRotation.Yaw,
			ExpectedCameraRotation.Roll,
			ExpectedFov));
	FHttpSmokeResult SetCameraResult;
	TestTrue(TEXT("Viewport set_camera request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SetCameraBody, SetCameraResult));
	TestEqual(TEXT("Viewport set_camera status code"), SetCameraResult.StatusCode, 200);

	auto DestroyActorByName = [this, &ServerScope](const FString& RequestId, const FString& ActorName) -> void
	{
		if (ActorName.IsEmpty())
		{
			return;
		}

		const FString DestroyBody = MakeExecRequestBody(
			RequestId,
			TEXT("destroy_actor"),
			FString::Printf(TEXT("{\"id\":\"%s\"}"), *ActorName));
		FHttpSmokeResult DestroyResult;
		TestTrue(TEXT("Destroy actor request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, DestroyBody, DestroyResult));
		TestEqual(TEXT("Destroy actor status code"), DestroyResult.StatusCode, 200);
	};

	const FString SpawnBody = MakeExecRequestBody(
		TEXT("smoke-viewport-actor-spawn-001"),
		TEXT("spawn_actor"),
		FString::Printf(
			TEXT("{\"class_path\":\"/Script/Engine.StaticMeshActor\",\"label\":\"%s\",\"static_mesh\":\"/Engine/BasicShapes/Cube.Cube\",\"location\":{\"x\":900.0,\"y\":650.0,\"z\":220.0},\"scale\":{\"x\":12.0,\"y\":6.0,\"z\":5.0}}"),
			*ActorLabel));
	FHttpSmokeResult SpawnResult;
	TestTrue(TEXT("Spawn actor request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SpawnBody, SpawnResult));
	TestEqual(TEXT("Spawn actor status code"), SpawnResult.StatusCode, 200);

	TSharedPtr<FJsonObject> SpawnJson;
	TestTrue(TEXT("Spawn actor response parses as JSON"), ParseJson(SpawnResult.Body, SpawnJson));
	TestTrue(TEXT("Spawn actor root ok=true"), SpawnJson.IsValid() && SpawnJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* SpawnData = nullptr;
	TestTrue(TEXT("Spawn actor contains data object"), SpawnJson.IsValid() && SpawnJson->TryGetObjectField(TEXT("data"), SpawnData) && SpawnData && SpawnData->IsValid());
	if (!(SpawnData && SpawnData->IsValid()))
	{
		return false;
	}

	const FString ActorName = (*SpawnData)->GetStringField(TEXT("name"));
	TestFalse(TEXT("Spawn actor returned name"), ActorName.IsEmpty());
	if (ActorName.IsEmpty())
	{
		return false;
	}

	const FString FrameActorBody = MakeExecRequestBody(
		TEXT("smoke-viewport-actor-frame-001"),
		TEXT("viewport_frame_actor"),
		FString::Printf(TEXT("{\"id\":\"%s\",\"padding\":1.15}"), *ActorName));
	FHttpSmokeResult FrameActorResult;
	TestTrue(TEXT("Viewport frame_actor request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, FrameActorBody, FrameActorResult));
	TestEqual(TEXT("Viewport frame_actor status code"), FrameActorResult.StatusCode, 200);

	TSharedPtr<FJsonObject> FrameActorJson;
	TestTrue(TEXT("Viewport frame_actor response parses as JSON"), ParseJson(FrameActorResult.Body, FrameActorJson));
	TestTrue(TEXT("Viewport frame_actor root ok=true"), FrameActorJson.IsValid() && FrameActorJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* FrameActorData = nullptr;
	TestTrue(TEXT("Viewport frame_actor contains data object"), FrameActorJson.IsValid() && FrameActorJson->TryGetObjectField(TEXT("data"), FrameActorData) && FrameActorData && FrameActorData->IsValid());
	if (FrameActorData && FrameActorData->IsValid())
	{
		const TSharedPtr<FJsonObject>* ActorSummary = nullptr;
		const TSharedPtr<FJsonObject>* PreviousLocation = nullptr;
		const TSharedPtr<FJsonObject>* NewLocation = nullptr;
		const TSharedPtr<FJsonObject>* Rotation = nullptr;
		TestTrue(TEXT("Viewport frame_actor contains actor summary"), (*FrameActorData)->TryGetObjectField(TEXT("actor"), ActorSummary) && ActorSummary && ActorSummary->IsValid());
		TestTrue(TEXT("Viewport frame_actor contains bounds"), (*FrameActorData)->HasTypedField<EJson::Object>(TEXT("bounds")));
		TestTrue(TEXT("Viewport frame_actor contains previous_location"), (*FrameActorData)->TryGetObjectField(TEXT("previous_location"), PreviousLocation) && PreviousLocation && PreviousLocation->IsValid());
		TestTrue(TEXT("Viewport frame_actor contains new_location"), (*FrameActorData)->TryGetObjectField(TEXT("new_location"), NewLocation) && NewLocation && NewLocation->IsValid());
		TestTrue(TEXT("Viewport frame_actor contains rotation"), (*FrameActorData)->TryGetObjectField(TEXT("rotation"), Rotation) && Rotation && Rotation->IsValid());
		TestTrue(TEXT("Viewport frame_actor padding preserved"), FMath::IsNearlyEqual(static_cast<float>((*FrameActorData)->GetNumberField(TEXT("padding"))), 1.15f, 0.01f));

		if (ActorSummary && ActorSummary->IsValid())
		{
			TestEqual(TEXT("Viewport frame_actor actor name matches"), (*ActorSummary)->GetStringField(TEXT("name")), ActorName);
			TestEqual(TEXT("Viewport frame_actor actor label matches"), (*ActorSummary)->GetStringField(TEXT("label")), ActorLabel);
		}

		if (PreviousLocation && PreviousLocation->IsValid())
		{
			TestTrue(TEXT("Viewport frame_actor previous location x matches"), FMath::IsNearlyEqual(static_cast<float>((*PreviousLocation)->GetNumberField(TEXT("x"))), ExpectedCameraLocation.X, 0.5f));
			TestTrue(TEXT("Viewport frame_actor previous location y matches"), FMath::IsNearlyEqual(static_cast<float>((*PreviousLocation)->GetNumberField(TEXT("y"))), ExpectedCameraLocation.Y, 0.5f));
			TestTrue(TEXT("Viewport frame_actor previous location z matches"), FMath::IsNearlyEqual(static_cast<float>((*PreviousLocation)->GetNumberField(TEXT("z"))), ExpectedCameraLocation.Z, 0.5f));
		}

		if (Rotation && Rotation->IsValid())
		{
			TestTrue(TEXT("Viewport frame_actor rotation pitch preserved"), FMath::IsNearlyEqual(static_cast<float>((*Rotation)->GetNumberField(TEXT("pitch"))), ExpectedCameraRotation.Pitch, 0.1f));
			TestTrue(TEXT("Viewport frame_actor rotation yaw preserved"), FMath::IsNearlyEqual(static_cast<float>((*Rotation)->GetNumberField(TEXT("yaw"))), ExpectedCameraRotation.Yaw, 0.1f));
			TestTrue(TEXT("Viewport frame_actor rotation roll preserved"), FMath::IsNearlyEqual(static_cast<float>((*Rotation)->GetNumberField(TEXT("roll"))), ExpectedCameraRotation.Roll, 0.1f));
		}

		TestTrue(TEXT("Viewport frame_actor fov preserved"), FMath::IsNearlyEqual(static_cast<float>((*FrameActorData)->GetNumberField(TEXT("fov"))), static_cast<float>(ExpectedFov), 0.1f));

		if (PreviousLocation && PreviousLocation->IsValid() && NewLocation && NewLocation->IsValid())
		{
			const FVector PreviousVector(
				static_cast<float>((*PreviousLocation)->GetNumberField(TEXT("x"))),
				static_cast<float>((*PreviousLocation)->GetNumberField(TEXT("y"))),
				static_cast<float>((*PreviousLocation)->GetNumberField(TEXT("z"))));
			const FVector NewVector(
				static_cast<float>((*NewLocation)->GetNumberField(TEXT("x"))),
				static_cast<float>((*NewLocation)->GetNumberField(TEXT("y"))),
				static_cast<float>((*NewLocation)->GetNumberField(TEXT("z"))));
			TestTrue(TEXT("Viewport frame_actor moved camera"), !PreviousVector.Equals(NewVector, 0.1f));
		}
	}

	FString WallActorName;
	{
		const FVector TargetLocation(50000.0f, 52000.0f, 220.0f);
		const FVector WallLocation(TargetLocation.X - 300.0f, TargetLocation.Y, TargetLocation.Z);

		const FString RepositionBody = MakeExecRequestBody(
			TEXT("smoke-viewport-actor-collision-reposition-001"),
			TEXT("level_set_actor_transform"),
			FString::Printf(
				TEXT("{\"id\":\"%s\",\"location\":{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f},\"rotation\":{\"pitch\":0.0,\"yaw\":0.0,\"roll\":0.0},\"scale\":{\"x\":10.0,\"y\":10.0,\"z\":10.0}}"),
				*ActorName,
				TargetLocation.X,
				TargetLocation.Y,
				TargetLocation.Z));
		FHttpSmokeResult RepositionResult;
		TestTrue(TEXT("Reposition actor for collision-aware frame request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, RepositionBody, RepositionResult));
		TestEqual(TEXT("Reposition actor for collision-aware frame status code"), RepositionResult.StatusCode, 200);

		const FString WallLabel = FString::Printf(TEXT("SmokeViewportFrameWall_%s"), *UniqueSuffix);
		const FString WallSpawnBody = MakeExecRequestBody(
			TEXT("smoke-viewport-actor-collision-wall-spawn-001"),
			TEXT("spawn_actor"),
			FString::Printf(
				TEXT("{\"class_path\":\"/Script/Engine.StaticMeshActor\",\"label\":\"%s\",\"static_mesh\":\"/Engine/BasicShapes/Cube.Cube\",\"location\":{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f},\"scale\":{\"x\":0.5,\"y\":40.0,\"z\":40.0}}"),
				*WallLabel,
				WallLocation.X,
				WallLocation.Y,
				WallLocation.Z));
		FHttpSmokeResult WallSpawnResult;
		TestTrue(TEXT("Spawn wall actor request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, WallSpawnBody, WallSpawnResult));
		TestEqual(TEXT("Spawn wall actor status code"), WallSpawnResult.StatusCode, 200);

		TSharedPtr<FJsonObject> WallSpawnJson;
		TestTrue(TEXT("Spawn wall actor response parses as JSON"), ParseJson(WallSpawnResult.Body, WallSpawnJson));
		TestTrue(TEXT("Spawn wall actor root ok=true"), WallSpawnJson.IsValid() && WallSpawnJson->GetBoolField(TEXT("ok")));
		if (WallSpawnJson.IsValid())
		{
			const TSharedPtr<FJsonObject>* WallSpawnData = nullptr;
			if (WallSpawnJson->TryGetObjectField(TEXT("data"), WallSpawnData) && WallSpawnData && WallSpawnData->IsValid())
			{
				WallActorName = (*WallSpawnData)->GetStringField(TEXT("name"));
			}
		}
		TestFalse(TEXT("Spawn wall actor returned name"), WallActorName.IsEmpty());

		const FVector CollisionCameraLocation(TargetLocation.X - 2500.0f, TargetLocation.Y, TargetLocation.Z);
		const FString SetCollisionCameraBody = MakeExecRequestBody(
			TEXT("smoke-viewport-actor-collision-set-camera-001"),
			TEXT("viewport_set_camera"),
			FString::Printf(
				TEXT("{\"location\":{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f},\"rotation\":{\"pitch\":0.0,\"yaw\":0.0,\"roll\":0.0},\"fov\":60.0}"),
				CollisionCameraLocation.X,
				CollisionCameraLocation.Y,
				CollisionCameraLocation.Z));
		FHttpSmokeResult SetCollisionCameraResult;
		TestTrue(TEXT("Viewport set_camera for collision-aware frame request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SetCollisionCameraBody, SetCollisionCameraResult));
		TestEqual(TEXT("Viewport set_camera for collision-aware frame status code"), SetCollisionCameraResult.StatusCode, 200);

		const FString FrameCollisionBody = MakeExecRequestBody(
			TEXT("smoke-viewport-actor-collision-frame-001"),
			TEXT("viewport_frame_actor"),
			FString::Printf(TEXT("{\"id\":\"%s\",\"padding\":1.1,\"collision_aware\":true,\"safety_offset_cm\":10.0}"), *ActorName));
		FHttpSmokeResult FrameCollisionResult;
		TestTrue(TEXT("Viewport frame_actor collision-aware request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, FrameCollisionBody, FrameCollisionResult));
		TestEqual(TEXT("Viewport frame_actor collision-aware status code"), FrameCollisionResult.StatusCode, 200);

		TSharedPtr<FJsonObject> FrameCollisionJson;
		TestTrue(TEXT("Viewport frame_actor collision-aware response parses as JSON"), ParseJson(FrameCollisionResult.Body, FrameCollisionJson));
		TestTrue(TEXT("Viewport frame_actor collision-aware root ok=true"), FrameCollisionJson.IsValid() && FrameCollisionJson->GetBoolField(TEXT("ok")));

		const TSharedPtr<FJsonObject>* FrameCollisionData = nullptr;
		TestTrue(TEXT("Viewport frame_actor collision-aware contains data object"), FrameCollisionJson.IsValid() && FrameCollisionJson->TryGetObjectField(TEXT("data"), FrameCollisionData) && FrameCollisionData && FrameCollisionData->IsValid());
		if (FrameCollisionData && FrameCollisionData->IsValid())
		{
			TestTrue(TEXT("Viewport frame_actor collision-aware flag true"), (*FrameCollisionData)->GetBoolField(TEXT("collision_aware")));
			TestTrue(TEXT("Viewport frame_actor collision-adjusted true"), (*FrameCollisionData)->GetBoolField(TEXT("collision_adjusted")));

			const TSharedPtr<FJsonObject>* DesiredLocation = nullptr;
			const TSharedPtr<FJsonObject>* NewLocation = nullptr;
			const TSharedPtr<FJsonObject>* CollisionTrace = nullptr;
			TestTrue(TEXT("Viewport frame_actor collision-aware contains desired_location"), (*FrameCollisionData)->TryGetObjectField(TEXT("desired_location"), DesiredLocation) && DesiredLocation && DesiredLocation->IsValid());
			TestTrue(TEXT("Viewport frame_actor collision-aware contains new_location"), (*FrameCollisionData)->TryGetObjectField(TEXT("new_location"), NewLocation) && NewLocation && NewLocation->IsValid());
			TestTrue(TEXT("Viewport frame_actor collision-aware contains collision_trace"), (*FrameCollisionData)->TryGetObjectField(TEXT("collision_trace"), CollisionTrace) && CollisionTrace && CollisionTrace->IsValid());
			if (CollisionTrace && CollisionTrace->IsValid())
			{
				TestTrue(TEXT("Viewport frame_actor collision-aware trace hit"), (*CollisionTrace)->GetBoolField(TEXT("hit")));
			}
			if (DesiredLocation && DesiredLocation->IsValid() && NewLocation && NewLocation->IsValid())
			{
				const FVector DesiredVector(
					static_cast<float>((*DesiredLocation)->GetNumberField(TEXT("x"))),
					static_cast<float>((*DesiredLocation)->GetNumberField(TEXT("y"))),
					static_cast<float>((*DesiredLocation)->GetNumberField(TEXT("z"))));
				const FVector NewVector(
					static_cast<float>((*NewLocation)->GetNumberField(TEXT("x"))),
					static_cast<float>((*NewLocation)->GetNumberField(TEXT("y"))),
					static_cast<float>((*NewLocation)->GetNumberField(TEXT("z"))));
				TestTrue(TEXT("Viewport frame_actor collision-aware moved camera off desired"), !DesiredVector.Equals(NewVector, 0.1f));
			}
		}
	}

	const FString RepositionForPickBody = MakeExecRequestBody(
		TEXT("smoke-viewport-actor-reposition-001"),
		TEXT("level_set_actor_transform"),
		FString::Printf(
			TEXT("{\"id\":\"%s\",\"location\":{\"x\":6000.0,\"y\":0.0,\"z\":250.0},\"rotation\":{\"pitch\":0.0,\"yaw\":0.0,\"roll\":0.0},\"scale\":{\"x\":50.0,\"y\":50.0,\"z\":20.0}}"),
			*ActorName));
	FHttpSmokeResult RepositionForPickResult;
	TestTrue(TEXT("Level set_actor_transform request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, RepositionForPickBody, RepositionForPickResult));
	TestEqual(TEXT("Level set_actor_transform status code"), RepositionForPickResult.StatusCode, 200);

	const FString SetPickCameraBody = MakeExecRequestBody(
		TEXT("smoke-viewport-pick-set-camera-001"),
		TEXT("viewport_set_camera"),
		TEXT("{\"location\":{\"x\":0.0,\"y\":0.0,\"z\":250.0},\"rotation\":{\"pitch\":0.0,\"yaw\":0.0,\"roll\":0.0},\"fov\":60.0}"));
	FHttpSmokeResult SetPickCameraResult;
	TestTrue(TEXT("Viewport set_camera for pick request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SetPickCameraBody, SetPickCameraResult));
	TestEqual(TEXT("Viewport set_camera for pick status code"), SetPickCameraResult.StatusCode, 200);

	const FIntPoint PickScreenPoint(400, 300);
	const FString DeprojectBody = MakeExecRequestBody(
		TEXT("smoke-viewport-pick-deproject-001"),
		TEXT("viewport_deproject_screen_to_world"),
		FString::Printf(TEXT("{\"screen_x\":%d,\"screen_y\":%d}"), PickScreenPoint.X, PickScreenPoint.Y));
	FHttpSmokeResult DeprojectResult;
	TestTrue(TEXT("Viewport deproject request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, DeprojectBody, DeprojectResult));
	TestEqual(TEXT("Viewport deproject status code"), DeprojectResult.StatusCode, 200);

	TSharedPtr<FJsonObject> DeprojectJson;
	TestTrue(TEXT("Viewport deproject response parses as JSON"), ParseJson(DeprojectResult.Body, DeprojectJson));
	TestTrue(TEXT("Viewport deproject root ok=true"), DeprojectJson.IsValid() && DeprojectJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* DeprojectData = nullptr;
	TestTrue(TEXT("Viewport deproject contains data object"), DeprojectJson.IsValid() && DeprojectJson->TryGetObjectField(TEXT("data"), DeprojectData) && DeprojectData && DeprojectData->IsValid());

	FVector DeprojectOrigin = FVector::ZeroVector;
	FVector DeprojectDirection = FVector::ForwardVector;
	if (DeprojectData && DeprojectData->IsValid())
	{
		const TSharedPtr<FJsonObject>* OriginObject = nullptr;
		const TSharedPtr<FJsonObject>* DirectionObject = nullptr;
		TestTrue(TEXT("Viewport deproject contains world_origin"), (*DeprojectData)->TryGetObjectField(TEXT("world_origin"), OriginObject) && OriginObject && OriginObject->IsValid());
		TestTrue(TEXT("Viewport deproject contains world_direction"), (*DeprojectData)->TryGetObjectField(TEXT("world_direction"), DirectionObject) && DirectionObject && DirectionObject->IsValid());

		if (OriginObject && OriginObject->IsValid())
		{
			DeprojectOrigin = FVector(
				static_cast<float>((*OriginObject)->GetNumberField(TEXT("x"))),
				static_cast<float>((*OriginObject)->GetNumberField(TEXT("y"))),
				static_cast<float>((*OriginObject)->GetNumberField(TEXT("z"))));
		}
		if (DirectionObject && DirectionObject->IsValid())
		{
			DeprojectDirection = FVector(
				static_cast<float>((*DirectionObject)->GetNumberField(TEXT("x"))),
				static_cast<float>((*DirectionObject)->GetNumberField(TEXT("y"))),
				static_cast<float>((*DirectionObject)->GetNumberField(TEXT("z")))).GetSafeNormal();
		}
	}

	const FVector PickLocation = DeprojectOrigin + (DeprojectDirection * 900.0f);
	const FString RepositionForPrecisePickBody = MakeExecRequestBody(
		TEXT("smoke-viewport-actor-reposition-002"),
		TEXT("level_set_actor_transform"),
		FString::Printf(
			TEXT("{\"id\":\"%s\",\"location\":{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f},\"rotation\":{\"pitch\":0.0,\"yaw\":0.0,\"roll\":0.0},\"scale\":{\"x\":12.0,\"y\":12.0,\"z\":12.0}}"),
			*ActorName,
			PickLocation.X,
			PickLocation.Y,
			PickLocation.Z));
	FHttpSmokeResult RepositionForPrecisePickResult;
	TestTrue(TEXT("Level set_actor_transform for precise pick request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, RepositionForPrecisePickBody, RepositionForPrecisePickResult));
	TestEqual(TEXT("Level set_actor_transform for precise pick status code"), RepositionForPrecisePickResult.StatusCode, 200);

	const FString PickBody = MakeExecRequestBody(
		TEXT("smoke-viewport-pick-actor-001"),
		TEXT("viewport_pick_actor_at_screen"),
		FString::Printf(TEXT("{\"screen_x\":%d,\"screen_y\":%d,\"trace_distance\":50000.0}"), PickScreenPoint.X, PickScreenPoint.Y));
	FHttpSmokeResult PickResult;
	TestTrue(TEXT("Viewport pick_actor_at_screen request completed"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, PickBody, PickResult));
	TestEqual(TEXT("Viewport pick_actor_at_screen status code"), PickResult.StatusCode, 200);

	TSharedPtr<FJsonObject> PickJson;
	TestTrue(TEXT("Viewport pick_actor_at_screen response parses as JSON"), ParseJson(PickResult.Body, PickJson));
	TestTrue(TEXT("Viewport pick_actor_at_screen root ok=true"), PickJson.IsValid() && PickJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* PickData = nullptr;
	TestTrue(TEXT("Viewport pick_actor_at_screen contains data object"), PickJson.IsValid() && PickJson->TryGetObjectField(TEXT("data"), PickData) && PickData && PickData->IsValid());
	if (PickData && PickData->IsValid())
	{
		const TSharedPtr<FJsonObject>* PickedActor = nullptr;
		TestTrue(TEXT("Viewport pick_actor_at_screen contains actor summary"), (*PickData)->TryGetObjectField(TEXT("actor"), PickedActor) && PickedActor && PickedActor->IsValid());
		TestTrue(TEXT("Viewport pick_actor_at_screen contains screen_point"), (*PickData)->HasTypedField<EJson::Object>(TEXT("screen_point")));
		TestTrue(TEXT("Viewport pick_actor_at_screen contains world_origin"), (*PickData)->HasTypedField<EJson::Object>(TEXT("world_origin")));
		TestTrue(TEXT("Viewport pick_actor_at_screen contains world_direction"), (*PickData)->HasTypedField<EJson::Object>(TEXT("world_direction")));
		TestTrue(TEXT("Viewport pick_actor_at_screen contains trace_channel"), (*PickData)->HasTypedField<EJson::String>(TEXT("trace_channel")));
		TestTrue(TEXT("Viewport pick_actor_at_screen contains hit distance"), (*PickData)->HasTypedField<EJson::Number>(TEXT("distance")));
		TestEqual(TEXT("Viewport pick_actor_at_screen actor_name matches"), (*PickData)->GetStringField(TEXT("actor_name")), ActorName);

		if (PickedActor && PickedActor->IsValid())
		{
			TestEqual(TEXT("Viewport pick_actor_at_screen actor summary name matches"), (*PickedActor)->GetStringField(TEXT("name")), ActorName);
			TestEqual(TEXT("Viewport pick_actor_at_screen actor summary label matches"), (*PickedActor)->GetStringField(TEXT("label")), ActorLabel);
		}
	}

	DestroyActorByName(TEXT("smoke-viewport-actor-destroy-001"), ActorName);
	DestroyActorByName(TEXT("smoke-viewport-actor-destroy-wall-001"), WallActorName);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentInterfaceViewportSelectAndNearbyObbsSmokeTest,
	"GptProjectTest.UeAgentInterface.Smoke.ViewportSelectAndNearbyObbs",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUeAgentInterfaceViewportSelectAndNearbyObbsSmokeTest::RunTest(const FString& Parameters)
{
	if (!FApp::CanEverRender())
	{
		AddInfo(TEXT("Skipping viewport select/obb smoke because rendering is disabled in the current automation environment."));
		return true;
	}

	FScopedUeAgentHttpServer ServerScope;
	FString InitError;
	if (!ServerScope.Initialize(InitError))
	{
		AddError(FString::Printf(TEXT("Initialize failed: %s"), *InitError));
		return false;
	}

	const FString UniqueSuffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString SubjectLabel = FString::Printf(TEXT("SmokeViewportSelectSubject_%s"), *UniqueSuffix);
	const FString NearbyLabel = FString::Printf(TEXT("SmokeViewportSelectNearby_%s"), *UniqueSuffix);
	const FString FarLabel = FString::Printf(TEXT("SmokeViewportSelectFar_%s"), *UniqueSuffix);

	auto SpawnCubeActor = [this, &ServerScope](const FString& RequestId, const FString& ActorLabel, const FVector& Location, const FVector& Scale, FString& OutActorName) -> bool
	{
		const FString SpawnBody = MakeExecRequestBody(
			RequestId,
			TEXT("spawn_actor"),
			FString::Printf(
				TEXT("{\"class_path\":\"/Script/Engine.StaticMeshActor\",\"label\":\"%s\",\"static_mesh\":\"/Engine/BasicShapes/Cube.Cube\",\"location\":{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f},\"scale\":{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f}}"),
				*ActorLabel,
				Location.X, Location.Y, Location.Z,
				Scale.X, Scale.Y, Scale.Z));
		FHttpSmokeResult SpawnResult;
		TestTrue(TEXT("Spawn cube actor request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SpawnBody, SpawnResult));
		TestEqual(TEXT("Spawn cube actor status code"), SpawnResult.StatusCode, 200);

		TSharedPtr<FJsonObject> SpawnJson;
		TestTrue(TEXT("Spawn cube actor response parses as JSON"), ParseJson(SpawnResult.Body, SpawnJson));
		TestTrue(TEXT("Spawn cube actor root ok=true"), SpawnJson.IsValid() && SpawnJson->GetBoolField(TEXT("ok")));

		const TSharedPtr<FJsonObject>* SpawnData = nullptr;
		TestTrue(TEXT("Spawn cube actor contains data object"), SpawnJson.IsValid() && SpawnJson->TryGetObjectField(TEXT("data"), SpawnData) && SpawnData && SpawnData->IsValid());
		if (!(SpawnData && SpawnData->IsValid()))
		{
			return false;
		}

		OutActorName = (*SpawnData)->GetStringField(TEXT("name"));
		return !OutActorName.IsEmpty();
	};

	auto DestroyActorByName = [this, &ServerScope](const FString& RequestId, const FString& ActorName) -> void
	{
		if (ActorName.IsEmpty())
		{
			return;
		}

		const FString DestroyBody = MakeExecRequestBody(RequestId, TEXT("destroy_actor"), FString::Printf(TEXT("{\"id\":\"%s\"}"), *ActorName));
		FHttpSmokeResult DestroyResult;
		ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, DestroyBody, DestroyResult);
	};

	const FString SetCameraBody = MakeExecRequestBody(
		TEXT("smoke-viewport-select-set-camera-001"),
		TEXT("viewport_set_camera"),
		TEXT("{\"location\":{\"x\":0.0,\"y\":0.0,\"z\":250.0},\"rotation\":{\"pitch\":0.0,\"yaw\":0.0,\"roll\":0.0},\"fov\":60.0}"));
	FHttpSmokeResult SetCameraResult;
	TestTrue(TEXT("Viewport set_camera request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SetCameraBody, SetCameraResult));
	TestEqual(TEXT("Viewport set_camera status code"), SetCameraResult.StatusCode, 200);

	const FIntPoint PickScreenPoint(400, 300);
	const FString DeprojectBody = MakeExecRequestBody(
		TEXT("smoke-viewport-select-deproject-001"),
		TEXT("viewport_deproject_screen_to_world"),
		FString::Printf(TEXT("{\"screen_x\":%d,\"screen_y\":%d}"), PickScreenPoint.X, PickScreenPoint.Y));
	FHttpSmokeResult DeprojectResult;
	TestTrue(TEXT("Viewport deproject request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, DeprojectBody, DeprojectResult));
	TestEqual(TEXT("Viewport deproject status code"), DeprojectResult.StatusCode, 200);

	TSharedPtr<FJsonObject> DeprojectJson;
	TestTrue(TEXT("Viewport deproject response parses as JSON"), ParseJson(DeprojectResult.Body, DeprojectJson));
	TestTrue(TEXT("Viewport deproject root ok=true"), DeprojectJson.IsValid() && DeprojectJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* DeprojectData = nullptr;
	TestTrue(TEXT("Viewport deproject contains data"), DeprojectJson.IsValid() && DeprojectJson->TryGetObjectField(TEXT("data"), DeprojectData) && DeprojectData && DeprojectData->IsValid());

	FVector DeprojectOrigin = FVector::ZeroVector;
	FVector DeprojectDirection = FVector::ForwardVector;
	if (DeprojectData && DeprojectData->IsValid())
	{
		const TSharedPtr<FJsonObject>* OriginObject = nullptr;
		const TSharedPtr<FJsonObject>* DirectionObject = nullptr;
		if ((*DeprojectData)->TryGetObjectField(TEXT("world_origin"), OriginObject) && OriginObject && OriginObject->IsValid())
		{
			DeprojectOrigin = FVector(
				static_cast<float>((*OriginObject)->GetNumberField(TEXT("x"))),
				static_cast<float>((*OriginObject)->GetNumberField(TEXT("y"))),
				static_cast<float>((*OriginObject)->GetNumberField(TEXT("z"))));
		}
		if ((*DeprojectData)->TryGetObjectField(TEXT("world_direction"), DirectionObject) && DirectionObject && DirectionObject->IsValid())
		{
			DeprojectDirection = FVector(
				static_cast<float>((*DirectionObject)->GetNumberField(TEXT("x"))),
				static_cast<float>((*DirectionObject)->GetNumberField(TEXT("y"))),
				static_cast<float>((*DirectionObject)->GetNumberField(TEXT("z")))).GetSafeNormal();
		}
	}

	const FVector SubjectLocation = DeprojectOrigin + (DeprojectDirection * 900.0f);
	const FVector NearbyLocation = SubjectLocation + FVector(320.0f, 0.0f, 0.0f);
	const FVector FarLocation = SubjectLocation + FVector(1800.0f, 0.0f, 0.0f);

	FString SubjectActorName;
	FString NearbyActorName;
	FString FarActorName;
	if (!SpawnCubeActor(TEXT("smoke-viewport-select-spawn-001"), SubjectLabel, SubjectLocation, FVector(10.0f, 8.0f, 6.0f), SubjectActorName) ||
		!SpawnCubeActor(TEXT("smoke-viewport-select-spawn-002"), NearbyLabel, NearbyLocation, FVector(4.0f, 4.0f, 4.0f), NearbyActorName) ||
		!SpawnCubeActor(TEXT("smoke-viewport-select-spawn-003"), FarLabel, FarLocation, FVector(4.0f, 4.0f, 4.0f), FarActorName))
	{
		DestroyActorByName(TEXT("smoke-viewport-select-destroy-subject-early"), SubjectActorName);
		DestroyActorByName(TEXT("smoke-viewport-select-destroy-nearby-early"), NearbyActorName);
		DestroyActorByName(TEXT("smoke-viewport-select-destroy-far-early"), FarActorName);
		return false;
	}

	const FString SelectBody = MakeExecRequestBody(
		TEXT("smoke-viewport-select-actor-001"),
		TEXT("viewport_select_actor_at_screen"),
		FString::Printf(TEXT("{\"screen_x\":%d,\"screen_y\":%d,\"selection_mode\":\"replace\",\"trace_distance\":50000.0}"), PickScreenPoint.X, PickScreenPoint.Y));
	FHttpSmokeResult SelectResult;
	TestTrue(TEXT("Viewport select_actor_at_screen request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SelectBody, SelectResult));
	TestEqual(TEXT("Viewport select_actor_at_screen status code"), SelectResult.StatusCode, 200);

	TSharedPtr<FJsonObject> SelectJson;
	TestTrue(TEXT("Viewport select_actor_at_screen parses as JSON"), ParseJson(SelectResult.Body, SelectJson));
	TestTrue(TEXT("Viewport select_actor_at_screen root ok=true"), SelectJson.IsValid() && SelectJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* SelectData = nullptr;
	TestTrue(TEXT("Viewport select_actor_at_screen contains data"), SelectJson.IsValid() && SelectJson->TryGetObjectField(TEXT("data"), SelectData) && SelectData && SelectData->IsValid());
	if (SelectData && SelectData->IsValid())
	{
		const TSharedPtr<FJsonObject>* SelectedActor = nullptr;
		TestTrue(TEXT("Viewport select_actor_at_screen returns actor info"), (*SelectData)->TryGetObjectField(TEXT("actor_info"), SelectedActor) && SelectedActor && SelectedActor->IsValid());
		if (SelectedActor && SelectedActor->IsValid())
		{
			TestEqual(TEXT("Viewport select_actor_at_screen selected actor label"), (*SelectedActor)->GetStringField(TEXT("label")), SubjectLabel);
		}
	}

	const FString NearbyObbBody = MakeExecRequestBody(
		TEXT("smoke-level-nearby-obbs-001"),
		TEXT("level_get_nearby_actor_obbs"),
		FString::Printf(TEXT("{\"id\":\"%s\",\"radius\":600.0}"), *SubjectActorName));
	FHttpSmokeResult NearbyObbResult;
	TestTrue(TEXT("Level nearby actor obbs request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, NearbyObbBody, NearbyObbResult));
	TestEqual(TEXT("Level nearby actor obbs status code"), NearbyObbResult.StatusCode, 200);

	TSharedPtr<FJsonObject> NearbyObbJson;
	TestTrue(TEXT("Level nearby actor obbs parses as JSON"), ParseJson(NearbyObbResult.Body, NearbyObbJson));
	TestTrue(TEXT("Level nearby actor obbs root ok=true"), NearbyObbJson.IsValid() && NearbyObbJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* NearbyObbData = nullptr;
	TestTrue(TEXT("Level nearby actor obbs contains data"), NearbyObbJson.IsValid() && NearbyObbJson->TryGetObjectField(TEXT("data"), NearbyObbData) && NearbyObbData && NearbyObbData->IsValid());
	if (NearbyObbData && NearbyObbData->IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* NearbyActors = nullptr;
		TestTrue(TEXT("Level nearby actor obbs returns actors array"), (*NearbyObbData)->TryGetArrayField(TEXT("actors"), NearbyActors) && NearbyActors);
		if (NearbyActors)
		{
			TestTrue(TEXT("Level nearby actor obbs returns at least one actor"), NearbyActors->Num() >= 1);
			bool bFoundNearby = false;
			bool bFoundFar = false;
			for (const TSharedPtr<FJsonValue>& ItemValue : *NearbyActors)
			{
				if (!ItemValue.IsValid() || ItemValue->Type != EJson::Object)
				{
					continue;
				}
				const TSharedPtr<FJsonObject> ItemObject = ItemValue->AsObject();
				const TSharedPtr<FJsonObject>* ActorObject = nullptr;
				if (!ItemObject.IsValid() || !ItemObject->TryGetObjectField(TEXT("actor"), ActorObject) || !ActorObject || !ActorObject->IsValid())
				{
					continue;
				}

				const FString Label = (*ActorObject)->GetStringField(TEXT("label"));
				bFoundNearby |= Label == NearbyLabel;
				bFoundFar |= Label == FarLabel;
				TestTrue(TEXT("Level nearby actor obbs entry contains obb"), ItemObject->HasTypedField<EJson::Object>(TEXT("obb")));
			}
			TestTrue(TEXT("Level nearby actor obbs includes nearby actor"), bFoundNearby);
			TestFalse(TEXT("Level nearby actor obbs excludes far actor"), bFoundFar);
		}
	}

	DestroyActorByName(TEXT("smoke-viewport-select-destroy-001"), SubjectActorName);
	DestroyActorByName(TEXT("smoke-viewport-select-destroy-002"), NearbyActorName);
	DestroyActorByName(TEXT("smoke-viewport-select-destroy-003"), FarActorName);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentInterfaceModelingReadonlySmokeTest,
	"GptProjectTest.UeAgentInterface.Smoke.ModelingReadonlyQueries",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUeAgentInterfaceModelingReadonlySmokeTest::RunTest(const FString& Parameters)
{
	FScopedUeAgentHttpServer ServerScope;
	FString InitError;
	if (!ServerScope.Initialize(InitError))
	{
		AddError(FString::Printf(TEXT("Initialize failed: %s"), *InitError));
		return false;
	}

	const FString ActivateBody = MakeExecRequestBody(TEXT("smoke-modeling-activate-001"), TEXT("modeling_activate_mode"));
	FHttpSmokeResult ActivateResult;
	TestTrue(TEXT("Modeling activate request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ActivateBody, ActivateResult));
	TestEqual(TEXT("Modeling activate status code"), ActivateResult.StatusCode, 200);

	TSharedPtr<FJsonObject> ActivateJson;
	TestTrue(TEXT("Modeling activate response parses as JSON"), ParseJson(ActivateResult.Body, ActivateJson));
	TestTrue(TEXT("Modeling activate root ok=true"), ActivateJson.IsValid() && ActivateJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* ActivateData = nullptr;
	TestTrue(TEXT("Modeling activate contains data object"), ActivateJson.IsValid() && ActivateJson->TryGetObjectField(TEXT("data"), ActivateData) && ActivateData && ActivateData->IsValid());
	if (ActivateData && ActivateData->IsValid())
	{
		TestTrue(TEXT("Modeling activate reports mode_active"), (*ActivateData)->GetBoolField(TEXT("mode_active")));
		TestTrue(TEXT("Modeling activate returns mode id"), !(*ActivateData)->GetStringField(TEXT("mode_id")).IsEmpty());
	}

	const FString GetSelectionBody = MakeExecRequestBody(TEXT("smoke-modeling-selection-001"), TEXT("modeling_get_selection"));
	FHttpSmokeResult GetSelectionResult;
	TestTrue(TEXT("Modeling get_selection request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, GetSelectionBody, GetSelectionResult));
	TestEqual(TEXT("Modeling get_selection status code"), GetSelectionResult.StatusCode, 200);

	TSharedPtr<FJsonObject> GetSelectionJson;
	TestTrue(TEXT("Modeling get_selection response parses as JSON"), ParseJson(GetSelectionResult.Body, GetSelectionJson));
	TestTrue(TEXT("Modeling get_selection root ok=true"), GetSelectionJson.IsValid() && GetSelectionJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* GetSelectionData = nullptr;
	TestTrue(TEXT("Modeling get_selection contains data object"), GetSelectionJson.IsValid() && GetSelectionJson->TryGetObjectField(TEXT("data"), GetSelectionData) && GetSelectionData && GetSelectionData->IsValid());
	if (GetSelectionData && GetSelectionData->IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* SelectedActors = nullptr;
		TestTrue(TEXT("Modeling get_selection contains selected_actors array"), (*GetSelectionData)->TryGetArrayField(TEXT("selected_actors"), SelectedActors) && SelectedActors != nullptr);
		TestTrue(TEXT("Modeling get_selection contains mesh_selection_element_type"), (*GetSelectionData)->HasTypedField<EJson::String>(TEXT("mesh_selection_element_type")));
		TestTrue(TEXT("Modeling get_selection contains mesh_topology_mode"), (*GetSelectionData)->HasTypedField<EJson::String>(TEXT("mesh_topology_mode")));
		TestTrue(TEXT("Modeling get_selection contains mesh_target_count"), (*GetSelectionData)->HasTypedField<EJson::Number>(TEXT("mesh_target_count")));
		TestTrue(TEXT("Modeling get_selection contains mesh_selection_empty"), (*GetSelectionData)->HasTypedField<EJson::Boolean>(TEXT("mesh_selection_empty")));
	}

	const FString GetMeshSelectionInfoBody = MakeExecRequestBody(TEXT("smoke-modeling-mesh-selection-001"), TEXT("modeling_get_mesh_selection_info"));
	FHttpSmokeResult GetMeshSelectionInfoResult;
	TestTrue(TEXT("Modeling get_mesh_selection_info request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, GetMeshSelectionInfoBody, GetMeshSelectionInfoResult));
	TestEqual(TEXT("Modeling get_mesh_selection_info status code"), GetMeshSelectionInfoResult.StatusCode, 200);

	TSharedPtr<FJsonObject> GetMeshSelectionInfoJson;
	TestTrue(TEXT("Modeling get_mesh_selection_info response parses as JSON"), ParseJson(GetMeshSelectionInfoResult.Body, GetMeshSelectionInfoJson));
	TestTrue(TEXT("Modeling get_mesh_selection_info root ok=true"), GetMeshSelectionInfoJson.IsValid() && GetMeshSelectionInfoJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* GetMeshSelectionInfoData = nullptr;
	TestTrue(TEXT("Modeling get_mesh_selection_info contains data object"), GetMeshSelectionInfoJson.IsValid() && GetMeshSelectionInfoJson->TryGetObjectField(TEXT("data"), GetMeshSelectionInfoData) && GetMeshSelectionInfoData && GetMeshSelectionInfoData->IsValid());
	if (GetMeshSelectionInfoData && GetMeshSelectionInfoData->IsValid())
	{
		TestTrue(TEXT("Modeling get_mesh_selection_info contains element_type"), (*GetMeshSelectionInfoData)->HasTypedField<EJson::String>(TEXT("element_type")));
		TestTrue(TEXT("Modeling get_mesh_selection_info contains topology_mode"), (*GetMeshSelectionInfoData)->HasTypedField<EJson::String>(TEXT("topology_mode")));
		TestTrue(TEXT("Modeling get_mesh_selection_info contains target_count"), (*GetMeshSelectionInfoData)->HasTypedField<EJson::Number>(TEXT("target_count")));
		TestTrue(TEXT("Modeling get_mesh_selection_info contains is_empty"), (*GetMeshSelectionInfoData)->HasTypedField<EJson::Boolean>(TEXT("is_empty")));
		TestTrue(TEXT("Modeling get_mesh_selection_info contains has_active_targets"), (*GetMeshSelectionInfoData)->HasTypedField<EJson::Boolean>(TEXT("has_active_targets")));
	}

	const FString GetMeshSpatialInfoBody = MakeExecRequestBody(
		TEXT("smoke-modeling-mesh-spatial-info-001"),
		TEXT("modeling_get_mesh_spatial_info"),
		TEXT("{\"asset_path\":\"/Engine/BasicShapes/Cube.Cube\"}"));
	FHttpSmokeResult GetMeshSpatialInfoResult;
	TestTrue(TEXT("Modeling get_mesh_spatial_info request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, GetMeshSpatialInfoBody, GetMeshSpatialInfoResult));
	TestEqual(TEXT("Modeling get_mesh_spatial_info status code"), GetMeshSpatialInfoResult.StatusCode, 200);

	TSharedPtr<FJsonObject> GetMeshSpatialInfoJson;
	TestTrue(TEXT("Modeling get_mesh_spatial_info response parses as JSON"), ParseJson(GetMeshSpatialInfoResult.Body, GetMeshSpatialInfoJson));
	TestTrue(TEXT("Modeling get_mesh_spatial_info root ok=true"), GetMeshSpatialInfoJson.IsValid() && GetMeshSpatialInfoJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* GetMeshSpatialInfoData = nullptr;
	TestTrue(TEXT("Modeling get_mesh_spatial_info contains data object"), GetMeshSpatialInfoJson.IsValid() && GetMeshSpatialInfoJson->TryGetObjectField(TEXT("data"), GetMeshSpatialInfoData) && GetMeshSpatialInfoData && GetMeshSpatialInfoData->IsValid());
	if (GetMeshSpatialInfoData && GetMeshSpatialInfoData->IsValid())
	{
		const TSharedPtr<FJsonObject>* Target = nullptr;
		const TSharedPtr<FJsonObject>* Frame = nullptr;
		const TSharedPtr<FJsonObject>* Aabb = nullptr;
		const TSharedPtr<FJsonObject>* PivotInAabb = nullptr;
		TestTrue(TEXT("Modeling mesh spatial info contains target"), (*GetMeshSpatialInfoData)->TryGetObjectField(TEXT("target"), Target) && Target && Target->IsValid());
		TestTrue(TEXT("Modeling mesh spatial info contains frame"), (*GetMeshSpatialInfoData)->TryGetObjectField(TEXT("frame"), Frame) && Frame && Frame->IsValid());
		TestTrue(TEXT("Modeling mesh spatial info contains aabb"), (*GetMeshSpatialInfoData)->TryGetObjectField(TEXT("aabb"), Aabb) && Aabb && Aabb->IsValid());
		TestTrue(TEXT("Modeling mesh spatial info contains pivot_in_aabb"), (*GetMeshSpatialInfoData)->TryGetObjectField(TEXT("pivot_in_aabb"), PivotInAabb) && PivotInAabb && PivotInAabb->IsValid());
		TestFalse(TEXT("Modeling mesh spatial info omits issues for regular cube"), (*GetMeshSpatialInfoData)->HasField(TEXT("issues")));

		if (Target && Target->IsValid())
		{
			TestEqual(TEXT("Modeling mesh spatial info target type"), (*Target)->GetStringField(TEXT("target_type")), FString(TEXT("static_mesh_asset")));
			TestEqual(TEXT("Modeling mesh spatial info asset path"), (*Target)->GetStringField(TEXT("asset_path")), FString(TEXT("/Engine/BasicShapes/Cube")));
		}
		if (Frame && Frame->IsValid())
		{
			TestEqual(TEXT("Modeling mesh spatial info bounds space"), (*Frame)->GetStringField(TEXT("bounds_space")), FString(TEXT("asset_local")));
			TestEqual(TEXT("Modeling mesh spatial info bounds source"), (*Frame)->GetStringField(TEXT("bounds_source")), FString(TEXT("visual_mesh")));
			TestEqual(TEXT("Modeling mesh spatial info unit"), (*Frame)->GetStringField(TEXT("unit")), FString(TEXT("cm")));
		}
		if (Aabb && Aabb->IsValid())
		{
			const TSharedPtr<FJsonObject>* Min = nullptr;
			const TSharedPtr<FJsonObject>* Max = nullptr;
			TestTrue(TEXT("Modeling mesh spatial info has aabb min"), (*Aabb)->TryGetObjectField(TEXT("min"), Min) && Min && Min->IsValid());
			TestTrue(TEXT("Modeling mesh spatial info has aabb max"), (*Aabb)->TryGetObjectField(TEXT("max"), Max) && Max && Max->IsValid());
			if (Min && Min->IsValid() && Max && Max->IsValid())
			{
				TestTrue(TEXT("Modeling mesh spatial info cube min x"), FMath::IsNearlyEqual((*Min)->GetNumberField(TEXT("x")), -50.0, 0.01));
				TestTrue(TEXT("Modeling mesh spatial info cube min y"), FMath::IsNearlyEqual((*Min)->GetNumberField(TEXT("y")), -50.0, 0.01));
				TestTrue(TEXT("Modeling mesh spatial info cube min z"), FMath::IsNearlyEqual((*Min)->GetNumberField(TEXT("z")), -50.0, 0.01));
				TestTrue(TEXT("Modeling mesh spatial info cube max x"), FMath::IsNearlyEqual((*Max)->GetNumberField(TEXT("x")), 50.0, 0.01));
				TestTrue(TEXT("Modeling mesh spatial info cube max y"), FMath::IsNearlyEqual((*Max)->GetNumberField(TEXT("y")), 50.0, 0.01));
				TestTrue(TEXT("Modeling mesh spatial info cube max z"), FMath::IsNearlyEqual((*Max)->GetNumberField(TEXT("z")), 50.0, 0.01));
			}
		}
		if (PivotInAabb && PivotInAabb->IsValid())
		{
			const TSharedPtr<FJsonObject>* Ratio = nullptr;
			TestTrue(TEXT("Modeling mesh spatial info has pivot ratio"), (*PivotInAabb)->TryGetObjectField(TEXT("ratio"), Ratio) && Ratio && Ratio->IsValid());
			if (Ratio && Ratio->IsValid())
			{
				TestTrue(TEXT("Modeling mesh spatial info pivot ratio x"), FMath::IsNearlyEqual((*Ratio)->GetNumberField(TEXT("x")), 0.5, 0.001));
				TestTrue(TEXT("Modeling mesh spatial info pivot ratio y"), FMath::IsNearlyEqual((*Ratio)->GetNumberField(TEXT("y")), 0.5, 0.001));
				TestTrue(TEXT("Modeling mesh spatial info pivot ratio z"), FMath::IsNearlyEqual((*Ratio)->GetNumberField(TEXT("z")), 0.5, 0.001));
			}
		}
	}

	const FString GetActiveToolBody = MakeExecRequestBody(TEXT("smoke-modeling-active-tool-001"), TEXT("modeling_get_active_tool"));
	FHttpSmokeResult GetActiveToolResult;
	TestTrue(TEXT("Modeling get_active_tool request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, GetActiveToolBody, GetActiveToolResult));
	TestEqual(TEXT("Modeling get_active_tool status code"), GetActiveToolResult.StatusCode, 200);

	TSharedPtr<FJsonObject> GetActiveToolJson;
	TestTrue(TEXT("Modeling get_active_tool response parses as JSON"), ParseJson(GetActiveToolResult.Body, GetActiveToolJson));
	TestTrue(TEXT("Modeling get_active_tool root ok=true"), GetActiveToolJson.IsValid() && GetActiveToolJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* GetActiveToolData = nullptr;
	TestTrue(TEXT("Modeling get_active_tool contains data object"), GetActiveToolJson.IsValid() && GetActiveToolJson->TryGetObjectField(TEXT("data"), GetActiveToolData) && GetActiveToolData && GetActiveToolData->IsValid());
	if (GetActiveToolData && GetActiveToolData->IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* PropertySets = nullptr;
		TestTrue(TEXT("Modeling get_active_tool contains has_active_tool"), (*GetActiveToolData)->HasTypedField<EJson::Boolean>(TEXT("has_active_tool")));
		TestTrue(TEXT("Modeling get_active_tool contains active_tool_name"), (*GetActiveToolData)->HasTypedField<EJson::String>(TEXT("active_tool_name")));
		TestTrue(TEXT("Modeling get_active_tool contains can_accept"), (*GetActiveToolData)->HasTypedField<EJson::Boolean>(TEXT("can_accept")));
		TestTrue(TEXT("Modeling get_active_tool contains can_cancel"), (*GetActiveToolData)->HasTypedField<EJson::Boolean>(TEXT("can_cancel")));
		TestTrue(TEXT("Modeling get_active_tool contains tool_has_accept"), (*GetActiveToolData)->HasTypedField<EJson::Boolean>(TEXT("tool_has_accept")));
		TestTrue(TEXT("Modeling get_active_tool contains property_sets array"), (*GetActiveToolData)->TryGetArrayField(TEXT("property_sets"), PropertySets) && PropertySets != nullptr);
	}

	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentInterfaceModelingCreateByBoundsSmokeTest,
	"GptProjectTest.UeAgentInterface.Smoke.ModelingCreateByBoundsWorkflow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUeAgentInterfaceModelingCreateByBoundsSmokeTest::RunTest(const FString& Parameters)
{
	FScopedUeAgentHttpServer ServerScope;
	FString InitError;
	if (!ServerScope.Initialize(InitError))
	{
		AddError(FString::Printf(TEXT("Initialize failed: %s"), *InitError));
		return false;
	}

	const FVector ExpectedCenter(1234.0f, -567.0f, 210.0f);
	const FVector ExpectedExtent(150.0f, 80.0f, 45.0f);
	const FRotator ExpectedRotation(0.0f, 35.0f, 0.0f);
	const FString FolderPath = FString::Printf(TEXT("Smoke/ModelingBounds/%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));

	auto VerifyCreatedPrimitiveByBounds =
		[this, &ServerScope, &FolderPath](
			const FString& RequestId,
			const FString& Command,
			const FVector& ExpectedCenterLocal,
			const FVector& ExpectedExtentLocal,
			const FRotator& ExpectedRotationLocal,
			const FString& ExpectedCollisionModeLocal,
			const FString& ExpectedCollisionSourceLocal,
			int32 ExpectedMinCollisionBoxesLocal,
			int32 ExpectedMinCollisionShapesLocal,
			bool bExpectPrimitiveCollisionShapeLocal) -> bool
	{
		const FString ExpectedActorLabelLocal = RequestId;
		const FString CreateBodyLocal = MakeExecRequestBody(
			RequestId,
			Command,
			FString::Printf(
				TEXT("{\"bounds\":{\"center\":{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f},\"extent\":{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f}},\"rotation\":{\"pitch\":%.3f,\"yaw\":%.3f,\"roll\":%.3f},\"folder_path\":\"%s\",\"label\":\"%s\"}"),
				ExpectedCenterLocal.X,
				ExpectedCenterLocal.Y,
				ExpectedCenterLocal.Z,
				ExpectedExtentLocal.X,
				ExpectedExtentLocal.Y,
				ExpectedExtentLocal.Z,
				ExpectedRotationLocal.Pitch,
				ExpectedRotationLocal.Yaw,
				ExpectedRotationLocal.Roll,
				*FolderPath,
				*ExpectedActorLabelLocal));

		FHttpSmokeResult CreateResultLocal;
		TestTrue(FString::Printf(TEXT("%s request succeeded"), *Command), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, CreateBodyLocal, CreateResultLocal));
		TestEqual(FString::Printf(TEXT("%s status code"), *Command), CreateResultLocal.StatusCode, 200);

		TSharedPtr<FJsonObject> CreateJsonLocal;
		TestTrue(FString::Printf(TEXT("%s response parses as JSON"), *Command), ParseJson(CreateResultLocal.Body, CreateJsonLocal));
		TestTrue(FString::Printf(TEXT("%s root ok=true"), *Command), CreateJsonLocal.IsValid() && CreateJsonLocal->GetBoolField(TEXT("ok")));

		const TSharedPtr<FJsonObject>* CreateDataLocal = nullptr;
		TestTrue(FString::Printf(TEXT("%s contains data object"), *Command), CreateJsonLocal.IsValid() && CreateJsonLocal->TryGetObjectField(TEXT("data"), CreateDataLocal) && CreateDataLocal && CreateDataLocal->IsValid());
		if (!(CreateDataLocal && CreateDataLocal->IsValid()))
		{
			return false;
		}

		FString CreatedActorNameLocal;
		const TSharedPtr<FJsonObject>* CreatedActorLocal = nullptr;
		TestTrue(FString::Printf(TEXT("%s reports used_bounds_placement"), *Command), (*CreateDataLocal)->GetBoolField(TEXT("used_bounds_placement")));
		TestTrue(FString::Printf(TEXT("%s reports default dynamic output applied"), *Command), (*CreateDataLocal)->GetBoolField(TEXT("default_output_type_applied")));
		TestEqual(FString::Printf(TEXT("%s default output type"), *Command), (*CreateDataLocal)->GetStringField(TEXT("default_output_type")), FString(TEXT("Dynamic Mesh")));
		TestTrue(FString::Printf(TEXT("%s contains created_actor"), *Command), (*CreateDataLocal)->TryGetObjectField(TEXT("created_actor"), CreatedActorLocal) && CreatedActorLocal && CreatedActorLocal->IsValid());
		if (CreatedActorLocal && CreatedActorLocal->IsValid())
		{
			CreatedActorNameLocal = (*CreatedActorLocal)->GetStringField(TEXT("name"));
			const FString CreatedActorClassLocal = (*CreatedActorLocal)->GetStringField(TEXT("class"));
			TestFalse(FString::Printf(TEXT("%s created_actor name not empty"), *Command), CreatedActorNameLocal.IsEmpty());
			TestEqual(FString::Printf(TEXT("%s created_actor label matches"), *Command), (*CreatedActorLocal)->GetStringField(TEXT("label")), ExpectedActorLabelLocal);
			TestEqual(FString::Printf(TEXT("%s created_actor class dynamic mesh"), *Command), CreatedActorClassLocal, FString(TEXT("/Script/GeometryFramework.DynamicMeshActor")));
		}

		const TSharedPtr<FJsonObject>* DerivedPrimitiveLocal = nullptr;
		TestTrue(FString::Printf(TEXT("%s contains derived_primitive"), *Command), (*CreateDataLocal)->TryGetObjectField(TEXT("derived_primitive"), DerivedPrimitiveLocal) && DerivedPrimitiveLocal && DerivedPrimitiveLocal->IsValid());
		if (DerivedPrimitiveLocal && DerivedPrimitiveLocal->IsValid())
		{
			if (Command == TEXT("modeling_create_stairs"))
			{
				TestTrue(TEXT("modeling_create_stairs derived has StepWidth"), (*DerivedPrimitiveLocal)->HasTypedField<EJson::Number>(TEXT("StepWidth")));
				TestTrue(TEXT("modeling_create_stairs derived has StepHeight"), (*DerivedPrimitiveLocal)->HasTypedField<EJson::Number>(TEXT("StepHeight")));
				TestTrue(TEXT("modeling_create_stairs derived has StepDepth"), (*DerivedPrimitiveLocal)->HasTypedField<EJson::Number>(TEXT("StepDepth")));
				TestTrue(TEXT("modeling_create_stairs derived has NumSteps"), (*DerivedPrimitiveLocal)->HasTypedField<EJson::Number>(TEXT("NumSteps")));
				TestTrue(TEXT("modeling_create_stairs derived step count is valid"), static_cast<int32>((*DerivedPrimitiveLocal)->GetNumberField(TEXT("NumSteps"))) >= 2);
			}
			else
			{
				TestEqual(FString::Printf(TEXT("%s derived depth"), *Command), (*DerivedPrimitiveLocal)->GetNumberField(TEXT("Depth")), static_cast<double>(ExpectedExtentLocal.X * 2.0f));
				TestEqual(FString::Printf(TEXT("%s derived width"), *Command), (*DerivedPrimitiveLocal)->GetNumberField(TEXT("Width")), static_cast<double>(ExpectedExtentLocal.Y * 2.0f));
				TestEqual(FString::Printf(TEXT("%s derived height"), *Command), (*DerivedPrimitiveLocal)->GetNumberField(TEXT("Height")), static_cast<double>(ExpectedExtentLocal.Z * 2.0f));
			}
		}

		const TSharedPtr<FJsonObject>* CollisionLocal = nullptr;
		TestTrue(FString::Printf(TEXT("%s contains collision readback"), *Command), (*CreateDataLocal)->TryGetObjectField(TEXT("collision"), CollisionLocal) && CollisionLocal && CollisionLocal->IsValid());
		if (CollisionLocal && CollisionLocal->IsValid())
		{
			TestTrue(FString::Printf(TEXT("%s collision generated"), *Command), (*CollisionLocal)->GetBoolField(TEXT("generated")));
			TestEqual(FString::Printf(TEXT("%s collision source"), *Command), (*CollisionLocal)->GetStringField(TEXT("source")), ExpectedCollisionSourceLocal);
			TestEqual(FString::Printf(TEXT("%s collision mode"), *Command), (*CollisionLocal)->GetStringField(TEXT("mode")), ExpectedCollisionModeLocal);
			TestTrue(FString::Printf(TEXT("%s collision has expected boxes"), *Command), static_cast<int32>((*CollisionLocal)->GetNumberField(TEXT("box_count"))) >= ExpectedMinCollisionBoxesLocal);
			TestTrue(FString::Printf(TEXT("%s collision has expected shapes"), *Command), static_cast<int32>((*CollisionLocal)->GetNumberField(TEXT("shape_count"))) >= ExpectedMinCollisionShapesLocal);
		}

		if (CreatedActorNameLocal.IsEmpty())
		{
			return false;
		}

		if (bExpectPrimitiveCollisionShapeLocal)
		{
			const FVector QueryMinLocal = ExpectedCenterLocal - ExpectedExtentLocal - FVector(80.0f, 80.0f, 80.0f);
			const FVector QueryMaxLocal = ExpectedCenterLocal + ExpectedExtentLocal + FVector(80.0f, 80.0f, 80.0f);
			const FString MeshShapesBodyLocal = MakeExecRequestBody(
				RequestId + TEXT("-mesh-shapes"),
				TEXT("level_get_nearby_mesh_shapes"),
				FString::Printf(
					TEXT("{\"query\":{\"type\":\"aabb\",\"min\":{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f},\"max\":{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f}},\"filters\":{\"types\":[\"primitive_collision\"]},\"max_shapes_per_actor\":64}"),
					QueryMinLocal.X,
					QueryMinLocal.Y,
					QueryMinLocal.Z,
					QueryMaxLocal.X,
					QueryMaxLocal.Y,
					QueryMaxLocal.Z));
			FHttpSmokeResult MeshShapesResultLocal;
			TestTrue(FString::Printf(TEXT("%s mesh_shapes request succeeded"), *Command), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, MeshShapesBodyLocal, MeshShapesResultLocal));
			TestEqual(FString::Printf(TEXT("%s mesh_shapes status code"), *Command), MeshShapesResultLocal.StatusCode, 200);

			TSharedPtr<FJsonObject> MeshShapesJsonLocal;
			TestTrue(FString::Printf(TEXT("%s mesh_shapes parses as JSON"), *Command), ParseJson(MeshShapesResultLocal.Body, MeshShapesJsonLocal));
			TestTrue(FString::Printf(TEXT("%s mesh_shapes root ok=true"), *Command), MeshShapesJsonLocal.IsValid() && MeshShapesJsonLocal->GetBoolField(TEXT("ok")));

			const TSharedPtr<FJsonObject>* MeshShapesDataLocal = nullptr;
			TestTrue(FString::Printf(TEXT("%s mesh_shapes contains data"), *Command), MeshShapesJsonLocal.IsValid() && MeshShapesJsonLocal->TryGetObjectField(TEXT("data"), MeshShapesDataLocal) && MeshShapesDataLocal && MeshShapesDataLocal->IsValid());
			if (MeshShapesDataLocal && MeshShapesDataLocal->IsValid())
			{
				const TArray<TSharedPtr<FJsonValue>>* ActorsLocal = nullptr;
				TestTrue(FString::Printf(TEXT("%s mesh_shapes actors exists"), *Command), (*MeshShapesDataLocal)->TryGetArrayField(TEXT("actors"), ActorsLocal) && ActorsLocal);

				bool bFoundPrimitiveCollisionShape = false;
				if (ActorsLocal)
				{
					for (const TSharedPtr<FJsonValue>& ActorValue : *ActorsLocal)
					{
						const TSharedPtr<FJsonObject> ActorObj = ActorValue.IsValid() ? ActorValue->AsObject() : nullptr;
						const TArray<TSharedPtr<FJsonValue>>* Components = nullptr;
						if (!ActorObj.IsValid() || ActorObj->GetStringField(TEXT("name")) != ExpectedActorLabelLocal || !ActorObj->TryGetArrayField(TEXT("components"), Components) || !Components)
						{
							continue;
						}

						for (const TSharedPtr<FJsonValue>& ComponentValue : *Components)
						{
							const TSharedPtr<FJsonObject> ComponentObj = ComponentValue.IsValid() ? ComponentValue->AsObject() : nullptr;
							const TArray<TSharedPtr<FJsonValue>>* Shapes = nullptr;
							if (!ComponentObj.IsValid() || !ComponentObj->TryGetArrayField(TEXT("shapes"), Shapes) || !Shapes)
							{
								continue;
							}

							for (const TSharedPtr<FJsonValue>& ShapeValue : *Shapes)
							{
								const TSharedPtr<FJsonObject> ShapeObj = ShapeValue.IsValid() ? ShapeValue->AsObject() : nullptr;
								if (ShapeObj.IsValid() && ShapeObj->GetStringField(TEXT("src")) == TEXT("collision_box/block"))
								{
									bFoundPrimitiveCollisionShape = true;
								}
							}
						}
					}
				}
				TestTrue(FString::Printf(TEXT("%s mesh_shapes sees primitive collision"), *Command), bFoundPrimitiveCollisionShape);
			}
		}

		const FString GetTransformBodyLocal = MakeExecRequestBody(
			RequestId + TEXT("-transform"),
			TEXT("level_get_actor_transform"),
			FString::Printf(TEXT("{\"id\":\"%s\"}"), *CreatedActorNameLocal));
		FHttpSmokeResult GetTransformResultLocal;
		TestTrue(FString::Printf(TEXT("%s get_transform request succeeded"), *Command), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, GetTransformBodyLocal, GetTransformResultLocal));
		TestEqual(FString::Printf(TEXT("%s get_transform status code"), *Command), GetTransformResultLocal.StatusCode, 200);

		TSharedPtr<FJsonObject> GetTransformJsonLocal;
		TestTrue(FString::Printf(TEXT("%s get_transform parses as JSON"), *Command), ParseJson(GetTransformResultLocal.Body, GetTransformJsonLocal));
		TestTrue(FString::Printf(TEXT("%s get_transform root ok=true"), *Command), GetTransformJsonLocal.IsValid() && GetTransformJsonLocal->GetBoolField(TEXT("ok")));

		const TSharedPtr<FJsonObject>* TransformDataLocal = nullptr;
		TestTrue(FString::Printf(TEXT("%s get_transform contains data object"), *Command), GetTransformJsonLocal.IsValid() && GetTransformJsonLocal->TryGetObjectField(TEXT("data"), TransformDataLocal) && TransformDataLocal && TransformDataLocal->IsValid());
		if (TransformDataLocal && TransformDataLocal->IsValid())
		{
			const TSharedPtr<FJsonObject>* LocationLocal = nullptr;
			const TSharedPtr<FJsonObject>* RotationLocal = nullptr;
			TestTrue(FString::Printf(TEXT("%s get_transform contains location"), *Command), (*TransformDataLocal)->TryGetObjectField(TEXT("location"), LocationLocal) && LocationLocal && LocationLocal->IsValid());
			TestTrue(FString::Printf(TEXT("%s get_transform contains rotation"), *Command), (*TransformDataLocal)->TryGetObjectField(TEXT("rotation"), RotationLocal) && RotationLocal && RotationLocal->IsValid());
			if (LocationLocal && LocationLocal->IsValid())
			{
				TestTrue(FString::Printf(TEXT("%s location x matches"), *Command), FMath::IsNearlyEqual((float)(*LocationLocal)->GetNumberField(TEXT("x")), ExpectedCenterLocal.X, 0.5f));
				TestTrue(FString::Printf(TEXT("%s location y matches"), *Command), FMath::IsNearlyEqual((float)(*LocationLocal)->GetNumberField(TEXT("y")), ExpectedCenterLocal.Y, 0.5f));
				TestTrue(FString::Printf(TEXT("%s location z matches"), *Command), FMath::IsNearlyEqual((float)(*LocationLocal)->GetNumberField(TEXT("z")), ExpectedCenterLocal.Z, 0.5f));
			}
			if (RotationLocal && RotationLocal->IsValid())
			{
				TestTrue(FString::Printf(TEXT("%s rotation pitch matches"), *Command), FMath::IsNearlyEqual((float)(*RotationLocal)->GetNumberField(TEXT("pitch")), ExpectedRotationLocal.Pitch, 0.5f));
				TestTrue(FString::Printf(TEXT("%s rotation yaw matches"), *Command), FMath::IsNearlyEqual((float)(*RotationLocal)->GetNumberField(TEXT("yaw")), ExpectedRotationLocal.Yaw, 0.5f));
				TestTrue(FString::Printf(TEXT("%s rotation roll matches"), *Command), FMath::IsNearlyEqual((float)(*RotationLocal)->GetNumberField(TEXT("roll")), ExpectedRotationLocal.Roll, 0.5f));
			}
		}

		if (ExpectedCollisionModeLocal == TEXT("complex_mesh"))
		{
			const float SweepDistance = FMath::Max(ExpectedExtentLocal.Y + 120.0f, 220.0f);
			const FVector SweepStartLocal = ExpectedCenterLocal + FVector(0.0f, -SweepDistance, 0.0f);
			const FVector SweepEndLocal = ExpectedCenterLocal + FVector(0.0f, SweepDistance, 0.0f);
			const FString SweepBodyLocal = MakeExecRequestBody(
				RequestId + TEXT("-complex-as-simple-sweep"),
				TEXT("level_sweep_capsule"),
				FString::Printf(
					TEXT("{\"start\":{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f},\"end\":{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f},\"radius_cm\":34.0,\"half_height_cm\":88.0,\"trace_channel\":\"Pawn\",\"trace_complex\":false,\"find_initial_overlaps\":true}"),
					SweepStartLocal.X,
					SweepStartLocal.Y,
					SweepStartLocal.Z,
					SweepEndLocal.X,
					SweepEndLocal.Y,
					SweepEndLocal.Z));
			FHttpSmokeResult SweepResultLocal;
			TestTrue(FString::Printf(TEXT("%s complex-as-simple sweep request succeeded"), *Command), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SweepBodyLocal, SweepResultLocal));
			TestEqual(FString::Printf(TEXT("%s complex-as-simple sweep status code"), *Command), SweepResultLocal.StatusCode, 200);

			TSharedPtr<FJsonObject> SweepJsonLocal;
			TestTrue(FString::Printf(TEXT("%s complex-as-simple sweep parses JSON"), *Command), ParseJson(SweepResultLocal.Body, SweepJsonLocal));
			TestTrue(FString::Printf(TEXT("%s complex-as-simple sweep root ok=true"), *Command), SweepJsonLocal.IsValid() && SweepJsonLocal->GetBoolField(TEXT("ok")));

			const TSharedPtr<FJsonObject>* SweepDataLocal = nullptr;
			TestTrue(FString::Printf(TEXT("%s complex-as-simple sweep contains data object"), *Command), SweepJsonLocal.IsValid() && SweepJsonLocal->TryGetObjectField(TEXT("data"), SweepDataLocal) && SweepDataLocal && SweepDataLocal->IsValid());
			if (SweepDataLocal && SweepDataLocal->IsValid())
			{
				TestTrue(FString::Printf(TEXT("%s complex-as-simple blocks Pawn sweep without trace_complex"), *Command), (*SweepDataLocal)->GetBoolField(TEXT("blocking_hit")));
				TestEqual(FString::Printf(TEXT("%s complex-as-simple sweep actor_name matches"), *Command), (*SweepDataLocal)->GetStringField(TEXT("actor_name")), CreatedActorNameLocal);
			}
		}

		return !HasAnyErrors();
	};
	VerifyCreatedPrimitiveByBounds(TEXT("smoke-modeling-create-box-bounds-001"), TEXT("modeling_create_box"), ExpectedCenter, ExpectedExtent, ExpectedRotation, TEXT("bounds_box"), TEXT("actor_box_components"), 1, 1, true);
	VerifyCreatedPrimitiveByBounds(TEXT("smoke-modeling-create-ramp-bounds-001"), TEXT("modeling_create_ramp"), ExpectedCenter + FVector(400.0f, 0.0f, 0.0f), FVector(120.0f, 90.0f, 60.0f), FRotator(0.0f, 90.0f, 0.0f), TEXT("complex_mesh"), TEXT("dynamic_mesh_component"), 0, 1, false);
	VerifyCreatedPrimitiveByBounds(TEXT("smoke-modeling-create-ramp-corner-bounds-001"), TEXT("modeling_create_ramp_corner"), ExpectedCenter + FVector(-400.0f, 0.0f, 0.0f), FVector(100.0f, 100.0f, 80.0f), FRotator(0.0f, -25.0f, 0.0f), TEXT("complex_mesh"), TEXT("dynamic_mesh_component"), 0, 1, false);
	VerifyCreatedPrimitiveByBounds(TEXT("smoke-modeling-create-stairs-bounds-001"), TEXT("modeling_create_stairs"), ExpectedCenter + FVector(0.0f, 420.0f, 120.0f), FVector(220.0f, 80.0f, 120.0f), FRotator::ZeroRotator, TEXT("stairs_step_boxes"), TEXT("actor_box_components"), 2, 2, true);

	const FString VisualOnlyLabel = TEXT("smoke-modeling-create-visual-none-001");
	const FVector VisualOnlyCenter = ExpectedCenter + FVector(0.0f, -420.0f, 0.0f);
	const FString VisualOnlyBody = MakeExecRequestBody(
		VisualOnlyLabel,
		TEXT("modeling_create_box"),
		FString::Printf(
			TEXT("{\"bounds\":{\"center\":{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f},\"extent\":{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f}},\"folder_path\":\"%s\",\"label\":\"%s\",\"collision\":{\"mode\":\"none\"}}"),
			VisualOnlyCenter.X,
			VisualOnlyCenter.Y,
			VisualOnlyCenter.Z,
			ExpectedExtent.X,
			ExpectedExtent.Y,
			ExpectedExtent.Z,
			*FolderPath,
			*VisualOnlyLabel));
	FHttpSmokeResult VisualOnlyResult;
	TestTrue(TEXT("modeling_create_box visual-only request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, VisualOnlyBody, VisualOnlyResult));
	TestEqual(TEXT("modeling_create_box visual-only status code"), VisualOnlyResult.StatusCode, 200);

	TSharedPtr<FJsonObject> VisualOnlyJson;
	TestTrue(TEXT("modeling_create_box visual-only parses as JSON"), ParseJson(VisualOnlyResult.Body, VisualOnlyJson));
	TestTrue(TEXT("modeling_create_box visual-only root ok=true"), VisualOnlyJson.IsValid() && VisualOnlyJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* VisualOnlyData = nullptr;
	TestTrue(TEXT("modeling_create_box visual-only contains data"), VisualOnlyJson.IsValid() && VisualOnlyJson->TryGetObjectField(TEXT("data"), VisualOnlyData) && VisualOnlyData && VisualOnlyData->IsValid());
	if (VisualOnlyData && VisualOnlyData->IsValid())
	{
		const TSharedPtr<FJsonObject>* Collision = nullptr;
		TestTrue(TEXT("modeling_create_box visual-only collision readback exists"), (*VisualOnlyData)->TryGetObjectField(TEXT("collision"), Collision) && Collision && Collision->IsValid());
		if (Collision && Collision->IsValid())
		{
			TestFalse(TEXT("modeling_create_box visual-only collision not generated"), (*Collision)->GetBoolField(TEXT("generated")));
			TestTrue(TEXT("modeling_create_box visual-only collision skipped"), (*Collision)->GetBoolField(TEXT("skipped")));
			TestEqual(TEXT("modeling_create_box visual-only skip reason"), (*Collision)->GetStringField(TEXT("skip_reason")), FString(TEXT("collision_disabled")));
			TestTrue(TEXT("modeling_create_box visual-only disables dynamic mesh collision"), static_cast<int32>((*Collision)->GetNumberField(TEXT("disabled_dynamic_mesh_collision_components"))) > 0);
		}
	}

	const FVector VisualQueryMin = VisualOnlyCenter - ExpectedExtent - FVector(80.0f, 80.0f, 80.0f);
	const FVector VisualQueryMax = VisualOnlyCenter + ExpectedExtent + FVector(80.0f, 80.0f, 80.0f);
	const FString VisualMeshShapesBody = MakeExecRequestBody(
		TEXT("smoke-modeling-create-visual-none-mesh-shapes-001"),
		TEXT("level_get_nearby_mesh_shapes"),
		FString::Printf(
			TEXT("{\"query\":{\"type\":\"aabb\",\"min\":{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f},\"max\":{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f}},\"filters\":{\"types\":[\"primitive_collision\"]},\"max_shapes_per_actor\":16}"),
			VisualQueryMin.X,
			VisualQueryMin.Y,
			VisualQueryMin.Z,
			VisualQueryMax.X,
			VisualQueryMax.Y,
			VisualQueryMax.Z));
	FHttpSmokeResult VisualMeshShapesResult;
	TestTrue(TEXT("modeling_create_box visual-only mesh_shapes request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, VisualMeshShapesBody, VisualMeshShapesResult));
	TestEqual(TEXT("modeling_create_box visual-only mesh_shapes status code"), VisualMeshShapesResult.StatusCode, 200);

	TSharedPtr<FJsonObject> VisualMeshShapesJson;
	TestTrue(TEXT("modeling_create_box visual-only mesh_shapes parses JSON"), ParseJson(VisualMeshShapesResult.Body, VisualMeshShapesJson));
	TestTrue(TEXT("modeling_create_box visual-only mesh_shapes root ok=true"), VisualMeshShapesJson.IsValid() && VisualMeshShapesJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* VisualMeshShapesData = nullptr;
	TestTrue(TEXT("modeling_create_box visual-only mesh_shapes contains data"), VisualMeshShapesJson.IsValid() && VisualMeshShapesJson->TryGetObjectField(TEXT("data"), VisualMeshShapesData) && VisualMeshShapesData && VisualMeshShapesData->IsValid());
	if (VisualMeshShapesData && VisualMeshShapesData->IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Actors = nullptr;
		TestTrue(TEXT("modeling_create_box visual-only mesh_shapes actors exists"), (*VisualMeshShapesData)->TryGetArrayField(TEXT("actors"), Actors) && Actors);
		bool bFoundVisualOnlyCollision = false;
		if (Actors)
		{
			for (const TSharedPtr<FJsonValue>& ActorValue : *Actors)
			{
				const TSharedPtr<FJsonObject> ActorObj = ActorValue.IsValid() ? ActorValue->AsObject() : nullptr;
				if (ActorObj.IsValid() && ActorObj->GetStringField(TEXT("name")) == VisualOnlyLabel)
				{
					bFoundVisualOnlyCollision = true;
				}
			}
		}
		TestFalse(TEXT("modeling_create_box visual-only has no primitive collision shapes"), bFoundVisualOnlyCollision);
	}

	const FVector VisualTraceStart = VisualOnlyCenter + FVector(-ExpectedExtent.X - 120.0f, 0.0f, 0.0f);
	const FString VisualTraceBody = MakeExecRequestBody(
		TEXT("smoke-modeling-create-visual-none-trace-001"),
		TEXT("level_trace_world_ray"),
		FString::Printf(
			TEXT("{\"start\":{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f},\"direction\":{\"x\":1.0,\"y\":0.0,\"z\":0.0},\"trace_distance\":%.3f,\"trace_channel\":\"Visibility\",\"trace_complex\":true}"),
			VisualTraceStart.X,
			VisualTraceStart.Y,
			VisualTraceStart.Z,
			ExpectedExtent.X * 2.0f + 240.0f));
	FHttpSmokeResult VisualTraceResult;
	TestTrue(TEXT("modeling_create_box visual-only complex trace request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, VisualTraceBody, VisualTraceResult));
	TestEqual(TEXT("modeling_create_box visual-only complex trace status code"), VisualTraceResult.StatusCode, 200);

	TSharedPtr<FJsonObject> VisualTraceJson;
	TestTrue(TEXT("modeling_create_box visual-only complex trace parses JSON"), ParseJson(VisualTraceResult.Body, VisualTraceJson));
	TestTrue(TEXT("modeling_create_box visual-only complex trace root ok=true"), VisualTraceJson.IsValid() && VisualTraceJson->GetBoolField(TEXT("ok")));
	const TSharedPtr<FJsonObject>* VisualTraceData = nullptr;
	TestTrue(TEXT("modeling_create_box visual-only complex trace contains data"), VisualTraceJson.IsValid() && VisualTraceJson->TryGetObjectField(TEXT("data"), VisualTraceData) && VisualTraceData && VisualTraceData->IsValid());
	if (VisualTraceData && VisualTraceData->IsValid())
	{
		TestFalse(TEXT("modeling_create_box visual-only complex trace does not hit disabled visual mesh"), (*VisualTraceData)->GetBoolField(TEXT("hit")));
	}

	const FString DestroyBody = MakeExecRequestBody(
		TEXT("smoke-modeling-create-bounds-destroy-001"),
		TEXT("level_destroy_folder_actors"),
		FString::Printf(TEXT("{\"folder_path\":\"%s\"}"), *FolderPath));
	FHttpSmokeResult DestroyResult;
	TestTrue(TEXT("Modeling bounds workflow destroy request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, DestroyBody, DestroyResult));
	TestEqual(TEXT("Modeling bounds workflow destroy status code"), DestroyResult.StatusCode, 200);

	TSharedPtr<FJsonObject> DestroyJson;
	TestTrue(TEXT("Modeling bounds workflow destroy parses as JSON"), ParseJson(DestroyResult.Body, DestroyJson));
	TestTrue(TEXT("Modeling bounds workflow destroy root ok=true"), DestroyJson.IsValid() && DestroyJson->GetBoolField(TEXT("ok")));

	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentInterfaceModelingVoxelGridSmokeTest,
	"GptProjectTest.UeAgentInterface.Smoke.ModelingVoxelGridWorkflow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUeAgentInterfaceModelingVoxelGridSmokeTest::RunTest(const FString& Parameters)
{
	FScopedUeAgentHttpServer ServerScope;
	FString InitError;
	if (!ServerScope.Initialize(InitError))
	{
		AddError(FString::Printf(TEXT("Initialize failed: %s"), *InitError));
		return false;
	}

	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);
	const FString ActorLabel = FString::Printf(TEXT("SmokeVoxelGrid_%s"), *Suffix);
	const FString FolderPath = FString::Printf(TEXT("Smoke/ModelingVoxelGrid/%s"), *Suffix);
	FString SessionId;

	auto ExecJson = [this, &ServerScope](const FString& RequestId, const FString& Command, const FString& ParamsJson, TSharedPtr<FJsonObject>& OutJson) -> bool
	{
		const FString Body = MakeExecRequestBody(RequestId, Command, ParamsJson);
		FHttpSmokeResult Result;
		TestTrue(FString::Printf(TEXT("%s request succeeded"), *Command), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, Body, Result));
		TestEqual(FString::Printf(TEXT("%s status code"), *Command), Result.StatusCode, 200);
		TestTrue(FString::Printf(TEXT("%s response parses as JSON"), *Command), ParseJson(Result.Body, OutJson));
		TestTrue(FString::Printf(TEXT("%s root ok=true"), *Command), OutJson.IsValid() && OutJson->GetBoolField(TEXT("ok")));
		return OutJson.IsValid() && OutJson->GetBoolField(TEXT("ok"));
	};

	auto GetData = [this](const FString& Label, const TSharedPtr<FJsonObject>& Json) -> const TSharedPtr<FJsonObject>*
	{
		const TSharedPtr<FJsonObject>* Data = nullptr;
		TestTrue(FString::Printf(TEXT("%s contains data object"), *Label), Json.IsValid() && Json->TryGetObjectField(TEXT("data"), Data) && Data && Data->IsValid());
		return Data;
	};

	TSharedPtr<FJsonObject> BeginJson;
	TestTrue(
		TEXT("Voxel grid begin executes"),
		ExecJson(
			TEXT("smoke-modeling-voxel-begin-001"),
			TEXT("modeling_voxel_grid_begin"),
			FString::Printf(
				TEXT("{\"mode\":\"create\",\"label\":\"%s\",\"folder_path\":\"%s\",\"grid\":{\"cell_size\":100,\"origin\":{\"x\":0,\"y\":0,\"z\":0},\"rotation\":{\"pitch\":0,\"yaw\":0,\"roll\":0}}}"),
				*ActorLabel,
				*FolderPath),
			BeginJson));

	const TSharedPtr<FJsonObject>* BeginData = GetData(TEXT("Voxel grid begin"), BeginJson);
	if (BeginData && BeginData->IsValid())
	{
		SessionId = (*BeginData)->GetStringField(TEXT("session_id"));
		TestFalse(TEXT("Voxel grid session id not empty"), SessionId.IsEmpty());
		TestEqual(TEXT("Voxel grid output type is dynamic mesh actor"), (*BeginData)->GetStringField(TEXT("output_type")), FString(TEXT("dynamic_mesh_actor")));
		TestEqual(TEXT("Voxel grid begin cell size"), (*BeginData)->GetNumberField(TEXT("cell_size")), 100.0);
	}

	if (SessionId.IsEmpty())
	{
		return false;
	}

	TSharedPtr<FJsonObject> AddStripJson;
	TestTrue(
		TEXT("Voxel grid add horizontal strip executes"),
		ExecJson(
			TEXT("smoke-modeling-voxel-add-strip-001"),
			TEXT("modeling_voxel_grid_add_bounds"),
			FString::Printf(TEXT("{\"session_id\":\"%s\",\"expected_revision\":0,\"bounds\":{\"space\":\"grid\",\"min\":{\"x\":0,\"y\":0,\"z\":0},\"max\":{\"x\":300,\"y\":100,\"z\":100}}}"), *SessionId),
			AddStripJson));
	const TSharedPtr<FJsonObject>* AddStripData = GetData(TEXT("Voxel grid add strip"), AddStripJson);
	if (AddStripData && AddStripData->IsValid())
	{
		TestEqual(TEXT("Voxel grid add strip changed 3 cells"), (*AddStripData)->GetNumberField(TEXT("changed_cell_count")), 3.0);
		TestEqual(TEXT("Voxel grid revision after strip"), (*AddStripData)->GetNumberField(TEXT("revision")), 1.0);
	}

	TSharedPtr<FJsonObject> AddLegJson;
	TestTrue(
		TEXT("Voxel grid add vertical leg executes"),
		ExecJson(
			TEXT("smoke-modeling-voxel-add-leg-001"),
			TEXT("modeling_voxel_grid_add_bounds"),
			FString::Printf(TEXT("{\"session_id\":\"%s\",\"expected_revision\":1,\"bounds\":{\"space\":\"grid\",\"min\":{\"x\":0,\"y\":100,\"z\":0},\"max\":{\"x\":100,\"y\":300,\"z\":100}}}"), *SessionId),
			AddLegJson));
	const TSharedPtr<FJsonObject>* AddLegData = GetData(TEXT("Voxel grid add leg"), AddLegJson);
	if (AddLegData && AddLegData->IsValid())
	{
		TestEqual(TEXT("Voxel grid add leg changed 2 cells"), (*AddLegData)->GetNumberField(TEXT("changed_cell_count")), 2.0);
		TestEqual(TEXT("Voxel grid occupied after add"), (*AddLegData)->GetNumberField(TEXT("occupied_cell_count")), 5.0);
		TestEqual(TEXT("Voxel grid revision after leg"), (*AddLegData)->GetNumberField(TEXT("revision")), 2.0);
	}

	TSharedPtr<FJsonObject> ApplyJson;
	TestTrue(
		TEXT("Voxel grid first apply executes"),
		ExecJson(
			TEXT("smoke-modeling-voxel-apply-001"),
			TEXT("modeling_voxel_grid_apply"),
			FString::Printf(TEXT("{\"session_id\":\"%s\",\"expected_revision\":2,\"keep_session\":true,\"collision\":{\"mode\":\"merged_box_components\",\"max_boxes\":16,\"profile_name\":\"BlockAll\"}}"), *SessionId),
			ApplyJson));
	const TSharedPtr<FJsonObject>* ApplyData = GetData(TEXT("Voxel grid first apply"), ApplyJson);
	if (ApplyData && ApplyData->IsValid())
	{
		const TSharedPtr<FJsonObject>* Mesh = nullptr;
		const TSharedPtr<FJsonObject>* Collision = nullptr;
		TestEqual(TEXT("Voxel grid first apply occupied cells"), (*ApplyData)->GetNumberField(TEXT("occupied_cell_count")), 5.0);
		TestEqual(TEXT("Voxel grid first apply output type"), (*ApplyData)->GetStringField(TEXT("output_type")), FString(TEXT("dynamic_mesh_actor")));
		TestTrue(TEXT("Voxel grid first apply keeps session"), !(*ApplyData)->GetBoolField(TEXT("session_closed")));
		TestTrue(TEXT("Voxel grid first apply has mesh"), (*ApplyData)->TryGetObjectField(TEXT("mesh"), Mesh) && Mesh && Mesh->IsValid());
		TestTrue(TEXT("Voxel grid first apply has collision"), (*ApplyData)->TryGetObjectField(TEXT("collision"), Collision) && Collision && Collision->IsValid());
		if (Mesh && Mesh->IsValid())
		{
			TestTrue(TEXT("Voxel grid first apply generated triangles"), (*Mesh)->GetNumberField(TEXT("triangle_count")) > 0.0);
			const TSharedPtr<FJsonObject>* Material = nullptr;
			TestTrue(TEXT("Voxel grid first apply has material readback"), (*Mesh)->TryGetObjectField(TEXT("material"), Material) && Material && Material->IsValid());
			if (Material && Material->IsValid())
			{
				TestTrue(TEXT("Voxel grid first apply leaves CubeGrid default material slot empty"), (*Material)->GetStringField(TEXT("path")).IsEmpty());
				TestEqual(TEXT("Voxel grid first apply reports CubeGrid component default material"), (*Material)->GetStringField(TEXT("source")), FString(TEXT("cubegrid_component_default")));
			}
		}
		if (Collision && Collision->IsValid())
		{
			TestEqual(TEXT("Voxel grid first apply collision boxes"), (*Collision)->GetNumberField(TEXT("box_count")), 2.0);
			TestEqual(TEXT("Voxel grid first apply covered cells"), (*Collision)->GetNumberField(TEXT("covered_cell_count")), 5.0);
			TestTrue(TEXT("Voxel grid first apply exact collision coverage"), (*Collision)->GetBoolField(TEXT("coverage_matches_occupied_cells")));
		}
	}

	UWorld* EditorWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	TestNotNull(TEXT("Voxel grid editor world exists"), EditorWorld);
	AActor* VoxelActor = nullptr;
	if (EditorWorld)
	{
		for (TActorIterator<AActor> It(EditorWorld); It; ++It)
		{
			if ((*It)->GetActorLabel() == ActorLabel)
			{
				VoxelActor = *It;
				break;
			}
		}
	}
	TestNotNull(TEXT("Voxel grid actor exists after apply"), VoxelActor);
	UDynamicMeshComponent* VoxelComponent = VoxelActor ? VoxelActor->FindComponentByClass<UDynamicMeshComponent>() : nullptr;
	TestNotNull(TEXT("Voxel grid dynamic mesh component exists after apply"), VoxelComponent);
	if (VoxelComponent)
	{
		TestEqual(TEXT("Voxel grid component has one CubeGrid default material slot"), VoxelComponent->BaseMaterials.Num(), 1);
		TestNull(TEXT("Voxel grid component CubeGrid default material slot is empty"), VoxelComponent->BaseMaterials.Num() > 0 ? VoxelComponent->BaseMaterials[0].Get() : nullptr);

		constexpr double CellSize = 100.0;
		TSet<FIntVector> OccupiedCells;
		OccupiedCells.Add(FIntVector(0, 0, 0));
		OccupiedCells.Add(FIntVector(1, 0, 0));
		OccupiedCells.Add(FIntVector(2, 0, 0));
		OccupiedCells.Add(FIntVector(0, 1, 0));
		OccupiedCells.Add(FIntVector(0, 2, 0));

		auto PointToCell = [CellSize](const FVector3d& Point) -> FIntVector
		{
			return FIntVector(
				FMath::FloorToInt(Point.X / CellSize),
				FMath::FloorToInt(Point.Y / CellSize),
				FMath::FloorToInt(Point.Z / CellSize));
		};

		const UDynamicMesh* DynamicMesh = VoxelComponent->GetDynamicMesh();
		TestNotNull(TEXT("Voxel grid UDynamicMesh exists after apply"), DynamicMesh);
		int32 CheckedTriangleCount = 0;
		bool bAllTrianglesHaveNormalOverlay = true;
		bool bAllNormalsPointOutward = true;
		bool bAllWindingMatchesVisibleFrontFace = true;
		bool bHasContinuousUVRange = false;
		if (DynamicMesh)
		{
			DynamicMesh->ProcessMesh([&](const UE::Geometry::FDynamicMesh3& Mesh)
			{
				const UE::Geometry::FDynamicMeshNormalOverlay* NormalOverlay = Mesh.HasAttributes() ? Mesh.Attributes()->PrimaryNormals() : nullptr;
				const UE::Geometry::FDynamicMeshUVOverlay* UVOverlay = Mesh.HasAttributes() ? Mesh.Attributes()->PrimaryUV() : nullptr;
				if (UVOverlay)
				{
					FVector2f MinUV(TNumericLimits<float>::Max(), TNumericLimits<float>::Max());
					FVector2f MaxUV(TNumericLimits<float>::Lowest(), TNumericLimits<float>::Lowest());
					for (const int32 ElementId : UVOverlay->ElementIndicesItr())
					{
						const FVector2f UV = UVOverlay->GetElement(ElementId);
						MinUV.X = FMath::Min(MinUV.X, UV.X);
						MinUV.Y = FMath::Min(MinUV.Y, UV.Y);
						MaxUV.X = FMath::Max(MaxUV.X, UV.X);
						MaxUV.Y = FMath::Max(MaxUV.Y, UV.Y);
					}
					bHasContinuousUVRange = (MaxUV.X - MinUV.X) > 2.5f || (MaxUV.Y - MinUV.Y) > 2.5f;
				}
				for (const int32 TriId : Mesh.TriangleIndicesItr())
				{
					const UE::Geometry::FIndex3i Triangle = Mesh.GetTriangle(TriId);
					const FVector3d A = Mesh.GetVertex(Triangle.A);
					const FVector3d B = Mesh.GetVertex(Triangle.B);
					const FVector3d C = Mesh.GetVertex(Triangle.C);
					FVector3d WindingNormal = (B - A).Cross(C - A);
					const double WindingNormalLength = WindingNormal.Length();
					if (WindingNormalLength <= UE_DOUBLE_SMALL_NUMBER)
					{
						continue;
					}
					WindingNormal /= WindingNormalLength;

					if (!NormalOverlay || !NormalOverlay->IsSetTriangle(TriId))
					{
						bAllTrianglesHaveNormalOverlay = false;
						continue;
					}

					const UE::Geometry::FIndex3i NormalTriangle = NormalOverlay->GetTriangle(TriId);
					const FVector3f N0 = NormalOverlay->GetElement(NormalTriangle.A);
					const FVector3f N1 = NormalOverlay->GetElement(NormalTriangle.B);
					const FVector3f N2 = NormalOverlay->GetElement(NormalTriangle.C);
					FVector3d OutwardNormal(
						static_cast<double>(N0.X + N1.X + N2.X) / 3.0,
						static_cast<double>(N0.Y + N1.Y + N2.Y) / 3.0,
						static_cast<double>(N0.Z + N1.Z + N2.Z) / 3.0);
					const double OutwardNormalLength = OutwardNormal.Length();
					if (OutwardNormalLength <= UE_DOUBLE_SMALL_NUMBER)
					{
						bAllNormalsPointOutward = false;
						continue;
					}
					OutwardNormal /= OutwardNormalLength;

					const FVector3d TriangleCenter = (A + B + C) / 3.0;
					const FIntVector InsideCell = PointToCell(TriangleCenter - OutwardNormal);
					const FIntVector OutsideCell = PointToCell(TriangleCenter + OutwardNormal);
					if (!OccupiedCells.Contains(InsideCell) || OccupiedCells.Contains(OutsideCell))
					{
						bAllNormalsPointOutward = false;
					}

					if (OutwardNormal.Dot(WindingNormal) > -0.9)
					{
						bAllWindingMatchesVisibleFrontFace = false;
					}

					++CheckedTriangleCount;
				}
			});
		}
		TestTrue(TEXT("Voxel grid checked generated triangles"), CheckedTriangleCount > 0);
		TestTrue(TEXT("Voxel grid triangles have normal overlay"), bAllTrianglesHaveNormalOverlay);
		TestTrue(TEXT("Voxel grid overlay normals point out of occupied cells"), bAllNormalsPointOutward);
		TestTrue(TEXT("Voxel grid winding matches visible front-face convention"), bAllWindingMatchesVisibleFrontFace);
		TestTrue(TEXT("Voxel grid UVs stay continuous across cell bounds"), bHasContinuousUVRange);
	}

	TSharedPtr<FJsonObject> RemoveJson;
	TestTrue(
		TEXT("Voxel grid remove bound executes"),
		ExecJson(
			TEXT("smoke-modeling-voxel-remove-001"),
			TEXT("modeling_voxel_grid_remove_bounds"),
			FString::Printf(TEXT("{\"session_id\":\"%s\",\"expected_revision\":2,\"bounds\":{\"space\":\"grid\",\"min\":{\"x\":0,\"y\":100,\"z\":0},\"max\":{\"x\":100,\"y\":200,\"z\":100}}}"), *SessionId),
			RemoveJson));
	const TSharedPtr<FJsonObject>* RemoveData = GetData(TEXT("Voxel grid remove"), RemoveJson);
	if (RemoveData && RemoveData->IsValid())
	{
		TestEqual(TEXT("Voxel grid remove changed 1 cell"), (*RemoveData)->GetNumberField(TEXT("changed_cell_count")), 1.0);
		TestEqual(TEXT("Voxel grid occupied after remove"), (*RemoveData)->GetNumberField(TEXT("occupied_cell_count")), 4.0);
		TestEqual(TEXT("Voxel grid revision after remove"), (*RemoveData)->GetNumberField(TEXT("revision")), 3.0);
	}

	TSharedPtr<FJsonObject> FinalApplyJson;
	TestTrue(
		TEXT("Voxel grid final apply executes"),
		ExecJson(
			TEXT("smoke-modeling-voxel-apply-final-001"),
			TEXT("modeling_voxel_grid_apply"),
			FString::Printf(TEXT("{\"session_id\":\"%s\",\"expected_revision\":3,\"collision\":{\"mode\":\"merged_box_components\",\"max_boxes\":16,\"profile_name\":\"BlockAll\"}}"), *SessionId),
			FinalApplyJson));
	const TSharedPtr<FJsonObject>* FinalApplyData = GetData(TEXT("Voxel grid final apply"), FinalApplyJson);
	if (FinalApplyData && FinalApplyData->IsValid())
	{
		const TSharedPtr<FJsonObject>* Collision = nullptr;
		TestEqual(TEXT("Voxel grid final apply occupied cells"), (*FinalApplyData)->GetNumberField(TEXT("occupied_cell_count")), 4.0);
		TestTrue(TEXT("Voxel grid final apply closes session"), (*FinalApplyData)->GetBoolField(TEXT("session_closed")));
		TestTrue(TEXT("Voxel grid final apply has collision"), (*FinalApplyData)->TryGetObjectField(TEXT("collision"), Collision) && Collision && Collision->IsValid());
		if (Collision && Collision->IsValid())
		{
			TestEqual(TEXT("Voxel grid final apply collision boxes"), (*Collision)->GetNumberField(TEXT("box_count")), 2.0);
			TestEqual(TEXT("Voxel grid final apply covered cells"), (*Collision)->GetNumberField(TEXT("covered_cell_count")), 4.0);
			TestTrue(TEXT("Voxel grid final apply exact collision coverage"), (*Collision)->GetBoolField(TEXT("coverage_matches_occupied_cells")));
		}
	}

	const FString DestroyBody = MakeExecRequestBody(
		TEXT("smoke-modeling-voxel-destroy-001"),
		TEXT("level_destroy_folder_actors"),
		FString::Printf(TEXT("{\"folder_path\":\"%s\",\"include_child_folders\":true}"), *FolderPath));
	FHttpSmokeResult DestroyResult;
	TestTrue(TEXT("Voxel grid destroy request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, DestroyBody, DestroyResult));
	TestEqual(TEXT("Voxel grid destroy status code"), DestroyResult.StatusCode, 200);

	TSharedPtr<FJsonObject> DestroyJson;
	TestTrue(TEXT("Voxel grid destroy parses as JSON"), ParseJson(DestroyResult.Body, DestroyJson));
	TestTrue(TEXT("Voxel grid destroy root ok=true"), DestroyJson.IsValid() && DestroyJson->GetBoolField(TEXT("ok")));

	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentInterfaceLevelActorWorkflowSmokeTest,
	"GptProjectTest.UeAgentInterface.Smoke.LevelActorWorkflow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUeAgentInterfaceLevelActorWorkflowSmokeTest::RunTest(const FString& Parameters)
{
	FScopedUeAgentHttpServer ServerScope;
	FString InitError;
	if (!ServerScope.Initialize(InitError))
	{
		AddError(FString::Printf(TEXT("Initialize failed: %s"), *InitError));
		return false;
	}

	const FString ActorLabel = FString::Printf(TEXT("SmokeActor_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8));
	const FString TransactionLabel = FString::Printf(TEXT("SmokeTx_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8));
	const FString RollbackActorLabel = FString::Printf(TEXT("SmokeRollbackActor_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8));
	const FString RollbackTransactionLabel = FString::Printf(TEXT("SmokeRollbackTx_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8));
	FString DuplicatedActorName;

	auto GetVectorComponent = [this](const TSharedPtr<FJsonObject>* Object, const TCHAR* FieldName, const TCHAR* ComponentName, double ExpectedValue, double Tolerance, const TCHAR* AssertionLabel)
	{
		if (!Object || !Object->IsValid())
		{
			AddError(FString::Printf(TEXT("%s object invalid"), AssertionLabel));
			return;
		}

		const TSharedPtr<FJsonObject>* VectorObject = nullptr;
		TestTrue(AssertionLabel, (*Object)->TryGetObjectField(FieldName, VectorObject) && VectorObject && VectorObject->IsValid());
		if (VectorObject && VectorObject->IsValid())
		{
			TestTrue(AssertionLabel, FMath::IsNearlyEqual((*VectorObject)->GetNumberField(ComponentName), ExpectedValue, Tolerance));
		}
	};

	auto ActorExistsByLabel = [&](const FString& Label, bool& bExistsOut) -> bool
	{
		const FString ListActorsBody = MakeExecRequestBody(
			TEXT("smoke-level-list-actors-001"),
			TEXT("list_actors"),
			TEXT("{\"limit\":2000}"));
		FHttpSmokeResult ListActorsResult;
		if (!TestTrue(TEXT("Level list_actors request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ListActorsBody, ListActorsResult)))
		{
			return false;
		}
		TestEqual(TEXT("Level list_actors status code"), ListActorsResult.StatusCode, 200);

		TSharedPtr<FJsonObject> ListActorsJson;
		if (!TestTrue(TEXT("Level list_actors response parses as JSON"), ParseJson(ListActorsResult.Body, ListActorsJson)))
		{
			return false;
		}
		TestTrue(TEXT("Level list_actors root ok=true"), ListActorsJson.IsValid() && ListActorsJson->GetBoolField(TEXT("ok")));

		const TSharedPtr<FJsonObject>* ListActorsData = nullptr;
		if (!TestTrue(TEXT("Level list_actors contains data object"), ListActorsJson.IsValid() && ListActorsJson->TryGetObjectField(TEXT("data"), ListActorsData) && ListActorsData && ListActorsData->IsValid()))
		{
			return false;
		}

		bExistsOut = false;
		if (ListActorsData && ListActorsData->IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* Actors = nullptr;
			TestTrue(TEXT("Level list_actors contains actors array"), (*ListActorsData)->TryGetArrayField(TEXT("actors"), Actors) && Actors != nullptr);
			if (Actors)
			{
				for (const TSharedPtr<FJsonValue>& ActorValue : *Actors)
				{
					const TSharedPtr<FJsonObject>* ActorObject = nullptr;
					if (!ActorValue.IsValid() || !ActorValue->TryGetObject(ActorObject) || !ActorObject || !ActorObject->IsValid())
					{
						continue;
					}

					if ((*ActorObject)->GetStringField(TEXT("label")) == Label)
					{
						bExistsOut = true;
						break;
					}
				}
			}
		}
		return true;
	};

	const FString SpawnBody = MakeExecRequestBody(
		TEXT("smoke-level-spawn-001"),
		TEXT("spawn_actor"),
		FString::Printf(
			TEXT("{\"class_path\":\"/Script/Engine.StaticMeshActor\",\"label\":\"%s\",\"static_mesh\":\"/Engine/BasicShapes/Cube.Cube\",\"location\":{\"x\":0,\"y\":0,\"z\":128},\"scale\":{\"x\":1.0,\"y\":1.0,\"z\":1.0}}"),
			*ActorLabel));
	FHttpSmokeResult SpawnResult;
	TestTrue(TEXT("Level spawn_actor request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SpawnBody, SpawnResult));
	TestEqual(TEXT("Level spawn_actor status code"), SpawnResult.StatusCode, 200);

	TSharedPtr<FJsonObject> SpawnJson;
	TestTrue(TEXT("Level spawn_actor response parses as JSON"), ParseJson(SpawnResult.Body, SpawnJson));
	TestTrue(TEXT("Level spawn_actor root ok=true"), SpawnJson.IsValid() && SpawnJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* SpawnData = nullptr;
	TestTrue(TEXT("Level spawn_actor contains data object"), SpawnJson.IsValid() && SpawnJson->TryGetObjectField(TEXT("data"), SpawnData) && SpawnData && SpawnData->IsValid());
	if (SpawnData && SpawnData->IsValid())
	{
		TestEqual(TEXT("Level spawn_actor label"), (*SpawnData)->GetStringField(TEXT("label")), ActorLabel);
		TestEqual(TEXT("Level spawn_actor class"), (*SpawnData)->GetStringField(TEXT("class")), FString(TEXT("/Script/Engine.StaticMeshActor")));
	}

	const FString SetSelectionBody = MakeExecRequestBody(
		TEXT("smoke-level-selection-001"),
		TEXT("level_set_selection"),
		FString::Printf(TEXT("{\"actor_ids\":[\"%s\"],\"mode\":\"replace\"}"), *ActorLabel));
	FHttpSmokeResult SetSelectionResult;
	TestTrue(TEXT("Level set_selection request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SetSelectionBody, SetSelectionResult));
	TestEqual(TEXT("Level set_selection status code"), SetSelectionResult.StatusCode, 200);

	TSharedPtr<FJsonObject> SetSelectionJson;
	TestTrue(TEXT("Level set_selection response parses as JSON"), ParseJson(SetSelectionResult.Body, SetSelectionJson));
	TestTrue(TEXT("Level set_selection root ok=true"), SetSelectionJson.IsValid() && SetSelectionJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* SetSelectionData = nullptr;
	TestTrue(TEXT("Level set_selection contains data object"), SetSelectionJson.IsValid() && SetSelectionJson->TryGetObjectField(TEXT("data"), SetSelectionData) && SetSelectionData && SetSelectionData->IsValid());
	if (SetSelectionData && SetSelectionData->IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Actors = nullptr;
		bool bFoundSelectedActor = false;
		TestTrue(TEXT("Level set_selection contains actors array"), (*SetSelectionData)->TryGetArrayField(TEXT("actors"), Actors) && Actors != nullptr);
		if (Actors)
		{
			for (const TSharedPtr<FJsonValue>& ActorValue : *Actors)
			{
				const TSharedPtr<FJsonObject>* ActorObject = nullptr;
				if (!ActorValue.IsValid() || !ActorValue->TryGetObject(ActorObject) || !ActorObject || !ActorObject->IsValid())
				{
					continue;
				}

				if ((*ActorObject)->GetStringField(TEXT("label")) == ActorLabel)
				{
					bFoundSelectedActor = true;
					break;
				}
			}
		}
		TestTrue(TEXT("Level set_selection selected actor present"), bFoundSelectedActor);
	}

	const FString SetTransformBody = MakeExecRequestBody(
		TEXT("smoke-level-transform-001"),
		TEXT("level_set_actor_transform"),
		FString::Printf(
			TEXT("{\"id\":\"%s\",\"location\":{\"x\":123,\"y\":45,\"z\":67},\"rotation\":{\"pitch\":0,\"yaw\":33,\"roll\":0},\"scale\":{\"x\":1.25,\"y\":1.0,\"z\":0.75}}"),
			*ActorLabel));
	FHttpSmokeResult SetTransformResult;
	TestTrue(TEXT("Level set_actor_transform request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SetTransformBody, SetTransformResult));
	TestEqual(TEXT("Level set_actor_transform status code"), SetTransformResult.StatusCode, 200);

	const FString GetTransformBody = MakeExecRequestBody(
		TEXT("smoke-level-get-transform-001"),
		TEXT("level_get_actor_transform"),
		FString::Printf(TEXT("{\"id\":\"%s\"}"), *ActorLabel));
	FHttpSmokeResult GetTransformResult;
	TestTrue(TEXT("Level get_actor_transform request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, GetTransformBody, GetTransformResult));
	TestEqual(TEXT("Level get_actor_transform status code"), GetTransformResult.StatusCode, 200);

	TSharedPtr<FJsonObject> GetTransformJson;
	TestTrue(TEXT("Level get_actor_transform response parses as JSON"), ParseJson(GetTransformResult.Body, GetTransformJson));
	TestTrue(TEXT("Level get_actor_transform root ok=true"), GetTransformJson.IsValid() && GetTransformJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* GetTransformData = nullptr;
	TestTrue(TEXT("Level get_actor_transform contains data object"), GetTransformJson.IsValid() && GetTransformJson->TryGetObjectField(TEXT("data"), GetTransformData) && GetTransformData && GetTransformData->IsValid());
	GetVectorComponent(GetTransformData, TEXT("location"), TEXT("x"), 123.0, 0.1, TEXT("Level get_actor_transform location.x"));
	GetVectorComponent(GetTransformData, TEXT("location"), TEXT("z"), 67.0, 0.1, TEXT("Level get_actor_transform location.z"));

	const FString BeginTransactionBody = MakeExecRequestBody(
		TEXT("smoke-level-begin-transaction-001"),
		TEXT("begin_transaction"),
		FString::Printf(TEXT("{\"label\":\"%s\"}"), *TransactionLabel));
	FHttpSmokeResult BeginTransactionResult;
	TestTrue(TEXT("Level begin_transaction request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, BeginTransactionBody, BeginTransactionResult));
	TestEqual(TEXT("Level begin_transaction status code"), BeginTransactionResult.StatusCode, 200);

	const FString SetLocationBody = MakeExecRequestBody(
		TEXT("smoke-level-location-001"),
		TEXT("level_set_actor_location"),
		FString::Printf(TEXT("{\"id\":\"%s\",\"location\":{\"x\":200,\"y\":300,\"z\":400}}"), *ActorLabel));
	FHttpSmokeResult SetLocationResult;
	TestTrue(TEXT("Level set_actor_location request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SetLocationBody, SetLocationResult));
	TestEqual(TEXT("Level set_actor_location status code"), SetLocationResult.StatusCode, 200);

	const FString EndTransactionBody = MakeExecRequestBody(
		TEXT("smoke-level-end-transaction-001"),
		TEXT("end_transaction"),
		TEXT("{\"commit\":true}"));
	FHttpSmokeResult EndTransactionResult;
	TestTrue(TEXT("Level end_transaction request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, EndTransactionBody, EndTransactionResult));
	TestEqual(TEXT("Level end_transaction status code"), EndTransactionResult.StatusCode, 200);

	const FString GetTransformMovedBody = MakeExecRequestBody(
		TEXT("smoke-level-get-transform-002"),
		TEXT("level_get_actor_transform"),
		FString::Printf(TEXT("{\"id\":\"%s\"}"), *ActorLabel));
	FHttpSmokeResult GetTransformMovedResult;
	TestTrue(TEXT("Level get_actor_transform moved request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, GetTransformMovedBody, GetTransformMovedResult));
	TestEqual(TEXT("Level get_actor_transform moved status code"), GetTransformMovedResult.StatusCode, 200);

	TSharedPtr<FJsonObject> GetTransformMovedJson;
	TestTrue(TEXT("Level get_actor_transform moved response parses as JSON"), ParseJson(GetTransformMovedResult.Body, GetTransformMovedJson));
	TestTrue(TEXT("Level get_actor_transform moved root ok=true"), GetTransformMovedJson.IsValid() && GetTransformMovedJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* GetTransformMovedData = nullptr;
	TestTrue(TEXT("Level get_actor_transform moved contains data object"), GetTransformMovedJson.IsValid() && GetTransformMovedJson->TryGetObjectField(TEXT("data"), GetTransformMovedData) && GetTransformMovedData && GetTransformMovedData->IsValid());
	GetVectorComponent(GetTransformMovedData, TEXT("location"), TEXT("x"), 200.0, 0.1, TEXT("Level moved transform location.x"));
	GetVectorComponent(GetTransformMovedData, TEXT("location"), TEXT("y"), 300.0, 0.1, TEXT("Level moved transform location.y"));

	const FString UndoBody = MakeExecRequestBody(TEXT("smoke-level-undo-001"), TEXT("undo"));
	FHttpSmokeResult UndoResult;
	TestTrue(TEXT("Level undo request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, UndoBody, UndoResult));
	TestEqual(TEXT("Level undo status code"), UndoResult.StatusCode, 200);

	const FString GetTransformUndoBody = MakeExecRequestBody(
		TEXT("smoke-level-get-transform-003"),
		TEXT("level_get_actor_transform"),
		FString::Printf(TEXT("{\"id\":\"%s\"}"), *ActorLabel));
	FHttpSmokeResult GetTransformUndoResult;
	TestTrue(TEXT("Level get_actor_transform after undo request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, GetTransformUndoBody, GetTransformUndoResult));
	TestEqual(TEXT("Level get_actor_transform after undo status code"), GetTransformUndoResult.StatusCode, 200);

	TSharedPtr<FJsonObject> GetTransformUndoJson;
	TestTrue(TEXT("Level get_actor_transform after undo response parses as JSON"), ParseJson(GetTransformUndoResult.Body, GetTransformUndoJson));
	TestTrue(TEXT("Level get_actor_transform after undo root ok=true"), GetTransformUndoJson.IsValid() && GetTransformUndoJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* GetTransformUndoData = nullptr;
	TestTrue(TEXT("Level get_actor_transform after undo contains data object"), GetTransformUndoJson.IsValid() && GetTransformUndoJson->TryGetObjectField(TEXT("data"), GetTransformUndoData) && GetTransformUndoData && GetTransformUndoData->IsValid());
	GetVectorComponent(GetTransformUndoData, TEXT("location"), TEXT("x"), 123.0, 0.1, TEXT("Level undo transform location.x"));
	GetVectorComponent(GetTransformUndoData, TEXT("location"), TEXT("z"), 67.0, 0.1, TEXT("Level undo transform location.z"));

	const FString RedoBody = MakeExecRequestBody(TEXT("smoke-level-redo-001"), TEXT("redo"));
	FHttpSmokeResult RedoResult;
	TestTrue(TEXT("Level redo request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, RedoBody, RedoResult));
	TestEqual(TEXT("Level redo status code"), RedoResult.StatusCode, 200);

	const FString GetTransformRedoBody = MakeExecRequestBody(
		TEXT("smoke-level-get-transform-004"),
		TEXT("level_get_actor_transform"),
		FString::Printf(TEXT("{\"id\":\"%s\"}"), *ActorLabel));
	FHttpSmokeResult GetTransformRedoResult;
	TestTrue(TEXT("Level get_actor_transform after redo request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, GetTransformRedoBody, GetTransformRedoResult));
	TestEqual(TEXT("Level get_actor_transform after redo status code"), GetTransformRedoResult.StatusCode, 200);

	TSharedPtr<FJsonObject> GetTransformRedoJson;
	TestTrue(TEXT("Level get_actor_transform after redo response parses as JSON"), ParseJson(GetTransformRedoResult.Body, GetTransformRedoJson));
	TestTrue(TEXT("Level get_actor_transform after redo root ok=true"), GetTransformRedoJson.IsValid() && GetTransformRedoJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* GetTransformRedoData = nullptr;
	TestTrue(TEXT("Level get_actor_transform after redo contains data object"), GetTransformRedoJson.IsValid() && GetTransformRedoJson->TryGetObjectField(TEXT("data"), GetTransformRedoData) && GetTransformRedoData && GetTransformRedoData->IsValid());
	GetVectorComponent(GetTransformRedoData, TEXT("location"), TEXT("x"), 200.0, 0.1, TEXT("Level redo transform location.x"));
	GetVectorComponent(GetTransformRedoData, TEXT("location"), TEXT("y"), 300.0, 0.1, TEXT("Level redo transform location.y"));

	const FString BeginRollbackTransactionBody = MakeExecRequestBody(
		TEXT("smoke-level-begin-rollback-transaction-001"),
		TEXT("begin_transaction"),
		FString::Printf(TEXT("{\"label\":\"%s\"}"), *RollbackTransactionLabel));
	FHttpSmokeResult BeginRollbackTransactionResult;
	TestTrue(TEXT("Level begin rollback transaction request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, BeginRollbackTransactionBody, BeginRollbackTransactionResult));
	TestEqual(TEXT("Level begin rollback transaction status code"), BeginRollbackTransactionResult.StatusCode, 200);

	const FString SpawnRollbackBody = MakeExecRequestBody(
		TEXT("smoke-level-spawn-rollback-001"),
		TEXT("spawn_actor"),
		FString::Printf(
			TEXT("{\"class_path\":\"/Script/Engine.StaticMeshActor\",\"label\":\"%s\",\"static_mesh\":\"/Engine/BasicShapes/Cube.Cube\",\"location\":{\"x\":320,\"y\":0,\"z\":128},\"scale\":{\"x\":0.5,\"y\":0.5,\"z\":0.5}}"),
			*RollbackActorLabel));
	FHttpSmokeResult SpawnRollbackResult;
	TestTrue(TEXT("Level spawn rollback actor request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SpawnRollbackBody, SpawnRollbackResult));
	TestEqual(TEXT("Level spawn rollback actor status code"), SpawnRollbackResult.StatusCode, 200);

	const FString EndRollbackTransactionBody = MakeExecRequestBody(
		TEXT("smoke-level-end-rollback-transaction-001"),
		TEXT("end_transaction"),
		TEXT("{\"commit\":false}"));
	FHttpSmokeResult EndRollbackTransactionResult;
	TestTrue(TEXT("Level rollback transaction request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, EndRollbackTransactionBody, EndRollbackTransactionResult));
	TestEqual(TEXT("Level rollback transaction status code"), EndRollbackTransactionResult.StatusCode, 200);

	TSharedPtr<FJsonObject> EndRollbackTransactionJson;
	TestTrue(TEXT("Level rollback transaction response parses as JSON"), ParseJson(EndRollbackTransactionResult.Body, EndRollbackTransactionJson));
	TestTrue(TEXT("Level rollback transaction root ok=true"), EndRollbackTransactionJson.IsValid() && EndRollbackTransactionJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* EndRollbackTransactionData = nullptr;
	TestTrue(TEXT("Level rollback transaction contains data object"), EndRollbackTransactionJson.IsValid() && EndRollbackTransactionJson->TryGetObjectField(TEXT("data"), EndRollbackTransactionData) && EndRollbackTransactionData && EndRollbackTransactionData->IsValid());
	if (EndRollbackTransactionData && EndRollbackTransactionData->IsValid())
	{
		TestTrue(TEXT("Level rollback transaction registered spawned actor"), (*EndRollbackTransactionData)->GetNumberField(TEXT("registered_spawned_actor_count")) >= 1.0);
		TestTrue(TEXT("Level rollback transaction removed spawned actor"), (*EndRollbackTransactionData)->GetNumberField(TEXT("rolled_back_spawned_actor_count")) >= 1.0);
	}

	bool bRollbackActorStillExists = true;
	if (ActorExistsByLabel(RollbackActorLabel, bRollbackActorStillExists))
	{
		TestFalse(TEXT("Level rollback actor removed from world"), bRollbackActorStillExists);
	}

	const FString DuplicateBody = MakeExecRequestBody(
		TEXT("smoke-level-duplicate-001"),
		TEXT("level_duplicate_actor"),
		FString::Printf(TEXT("{\"id\":\"%s\",\"offset\":{\"x\":100,\"y\":0,\"z\":0}}"), *ActorLabel));
	FHttpSmokeResult DuplicateResult;
	TestTrue(TEXT("Level duplicate_actor request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, DuplicateBody, DuplicateResult));
	TestEqual(TEXT("Level duplicate_actor status code"), DuplicateResult.StatusCode, 200);

	TSharedPtr<FJsonObject> DuplicateJson;
	TestTrue(TEXT("Level duplicate_actor response parses as JSON"), ParseJson(DuplicateResult.Body, DuplicateJson));
	TestTrue(TEXT("Level duplicate_actor root ok=true"), DuplicateJson.IsValid() && DuplicateJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* DuplicateData = nullptr;
	TestTrue(TEXT("Level duplicate_actor contains data object"), DuplicateJson.IsValid() && DuplicateJson->TryGetObjectField(TEXT("data"), DuplicateData) && DuplicateData && DuplicateData->IsValid());
	if (DuplicateData && DuplicateData->IsValid())
	{
		const TSharedPtr<FJsonObject>* DuplicatedActor = nullptr;
		TestTrue(TEXT("Level duplicate_actor contains duplicated_actor object"), (*DuplicateData)->TryGetObjectField(TEXT("duplicated_actor"), DuplicatedActor) && DuplicatedActor && DuplicatedActor->IsValid());
		if (DuplicatedActor && DuplicatedActor->IsValid())
		{
			DuplicatedActorName = (*DuplicatedActor)->GetStringField(TEXT("name"));
			TestTrue(TEXT("Level duplicate_actor duplicated name non-empty"), !DuplicatedActorName.IsEmpty());
		}
	}

	const FString DestroyDuplicateBody = MakeExecRequestBody(
		TEXT("smoke-level-destroy-duplicate-001"),
		TEXT("destroy_actor"),
		FString::Printf(TEXT("{\"id\":\"%s\"}"), *DuplicatedActorName));
	FHttpSmokeResult DestroyDuplicateResult;
	TestTrue(TEXT("Level destroy duplicate request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, DestroyDuplicateBody, DestroyDuplicateResult));
	TestEqual(TEXT("Level destroy duplicate status code"), DestroyDuplicateResult.StatusCode, 200);

	const FString DestroySourceBody = MakeExecRequestBody(
		TEXT("smoke-level-destroy-source-001"),
		TEXT("destroy_actor"),
		FString::Printf(TEXT("{\"id\":\"%s\"}"), *ActorLabel));
	FHttpSmokeResult DestroySourceResult;
	TestTrue(TEXT("Level destroy source request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, DestroySourceBody, DestroySourceResult));
	TestEqual(TEXT("Level destroy source status code"), DestroySourceResult.StatusCode, 200);

	bool bSourceActorStillExists = true;
	if (ActorExistsByLabel(ActorLabel, bSourceActorStillExists))
	{
		TestFalse(TEXT("Level source actor removed from world"), bSourceActorStillExists);
	}

	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentInterfaceLandscapeSmokeTest,
	"GptProjectTest.UeAgentInterface.Smoke.LandscapeWorkflow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUeAgentInterfaceLandscapeSmokeTest::RunTest(const FString& Parameters)
{
	FScopedUeAgentHttpServer ServerScope;
	FString InitError;
	if (!ServerScope.Initialize(InitError))
	{
		AddError(FString::Printf(TEXT("Initialize failed: %s"), *InitError));
		return false;
	}

	const FString LandscapeLabel = FString::Printf(TEXT("SmokeLandscape_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8));

	auto QueryWorldActors = [&]() -> TSharedPtr<FJsonObject>
	{
		const FString ListBody = MakeExecRequestBody(
			TEXT("smoke-landscape-list-actors-001"),
			TEXT("list_actors"),
			TEXT("{\"limit\":2000}"));
		FHttpSmokeResult ListResult;
		TestTrue(TEXT("Landscape list_actors request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ListBody, ListResult));
		TestEqual(TEXT("Landscape list_actors status code"), ListResult.StatusCode, 200);

		TSharedPtr<FJsonObject> ListJson;
		TestTrue(TEXT("Landscape list_actors response parses as JSON"), ParseJson(ListResult.Body, ListJson));
		TestTrue(TEXT("Landscape list_actors root ok=true"), ListJson.IsValid() && ListJson->GetBoolField(TEXT("ok")));

		const TSharedPtr<FJsonObject>* ListData = nullptr;
		TestTrue(TEXT("Landscape list_actors contains data object"), ListJson.IsValid() && ListJson->TryGetObjectField(TEXT("data"), ListData) && ListData && ListData->IsValid());
		return (ListData && ListData->IsValid()) ? *ListData : nullptr;
	};

	auto ListActorsContainsLabel = [&](const FString& Label, bool& bFoundOut)
	{
		const TSharedPtr<FJsonObject> ListData = QueryWorldActors();

		bFoundOut = false;
		if (ListData.IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* Actors = nullptr;
			TestTrue(TEXT("Landscape list_actors contains actors array"), ListData->TryGetArrayField(TEXT("actors"), Actors) && Actors != nullptr);
			if (Actors)
			{
				for (const TSharedPtr<FJsonValue>& ActorValue : *Actors)
				{
					const TSharedPtr<FJsonObject>* ActorObject = nullptr;
					if (!ActorValue.IsValid() || !ActorValue->TryGetObject(ActorObject) || !ActorObject || !ActorObject->IsValid())
					{
						continue;
					}

					if ((*ActorObject)->GetStringField(TEXT("label")) == Label)
					{
						bFoundOut = true;
						break;
					}
				}
			}
		}
	};

	double LandscapeCenterX = 0.0;
	double LandscapeCenterY = 0.0;
	double LandscapeCenterZ = 0.0;
	FString ActiveLandscapeLabel = LandscapeLabel;
	bool bReusedExistingLandscape = false;

	const TSharedPtr<FJsonObject> InitialActorsData = QueryWorldActors();
	if (InitialActorsData.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Actors = nullptr;
		TestTrue(TEXT("Landscape initial actor query contains actors array"), InitialActorsData->TryGetArrayField(TEXT("actors"), Actors) && Actors != nullptr);
		if (Actors)
		{
			for (const TSharedPtr<FJsonValue>& ActorValue : *Actors)
			{
				const TSharedPtr<FJsonObject>* ActorObject = nullptr;
				if (!ActorValue.IsValid() || !ActorValue->TryGetObject(ActorObject) || !ActorObject || !ActorObject->IsValid())
				{
					continue;
				}

				FString ActorClass;
				(*ActorObject)->TryGetStringField(TEXT("class"), ActorClass);
				if (!ActorClass.Contains(TEXT("Landscape")))
				{
					continue;
				}

				(*ActorObject)->TryGetStringField(TEXT("label"), ActiveLandscapeLabel);
				const TSharedPtr<FJsonObject>* LocationObject = nullptr;
				if ((*ActorObject)->TryGetObjectField(TEXT("location"), LocationObject) && LocationObject && LocationObject->IsValid())
				{
					LandscapeCenterX = (*LocationObject)->GetNumberField(TEXT("x"));
					LandscapeCenterY = (*LocationObject)->GetNumberField(TEXT("y"));
					LandscapeCenterZ = (*LocationObject)->GetNumberField(TEXT("z"));
				}

				bReusedExistingLandscape = true;
				break;
			}
		}
	}

	if (!bReusedExistingLandscape)
	{
		const FString CreateLandscapeBody = MakeExecRequestBody(
			TEXT("smoke-landscape-create-001"),
			TEXT("landscape_create"),
			FString::Printf(TEXT("{\"label\":\"%s\",\"quads_per_section\":7,\"sections_per_component\":1,\"component_count_x\":1,\"component_count_y\":1,\"location\":{\"x\":200000,\"y\":0,\"z\":0},\"scale\":{\"x\":100,\"y\":100,\"z\":100}}"), *LandscapeLabel));
		FHttpSmokeResult CreateLandscapeResult;
		TestTrue(TEXT("Landscape create request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, CreateLandscapeBody, CreateLandscapeResult));
		TestEqual(TEXT("Landscape create status code"), CreateLandscapeResult.StatusCode, 200);

		TSharedPtr<FJsonObject> CreateLandscapeJson;
		TestTrue(TEXT("Landscape create response parses as JSON"), ParseJson(CreateLandscapeResult.Body, CreateLandscapeJson));
		TestTrue(TEXT("Landscape create root ok=true"), CreateLandscapeJson.IsValid() && CreateLandscapeJson->GetBoolField(TEXT("ok")));

		const TSharedPtr<FJsonObject>* CreateLandscapeData = nullptr;
		TestTrue(TEXT("Landscape create contains data object"), CreateLandscapeJson.IsValid() && CreateLandscapeJson->TryGetObjectField(TEXT("data"), CreateLandscapeData) && CreateLandscapeData && CreateLandscapeData->IsValid());
		if (CreateLandscapeData && CreateLandscapeData->IsValid())
		{
			(*CreateLandscapeData)->TryGetStringField(TEXT("label"), ActiveLandscapeLabel);
			TestEqual(TEXT("Landscape create label"), ActiveLandscapeLabel, LandscapeLabel);
			const TSharedPtr<FJsonObject>* LocationObject = nullptr;
			TestTrue(TEXT("Landscape create contains location object"), (*CreateLandscapeData)->TryGetObjectField(TEXT("location"), LocationObject) && LocationObject && LocationObject->IsValid());
			if (LocationObject && LocationObject->IsValid())
			{
				LandscapeCenterX = (*LocationObject)->GetNumberField(TEXT("x"));
				LandscapeCenterY = (*LocationObject)->GetNumberField(TEXT("y"));
				LandscapeCenterZ = (*LocationObject)->GetNumberField(TEXT("z"));
			}
		}
	}

	bool bLandscapeExistsAfterCreate = false;
	ListActorsContainsLabel(ActiveLandscapeLabel, bLandscapeExistsAfterCreate);
	TestTrue(TEXT("Landscape actor present after create or reuse"), bLandscapeExistsAfterCreate);

	const FString RaiseLandscapeBody = MakeExecRequestBody(
		TEXT("smoke-landscape-raise-001"),
		TEXT("landscape_raise_circle"),
		FString::Printf(TEXT("{\"center\":{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f},\"radius_cm\":300.0,\"strength_cm\":40.0,\"falloff\":1.0}"), LandscapeCenterX, LandscapeCenterY, LandscapeCenterZ));
	FHttpSmokeResult RaiseLandscapeResult;
	TestTrue(TEXT("Landscape raise_circle request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, RaiseLandscapeBody, RaiseLandscapeResult));
	TestEqual(TEXT("Landscape raise_circle status code"), RaiseLandscapeResult.StatusCode, 200);

	TSharedPtr<FJsonObject> RaiseLandscapeJson;
	TestTrue(TEXT("Landscape raise_circle response parses as JSON"), ParseJson(RaiseLandscapeResult.Body, RaiseLandscapeJson));
	TestTrue(TEXT("Landscape raise_circle root ok=true"), RaiseLandscapeJson.IsValid() && RaiseLandscapeJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* RaiseLandscapeData = nullptr;
	TestTrue(TEXT("Landscape raise_circle contains data object"), RaiseLandscapeJson.IsValid() && RaiseLandscapeJson->TryGetObjectField(TEXT("data"), RaiseLandscapeData) && RaiseLandscapeData && RaiseLandscapeData->IsValid());
	if (RaiseLandscapeData && RaiseLandscapeData->IsValid())
	{
		TestTrue(TEXT("Landscape raise_circle region_x2 >= region_x1"), static_cast<int32>((*RaiseLandscapeData)->GetIntegerField(TEXT("region_x2"))) >= static_cast<int32>((*RaiseLandscapeData)->GetIntegerField(TEXT("region_x1"))));
		TestTrue(TEXT("Landscape raise_circle region_y2 >= region_y1"), static_cast<int32>((*RaiseLandscapeData)->GetIntegerField(TEXT("region_y2"))) >= static_cast<int32>((*RaiseLandscapeData)->GetIntegerField(TEXT("region_y1"))));
	}

	return !HasAnyErrors();
}

#endif

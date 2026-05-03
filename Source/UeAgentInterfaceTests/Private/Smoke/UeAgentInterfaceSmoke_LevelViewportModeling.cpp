#if WITH_DEV_AUTOMATION_TESTS

#include "UeAgentSmokeTestSharedIncludes.inl"

#include "Components/DirectionalLightComponent.h"

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
			const FRotator& ExpectedRotationLocal) -> bool
	{
		const FString CreateBodyLocal = MakeExecRequestBody(
			RequestId,
			Command,
			FString::Printf(
				TEXT("{\"bounds\":{\"center\":{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f},\"extent\":{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f}},\"rotation\":{\"pitch\":%.3f,\"yaw\":%.3f,\"roll\":%.3f},\"folder_path\":\"%s\"}"),
				ExpectedCenterLocal.X,
				ExpectedCenterLocal.Y,
				ExpectedCenterLocal.Z,
				ExpectedExtentLocal.X,
				ExpectedExtentLocal.Y,
				ExpectedExtentLocal.Z,
				ExpectedRotationLocal.Pitch,
				ExpectedRotationLocal.Yaw,
				ExpectedRotationLocal.Roll,
				*FolderPath));

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
		TestTrue(FString::Printf(TEXT("%s contains created_actor"), *Command), (*CreateDataLocal)->TryGetObjectField(TEXT("created_actor"), CreatedActorLocal) && CreatedActorLocal && CreatedActorLocal->IsValid());
		if (CreatedActorLocal && CreatedActorLocal->IsValid())
		{
			CreatedActorNameLocal = (*CreatedActorLocal)->GetStringField(TEXT("name"));
			TestFalse(FString::Printf(TEXT("%s created_actor name not empty"), *Command), CreatedActorNameLocal.IsEmpty());
		}

		const TSharedPtr<FJsonObject>* DerivedPrimitiveLocal = nullptr;
		TestTrue(FString::Printf(TEXT("%s contains derived_primitive"), *Command), (*CreateDataLocal)->TryGetObjectField(TEXT("derived_primitive"), DerivedPrimitiveLocal) && DerivedPrimitiveLocal && DerivedPrimitiveLocal->IsValid());
		if (DerivedPrimitiveLocal && DerivedPrimitiveLocal->IsValid())
		{
			TestEqual(FString::Printf(TEXT("%s derived depth"), *Command), (*DerivedPrimitiveLocal)->GetNumberField(TEXT("Depth")), static_cast<double>(ExpectedExtentLocal.X * 2.0f));
			TestEqual(FString::Printf(TEXT("%s derived width"), *Command), (*DerivedPrimitiveLocal)->GetNumberField(TEXT("Width")), static_cast<double>(ExpectedExtentLocal.Y * 2.0f));
			TestEqual(FString::Printf(TEXT("%s derived height"), *Command), (*DerivedPrimitiveLocal)->GetNumberField(TEXT("Height")), static_cast<double>(ExpectedExtentLocal.Z * 2.0f));
		}

		if (CreatedActorNameLocal.IsEmpty())
		{
			return false;
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

		return !HasAnyErrors();
	};
	VerifyCreatedPrimitiveByBounds(TEXT("smoke-modeling-create-box-bounds-001"), TEXT("modeling_create_box"), ExpectedCenter, ExpectedExtent, ExpectedRotation);
	VerifyCreatedPrimitiveByBounds(TEXT("smoke-modeling-create-ramp-bounds-001"), TEXT("modeling_create_ramp"), ExpectedCenter + FVector(400.0f, 0.0f, 0.0f), FVector(120.0f, 90.0f, 60.0f), FRotator(0.0f, 90.0f, 0.0f));
	VerifyCreatedPrimitiveByBounds(TEXT("smoke-modeling-create-ramp-corner-bounds-001"), TEXT("modeling_create_ramp_corner"), ExpectedCenter + FVector(-400.0f, 0.0f, 0.0f), FVector(100.0f, 100.0f, 80.0f), FRotator(0.0f, -25.0f, 0.0f));

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

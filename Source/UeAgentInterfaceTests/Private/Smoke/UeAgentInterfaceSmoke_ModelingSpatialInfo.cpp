#if WITH_DEV_AUTOMATION_TESTS

#include "UeAgentSmokeTestSharedIncludes.inl"

#include "Engine/StaticMeshActor.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentInterfaceModelingSpatialInfoSmokeTest,
	"GptProjectTest.UeAgentInterface.Smoke.ModelingSpatialInfo",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUeAgentInterfaceModelingSpatialInfoSmokeTest::RunTest(const FString& Parameters)
{
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
		TestTrue(FString::Printf(TEXT("%s request completed"), *RequestId), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, Body, Result, 30.0));
		TestEqual(FString::Printf(TEXT("%s status code"), *RequestId), Result.StatusCode, ExpectedStatus);
		TestTrue(FString::Printf(TEXT("%s response parses as JSON"), *RequestId), ParseJson(Result.Body, OutJson));
		return OutJson.IsValid();
	};

	auto GetData = [this](const FString& Label, const TSharedPtr<FJsonObject>& Json) -> const TSharedPtr<FJsonObject>*
	{
		const TSharedPtr<FJsonObject>* Data = nullptr;
		TestTrue(FString::Printf(TEXT("%s contains data object"), *Label), Json.IsValid() && Json->TryGetObjectField(TEXT("data"), Data) && Data && Data->IsValid());
		return Data;
	};

	auto ValidateSpatialData = [this](const FString& Label, const TSharedPtr<FJsonObject>& Data, const FString& ExpectedTargetType, const FString& ExpectedBoundsSpace) -> bool
	{
		if (!Data.IsValid())
		{
			AddError(FString::Printf(TEXT("%s data missing"), *Label));
			return false;
		}

		const TSharedPtr<FJsonObject>* Target = nullptr;
		const TSharedPtr<FJsonObject>* Frame = nullptr;
		const TSharedPtr<FJsonObject>* Aabb = nullptr;
		const TSharedPtr<FJsonObject>* PivotInAabb = nullptr;
		TestTrue(FString::Printf(TEXT("%s target exists"), *Label), Data->TryGetObjectField(TEXT("target"), Target) && Target && Target->IsValid());
		TestTrue(FString::Printf(TEXT("%s frame exists"), *Label), Data->TryGetObjectField(TEXT("frame"), Frame) && Frame && Frame->IsValid());
		TestTrue(FString::Printf(TEXT("%s aabb exists"), *Label), Data->TryGetObjectField(TEXT("aabb"), Aabb) && Aabb && Aabb->IsValid());
		TestTrue(FString::Printf(TEXT("%s pivot_in_aabb exists"), *Label), Data->TryGetObjectField(TEXT("pivot_in_aabb"), PivotInAabb) && PivotInAabb && PivotInAabb->IsValid());
		TestFalse(FString::Printf(TEXT("%s does not return world by default"), *Label), Data->HasField(TEXT("world")));
		TestFalse(FString::Printf(TEXT("%s does not return corners by default"), *Label), Data->HasField(TEXT("corners")));
		TestFalse(FString::Printf(TEXT("%s does not return center by default"), *Label), Data->HasField(TEXT("center")));
		TestFalse(FString::Printf(TEXT("%s does not return extent by default"), *Label), Data->HasField(TEXT("extent")));

		if (Target && Target->IsValid())
		{
			TestEqual(FString::Printf(TEXT("%s target type"), *Label), (*Target)->GetStringField(TEXT("target_type")), ExpectedTargetType);
		}
		if (Frame && Frame->IsValid())
		{
			TestEqual(FString::Printf(TEXT("%s bounds space"), *Label), (*Frame)->GetStringField(TEXT("bounds_space")), ExpectedBoundsSpace);
			TestEqual(FString::Printf(TEXT("%s bounds source"), *Label), (*Frame)->GetStringField(TEXT("bounds_source")), FString(TEXT("visual_mesh")));
			TestEqual(FString::Printf(TEXT("%s unit"), *Label), (*Frame)->GetStringField(TEXT("unit")), FString(TEXT("cm")));
		}
		if (Aabb && Aabb->IsValid())
		{
			const TSharedPtr<FJsonObject>* Min = nullptr;
			const TSharedPtr<FJsonObject>* Max = nullptr;
			TestTrue(FString::Printf(TEXT("%s aabb min exists"), *Label), (*Aabb)->TryGetObjectField(TEXT("min"), Min) && Min && Min->IsValid());
			TestTrue(FString::Printf(TEXT("%s aabb max exists"), *Label), (*Aabb)->TryGetObjectField(TEXT("max"), Max) && Max && Max->IsValid());
		}
		if (PivotInAabb && PivotInAabb->IsValid())
		{
			const TSharedPtr<FJsonObject>* Ratio = nullptr;
			TestTrue(FString::Printf(TEXT("%s pivot ratio exists"), *Label), (*PivotInAabb)->TryGetObjectField(TEXT("ratio"), Ratio) && Ratio && Ratio->IsValid());
		}
		return !HasAnyErrors();
	};

	auto ValidateCenteredRatio = [this](const FString& Label, const TSharedPtr<FJsonObject>& Data) -> void
	{
		const TSharedPtr<FJsonObject>* PivotInAabb = nullptr;
		const TSharedPtr<FJsonObject>* Ratio = nullptr;
		TestTrue(FString::Printf(TEXT("%s pivot_in_aabb exists"), *Label), Data.IsValid() && Data->TryGetObjectField(TEXT("pivot_in_aabb"), PivotInAabb) && PivotInAabb && PivotInAabb->IsValid());
		TestTrue(FString::Printf(TEXT("%s ratio exists"), *Label), PivotInAabb && (*PivotInAabb)->TryGetObjectField(TEXT("ratio"), Ratio) && Ratio && Ratio->IsValid());
		if (Ratio && Ratio->IsValid())
		{
			TestTrue(FString::Printf(TEXT("%s ratio x centered"), *Label), FMath::IsNearlyEqual((*Ratio)->GetNumberField(TEXT("x")), 0.5, 0.01));
			TestTrue(FString::Printf(TEXT("%s ratio y centered"), *Label), FMath::IsNearlyEqual((*Ratio)->GetNumberField(TEXT("y")), 0.5, 0.01));
			TestTrue(FString::Printf(TEXT("%s ratio z centered"), *Label), FMath::IsNearlyEqual((*Ratio)->GetNumberField(TEXT("z")), 0.5, 0.01));
		}
	};

	auto ValidateNumericRatio = [this](const FString& Label, const TSharedPtr<FJsonObject>& Data) -> void
	{
		const TSharedPtr<FJsonObject>* PivotInAabb = nullptr;
		const TSharedPtr<FJsonObject>* Ratio = nullptr;
		TestTrue(FString::Printf(TEXT("%s pivot_in_aabb exists"), *Label), Data.IsValid() && Data->TryGetObjectField(TEXT("pivot_in_aabb"), PivotInAabb) && PivotInAabb && PivotInAabb->IsValid());
		TestTrue(FString::Printf(TEXT("%s ratio exists"), *Label), PivotInAabb && (*PivotInAabb)->TryGetObjectField(TEXT("ratio"), Ratio) && Ratio && Ratio->IsValid());
		if (Ratio && Ratio->IsValid())
		{
			TestTrue(FString::Printf(TEXT("%s ratio x numeric"), *Label), (*Ratio)->HasTypedField<EJson::Number>(TEXT("x")));
			TestTrue(FString::Printf(TEXT("%s ratio y numeric"), *Label), (*Ratio)->HasTypedField<EJson::Number>(TEXT("y")));
			TestTrue(FString::Printf(TEXT("%s ratio z numeric"), *Label), (*Ratio)->HasTypedField<EJson::Number>(TEXT("z")));
		}
	};

	auto ValidatePivotAfterRatio = [this](const FString& Label, const TSharedPtr<FJsonObject>& Data, double ExpectedX, double ExpectedY, double ExpectedZ) -> void
	{
		const TSharedPtr<FJsonObject>* PivotAfter = nullptr;
		const TSharedPtr<FJsonObject>* PivotInAabb = nullptr;
		const TSharedPtr<FJsonObject>* Ratio = nullptr;
		TestTrue(FString::Printf(TEXT("%s pivot_after exists"), *Label), Data.IsValid() && Data->TryGetObjectField(TEXT("pivot_after"), PivotAfter) && PivotAfter && PivotAfter->IsValid());
		TestTrue(FString::Printf(TEXT("%s pivot_after pivot_in_aabb exists"), *Label), PivotAfter && (*PivotAfter)->TryGetObjectField(TEXT("pivot_in_aabb"), PivotInAabb) && PivotInAabb && PivotInAabb->IsValid());
		TestTrue(FString::Printf(TEXT("%s pivot_after ratio exists"), *Label), PivotInAabb && (*PivotInAabb)->TryGetObjectField(TEXT("ratio"), Ratio) && Ratio && Ratio->IsValid());
		if (Ratio && Ratio->IsValid())
		{
			TestTrue(FString::Printf(TEXT("%s pivot_after ratio x"), *Label), FMath::IsNearlyEqual((*Ratio)->GetNumberField(TEXT("x")), ExpectedX, 0.01));
			TestTrue(FString::Printf(TEXT("%s pivot_after ratio y"), *Label), FMath::IsNearlyEqual((*Ratio)->GetNumberField(TEXT("y")), ExpectedY, 0.01));
			TestTrue(FString::Printf(TEXT("%s pivot_after ratio z"), *Label), FMath::IsNearlyEqual((*Ratio)->GetNumberField(TEXT("z")), ExpectedZ, 0.01));
		}
	};

	TSharedPtr<FJsonObject> StaticAssetJson;
	if (ExecJson(TEXT("smoke-modeling-spatial-static-asset"), TEXT("modeling_get_mesh_spatial_info"), TEXT("{\"asset_path\":\"/Engine/BasicShapes/Cube.Cube\"}"), 200, StaticAssetJson))
	{
		TestTrue(TEXT("Static asset spatial info ok"), StaticAssetJson->GetBoolField(TEXT("ok")));
		const TSharedPtr<FJsonObject>* Data = GetData(TEXT("Static asset spatial info"), StaticAssetJson);
		if (Data && Data->IsValid())
		{
			ValidateSpatialData(TEXT("Static asset spatial info"), *Data, TEXT("static_mesh_asset"), TEXT("asset_local"));
			ValidateCenteredRatio(TEXT("Static asset spatial info"), *Data);
		}
	}

	TSharedPtr<FJsonObject> SkeletalAssetJson;
	if (ExecJson(TEXT("smoke-modeling-spatial-skeletal-asset"), TEXT("modeling_get_mesh_spatial_info"), TEXT("{\"asset_path\":\"/Engine/EngineMeshes/SkeletalCube.SkeletalCube\"}"), 200, SkeletalAssetJson))
	{
		TestTrue(TEXT("Skeletal asset spatial info ok"), SkeletalAssetJson->GetBoolField(TEXT("ok")));
		const TSharedPtr<FJsonObject>* Data = GetData(TEXT("Skeletal asset spatial info"), SkeletalAssetJson);
		if (Data && Data->IsValid())
		{
			ValidateSpatialData(TEXT("Skeletal asset spatial info"), *Data, TEXT("skeletal_mesh_asset"), TEXT("asset_local"));
			ValidateNumericRatio(TEXT("Skeletal asset spatial info"), *Data);
		}
	}

	TSharedPtr<FJsonObject> PlaneAssetJson;
	if (ExecJson(TEXT("smoke-modeling-spatial-zero-axis"), TEXT("modeling_get_mesh_spatial_info"), TEXT("{\"asset_path\":\"/Engine/BasicShapes/Plane.Plane\"}"), 200, PlaneAssetJson))
	{
		TestTrue(TEXT("Plane asset spatial info ok"), PlaneAssetJson->GetBoolField(TEXT("ok")));
		const TSharedPtr<FJsonObject>* Data = GetData(TEXT("Plane asset spatial info"), PlaneAssetJson);
		if (Data && Data->IsValid())
		{
			ValidateSpatialData(TEXT("Plane asset spatial info"), *Data, TEXT("static_mesh_asset"), TEXT("asset_local"));
			const TArray<TSharedPtr<FJsonValue>>* Issues = nullptr;
			TestTrue(TEXT("Plane asset spatial info reports issues"), (*Data)->TryGetArrayField(TEXT("issues"), Issues) && Issues && Issues->Num() > 0);
			const TSharedPtr<FJsonObject>* PivotInAabb = nullptr;
			const TSharedPtr<FJsonObject>* Ratio = nullptr;
			TestTrue(TEXT("Plane asset pivot ratio exists"), (*Data)->TryGetObjectField(TEXT("pivot_in_aabb"), PivotInAabb) && PivotInAabb && PivotInAabb->IsValid());
			TestTrue(TEXT("Plane asset ratio exists"), PivotInAabb && (*PivotInAabb)->TryGetObjectField(TEXT("ratio"), Ratio) && Ratio && Ratio->IsValid());
			if (Ratio && Ratio->IsValid())
			{
				TestTrue(TEXT("Plane asset ratio z is null"), (*Ratio)->HasTypedField<EJson::Null>(TEXT("z")));
			}
		}
	}

	UWorld* EditorWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	UStaticMesh* CubeMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	TestNotNull(TEXT("Modeling spatial cube mesh exists"), CubeMesh);
	const FString AmbiguousLabel = FString::Printf(TEXT("UAI_SpatialInfo_Ambiguous_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	AActor* AmbiguousActor = (EditorWorld && CubeMesh) ? EditorWorld->SpawnActor<AActor>(AActor::StaticClass(), FVector(30000.0, 30000.0, 120.0), FRotator::ZeroRotator) : nullptr;
	TestNotNull(TEXT("Modeling spatial ambiguous actor spawned"), AmbiguousActor);
	if (AmbiguousActor && CubeMesh)
	{
		AmbiguousActor->SetActorLabel(AmbiguousLabel);
		UStaticMeshComponent* MeshA = NewObject<UStaticMeshComponent>(AmbiguousActor, TEXT("SpatialMeshA"));
		UStaticMeshComponent* MeshB = NewObject<UStaticMeshComponent>(AmbiguousActor, TEXT("SpatialMeshB"));
		MeshA->SetStaticMesh(CubeMesh);
		MeshB->SetStaticMesh(CubeMesh);
		AmbiguousActor->SetRootComponent(MeshA);
		MeshB->SetupAttachment(MeshA);
		AmbiguousActor->AddInstanceComponent(MeshA);
		AmbiguousActor->AddInstanceComponent(MeshB);
		MeshA->RegisterComponent();
		MeshB->RegisterComponent();
	}

	if (AmbiguousActor)
	{
		TSharedPtr<FJsonObject> AmbiguousJson;
		ExecJson(
			TEXT("smoke-modeling-spatial-ambiguous-component"),
			TEXT("modeling_get_mesh_spatial_info"),
			FString::Printf(TEXT("{\"actor_label\":\"%s\"}"), *AmbiguousLabel),
			400,
			AmbiguousJson);
		TestTrue(TEXT("Ambiguous component spatial info ok=false"), AmbiguousJson.IsValid() && !AmbiguousJson->GetBoolField(TEXT("ok")));
		const FString ErrorText = AmbiguousJson.IsValid() ? AmbiguousJson->GetStringField(TEXT("error")) : FString();
		TestTrue(TEXT("Ambiguous component error code"), ErrorText.Contains(TEXT("ambiguous_mesh_component")));

		TSharedPtr<FJsonObject> ComponentJson;
		if (ExecJson(
			TEXT("smoke-modeling-spatial-component"),
			TEXT("modeling_get_mesh_spatial_info"),
			FString::Printf(TEXT("{\"actor_label\":\"%s\",\"component_name\":\"SpatialMeshA\"}"), *AmbiguousLabel),
			200,
			ComponentJson))
		{
			TestTrue(TEXT("Component spatial info ok"), ComponentJson->GetBoolField(TEXT("ok")));
			const TSharedPtr<FJsonObject>* Data = GetData(TEXT("Component spatial info"), ComponentJson);
			if (Data && Data->IsValid())
			{
				ValidateSpatialData(TEXT("Component spatial info"), *Data, TEXT("static_mesh_component"), TEXT("component_local"));
				ValidateCenteredRatio(TEXT("Component spatial info"), *Data);
			}
		}
	}

	FString StaticDuplicateAssetPath;
	const FString SharedLabelA = FString::Printf(TEXT("UAI_SetPivot_Static_A_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	const FString SharedLabelB = FString::Printf(TEXT("UAI_SetPivot_Static_B_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	AStaticMeshActor* SharedActorA = (EditorWorld && CubeMesh) ? EditorWorld->SpawnActor<AStaticMeshActor>(FVector(30600.0, 30000.0, 120.0), FRotator::ZeroRotator) : nullptr;
	AStaticMeshActor* SharedActorB = (EditorWorld && CubeMesh) ? EditorWorld->SpawnActor<AStaticMeshActor>(FVector(30800.0, 30000.0, 120.0), FRotator::ZeroRotator) : nullptr;
	TestNotNull(TEXT("Static pivot shared actor A spawned"), SharedActorA);
	TestNotNull(TEXT("Static pivot shared actor B spawned"), SharedActorB);
	if (SharedActorA && SharedActorB && CubeMesh)
	{
		SharedActorA->SetActorLabel(SharedLabelA);
		SharedActorB->SetActorLabel(SharedLabelB);
		SharedActorA->GetStaticMeshComponent()->SetStaticMesh(CubeMesh);
		SharedActorB->GetStaticMeshComponent()->SetStaticMesh(CubeMesh);
		SharedActorA->GetStaticMeshComponent()->UpdateBounds();
		SharedActorB->GetStaticMeshComponent()->UpdateBounds();

		TSharedPtr<FJsonObject> StaticPivotJson;
		if (ExecJson(
			TEXT("smoke-modeling-set-pivot-static-duplicate"),
			TEXT("modeling_set_pivot"),
			FString::Printf(TEXT("{\"actor_label\":\"%s\",\"preset\":\"bottom\",\"asset_write_policy\":\"duplicate_if_shared\",\"readback\":true}"), *SharedLabelA),
			200,
			StaticPivotJson))
		{
			TestTrue(TEXT("Static duplicate pivot ok"), StaticPivotJson->GetBoolField(TEXT("ok")));
			const TSharedPtr<FJsonObject>* Data = GetData(TEXT("Static duplicate pivot"), StaticPivotJson);
			if (Data && Data->IsValid())
			{
				TestEqual(TEXT("Static duplicate pivot backend"), (*Data)->GetStringField(TEXT("pivot_backend")), FString(TEXT("static_mesh_asset")));
				TestTrue(TEXT("Static duplicate pivot accepted"), (*Data)->GetBoolField(TEXT("accepted")));
				TestTrue(TEXT("Static duplicate pivot duplicates asset"), (*Data)->GetBoolField(TEXT("asset_duplicated")));
				TestFalse(TEXT("Static duplicate pivot does not modify shared original"), (*Data)->GetBoolField(TEXT("shared_asset_modified")));
				TestEqual(TEXT("Static duplicate pivot moves one component"), (int32)(*Data)->GetNumberField(TEXT("affected_component_count")), 1);
				TestTrue(TEXT("Static duplicate target component rebound"), SharedActorA->GetStaticMeshComponent()->GetStaticMesh() != CubeMesh);
				TestTrue(TEXT("Static duplicate other component keeps source"), SharedActorB->GetStaticMeshComponent()->GetStaticMesh() == CubeMesh);
				ValidatePivotAfterRatio(TEXT("Static duplicate pivot"), *Data, 0.5, 0.5, 0.0);
				(*Data)->TryGetStringField(TEXT("edited_asset_path"), StaticDuplicateAssetPath);
				TestFalse(TEXT("Static duplicate asset path differs from source"), StaticDuplicateAssetPath.Equals(TEXT("/Engine/BasicShapes/Cube"), ESearchCase::IgnoreCase));
			}
		}
	}

	const FString DynamicLabel = FString::Printf(TEXT("UAI_SpatialInfo_Dynamic_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	TSharedPtr<FJsonObject> CreateDynamicJson;
	if (ExecJson(
		TEXT("smoke-modeling-spatial-create-dynamic"),
		TEXT("modeling_create_box"),
		FString::Printf(TEXT("{\"bounds\":{\"center\":{\"x\":30400,\"y\":30000,\"z\":120},\"extent\":{\"x\":40,\"y\":50,\"z\":60}},\"label\":\"%s\",\"folder_path\":\"Smoke/ModelingSpatialInfo\"}"), *DynamicLabel),
		200,
		CreateDynamicJson))
	{
		TestTrue(TEXT("Dynamic mesh create ok"), CreateDynamicJson->GetBoolField(TEXT("ok")));
		TSharedPtr<FJsonObject> DynamicJson;
		if (ExecJson(
			TEXT("smoke-modeling-spatial-dynamic-component"),
			TEXT("modeling_get_mesh_spatial_info"),
			FString::Printf(TEXT("{\"actor_label\":\"%s\"}"), *DynamicLabel),
			200,
			DynamicJson))
		{
			TestTrue(TEXT("Dynamic component spatial info ok"), DynamicJson->GetBoolField(TEXT("ok")));
			const TSharedPtr<FJsonObject>* Data = GetData(TEXT("Dynamic component spatial info"), DynamicJson);
			if (Data && Data->IsValid())
			{
				ValidateSpatialData(TEXT("Dynamic component spatial info"), *Data, TEXT("dynamic_mesh_component"), TEXT("component_local"));
				ValidateCenteredRatio(TEXT("Dynamic component spatial info"), *Data);
			}
		}

		TSharedPtr<FJsonObject> DynamicBottomPivotJson;
		if (ExecJson(
			TEXT("smoke-modeling-set-pivot-dynamic-bottom"),
			TEXT("modeling_set_pivot"),
			FString::Printf(TEXT("{\"actor_label\":\"%s\",\"preset\":\"bottom\",\"readback\":true}"), *DynamicLabel),
			200,
			DynamicBottomPivotJson))
		{
			TestTrue(TEXT("Dynamic bottom pivot ok"), DynamicBottomPivotJson->GetBoolField(TEXT("ok")));
			const TSharedPtr<FJsonObject>* Data = GetData(TEXT("Dynamic bottom pivot"), DynamicBottomPivotJson);
			if (Data && Data->IsValid())
			{
				TestEqual(TEXT("Dynamic bottom pivot backend"), (*Data)->GetStringField(TEXT("pivot_backend")), FString(TEXT("dynamic_mesh_component")));
				TestTrue(TEXT("Dynamic bottom pivot accepted"), (*Data)->GetBoolField(TEXT("accepted")));
				TestTrue(TEXT("Dynamic bottom pivot preserves geometry"), (*Data)->GetBoolField(TEXT("geometry_preserved")));
				ValidatePivotAfterRatio(TEXT("Dynamic bottom pivot"), *Data, 0.5, 0.5, 0.0);
			}
		}

		TSharedPtr<FJsonObject> DynamicCustomPivotJson;
		if (ExecJson(
			TEXT("smoke-modeling-set-pivot-dynamic-custom"),
			TEXT("modeling_set_pivot"),
			FString::Printf(TEXT("{\"actor_label\":\"%s\",\"custom_point\":{\"x\":0,\"y\":0,\"z\":60,\"space\":\"component_local\"},\"readback\":true}"), *DynamicLabel),
			200,
			DynamicCustomPivotJson))
		{
			TestTrue(TEXT("Dynamic custom pivot ok"), DynamicCustomPivotJson->GetBoolField(TEXT("ok")));
			const TSharedPtr<FJsonObject>* Data = GetData(TEXT("Dynamic custom pivot"), DynamicCustomPivotJson);
			if (Data && Data->IsValid())
			{
				TestEqual(TEXT("Dynamic custom pivot backend"), (*Data)->GetStringField(TEXT("pivot_backend")), FString(TEXT("dynamic_mesh_component")));
				TestTrue(TEXT("Dynamic custom pivot accepted"), (*Data)->GetBoolField(TEXT("accepted")));
				TestTrue(TEXT("Dynamic custom pivot preserves geometry"), (*Data)->GetBoolField(TEXT("geometry_preserved")));
				ValidatePivotAfterRatio(TEXT("Dynamic custom pivot"), *Data, 0.5, 0.5, 0.5);
			}
		}
	}

	const FString VoxelLabel = FString::Printf(TEXT("UAI_SetPivot_Voxel_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	FString VoxelSessionId;
	int64 VoxelRevision = 0;
	TSharedPtr<FJsonObject> VoxelBeginJson;
	if (ExecJson(
		TEXT("smoke-modeling-voxel-pivot-begin"),
		TEXT("modeling_voxel_grid_begin"),
		FString::Printf(TEXT("{\"mode\":\"create\",\"label\":\"%s\",\"folder_path\":\"Smoke/ModelingSpatialInfo\",\"grid\":{\"origin\":{\"x\":31000,\"y\":30300,\"z\":0},\"rotation\":{\"pitch\":0,\"yaw\":0,\"roll\":0},\"cell_size\":100}}"), *VoxelLabel),
		200,
		VoxelBeginJson))
	{
		TestTrue(TEXT("Voxel pivot begin ok"), VoxelBeginJson->GetBoolField(TEXT("ok")));
		const TSharedPtr<FJsonObject>* Data = GetData(TEXT("Voxel pivot begin"), VoxelBeginJson);
		if (Data && Data->IsValid())
		{
			(*Data)->TryGetStringField(TEXT("session_id"), VoxelSessionId);
			VoxelRevision = (int64)(*Data)->GetNumberField(TEXT("revision"));
		}
	}

	if (!VoxelSessionId.IsEmpty())
	{
		TSharedPtr<FJsonObject> VoxelAddJson;
		if (ExecJson(
			TEXT("smoke-modeling-voxel-pivot-add-initial"),
			TEXT("modeling_voxel_grid_add_bounds"),
			FString::Printf(TEXT("{\"session_id\":\"%s\",\"expected_revision\":%lld,\"bounds\":{\"space\":\"grid\",\"min\":{\"x\":0,\"y\":0,\"z\":0},\"max\":{\"x\":200,\"y\":100,\"z\":100}}}"), *VoxelSessionId, VoxelRevision),
			200,
			VoxelAddJson))
		{
			TestTrue(TEXT("Voxel pivot initial add ok"), VoxelAddJson->GetBoolField(TEXT("ok")));
			const TSharedPtr<FJsonObject>* Data = GetData(TEXT("Voxel pivot initial add"), VoxelAddJson);
			if (Data && Data->IsValid())
			{
				VoxelRevision = (int64)(*Data)->GetNumberField(TEXT("revision"));
				TestEqual(TEXT("Voxel pivot initial occupied cells"), (int32)(*Data)->GetNumberField(TEXT("occupied_cell_count")), 2);
			}
		}

		TSharedPtr<FJsonObject> VoxelApplyJson;
		if (ExecJson(
			TEXT("smoke-modeling-voxel-pivot-apply-initial"),
			TEXT("modeling_voxel_grid_apply"),
			FString::Printf(TEXT("{\"session_id\":\"%s\",\"expected_revision\":%lld}"), *VoxelSessionId, VoxelRevision),
			200,
			VoxelApplyJson))
		{
			TestTrue(TEXT("Voxel pivot initial apply ok"), VoxelApplyJson->GetBoolField(TEXT("ok")));
			const TSharedPtr<FJsonObject>* Data = GetData(TEXT("Voxel pivot initial apply"), VoxelApplyJson);
			if (Data && Data->IsValid())
			{
				VoxelRevision = (int64)(*Data)->GetNumberField(TEXT("revision"));
				const TSharedPtr<FJsonObject>* Collision = nullptr;
				TestTrue(TEXT("Voxel pivot initial collision exists"), (*Data)->TryGetObjectField(TEXT("collision"), Collision) && Collision && Collision->IsValid());
				if (Collision && Collision->IsValid())
				{
					TestTrue(TEXT("Voxel pivot initial collision coverage"), (*Collision)->GetBoolField(TEXT("coverage_matches_occupied_cells")));
				}
			}
		}

		TSharedPtr<FJsonObject> VoxelPivotJson;
		if (ExecJson(
			TEXT("smoke-modeling-set-pivot-voxel-bottom"),
			TEXT("modeling_set_pivot"),
			FString::Printf(TEXT("{\"actor_label\":\"%s\",\"preset\":\"bottom\",\"readback\":true}"), *VoxelLabel),
			200,
			VoxelPivotJson))
		{
			TestTrue(TEXT("Voxel bottom pivot ok"), VoxelPivotJson->GetBoolField(TEXT("ok")));
			const TSharedPtr<FJsonObject>* Data = GetData(TEXT("Voxel bottom pivot"), VoxelPivotJson);
			if (Data && Data->IsValid())
			{
				TestEqual(TEXT("Voxel bottom pivot backend"), (*Data)->GetStringField(TEXT("pivot_backend")), FString(TEXT("voxel_grid")));
				TestTrue(TEXT("Voxel bottom pivot accepted"), (*Data)->GetBoolField(TEXT("accepted")));
				TestTrue(TEXT("Voxel bottom pivot preserves geometry"), (*Data)->GetBoolField(TEXT("geometry_preserved")));
				TestTrue(TEXT("Voxel bottom pivot preserves collision"), (*Data)->GetBoolField(TEXT("collision_preserved")));
				ValidatePivotAfterRatio(TEXT("Voxel bottom pivot"), *Data, 0.5, 0.5, 0.0);
				const TSharedPtr<FJsonObject>* AnchorAfter = nullptr;
				TestTrue(TEXT("Voxel bottom anchor_after exists"), (*Data)->TryGetObjectField(TEXT("anchor_after"), AnchorAfter) && AnchorAfter && AnchorAfter->IsValid());
				if (AnchorAfter && AnchorAfter->IsValid())
				{
					TestTrue(TEXT("Voxel bottom anchor x"), FMath::IsNearlyEqual((*AnchorAfter)->GetNumberField(TEXT("x")), 100.0, 0.01));
					TestTrue(TEXT("Voxel bottom anchor y"), FMath::IsNearlyEqual((*AnchorAfter)->GetNumberField(TEXT("y")), 50.0, 0.01));
					TestTrue(TEXT("Voxel bottom anchor z"), FMath::IsNearlyEqual((*AnchorAfter)->GetNumberField(TEXT("z")), 0.0, 0.01));
				}
				VoxelRevision = (int64)(*Data)->GetNumberField(TEXT("revision"));
			}
		}

		TSharedPtr<FJsonObject> VoxelEditJson;
		if (ExecJson(
			TEXT("smoke-modeling-voxel-pivot-edit"),
			TEXT("modeling_voxel_grid_begin"),
			FString::Printf(TEXT("{\"mode\":\"edit\",\"actor_id\":\"%s\"}"), *VoxelLabel),
			200,
			VoxelEditJson))
		{
			TestTrue(TEXT("Voxel pivot edit begin ok"), VoxelEditJson->GetBoolField(TEXT("ok")));
			const TSharedPtr<FJsonObject>* Data = GetData(TEXT("Voxel pivot edit begin"), VoxelEditJson);
			if (Data && Data->IsValid())
			{
				(*Data)->TryGetStringField(TEXT("session_id"), VoxelSessionId);
				VoxelRevision = (int64)(*Data)->GetNumberField(TEXT("revision"));
			}
		}

		TSharedPtr<FJsonObject> VoxelWorldAddJson;
		if (ExecJson(
			TEXT("smoke-modeling-voxel-pivot-add-world-after-anchor"),
			TEXT("modeling_voxel_grid_add_bounds"),
			FString::Printf(TEXT("{\"session_id\":\"%s\",\"expected_revision\":%lld,\"bounds\":{\"space\":\"world\",\"min\":{\"x\":31200,\"y\":30300,\"z\":0},\"max\":{\"x\":31300,\"y\":30400,\"z\":100}}}"), *VoxelSessionId, VoxelRevision),
			200,
			VoxelWorldAddJson))
		{
			TestTrue(TEXT("Voxel pivot world add ok"), VoxelWorldAddJson->GetBoolField(TEXT("ok")));
			const TSharedPtr<FJsonObject>* Data = GetData(TEXT("Voxel pivot world add"), VoxelWorldAddJson);
			if (Data && Data->IsValid())
			{
				VoxelRevision = (int64)(*Data)->GetNumberField(TEXT("revision"));
				TestEqual(TEXT("Voxel pivot world add occupied cells"), (int32)(*Data)->GetNumberField(TEXT("occupied_cell_count")), 3);
			}
		}

		TSharedPtr<FJsonObject> VoxelFinalApplyJson;
		if (ExecJson(
			TEXT("smoke-modeling-voxel-pivot-apply-final"),
			TEXT("modeling_voxel_grid_apply"),
			FString::Printf(TEXT("{\"session_id\":\"%s\",\"expected_revision\":%lld}"), *VoxelSessionId, VoxelRevision),
			200,
			VoxelFinalApplyJson))
		{
			TestTrue(TEXT("Voxel pivot final apply ok"), VoxelFinalApplyJson->GetBoolField(TEXT("ok")));
			const TSharedPtr<FJsonObject>* Data = GetData(TEXT("Voxel pivot final apply"), VoxelFinalApplyJson);
			if (Data && Data->IsValid())
			{
				TestEqual(TEXT("Voxel pivot final occupied cells"), (int32)(*Data)->GetNumberField(TEXT("occupied_cell_count")), 3);
				const TSharedPtr<FJsonObject>* Collision = nullptr;
				TestTrue(TEXT("Voxel pivot final collision exists"), (*Data)->TryGetObjectField(TEXT("collision"), Collision) && Collision && Collision->IsValid());
				if (Collision && Collision->IsValid())
				{
					TestTrue(TEXT("Voxel pivot final collision coverage"), (*Collision)->GetBoolField(TEXT("coverage_matches_occupied_cells")));
					TestEqual(TEXT("Voxel pivot final collision box count"), (int32)(*Collision)->GetNumberField(TEXT("box_count")), 1);
				}
				const TSharedPtr<FJsonObject>* Metadata = nullptr;
				TestTrue(TEXT("Voxel pivot final metadata exists"), (*Data)->TryGetObjectField(TEXT("metadata"), Metadata) && Metadata && Metadata->IsValid());
				if (Metadata && Metadata->IsValid())
				{
					const TSharedPtr<FJsonObject>* Anchor = nullptr;
					TestTrue(TEXT("Voxel pivot final metadata anchor exists"), (*Metadata)->TryGetObjectField(TEXT("anchor"), Anchor) && Anchor && Anchor->IsValid());
					if (Anchor && Anchor->IsValid())
					{
						TestTrue(TEXT("Voxel pivot final anchor x"), FMath::IsNearlyEqual((*Anchor)->GetNumberField(TEXT("x")), 100.0, 0.01));
						TestTrue(TEXT("Voxel pivot final anchor y"), FMath::IsNearlyEqual((*Anchor)->GetNumberField(TEXT("y")), 50.0, 0.01));
						TestTrue(TEXT("Voxel pivot final anchor z"), FMath::IsNearlyEqual((*Anchor)->GetNumberField(TEXT("z")), 0.0, 0.01));
					}
				}
			}
		}
	}

	if (AmbiguousActor && EditorWorld)
	{
		EditorWorld->DestroyActor(AmbiguousActor);
	}
	if (SharedActorA && EditorWorld)
	{
		EditorWorld->DestroyActor(SharedActorA);
	}
	if (SharedActorB && EditorWorld)
	{
		EditorWorld->DestroyActor(SharedActorB);
	}
	if (!StaticDuplicateAssetPath.IsEmpty())
	{
		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		TSharedPtr<FJsonObject> DeleteDuplicateJson;
		ExecJson(
			TEXT("smoke-modeling-set-pivot-static-delete-duplicate"),
			TEXT("asset_delete"),
			FString::Printf(TEXT("{\"asset_path\":\"%s\",\"dry_run\":false,\"allow_dirty\":true,\"delete_empty_folders\":true}"), *StaticDuplicateAssetPath),
			200,
			DeleteDuplicateJson);
		TestTrue(TEXT("Static duplicate cleanup ok"), DeleteDuplicateJson.IsValid() && DeleteDuplicateJson->GetBoolField(TEXT("ok")));
	}

	TSharedPtr<FJsonObject> DestroyDynamicJson;
	ExecJson(
		TEXT("smoke-modeling-spatial-destroy-dynamic"),
		TEXT("level_destroy_folder_actors"),
		TEXT("{\"folder_path\":\"Smoke/ModelingSpatialInfo\"}"),
		200,
		DestroyDynamicJson);

	return !HasAnyErrors();
}

#endif

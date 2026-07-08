#if WITH_DEV_AUTOMATION_TESTS

#include "../UeAgentSmokeTestSharedIncludes.inl"

#include "Components/StaticMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "PhysicsEngine/PhysicsConstraintActor.h"
#include "PhysicsEngine/PhysicsConstraintComponent.h"

namespace
{
	static bool ExecPhysicsSmokeCommand(
		FAutomationTestBase* Test,
		const FScopedUeAgentHttpServer& ServerScope,
		const FString& RequestId,
		const FString& Command,
		const FString& ParamsJson,
		const int32 ExpectedStatusCode,
		TSharedPtr<FJsonObject>& OutRoot,
		TSharedPtr<FJsonObject>& OutData,
		const double TimeoutSeconds = 30.0)
	{
		const FString Body = MakeRawExecRequestBody(RequestId, Command, ParamsJson);
		FHttpSmokeResult Result;
		const bool bRequestOk = ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, Body, Result, TimeoutSeconds);
		Test->TestTrue(FString::Printf(TEXT("%s request completed"), *RequestId), bRequestOk);
		Test->TestEqual(FString::Printf(TEXT("%s status code"), *RequestId), Result.StatusCode, ExpectedStatusCode);
		if (Result.StatusCode != ExpectedStatusCode)
		{
			Test->AddError(FString::Printf(TEXT("%s response body: %s"), *RequestId, *Result.Body));
		}
		Test->TestTrue(FString::Printf(TEXT("%s response parses as JSON"), *RequestId), ParseJson(Result.Body, OutRoot));

		const bool bExpectOk = ExpectedStatusCode >= 200 && ExpectedStatusCode < 300;
		Test->TestTrue(
			FString::Printf(TEXT("%s ok flag matches status"), *RequestId),
			OutRoot.IsValid() && OutRoot->GetBoolField(TEXT("ok")) == bExpectOk);

		OutData.Reset();
		const TSharedPtr<FJsonObject>* DataObject = nullptr;
		if (OutRoot.IsValid() && OutRoot->TryGetObjectField(TEXT("data"), DataObject) && DataObject && DataObject->IsValid())
		{
			OutData = *DataObject;
		}
		return bRequestOk && Result.StatusCode == ExpectedStatusCode && OutRoot.IsValid();
	}

	static UWorld* GetPhysicsSmokeWorld()
	{
		return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	}

	static AStaticMeshActor* SpawnPhysicsSmokeCube(UWorld* World, const FString& Label, const FVector& Location, const bool bSimulate)
	{
		if (!World)
		{
			return nullptr;
		}
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		Params.ObjectFlags |= RF_Transactional;
		AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>(AStaticMeshActor::StaticClass(), Location, FRotator::ZeroRotator, Params);
		if (!Actor)
		{
			return nullptr;
		}
		Actor->SetActorLabel(Label);
		UStaticMeshComponent* Component = Actor->GetStaticMeshComponent();
		UStaticMesh* CubeMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
		if (Component && CubeMesh)
		{
			Component->SetMobility(EComponentMobility::Movable);
			Component->SetStaticMesh(CubeMesh);
			Component->SetCollisionProfileName(bSimulate ? FName(TEXT("PhysicsActor")) : FName(TEXT("BlockAll")));
			Component->SetEnableGravity(false);
			Component->SetSimulatePhysics(bSimulate);
			if (!Component->IsRegistered())
			{
				Component->RegisterComponent();
			}
		}
		return Actor;
	}

	static AActor* FindPhysicsSmokeActor(UWorld* World, const FString& NameOrLabel)
	{
		if (!World)
		{
			return nullptr;
		}
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (!Actor)
			{
				continue;
			}
			if (Actor->GetName().Equals(NameOrLabel, ESearchCase::IgnoreCase) || Actor->GetActorLabel().Equals(NameOrLabel, ESearchCase::IgnoreCase))
			{
				return Actor;
			}
		}
		return nullptr;
	}

	static FString JsonObjectToSmokeString(const TSharedPtr<FJsonObject>& Obj)
	{
		FString JsonText;
		if (Obj.IsValid())
		{
			const TSharedRef<TJsonWriter<TCHAR>> Writer = TJsonWriterFactory<TCHAR>::Create(&JsonText);
			FJsonSerializer::Serialize(Obj.ToSharedRef(), Writer);
		}
		return JsonText;
	}

	static bool WritePhysicsSmokeJsonFile(FAutomationTestBase* Test, const FString& FilePath, const FString& JsonText)
	{
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(FilePath), true);
		const bool bSaved = FFileHelper::SaveStringToFile(JsonText, *FilePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		Test->TestTrue(FString::Printf(TEXT("write json %s"), *FilePath), bSaved);
		return bSaved;
	}

	static FString MakePhysicsSmokeFolder(const FString& Name)
	{
		return ToSmokeJsonPath(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation"), TEXT("UAIPhysics"), Name));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentInterfacePhysicsCapabilitiesSmokeTest,
	"GptProjectTest.UeAgentInterface.Smoke.Physics.BaselineCapabilities",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUeAgentInterfacePhysicsCapabilitiesSmokeTest::RunTest(const FString& Parameters)
{
	FScopedUeAgentHttpServer ServerScope;
	FString InitError;
	if (!ServerScope.Initialize(InitError))
	{
		AddError(FString::Printf(TEXT("Initialize failed: %s"), *InitError));
		return false;
	}

	TSharedPtr<FJsonObject> Root;
	TSharedPtr<FJsonObject> Data;
	TestTrue(TEXT("physics_capabilities_query succeeds"), ExecPhysicsSmokeCommand(
		this,
		ServerScope,
		TEXT("smoke-physics-capabilities-001"),
		TEXT("physics_capabilities_query"),
		TEXT("{\"include_project_settings\":true,\"include_collision_profiles\":true,\"include_optional_plugin_status\":true}"),
		200,
		Root,
		Data));
	TestTrue(TEXT("baseline physics available"), Data.IsValid() && Data->GetBoolField(TEXT("baseline_physics_available")));
	const TArray<TSharedPtr<FJsonValue>>* OptionalPlugins = nullptr;
	TestTrue(TEXT("optional plugins listed as excluded"), Data.IsValid() && Data->TryGetArrayField(TEXT("excluded_optional_plugins"), OptionalPlugins) && OptionalPlugins && OptionalPlugins->Num() > 0);
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentInterfacePhysicalMaterialSmokeTest,
	"GptProjectTest.UeAgentInterface.Smoke.Physics.PhysicalMaterialWorkflow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUeAgentInterfacePhysicalMaterialSmokeTest::RunTest(const FString& Parameters)
{
	FScopedUeAgentHttpServer ServerScope;
	FString InitError;
	if (!ServerScope.Initialize(InitError))
	{
		AddError(FString::Printf(TEXT("Initialize failed: %s"), *InitError));
		return false;
	}

	const FString MaterialPath = MakeAutomationAssetPath(TEXT("PM_PhysicsSmoke"));
	const FString ExportFile = ToSmokeJsonPath(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Automation"), TEXT("UAIPhysics"), TEXT("physical_material_export.json")));
	TSharedPtr<FJsonObject> Root;
	TSharedPtr<FJsonObject> Data;
	TestTrue(TEXT("physical_material_apply_json creates asset"), ExecPhysicsSmokeCommand(
		this,
		ServerScope,
		TEXT("smoke-physics-pm-apply-001"),
		TEXT("physical_material_apply_json"),
		FString::Printf(
			TEXT("{\"json\":{\"schema\":\"ue_agent_interface.physical_material.v1\",\"asset_path\":\"%s\",\"class\":\"PhysicalMaterial\",\"properties\":{\"friction\":0.85,\"static_friction\":0.9,\"restitution\":0.25,\"density\":1.2,\"raise_mass_to_power\":0.75,\"override_friction_combine_mode\":true,\"friction_combine_mode\":\"Max\",\"override_restitution_combine_mode\":true,\"restitution_combine_mode\":\"Average\",\"surface_type\":\"SurfaceType_Default\"}},\"create_if_missing\":true,\"save_after_apply\":false,\"strict\":true,\"backup\":false}"),
			*MaterialPath),
		200,
		Root,
		Data));
	TestTrue(TEXT("physical material readback present"), Data.IsValid() && Data->HasTypedField<EJson::Object>(TEXT("readback")));

	TestTrue(TEXT("physical_material_export_json writes file"), ExecPhysicsSmokeCommand(
		this,
		ServerScope,
		TEXT("smoke-physics-pm-export-001"),
		TEXT("physical_material_export_json"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"output_file\":\"%s\"}"), *MaterialPath, *ExportFile),
		200,
		Root,
		Data));
	TestTrue(TEXT("physical material export file exists"), IFileManager::Get().FileExists(*ExportFile));

	TestTrue(TEXT("physical_material_validate_json accepts export"), ExecPhysicsSmokeCommand(
		this,
		ServerScope,
		TEXT("smoke-physics-pm-validate-001"),
		TEXT("physical_material_validate_json"),
		FString::Printf(TEXT("{\"json_file\":\"%s\",\"strict\":true}"), *ExportFile),
		200,
		Root,
		Data));

	TestTrue(TEXT("physical_material_validate_json rejects invalid friction"), ExecPhysicsSmokeCommand(
		this,
		ServerScope,
		TEXT("smoke-physics-pm-invalid-001"),
		TEXT("physical_material_validate_json"),
		FString::Printf(TEXT("{\"json\":{\"schema\":\"ue_agent_interface.physical_material.v1\",\"asset_path\":\"%s\",\"properties\":{\"friction\":-1.0,\"surface_type\":\"SurfaceType_Default\"}},\"strict\":true}"), *MaterialPath),
		400,
		Root,
		Data));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentInterfacePhysicsComponentSmokeTest,
	"GptProjectTest.UeAgentInterface.Smoke.Physics.ComponentQueryAndPatch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUeAgentInterfacePhysicsComponentSmokeTest::RunTest(const FString& Parameters)
{
	FScopedUeAgentHttpServer ServerScope;
	FString InitError;
	if (!ServerScope.Initialize(InitError))
	{
		AddError(FString::Printf(TEXT("Initialize failed: %s"), *InitError));
		return false;
	}
	UWorld* World = GetPhysicsSmokeWorld();
	TestNotNull(TEXT("editor world"), World);
	const FString ActorLabel = FString::Printf(TEXT("UAI_PhysicsSmoke_Component_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8));
	AStaticMeshActor* Actor = SpawnPhysicsSmokeCube(World, ActorLabel, FVector(70000.0, 70000.0, 800.0), false);
	TestNotNull(TEXT("component smoke actor"), Actor);

	TSharedPtr<FJsonObject> Root;
	TSharedPtr<FJsonObject> Data;
	TestTrue(TEXT("physics_component_query reads primitive state"), ExecPhysicsSmokeCommand(
		this,
		ServerScope,
		TEXT("smoke-physics-component-query-001"),
		TEXT("physics_component_query"),
		FString::Printf(TEXT("{\"actor_id\":\"%s\",\"component\":\"StaticMeshComponent\",\"include_collision_responses\":true,\"include_material\":true,\"include_runtime_state\":true}"), *ActorLabel),
		200,
		Root,
		Data));
	TestTrue(TEXT("component query resolved primitive"), Data.IsValid() && Data->GetBoolField(TEXT("is_primitive_component")));

	TestTrue(TEXT("physics_component_validate accepts current state"), ExecPhysicsSmokeCommand(
		this,
		ServerScope,
		TEXT("smoke-physics-component-validate-001"),
		TEXT("physics_component_validate"),
		FString::Printf(TEXT("{\"actor_id\":\"%s\",\"component\":\"StaticMeshComponent\",\"expected\":{\"simulate_physics\":false,\"collision_profile_name\":\"BlockAll\",\"min_mass_kg\":0.1}}"), *ActorLabel),
		200,
		Root,
		Data));

	TestTrue(TEXT("physics_component_plan_patch emits level content patch"), ExecPhysicsSmokeCommand(
		this,
		ServerScope,
		TEXT("smoke-physics-component-plan-001"),
		TEXT("physics_component_plan_patch"),
		FString::Printf(TEXT("{\"actor_id\":\"%s\",\"component\":\"StaticMeshComponent\",\"desired\":{\"simulate_physics\":true,\"collision_profile_name\":\"PhysicsActor\",\"enable_gravity\":false,\"use_ccd\":true},\"emit_level_content_patch\":true}"), *ActorLabel),
		200,
		Root,
		Data));
	const TSharedPtr<FJsonObject>* PatchObj = nullptr;
	TestTrue(TEXT("plan patch object exists"), Data.IsValid() && Data->TryGetObjectField(TEXT("level_content_patch"), PatchObj) && PatchObj && PatchObj->IsValid());
	const FString PatchJson = JsonObjectToSmokeString(PatchObj ? *PatchObj : nullptr);
	TestFalse(TEXT("patch json serialized"), PatchJson.IsEmpty());
	TestTrue(TEXT("level_content_apply_json can apply physics patch"), ExecPhysicsSmokeCommand(
		this,
		ServerScope,
		TEXT("smoke-physics-component-level-apply-001"),
		TEXT("level_content_apply_json"),
		FString::Printf(TEXT("{\"content\":%s,\"mode\":\"patch\",\"transaction\":true,\"preflight\":true,\"validate_after_apply\":true}"), *PatchJson),
		200,
		Root,
		Data,
		60.0));

	TestTrue(TEXT("physics_component_validate sees patched state"), ExecPhysicsSmokeCommand(
		this,
		ServerScope,
		TEXT("smoke-physics-component-validate-002"),
		TEXT("physics_component_validate"),
		FString::Printf(TEXT("{\"actor_id\":\"%s\",\"component\":\"StaticMeshComponent\",\"expected\":{\"simulate_physics\":true,\"collision_profile_name\":\"PhysicsActor\"}}"), *ActorLabel),
		200,
		Root,
		Data));

	if (World && Actor)
	{
		World->DestroyActor(Actor);
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentInterfacePhysicsConstraintSmokeTest,
	"GptProjectTest.UeAgentInterface.Smoke.Physics.ConstraintWorkflow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUeAgentInterfacePhysicsConstraintSmokeTest::RunTest(const FString& Parameters)
{
	FScopedUeAgentHttpServer ServerScope;
	FString InitError;
	if (!ServerScope.Initialize(InitError))
	{
		AddError(FString::Printf(TEXT("Initialize failed: %s"), *InitError));
		return false;
	}
	UWorld* World = GetPhysicsSmokeWorld();
	TestNotNull(TEXT("editor world"), World);
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);
	const FString ActorA = TEXT("UAI_PhysicsSmoke_ConstraintA_") + Suffix;
	const FString ActorB = TEXT("UAI_PhysicsSmoke_ConstraintB_") + Suffix;
	const FString ConstraintActor = TEXT("UAI_PhysicsSmoke_Constraint_") + Suffix;
	AStaticMeshActor* CubeA = SpawnPhysicsSmokeCube(World, ActorA, FVector(70200.0, 70000.0, 800.0), true);
	AStaticMeshActor* CubeB = SpawnPhysicsSmokeCube(World, ActorB, FVector(70400.0, 70000.0, 800.0), true);
	TestNotNull(TEXT("constraint cube A"), CubeA);
	TestNotNull(TEXT("constraint cube B"), CubeB);

	TSharedPtr<FJsonObject> Root;
	TSharedPtr<FJsonObject> Data;
	const FString ConstraintJson = FString::Printf(
		TEXT("{\"schema\":\"ue_agent_interface.physics_constraint.v1\",\"target\":{\"actor_id\":\"%s\",\"component\":\"ConstraintComp\",\"location\":[70300,70000,820]},\"constrained\":{\"component1\":{\"actor_id\":\"%s\",\"component\":\"StaticMeshComponent\"},\"component2\":{\"actor_id\":\"%s\",\"component\":\"StaticMeshComponent\"}},\"constraint\":{\"disable_collision\":true,\"linear\":{\"x_motion\":\"Locked\",\"y_motion\":\"Locked\",\"z_motion\":\"Locked\",\"limit\":0},\"angular\":{\"swing1_motion\":\"Limited\",\"swing1_limit_degrees\":45,\"swing2_motion\":\"Locked\",\"twist_motion\":\"Locked\"},\"projection\":{\"enabled\":true}}}"),
		*ConstraintActor,
		*ActorA,
		*ActorB);
	TestTrue(TEXT("physics_constraint_validate_json accepts endpoints"), ExecPhysicsSmokeCommand(
		this,
		ServerScope,
		TEXT("smoke-physics-constraint-validate-001"),
		TEXT("physics_constraint_validate_json"),
		FString::Printf(TEXT("{\"json\":%s,\"strict\":true}"), *ConstraintJson),
		200,
		Root,
		Data));
	TestTrue(TEXT("physics_constraint_apply_json creates constraint actor"), ExecPhysicsSmokeCommand(
		this,
		ServerScope,
		TEXT("smoke-physics-constraint-apply-001"),
		TEXT("physics_constraint_apply_json"),
		FString::Printf(TEXT("{\"json\":%s,\"create_if_missing\":true,\"strict\":true}"), *ConstraintJson),
		200,
		Root,
		Data));
	TestTrue(TEXT("constraint apply reports endpoint binding"), Data.IsValid() && Data->GetBoolField(TEXT("set_constrained_components_applied")));

	TestTrue(TEXT("physics_constraint_export_json reads constraint"), ExecPhysicsSmokeCommand(
		this,
		ServerScope,
		TEXT("smoke-physics-constraint-export-001"),
		TEXT("physics_constraint_export_json"),
		FString::Printf(TEXT("{\"actor_id\":\"%s\",\"component\":\"ConstraintComp\",\"include_connected_bodies\":true,\"include_profile\":true}"), *ConstraintActor),
		200,
		Root,
		Data));
	TestTrue(TEXT("constraint export resolved"), Data.IsValid() && Data->GetBoolField(TEXT("resolved")));

	const FString InvalidConstraintJson = ConstraintJson.Replace(TEXT("\"x_motion\":\"Locked\""), TEXT("\"x_motion\":\"Teleport\""));
	TestTrue(TEXT("physics_constraint_validate_json rejects invalid motion"), ExecPhysicsSmokeCommand(
		this,
		ServerScope,
		TEXT("smoke-physics-constraint-invalid-001"),
		TEXT("physics_constraint_validate_json"),
		FString::Printf(TEXT("{\"json\":%s,\"strict\":true}"), *InvalidConstraintJson),
		400,
		Root,
		Data));

	if (World)
	{
		if (CubeA) { World->DestroyActor(CubeA); }
		if (CubeB) { World->DestroyActor(CubeB); }
		if (AActor* CreatedConstraint = FindPhysicsSmokeActor(World, ConstraintActor))
		{
			World->DestroyActor(CreatedConstraint);
		}
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentInterfacePhysicsAssetSmokeTest,
	"GptProjectTest.UeAgentInterface.Smoke.Physics.PhysicsAssetWorkflow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUeAgentInterfacePhysicsAssetSmokeTest::RunTest(const FString& Parameters)
{
	FScopedUeAgentHttpServer ServerScope;
	FString InitError;
	if (!ServerScope.Initialize(InitError))
	{
		AddError(FString::Printf(TEXT("Initialize failed: %s"), *InitError));
		return false;
	}
	USkeletalMesh* SkeletalMesh = LoadObject<USkeletalMesh>(nullptr, TEXT("/Engine/EngineMeshes/SkeletalCube.SkeletalCube"));
	TestNotNull(TEXT("SkeletalCube mesh exists"), SkeletalMesh);
	if (!SkeletalMesh || SkeletalMesh->GetRefSkeleton().GetNum() <= 0)
	{
		return false;
	}
	const FString BoneName = SkeletalMesh->GetRefSkeleton().GetBoneName(0).ToString();
	const FString AssetPath = MakeAutomationAssetPath(TEXT("PA_PhysicsSmoke"));
	const FString FolderPath = MakePhysicsSmokeFolder(TEXT("PhysicsAssetWorkflow"));
	WritePhysicsSmokeJsonFile(
		this,
		FPaths::Combine(FolderPath, TEXT("asset.json")),
		FString::Printf(TEXT("{\"schema\":\"ue_agent_interface.physics_asset.v1\",\"asset_path\":\"%s\",\"skeletal_mesh\":\"/Engine/EngineMeshes/SkeletalCube.SkeletalCube\",\"skeleton\":\"\"}"), *AssetPath));
	WritePhysicsSmokeJsonFile(
		this,
		FPaths::Combine(FolderPath, TEXT("bodies.json")),
		FString::Printf(TEXT("{\"replace_all\":true,\"bodies\":[{\"bone_name\":\"%s\",\"body_name\":\"%s\",\"physics_type\":\"Default\",\"primitive_shapes\":[{\"type\":\"sphere\",\"radius\":32,\"transform\":{\"location\":[0,0,0],\"rotation\":[0,0,0],\"scale\":[1,1,1]}},{\"type\":\"box\",\"x\":40,\"y\":30,\"z\":20,\"transform\":{\"location\":[5,0,0],\"rotation\":[0,0,0],\"scale\":[1,1,1]}}]}]}"), *BoneName, *BoneName));
	WritePhysicsSmokeJsonFile(this, FPaths::Combine(FolderPath, TEXT("constraints.json")), TEXT("{\"replace_all\":true,\"constraints\":[]}"));
	WritePhysicsSmokeJsonFile(this, FPaths::Combine(FolderPath, TEXT("profiles.json")), TEXT("{\"constraint_profiles\":[\"SmokeConstraintProfile\"],\"physical_animation_profiles\":[\"SmokePhysicalProfile\"]}"));

	TSharedPtr<FJsonObject> Root;
	TSharedPtr<FJsonObject> Data;
	TestTrue(TEXT("physics_asset_validate_folder accepts body json"), ExecPhysicsSmokeCommand(
		this,
		ServerScope,
		TEXT("smoke-physics-asset-validate-001"),
		TEXT("physics_asset_validate_folder"),
		FString::Printf(TEXT("{\"folder_path\":\"%s\",\"allow_destructive_replace\":true,\"strict\":true}"), *FolderPath),
		200,
		Root,
		Data));
	TestTrue(TEXT("physics_asset_apply_folder creates asset"), ExecPhysicsSmokeCommand(
		this,
		ServerScope,
		TEXT("smoke-physics-asset-apply-001"),
		TEXT("physics_asset_apply_folder"),
		FString::Printf(TEXT("{\"folder_path\":\"%s\",\"create_if_missing\":true,\"allow_destructive_replace\":true,\"save_after_apply\":false,\"backup\":false,\"strict\":true}"), *FolderPath),
		200,
		Root,
		Data,
		60.0));
	TestTrue(TEXT("physics asset readback has body"), Data.IsValid() && Data->HasTypedField<EJson::Object>(TEXT("readback")));

	const FString ExportFolder = MakePhysicsSmokeFolder(TEXT("PhysicsAssetExport"));
	TestTrue(TEXT("physics_asset_export_folder writes roundtrip folder"), ExecPhysicsSmokeCommand(
		this,
		ServerScope,
		TEXT("smoke-physics-asset-export-001"),
		TEXT("physics_asset_export_folder"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"folder_path\":\"%s\"}"), *AssetPath, *ExportFolder),
		200,
		Root,
		Data,
		60.0));
	TestTrue(TEXT("physics asset export folder has asset json"), IFileManager::Get().FileExists(*FPaths::Combine(ExportFolder, TEXT("asset.json"))));
	TSharedPtr<FJsonObject> ExportedAssetJson;
	TestTrue(TEXT("physics asset export asset json loads"), LoadSmokeJsonFile(this, FPaths::Combine(ExportFolder, TEXT("asset.json")), ExportedAssetJson, TEXT("PhysicsAsset folder asset.json")));
	TSharedPtr<FJsonObject> PhysicsAssetDetailsJson;
	TestTrue(TEXT("physics asset export asset details grouped"), SmokeLoadGroupedDetailsForRef(this, FPaths::Combine(ExportFolder, TEXT("asset.json")), ExportedAssetJson, TEXT("details"), TEXT("physics_asset"), PhysicsAssetDetailsJson));
	TestFalse(TEXT("physics asset export omits root raw_properties.json"), IFileManager::Get().FileExists(*FPaths::Combine(ExportFolder, TEXT("raw_properties.json"))));
	TestFalse(TEXT("physics asset export omits root readonly_properties.json"), IFileManager::Get().FileExists(*FPaths::Combine(ExportFolder, TEXT("readonly_properties.json"))));
	FString ExportedBodiesText;
	TSharedPtr<FJsonObject> ExportedBodiesJson;
	TestTrue(TEXT("physics asset export bodies json loads"), FFileHelper::LoadFileToString(ExportedBodiesText, *FPaths::Combine(ExportFolder, TEXT("bodies.json"))) && ParseJson(ExportedBodiesText, ExportedBodiesJson));
	if (ExportedBodiesJson.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Bodies = nullptr;
		bool bFoundBodyAuthoringMetadata = false;
		if (ExportedBodiesJson->TryGetArrayField(TEXT("bodies"), Bodies) && Bodies)
		{
			for (const TSharedPtr<FJsonValue>& BodyValue : *Bodies)
			{
				const TSharedPtr<FJsonObject>* BodyObj = nullptr;
				if (BodyValue.IsValid() && BodyValue->TryGetObject(BodyObj) && BodyObj && (*BodyObj)->HasTypedField<EJson::Object>(TEXT("selector")) && (*BodyObj)->HasTypedField<EJson::Array>(TEXT("operations")))
				{
					bFoundBodyAuthoringMetadata = true;
					break;
				}
			}
		}
		TestTrue(TEXT("physics asset exported body exposes authoring metadata"), bFoundBodyAuthoringMetadata);
	}

	WritePhysicsSmokeJsonFile(
		this,
		FPaths::Combine(FolderPath, TEXT("bodies.json")),
		FString::Printf(TEXT("{\"replace_all\":true,\"geometry_collection\":{\"apply\":true},\"bodies\":[{\"bone_name\":\"%s\",\"primitive_shapes\":[{\"type\":\"sphere\",\"radius\":32}]}]}"), *BoneName));
	TestTrue(TEXT("physics_asset_validate_folder rejects optional plugin field"), ExecPhysicsSmokeCommand(
		this,
		ServerScope,
		TEXT("smoke-physics-asset-optional-guard-001"),
		TEXT("physics_asset_validate_folder"),
		FString::Printf(TEXT("{\"folder_path\":\"%s\",\"allow_destructive_replace\":true,\"strict\":true}"), *FolderPath),
		400,
		Root,
		Data));
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentInterfacePhysicsRuntimeProbeSmokeTest,
	"GptProjectTest.UeAgentInterface.Smoke.Physics.RuntimeProbe",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUeAgentInterfacePhysicsRuntimeProbeSmokeTest::RunTest(const FString& Parameters)
{
	FScopedUeAgentHttpServer ServerScope;
	FString InitError;
	if (!ServerScope.Initialize(InitError))
	{
		AddError(FString::Printf(TEXT("Initialize failed: %s"), *InitError));
		return false;
	}
	UWorld* World = GetPhysicsSmokeWorld();
	TestNotNull(TEXT("editor world"), World);
	const FString ActorLabel = FString::Printf(TEXT("UAI_PhysicsSmoke_Runtime_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8));
	AStaticMeshActor* Actor = SpawnPhysicsSmokeCube(World, ActorLabel, FVector(70600.0, 70000.0, 1000.0), true);
	TestNotNull(TEXT("runtime smoke actor"), Actor);

	TSharedPtr<FJsonObject> Root;
	TSharedPtr<FJsonObject> Data;
	TestTrue(TEXT("physics_runtime_probe observes impulse velocity and restores actor"), ExecPhysicsSmokeCommand(
		this,
		ServerScope,
		TEXT("smoke-physics-runtime-probe-001"),
		TEXT("physics_runtime_probe"),
		FString::Printf(TEXT("{\"world\":\"editor\",\"duration_seconds\":0.1,\"fixed_delta_seconds\":0.0166667,\"targets\":[{\"actor_id\":\"%s\",\"component\":\"StaticMeshComponent\",\"initial_impulse\":[0,0,250],\"initial_velocity\":[0,0,250]}],\"expect\":[{\"actor_id\":\"%s\",\"component\":\"StaticMeshComponent\",\"min_velocity_size\":1.0}],\"cleanup\":\"restore_transforms\"}"), *ActorLabel, *ActorLabel),
		200,
		Root,
		Data,
		60.0));
	TestTrue(TEXT("runtime probe restored state"), Data.IsValid() && Data->GetBoolField(TEXT("restored")));
	if (World && Actor)
	{
		World->DestroyActor(Actor);
	}
	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentInterfacePhysicsOptionalPluginGuardSmokeTest,
	"GptProjectTest.UeAgentInterface.Smoke.Physics.OptionalPluginGuards",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUeAgentInterfacePhysicsOptionalPluginGuardSmokeTest::RunTest(const FString& Parameters)
{
	FScopedUeAgentHttpServer ServerScope;
	FString InitError;
	if (!ServerScope.Initialize(InitError))
	{
		AddError(FString::Printf(TEXT("Initialize failed: %s"), *InitError));
		return false;
	}
	UWorld* World = GetPhysicsSmokeWorld();
	TestNotNull(TEXT("editor world"), World);
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);
	const FString ActorA = TEXT("UAI_PhysicsSmoke_OptionalA_") + Suffix;
	const FString ActorB = TEXT("UAI_PhysicsSmoke_OptionalB_") + Suffix;
	AStaticMeshActor* CubeA = SpawnPhysicsSmokeCube(World, ActorA, FVector(70800.0, 70000.0, 800.0), true);
	AStaticMeshActor* CubeB = SpawnPhysicsSmokeCube(World, ActorB, FVector(71000.0, 70000.0, 800.0), true);
	TestNotNull(TEXT("optional guard cube A"), CubeA);
	TestNotNull(TEXT("optional guard cube B"), CubeB);

	TSharedPtr<FJsonObject> Root;
	TSharedPtr<FJsonObject> Data;
	const FString OptionalConstraintJson = FString::Printf(
		TEXT("{\"schema\":\"ue_agent_interface.physics_constraint.v1\",\"target\":{\"actor_id\":\"UAI_PhysicsSmoke_OptionalConstraint_%s\"},\"constrained\":{\"component1\":{\"actor_id\":\"%s\",\"component\":\"StaticMeshComponent\"},\"component2\":{\"actor_id\":\"%s\",\"component\":\"StaticMeshComponent\"}},\"constraint\":{\"vehicle_drive\":{\"apply\":true},\"linear\":{\"x_motion\":\"Locked\",\"y_motion\":\"Locked\",\"z_motion\":\"Locked\"}}}"),
		*Suffix,
		*ActorA,
		*ActorB);
	TestTrue(TEXT("physics_constraint_validate_json rejects optional vehicle field"), ExecPhysicsSmokeCommand(
		this,
		ServerScope,
		TEXT("smoke-physics-optional-constraint-001"),
		TEXT("physics_constraint_validate_json"),
		FString::Printf(TEXT("{\"json\":%s,\"strict\":true}"), *OptionalConstraintJson),
		400,
		Root,
		Data));
	if (World)
	{
		if (CubeA) { World->DestroyActor(CubeA); }
		if (CubeB) { World->DestroyActor(CubeB); }
	}
	return !HasAnyErrors();
}

#endif

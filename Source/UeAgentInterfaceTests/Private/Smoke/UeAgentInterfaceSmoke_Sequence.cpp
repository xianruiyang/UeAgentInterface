#if WITH_DEV_AUTOMATION_TESTS

#include "UeAgentSmokeTestSharedIncludes.inl"

#include "MovieSceneTrack.h"
#include "Sequencer/MovieSceneControlRigParameterSection.h"
#include "Sequencer/MovieSceneControlRigParameterTrack.h"

namespace
{
	static bool ExecuteSequenceCommandExpectFailure(
		FAutomationTestBase* Test,
		FScopedUeAgentHttpServer& ServerScope,
		const FString& RequestId,
		const FString& Command,
		const FString& ParamsJson,
		const FString& ExpectedError)
	{
		FHttpSmokeResult Result;
		const FString Body = MakeExecRequestBody(RequestId, Command, ParamsJson);
		if (!Test->TestTrue(Command + TEXT(" failure request completed"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, Body, Result)))
		{
			return false;
		}
		if (!Test->TestTrue(Command + TEXT(" failure http status"), Result.StatusCode == 200 || Result.StatusCode == 400))
		{
			Test->AddError(Result.Body.Left(1024));
			return false;
		}

		TSharedPtr<FJsonObject> Root;
		if (!Test->TestTrue(Command + TEXT(" failure response parses"), ParseJson(Result.Body, Root)) || !Root.IsValid())
		{
			return false;
		}
		bool bOk = true;
		Root->TryGetBoolField(TEXT("ok"), bOk);
		if (!Test->TestFalse(Command + TEXT(" failure ok=false"), bOk))
		{
			Test->AddError(Result.Body.Left(1024));
			return false;
		}

		FString Error;
		Root->TryGetStringField(TEXT("error"), Error);
		if (!Test->TestEqual(Command + TEXT(" failure error"), Error, ExpectedError))
		{
			Test->AddError(Result.Body.Left(1024));
			return false;
		}
		return true;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentInterfaceSequenceSmokeTest,
	"GptProjectTest.UeAgentInterface.Smoke.SequenceWorkflow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUeAgentInterfaceSequenceSmokeTest::RunTest(const FString& Parameters)
{
	FScopedUeAgentHttpServer ServerScope;
	FString InitError;
	if (!ServerScope.Initialize(InitError))
	{
		AddError(FString::Printf(TEXT("Initialize failed: %s"), *InitError));
		return false;
	}

	const FString RootPath = TEXT("/Game/__UeAgentInterfaceSmoke");
	const FString SequenceAssetPath = MakeAutomationAssetPath(TEXT("SEQ"));

	const FString CreateBody = MakeExecRequestBody(
		TEXT("smoke-sequence-create-001"),
		TEXT("sequence_create_level_sequence"),
		FString::Printf(
			TEXT("{\"asset_path\":\"%s\",\"start_seconds\":0.5,\"duration_seconds\":3.0,\"display_rate_num\":24,\"display_rate_den\":1,\"open_editor\":false,\"save_after_create\":false}"),
			*SequenceAssetPath));
	FHttpSmokeResult CreateResult;
	TestTrue(TEXT("Sequence create request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, CreateBody, CreateResult));
	TestEqual(TEXT("Sequence create status code"), CreateResult.StatusCode, 200);

	TSharedPtr<FJsonObject> CreateJson;
	TestTrue(TEXT("Sequence create response parses as JSON"), ParseJson(CreateResult.Body, CreateJson));
	TestTrue(TEXT("Sequence create root ok=true"), CreateJson.IsValid() && CreateJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* CreateData = nullptr;
	TestTrue(TEXT("Sequence create contains data object"), CreateJson.IsValid() && CreateJson->TryGetObjectField(TEXT("data"), CreateData) && CreateData && CreateData->IsValid());
	if (CreateData && CreateData->IsValid())
	{
		TestEqual(TEXT("Sequence create asset path"), (*CreateData)->GetStringField(TEXT("asset_path")), SequenceAssetPath);
		TestEqual(TEXT("Sequence create display rate"), (*CreateData)->GetStringField(TEXT("display_rate")), FString(TEXT("24/1")));
	}

	const FString GuardedCreateAssetPath = MakeAutomationAssetPath(TEXT("SEQOPENBLOCK"));
	TestTrue(
		TEXT("Sequence create open_editor requires explicit allow"),
		ExecuteSequenceCommandExpectFailure(
			this,
			ServerScope,
			TEXT("smoke-sequence-create-open-editor-denied-001"),
			TEXT("sequence_create_level_sequence"),
			FString::Printf(TEXT("{\"asset_path\":\"%s\",\"start_seconds\":0.0,\"duration_seconds\":1.0,\"open_editor\":true,\"save_after_create\":false}"), *GuardedCreateAssetPath),
			TEXT("open_editor_requires_allow_editor_open")));

	TestTrue(
		TEXT("Sequence open requires explicit allow"),
		ExecuteSequenceCommandExpectFailure(
			this,
			ServerScope,
			TEXT("smoke-sequence-open-denied-001"),
			TEXT("sequence_open_level_sequence"),
			FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *SequenceAssetPath),
			TEXT("sequence_open_level_sequence_requires_allow_editor_open")));

	const FString ListBody = MakeExecRequestBody(
		TEXT("smoke-sequence-list-001"),
		TEXT("sequence_list_level_sequences"),
		FString::Printf(TEXT("{\"root_path\":\"%s\",\"limit\":32}"), *RootPath));
	FHttpSmokeResult ListResult;
	TestTrue(TEXT("Sequence list request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ListBody, ListResult));
	TestEqual(TEXT("Sequence list status code"), ListResult.StatusCode, 200);

	TSharedPtr<FJsonObject> ListJson;
	TestTrue(TEXT("Sequence list response parses as JSON"), ParseJson(ListResult.Body, ListJson));
	TestTrue(TEXT("Sequence list root ok=true"), ListJson.IsValid() && ListJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* ListData = nullptr;
	TestTrue(TEXT("Sequence list contains data object"), ListJson.IsValid() && ListJson->TryGetObjectField(TEXT("data"), ListData) && ListData && ListData->IsValid());
	if (ListData && ListData->IsValid())
	{
		TestEqual(TEXT("Sequence list root path"), (*ListData)->GetStringField(TEXT("root_path")), RootPath);
		TestTrue(TEXT("Sequence list returned_count >= 1"), static_cast<int32>((*ListData)->GetIntegerField(TEXT("returned_count"))) >= 1);
	}

	const FString GetInfoBody = MakeExecRequestBody(
		TEXT("smoke-sequence-info-001"),
		TEXT("sequence_get_level_sequence_info"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *SequenceAssetPath));
	FHttpSmokeResult GetInfoResult;
	TestTrue(TEXT("Sequence get_info request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, GetInfoBody, GetInfoResult));
	TestEqual(TEXT("Sequence get_info status code"), GetInfoResult.StatusCode, 200);

	TSharedPtr<FJsonObject> GetInfoJson;
	TestTrue(TEXT("Sequence get_info response parses as JSON"), ParseJson(GetInfoResult.Body, GetInfoJson));
	TestTrue(TEXT("Sequence get_info root ok=true"), GetInfoJson.IsValid() && GetInfoJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* GetInfoData = nullptr;
	TestTrue(TEXT("Sequence get_info contains data object"), GetInfoJson.IsValid() && GetInfoJson->TryGetObjectField(TEXT("data"), GetInfoData) && GetInfoData && GetInfoData->IsValid());
	if (GetInfoData && GetInfoData->IsValid())
	{
		TestTrue(TEXT("Sequence get_info has display rate"), !(*GetInfoData)->GetStringField(TEXT("display_rate")).IsEmpty());
		TestEqual(TEXT("Sequence get_info binding count is zero"), static_cast<int32>((*GetInfoData)->GetIntegerField(TEXT("binding_count"))), 0);
	}

	const FString SetRangeBody = MakeExecRequestBody(
		TEXT("smoke-sequence-range-001"),
		TEXT("sequence_set_level_sequence_playback_range"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"start_seconds\":1.25,\"duration_seconds\":2.5,\"save_after_set\":false}"), *SequenceAssetPath));
	FHttpSmokeResult SetRangeResult;
	TestTrue(TEXT("Sequence set playback range request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SetRangeBody, SetRangeResult));
	TestEqual(TEXT("Sequence set playback range status code"), SetRangeResult.StatusCode, 200);

	TSharedPtr<FJsonObject> SetRangeJson;
	TestTrue(TEXT("Sequence set playback range response parses as JSON"), ParseJson(SetRangeResult.Body, SetRangeJson));
	TestTrue(TEXT("Sequence set playback range root ok=true"), SetRangeJson.IsValid() && SetRangeJson->GetBoolField(TEXT("ok")));

	const FString SetDisplayRateBody = MakeExecRequestBody(
		TEXT("smoke-sequence-rate-001"),
		TEXT("sequence_set_level_sequence_display_rate"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"display_rate_num\":12,\"display_rate_den\":1,\"save_after_set\":false}"), *SequenceAssetPath));
	FHttpSmokeResult SetDisplayRateResult;
	TestTrue(TEXT("Sequence set display rate request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SetDisplayRateBody, SetDisplayRateResult));
	TestEqual(TEXT("Sequence set display rate status code"), SetDisplayRateResult.StatusCode, 200);

	TSharedPtr<FJsonObject> SetDisplayRateJson;
	TestTrue(TEXT("Sequence set display rate response parses as JSON"), ParseJson(SetDisplayRateResult.Body, SetDisplayRateJson));
	TestTrue(TEXT("Sequence set display rate root ok=true"), SetDisplayRateJson.IsValid() && SetDisplayRateJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* SetDisplayRateData = nullptr;
	TestTrue(TEXT("Sequence set display rate contains data object"), SetDisplayRateJson.IsValid() && SetDisplayRateJson->TryGetObjectField(TEXT("data"), SetDisplayRateData) && SetDisplayRateData && SetDisplayRateData->IsValid());
	if (SetDisplayRateData && SetDisplayRateData->IsValid())
	{
		TestEqual(TEXT("Sequence set display rate response"), (*SetDisplayRateData)->GetStringField(TEXT("display_rate")), FString(TEXT("12/1")));
	}

	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentInterfaceSequenceFolderSmokeTest,
	"GptProjectTest.UeAgentInterface.Smoke.SequenceFolderWorkflow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUeAgentInterfaceSequenceFolderSmokeTest::RunTest(const FString& Parameters)
{
	FScopedUeAgentHttpServer ServerScope;
	FString InitError;
	if (!ServerScope.Initialize(InitError))
	{
		AddError(FString::Printf(TEXT("Initialize failed: %s"), *InitError));
		return false;
	}

	const FString SequenceAssetPath = MakeAutomationAssetPath(TEXT("SEQFOLDER"));
	const FString ActorLabel = FString::Printf(TEXT("SmokeSeqActor_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8));
	const FString CameraActorLabel = FString::Printf(TEXT("SmokeSeqCamera_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8));
	const FString SkeletalActorLabel = FString::Printf(TEXT("SmokeSeqSkeletal_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8));
	const FString ChildSequenceAssetPath = MakeAutomationAssetPath(TEXT("SEQCHILD"));
	const FString FolderRoot = FPaths::ConvertRelativePathToFull(
		FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UeAssetFolders"), TEXT("LevelSequence")));
	const FString FolderPath = FPaths::Combine(FolderRoot, SequenceAssetPath.RightChop(1));

	auto LoadJsonFile = [this](const FString& FilePath, TSharedPtr<FJsonObject>& OutObj) -> bool
	{
		FString JsonText;
		if (!FFileHelper::LoadFileToString(JsonText, *FilePath))
		{
			AddError(FString::Printf(TEXT("Load file failed: %s"), *FilePath));
			return false;
		}
		const TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(JsonText);
		if (!FJsonSerializer::Deserialize(Reader, OutObj) || !OutObj.IsValid())
		{
			AddError(FString::Printf(TEXT("Parse json failed: %s"), *FilePath));
			return false;
		}
		return true;
	};

	auto SaveJsonFile = [this](const FString& FilePath, const TSharedPtr<FJsonObject>& Obj) -> bool
	{
		FString JsonText;
		const TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&JsonText);
		if (!FJsonSerializer::Serialize(Obj.ToSharedRef(), Writer))
		{
			AddError(FString::Printf(TEXT("Serialize json failed: %s"), *FilePath));
			return false;
		}
		if (!FFileHelper::SaveStringToFile(JsonText, *FilePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			AddError(FString::Printf(TEXT("Save file failed: %s"), *FilePath));
			return false;
		}
		return true;
	};

	const FString SpawnActorBody = MakeExecRequestBody(
		TEXT("smoke-sequence-folder-spawn-001"),
		TEXT("spawn_actor"),
		FString::Printf(TEXT("{\"class_path\":\"/Script/Engine.StaticMeshActor\",\"label\":\"%s\",\"static_mesh\":\"/Engine/BasicShapes/Cube.Cube\",\"location\":{\"x\":0,\"y\":0,\"z\":120}}"), *ActorLabel));
	FHttpSmokeResult SpawnActorResult;
	TestTrue(TEXT("Sequence folder spawn actor request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SpawnActorBody, SpawnActorResult));
	TestEqual(TEXT("Sequence folder spawn actor status code"), SpawnActorResult.StatusCode, 200);

	const FString CreateSequenceBody = MakeExecRequestBody(
		TEXT("smoke-sequence-folder-create-001"),
		TEXT("sequence_create_level_sequence"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"start_seconds\":0.0,\"duration_seconds\":3.0,\"display_rate_num\":24,\"display_rate_den\":1,\"open_editor\":false,\"save_after_create\":false}"), *SequenceAssetPath));
	FHttpSmokeResult CreateSequenceResult;
	TestTrue(TEXT("Sequence folder create request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, CreateSequenceBody, CreateSequenceResult));
	TestEqual(TEXT("Sequence folder create status code"), CreateSequenceResult.StatusCode, 200);

	const FString CreateChildSequenceBody = MakeExecRequestBody(
		TEXT("smoke-sequence-folder-create-child-001"),
		TEXT("sequence_create_level_sequence"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"start_seconds\":0.0,\"duration_seconds\":1.0,\"display_rate_num\":24,\"display_rate_den\":1,\"open_editor\":false,\"save_after_create\":false}"), *ChildSequenceAssetPath));
	FHttpSmokeResult CreateChildSequenceResult;
	TestTrue(TEXT("Sequence folder create child request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, CreateChildSequenceBody, CreateChildSequenceResult));
	TestEqual(TEXT("Sequence folder create child status code"), CreateChildSequenceResult.StatusCode, 200);

	UWorld* DirectEditorWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	TestNotNull(TEXT("Sequence folder direct editor world available for camera"), DirectEditorWorld);
	ACameraActor* DirectCameraActor = nullptr;
	if (DirectEditorWorld)
	{
		FActorSpawnParameters SpawnParams;
		SpawnParams.ObjectFlags = RF_Transient;
		SpawnParams.bHideFromSceneOutliner = false;
		SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		DirectCameraActor = DirectEditorWorld->SpawnActor<ACameraActor>(ACameraActor::StaticClass(), FVector(300.0, 0.0, 120.0), FRotator(-10.0, 180.0, 0.0), SpawnParams);
		if (DirectCameraActor)
		{
			DirectCameraActor->SetActorLabel(CameraActorLabel);
		}
	}
	TestNotNull(TEXT("Sequence folder direct camera actor"), DirectCameraActor);

	USkeletalMesh* SmokeSkeletalMesh = LoadObject<USkeletalMesh>(nullptr, TEXT("/Engine/EngineMeshes/SkeletalCube.SkeletalCube"));
	TestNotNull(TEXT("Sequence folder smoke skeletal mesh"), SmokeSkeletalMesh);
	FString ControlRigSmokeControlName;
	FString ControlRigSmokeBoneName;
	if (SmokeSkeletalMesh && SmokeSkeletalMesh->GetRefSkeleton().GetNum() > 0)
	{
		ControlRigSmokeBoneName = SmokeSkeletalMesh->GetRefSkeleton().GetBoneName(0).ToString();
		ControlRigSmokeControlName = ControlRigSmokeBoneName.ToLower() + TEXT("_CONTROL");
	}
	TestTrue(TEXT("Sequence folder control rig smoke control name non-empty"), !ControlRigSmokeControlName.IsEmpty());
	TestTrue(TEXT("Sequence folder control rig smoke bone name non-empty"), !ControlRigSmokeBoneName.IsEmpty());

	ASkeletalMeshActor* DirectSkeletalActor = nullptr;
	if (DirectEditorWorld && SmokeSkeletalMesh)
	{
		FActorSpawnParameters SpawnParams;
		SpawnParams.ObjectFlags = RF_Transient;
		SpawnParams.bHideFromSceneOutliner = false;
		SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		DirectSkeletalActor = DirectEditorWorld->SpawnActor<ASkeletalMeshActor>(ASkeletalMeshActor::StaticClass(), FVector(120.0, 0.0, 120.0), FRotator::ZeroRotator, SpawnParams);
		if (DirectSkeletalActor)
		{
			DirectSkeletalActor->SetActorLabel(SkeletalActorLabel);
			if (USkeletalMeshComponent* SkeletalMeshComponent = DirectSkeletalActor->GetSkeletalMeshComponent())
			{
				SkeletalMeshComponent->SetSkeletalMesh(SmokeSkeletalMesh);
			}
		}
	}
	TestNotNull(TEXT("Sequence folder direct skeletal actor"), DirectSkeletalActor);

	const FString AddBindingBody = MakeExecRequestBody(
		TEXT("smoke-sequence-folder-bind-001"),
		TEXT("sequence_add_actor_binding"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"actor_id\":\"%s\",\"save_after_set\":false}"), *SequenceAssetPath, *ActorLabel));
	FHttpSmokeResult AddBindingResult;
	TestTrue(TEXT("Sequence folder add binding request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddBindingBody, AddBindingResult));
	TestEqual(TEXT("Sequence folder add binding status code"), AddBindingResult.StatusCode, 200);

	TSharedPtr<FJsonObject> AddBindingJson;
	TestTrue(TEXT("Sequence folder add binding parses JSON"), ParseJson(AddBindingResult.Body, AddBindingJson));
	const TSharedPtr<FJsonObject>* AddBindingData = nullptr;
	TestTrue(TEXT("Sequence folder add binding contains data"), AddBindingJson.IsValid() && AddBindingJson->TryGetObjectField(TEXT("data"), AddBindingData) && AddBindingData && AddBindingData->IsValid());
	const FString BindingGuid = (AddBindingData && AddBindingData->IsValid()) ? (*AddBindingData)->GetStringField(TEXT("binding_guid")) : FString();
	TestTrue(TEXT("Sequence folder binding guid non-empty"), !BindingGuid.IsEmpty());

	const FString AddCameraBindingBody = MakeExecRequestBody(
		TEXT("smoke-sequence-folder-bind-camera-001"),
		TEXT("sequence_add_actor_binding"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"actor_id\":\"%s\",\"save_after_set\":false}"), *SequenceAssetPath, *CameraActorLabel));
	FHttpSmokeResult AddCameraBindingResult;
	TestTrue(TEXT("Sequence folder add camera binding request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddCameraBindingBody, AddCameraBindingResult));
	TestEqual(TEXT("Sequence folder add camera binding status code"), AddCameraBindingResult.StatusCode, 200);

	TSharedPtr<FJsonObject> AddCameraBindingJson;
	TestTrue(TEXT("Sequence folder add camera binding parses JSON"), ParseJson(AddCameraBindingResult.Body, AddCameraBindingJson));
	const TSharedPtr<FJsonObject>* AddCameraBindingData = nullptr;
	TestTrue(TEXT("Sequence folder add camera binding contains data"), AddCameraBindingJson.IsValid() && AddCameraBindingJson->TryGetObjectField(TEXT("data"), AddCameraBindingData) && AddCameraBindingData && AddCameraBindingData->IsValid());
	const FString CameraBindingGuid = (AddCameraBindingData && AddCameraBindingData->IsValid()) ? (*AddCameraBindingData)->GetStringField(TEXT("binding_guid")) : FString();
	TestTrue(TEXT("Sequence folder camera binding guid non-empty"), !CameraBindingGuid.IsEmpty());

	const FString AddSkeletalBindingBody = MakeExecRequestBody(
		TEXT("smoke-sequence-folder-bind-skeletal-001"),
		TEXT("sequence_add_actor_binding"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"actor_id\":\"%s\",\"save_after_set\":false}"), *SequenceAssetPath, *SkeletalActorLabel));
	FHttpSmokeResult AddSkeletalBindingResult;
	TestTrue(TEXT("Sequence folder add skeletal binding request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddSkeletalBindingBody, AddSkeletalBindingResult));
	TestEqual(TEXT("Sequence folder add skeletal binding status code"), AddSkeletalBindingResult.StatusCode, 200);

	TSharedPtr<FJsonObject> AddSkeletalBindingJson;
	TestTrue(TEXT("Sequence folder add skeletal binding parses JSON"), ParseJson(AddSkeletalBindingResult.Body, AddSkeletalBindingJson));
	const TSharedPtr<FJsonObject>* AddSkeletalBindingData = nullptr;
	TestTrue(TEXT("Sequence folder add skeletal binding contains data"), AddSkeletalBindingJson.IsValid() && AddSkeletalBindingJson->TryGetObjectField(TEXT("data"), AddSkeletalBindingData) && AddSkeletalBindingData && AddSkeletalBindingData->IsValid());
	const FString SkeletalBindingGuid = (AddSkeletalBindingData && AddSkeletalBindingData->IsValid()) ? (*AddSkeletalBindingData)->GetStringField(TEXT("binding_guid")) : FString();
	TestTrue(TEXT("Sequence folder skeletal binding guid non-empty"), !SkeletalBindingGuid.IsEmpty());

	const FString AddTransformTrackBody = MakeExecRequestBody(
		TEXT("smoke-sequence-folder-transform-track-001"),
		TEXT("sequence_add_transform_track"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"binding_guid\":\"%s\",\"save_after_set\":false}"), *SequenceAssetPath, *BindingGuid));
	FHttpSmokeResult AddTransformTrackResult;
	TestTrue(TEXT("Sequence folder add transform track request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddTransformTrackBody, AddTransformTrackResult));
	TestEqual(TEXT("Sequence folder add transform track status code"), AddTransformTrackResult.StatusCode, 200);

	const FString AddTransformKeyBody = MakeExecRequestBody(
		TEXT("smoke-sequence-folder-transform-key-001"),
		TEXT("sequence_add_transform_key"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"binding_guid\":\"%s\",\"time_seconds\":1.0,\"location\":{\"x\":10,\"y\":20,\"z\":30},\"rotation\":{\"pitch\":0,\"yaw\":45,\"roll\":0},\"scale\":{\"x\":1.0,\"y\":1.0,\"z\":1.5},\"save_after_set\":false}"), *SequenceAssetPath, *BindingGuid));
	FHttpSmokeResult AddTransformKeyResult;
	TestTrue(TEXT("Sequence folder add transform key request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddTransformKeyBody, AddTransformKeyResult));
	TestEqual(TEXT("Sequence folder add transform key status code"), AddTransformKeyResult.StatusCode, 200);

	const FString AddFloatTrackBody = MakeExecRequestBody(
		TEXT("smoke-sequence-folder-float-track-001"),
		TEXT("sequence_add_property_track"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"binding_guid\":\"%s\",\"property_type\":\"float\",\"property_name\":\"CustomTimeDilation\",\"property_path\":\"CustomTimeDilation\",\"save_after_set\":false}"), *SequenceAssetPath, *BindingGuid));
	FHttpSmokeResult AddFloatTrackResult;
	TestTrue(TEXT("Sequence folder add float track request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddFloatTrackBody, AddFloatTrackResult));
	TestEqual(TEXT("Sequence folder add float track status code"), AddFloatTrackResult.StatusCode, 200);

	const FString AddFloatKeyBody = MakeExecRequestBody(
		TEXT("smoke-sequence-folder-float-key-001"),
		TEXT("sequence_add_property_key"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"binding_guid\":\"%s\",\"property_type\":\"float\",\"property_name\":\"CustomTimeDilation\",\"property_path\":\"CustomTimeDilation\",\"time_seconds\":1.0,\"value\":1.5,\"save_after_set\":false}"), *SequenceAssetPath, *BindingGuid));
	FHttpSmokeResult AddFloatKeyResult;
	TestTrue(TEXT("Sequence folder add float key request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddFloatKeyBody, AddFloatKeyResult));
	TestEqual(TEXT("Sequence folder add float key status code"), AddFloatKeyResult.StatusCode, 200);

	const FString SequenceObjectPath = FString::Printf(TEXT("%s.%s"), *SequenceAssetPath, *FPackageName::GetLongPackageAssetName(SequenceAssetPath));
	ULevelSequence* SequenceAsset = LoadObject<ULevelSequence>(nullptr, *SequenceObjectPath);
	TestNotNull(TEXT("Sequence folder direct load sequence asset"), SequenceAsset);
	UMovieScene* MovieScene = SequenceAsset ? SequenceAsset->GetMovieScene() : nullptr;
	TestNotNull(TEXT("Sequence folder direct movie scene"), MovieScene);
	UWorld* EditorWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	TestNotNull(TEXT("Sequence folder direct editor world"), EditorWorld);
	auto FindActorById = [](UWorld* World, const FString& ActorId) -> AActor*
	{
		if (!World || ActorId.IsEmpty())
		{
			return nullptr;
		}
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (Actor && (Actor->GetName().Equals(ActorId, ESearchCase::IgnoreCase) || Actor->GetActorLabel().Equals(ActorId, ESearchCase::IgnoreCase)))
			{
				return Actor;
			}
		}
		return nullptr;
	};
	AActor* BoundActor = FindActorById(EditorWorld, ActorLabel);
	TestNotNull(TEXT("Sequence folder direct bound actor"), BoundActor);
	UStaticMeshComponent* BoundMeshComponent = BoundActor ? BoundActor->FindComponentByClass<UStaticMeshComponent>() : nullptr;
	TestNotNull(TEXT("Sequence folder direct bound mesh component"), BoundMeshComponent);

	FGuid ActorBindingGuidValue;
	TestTrue(TEXT("Sequence folder parse actor binding guid"), FGuid::Parse(BindingGuid, ActorBindingGuidValue));

	FGuid ComponentBindingGuidValue;
	if (SequenceAsset && MovieScene && BoundMeshComponent)
	{
		ComponentBindingGuidValue = MovieScene->AddPossessable(BoundMeshComponent->GetName(), BoundMeshComponent->GetClass());
		if (FMovieScenePossessable* ComponentPossessable = MovieScene->FindPossessable(ComponentBindingGuidValue))
		{
			ComponentPossessable->SetParent(ActorBindingGuidValue, MovieScene);
		}
		SequenceAsset->BindPossessableObject(ComponentBindingGuidValue, *BoundMeshComponent, BoundActor);
		SequenceAsset->MarkPackageDirty();
	}
	TestTrue(TEXT("Sequence folder component binding guid valid"), ComponentBindingGuidValue.IsValid());
	const FString ComponentBindingGuid = ComponentBindingGuidValue.ToString(EGuidFormats::DigitsWithHyphensLower);

	const FString AddComponentFloatTrackBody = MakeExecRequestBody(
		TEXT("smoke-sequence-folder-component-float-track-001"),
		TEXT("sequence_add_property_track"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"binding_guid\":\"%s\",\"property_type\":\"float\",\"property_name\":\"BoundsScale\",\"property_path\":\"BoundsScale\",\"save_after_set\":false}"), *SequenceAssetPath, *ComponentBindingGuid));
	FHttpSmokeResult AddComponentFloatTrackResult;
	TestTrue(TEXT("Sequence folder add component float track request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddComponentFloatTrackBody, AddComponentFloatTrackResult));
	TestEqual(TEXT("Sequence folder add component float track status code"), AddComponentFloatTrackResult.StatusCode, 200);

	const FString AddComponentFloatKeyBody = MakeExecRequestBody(
		TEXT("smoke-sequence-folder-component-float-key-001"),
		TEXT("sequence_add_property_key"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"binding_guid\":\"%s\",\"property_type\":\"float\",\"property_name\":\"BoundsScale\",\"property_path\":\"BoundsScale\",\"time_seconds\":1.0,\"value\":1.25,\"save_after_set\":false}"), *SequenceAssetPath, *ComponentBindingGuid));
	FHttpSmokeResult AddComponentFloatKeyResult;
	TestTrue(TEXT("Sequence folder add component float key request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddComponentFloatKeyBody, AddComponentFloatKeyResult));
	TestEqual(TEXT("Sequence folder add component float key status code"), AddComponentFloatKeyResult.StatusCode, 200);

	FGuid SpawnableBindingGuidValue;
	if (SequenceAsset && EditorWorld)
	{
		FActorSpawnParameters SpawnParams;
		SpawnParams.ObjectFlags = RF_Transient;
		SpawnParams.bHideFromSceneOutliner = true;
		SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		ACameraActor* TempSpawnableActor = EditorWorld->SpawnActor<ACameraActor>(ACameraActor::StaticClass(), FTransform::Identity, SpawnParams);
		TestNotNull(TEXT("Sequence folder temp spawnable actor"), TempSpawnableActor);
		if (TempSpawnableActor)
		{
			UObject* SpawnableTemplate = MovieSceneHelpers::MakeSpawnableTemplateFromInstance(*TempSpawnableActor, MovieScene, TempSpawnableActor->GetFName());
			TestNotNull(TEXT("Sequence folder spawnable template"), SpawnableTemplate);
			if (SpawnableTemplate)
			{
				SpawnableBindingGuidValue = MovieScene->AddSpawnable(TEXT("SmokeCameraSpawnable"), *SpawnableTemplate);
				if (UMovieSceneSpawnTrack* SpawnTrack = MovieScene->AddTrack<UMovieSceneSpawnTrack>(SpawnableBindingGuidValue))
				{
					SpawnTrack->SetObjectId(SpawnableBindingGuidValue);
					if (SpawnTrack->GetAllSections().Num() == 0)
					{
						if (UMovieSceneSection* NewSection = SpawnTrack->CreateNewSection())
						{
							NewSection->SetRange(TRange<FFrameNumber>::All());
							SpawnTrack->AddSection(*NewSection);
						}
					}
				}
			}
			TempSpawnableActor->Destroy();
		}
	}
	TestTrue(TEXT("Sequence folder spawnable binding guid valid"), SpawnableBindingGuidValue.IsValid());
	const FString SpawnableBindingGuid = SpawnableBindingGuidValue.ToString(EGuidFormats::DigitsWithHyphensLower);

	const FString AddSpawnableTransformTrackBody = MakeExecRequestBody(
		TEXT("smoke-sequence-folder-spawnable-transform-track-001"),
		TEXT("sequence_add_transform_track"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"binding_guid\":\"%s\",\"save_after_set\":false}"), *SequenceAssetPath, *SpawnableBindingGuid));
	FHttpSmokeResult AddSpawnableTransformTrackResult;
	TestTrue(TEXT("Sequence folder add spawnable transform track request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddSpawnableTransformTrackBody, AddSpawnableTransformTrackResult));
	TestEqual(TEXT("Sequence folder add spawnable transform track status code"), AddSpawnableTransformTrackResult.StatusCode, 200);

	const FString AddSpawnableTransformKeyBody = MakeExecRequestBody(
		TEXT("smoke-sequence-folder-spawnable-transform-key-001"),
		TEXT("sequence_add_transform_key"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"binding_guid\":\"%s\",\"time_seconds\":0.5,\"location\":{\"x\":40,\"y\":5,\"z\":100},\"rotation\":{\"pitch\":0,\"yaw\":15,\"roll\":0},\"scale\":{\"x\":1.0,\"y\":1.0,\"z\":1.0},\"save_after_set\":false}"), *SequenceAssetPath, *SpawnableBindingGuid));
	FHttpSmokeResult AddSpawnableTransformKeyResult;
	TestTrue(TEXT("Sequence folder add spawnable transform key request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddSpawnableTransformKeyBody, AddSpawnableTransformKeyResult));
	TestEqual(TEXT("Sequence folder add spawnable transform key status code"), AddSpawnableTransformKeyResult.StatusCode, 200);

	if (MovieScene)
	{
		if (UMovieSceneSpawnTrack* SpawnTrack = MovieScene->FindTrack<UMovieSceneSpawnTrack>(SpawnableBindingGuidValue))
		{
			UMovieSceneSpawnSection* SpawnSection = SpawnTrack->GetAllSections().Num() > 0 ? Cast<UMovieSceneSpawnSection>(SpawnTrack->GetAllSections()[0]) : nullptr;
			if (SpawnSection)
			{
				FMovieSceneBoolChannel& SpawnChannel = SpawnSection->GetChannel();
				SpawnChannel.GetData().Reset();
				SpawnChannel.GetData().UpdateOrAddKey(MovieScene->GetTickResolution().AsFrameTime(0.0).RoundToFrame(), true);
				SpawnChannel.GetData().UpdateOrAddKey(MovieScene->GetTickResolution().AsFrameTime(2.0).RoundToFrame(), false);
			}
		}
	}

	const FString ExportFolderBody = MakeExecRequestBody(
		TEXT("smoke-sequence-folder-export-001"),
		TEXT("sequence_export_folder"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"clean_output_dir\":true,\"include_validation\":true}"), *SequenceAssetPath));
	FHttpSmokeResult ExportFolderResult;
	TestTrue(TEXT("Sequence folder export request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ExportFolderBody, ExportFolderResult));
	TestEqual(TEXT("Sequence folder export status code"), ExportFolderResult.StatusCode, 200);

	TSharedPtr<FJsonObject> ExportFolderJson;
	TestTrue(TEXT("Sequence folder export parses JSON"), ParseJson(ExportFolderResult.Body, ExportFolderJson));
	const TSharedPtr<FJsonObject>* ExportFolderData = nullptr;
	TestTrue(TEXT("Sequence folder export contains data"), ExportFolderJson.IsValid() && ExportFolderJson->TryGetObjectField(TEXT("data"), ExportFolderData) && ExportFolderData && ExportFolderData->IsValid());
	if (ExportFolderData && ExportFolderData->IsValid())
	{
		TestTrue(TEXT("Sequence folder export complete inventory flag"), (*ExportFolderData)->GetBoolField(TEXT("complete_inventory_exported")));
		const FString CompleteInventoryPath = (*ExportFolderData)->GetStringField(TEXT("complete_inventory_path"));
		TestFalse(TEXT("Sequence folder export complete inventory path non-empty"), CompleteInventoryPath.IsEmpty());
		TestTrue(TEXT("Sequence folder export complete inventory file exists"), IFileManager::Get().FileExists(*CompleteInventoryPath));

		const TSharedPtr<FJsonObject>* CoverageReport = nullptr;
		TestTrue(TEXT("Sequence folder export returns coverage report"), (*ExportFolderData)->TryGetObjectField(TEXT("coverage_report"), CoverageReport) && CoverageReport && CoverageReport->IsValid());
		if (CoverageReport && CoverageReport->IsValid())
		{
			TestEqual(TEXT("Sequence folder coverage schema"), (*CoverageReport)->GetStringField(TEXT("schema")), FString(TEXT("uai.sequence.folder_coverage.v1")));
			TestEqual(TEXT("Sequence folder coverage implementation status"), (*CoverageReport)->GetStringField(TEXT("implementation_status")), FString(TEXT("partial_folder_profile")));
			TestFalse(TEXT("Sequence folder coverage is not complete target schema"), (*CoverageReport)->GetBoolField(TEXT("is_complete_target_schema")));
			TestTrue(TEXT("Sequence folder coverage has tracks"), static_cast<int32>((*CoverageReport)->GetIntegerField(TEXT("track_count"))) > 0);
			TestTrue(TEXT("Sequence folder coverage reports partial tracks"), static_cast<int32>((*CoverageReport)->GetIntegerField(TEXT("partial_roundtrip_track_count"))) > 0);
		}
	}

	TSharedPtr<FJsonObject> CompleteInventoryJson;
	TestTrue(TEXT("Sequence folder complete inventory load"), LoadJsonFile(FPaths::Combine(FolderPath, TEXT("complete"), TEXT("inventory.json")), CompleteInventoryJson));
	if (CompleteInventoryJson.IsValid())
	{
		TestEqual(TEXT("Sequence folder complete inventory schema"), CompleteInventoryJson->GetStringField(TEXT("schema")), FString(TEXT("uai.sequence.complete_inventory.v1")));
		TestTrue(TEXT("Sequence folder complete inventory read role"), CompleteInventoryJson->GetBoolField(TEXT("is_complete_read_inventory")));
		TestFalse(TEXT("Sequence folder complete inventory not lossless apply"), CompleteInventoryJson->GetBoolField(TEXT("is_complete_lossless_apply")));
		TestTrue(TEXT("Sequence folder complete inventory has tracks"), static_cast<int32>(CompleteInventoryJson->GetIntegerField(TEXT("track_count"))) > 0);
		TestTrue(TEXT("Sequence folder complete inventory has sections"), static_cast<int32>(CompleteInventoryJson->GetIntegerField(TEXT("section_count"))) > 0);
		TestFalse(TEXT("Sequence folder complete inventory omits sequence raw properties by default"), CompleteInventoryJson->HasField(TEXT("sequence_raw_properties")));
		TestFalse(TEXT("Sequence folder complete inventory omits movie scene raw properties by default"), CompleteInventoryJson->HasField(TEXT("movie_scene_raw_properties")));
		bool bCompleteInventoryHasTrackRawProperties = false;
		bool bCompleteInventoryHasSectionRawProperties = false;
		bool bCompleteInventoryHasSectionChannels = false;
		const TArray<TSharedPtr<FJsonValue>>* CompleteBindings = nullptr;
		if (CompleteInventoryJson->TryGetArrayField(TEXT("bindings"), CompleteBindings) && CompleteBindings)
		{
			for (const TSharedPtr<FJsonValue>& BindingValue : *CompleteBindings)
			{
				const TSharedPtr<FJsonObject>* BindingObj = nullptr;
				if (!BindingValue.IsValid() || !BindingValue->TryGetObject(BindingObj) || !BindingObj || !(*BindingObj).IsValid())
				{
					continue;
				}
				const TArray<TSharedPtr<FJsonValue>>* CompleteTracks = nullptr;
				if (!(*BindingObj)->TryGetArrayField(TEXT("tracks"), CompleteTracks) || !CompleteTracks)
				{
					continue;
				}
				for (const TSharedPtr<FJsonValue>& TrackValue : *CompleteTracks)
				{
					const TSharedPtr<FJsonObject>* TrackObj = nullptr;
					if (!TrackValue.IsValid() || !TrackValue->TryGetObject(TrackObj) || !TrackObj || !(*TrackObj).IsValid())
					{
						continue;
					}
					const TArray<TSharedPtr<FJsonValue>>* TrackRawProperties = nullptr;
					if ((*TrackObj)->TryGetArrayField(TEXT("raw_properties"), TrackRawProperties) && TrackRawProperties && TrackRawProperties->Num() > 0)
					{
						bCompleteInventoryHasTrackRawProperties = true;
					}
					const TArray<TSharedPtr<FJsonValue>>* CompleteSections = nullptr;
					if (!(*TrackObj)->TryGetArrayField(TEXT("sections"), CompleteSections) || !CompleteSections)
					{
						continue;
					}
					for (const TSharedPtr<FJsonValue>& SectionValue : *CompleteSections)
					{
						const TSharedPtr<FJsonObject>* SectionObj = nullptr;
						if (!SectionValue.IsValid() || !SectionValue->TryGetObject(SectionObj) || !SectionObj || !(*SectionObj).IsValid())
						{
							continue;
						}
						const TArray<TSharedPtr<FJsonValue>>* SectionRawProperties = nullptr;
						if ((*SectionObj)->TryGetArrayField(TEXT("raw_properties"), SectionRawProperties) && SectionRawProperties && SectionRawProperties->Num() > 0)
						{
							bCompleteInventoryHasSectionRawProperties = true;
						}
						const TArray<TSharedPtr<FJsonValue>>* SectionChannels = nullptr;
						if ((*SectionObj)->TryGetArrayField(TEXT("channels"), SectionChannels) && SectionChannels && SectionChannels->Num() > 0)
						{
							bCompleteInventoryHasSectionChannels = true;
						}
					}
				}
			}
		}
		TestFalse(TEXT("Sequence folder complete inventory track raw properties omitted by default"), bCompleteInventoryHasTrackRawProperties);
		TestFalse(TEXT("Sequence folder complete inventory section raw properties omitted by default"), bCompleteInventoryHasSectionRawProperties);
		TestTrue(TEXT("Sequence folder complete inventory section channels"), bCompleteInventoryHasSectionChannels);
	}

	TSharedPtr<FJsonObject> CoverageReportJson;
	TestTrue(TEXT("Sequence folder coverage report load"), LoadJsonFile(FPaths::Combine(FolderPath, TEXT("validation"), TEXT("coverage_report.json")), CoverageReportJson));
	if (CoverageReportJson.IsValid())
	{
		TestEqual(TEXT("Sequence folder coverage report file schema"), CoverageReportJson->GetStringField(TEXT("schema")), FString(TEXT("uai.sequence.folder_coverage.v1")));
		TestEqual(TEXT("Sequence folder coverage report file contract"), CoverageReportJson->GetStringField(TEXT("authoring_contract")), FString(TEXT("curated_common_subset_not_full_ue_sequencer_model")));
	}

	TSharedPtr<FJsonObject> AssetJson;
	TestTrue(TEXT("Sequence folder asset json load"), LoadJsonFile(FPaths::Combine(FolderPath, TEXT("asset.json")), AssetJson));
	if (AssetJson.IsValid())
	{
		TestFalse(TEXT("Sequence folder asset json omits sequence raw properties by default"), AssetJson->HasField(TEXT("sequence_raw_properties")));
		TSharedPtr<FJsonObject> SequenceDetailsJson;
		TestTrue(
			TEXT("Sequence folder asset details grouped"),
			SmokeLoadGroupedDetailsForRef(this, FPaths::Combine(FolderPath, TEXT("asset.json")), AssetJson, TEXT("details"), TEXT("sequence_asset"), SequenceDetailsJson));
	}

	TSharedPtr<FJsonObject> SettingsJson;
	TestTrue(TEXT("Sequence folder settings load"), LoadJsonFile(FPaths::Combine(FolderPath, TEXT("settings"), TEXT("sequence.json")), SettingsJson));
	if (SettingsJson.IsValid())
	{
		TestFalse(TEXT("Sequence folder settings omits movie scene raw properties by default"), SettingsJson->HasField(TEXT("movie_scene_raw_properties")));
		TSharedPtr<FJsonObject> MovieSceneDetailsJson;
		TestTrue(
			TEXT("Sequence folder settings details grouped"),
			SmokeLoadGroupedDetailsForRef(this, FPaths::Combine(FolderPath, TEXT("settings"), TEXT("sequence.json")), SettingsJson, TEXT("details"), TEXT("movie_scene_settings"), MovieSceneDetailsJson));
		const TSharedPtr<FJsonObject>* PlaybackObj = nullptr;
		TestTrue(TEXT("Sequence folder settings playback exists"), SettingsJson->TryGetObjectField(TEXT("playback"), PlaybackObj) && PlaybackObj && (*PlaybackObj).IsValid());
		if (PlaybackObj && (*PlaybackObj).IsValid())
		{
			(*PlaybackObj)->SetNumberField(TEXT("start_seconds"), 0.25);
			(*PlaybackObj)->SetNumberField(TEXT("duration_seconds"), 4.0);
		}
		const TSharedPtr<FJsonObject>* DisplayRateObj = nullptr;
		TestTrue(TEXT("Sequence folder settings display rate exists"), SettingsJson->TryGetObjectField(TEXT("display_rate"), DisplayRateObj) && DisplayRateObj && (*DisplayRateObj).IsValid());
		if (DisplayRateObj && (*DisplayRateObj).IsValid())
		{
			(*DisplayRateObj)->SetNumberField(TEXT("numerator"), 12);
			(*DisplayRateObj)->SetNumberField(TEXT("denominator"), 1);
		}
	}
	TestTrue(TEXT("Sequence folder settings save"), SaveJsonFile(FPaths::Combine(FolderPath, TEXT("settings"), TEXT("sequence.json")), SettingsJson));

	TSharedPtr<FJsonObject> BindingsIndexJson;
	TestTrue(TEXT("Sequence folder bindings index load"), LoadJsonFile(FPaths::Combine(FolderPath, TEXT("bindings"), TEXT("index.json")), BindingsIndexJson));
	TMap<FString, FString> BindingFolderByGuid;
	if (BindingsIndexJson.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Bindings = nullptr;
		TestTrue(TEXT("Sequence folder bindings index array exists"), BindingsIndexJson->TryGetArrayField(TEXT("bindings"), Bindings) && Bindings && Bindings->Num() >= 5);
		if (Bindings)
		{
			for (const TSharedPtr<FJsonValue>& BindingValue : *Bindings)
			{
				const TSharedPtr<FJsonObject>* BindingObj = nullptr;
				if (!BindingValue.IsValid() || !BindingValue->TryGetObject(BindingObj) || !BindingObj || !(*BindingObj).IsValid())
				{
					continue;
				}
				BindingFolderByGuid.Add((*BindingObj)->GetStringField(TEXT("binding_guid")), (*BindingObj)->GetStringField(TEXT("folder_name")));
			}
		}
	}
	const FString BindingFolderName = BindingFolderByGuid.FindRef(BindingGuid);
	const FString ComponentBindingFolderName = BindingFolderByGuid.FindRef(ComponentBindingGuid);
	const FString SpawnableBindingFolderName = BindingFolderByGuid.FindRef(SpawnableBindingGuid);
	const FString SkeletalBindingFolderName = BindingFolderByGuid.FindRef(SkeletalBindingGuid);
	TestTrue(TEXT("Sequence folder actor binding folder name non-empty"), !BindingFolderName.IsEmpty());
	TestTrue(TEXT("Sequence folder component binding folder name non-empty"), !ComponentBindingFolderName.IsEmpty());
	TestTrue(TEXT("Sequence folder spawnable binding folder name non-empty"), !SpawnableBindingFolderName.IsEmpty());
	TestTrue(TEXT("Sequence folder skeletal binding folder name non-empty"), !SkeletalBindingFolderName.IsEmpty());

	auto TrackFolderHasSectionMetadata = [this, &LoadJsonFile](const FString& TracksDir) -> bool
	{
		TArray<FString> TrackFiles;
		IFileManager::Get().FindFiles(TrackFiles, *(FPaths::Combine(TracksDir, TEXT("*.json"))), true, false);
		TrackFiles.Sort();
		for (const FString& TrackFile : TrackFiles)
		{
			TSharedPtr<FJsonObject> TrackJson;
			if (!LoadJsonFile(FPaths::Combine(TracksDir, TrackFile), TrackJson) || !TrackJson.IsValid())
			{
				continue;
			}
			const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
			if (!TrackJson->TryGetArrayField(TEXT("sections"), Sections) || !Sections)
			{
				continue;
			}
			for (const TSharedPtr<FJsonValue>& SectionValue : *Sections)
			{
				const TSharedPtr<FJsonObject>* SectionObj = nullptr;
				if (!SectionValue.IsValid() || !SectionValue->TryGetObject(SectionObj) || !SectionObj || !SectionObj->IsValid())
				{
					continue;
				}
				const TSharedPtr<FJsonObject>* SectionMetadata = nullptr;
				if ((*SectionObj)->TryGetObjectField(TEXT("section_metadata"), SectionMetadata) && SectionMetadata && SectionMetadata->IsValid())
				{
					return true;
				}
			}
		}
		return false;
	};
	TestTrue(TEXT("Sequence folder exported section metadata exists"), TrackFolderHasSectionMetadata(FPaths::Combine(FolderPath, TEXT("bindings"), BindingFolderName, TEXT("tracks"))));

	auto HasAuthoringMetadata = [](const TSharedPtr<FJsonObject>& Obj) -> bool
	{
		return Obj.IsValid()
			&& Obj->HasTypedField<EJson::Object>(TEXT("selector"))
			&& Obj->HasTypedField<EJson::Array>(TEXT("operations"));
	};
	TSharedPtr<FJsonObject> BindingJsonForAuthoring;
	TestTrue(TEXT("Sequence folder binding authoring json load"), LoadJsonFile(FPaths::Combine(FolderPath, TEXT("bindings"), BindingFolderName, TEXT("binding.json")), BindingJsonForAuthoring));
	if (BindingJsonForAuthoring.IsValid())
	{
		const TSharedPtr<FJsonObject>* BindingObjForAuthoring = nullptr;
		TestTrue(TEXT("Sequence folder binding has authoring metadata"), BindingJsonForAuthoring->TryGetObjectField(TEXT("binding"), BindingObjForAuthoring) && BindingObjForAuthoring && HasAuthoringMetadata(*BindingObjForAuthoring));
	}

	auto TrackFolderHasAuthoringMetadata = [&LoadJsonFile, &HasAuthoringMetadata](const FString& TracksDir) -> bool
	{
		TArray<FString> TrackFiles;
		IFileManager::Get().FindFiles(TrackFiles, *(FPaths::Combine(TracksDir, TEXT("*.json"))), true, false);
		TrackFiles.Sort();
		for (const FString& TrackFile : TrackFiles)
		{
			TSharedPtr<FJsonObject> TrackJson;
			if (!LoadJsonFile(FPaths::Combine(TracksDir, TrackFile), TrackJson) || !HasAuthoringMetadata(TrackJson))
			{
				continue;
			}
			const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
			if (!TrackJson->TryGetArrayField(TEXT("sections"), Sections) || !Sections)
			{
				continue;
			}
			for (const TSharedPtr<FJsonValue>& SectionValue : *Sections)
			{
				const TSharedPtr<FJsonObject>* SectionObj = nullptr;
				if (SectionValue.IsValid() && SectionValue->TryGetObject(SectionObj) && SectionObj && HasAuthoringMetadata(*SectionObj))
				{
					return true;
				}
			}
		}
		return false;
	};
	TestTrue(TEXT("Sequence folder track and section authoring metadata exists"), TrackFolderHasAuthoringMetadata(FPaths::Combine(FolderPath, TEXT("bindings"), BindingFolderName, TEXT("tracks"))));

	auto TrackFolderHasRawProperties = [this, &LoadJsonFile](const FString& TracksDir) -> bool
	{
		TArray<FString> TrackFiles;
		IFileManager::Get().FindFiles(TrackFiles, *(FPaths::Combine(TracksDir, TEXT("*.json"))), true, false);
		TrackFiles.Sort();
		for (const FString& TrackFile : TrackFiles)
		{
			TSharedPtr<FJsonObject> TrackJson;
			if (!LoadJsonFile(FPaths::Combine(TracksDir, TrackFile), TrackJson) || !TrackJson.IsValid())
			{
				continue;
			}
			const TArray<TSharedPtr<FJsonValue>>* TrackRawProperties = nullptr;
			if (TrackJson->TryGetArrayField(TEXT("raw_properties"), TrackRawProperties) && TrackRawProperties && TrackRawProperties->Num() > 0)
			{
				return true;
			}
			const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
			if (!TrackJson->TryGetArrayField(TEXT("sections"), Sections) || !Sections)
			{
				continue;
			}
			for (const TSharedPtr<FJsonValue>& SectionValue : *Sections)
			{
				const TSharedPtr<FJsonObject>* SectionObj = nullptr;
				if (!SectionValue.IsValid() || !SectionValue->TryGetObject(SectionObj) || !SectionObj || !(*SectionObj).IsValid())
				{
					continue;
				}
				const TArray<TSharedPtr<FJsonValue>>* SectionRawProperties = nullptr;
				if ((*SectionObj)->TryGetArrayField(TEXT("raw_properties"), SectionRawProperties) && SectionRawProperties && SectionRawProperties->Num() > 0)
				{
					return true;
				}
			}
		}
		return false;
	};
	TestFalse(TEXT("Sequence folder exported track raw properties omitted by default"), TrackFolderHasRawProperties(FPaths::Combine(FolderPath, TEXT("bindings"), BindingFolderName, TEXT("tracks"))));

	auto TrackFolderHasGroupedDetails = [this, &LoadJsonFile](const FString& TracksDir) -> bool
	{
		TArray<FString> TrackFiles;
		IFileManager::Get().FindFiles(TrackFiles, *(FPaths::Combine(TracksDir, TEXT("*.json"))), true, false);
		TrackFiles.Sort();
		bool bFoundTrackDetails = false;
		bool bFoundSectionDetails = false;
		for (const FString& TrackFile : TrackFiles)
		{
			const FString TrackJsonPath = FPaths::Combine(TracksDir, TrackFile);
			TSharedPtr<FJsonObject> TrackJson;
			if (!LoadJsonFile(TrackJsonPath, TrackJson) || !TrackJson.IsValid())
			{
				continue;
			}
			TSharedPtr<FJsonObject> TrackDetailsJson;
			bFoundTrackDetails = bFoundTrackDetails || SmokeLoadGroupedDetailsForRef(this, TrackJsonPath, TrackJson, TEXT("details"), TEXT("sequence_track"), TrackDetailsJson);
			const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
			if (!TrackJson->TryGetArrayField(TEXT("sections"), Sections) || !Sections)
			{
				continue;
			}
			for (const TSharedPtr<FJsonValue>& SectionValue : *Sections)
			{
				const TSharedPtr<FJsonObject>* SectionObj = nullptr;
				if (!SectionValue.IsValid() || !SectionValue->TryGetObject(SectionObj) || !SectionObj || !(*SectionObj).IsValid())
				{
					continue;
				}
				TSharedPtr<FJsonObject> SectionDetailsJson;
				bFoundSectionDetails = bFoundSectionDetails || SmokeLoadGroupedDetailsForRef(this, TrackJsonPath, *SectionObj, TEXT("details"), TEXT("sequence_section"), SectionDetailsJson);
			}
		}
		return bFoundTrackDetails && bFoundSectionDetails;
	};
	TestTrue(TEXT("Sequence folder exported track and section details grouped"), TrackFolderHasGroupedDetails(FPaths::Combine(FolderPath, TEXT("bindings"), BindingFolderName, TEXT("tracks"))));

	auto TrackFolderHasSectionChannels = [this, &LoadJsonFile](const FString& TracksDir) -> bool
	{
		TArray<FString> TrackFiles;
		IFileManager::Get().FindFiles(TrackFiles, *(FPaths::Combine(TracksDir, TEXT("*.json"))), true, false);
		TrackFiles.Sort();
		for (const FString& TrackFile : TrackFiles)
		{
			TSharedPtr<FJsonObject> TrackJson;
			if (!LoadJsonFile(FPaths::Combine(TracksDir, TrackFile), TrackJson) || !TrackJson.IsValid())
			{
				continue;
			}
			const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
			if (!TrackJson->TryGetArrayField(TEXT("sections"), Sections) || !Sections)
			{
				continue;
			}
			for (const TSharedPtr<FJsonValue>& SectionValue : *Sections)
			{
				const TSharedPtr<FJsonObject>* SectionObj = nullptr;
				if (!SectionValue.IsValid() || !SectionValue->TryGetObject(SectionObj) || !SectionObj || !(*SectionObj).IsValid())
				{
					continue;
				}
				const TArray<TSharedPtr<FJsonValue>>* SectionChannels = nullptr;
				if ((*SectionObj)->TryGetArrayField(TEXT("channels"), SectionChannels) && SectionChannels && SectionChannels->Num() > 0)
				{
					return true;
				}
			}
		}
		return false;
	};
	TestTrue(TEXT("Sequence folder exported section channels exists"), TrackFolderHasSectionChannels(FPaths::Combine(FolderPath, TEXT("bindings"), BindingFolderName, TEXT("tracks"))));

	auto ModifyTrackFiles = [this, &LoadJsonFile, &SaveJsonFile, &ComponentBindingGuid, &SpawnableBindingGuid](const FString& TracksDir, const FString& ExpectedBindingGuid, bool& bOutModifiedTransform, bool& bOutModifiedFloat, bool& bOutModifiedSpawn)
	{
		TArray<FString> TrackFiles;
		IFileManager::Get().FindFiles(TrackFiles, *(FPaths::Combine(TracksDir, TEXT("*.json"))), true, false);
		TrackFiles.Sort();
		for (const FString& TrackFile : TrackFiles)
		{
			const FString TrackPath = FPaths::Combine(TracksDir, TrackFile);
			TSharedPtr<FJsonObject> TrackJson;
			TestTrue(FString::Printf(TEXT("Sequence folder track load: %s"), *TrackFile), LoadJsonFile(TrackPath, TrackJson));
			if (!TrackJson.IsValid())
			{
				continue;
			}
			const FString TrackType = TrackJson->GetStringField(TEXT("track_type"));
			if (TrackType == TEXT("transform"))
			{
				const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
				if (TrackJson->TryGetArrayField(TEXT("sections"), Sections) && Sections && Sections->Num() == 1)
				{
					const TSharedPtr<FJsonObject>* SectionObj = nullptr;
					if ((*Sections)[0]->TryGetObject(SectionObj) && SectionObj && (*SectionObj).IsValid())
					{
						const TArray<TSharedPtr<FJsonValue>>* Keys = nullptr;
						if ((*SectionObj)->TryGetArrayField(TEXT("keys"), Keys) && Keys && Keys->Num() == 1)
						{
							const TSharedPtr<FJsonObject>* KeyObj = nullptr;
							if ((*Keys)[0]->TryGetObject(KeyObj) && KeyObj && (*KeyObj).IsValid())
							{
								const TSharedPtr<FJsonObject>* LocationObj = nullptr;
								if ((*KeyObj)->TryGetObjectField(TEXT("location"), LocationObj) && LocationObj && (*LocationObj).IsValid())
								{
									(*LocationObj)->SetNumberField(TEXT("x"), ExpectedBindingGuid == SpawnableBindingGuid ? 55.0 : 110.0);
									bOutModifiedTransform = true;
								}
							}
						}
					}
				}
			}
			else if (TrackType == TEXT("property") && TrackJson->GetStringField(TEXT("property_type")) == TEXT("float"))
			{
				const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
				if (TrackJson->TryGetArrayField(TEXT("sections"), Sections) && Sections && Sections->Num() == 1)
				{
					const TSharedPtr<FJsonObject>* SectionObj = nullptr;
					if ((*Sections)[0]->TryGetObject(SectionObj) && SectionObj && (*SectionObj).IsValid())
					{
						const TArray<TSharedPtr<FJsonValue>>* Keys = nullptr;
						if ((*SectionObj)->TryGetArrayField(TEXT("keys"), Keys) && Keys && Keys->Num() == 1)
						{
							const TSharedPtr<FJsonObject>* KeyObj = nullptr;
							if ((*Keys)[0]->TryGetObject(KeyObj) && KeyObj && (*KeyObj).IsValid())
							{
								(*KeyObj)->SetNumberField(TEXT("value"), ExpectedBindingGuid == ComponentBindingGuid ? 1.75 : 2.5);
								bOutModifiedFloat = true;
							}
						}
					}
				}
			}
			else if (TrackType == TEXT("spawn"))
			{
				const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
				if (TrackJson->TryGetArrayField(TEXT("sections"), Sections) && Sections && Sections->Num() == 1)
				{
					const TSharedPtr<FJsonObject>* SectionObj = nullptr;
					if ((*Sections)[0]->TryGetObject(SectionObj) && SectionObj && (*SectionObj).IsValid())
					{
						const TArray<TSharedPtr<FJsonValue>>* Keys = nullptr;
						if ((*SectionObj)->TryGetArrayField(TEXT("keys"), Keys) && Keys && Keys->Num() >= 2)
						{
							const TSharedPtr<FJsonObject>* KeyObj = nullptr;
							if ((*Keys)[1]->TryGetObject(KeyObj) && KeyObj && (*KeyObj).IsValid())
							{
								(*KeyObj)->SetNumberField(TEXT("time_seconds"), 2.5);
								bOutModifiedSpawn = true;
							}
						}
					}
				}
			}
			TestTrue(FString::Printf(TEXT("Sequence folder track save: %s"), *TrackFile), SaveJsonFile(TrackPath, TrackJson));
		}
	};

	bool bModifiedTransform = false;
	bool bModifiedFloat = false;
	bool bModifiedComponentFloat = false;
	bool bModifiedSpawnTransform = false;
	bool bModifiedSpawnTrack = false;
	ModifyTrackFiles(FPaths::Combine(FolderPath, TEXT("bindings"), BindingFolderName, TEXT("tracks")), BindingGuid, bModifiedTransform, bModifiedFloat, bModifiedSpawnTrack);
	ModifyTrackFiles(FPaths::Combine(FolderPath, TEXT("bindings"), ComponentBindingFolderName, TEXT("tracks")), ComponentBindingGuid, bModifiedSpawnTransform, bModifiedComponentFloat, bModifiedSpawnTrack);
	ModifyTrackFiles(FPaths::Combine(FolderPath, TEXT("bindings"), SpawnableBindingFolderName, TEXT("tracks")), SpawnableBindingGuid, bModifiedSpawnTransform, bModifiedComponentFloat, bModifiedSpawnTrack);
	TestTrue(TEXT("Sequence folder modified actor transform track"), bModifiedTransform);
	TestTrue(TEXT("Sequence folder modified actor float track"), bModifiedFloat);
	TestTrue(TEXT("Sequence folder modified component float track"), bModifiedComponentFloat);
	TestTrue(TEXT("Sequence folder modified spawnable transform track"), bModifiedSpawnTransform);
	TestTrue(TEXT("Sequence folder modified spawn track"), bModifiedSpawnTrack);

	bool bAddedControlRigTrack = false;
	if (!SkeletalBindingFolderName.IsEmpty() && !ControlRigSmokeControlName.IsEmpty())
	{
		auto MakeVectorJson = [](double X, double Y, double Z) -> TSharedPtr<FJsonObject>
		{
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetNumberField(TEXT("x"), X);
			Obj->SetNumberField(TEXT("y"), Y);
			Obj->SetNumberField(TEXT("z"), Z);
			return Obj;
		};

		TSharedPtr<FJsonObject> TransformValueObj = MakeShared<FJsonObject>();
		TransformValueObj->SetObjectField(TEXT("location"), MakeVectorJson(12.0, 3.0, 4.0));
		TSharedPtr<FJsonObject> RotationObj = MakeShared<FJsonObject>();
		RotationObj->SetNumberField(TEXT("pitch"), 0.0);
		RotationObj->SetNumberField(TEXT("yaw"), 15.0);
		RotationObj->SetNumberField(TEXT("roll"), 0.0);
		TransformValueObj->SetObjectField(TEXT("rotation"), RotationObj);
		TransformValueObj->SetObjectField(TEXT("scale"), MakeVectorJson(1.0, 1.0, 1.0));

		TSharedPtr<FJsonObject> KeyObj = MakeShared<FJsonObject>();
		KeyObj->SetNumberField(TEXT("time_seconds"), 0.5);
		KeyObj->SetStringField(TEXT("control"), ControlRigSmokeControlName);
		KeyObj->SetStringField(TEXT("control_type"), TEXT("transform"));
		KeyObj->SetObjectField(TEXT("value"), TransformValueObj);

		TSharedPtr<FJsonObject> SectionObj = MakeShared<FJsonObject>();
		SectionObj->SetStringField(TEXT("section_type"), TEXT("control_rig"));
		SectionObj->SetArrayField(TEXT("keys"), { MakeShared<FJsonValueObject>(KeyObj) });

		TSharedPtr<FJsonObject> ControlRigTrackObj = MakeShared<FJsonObject>();
		ControlRigTrackObj->SetStringField(TEXT("track_type"), TEXT("control_rig"));
		ControlRigTrackObj->SetStringField(TEXT("track_name"), TEXT("FK Control Rig"));
		ControlRigTrackObj->SetStringField(TEXT("display_name"), TEXT("FK Control Rig"));
		ControlRigTrackObj->SetStringField(TEXT("control_rig_class"), TEXT("/Script/ControlRig.FKControlRig"));
		ControlRigTrackObj->SetBoolField(TEXT("is_additive"), false);
		ControlRigTrackObj->SetArrayField(TEXT("sections"), { MakeShared<FJsonValueObject>(SectionObj) });

		bAddedControlRigTrack = SaveJsonFile(
			FPaths::Combine(FolderPath, TEXT("bindings"), SkeletalBindingFolderName, TEXT("tracks"), TEXT("control_rig_smoke.json")),
			ControlRigTrackObj);
	}
	TestTrue(TEXT("Sequence folder added control rig track json"), bAddedControlRigTrack);

	TSharedPtr<FJsonObject> MasterTracksJson;
	TestTrue(TEXT("Sequence folder master tracks load"), LoadJsonFile(FPaths::Combine(FolderPath, TEXT("master_tracks"), TEXT("index.json")), MasterTracksJson));
	if (MasterTracksJson.IsValid())
	{
		TArray<TSharedPtr<FJsonValue>> MasterTracks;

		TSharedPtr<FJsonObject> CameraCutTrackObj = MakeShared<FJsonObject>();
		CameraCutTrackObj->SetStringField(TEXT("track_type"), TEXT("camera_cut"));
		CameraCutTrackObj->SetBoolField(TEXT("can_blend"), false);
		TSharedPtr<FJsonObject> CameraCutSectionObj = MakeShared<FJsonObject>();
		CameraCutSectionObj->SetStringField(TEXT("section_type"), TEXT("camera_cut"));
		CameraCutSectionObj->SetStringField(TEXT("camera_binding_guid"), CameraBindingGuid);
		CameraCutSectionObj->SetNumberField(TEXT("start_seconds"), 0.0);
		CameraCutSectionObj->SetNumberField(TEXT("duration_seconds"), 2.0);
		CameraCutSectionObj->SetBoolField(TEXT("lock_previous_camera"), false);
		CameraCutTrackObj->SetArrayField(TEXT("sections"), { MakeShared<FJsonValueObject>(CameraCutSectionObj) });
		MasterTracks.Add(MakeShared<FJsonValueObject>(CameraCutTrackObj));

		TSharedPtr<FJsonObject> SubSequenceTrackObj = MakeShared<FJsonObject>();
		SubSequenceTrackObj->SetStringField(TEXT("track_type"), TEXT("sub_sequence"));
		TSharedPtr<FJsonObject> SubSequenceSectionObj = MakeShared<FJsonObject>();
		SubSequenceSectionObj->SetStringField(TEXT("section_type"), TEXT("sub_sequence"));
		SubSequenceSectionObj->SetStringField(TEXT("sequence_asset"), ChildSequenceAssetPath);
		SubSequenceSectionObj->SetNumberField(TEXT("start_seconds"), 2.0);
		SubSequenceSectionObj->SetNumberField(TEXT("duration_seconds"), 1.0);
		SubSequenceSectionObj->SetNumberField(TEXT("row_index"), 0);
		SubSequenceSectionObj->SetNumberField(TEXT("start_frame_offset"), 0);
		SubSequenceSectionObj->SetBoolField(TEXT("can_loop"), false);
		SubSequenceSectionObj->SetNumberField(TEXT("end_frame_offset"), 0);
		SubSequenceSectionObj->SetNumberField(TEXT("first_loop_start_frame_offset"), 0);
		SubSequenceSectionObj->SetNumberField(TEXT("hierarchical_bias"), 100);
		SubSequenceSectionObj->SetNumberField(TEXT("network_mask"), 0);
		SubSequenceSectionObj->SetStringField(TEXT("time_scale_type"), TEXT("fixed"));
		SubSequenceSectionObj->SetNumberField(TEXT("time_scale"), 1.0);
		SubSequenceTrackObj->SetArrayField(TEXT("sections"), { MakeShared<FJsonValueObject>(SubSequenceSectionObj) });
		MasterTracks.Add(MakeShared<FJsonValueObject>(SubSequenceTrackObj));

		MasterTracksJson->SetArrayField(TEXT("tracks"), MasterTracks);
	}
	TestTrue(TEXT("Sequence folder master tracks save"), SaveJsonFile(FPaths::Combine(FolderPath, TEXT("master_tracks"), TEXT("index.json")), MasterTracksJson));

	const FString RemoveBindingBody = MakeExecRequestBody(
		TEXT("smoke-sequence-folder-remove-binding-001"),
		TEXT("sequence_remove_actor_binding"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"binding_guid\":\"%s\",\"save_after_set\":false}"), *SequenceAssetPath, *BindingGuid));
	FHttpSmokeResult RemoveBindingResult;
	TestTrue(TEXT("Sequence folder remove binding request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, RemoveBindingBody, RemoveBindingResult));
	TestEqual(TEXT("Sequence folder remove binding status code"), RemoveBindingResult.StatusCode, 200);

	const FString RemoveComponentBindingBody = MakeExecRequestBody(
		TEXT("smoke-sequence-folder-remove-binding-002"),
		TEXT("sequence_remove_actor_binding"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"binding_guid\":\"%s\",\"save_after_set\":false}"), *SequenceAssetPath, *ComponentBindingGuid));
	FHttpSmokeResult RemoveComponentBindingResult;
	TestTrue(TEXT("Sequence folder remove component binding request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, RemoveComponentBindingBody, RemoveComponentBindingResult));
	TestEqual(TEXT("Sequence folder remove component binding status code"), RemoveComponentBindingResult.StatusCode, 200);

	const FString RemoveSpawnableBindingBody = MakeExecRequestBody(
		TEXT("smoke-sequence-folder-remove-binding-003"),
		TEXT("sequence_remove_actor_binding"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"binding_guid\":\"%s\",\"save_after_set\":false}"), *SequenceAssetPath, *SpawnableBindingGuid));
	FHttpSmokeResult RemoveSpawnableBindingResult;
	TestTrue(TEXT("Sequence folder remove spawnable binding request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, RemoveSpawnableBindingBody, RemoveSpawnableBindingResult));
	TestEqual(TEXT("Sequence folder remove spawnable binding status code"), RemoveSpawnableBindingResult.StatusCode, 200);

	const FString RemoveSkeletalBindingBody = MakeExecRequestBody(
		TEXT("smoke-sequence-folder-remove-binding-004"),
		TEXT("sequence_remove_actor_binding"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"binding_guid\":\"%s\",\"save_after_set\":false}"), *SequenceAssetPath, *SkeletalBindingGuid));
	FHttpSmokeResult RemoveSkeletalBindingResult;
	TestTrue(TEXT("Sequence folder remove skeletal binding request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, RemoveSkeletalBindingBody, RemoveSkeletalBindingResult));
	TestEqual(TEXT("Sequence folder remove skeletal binding status code"), RemoveSkeletalBindingResult.StatusCode, 200);

	const FString ApplyFolderBody = MakeExecRequestBody(
		TEXT("smoke-sequence-folder-apply-001"),
		TEXT("sequence_apply_folder"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"create_if_missing\":false,\"save_after_apply\":false,\"replace_mode\":\"patch\",\"verify_after_apply\":true}"), *SequenceAssetPath));
	FHttpSmokeResult ApplyFolderResult;
	TestTrue(TEXT("Sequence folder apply request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ApplyFolderBody, ApplyFolderResult));
	TestEqual(TEXT("Sequence folder apply status code"), ApplyFolderResult.StatusCode, 200);

	TSharedPtr<FJsonObject> ApplyFolderJson;
	TestTrue(TEXT("Sequence folder apply parses JSON"), ParseJson(ApplyFolderResult.Body, ApplyFolderJson));
	const TSharedPtr<FJsonObject>* ApplyFolderData = nullptr;
	TestTrue(TEXT("Sequence folder apply contains data"), ApplyFolderJson.IsValid() && ApplyFolderJson->TryGetObjectField(TEXT("data"), ApplyFolderData) && ApplyFolderData && ApplyFolderData->IsValid());
	if (ApplyFolderData && ApplyFolderData->IsValid())
	{
		TestEqual(TEXT("Sequence folder apply replace mode"), (*ApplyFolderData)->GetStringField(TEXT("replace_mode")), FString(TEXT("patch")));
		TestTrue(TEXT("Sequence folder apply raw property count field exists"), (*ApplyFolderData)->HasField(TEXT("raw_properties_applied")));
		TestEqual(TEXT("Sequence folder apply raw property count is zero for default export"), static_cast<int32>((*ApplyFolderData)->GetIntegerField(TEXT("raw_properties_applied"))), 0);
		TestTrue(TEXT("Sequence folder apply grouped details count field exists"), (*ApplyFolderData)->HasField(TEXT("grouped_details_applied")));
		TestTrue(TEXT("Sequence folder apply channel count field exists"), (*ApplyFolderData)->HasField(TEXT("channels_applied")));
		TestTrue(TEXT("Sequence folder apply channel count positive"), static_cast<int32>((*ApplyFolderData)->GetIntegerField(TEXT("channels_applied"))) > 0);
		const TSharedPtr<FJsonObject>* InputCoverageReport = nullptr;
		TestTrue(TEXT("Sequence folder apply input coverage report exists"), (*ApplyFolderData)->TryGetObjectField(TEXT("input_coverage_report"), InputCoverageReport) && InputCoverageReport && InputCoverageReport->IsValid());
		if (InputCoverageReport && InputCoverageReport->IsValid())
		{
			TestEqual(TEXT("Sequence folder apply input coverage schema"), (*InputCoverageReport)->GetStringField(TEXT("schema")), FString(TEXT("uai.sequence.folder_coverage.v1")));
			TestTrue(TEXT("Sequence folder apply input coverage has tracks"), static_cast<int32>((*InputCoverageReport)->GetIntegerField(TEXT("track_count"))) > 0);
		}
		const TSharedPtr<FJsonObject>* PostApplyCoverageReport = nullptr;
		TestTrue(TEXT("Sequence folder apply post coverage report exists"), (*ApplyFolderData)->TryGetObjectField(TEXT("post_apply_coverage_report"), PostApplyCoverageReport) && PostApplyCoverageReport && PostApplyCoverageReport->IsValid());
		if (PostApplyCoverageReport && PostApplyCoverageReport->IsValid())
		{
			TestEqual(TEXT("Sequence folder apply post coverage schema"), (*PostApplyCoverageReport)->GetStringField(TEXT("schema")), FString(TEXT("uai.sequence.folder_coverage.v1")));
			TestTrue(TEXT("Sequence folder apply post coverage has tracks"), static_cast<int32>((*PostApplyCoverageReport)->GetIntegerField(TEXT("track_count"))) > 0);
		}
		const TSharedPtr<FJsonObject>* CoverageDiff = nullptr;
		TestTrue(TEXT("Sequence folder apply coverage diff exists"), (*ApplyFolderData)->TryGetObjectField(TEXT("coverage_diff"), CoverageDiff) && CoverageDiff && CoverageDiff->IsValid());
		if (CoverageDiff && CoverageDiff->IsValid())
		{
			TestEqual(TEXT("Sequence folder apply coverage diff schema"), (*CoverageDiff)->GetStringField(TEXT("schema")), FString(TEXT("uai.sequence.folder_coverage_diff.v1")));
			TestTrue(TEXT("Sequence folder apply coverage diff has post report"), (*CoverageDiff)->GetBoolField(TEXT("has_post_apply_report")));
		}
		const TSharedPtr<FJsonObject>* StructuralDiff = nullptr;
		TestTrue(TEXT("Sequence folder apply structural diff exists"), (*ApplyFolderData)->TryGetObjectField(TEXT("structural_diff"), StructuralDiff) && StructuralDiff && StructuralDiff->IsValid());
		if (StructuralDiff && StructuralDiff->IsValid())
		{
			TestEqual(TEXT("Sequence folder apply structural diff schema"), (*StructuralDiff)->GetStringField(TEXT("schema")), FString(TEXT("uai.sequence.folder_structural_diff.v1")));
			TestTrue(TEXT("Sequence folder apply structural diff succeeded"), (*StructuralDiff)->GetBoolField(TEXT("success")));
			TestEqual(TEXT("Sequence folder apply structural diff missing count"), static_cast<int32>((*StructuralDiff)->GetIntegerField(TEXT("missing_entry_count"))), 0);
			TestEqual(TEXT("Sequence folder apply structural diff extra count"), static_cast<int32>((*StructuralDiff)->GetIntegerField(TEXT("extra_entry_count"))), 0);
			TestEqual(TEXT("Sequence folder apply structural diff changed count"), static_cast<int32>((*StructuralDiff)->GetIntegerField(TEXT("changed_entry_count"))), 0);
			const bool bStructurallyEqual = (*StructuralDiff)->GetBoolField(TEXT("is_structurally_equal"));
			if (!bStructurallyEqual)
			{
				FString StructuralDiffText;
				const TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
					TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&StructuralDiffText);
				FJsonSerializer::Serialize((*StructuralDiff).ToSharedRef(), Writer);
				AddError(FString::Printf(TEXT("Sequence folder structural diff detail: %s"), *StructuralDiffText.Left(20000)));
			}
			TestTrue(TEXT("Sequence folder apply structural diff equal"), bStructurallyEqual);
		}
	}

	if (FApp::CanEverRender())
	{
		const FString ProbeControlRigCandidatesBody = MakeExecRequestBody(
			TEXT("smoke-sequence-control-rig-probe-001"),
			TEXT("probe_ik_goal_candidates"),
			FString::Printf(
				TEXT("{\"sequence_path\":\"%s\",\"binding_guid\":\"%s\",\"control_name\":\"%s\",\"time_seconds\":0.5,\"baseline_from_skeletal_pose\":true,\"baseline_control_bone_map\":{\"%s\":\"%s\"},\"frame_width\":128,\"frame_height\":128,\"max_size\":128,\"frame_sheet_max_size\":768,\"write_individual_frames\":false,\"create_frame_sheet\":true,\"candidates\":[{\"label\":\"same\",\"transform\":{\"location\":{\"x\":0.0,\"y\":0.0,\"z\":0.0}}},{\"label\":\"forward\",\"transform\":{\"location\":{\"x\":24.0,\"y\":0.0,\"z\":0.0}}},{\"label\":\"back\",\"transform\":{\"location\":{\"x\":-24.0,\"y\":0.0,\"z\":0.0}}},{\"label\":\"side_pos\",\"transform\":{\"location\":{\"x\":0.0,\"y\":24.0,\"z\":0.0}}},{\"label\":\"side_neg\",\"transform\":{\"location\":{\"x\":0.0,\"y\":-24.0,\"z\":0.0}}},{\"label\":\"up\",\"transform\":{\"location\":{\"x\":0.0,\"y\":0.0,\"z\":24.0}}},{\"label\":\"down\",\"transform\":{\"location\":{\"x\":0.0,\"y\":0.0,\"z\":-24.0}}},{\"label\":\"yaw_scale\",\"transform\":{\"rotation\":{\"pitch\":0.0,\"yaw\":25.0,\"roll\":0.0},\"scale\":{\"x\":1.08,\"y\":1.0,\"z\":1.0}}}]}"),
				*SequenceAssetPath,
				*SkeletalBindingGuid,
				*ControlRigSmokeControlName,
				*ControlRigSmokeControlName,
				*ControlRigSmokeBoneName));
		FHttpSmokeResult ProbeControlRigCandidatesResult;
		TestTrue(TEXT("Sequence control rig probe candidates request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ProbeControlRigCandidatesBody, ProbeControlRigCandidatesResult, 60.0));
		TestEqual(TEXT("Sequence control rig probe candidates status code"), ProbeControlRigCandidatesResult.StatusCode, 200);

		TSharedPtr<FJsonObject> ProbeControlRigCandidatesJson;
		TestTrue(TEXT("Sequence control rig probe candidates parses JSON"), ParseJson(ProbeControlRigCandidatesResult.Body, ProbeControlRigCandidatesJson));
		TestTrue(TEXT("Sequence control rig probe candidates ok"), ProbeControlRigCandidatesJson.IsValid() && ProbeControlRigCandidatesJson->GetBoolField(TEXT("ok")));
		const TSharedPtr<FJsonObject>* ProbeControlRigCandidatesData = nullptr;
		TestTrue(TEXT("Sequence control rig probe candidates contains data"), ProbeControlRigCandidatesJson.IsValid() && ProbeControlRigCandidatesJson->TryGetObjectField(TEXT("data"), ProbeControlRigCandidatesData) && ProbeControlRigCandidatesData && ProbeControlRigCandidatesData->IsValid());
		if (ProbeControlRigCandidatesData && ProbeControlRigCandidatesData->IsValid())
		{
			TestEqual(TEXT("Sequence control rig probe schema"), (*ProbeControlRigCandidatesData)->GetStringField(TEXT("schema")), FString(TEXT("uai.probe.ik_goal_candidates.v2")));
			TestEqual(TEXT("Sequence control rig probe context"), (*ProbeControlRigCandidatesData)->GetStringField(TEXT("context_mode")), FString(TEXT("sequence_control_rig")));
			TestEqual(TEXT("Sequence control rig probe sequence"), (*ProbeControlRigCandidatesData)->GetStringField(TEXT("sequence_path")), SequenceAssetPath);
			TestEqual(TEXT("Sequence control rig probe binding"), (*ProbeControlRigCandidatesData)->GetStringField(TEXT("binding_guid")), SkeletalBindingGuid);
			TestEqual(TEXT("Sequence control rig probe control"), (*ProbeControlRigCandidatesData)->GetStringField(TEXT("control_name")), ControlRigSmokeControlName);
			TestEqual(TEXT("Sequence control rig probe candidate count"), static_cast<int32>((*ProbeControlRigCandidatesData)->GetIntegerField(TEXT("candidate_count"))), 8);
			TestEqual(TEXT("Sequence control rig probe frame count includes original"), static_cast<int32>((*ProbeControlRigCandidatesData)->GetIntegerField(TEXT("frame_count"))), 9);
			TestEqual(TEXT("Sequence control rig probe transform semantics"), (*ProbeControlRigCandidatesData)->GetStringField(TEXT("candidate_transform_semantics")), FString(TEXT("relative_transform_from_original_sequence_control_at_frame")));
			TestEqual(TEXT("Sequence control rig probe baseline mode"), (*ProbeControlRigCandidatesData)->GetStringField(TEXT("baseline_mode")), FString(TEXT("current_sequence_frame_control_transform")));
			TestEqual(TEXT("Sequence control rig probe baseline control count"), static_cast<int32>((*ProbeControlRigCandidatesData)->GetIntegerField(TEXT("baseline_control_count"))), 0);
			TestFalse(TEXT("Sequence control rig probe does not write keys"), (*ProbeControlRigCandidatesData)->GetBoolField(TEXT("writes_sequence_keys")));
			TestFalse(TEXT("Sequence control rig probe does not duplicate sequence"), (*ProbeControlRigCandidatesData)->GetBoolField(TEXT("duplicates_sequence")));
			TestTrue(TEXT("Sequence control rig probe restored"), (*ProbeControlRigCandidatesData)->GetBoolField(TEXT("restored")));
			TestTrue(TEXT("Sequence control rig probe cache available"), (*ProbeControlRigCandidatesData)->GetBoolField(TEXT("candidate_cache_available")));
			TestEqual(TEXT("Sequence control rig probe cache request id"), (*ProbeControlRigCandidatesData)->GetStringField(TEXT("candidate_cache_request_id")), FString(TEXT("smoke-sequence-control-rig-probe-001")));

			const FString ProbeControlRigSheetPath = (*ProbeControlRigCandidatesData)->GetStringField(TEXT("frame_sheet_file_path"));
			TestFalse(TEXT("Sequence control rig probe frame sheet path non-empty"), ProbeControlRigSheetPath.IsEmpty());
			TestTrue(TEXT("Sequence control rig probe frame sheet file exists"), IFileManager::Get().FileExists(*ProbeControlRigSheetPath));

			const TArray<TSharedPtr<FJsonValue>>* ProbeControlRigFrames = nullptr;
			TestTrue(TEXT("Sequence control rig probe frames array exists"), (*ProbeControlRigCandidatesData)->TryGetArrayField(TEXT("frames"), ProbeControlRigFrames) && ProbeControlRigFrames != nullptr);
			if (ProbeControlRigFrames)
			{
				TestEqual(TEXT("Sequence control rig probe frames array count"), ProbeControlRigFrames->Num(), 9);
				const TSharedPtr<FJsonObject>* FirstProbeControlRigFrame = nullptr;
				TestTrue(TEXT("Sequence control rig probe first frame parses"), (*ProbeControlRigFrames)[0].IsValid() && (*ProbeControlRigFrames)[0]->TryGetObject(FirstProbeControlRigFrame) && FirstProbeControlRigFrame && FirstProbeControlRigFrame->IsValid());
				if (FirstProbeControlRigFrame && FirstProbeControlRigFrame->IsValid())
				{
					TestEqual(TEXT("Sequence control rig probe first frame label"), (*FirstProbeControlRigFrame)->GetStringField(TEXT("frame_label")), FString(TEXT("F0")));
					TestEqual(TEXT("Sequence control rig probe first frame kind"), (*FirstProbeControlRigFrame)->GetStringField(TEXT("kind")), FString(TEXT("original")));
				}
			}
		}

		const FString ReviewControlRigProbeFramesBody = MakeExecRequestBody(
			TEXT("smoke-sequence-control-rig-probe-review-001"),
			TEXT("probe_ik_goal_review_frames"),
			TEXT("{\"probe_request_id\":\"smoke-sequence-control-rig-probe-001\",\"frames\":[0,2,5],\"frame_sheet_max_size\":768,\"frame_sheet_label_scale\":1}"));
		FHttpSmokeResult ReviewControlRigProbeFramesResult;
		TestTrue(TEXT("Sequence control rig probe review frames request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ReviewControlRigProbeFramesBody, ReviewControlRigProbeFramesResult, 60.0));
		TestEqual(TEXT("Sequence control rig probe review frames status code"), ReviewControlRigProbeFramesResult.StatusCode, 200);

		TSharedPtr<FJsonObject> ReviewControlRigProbeFramesJson;
		TestTrue(TEXT("Sequence control rig probe review frames parses JSON"), ParseJson(ReviewControlRigProbeFramesResult.Body, ReviewControlRigProbeFramesJson));
		TestTrue(TEXT("Sequence control rig probe review frames ok"), ReviewControlRigProbeFramesJson.IsValid() && ReviewControlRigProbeFramesJson->GetBoolField(TEXT("ok")));
		const TSharedPtr<FJsonObject>* ReviewControlRigProbeFramesData = nullptr;
		TestTrue(TEXT("Sequence control rig probe review frames contains data"), ReviewControlRigProbeFramesJson.IsValid() && ReviewControlRigProbeFramesJson->TryGetObjectField(TEXT("data"), ReviewControlRigProbeFramesData) && ReviewControlRigProbeFramesData && ReviewControlRigProbeFramesData->IsValid());
		if (ReviewControlRigProbeFramesData && ReviewControlRigProbeFramesData->IsValid())
		{
			TestEqual(TEXT("Sequence control rig probe review schema"), (*ReviewControlRigProbeFramesData)->GetStringField(TEXT("schema")), FString(TEXT("uai.probe.ik_goal_review_frames.v1")));
			TestEqual(TEXT("Sequence control rig probe review source request id"), (*ReviewControlRigProbeFramesData)->GetStringField(TEXT("source_probe_request_id")), FString(TEXT("smoke-sequence-control-rig-probe-001")));
			TestEqual(TEXT("Sequence control rig probe review selected frame count"), static_cast<int32>((*ReviewControlRigProbeFramesData)->GetIntegerField(TEXT("selected_frame_count"))), 3);
			TestEqual(TEXT("Sequence control rig probe review selection semantics"), (*ReviewControlRigProbeFramesData)->GetStringField(TEXT("selection_semantics")), FString(TEXT("frame_index_matches_source_sheet_label")));
			TestFalse(TEXT("Sequence control rig probe review does not write keys"), (*ReviewControlRigProbeFramesData)->GetBoolField(TEXT("writes_sequence_keys")));
			TestFalse(TEXT("Sequence control rig probe review does not evaluate sequence"), (*ReviewControlRigProbeFramesData)->GetBoolField(TEXT("evaluates_sequence")));

			const FString ReviewControlRigSheetPath = (*ReviewControlRigProbeFramesData)->GetStringField(TEXT("frame_sheet_file_path"));
			TestFalse(TEXT("Sequence control rig probe review frame sheet path non-empty"), ReviewControlRigSheetPath.IsEmpty());
			TestTrue(TEXT("Sequence control rig probe review frame sheet file exists"), IFileManager::Get().FileExists(*ReviewControlRigSheetPath));

			const TArray<TSharedPtr<FJsonValue>>* ReviewControlRigFrames = nullptr;
			TestTrue(TEXT("Sequence control rig probe review frames array exists"), (*ReviewControlRigProbeFramesData)->TryGetArrayField(TEXT("frames"), ReviewControlRigFrames) && ReviewControlRigFrames != nullptr);
			if (ReviewControlRigFrames && ReviewControlRigFrames->Num() >= 3)
			{
				const TSharedPtr<FJsonObject>* FirstReviewFrame = nullptr;
				const TSharedPtr<FJsonObject>* ThirdReviewFrame = nullptr;
				TestTrue(TEXT("Sequence control rig probe review first frame parses"), (*ReviewControlRigFrames)[0].IsValid() && (*ReviewControlRigFrames)[0]->TryGetObject(FirstReviewFrame) && FirstReviewFrame && FirstReviewFrame->IsValid());
				TestTrue(TEXT("Sequence control rig probe review third frame parses"), (*ReviewControlRigFrames)[2].IsValid() && (*ReviewControlRigFrames)[2]->TryGetObject(ThirdReviewFrame) && ThirdReviewFrame && ThirdReviewFrame->IsValid());
				if (FirstReviewFrame && FirstReviewFrame->IsValid())
				{
					TestEqual(TEXT("Sequence control rig probe review first source label"), (*FirstReviewFrame)->GetStringField(TEXT("source_frame_label")), FString(TEXT("F0")));
				}
				if (ThirdReviewFrame && ThirdReviewFrame->IsValid())
				{
					TestEqual(TEXT("Sequence control rig probe review third source label"), (*ThirdReviewFrame)->GetStringField(TEXT("source_frame_label")), FString(TEXT("F5")));
				}
			}
		}

		const FString ApplyControlRigProbeCandidateBody = MakeExecRequestBody(
			TEXT("smoke-sequence-control-rig-probe-apply-001"),
			TEXT("probe_ik_goal_apply_candidate"),
			FString::Printf(
				TEXT("{\"probe_request_id\":\"smoke-sequence-control-rig-probe-001\",\"sequence_path\":\"%s\",\"binding_guid\":\"%s\",\"control_name\":\"%s\",\"frame_index\":1,\"save_after_apply\":false}"),
				*SequenceAssetPath,
				*SkeletalBindingGuid,
				*ControlRigSmokeControlName));
		FHttpSmokeResult ApplyControlRigProbeCandidateResult;
		TestTrue(TEXT("Sequence control rig probe apply candidate request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ApplyControlRigProbeCandidateBody, ApplyControlRigProbeCandidateResult));
		TestEqual(TEXT("Sequence control rig probe apply candidate status code"), ApplyControlRigProbeCandidateResult.StatusCode, 200);

		TSharedPtr<FJsonObject> ApplyControlRigProbeCandidateJson;
		TestTrue(TEXT("Sequence control rig probe apply candidate parses JSON"), ParseJson(ApplyControlRigProbeCandidateResult.Body, ApplyControlRigProbeCandidateJson));
		TestTrue(TEXT("Sequence control rig probe apply candidate ok"), ApplyControlRigProbeCandidateJson.IsValid() && ApplyControlRigProbeCandidateJson->GetBoolField(TEXT("ok")));
		const TSharedPtr<FJsonObject>* ApplyControlRigProbeCandidateData = nullptr;
		TestTrue(TEXT("Sequence control rig probe apply candidate contains data"), ApplyControlRigProbeCandidateJson.IsValid() && ApplyControlRigProbeCandidateJson->TryGetObjectField(TEXT("data"), ApplyControlRigProbeCandidateData) && ApplyControlRigProbeCandidateData && ApplyControlRigProbeCandidateData->IsValid());
		if (ApplyControlRigProbeCandidateData && ApplyControlRigProbeCandidateData->IsValid())
		{
			TestEqual(TEXT("Sequence control rig probe apply schema"), (*ApplyControlRigProbeCandidateData)->GetStringField(TEXT("schema")), FString(TEXT("uai.probe.ik_goal_apply_candidate.sequence_control_rig.v1")));
			TestEqual(TEXT("Sequence control rig probe apply sequence"), (*ApplyControlRigProbeCandidateData)->GetStringField(TEXT("sequence_path")), SequenceAssetPath);
			TestEqual(TEXT("Sequence control rig probe apply binding"), (*ApplyControlRigProbeCandidateData)->GetStringField(TEXT("binding_guid")), SkeletalBindingGuid);
			TestEqual(TEXT("Sequence control rig probe apply control"), (*ApplyControlRigProbeCandidateData)->GetStringField(TEXT("control_name")), ControlRigSmokeControlName);
			TestEqual(TEXT("Sequence control rig probe apply frame index"), static_cast<int32>((*ApplyControlRigProbeCandidateData)->GetIntegerField(TEXT("applied_frame_index"))), 1);
			TestEqual(TEXT("Sequence control rig probe apply candidate index"), static_cast<int32>((*ApplyControlRigProbeCandidateData)->GetIntegerField(TEXT("applied_candidate_index"))), 0);
			TestEqual(TEXT("Sequence control rig probe apply baseline mode"), (*ApplyControlRigProbeCandidateData)->GetStringField(TEXT("baseline_mode")), FString(TEXT("current_sequence_frame_control_transform")));
			TestFalse(TEXT("Sequence control rig probe apply baseline controls applied"), (*ApplyControlRigProbeCandidateData)->GetBoolField(TEXT("baseline_controls_applied")));
			TestTrue(TEXT("Sequence control rig probe apply readback matches"), (*ApplyControlRigProbeCandidateData)->GetBoolField(TEXT("applied_matches_selected")));
		}

		bool bCorruptedControlRigChannelMap = false;
		if (ULevelSequence* ProbeSequence = LoadObject<ULevelSequence>(nullptr, *SequenceAssetPath))
		{
			if (UMovieScene* ProbeMovieScene = ProbeSequence->GetMovieScene())
			{
				FGuid ProbeBindingGuid;
				if (FGuid::Parse(SkeletalBindingGuid, ProbeBindingGuid))
				{
					const TArray<UMovieSceneTrack*> ProbeTracks = ProbeMovieScene->FindTracks(UMovieSceneControlRigParameterTrack::StaticClass(), ProbeBindingGuid, NAME_None);
					const FName ProbeControlName(*ControlRigSmokeControlName);
					for (UMovieSceneTrack* ProbeTrack : ProbeTracks)
					{
						UMovieSceneControlRigParameterTrack* ProbeControlRigTrack = Cast<UMovieSceneControlRigParameterTrack>(ProbeTrack);
						if (!ProbeControlRigTrack)
						{
							continue;
						}

						for (UMovieSceneSection* ProbeSectionBase : ProbeControlRigTrack->GetAllSections())
						{
							UMovieSceneControlRigParameterSection* ProbeSection = Cast<UMovieSceneControlRigParameterSection>(ProbeSectionBase);
							if (!ProbeSection)
							{
								continue;
							}

							ProbeSection->ReconstructChannelProxy();
							ProbeSection->GetChannelProxy().GetChannels<FMovieSceneFloatChannel>();
							if (FChannelMapInfo* ChannelInfo = ProbeSection->ControlChannelMap.Find(ProbeControlName))
							{
								ChannelInfo->ChannelIndex = TNumericLimits<int32>::Min() / 2;
								bCorruptedControlRigChannelMap = true;
								break;
							}
						}

						if (bCorruptedControlRigChannelMap)
						{
							break;
						}
					}
				}
			}
		}
		TestTrue(TEXT("Sequence control rig probe channel map corruption fixture applied"), bCorruptedControlRigChannelMap);

		const FString ApplyControlRigProbeCandidateAfterBadMapBody = MakeExecRequestBody(
			TEXT("smoke-sequence-control-rig-probe-apply-bad-channel-map-001"),
			TEXT("probe_ik_goal_apply_candidate"),
			FString::Printf(
				TEXT("{\"probe_request_id\":\"smoke-sequence-control-rig-probe-001\",\"sequence_path\":\"%s\",\"binding_guid\":\"%s\",\"control_name\":\"%s\",\"frame_index\":2,\"save_after_apply\":false}"),
				*SequenceAssetPath,
				*SkeletalBindingGuid,
				*ControlRigSmokeControlName));
		FHttpSmokeResult ApplyControlRigProbeCandidateAfterBadMapResult;
		TestTrue(TEXT("Sequence control rig probe apply survives bad channel map request"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ApplyControlRigProbeCandidateAfterBadMapBody, ApplyControlRigProbeCandidateAfterBadMapResult));
		TestEqual(TEXT("Sequence control rig probe apply survives bad channel map status code"), ApplyControlRigProbeCandidateAfterBadMapResult.StatusCode, 200);

		TSharedPtr<FJsonObject> ApplyControlRigProbeCandidateAfterBadMapJson;
		TestTrue(TEXT("Sequence control rig probe apply survives bad channel map parses JSON"), ParseJson(ApplyControlRigProbeCandidateAfterBadMapResult.Body, ApplyControlRigProbeCandidateAfterBadMapJson));
		TestTrue(TEXT("Sequence control rig probe apply survives bad channel map ok"), ApplyControlRigProbeCandidateAfterBadMapJson.IsValid() && ApplyControlRigProbeCandidateAfterBadMapJson->GetBoolField(TEXT("ok")));
		const TSharedPtr<FJsonObject>* ApplyControlRigProbeCandidateAfterBadMapData = nullptr;
		TestTrue(TEXT("Sequence control rig probe apply survives bad channel map contains data"), ApplyControlRigProbeCandidateAfterBadMapJson.IsValid() && ApplyControlRigProbeCandidateAfterBadMapJson->TryGetObjectField(TEXT("data"), ApplyControlRigProbeCandidateAfterBadMapData) && ApplyControlRigProbeCandidateAfterBadMapData && ApplyControlRigProbeCandidateAfterBadMapData->IsValid());
		if (ApplyControlRigProbeCandidateAfterBadMapData && ApplyControlRigProbeCandidateAfterBadMapData->IsValid())
		{
			TestEqual(TEXT("Sequence control rig probe apply bad channel map frame index"), static_cast<int32>((*ApplyControlRigProbeCandidateAfterBadMapData)->GetIntegerField(TEXT("applied_frame_index"))), 2);
			TestTrue(TEXT("Sequence control rig probe apply bad channel map readback matches"), (*ApplyControlRigProbeCandidateAfterBadMapData)->GetBoolField(TEXT("applied_matches_selected")));
		}

		const FString RestoreControlRigProbeCandidateBody = MakeExecRequestBody(
			TEXT("smoke-sequence-control-rig-probe-apply-restore-001"),
			TEXT("probe_ik_goal_apply_candidate"),
			FString::Printf(
				TEXT("{\"probe_request_id\":\"smoke-sequence-control-rig-probe-001\",\"sequence_path\":\"%s\",\"binding_guid\":\"%s\",\"control_name\":\"%s\",\"frame_index\":1,\"save_after_apply\":false}"),
				*SequenceAssetPath,
				*SkeletalBindingGuid,
				*ControlRigSmokeControlName));
		FHttpSmokeResult RestoreControlRigProbeCandidateResult;
		TestTrue(TEXT("Sequence control rig probe restore request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, RestoreControlRigProbeCandidateBody, RestoreControlRigProbeCandidateResult));
		TestEqual(TEXT("Sequence control rig probe restore status code"), RestoreControlRigProbeCandidateResult.StatusCode, 200);

		TSharedPtr<FJsonObject> RestoreControlRigProbeCandidateJson;
		TestTrue(TEXT("Sequence control rig probe restore parses JSON"), ParseJson(RestoreControlRigProbeCandidateResult.Body, RestoreControlRigProbeCandidateJson));
		TestTrue(TEXT("Sequence control rig probe restore ok"), RestoreControlRigProbeCandidateJson.IsValid() && RestoreControlRigProbeCandidateJson->GetBoolField(TEXT("ok")));
	}
	else
	{
		AddInfo(TEXT("Skipping sequence Control Rig probe smoke because rendering is disabled in the current automation environment."));
	}

	const FString ReExportBody = MakeExecRequestBody(
		TEXT("smoke-sequence-folder-export-002"),
		TEXT("sequence_export_folder"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"clean_output_dir\":true,\"include_validation\":true}"), *SequenceAssetPath));
	FHttpSmokeResult ReExportResult;
	TestTrue(TEXT("Sequence folder re-export request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ReExportBody, ReExportResult));
	TestEqual(TEXT("Sequence folder re-export status code"), ReExportResult.StatusCode, 200);

	TSharedPtr<FJsonObject> VerifySettingsJson;
	TestTrue(TEXT("Sequence folder verify settings load"), LoadJsonFile(FPaths::Combine(FolderPath, TEXT("settings"), TEXT("sequence.json")), VerifySettingsJson));
	if (VerifySettingsJson.IsValid())
	{
		const TSharedPtr<FJsonObject>* PlaybackObj = nullptr;
		TestTrue(TEXT("Sequence folder verify playback exists"), VerifySettingsJson->TryGetObjectField(TEXT("playback"), PlaybackObj) && PlaybackObj && (*PlaybackObj).IsValid());
		if (PlaybackObj && (*PlaybackObj).IsValid())
		{
			TestEqual(TEXT("Sequence folder playback start updated"), static_cast<float>((*PlaybackObj)->GetNumberField(TEXT("start_seconds"))), 0.25f);
			TestEqual(TEXT("Sequence folder playback duration updated"), static_cast<float>((*PlaybackObj)->GetNumberField(TEXT("duration_seconds"))), 4.0f);
		}
		const TSharedPtr<FJsonObject>* DisplayRateObj = nullptr;
		TestTrue(TEXT("Sequence folder verify display rate exists"), VerifySettingsJson->TryGetObjectField(TEXT("display_rate"), DisplayRateObj) && DisplayRateObj && (*DisplayRateObj).IsValid());
		if (DisplayRateObj && (*DisplayRateObj).IsValid())
		{
			TestEqual(TEXT("Sequence folder display rate updated"), static_cast<int32>((*DisplayRateObj)->GetIntegerField(TEXT("numerator"))), 12);
		}
	}

	TSharedPtr<FJsonObject> VerifyBindingsIndexJson;
	TestTrue(TEXT("Sequence folder verify bindings index load"), LoadJsonFile(FPaths::Combine(FolderPath, TEXT("bindings"), TEXT("index.json")), VerifyBindingsIndexJson));
	TMap<FString, FString> VerifyBindingFolderByGuid;
	if (VerifyBindingsIndexJson.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Bindings = nullptr;
		TestTrue(TEXT("Sequence folder verify bindings index array exists"), VerifyBindingsIndexJson->TryGetArrayField(TEXT("bindings"), Bindings) && Bindings && Bindings->Num() >= 5);
		if (Bindings)
		{
			for (const TSharedPtr<FJsonValue>& BindingValue : *Bindings)
			{
				const TSharedPtr<FJsonObject>* BindingObj = nullptr;
				if (!BindingValue.IsValid() || !BindingValue->TryGetObject(BindingObj) || !BindingObj || !(*BindingObj).IsValid())
				{
					continue;
				}
				VerifyBindingFolderByGuid.Add((*BindingObj)->GetStringField(TEXT("binding_guid")), (*BindingObj)->GetStringField(TEXT("folder_name")));
			}
		}
	}
	const FString VerifyActorBindingDir = FPaths::Combine(FolderPath, TEXT("bindings"), VerifyBindingFolderByGuid.FindRef(BindingGuid), TEXT("tracks"));
	const FString VerifyComponentBindingDir = FPaths::Combine(FolderPath, TEXT("bindings"), VerifyBindingFolderByGuid.FindRef(ComponentBindingGuid), TEXT("tracks"));
	const FString VerifySpawnableBindingDir = FPaths::Combine(FolderPath, TEXT("bindings"), VerifyBindingFolderByGuid.FindRef(SpawnableBindingGuid), TEXT("tracks"));
	const FString VerifySkeletalBindingDir = FPaths::Combine(FolderPath, TEXT("bindings"), VerifyBindingFolderByGuid.FindRef(SkeletalBindingGuid), TEXT("tracks"));
	TestTrue(TEXT("Sequence folder verify actor binding dir exists"), !VerifyBindingFolderByGuid.FindRef(BindingGuid).IsEmpty());
	TestTrue(TEXT("Sequence folder verify component binding dir exists"), !VerifyBindingFolderByGuid.FindRef(ComponentBindingGuid).IsEmpty());
	TestTrue(TEXT("Sequence folder verify spawnable binding dir exists"), !VerifyBindingFolderByGuid.FindRef(SpawnableBindingGuid).IsEmpty());
	TestTrue(TEXT("Sequence folder verify skeletal binding dir exists"), !VerifyBindingFolderByGuid.FindRef(SkeletalBindingGuid).IsEmpty());

	auto VerifyTrackFiles = [this, &LoadJsonFile, &ComponentBindingGuid, &SpawnableBindingGuid](const FString& TracksDir, const FString& ExpectedBindingGuid, bool& bOutTransform, bool& bOutFloat, bool& bOutSpawn)
	{
		TArray<FString> TrackFiles;
		IFileManager::Get().FindFiles(TrackFiles, *(FPaths::Combine(TracksDir, TEXT("*.json"))), true, false);
		TrackFiles.Sort();
		for (const FString& TrackFile : TrackFiles)
		{
			TSharedPtr<FJsonObject> TrackJson;
			TestTrue(FString::Printf(TEXT("Sequence folder verify track load: %s"), *TrackFile), LoadJsonFile(FPaths::Combine(TracksDir, TrackFile), TrackJson));
			if (!TrackJson.IsValid())
			{
				continue;
			}
			const FString TrackType = TrackJson->GetStringField(TEXT("track_type"));
			if (TrackType == TEXT("transform"))
			{
				const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
				if (TrackJson->TryGetArrayField(TEXT("sections"), Sections) && Sections && Sections->Num() == 1)
				{
					const TSharedPtr<FJsonObject>* SectionObj = nullptr;
					if ((*Sections)[0]->TryGetObject(SectionObj) && SectionObj && (*SectionObj).IsValid())
					{
						const TArray<TSharedPtr<FJsonValue>>* Keys = nullptr;
						if ((*SectionObj)->TryGetArrayField(TEXT("keys"), Keys) && Keys && Keys->Num() == 1)
						{
							const TSharedPtr<FJsonObject>* KeyObj = nullptr;
							if ((*Keys)[0]->TryGetObject(KeyObj) && KeyObj && (*KeyObj).IsValid())
							{
								const TSharedPtr<FJsonObject>* LocationObj = nullptr;
								if ((*KeyObj)->TryGetObjectField(TEXT("location"), LocationObj) && LocationObj && (*LocationObj).IsValid())
								{
									const float ExpectedX = ExpectedBindingGuid == SpawnableBindingGuid ? 55.0f : 110.0f;
									bOutTransform = FMath::IsNearlyEqual(static_cast<float>((*LocationObj)->GetNumberField(TEXT("x"))), ExpectedX, 0.01f);
								}
							}
						}
					}
				}
			}
			else if (TrackType == TEXT("property") && TrackJson->GetStringField(TEXT("property_type")) == TEXT("float"))
			{
				const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
				if (TrackJson->TryGetArrayField(TEXT("sections"), Sections) && Sections && Sections->Num() == 1)
				{
					const TSharedPtr<FJsonObject>* SectionObj = nullptr;
					if ((*Sections)[0]->TryGetObject(SectionObj) && SectionObj && (*SectionObj).IsValid())
					{
						const TArray<TSharedPtr<FJsonValue>>* Keys = nullptr;
						if ((*SectionObj)->TryGetArrayField(TEXT("keys"), Keys) && Keys && Keys->Num() == 1)
						{
							const TSharedPtr<FJsonObject>* KeyObj = nullptr;
							if ((*Keys)[0]->TryGetObject(KeyObj) && KeyObj && (*KeyObj).IsValid())
							{
								const float ExpectedValue = ExpectedBindingGuid == ComponentBindingGuid ? 1.75f : 2.5f;
								bOutFloat = FMath::IsNearlyEqual(static_cast<float>((*KeyObj)->GetNumberField(TEXT("value"))), ExpectedValue, 0.01f);
							}
						}
					}
				}
			}
			else if (TrackType == TEXT("spawn"))
			{
				const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
				if (TrackJson->TryGetArrayField(TEXT("sections"), Sections) && Sections && Sections->Num() == 1)
				{
					const TSharedPtr<FJsonObject>* SectionObj = nullptr;
					if ((*Sections)[0]->TryGetObject(SectionObj) && SectionObj && (*SectionObj).IsValid())
					{
						const TArray<TSharedPtr<FJsonValue>>* Keys = nullptr;
						if ((*SectionObj)->TryGetArrayField(TEXT("keys"), Keys) && Keys && Keys->Num() >= 2)
						{
							const TSharedPtr<FJsonObject>* KeyObj = nullptr;
							if ((*Keys)[1]->TryGetObject(KeyObj) && KeyObj && (*KeyObj).IsValid())
							{
								bOutSpawn = FMath::IsNearlyEqual(static_cast<float>((*KeyObj)->GetNumberField(TEXT("time_seconds"))), 2.5f, 0.01f);
							}
						}
					}
				}
			}
		}
	};

	auto VerifyControlRigTrackFiles = [this, &LoadJsonFile, &ControlRigSmokeControlName](const FString& TracksDir) -> bool
	{
		TArray<FString> TrackFiles;
		IFileManager::Get().FindFiles(TrackFiles, *(FPaths::Combine(TracksDir, TEXT("*.json"))), true, false);
		TrackFiles.Sort();

		bool bFoundControlRigTrack = false;
		bool bFoundControlRigKey = false;
		for (const FString& TrackFile : TrackFiles)
		{
			TSharedPtr<FJsonObject> TrackJson;
			TestTrue(FString::Printf(TEXT("Sequence folder verify control rig track load: %s"), *TrackFile), LoadJsonFile(FPaths::Combine(TracksDir, TrackFile), TrackJson));
			if (!TrackJson.IsValid())
			{
				continue;
			}

			FString TrackType;
			if (!TrackJson->TryGetStringField(TEXT("track_type"), TrackType) || TrackType != TEXT("control_rig"))
			{
				continue;
			}

			bFoundControlRigTrack = true;
			const FString ControlRigClass = TrackJson->GetStringField(TEXT("control_rig_class"));
			TestTrue(TEXT("Sequence folder control rig class is FKControlRig"), ControlRigClass.Contains(TEXT("FKControlRig")));

			const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
			if (!TrackJson->TryGetArrayField(TEXT("sections"), Sections) || !Sections)
			{
				continue;
			}

			for (const TSharedPtr<FJsonValue>& SectionValue : *Sections)
			{
				const TSharedPtr<FJsonObject>* SectionObj = nullptr;
				if (!SectionValue.IsValid() || !SectionValue->TryGetObject(SectionObj) || !SectionObj || !(*SectionObj).IsValid())
				{
					continue;
				}

				const TArray<TSharedPtr<FJsonValue>>* Keys = nullptr;
				if (!(*SectionObj)->TryGetArrayField(TEXT("keys"), Keys) || !Keys)
				{
					continue;
				}

				for (const TSharedPtr<FJsonValue>& KeyValue : *Keys)
				{
					const TSharedPtr<FJsonObject>* KeyObj = nullptr;
					if (!KeyValue.IsValid() || !KeyValue->TryGetObject(KeyObj) || !KeyObj || !(*KeyObj).IsValid())
					{
						continue;
					}

					FString ControlName;
					if (!(*KeyObj)->TryGetStringField(TEXT("control"), ControlName) || ControlName != ControlRigSmokeControlName)
					{
						continue;
					}

					const TSharedPtr<FJsonObject>* ValueObj = nullptr;
					const TSharedPtr<FJsonObject>* LocationObj = nullptr;
					if ((*KeyObj)->TryGetObjectField(TEXT("value"), ValueObj) && ValueObj && (*ValueObj).IsValid() &&
						(*ValueObj)->TryGetObjectField(TEXT("location"), LocationObj) && LocationObj && (*LocationObj).IsValid())
					{
						bFoundControlRigKey = FMath::IsNearlyEqual(static_cast<float>((*LocationObj)->GetNumberField(TEXT("x"))), 12.0f, 0.01f);
					}
				}
			}
		}

		return bFoundControlRigTrack && bFoundControlRigKey;
	};

	bool bVerifiedActorTransform = false;
	bool bVerifiedActorFloat = false;
	bool bVerifiedComponentFloat = false;
	bool bVerifiedSpawnableTransform = false;
	bool bVerifiedSpawnTrack = false;
	VerifyTrackFiles(VerifyActorBindingDir, BindingGuid, bVerifiedActorTransform, bVerifiedActorFloat, bVerifiedSpawnTrack);
	VerifyTrackFiles(VerifyComponentBindingDir, ComponentBindingGuid, bVerifiedSpawnableTransform, bVerifiedComponentFloat, bVerifiedSpawnTrack);
	VerifyTrackFiles(VerifySpawnableBindingDir, SpawnableBindingGuid, bVerifiedSpawnableTransform, bVerifiedComponentFloat, bVerifiedSpawnTrack);
	TestTrue(TEXT("Sequence folder actor transform key updated"), bVerifiedActorTransform);
	TestTrue(TEXT("Sequence folder actor float key updated"), bVerifiedActorFloat);
	TestTrue(TEXT("Sequence folder component float key updated"), bVerifiedComponentFloat);
	TestTrue(TEXT("Sequence folder spawnable transform key updated"), bVerifiedSpawnableTransform);
	TestTrue(TEXT("Sequence folder spawn track updated"), bVerifiedSpawnTrack);
	TestTrue(TEXT("Sequence folder control rig key updated"), VerifyControlRigTrackFiles(VerifySkeletalBindingDir));

	TSharedPtr<FJsonObject> VerifyMasterTracksJson;
	TestTrue(TEXT("Sequence folder verify master tracks load"), LoadJsonFile(FPaths::Combine(FolderPath, TEXT("master_tracks"), TEXT("index.json")), VerifyMasterTracksJson));
	bool bVerifiedCameraCutTrack = false;
	bool bVerifiedSubSequenceTrack = false;
	if (VerifyMasterTracksJson.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* MasterTracks = nullptr;
		TestTrue(TEXT("Sequence folder verify master tracks array exists"), VerifyMasterTracksJson->TryGetArrayField(TEXT("tracks"), MasterTracks) && MasterTracks != nullptr);
		if (MasterTracks)
		{
			for (const TSharedPtr<FJsonValue>& TrackValue : *MasterTracks)
			{
				const TSharedPtr<FJsonObject>* TrackObj = nullptr;
				if (!TrackValue.IsValid() || !TrackValue->TryGetObject(TrackObj) || !TrackObj || !(*TrackObj).IsValid())
				{
					continue;
				}
				const FString TrackType = (*TrackObj)->GetStringField(TEXT("track_type"));
				if (TrackType == TEXT("camera_cut"))
				{
					const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
					if ((*TrackObj)->TryGetArrayField(TEXT("sections"), Sections) && Sections && Sections->Num() == 1)
					{
						const TSharedPtr<FJsonObject>* SectionObj = nullptr;
						if ((*Sections)[0]->TryGetObject(SectionObj) && SectionObj && (*SectionObj).IsValid())
						{
							bVerifiedCameraCutTrack =
								(*SectionObj)->GetStringField(TEXT("camera_binding_guid")) == CameraBindingGuid &&
								FMath::IsNearlyEqual(static_cast<float>((*SectionObj)->GetNumberField(TEXT("duration_seconds"))), 2.0f, 0.01f);
						}
					}
				}
				else if (TrackType == TEXT("sub_sequence"))
				{
					const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
					if ((*TrackObj)->TryGetArrayField(TEXT("sections"), Sections) && Sections && Sections->Num() == 1)
					{
						const TSharedPtr<FJsonObject>* SectionObj = nullptr;
						if ((*Sections)[0]->TryGetObject(SectionObj) && SectionObj && (*SectionObj).IsValid())
						{
							const FString ExportedSequenceAsset = (*SectionObj)->GetStringField(TEXT("sequence_asset"));
							bVerifiedSubSequenceTrack =
								ExportedSequenceAsset.StartsWith(ChildSequenceAssetPath) &&
								FMath::IsNearlyEqual(static_cast<float>((*SectionObj)->GetNumberField(TEXT("start_seconds"))), 2.0f, 0.01f);
						}
					}
				}
			}
		}
	}
	TestTrue(TEXT("Sequence folder camera cut track updated"), bVerifiedCameraCutTrack);
	TestTrue(TEXT("Sequence folder sub sequence track updated"), bVerifiedSubSequenceTrack);

	const FString DestroyActorBody = MakeExecRequestBody(
		TEXT("smoke-sequence-folder-destroy-001"),
		TEXT("destroy_actor"),
		FString::Printf(TEXT("{\"id\":\"%s\"}"), *ActorLabel));
	FHttpSmokeResult DestroyActorResult;
	TestTrue(TEXT("Sequence folder destroy actor request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, DestroyActorBody, DestroyActorResult));
	TestEqual(TEXT("Sequence folder destroy actor status code"), DestroyActorResult.StatusCode, 200);

	if (DirectCameraActor)
	{
		DirectCameraActor->Destroy();
	}
	if (DirectSkeletalActor)
	{
		DirectSkeletalActor->Destroy();
	}

	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentInterfaceSequenceCinematicShotFolderSmokeTest,
	"GptProjectTest.UeAgentInterface.Smoke.SequenceCinematicShotFolderWorkflow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUeAgentInterfaceSequenceCinematicShotFolderSmokeTest::RunTest(const FString& Parameters)
{
	FScopedUeAgentHttpServer ServerScope;
	FString InitError;
	if (!ServerScope.Initialize(InitError))
	{
		AddError(FString::Printf(TEXT("Initialize failed: %s"), *InitError));
		return false;
	}

	const FString SequenceAssetPath = MakeAutomationAssetPath(TEXT("SEQCINESHOT"));
	const FString ChildSequenceAssetPath = MakeAutomationAssetPath(TEXT("SEQCINESUB"));
	const FString SequenceObjectPath = FString::Printf(TEXT("%s.%s"), *SequenceAssetPath, *FPackageName::GetLongPackageAssetName(SequenceAssetPath));
	const FString FolderRoot = FPaths::ConvertRelativePathToFull(
		FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UeAssetFolders"), TEXT("LevelSequence")));
	const FString FolderPath = FPaths::Combine(FolderRoot, SequenceAssetPath.RightChop(1));
	const FString ShotDisplayName = TEXT("SmokeShot_A");

	auto LoadJsonFile = [this](const FString& FilePath, TSharedPtr<FJsonObject>& OutObj) -> bool
	{
		FString JsonText;
		if (!FFileHelper::LoadFileToString(JsonText, *FilePath))
		{
			AddError(FString::Printf(TEXT("Load file failed: %s"), *FilePath));
			return false;
		}
		const TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(JsonText);
		if (!FJsonSerializer::Deserialize(Reader, OutObj) || !OutObj.IsValid())
		{
			AddError(FString::Printf(TEXT("Parse json failed: %s"), *FilePath));
			return false;
		}
		return true;
	};

	auto SaveJsonFile = [this](const FString& FilePath, const TSharedPtr<FJsonObject>& Obj) -> bool
	{
		FString JsonText;
		const TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&JsonText);
		if (!FJsonSerializer::Serialize(Obj.ToSharedRef(), Writer))
		{
			AddError(FString::Printf(TEXT("Serialize json failed: %s"), *FilePath));
			return false;
		}
		if (!FFileHelper::SaveStringToFile(JsonText, *FilePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			AddError(FString::Printf(TEXT("Save file failed: %s"), *FilePath));
			return false;
		}
		return true;
	};

	const FString CreateSequenceBody = MakeExecRequestBody(
		TEXT("smoke-sequence-cinematic-shot-create-001"),
		TEXT("sequence_create_level_sequence"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"start_seconds\":0.0,\"duration_seconds\":3.0,\"display_rate_num\":24,\"display_rate_den\":1,\"open_editor\":false,\"save_after_create\":false}"), *SequenceAssetPath));
	FHttpSmokeResult CreateSequenceResult;
	TestTrue(TEXT("Sequence cinematic shot create request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, CreateSequenceBody, CreateSequenceResult));
	TestEqual(TEXT("Sequence cinematic shot create status code"), CreateSequenceResult.StatusCode, 200);

	const FString CreateChildSequenceBody = MakeExecRequestBody(
		TEXT("smoke-sequence-cinematic-shot-create-child-001"),
		TEXT("sequence_create_level_sequence"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"start_seconds\":0.0,\"duration_seconds\":1.0,\"display_rate_num\":24,\"display_rate_den\":1,\"open_editor\":false,\"save_after_create\":false}"), *ChildSequenceAssetPath));
	FHttpSmokeResult CreateChildSequenceResult;
	TestTrue(TEXT("Sequence cinematic shot create child request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, CreateChildSequenceBody, CreateChildSequenceResult));
	TestEqual(TEXT("Sequence cinematic shot create child status code"), CreateChildSequenceResult.StatusCode, 200);

	const FString ExportFolderBody = MakeExecRequestBody(
		TEXT("smoke-sequence-cinematic-shot-export-001"),
		TEXT("sequence_export_folder"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"clean_output_dir\":true,\"include_validation\":true}"), *SequenceAssetPath));
	FHttpSmokeResult ExportFolderResult;
	TestTrue(TEXT("Sequence cinematic shot export request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ExportFolderBody, ExportFolderResult));
	TestEqual(TEXT("Sequence cinematic shot export status code"), ExportFolderResult.StatusCode, 200);

	TSharedPtr<FJsonObject> MasterTracksJson;
	TestTrue(TEXT("Sequence cinematic shot master tracks load"), LoadJsonFile(FPaths::Combine(FolderPath, TEXT("master_tracks"), TEXT("index.json")), MasterTracksJson));
	if (MasterTracksJson.IsValid())
	{
		TArray<TSharedPtr<FJsonValue>> MasterTracks;
		TSharedPtr<FJsonObject> ShotTrackObj = MakeShared<FJsonObject>();
		ShotTrackObj->SetStringField(TEXT("track_type"), TEXT("cinematic_shot"));
		TSharedPtr<FJsonObject> ShotSectionObj = MakeShared<FJsonObject>();
		ShotSectionObj->SetStringField(TEXT("section_type"), TEXT("cinematic_shot"));
		ShotSectionObj->SetStringField(TEXT("sequence_asset"), ChildSequenceAssetPath);
		ShotSectionObj->SetStringField(TEXT("shot_display_name"), ShotDisplayName);
		ShotSectionObj->SetNumberField(TEXT("start_seconds"), 0.5);
		ShotSectionObj->SetNumberField(TEXT("duration_seconds"), 1.25);
		ShotSectionObj->SetNumberField(TEXT("row_index"), 0);
		ShotSectionObj->SetNumberField(TEXT("start_frame_offset"), 0);
		ShotSectionObj->SetBoolField(TEXT("can_loop"), false);
		ShotSectionObj->SetNumberField(TEXT("end_frame_offset"), 0);
		ShotSectionObj->SetNumberField(TEXT("first_loop_start_frame_offset"), 0);
		ShotSectionObj->SetNumberField(TEXT("hierarchical_bias"), 100);
		ShotSectionObj->SetNumberField(TEXT("network_mask"), 0);
		ShotSectionObj->SetStringField(TEXT("time_scale_type"), TEXT("fixed"));
		ShotSectionObj->SetNumberField(TEXT("time_scale"), 1.0);
		ShotTrackObj->SetArrayField(TEXT("sections"), { MakeShared<FJsonValueObject>(ShotSectionObj) });
		MasterTracks.Add(MakeShared<FJsonValueObject>(ShotTrackObj));
		MasterTracksJson->SetArrayField(TEXT("tracks"), MasterTracks);
	}
	TestTrue(TEXT("Sequence cinematic shot master tracks save"), SaveJsonFile(FPaths::Combine(FolderPath, TEXT("master_tracks"), TEXT("index.json")), MasterTracksJson));

	const FString ApplyFolderBody = MakeExecRequestBody(
		TEXT("smoke-sequence-cinematic-shot-apply-001"),
		TEXT("sequence_apply_folder"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"create_if_missing\":false,\"save_after_apply\":false}"), *SequenceAssetPath));
	FHttpSmokeResult ApplyFolderResult;
	TestTrue(TEXT("Sequence cinematic shot apply request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ApplyFolderBody, ApplyFolderResult));
	TestEqual(TEXT("Sequence cinematic shot apply status code"), ApplyFolderResult.StatusCode, 200);

	TSharedPtr<FJsonObject> ApplyFolderJson;
	TestTrue(TEXT("Sequence cinematic shot apply response parses JSON"), ParseJson(ApplyFolderResult.Body, ApplyFolderJson));
	TestTrue(TEXT("Sequence cinematic shot apply root ok=true"), ApplyFolderJson.IsValid() && ApplyFolderJson->GetBoolField(TEXT("ok")));

	const FString ReExportBody = MakeExecRequestBody(
		TEXT("smoke-sequence-cinematic-shot-export-002"),
		TEXT("sequence_export_folder"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"clean_output_dir\":true,\"include_validation\":true}"), *SequenceAssetPath));
	FHttpSmokeResult ReExportResult;
	TestTrue(TEXT("Sequence cinematic shot re-export request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ReExportBody, ReExportResult));
	TestEqual(TEXT("Sequence cinematic shot re-export status code"), ReExportResult.StatusCode, 200);

	ULevelSequence* SequenceAsset = LoadObject<ULevelSequence>(nullptr, *SequenceObjectPath);
	TestNotNull(TEXT("Sequence cinematic shot direct load sequence asset"), SequenceAsset);
	UMovieScene* MovieScene = SequenceAsset ? SequenceAsset->GetMovieScene() : nullptr;
	TestNotNull(TEXT("Sequence cinematic shot direct movie scene"), MovieScene);

	UMovieSceneCinematicShotTrack* ShotTrack = MovieScene ? MovieScene->FindTrack<UMovieSceneCinematicShotTrack>() : nullptr;
	TestNotNull(TEXT("Sequence cinematic shot direct track exists"), ShotTrack);
	if (ShotTrack)
	{
		const TArray<UMovieSceneSection*>& ShotSections = ShotTrack->GetAllSections();
		TestEqual(TEXT("Sequence cinematic shot direct section count"), ShotSections.Num(), 1);
		if (ShotSections.Num() == 1)
		{
			UMovieSceneCinematicShotSection* ShotSection = Cast<UMovieSceneCinematicShotSection>(ShotSections[0]);
			TestNotNull(TEXT("Sequence cinematic shot direct section type"), ShotSection);
			if (ShotSection && MovieScene)
			{
				const FString ExportedSequenceAsset = ShotSection->GetSequence() ? ShotSection->GetSequence()->GetPathName() : FString();
				TestTrue(TEXT("Sequence cinematic shot direct child sequence path"), ExportedSequenceAsset.StartsWith(ChildSequenceAssetPath));
				TestEqual(TEXT("Sequence cinematic shot direct display name"), ShotSection->GetShotDisplayName(), ShotDisplayName);
				const FFrameRate TickResolution = MovieScene->GetTickResolution();
				const float StartSeconds = static_cast<float>(TickResolution.AsSeconds(FFrameTime(ShotSection->GetInclusiveStartFrame())));
				const float DurationSeconds = static_cast<float>(TickResolution.AsSeconds(FFrameTime(ShotSection->GetExclusiveEndFrame() - ShotSection->GetInclusiveStartFrame())));
				TestTrue(TEXT("Sequence cinematic shot direct start seconds"), FMath::IsNearlyEqual(StartSeconds, 0.5f, 0.01f));
				TestTrue(TEXT("Sequence cinematic shot direct duration seconds"), FMath::IsNearlyEqual(DurationSeconds, 1.25f, 0.01f));
			}
		}
	}

	TSharedPtr<FJsonObject> VerifyMasterTracksJson;
	TestTrue(TEXT("Sequence cinematic shot verify master tracks load"), LoadJsonFile(FPaths::Combine(FolderPath, TEXT("master_tracks"), TEXT("index.json")), VerifyMasterTracksJson));
	bool bVerifiedCinematicShotTrack = false;
	if (VerifyMasterTracksJson.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* MasterTracks = nullptr;
		TestTrue(TEXT("Sequence cinematic shot verify master tracks array exists"), VerifyMasterTracksJson->TryGetArrayField(TEXT("tracks"), MasterTracks) && MasterTracks != nullptr);
		if (MasterTracks)
		{
			for (const TSharedPtr<FJsonValue>& TrackValue : *MasterTracks)
			{
				const TSharedPtr<FJsonObject>* TrackObj = nullptr;
				if (!TrackValue.IsValid() || !TrackValue->TryGetObject(TrackObj) || !TrackObj || !(*TrackObj).IsValid())
				{
					continue;
				}

				if ((*TrackObj)->GetStringField(TEXT("track_type")) != TEXT("cinematic_shot"))
				{
					continue;
				}

				const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
				if (!(*TrackObj)->TryGetArrayField(TEXT("sections"), Sections) || !Sections || Sections->Num() != 1)
				{
					continue;
				}

				const TSharedPtr<FJsonObject>* SectionObj = nullptr;
				if (!(*Sections)[0]->TryGetObject(SectionObj) || !SectionObj || !(*SectionObj).IsValid())
				{
					continue;
				}

				const FString ExportedSequenceAsset = (*SectionObj)->GetStringField(TEXT("sequence_asset"));
				bVerifiedCinematicShotTrack =
					(*SectionObj)->GetStringField(TEXT("shot_display_name")) == ShotDisplayName &&
					ExportedSequenceAsset.StartsWith(ChildSequenceAssetPath) &&
					FMath::IsNearlyEqual(static_cast<float>((*SectionObj)->GetNumberField(TEXT("start_seconds"))), 0.5f, 0.01f) &&
					FMath::IsNearlyEqual(static_cast<float>((*SectionObj)->GetNumberField(TEXT("duration_seconds"))), 1.25f, 0.01f);
			}
		}
	}
	TestTrue(TEXT("Sequence cinematic shot track updated"), bVerifiedCinematicShotTrack);

	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentInterfaceSequenceOutlinerFoldersSmokeTest,
	"GptProjectTest.UeAgentInterface.Smoke.SequenceOutlinerFoldersWorkflow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUeAgentInterfaceSequenceOutlinerFoldersSmokeTest::RunTest(const FString& Parameters)
{
	FScopedUeAgentHttpServer ServerScope;
	FString InitError;
	if (!ServerScope.Initialize(InitError))
	{
		AddError(FString::Printf(TEXT("Initialize failed: %s"), *InitError));
		return false;
	}

	const FString SequenceAssetPath = MakeAutomationAssetPath(TEXT("SEQOUTLINER"));
	const FString ChildSequenceAssetPath = MakeAutomationAssetPath(TEXT("SEQOUTSUB"));
	const FString ActorLabel = FString::Printf(TEXT("SmokeSeqFolder_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8));
	const FString SequenceObjectPath = FString::Printf(TEXT("%s.%s"), *SequenceAssetPath, *FPackageName::GetLongPackageAssetName(SequenceAssetPath));
	const FString FolderRoot = FPaths::ConvertRelativePathToFull(
		FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UeAssetFolders"), TEXT("LevelSequence")));
	const FString FolderPath = FPaths::Combine(FolderRoot, SequenceAssetPath.RightChop(1));

	auto LoadJsonFile = [this](const FString& FilePath, TSharedPtr<FJsonObject>& OutObj) -> bool
	{
		FString JsonText;
		if (!FFileHelper::LoadFileToString(JsonText, *FilePath))
		{
			AddError(FString::Printf(TEXT("Load file failed: %s"), *FilePath));
			return false;
		}
		const TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(JsonText);
		if (!FJsonSerializer::Deserialize(Reader, OutObj) || !OutObj.IsValid())
		{
			AddError(FString::Printf(TEXT("Parse json failed: %s"), *FilePath));
			return false;
		}
		return true;
	};

	auto SaveJsonFile = [this](const FString& FilePath, const TSharedPtr<FJsonObject>& Obj) -> bool
	{
		FString JsonText;
		const TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&JsonText);
		if (!FJsonSerializer::Serialize(Obj.ToSharedRef(), Writer))
		{
			AddError(FString::Printf(TEXT("Serialize json failed: %s"), *FilePath));
			return false;
		}
		if (!FFileHelper::SaveStringToFile(JsonText, *FilePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			AddError(FString::Printf(TEXT("Save file failed: %s"), *FilePath));
			return false;
		}
		return true;
	};

	const FString SpawnActorBody = MakeExecRequestBody(
		TEXT("smoke-sequence-outliner-spawn-001"),
		TEXT("spawn_actor"),
		FString::Printf(TEXT("{\"class_path\":\"/Script/Engine.StaticMeshActor\",\"label\":\"%s\",\"static_mesh\":\"/Engine/BasicShapes/Cube.Cube\",\"location\":{\"x\":0,\"y\":0,\"z\":100}}"), *ActorLabel));
	FHttpSmokeResult SpawnActorResult;
	TestTrue(TEXT("Sequence outliner spawn actor request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SpawnActorBody, SpawnActorResult));
	TestEqual(TEXT("Sequence outliner spawn actor status code"), SpawnActorResult.StatusCode, 200);

	const FString CreateSequenceBody = MakeExecRequestBody(
		TEXT("smoke-sequence-outliner-create-001"),
		TEXT("sequence_create_level_sequence"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"start_seconds\":0.0,\"duration_seconds\":2.0,\"display_rate_num\":24,\"display_rate_den\":1,\"open_editor\":false,\"save_after_create\":false}"), *SequenceAssetPath));
	FHttpSmokeResult CreateSequenceResult;
	TestTrue(TEXT("Sequence outliner create request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, CreateSequenceBody, CreateSequenceResult));
	TestEqual(TEXT("Sequence outliner create status code"), CreateSequenceResult.StatusCode, 200);

	const FString CreateChildSequenceBody = MakeExecRequestBody(
		TEXT("smoke-sequence-outliner-create-child-001"),
		TEXT("sequence_create_level_sequence"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"start_seconds\":0.0,\"duration_seconds\":1.0,\"display_rate_num\":24,\"display_rate_den\":1,\"open_editor\":false,\"save_after_create\":false}"), *ChildSequenceAssetPath));
	FHttpSmokeResult CreateChildSequenceResult;
	TestTrue(TEXT("Sequence outliner create child request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, CreateChildSequenceBody, CreateChildSequenceResult));
	TestEqual(TEXT("Sequence outliner create child status code"), CreateChildSequenceResult.StatusCode, 200);

	const FString AddBindingBody = MakeExecRequestBody(
		TEXT("smoke-sequence-outliner-bind-001"),
		TEXT("sequence_add_actor_binding"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"actor_id\":\"%s\",\"save_after_set\":false}"), *SequenceAssetPath, *ActorLabel));
	FHttpSmokeResult AddBindingResult;
	TestTrue(TEXT("Sequence outliner add binding request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddBindingBody, AddBindingResult));
	TestEqual(TEXT("Sequence outliner add binding status code"), AddBindingResult.StatusCode, 200);

	TSharedPtr<FJsonObject> AddBindingJson;
	TestTrue(TEXT("Sequence outliner add binding parses JSON"), ParseJson(AddBindingResult.Body, AddBindingJson));
	const TSharedPtr<FJsonObject>* AddBindingData = nullptr;
	TestTrue(TEXT("Sequence outliner add binding contains data"), AddBindingJson.IsValid() && AddBindingJson->TryGetObjectField(TEXT("data"), AddBindingData) && AddBindingData && AddBindingData->IsValid());
	const FString BindingGuid = (AddBindingData && AddBindingData->IsValid()) ? (*AddBindingData)->GetStringField(TEXT("binding_guid")) : FString();
	TestTrue(TEXT("Sequence outliner binding guid non-empty"), !BindingGuid.IsEmpty());

	FGuid BindingGuidValue;
	TestTrue(TEXT("Sequence outliner parse binding guid"), FGuid::Parse(BindingGuid, BindingGuidValue));

	const FString AddTransformTrackBody = MakeExecRequestBody(
		TEXT("smoke-sequence-outliner-transform-track-001"),
		TEXT("sequence_add_transform_track"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"binding_guid\":\"%s\",\"save_after_set\":false}"), *SequenceAssetPath, *BindingGuid));
	FHttpSmokeResult AddTransformTrackResult;
	TestTrue(TEXT("Sequence outliner add transform track request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddTransformTrackBody, AddTransformTrackResult));
	TestEqual(TEXT("Sequence outliner add transform track status code"), AddTransformTrackResult.StatusCode, 200);

	const FString AddTransformKeyBody = MakeExecRequestBody(
		TEXT("smoke-sequence-outliner-transform-key-001"),
		TEXT("sequence_add_transform_key"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"binding_guid\":\"%s\",\"time_seconds\":0.5,\"location\":{\"x\":25,\"y\":0,\"z\":0},\"rotation\":{\"pitch\":0,\"yaw\":15,\"roll\":0},\"scale\":{\"x\":1.0,\"y\":1.0,\"z\":1.0},\"save_after_set\":false}"), *SequenceAssetPath, *BindingGuid));
	FHttpSmokeResult AddTransformKeyResult;
	TestTrue(TEXT("Sequence outliner add transform key request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddTransformKeyBody, AddTransformKeyResult));
	TestEqual(TEXT("Sequence outliner add transform key status code"), AddTransformKeyResult.StatusCode, 200);

	const FString ExportFolderBody = MakeExecRequestBody(
		TEXT("smoke-sequence-outliner-export-001"),
		TEXT("sequence_export_folder"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"clean_output_dir\":true,\"include_validation\":true}"), *SequenceAssetPath));
	FHttpSmokeResult ExportFolderResult;
	TestTrue(TEXT("Sequence outliner export request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ExportFolderBody, ExportFolderResult));
	TestEqual(TEXT("Sequence outliner export status code"), ExportFolderResult.StatusCode, 200);

	TSharedPtr<FJsonObject> BindingsIndexJson;
	TestTrue(TEXT("Sequence outliner bindings index load"), LoadJsonFile(FPaths::Combine(FolderPath, TEXT("bindings"), TEXT("index.json")), BindingsIndexJson));
	FString BindingFolderName;
	if (BindingsIndexJson.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Bindings = nullptr;
		TestTrue(TEXT("Sequence outliner bindings index array exists"), BindingsIndexJson->TryGetArrayField(TEXT("bindings"), Bindings) && Bindings != nullptr);
		if (Bindings)
		{
			for (const TSharedPtr<FJsonValue>& BindingValue : *Bindings)
			{
				const TSharedPtr<FJsonObject>* BindingObj = nullptr;
				if (!BindingValue.IsValid() || !BindingValue->TryGetObject(BindingObj) || !BindingObj || !(*BindingObj).IsValid())
				{
					continue;
				}
				if ((*BindingObj)->GetStringField(TEXT("binding_guid")) == BindingGuid)
				{
					BindingFolderName = (*BindingObj)->GetStringField(TEXT("folder_name"));
					break;
				}
			}
		}
	}
	TestTrue(TEXT("Sequence outliner binding folder name found"), !BindingFolderName.IsEmpty());

	FString TransformTrackFileName;
	if (!BindingFolderName.IsEmpty())
	{
		const FString BindingTracksDir = FPaths::Combine(FolderPath, TEXT("bindings"), BindingFolderName, TEXT("tracks"));
		TArray<FString> TrackFiles;
		IFileManager::Get().FindFiles(TrackFiles, *(FPaths::Combine(BindingTracksDir, TEXT("*.json"))), true, false);
		TrackFiles.Sort();
		for (const FString& TrackFile : TrackFiles)
		{
			TSharedPtr<FJsonObject> TrackJson;
			TestTrue(FString::Printf(TEXT("Sequence outliner load binding track file %s"), *TrackFile), LoadJsonFile(FPaths::Combine(BindingTracksDir, TrackFile), TrackJson));
			if (TrackJson.IsValid() && TrackJson->GetStringField(TEXT("track_type")) == TEXT("transform"))
			{
				TransformTrackFileName = TrackFile;
				break;
			}
		}
	}
	TestTrue(TEXT("Sequence outliner transform track file found"), !TransformTrackFileName.IsEmpty());

	TSharedPtr<FJsonObject> MasterTracksJson;
	TestTrue(TEXT("Sequence outliner master tracks load"), LoadJsonFile(FPaths::Combine(FolderPath, TEXT("master_tracks"), TEXT("index.json")), MasterTracksJson));
	if (MasterTracksJson.IsValid())
	{
		TArray<TSharedPtr<FJsonValue>> MasterTracks;
		TSharedPtr<FJsonObject> SubSequenceTrackObj = MakeShared<FJsonObject>();
		SubSequenceTrackObj->SetStringField(TEXT("track_type"), TEXT("sub_sequence"));
		TSharedPtr<FJsonObject> SubSequenceSectionObj = MakeShared<FJsonObject>();
		SubSequenceSectionObj->SetStringField(TEXT("section_type"), TEXT("sub_sequence"));
		SubSequenceSectionObj->SetStringField(TEXT("sequence_asset"), ChildSequenceAssetPath);
		SubSequenceSectionObj->SetNumberField(TEXT("start_seconds"), 0.25);
		SubSequenceSectionObj->SetNumberField(TEXT("duration_seconds"), 1.0);
		SubSequenceSectionObj->SetNumberField(TEXT("row_index"), 0);
		SubSequenceSectionObj->SetNumberField(TEXT("start_frame_offset"), 0);
		SubSequenceSectionObj->SetBoolField(TEXT("can_loop"), false);
		SubSequenceSectionObj->SetNumberField(TEXT("end_frame_offset"), 0);
		SubSequenceSectionObj->SetNumberField(TEXT("first_loop_start_frame_offset"), 0);
		SubSequenceSectionObj->SetNumberField(TEXT("hierarchical_bias"), 100);
		SubSequenceSectionObj->SetNumberField(TEXT("network_mask"), 0);
		SubSequenceSectionObj->SetStringField(TEXT("time_scale_type"), TEXT("fixed"));
		SubSequenceSectionObj->SetNumberField(TEXT("time_scale"), 1.0);
		SubSequenceTrackObj->SetArrayField(TEXT("sections"), { MakeShared<FJsonValueObject>(SubSequenceSectionObj) });
		MasterTracks.Add(MakeShared<FJsonValueObject>(SubSequenceTrackObj));
		MasterTracksJson->SetArrayField(TEXT("tracks"), MasterTracks);
	}
	TestTrue(TEXT("Sequence outliner master tracks save"), SaveJsonFile(FPaths::Combine(FolderPath, TEXT("master_tracks"), TEXT("index.json")), MasterTracksJson));

	TSharedPtr<FJsonObject> OutlinerFoldersJson = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> FolderEntries;
	{
		TSharedPtr<FJsonObject> RootFolderObj = MakeShared<FJsonObject>();
		RootFolderObj->SetStringField(TEXT("folder_id"), TEXT("Main"));
		RootFolderObj->SetStringField(TEXT("folder_name"), TEXT("Main"));
		RootFolderObj->SetStringField(TEXT("folder_path"), TEXT("Main"));
		RootFolderObj->SetStringField(TEXT("parent_folder_id"), TEXT(""));
		RootFolderObj->SetNumberField(TEXT("sorting_order"), 10);
		TSharedPtr<FJsonObject> RootColorObj = MakeShared<FJsonObject>();
		RootColorObj->SetNumberField(TEXT("r"), 255);
		RootColorObj->SetNumberField(TEXT("g"), 64);
		RootColorObj->SetNumberField(TEXT("b"), 64);
		RootColorObj->SetNumberField(TEXT("a"), 255);
		RootFolderObj->SetObjectField(TEXT("color"), RootColorObj);
		RootFolderObj->SetArrayField(TEXT("child_binding_guids"), { MakeShared<FJsonValueString>(BindingGuid) });
		TSharedPtr<FJsonObject> RootBindingTrackObj = MakeShared<FJsonObject>();
		RootBindingTrackObj->SetStringField(TEXT("binding_guid"), BindingGuid);
		RootBindingTrackObj->SetStringField(TEXT("track_file"), TransformTrackFileName);
		RootBindingTrackObj->SetStringField(TEXT("track_type"), TEXT("transform"));
		RootFolderObj->SetArrayField(TEXT("child_binding_tracks"), { MakeShared<FJsonValueObject>(RootBindingTrackObj) });
		RootFolderObj->SetArrayField(TEXT("child_master_tracks"), {});
		FolderEntries.Add(MakeShared<FJsonValueObject>(RootFolderObj));

		TSharedPtr<FJsonObject> ChildFolderObj = MakeShared<FJsonObject>();
		ChildFolderObj->SetStringField(TEXT("folder_id"), TEXT("Main/Shots"));
		ChildFolderObj->SetStringField(TEXT("folder_name"), TEXT("Shots"));
		ChildFolderObj->SetStringField(TEXT("folder_path"), TEXT("Main/Shots"));
		ChildFolderObj->SetStringField(TEXT("parent_folder_id"), TEXT("Main"));
		ChildFolderObj->SetNumberField(TEXT("sorting_order"), 20);
		TSharedPtr<FJsonObject> ChildColorObj = MakeShared<FJsonObject>();
		ChildColorObj->SetNumberField(TEXT("r"), 64);
		ChildColorObj->SetNumberField(TEXT("g"), 128);
		ChildColorObj->SetNumberField(TEXT("b"), 255);
		ChildColorObj->SetNumberField(TEXT("a"), 255);
		ChildFolderObj->SetObjectField(TEXT("color"), ChildColorObj);
		ChildFolderObj->SetArrayField(TEXT("child_binding_guids"), {});
		ChildFolderObj->SetArrayField(TEXT("child_binding_tracks"), {});
		TSharedPtr<FJsonObject> ChildTrackObj = MakeShared<FJsonObject>();
		ChildTrackObj->SetStringField(TEXT("track_type"), TEXT("sub_sequence"));
		ChildFolderObj->SetArrayField(TEXT("child_master_tracks"), { MakeShared<FJsonValueObject>(ChildTrackObj) });
		FolderEntries.Add(MakeShared<FJsonValueObject>(ChildFolderObj));
	}
	OutlinerFoldersJson->SetArrayField(TEXT("folders"), FolderEntries);
	TestTrue(TEXT("Sequence outliner folders save"), SaveJsonFile(FPaths::Combine(FolderPath, TEXT("outliner"), TEXT("folders.json")), OutlinerFoldersJson));

	const FString ApplyFolderBody = MakeExecRequestBody(
		TEXT("smoke-sequence-outliner-apply-001"),
		TEXT("sequence_apply_folder"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"create_if_missing\":false,\"save_after_apply\":false}"), *SequenceAssetPath));
	FHttpSmokeResult ApplyFolderResult;
	TestTrue(TEXT("Sequence outliner apply request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ApplyFolderBody, ApplyFolderResult));
	TestEqual(TEXT("Sequence outliner apply status code"), ApplyFolderResult.StatusCode, 200);

	TSharedPtr<FJsonObject> ApplyFolderJson;
	TestTrue(TEXT("Sequence outliner apply response parses JSON"), ParseJson(ApplyFolderResult.Body, ApplyFolderJson));
	TestTrue(TEXT("Sequence outliner apply root ok=true"), ApplyFolderJson.IsValid() && ApplyFolderJson->GetBoolField(TEXT("ok")));
	const TSharedPtr<FJsonObject>* ApplyFolderData = nullptr;
	TestTrue(TEXT("Sequence outliner apply contains data"), ApplyFolderJson.IsValid() && ApplyFolderJson->TryGetObjectField(TEXT("data"), ApplyFolderData) && ApplyFolderData && (*ApplyFolderData).IsValid());
	if (ApplyFolderData && (*ApplyFolderData).IsValid())
	{
		TestEqual(TEXT("Sequence outliner folders_applied"), static_cast<int32>((*ApplyFolderData)->GetIntegerField(TEXT("folders_applied"))), 2);
	}

	const FString ReExportBody = MakeExecRequestBody(
		TEXT("smoke-sequence-outliner-export-002"),
		TEXT("sequence_export_folder"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"clean_output_dir\":true,\"include_validation\":true}"), *SequenceAssetPath));
	FHttpSmokeResult ReExportResult;
	TestTrue(TEXT("Sequence outliner re-export request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ReExportBody, ReExportResult));
	TestEqual(TEXT("Sequence outliner re-export status code"), ReExportResult.StatusCode, 200);

	ULevelSequence* SequenceAsset = LoadObject<ULevelSequence>(nullptr, *SequenceObjectPath);
	TestNotNull(TEXT("Sequence outliner direct load sequence asset"), SequenceAsset);
	UMovieScene* MovieScene = SequenceAsset ? SequenceAsset->GetMovieScene() : nullptr;
	TestNotNull(TEXT("Sequence outliner direct movie scene"), MovieScene);
	UMovieSceneSubTrack* SubSequenceTrack = MovieScene ? MovieScene->FindTrack<UMovieSceneSubTrack>() : nullptr;
	TestNotNull(TEXT("Sequence outliner direct sub sequence track"), SubSequenceTrack);
	UMovieScene3DTransformTrack* BindingTransformTrack = nullptr;
	if (MovieScene)
	{
		if (const FMovieSceneBinding* Binding = MovieScene->FindBinding(BindingGuidValue))
		{
			for (UMovieSceneTrack* Track : Binding->GetTracks())
			{
				if (UMovieScene3DTransformTrack* TransformTrack = Cast<UMovieScene3DTransformTrack>(Track))
				{
					BindingTransformTrack = TransformTrack;
					break;
				}
			}
		}
	}
	TestNotNull(TEXT("Sequence outliner direct binding transform track"), BindingTransformTrack);

	TArray<UMovieSceneFolder*> RootFolders;
	if (MovieScene)
	{
		MovieScene->GetRootFolders(RootFolders);
	}
	TestEqual(TEXT("Sequence outliner root folder count"), RootFolders.Num(), 1);
	if (RootFolders.Num() == 1 && RootFolders[0])
	{
		UMovieSceneFolder* RootFolder = RootFolders[0];
		TestEqual(TEXT("Sequence outliner root folder name"), RootFolder->GetFolderName().ToString(), FString(TEXT("Main")));
		TestTrue(TEXT("Sequence outliner root contains actor binding"), RootFolder->GetChildObjectBindings().Contains(BindingGuidValue));
		TestTrue(TEXT("Sequence outliner root contains binding transform track"), BindingTransformTrack && RootFolder->GetChildTracks().Contains(BindingTransformTrack));
#if WITH_EDITORONLY_DATA
		TestEqual(TEXT("Sequence outliner root sorting order"), RootFolder->GetSortingOrder(), 10);
		TestEqual(TEXT("Sequence outliner root color r"), static_cast<int32>(RootFolder->GetFolderColor().R), 255);
#endif
		const TArrayView<UMovieSceneFolder* const> ChildFolders = RootFolder->GetChildFolders();
		TestEqual(TEXT("Sequence outliner child folder count"), static_cast<int32>(ChildFolders.Num()), 1);
		if (ChildFolders.Num() == 1 && ChildFolders[0])
		{
			UMovieSceneFolder* ChildFolder = ChildFolders[0];
			TestEqual(TEXT("Sequence outliner child folder name"), ChildFolder->GetFolderName().ToString(), FString(TEXT("Shots")));
			TestTrue(TEXT("Sequence outliner child contains sub sequence track"), ChildFolder->GetChildTracks().Contains(SubSequenceTrack));
#if WITH_EDITORONLY_DATA
			TestEqual(TEXT("Sequence outliner child sorting order"), ChildFolder->GetSortingOrder(), 20);
			TestEqual(TEXT("Sequence outliner child color b"), static_cast<int32>(ChildFolder->GetFolderColor().B), 255);
#endif
		}
	}

	TSharedPtr<FJsonObject> VerifyOutlinerJson;
	TestTrue(TEXT("Sequence outliner verify folders load"), LoadJsonFile(FPaths::Combine(FolderPath, TEXT("outliner"), TEXT("folders.json")), VerifyOutlinerJson));
	bool bVerifiedRootFolder = false;
	bool bVerifiedChildFolder = false;
	bool bVerifiedRootBindingTrack = false;
	if (VerifyOutlinerJson.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Folders = nullptr;
		TestTrue(TEXT("Sequence outliner verify folders array exists"), VerifyOutlinerJson->TryGetArrayField(TEXT("folders"), Folders) && Folders != nullptr);
		if (Folders)
		{
			for (const TSharedPtr<FJsonValue>& FolderValue : *Folders)
			{
				const TSharedPtr<FJsonObject>* FolderObj = nullptr;
				if (!FolderValue.IsValid() || !FolderValue->TryGetObject(FolderObj) || !FolderObj || !(*FolderObj).IsValid())
				{
					continue;
				}

				const FString FolderId = (*FolderObj)->GetStringField(TEXT("folder_id"));
				if (FolderId == TEXT("Main"))
				{
					const TArray<TSharedPtr<FJsonValue>>* ChildBindings = nullptr;
					if ((*FolderObj)->TryGetArrayField(TEXT("child_binding_guids"), ChildBindings) && ChildBindings && ChildBindings->Num() == 1)
					{
						FString ExportedBindingGuid;
						if ((*ChildBindings)[0].IsValid() && (*ChildBindings)[0]->TryGetString(ExportedBindingGuid))
						{
							bVerifiedRootFolder = ExportedBindingGuid == BindingGuid;
						}
					}
					const TArray<TSharedPtr<FJsonValue>>* ChildBindingTracks = nullptr;
					if ((*FolderObj)->TryGetArrayField(TEXT("child_binding_tracks"), ChildBindingTracks) && ChildBindingTracks && ChildBindingTracks->Num() == 1)
					{
						const TSharedPtr<FJsonObject>* TrackObj = nullptr;
						if ((*ChildBindingTracks)[0].IsValid() && (*ChildBindingTracks)[0]->TryGetObject(TrackObj) && TrackObj && (*TrackObj).IsValid())
						{
							bVerifiedRootBindingTrack =
								(*TrackObj)->GetStringField(TEXT("binding_guid")) == BindingGuid &&
								(*TrackObj)->GetStringField(TEXT("track_file")) == TransformTrackFileName &&
								(*TrackObj)->GetStringField(TEXT("track_type")) == TEXT("transform");
						}
					}
				}
				else if (FolderId == TEXT("Main/Shots"))
				{
					const TArray<TSharedPtr<FJsonValue>>* ChildMasterTracks = nullptr;
					if ((*FolderObj)->TryGetArrayField(TEXT("child_master_tracks"), ChildMasterTracks) && ChildMasterTracks && ChildMasterTracks->Num() == 1)
					{
						const TSharedPtr<FJsonObject>* TrackObj = nullptr;
						if ((*ChildMasterTracks)[0].IsValid() && (*ChildMasterTracks)[0]->TryGetObject(TrackObj) && TrackObj && (*TrackObj).IsValid())
						{
							bVerifiedChildFolder =
								(*FolderObj)->GetStringField(TEXT("parent_folder_id")) == TEXT("Main") &&
								(*TrackObj)->GetStringField(TEXT("track_type")) == TEXT("sub_sequence");
						}
					}
				}
			}
		}
	}
	TestTrue(TEXT("Sequence outliner verify root folder export"), bVerifiedRootFolder);
	TestTrue(TEXT("Sequence outliner verify root binding track export"), bVerifiedRootBindingTrack);
	TestTrue(TEXT("Sequence outliner verify child folder export"), bVerifiedChildFolder);

	const FString DestroyActorBody = MakeExecRequestBody(
		TEXT("smoke-sequence-outliner-destroy-001"),
		TEXT("destroy_actor"),
		FString::Printf(TEXT("{\"id\":\"%s\"}"), *ActorLabel));
	FHttpSmokeResult DestroyActorResult;
	TestTrue(TEXT("Sequence outliner destroy actor request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, DestroyActorBody, DestroyActorResult));
	TestEqual(TEXT("Sequence outliner destroy actor status code"), DestroyActorResult.StatusCode, 200);

	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentInterfaceSequenceBindingSmokeTest,
	"GptProjectTest.UeAgentInterface.Smoke.SequenceBindingWorkflow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUeAgentInterfaceSequenceBindingSmokeTest::RunTest(const FString& Parameters)
{
	FScopedUeAgentHttpServer ServerScope;
	FString InitError;
	if (!ServerScope.Initialize(InitError))
	{
		AddError(FString::Printf(TEXT("Initialize failed: %s"), *InitError));
		return false;
	}

	const FString ActorLabel = FString::Printf(TEXT("SmokeSeqActor_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8));
	const FString SequenceAssetPath = MakeAutomationAssetPath(TEXT("SeqBind"));

	const FString SpawnActorBody = MakeExecRequestBody(
		TEXT("smoke-seqbinding-spawn-001"),
		TEXT("spawn_actor"),
		FString::Printf(TEXT("{\"class_path\":\"/Script/Engine.StaticMeshActor\",\"label\":\"%s\",\"static_mesh\":\"/Engine/BasicShapes/Cube.Cube\",\"location\":{\"x\":256,\"y\":128,\"z\":64}}"), *ActorLabel));
	FHttpSmokeResult SpawnActorResult;
	TestTrue(TEXT("Sequence binding spawn_actor request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SpawnActorBody, SpawnActorResult));
	TestEqual(TEXT("Sequence binding spawn_actor status code"), SpawnActorResult.StatusCode, 200);

	const FString CreateSequenceBody = MakeExecRequestBody(
		TEXT("smoke-seqbinding-create-seq-001"),
		TEXT("sequence_create_level_sequence"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"start_seconds\":0,\"duration_seconds\":2.0,\"display_rate_num\":30,\"display_rate_den\":1,\"open_editor\":false,\"save_after_create\":false}"), *SequenceAssetPath));
	FHttpSmokeResult CreateSequenceResult;
	TestTrue(TEXT("Sequence binding create_level_sequence request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, CreateSequenceBody, CreateSequenceResult));
	TestEqual(TEXT("Sequence binding create_level_sequence status code"), CreateSequenceResult.StatusCode, 200);

	const FString AddBindingBody = MakeExecRequestBody(
		TEXT("smoke-seqbinding-add-binding-001"),
		TEXT("sequence_add_actor_binding"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"actor_id\":\"%s\",\"save_after_set\":false}"), *SequenceAssetPath, *ActorLabel));
	FHttpSmokeResult AddBindingResult;
	TestTrue(TEXT("Sequence binding add_actor_binding request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddBindingBody, AddBindingResult));
	TestEqual(TEXT("Sequence binding add_actor_binding status code"), AddBindingResult.StatusCode, 200);

	TSharedPtr<FJsonObject> AddBindingJson;
	TestTrue(TEXT("Sequence binding add_actor_binding response parses as JSON"), ParseJson(AddBindingResult.Body, AddBindingJson));
	TestTrue(TEXT("Sequence binding add_actor_binding root ok=true"), AddBindingJson.IsValid() && AddBindingJson->GetBoolField(TEXT("ok")));

	FString BindingGuid;
	const TSharedPtr<FJsonObject>* AddBindingData = nullptr;
	TestTrue(TEXT("Sequence binding add_actor_binding contains data object"), AddBindingJson.IsValid() && AddBindingJson->TryGetObjectField(TEXT("data"), AddBindingData) && AddBindingData && AddBindingData->IsValid());
	if (AddBindingData && AddBindingData->IsValid())
	{
		BindingGuid = (*AddBindingData)->GetStringField(TEXT("binding_guid"));
		TestTrue(TEXT("Sequence binding guid non-empty"), !BindingGuid.IsEmpty());
		TestEqual(TEXT("Sequence binding actor label"), (*AddBindingData)->GetStringField(TEXT("actor_label")), ActorLabel);
	}

	const FString AddTransformTrackBody = MakeExecRequestBody(
		TEXT("smoke-seqbinding-add-track-001"),
		TEXT("sequence_add_transform_track"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"binding_guid\":\"%s\",\"save_after_set\":false}"), *SequenceAssetPath, *BindingGuid));
	FHttpSmokeResult AddTransformTrackResult;
	TestTrue(TEXT("Sequence binding add_transform_track request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddTransformTrackBody, AddTransformTrackResult));
	TestEqual(TEXT("Sequence binding add_transform_track status code"), AddTransformTrackResult.StatusCode, 200);

	const FString AddTransformKeyBody = MakeExecRequestBody(
		TEXT("smoke-seqbinding-add-key-001"),
		TEXT("sequence_add_transform_key"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"binding_guid\":\"%s\",\"time_seconds\":0.5,\"location\":{\"x\":512,\"y\":128,\"z\":96},\"rotation\":{\"pitch\":0,\"yaw\":45,\"roll\":0},\"save_after_set\":false}"), *SequenceAssetPath, *BindingGuid));
	FHttpSmokeResult AddTransformKeyResult;
	TestTrue(TEXT("Sequence binding add_transform_key request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddTransformKeyBody, AddTransformKeyResult));
	TestEqual(TEXT("Sequence binding add_transform_key status code"), AddTransformKeyResult.StatusCode, 200);

	TSharedPtr<FJsonObject> AddTransformKeyJson;
	TestTrue(TEXT("Sequence binding add_transform_key response parses as JSON"), ParseJson(AddTransformKeyResult.Body, AddTransformKeyJson));
	TestTrue(TEXT("Sequence binding add_transform_key root ok=true"), AddTransformKeyJson.IsValid() && AddTransformKeyJson->GetBoolField(TEXT("ok")));

	const TSharedPtr<FJsonObject>* AddTransformKeyData = nullptr;
	TestTrue(TEXT("Sequence binding add_transform_key contains data object"), AddTransformKeyJson.IsValid() && AddTransformKeyJson->TryGetObjectField(TEXT("data"), AddTransformKeyData) && AddTransformKeyData && AddTransformKeyData->IsValid());
	if (AddTransformKeyData && AddTransformKeyData->IsValid())
	{
		TestEqual(TEXT("Sequence binding add_transform_key binding guid"), (*AddTransformKeyData)->GetStringField(TEXT("binding_guid")), BindingGuid);
		TestTrue(TEXT("Sequence binding add_transform_key frame number non-negative"), static_cast<int32>((*AddTransformKeyData)->GetIntegerField(TEXT("frame_number"))) >= 0);
	}

	const FString GetInfoAfterAddBody = MakeExecRequestBody(
		TEXT("smoke-seqbinding-info-001"),
		TEXT("sequence_get_level_sequence_info"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *SequenceAssetPath));
	FHttpSmokeResult GetInfoAfterAddResult;
	TestTrue(TEXT("Sequence binding get_info after add request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, GetInfoAfterAddBody, GetInfoAfterAddResult));
	TestEqual(TEXT("Sequence binding get_info after add status code"), GetInfoAfterAddResult.StatusCode, 200);

	TSharedPtr<FJsonObject> GetInfoAfterAddJson;
	TestTrue(TEXT("Sequence binding get_info after add response parses as JSON"), ParseJson(GetInfoAfterAddResult.Body, GetInfoAfterAddJson));
	TestTrue(TEXT("Sequence binding get_info after add root ok=true"), GetInfoAfterAddJson.IsValid() && GetInfoAfterAddJson->GetBoolField(TEXT("ok")));

	bool bFoundBinding = false;
	const TSharedPtr<FJsonObject>* GetInfoAfterAddData = nullptr;
	TestTrue(TEXT("Sequence binding get_info after add contains data object"), GetInfoAfterAddJson.IsValid() && GetInfoAfterAddJson->TryGetObjectField(TEXT("data"), GetInfoAfterAddData) && GetInfoAfterAddData && GetInfoAfterAddData->IsValid());
	if (GetInfoAfterAddData && GetInfoAfterAddData->IsValid())
	{
		TestTrue(TEXT("Sequence binding count after add >= 1"), static_cast<int32>((*GetInfoAfterAddData)->GetIntegerField(TEXT("binding_count"))) >= 1);
		const TArray<TSharedPtr<FJsonValue>>* Bindings = nullptr;
		TestTrue(TEXT("Sequence binding info contains bindings array"), (*GetInfoAfterAddData)->TryGetArrayField(TEXT("bindings"), Bindings) && Bindings != nullptr);
		if (Bindings)
		{
			for (const TSharedPtr<FJsonValue>& BindingValue : *Bindings)
			{
				const TSharedPtr<FJsonObject>* BindingObject = nullptr;
				if (!BindingValue.IsValid() || !BindingValue->TryGetObject(BindingObject) || !BindingObject || !BindingObject->IsValid())
				{
					continue;
				}

				if ((*BindingObject)->GetStringField(TEXT("guid")) == BindingGuid)
				{
					bFoundBinding = true;
					break;
				}
			}
		}
	}
	TestTrue(TEXT("Sequence binding present after add"), bFoundBinding);

	const FString RemoveBindingBody = MakeExecRequestBody(
		TEXT("smoke-seqbinding-remove-binding-001"),
		TEXT("sequence_remove_actor_binding"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"binding_guid\":\"%s\",\"save_after_set\":false}"), *SequenceAssetPath, *BindingGuid));
	FHttpSmokeResult RemoveBindingResult;
	TestTrue(TEXT("Sequence binding remove_actor_binding request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, RemoveBindingBody, RemoveBindingResult));
	TestEqual(TEXT("Sequence binding remove_actor_binding status code"), RemoveBindingResult.StatusCode, 200);

	const FString GetInfoAfterRemoveBody = MakeExecRequestBody(
		TEXT("smoke-seqbinding-info-002"),
		TEXT("sequence_get_level_sequence_info"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *SequenceAssetPath));
	FHttpSmokeResult GetInfoAfterRemoveResult;
	TestTrue(TEXT("Sequence binding get_info after remove request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, GetInfoAfterRemoveBody, GetInfoAfterRemoveResult));
	TestEqual(TEXT("Sequence binding get_info after remove status code"), GetInfoAfterRemoveResult.StatusCode, 200);

	TSharedPtr<FJsonObject> GetInfoAfterRemoveJson;
	TestTrue(TEXT("Sequence binding get_info after remove response parses as JSON"), ParseJson(GetInfoAfterRemoveResult.Body, GetInfoAfterRemoveJson));
	TestTrue(TEXT("Sequence binding get_info after remove root ok=true"), GetInfoAfterRemoveJson.IsValid() && GetInfoAfterRemoveJson->GetBoolField(TEXT("ok")));

	bool bFoundBindingAfterRemove = false;
	const TSharedPtr<FJsonObject>* GetInfoAfterRemoveData = nullptr;
	TestTrue(TEXT("Sequence binding get_info after remove contains data object"), GetInfoAfterRemoveJson.IsValid() && GetInfoAfterRemoveJson->TryGetObjectField(TEXT("data"), GetInfoAfterRemoveData) && GetInfoAfterRemoveData && GetInfoAfterRemoveData->IsValid());
	if (GetInfoAfterRemoveData && GetInfoAfterRemoveData->IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Bindings = nullptr;
		TestTrue(TEXT("Sequence binding info after remove contains bindings array"), (*GetInfoAfterRemoveData)->TryGetArrayField(TEXT("bindings"), Bindings) && Bindings != nullptr);
		if (Bindings)
		{
			for (const TSharedPtr<FJsonValue>& BindingValue : *Bindings)
			{
				const TSharedPtr<FJsonObject>* BindingObject = nullptr;
				if (!BindingValue.IsValid() || !BindingValue->TryGetObject(BindingObject) || !BindingObject || !BindingObject->IsValid())
				{
					continue;
				}

				if ((*BindingObject)->GetStringField(TEXT("guid")) == BindingGuid)
				{
					bFoundBindingAfterRemove = true;
					break;
				}
			}
		}
	}
	TestFalse(TEXT("Sequence binding removed from sequence"), bFoundBindingAfterRemove);

	const FString DestroyActorBody = MakeExecRequestBody(
		TEXT("smoke-seqbinding-destroy-actor-001"),
		TEXT("destroy_actor"),
		FString::Printf(TEXT("{\"id\":\"%s\"}"), *ActorLabel));
	FHttpSmokeResult DestroyActorResult;
	TestTrue(TEXT("Sequence binding destroy source actor request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, DestroyActorBody, DestroyActorResult));
	TestEqual(TEXT("Sequence binding destroy source actor status code"), DestroyActorResult.StatusCode, 200);

	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentInterfaceSequencePropertyTrackSmokeTest,
	"GptProjectTest.UeAgentInterface.Smoke.SequencePropertyTrackWorkflow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUeAgentInterfaceSequencePropertyTrackSmokeTest::RunTest(const FString& Parameters)
{
	FScopedUeAgentHttpServer ServerScope;
	FString InitError;
	if (!ServerScope.Initialize(InitError))
	{
		AddError(FString::Printf(TEXT("Initialize failed: %s"), *InitError));
		return false;
	}

	const FString ActorLabel = FString::Printf(TEXT("SmokeSeqPropActor_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8));
	const FString SequenceAssetPath = MakeAutomationAssetPath(TEXT("SeqProp"));
	const FString BoolPropertyName = TEXT("bActorEnableCollision");
	const FString FloatPropertyName = TEXT("CustomTimeDilation");
	const FString IntegerPropertyName = TEXT("CustomDepthStencilValue");
	const FString IntegerPropertyPath = TEXT("StaticMeshComponent.CustomDepthStencilValue");

	auto FindTrackByClassNeedle = [](const TSharedPtr<FJsonObject>* InfoData, const FString& BindingGuid, const FString& ClassNeedle, TSharedPtr<FJsonObject>& OutTrackObject) -> bool
	{
		if (!InfoData || !InfoData->IsValid())
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* Bindings = nullptr;
		if (!(*InfoData)->TryGetArrayField(TEXT("bindings"), Bindings) || !Bindings)
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& BindingValue : *Bindings)
		{
			const TSharedPtr<FJsonObject>* BindingObject = nullptr;
			if (!BindingValue.IsValid() || !BindingValue->TryGetObject(BindingObject) || !BindingObject || !BindingObject->IsValid())
			{
				continue;
			}

			if ((*BindingObject)->GetStringField(TEXT("guid")) != BindingGuid)
			{
				continue;
			}

			const TArray<TSharedPtr<FJsonValue>>* Tracks = nullptr;
			if (!(*BindingObject)->TryGetArrayField(TEXT("tracks"), Tracks) || !Tracks)
			{
				return false;
			}

			for (const TSharedPtr<FJsonValue>& TrackValue : *Tracks)
			{
				const TSharedPtr<FJsonObject>* TrackObject = nullptr;
				if (!TrackValue.IsValid() || !TrackValue->TryGetObject(TrackObject) || !TrackObject || !TrackObject->IsValid())
				{
					continue;
				}

				if ((*TrackObject)->GetStringField(TEXT("class")).Contains(ClassNeedle))
				{
					OutTrackObject = *TrackObject;
					return true;
				}
			}

			return false;
		}

		return false;
	};

	const FString SpawnActorBody = MakeExecRequestBody(
		TEXT("smoke-seqprop-spawn-001"),
		TEXT("spawn_actor"),
		FString::Printf(TEXT("{\"class_path\":\"/Script/Engine.StaticMeshActor\",\"label\":\"%s\",\"static_mesh\":\"/Engine/BasicShapes/Cube.Cube\",\"location\":{\"x\":640,\"y\":96,\"z\":64}}"), *ActorLabel));
	FHttpSmokeResult SpawnActorResult;
	TestTrue(TEXT("Sequence property spawn_actor request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SpawnActorBody, SpawnActorResult));
	TestEqual(TEXT("Sequence property spawn_actor status code"), SpawnActorResult.StatusCode, 200);

	const FString CreateSequenceBody = MakeExecRequestBody(
		TEXT("smoke-seqprop-create-seq-001"),
		TEXT("sequence_create_level_sequence"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"start_seconds\":0,\"duration_seconds\":2.0,\"display_rate_num\":30,\"display_rate_den\":1,\"open_editor\":false,\"save_after_create\":false}"), *SequenceAssetPath));
	FHttpSmokeResult CreateSequenceResult;
	TestTrue(TEXT("Sequence property create_level_sequence request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, CreateSequenceBody, CreateSequenceResult));
	TestEqual(TEXT("Sequence property create_level_sequence status code"), CreateSequenceResult.StatusCode, 200);

	const FString AddBindingBody = MakeExecRequestBody(
		TEXT("smoke-seqprop-add-binding-001"),
		TEXT("sequence_add_actor_binding"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"actor_id\":\"%s\",\"save_after_set\":false}"), *SequenceAssetPath, *ActorLabel));
	FHttpSmokeResult AddBindingResult;
	TestTrue(TEXT("Sequence property add_actor_binding request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddBindingBody, AddBindingResult));
	TestEqual(TEXT("Sequence property add_actor_binding status code"), AddBindingResult.StatusCode, 200);

	TSharedPtr<FJsonObject> AddBindingJson;
	TestTrue(TEXT("Sequence property add_actor_binding parses JSON"), ParseJson(AddBindingResult.Body, AddBindingJson));
	FString BindingGuid;
	const TSharedPtr<FJsonObject>* AddBindingData = nullptr;
	TestTrue(TEXT("Sequence property add_actor_binding contains data"), AddBindingJson.IsValid() && AddBindingJson->TryGetObjectField(TEXT("data"), AddBindingData) && AddBindingData && AddBindingData->IsValid());
	if (AddBindingData && AddBindingData->IsValid())
	{
		BindingGuid = (*AddBindingData)->GetStringField(TEXT("binding_guid"));
	}
	TestTrue(TEXT("Sequence property binding guid non-empty"), !BindingGuid.IsEmpty());

	const FString AddBoolTrackBody = MakeExecRequestBody(
		TEXT("smoke-seqprop-add-bool-track-001"),
		TEXT("sequence_add_bool_property_track"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"binding_guid\":\"%s\",\"property_name\":\"%s\",\"save_after_set\":false}"), *SequenceAssetPath, *BindingGuid, *BoolPropertyName));
	FHttpSmokeResult AddBoolTrackResult;
	TestTrue(TEXT("Sequence property add_bool_property_track request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddBoolTrackBody, AddBoolTrackResult));
	TestEqual(TEXT("Sequence property add_bool_property_track status code"), AddBoolTrackResult.StatusCode, 200);

	const FString AddBoolKeyBody = MakeExecRequestBody(
		TEXT("smoke-seqprop-add-bool-key-001"),
		TEXT("sequence_add_bool_property_key"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"binding_guid\":\"%s\",\"property_name\":\"%s\",\"time_seconds\":0.25,\"value\":false,\"value_before_key\":true,\"save_after_set\":false}"), *SequenceAssetPath, *BindingGuid, *BoolPropertyName));
	FHttpSmokeResult AddBoolKeyResult;
	TestTrue(TEXT("Sequence property add_bool_property_key request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddBoolKeyBody, AddBoolKeyResult));
	TestEqual(TEXT("Sequence property add_bool_property_key status code"), AddBoolKeyResult.StatusCode, 200);

	const FString AddFloatTrackBody = MakeExecRequestBody(
		TEXT("smoke-seqprop-add-float-track-001"),
		TEXT("sequence_add_float_property_track"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"binding_guid\":\"%s\",\"property_name\":\"%s\",\"save_after_set\":false}"), *SequenceAssetPath, *BindingGuid, *FloatPropertyName));
	FHttpSmokeResult AddFloatTrackResult;
	TestTrue(TEXT("Sequence property add_float_property_track request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddFloatTrackBody, AddFloatTrackResult));
	TestEqual(TEXT("Sequence property add_float_property_track status code"), AddFloatTrackResult.StatusCode, 200);

	const FString AddFloatKeyBody = MakeExecRequestBody(
		TEXT("smoke-seqprop-add-float-key-001"),
		TEXT("sequence_add_float_property_key"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"binding_guid\":\"%s\",\"property_name\":\"%s\",\"time_seconds\":0.5,\"value\":2.0,\"value_before_key\":1.0,\"save_after_set\":false}"), *SequenceAssetPath, *BindingGuid, *FloatPropertyName));
	FHttpSmokeResult AddFloatKeyResult;
	TestTrue(TEXT("Sequence property add_float_property_key request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddFloatKeyBody, AddFloatKeyResult));
	TestEqual(TEXT("Sequence property add_float_property_key status code"), AddFloatKeyResult.StatusCode, 200);

	const FString GetInfoBody = MakeExecRequestBody(
		TEXT("smoke-seqprop-info-001"),
		TEXT("sequence_get_level_sequence_info"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *SequenceAssetPath));
	FHttpSmokeResult GetInfoResult;
	TestTrue(TEXT("Sequence property get_level_sequence_info request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, GetInfoBody, GetInfoResult));
	TestEqual(TEXT("Sequence property get_level_sequence_info status code"), GetInfoResult.StatusCode, 200);

	TSharedPtr<FJsonObject> GetInfoJson;
	TestTrue(TEXT("Sequence property get_level_sequence_info parses JSON"), ParseJson(GetInfoResult.Body, GetInfoJson));
	const TSharedPtr<FJsonObject>* GetInfoData = nullptr;
	TestTrue(TEXT("Sequence property get_level_sequence_info contains data"), GetInfoJson.IsValid() && GetInfoJson->TryGetObjectField(TEXT("data"), GetInfoData) && GetInfoData && GetInfoData->IsValid());
	if (GetInfoData && GetInfoData->IsValid())
	{
		TSharedPtr<FJsonObject> BoolTrackObject;
		TestTrue(TEXT("Sequence bool property track summary exists"), FindTrackByClassNeedle(GetInfoData, BindingGuid, TEXT("MovieSceneBoolTrack"), BoolTrackObject));
		if (BoolTrackObject.IsValid())
		{
			TestEqual(TEXT("Sequence bool property_name matches"), BoolTrackObject->GetStringField(TEXT("property_name")), BoolPropertyName);
			TestEqual(TEXT("Sequence bool property_path matches"), BoolTrackObject->GetStringField(TEXT("property_path")), BoolPropertyName);

			const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
			TestTrue(TEXT("Sequence bool track contains sections"), BoolTrackObject->TryGetArrayField(TEXT("sections"), Sections) && Sections && Sections->Num() > 0);
			if (Sections && Sections->Num() > 0)
			{
				const TSharedPtr<FJsonObject>* SectionObject = nullptr;
				TestTrue(TEXT("Sequence bool section parses"), (*Sections)[0].IsValid() && (*Sections)[0]->TryGetObject(SectionObject) && SectionObject && SectionObject->IsValid());
				if (SectionObject && SectionObject->IsValid())
				{
					TestEqual(TEXT("Sequence bool section type"), (*SectionObject)->GetStringField(TEXT("section_type")), FString(TEXT("bool")));
					TestTrue(TEXT("Sequence bool section has keys"), static_cast<int32>((*SectionObject)->GetIntegerField(TEXT("key_count"))) >= 1);
				}
			}
		}

		TSharedPtr<FJsonObject> FloatTrackObject;
		TestTrue(TEXT("Sequence float property track summary exists"), FindTrackByClassNeedle(GetInfoData, BindingGuid, TEXT("MovieSceneFloatTrack"), FloatTrackObject));
		if (FloatTrackObject.IsValid())
		{
			TestEqual(TEXT("Sequence float property_name matches"), FloatTrackObject->GetStringField(TEXT("property_name")), FloatPropertyName);
			TestEqual(TEXT("Sequence float property_path matches"), FloatTrackObject->GetStringField(TEXT("property_path")), FloatPropertyName);

			const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
			TestTrue(TEXT("Sequence float track contains sections"), FloatTrackObject->TryGetArrayField(TEXT("sections"), Sections) && Sections && Sections->Num() > 0);
			if (Sections && Sections->Num() > 0)
			{
				const TSharedPtr<FJsonObject>* SectionObject = nullptr;
				TestTrue(TEXT("Sequence float section parses"), (*Sections)[0].IsValid() && (*Sections)[0]->TryGetObject(SectionObject) && SectionObject && SectionObject->IsValid());
				if (SectionObject && SectionObject->IsValid())
				{
					TestEqual(TEXT("Sequence float section type"), (*SectionObject)->GetStringField(TEXT("section_type")), FString(TEXT("float")));
					TestTrue(TEXT("Sequence float section has keys"), static_cast<int32>((*SectionObject)->GetIntegerField(TEXT("key_count"))) >= 1);
				}
			}
		}

		const FString AddIntegerTrackBody = MakeExecRequestBody(
			TEXT("smoke-seqprop-add-int-track-001"),
			TEXT("sequence_add_integer_property_track"),
			FString::Printf(TEXT("{\"asset_path\":\"%s\",\"binding_guid\":\"%s\",\"property_name\":\"%s\",\"property_path\":\"%s\",\"save_after_set\":false}"), *SequenceAssetPath, *BindingGuid, *IntegerPropertyName, *IntegerPropertyPath));
		FHttpSmokeResult AddIntegerTrackResult;
		TestTrue(TEXT("Sequence property add_integer_property_track request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddIntegerTrackBody, AddIntegerTrackResult));
		TestEqual(TEXT("Sequence property add_integer_property_track status code"), AddIntegerTrackResult.StatusCode, 200);

		const FString AddIntegerKeyBody = MakeExecRequestBody(
			TEXT("smoke-seqprop-add-int-key-001"),
			TEXT("sequence_add_integer_property_key"),
			FString::Printf(TEXT("{\"asset_path\":\"%s\",\"binding_guid\":\"%s\",\"property_name\":\"%s\",\"property_path\":\"%s\",\"time_seconds\":0.75,\"value\":7,\"value_before_key\":0,\"save_after_set\":false}"), *SequenceAssetPath, *BindingGuid, *IntegerPropertyName, *IntegerPropertyPath));
		FHttpSmokeResult AddIntegerKeyResult;
		TestTrue(TEXT("Sequence property add_integer_property_key request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddIntegerKeyBody, AddIntegerKeyResult));
		TestEqual(TEXT("Sequence property add_integer_property_key status code"), AddIntegerKeyResult.StatusCode, 200);

		const FString GetInfoAfterIntegerBody = MakeExecRequestBody(
			TEXT("smoke-seqprop-info-002"),
			TEXT("sequence_get_level_sequence_info"),
			FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *SequenceAssetPath));
		FHttpSmokeResult GetInfoAfterIntegerResult;
		TestTrue(TEXT("Sequence property get_level_sequence_info after integer request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, GetInfoAfterIntegerBody, GetInfoAfterIntegerResult));
		TestEqual(TEXT("Sequence property get_level_sequence_info after integer status code"), GetInfoAfterIntegerResult.StatusCode, 200);

		TSharedPtr<FJsonObject> GetInfoAfterIntegerJson;
		TestTrue(TEXT("Sequence property get_level_sequence_info after integer parses JSON"), ParseJson(GetInfoAfterIntegerResult.Body, GetInfoAfterIntegerJson));
		const TSharedPtr<FJsonObject>* GetInfoAfterIntegerData = nullptr;
		TestTrue(TEXT("Sequence property get_level_sequence_info after integer contains data"), GetInfoAfterIntegerJson.IsValid() && GetInfoAfterIntegerJson->TryGetObjectField(TEXT("data"), GetInfoAfterIntegerData) && GetInfoAfterIntegerData && GetInfoAfterIntegerData->IsValid());
		if (GetInfoAfterIntegerData && GetInfoAfterIntegerData->IsValid())
		{
			TSharedPtr<FJsonObject> IntegerTrackObject;
			TestTrue(TEXT("Sequence integer property track summary exists"), FindTrackByClassNeedle(GetInfoAfterIntegerData, BindingGuid, TEXT("MovieSceneIntegerTrack"), IntegerTrackObject));
			if (IntegerTrackObject.IsValid())
			{
				TestEqual(TEXT("Sequence integer property_name matches"), IntegerTrackObject->GetStringField(TEXT("property_name")), IntegerPropertyName);
				TestEqual(TEXT("Sequence integer property_path matches"), IntegerTrackObject->GetStringField(TEXT("property_path")), IntegerPropertyPath);

				const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
				TestTrue(TEXT("Sequence integer track contains sections"), IntegerTrackObject->TryGetArrayField(TEXT("sections"), Sections) && Sections && Sections->Num() > 0);
				if (Sections && Sections->Num() > 0)
				{
					const TSharedPtr<FJsonObject>* SectionObject = nullptr;
					TestTrue(TEXT("Sequence integer section parses"), (*Sections)[0].IsValid() && (*Sections)[0]->TryGetObject(SectionObject) && SectionObject && SectionObject->IsValid());
					if (SectionObject && SectionObject->IsValid())
					{
						TestEqual(TEXT("Sequence integer section type"), (*SectionObject)->GetStringField(TEXT("section_type")), FString(TEXT("integer")));
						TestTrue(TEXT("Sequence integer section has keys"), static_cast<int32>((*SectionObject)->GetIntegerField(TEXT("key_count"))) >= 1);
					}
				}
			}
		}
	}

	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentInterfaceSequenceGenericPropertyTrackSmokeTest,
	"GptProjectTest.UeAgentInterface.Smoke.SequenceGenericPropertyTrackWorkflow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUeAgentInterfaceSequenceGenericPropertyTrackSmokeTest::RunTest(const FString& Parameters)
{
	FScopedUeAgentHttpServer ServerScope;
	FString InitError;
	if (!ServerScope.Initialize(InitError))
	{
		AddError(FString::Printf(TEXT("Initialize failed: %s"), *InitError));
		return false;
	}

	const FString LightActorLabel = FString::Printf(TEXT("SmokeSeqGenLight_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8));
	const FString MeshActorLabel = FString::Printf(TEXT("SmokeSeqGenMesh_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8));
	const FString StateActorLabel = FString::Printf(TEXT("SmokeSeqGenState_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8));
	const FString TerminalActorLabel = FString::Printf(TEXT("SmokeSeqGenTerminal_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8));
	const FString DocActorLabel = FString::Printf(TEXT("SmokeSeqGenDoc_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8));
	const FString CharacterActorLabel = FString::Printf(TEXT("SmokeSeqGenChar_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8));
	const FString BonfireActorLabel = FString::Printf(TEXT("SmokeSeqGenBonfire_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8));
	const FString DoorActorLabel = FString::Printf(TEXT("SmokeSeqGenDoor_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8));
	const FString SequenceAssetPath = MakeAutomationAssetPath(TEXT("SeqPropGen"));
	const FString FolderRoot = FPaths::ConvertRelativePathToFull(
		FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UeAssetFolders"), TEXT("LevelSequence")));
	const FString FolderPath = FPaths::Combine(FolderRoot, SequenceAssetPath.RightChop(1));

	auto FindTrackByClassNeedle = [](const TSharedPtr<FJsonObject>* InfoData, const FString& BindingGuid, const FString& ClassNeedle, TSharedPtr<FJsonObject>& OutTrackObject) -> bool
	{
		if (!InfoData || !InfoData->IsValid())
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* Bindings = nullptr;
		if (!(*InfoData)->TryGetArrayField(TEXT("bindings"), Bindings) || !Bindings)
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& BindingValue : *Bindings)
		{
			const TSharedPtr<FJsonObject>* BindingObject = nullptr;
			if (!BindingValue.IsValid() || !BindingValue->TryGetObject(BindingObject) || !BindingObject || !BindingObject->IsValid())
			{
				continue;
			}

			if ((*BindingObject)->GetStringField(TEXT("guid")) != BindingGuid)
			{
				continue;
			}

			const TArray<TSharedPtr<FJsonValue>>* Tracks = nullptr;
			if (!(*BindingObject)->TryGetArrayField(TEXT("tracks"), Tracks) || !Tracks)
			{
				return false;
			}

			for (const TSharedPtr<FJsonValue>& TrackValue : *Tracks)
			{
				const TSharedPtr<FJsonObject>* TrackObject = nullptr;
				if (!TrackValue.IsValid() || !TrackValue->TryGetObject(TrackObject) || !TrackObject || !TrackObject->IsValid())
				{
					continue;
				}

				if ((*TrackObject)->GetStringField(TEXT("class")).Contains(ClassNeedle))
				{
					OutTrackObject = *TrackObject;
					return true;
				}
			}

			return false;
		}

		return false;
	};

	auto LoadJsonFile = [this](const FString& FilePath, TSharedPtr<FJsonObject>& OutObj) -> bool
	{
		FString JsonText;
		if (!FFileHelper::LoadFileToString(JsonText, *FilePath))
		{
			AddError(FString::Printf(TEXT("Load file failed: %s"), *FilePath));
			return false;
		}
		const TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(JsonText);
		if (!FJsonSerializer::Deserialize(Reader, OutObj) || !OutObj.IsValid())
		{
			AddError(FString::Printf(TEXT("Parse json failed: %s"), *FilePath));
			return false;
		}
		return true;
	};

	auto SaveJsonFile = [this](const FString& FilePath, const TSharedPtr<FJsonObject>& Obj) -> bool
	{
		FString JsonText;
		const TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&JsonText);
		if (!FJsonSerializer::Serialize(Obj.ToSharedRef(), Writer))
		{
			AddError(FString::Printf(TEXT("Serialize json failed: %s"), *FilePath));
			return false;
		}
		if (!FFileHelper::SaveStringToFile(JsonText, *FilePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			AddError(FString::Printf(TEXT("Save file failed: %s"), *FilePath));
			return false;
		}
		return true;
	};

	auto FindBindingFolderName = [this](const TSharedPtr<FJsonObject>& BindingsIndexJson, const FString& TargetBindingGuid, FString& OutFolderName) -> bool
	{
		OutFolderName.Reset();
		if (!BindingsIndexJson.IsValid())
		{
			return false;
		}
		const TArray<TSharedPtr<FJsonValue>>* Bindings = nullptr;
		if (!BindingsIndexJson->TryGetArrayField(TEXT("bindings"), Bindings) || !Bindings)
		{
			return false;
		}
		for (const TSharedPtr<FJsonValue>& BindingValue : *Bindings)
		{
			const TSharedPtr<FJsonObject>* BindingObj = nullptr;
			if (!BindingValue.IsValid() || !BindingValue->TryGetObject(BindingObj) || !BindingObj || !(*BindingObj).IsValid())
			{
				continue;
			}
			if ((*BindingObj)->GetStringField(TEXT("binding_guid")) == TargetBindingGuid)
			{
				OutFolderName = (*BindingObj)->GetStringField(TEXT("folder_name"));
				return !OutFolderName.IsEmpty();
			}
		}
		return false;
	};

	auto FindTrackFileInDir = [this, &LoadJsonFile](const FString& TracksDir, auto Predicate, FString& OutTrackFileName, TSharedPtr<FJsonObject>& OutTrackJson) -> bool
	{
		OutTrackFileName.Reset();
		OutTrackJson.Reset();
		TArray<FString> TrackFiles;
		IFileManager::Get().FindFiles(TrackFiles, *(FPaths::Combine(TracksDir, TEXT("*.json"))), true, false);
		TrackFiles.Sort();
		for (const FString& TrackFile : TrackFiles)
		{
			TSharedPtr<FJsonObject> TrackJson;
			if (!LoadJsonFile(FPaths::Combine(TracksDir, TrackFile), TrackJson) || !TrackJson.IsValid())
			{
				continue;
			}
			if (Predicate(TrackJson))
			{
				OutTrackFileName = TrackFile;
				OutTrackJson = TrackJson;
				return true;
			}
		}
		return false;
	};

	const FString SpawnLightBody = MakeExecRequestBody(
		TEXT("smoke-seqgen-spawn-light-001"),
		TEXT("spawn_actor"),
		FString::Printf(TEXT("{\"class_path\":\"/Script/Engine.PointLight\",\"label\":\"%s\",\"location\":{\"x\":980,\"y\":96,\"z\":160}}"), *LightActorLabel));
	FHttpSmokeResult SpawnLightResult;
	TestTrue(TEXT("Sequence generic spawn light request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SpawnLightBody, SpawnLightResult));
	TestEqual(TEXT("Sequence generic spawn light status code"), SpawnLightResult.StatusCode, 200);

	const FString SpawnMeshActorBody = MakeExecRequestBody(
		TEXT("smoke-seqgen-spawn-mesh-001"),
		TEXT("spawn_actor"),
		FString::Printf(TEXT("{\"class_path\":\"/Script/Engine.StaticMeshActor\",\"label\":\"%s\",\"static_mesh\":\"/Engine/BasicShapes/Cube.Cube\",\"location\":{\"x\":1040,\"y\":96,\"z\":96}}"), *MeshActorLabel));
	FHttpSmokeResult SpawnMeshActorResult;
	TestTrue(TEXT("Sequence generic spawn mesh actor request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SpawnMeshActorBody, SpawnMeshActorResult));
	TestEqual(TEXT("Sequence generic spawn mesh actor status code"), SpawnMeshActorResult.StatusCode, 200);

	const FString SpawnStateActorBody = MakeExecRequestBody(
		TEXT("smoke-seqgen-spawn-state-001"),
		TEXT("spawn_actor"),
		FString::Printf(TEXT("{\"class_path\":\"/Script/Engine.GameStateBase\",\"label\":\"%s\"}"), *StateActorLabel));
	FHttpSmokeResult SpawnStateActorResult;
	TestTrue(TEXT("Sequence generic spawn state actor request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SpawnStateActorBody, SpawnStateActorResult));
	TestEqual(TEXT("Sequence generic spawn state actor status code"), SpawnStateActorResult.StatusCode, 200);

	const FString SpawnTerminalActorBody = MakeExecRequestBody(
		TEXT("smoke-seqgen-spawn-terminal-001"),
		TEXT("spawn_actor"),
		FString::Printf(TEXT("{\"class_path\":\"/Script/GptProjectTest.FacilityControlTerminalActor\",\"label\":\"%s\",\"location\":{\"x\":1180,\"y\":96,\"z\":64}}"), *TerminalActorLabel));
	FHttpSmokeResult SpawnTerminalActorResult;
	TestTrue(TEXT("Sequence generic spawn terminal actor request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SpawnTerminalActorBody, SpawnTerminalActorResult));
	TestEqual(TEXT("Sequence generic spawn terminal actor status code"), SpawnTerminalActorResult.StatusCode, 200);

	const FString SpawnDocActorBody = MakeExecRequestBody(
		TEXT("smoke-seqgen-spawn-doc-001"),
		TEXT("spawn_actor"),
		FString::Printf(TEXT("{\"class_path\":\"/Script/Engine.DocumentationActor\",\"label\":\"%s\",\"location\":{\"x\":1300,\"y\":96,\"z\":64}}"), *DocActorLabel));
	FHttpSmokeResult SpawnDocActorResult;
	TestTrue(TEXT("Sequence generic spawn documentation actor request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SpawnDocActorBody, SpawnDocActorResult));
	TestEqual(TEXT("Sequence generic spawn documentation actor status code"), SpawnDocActorResult.StatusCode, 200);

	const FString SpawnCharacterActorBody = MakeExecRequestBody(
		TEXT("smoke-seqgen-spawn-char-001"),
		TEXT("spawn_actor"),
		FString::Printf(TEXT("{\"class_path\":\"/Script/GptProjectTest.GptProjectTestCharacter\",\"label\":\"%s\",\"location\":{\"x\":1400,\"y\":96,\"z\":120}}"), *CharacterActorLabel));
	FHttpSmokeResult SpawnCharacterActorResult;
	TestTrue(TEXT("Sequence generic spawn character actor request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SpawnCharacterActorBody, SpawnCharacterActorResult));
	TestEqual(TEXT("Sequence generic spawn character actor status code"), SpawnCharacterActorResult.StatusCode, 200);

	const FString SpawnBonfireActorBody = MakeExecRequestBody(
		TEXT("smoke-seqgen-spawn-bonfire-001"),
		TEXT("spawn_actor"),
		FString::Printf(TEXT("{\"class_path\":\"/Script/GptProjectTest.FacilityBonfireActor\",\"label\":\"%s\",\"location\":{\"x\":1500,\"y\":96,\"z\":64}}"), *BonfireActorLabel));
	FHttpSmokeResult SpawnBonfireActorResult;
	TestTrue(TEXT("Sequence generic spawn bonfire actor request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SpawnBonfireActorBody, SpawnBonfireActorResult));
	TestEqual(TEXT("Sequence generic spawn bonfire actor status code"), SpawnBonfireActorResult.StatusCode, 200);

	const FString SpawnDoorActorBody = MakeExecRequestBody(
		TEXT("smoke-seqgen-spawn-door-001"),
		TEXT("spawn_actor"),
		FString::Printf(TEXT("{\"class_path\":\"/Script/GptProjectTest.FacilityDoorActor\",\"label\":\"%s\",\"location\":{\"x\":1600,\"y\":96,\"z\":64}}"), *DoorActorLabel));
	FHttpSmokeResult SpawnDoorActorResult;
	TestTrue(TEXT("Sequence generic spawn door actor request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SpawnDoorActorBody, SpawnDoorActorResult));
	TestEqual(TEXT("Sequence generic spawn door actor status code"), SpawnDoorActorResult.StatusCode, 200);

	const FString CreateSequenceBody = MakeExecRequestBody(
		TEXT("smoke-seqgen-create-seq-001"),
		TEXT("sequence_create_level_sequence"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"start_seconds\":0,\"duration_seconds\":2.0,\"display_rate_num\":30,\"display_rate_den\":1,\"open_editor\":false,\"save_after_create\":false}"), *SequenceAssetPath));
	FHttpSmokeResult CreateSequenceResult;
	TestTrue(TEXT("Sequence generic create_level_sequence request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, CreateSequenceBody, CreateSequenceResult));
	TestEqual(TEXT("Sequence generic create_level_sequence status code"), CreateSequenceResult.StatusCode, 200);

	const FString AddBindingBody = MakeExecRequestBody(
		TEXT("smoke-seqgen-add-binding-001"),
		TEXT("sequence_add_actor_binding"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"actor_id\":\"%s\",\"save_after_set\":false}"), *SequenceAssetPath, *LightActorLabel));
	FHttpSmokeResult AddBindingResult;
	TestTrue(TEXT("Sequence generic add_actor_binding request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddBindingBody, AddBindingResult));
	TestEqual(TEXT("Sequence generic add_actor_binding status code"), AddBindingResult.StatusCode, 200);

	TSharedPtr<FJsonObject> AddBindingJson;
	TestTrue(TEXT("Sequence generic add_actor_binding parses JSON"), ParseJson(AddBindingResult.Body, AddBindingJson));
	FString BindingGuid;
	const TSharedPtr<FJsonObject>* AddBindingData = nullptr;
	TestTrue(TEXT("Sequence generic add_actor_binding contains data"), AddBindingJson.IsValid() && AddBindingJson->TryGetObjectField(TEXT("data"), AddBindingData) && AddBindingData && AddBindingData->IsValid());
	if (AddBindingData && AddBindingData->IsValid())
	{
		BindingGuid = (*AddBindingData)->GetStringField(TEXT("binding_guid"));
	}
	TestTrue(TEXT("Sequence generic binding guid non-empty"), !BindingGuid.IsEmpty());

	const FString AddMeshBindingBody = MakeExecRequestBody(
		TEXT("smoke-seqgen-add-mesh-binding-001"),
		TEXT("sequence_add_actor_binding"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"actor_id\":\"%s\",\"save_after_set\":false}"), *SequenceAssetPath, *MeshActorLabel));
	FHttpSmokeResult AddMeshBindingResult;
	TestTrue(TEXT("Sequence generic add mesh actor binding request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddMeshBindingBody, AddMeshBindingResult));
	TestEqual(TEXT("Sequence generic add mesh actor binding status code"), AddMeshBindingResult.StatusCode, 200);

	TSharedPtr<FJsonObject> AddMeshBindingJson;
	TestTrue(TEXT("Sequence generic add mesh actor binding parses JSON"), ParseJson(AddMeshBindingResult.Body, AddMeshBindingJson));
	FString MeshBindingGuid;
	const TSharedPtr<FJsonObject>* AddMeshBindingData = nullptr;
	TestTrue(TEXT("Sequence generic add mesh actor binding contains data"), AddMeshBindingJson.IsValid() && AddMeshBindingJson->TryGetObjectField(TEXT("data"), AddMeshBindingData) && AddMeshBindingData && AddMeshBindingData->IsValid());
	if (AddMeshBindingData && AddMeshBindingData->IsValid())
	{
		MeshBindingGuid = (*AddMeshBindingData)->GetStringField(TEXT("binding_guid"));
	}
	TestTrue(TEXT("Sequence generic mesh binding guid non-empty"), !MeshBindingGuid.IsEmpty());

	const FString AddStateBindingBody = MakeExecRequestBody(
		TEXT("smoke-seqgen-add-state-binding-001"),
		TEXT("sequence_add_actor_binding"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"actor_id\":\"%s\",\"save_after_set\":false}"), *SequenceAssetPath, *StateActorLabel));
	FHttpSmokeResult AddStateBindingResult;
	TestTrue(TEXT("Sequence generic add state actor binding request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddStateBindingBody, AddStateBindingResult));
	TestEqual(TEXT("Sequence generic add state actor binding status code"), AddStateBindingResult.StatusCode, 200);

	TSharedPtr<FJsonObject> AddStateBindingJson;
	TestTrue(TEXT("Sequence generic add state actor binding parses JSON"), ParseJson(AddStateBindingResult.Body, AddStateBindingJson));
	FString StateBindingGuid;
	const TSharedPtr<FJsonObject>* AddStateBindingData = nullptr;
	TestTrue(TEXT("Sequence generic add state actor binding contains data"), AddStateBindingJson.IsValid() && AddStateBindingJson->TryGetObjectField(TEXT("data"), AddStateBindingData) && AddStateBindingData && AddStateBindingData->IsValid());
	if (AddStateBindingData && AddStateBindingData->IsValid())
	{
		StateBindingGuid = (*AddStateBindingData)->GetStringField(TEXT("binding_guid"));
	}
	TestTrue(TEXT("Sequence generic state binding guid non-empty"), !StateBindingGuid.IsEmpty());

	const FString AddTerminalBindingBody = MakeExecRequestBody(
		TEXT("smoke-seqgen-add-terminal-binding-001"),
		TEXT("sequence_add_actor_binding"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"actor_id\":\"%s\",\"save_after_set\":false}"), *SequenceAssetPath, *TerminalActorLabel));
	FHttpSmokeResult AddTerminalBindingResult;
	TestTrue(TEXT("Sequence generic add terminal actor binding request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddTerminalBindingBody, AddTerminalBindingResult));
	TestEqual(TEXT("Sequence generic add terminal actor binding status code"), AddTerminalBindingResult.StatusCode, 200);

	TSharedPtr<FJsonObject> AddTerminalBindingJson;
	TestTrue(TEXT("Sequence generic add terminal actor binding parses JSON"), ParseJson(AddTerminalBindingResult.Body, AddTerminalBindingJson));
	FString TerminalBindingGuid;
	const TSharedPtr<FJsonObject>* AddTerminalBindingData = nullptr;
	TestTrue(TEXT("Sequence generic add terminal actor binding contains data"), AddTerminalBindingJson.IsValid() && AddTerminalBindingJson->TryGetObjectField(TEXT("data"), AddTerminalBindingData) && AddTerminalBindingData && AddTerminalBindingData->IsValid());
	if (AddTerminalBindingData && AddTerminalBindingData->IsValid())
	{
		TerminalBindingGuid = (*AddTerminalBindingData)->GetStringField(TEXT("binding_guid"));
	}
	TestTrue(TEXT("Sequence generic terminal binding guid non-empty"), !TerminalBindingGuid.IsEmpty());

	const FString AddDocBindingBody = MakeExecRequestBody(
		TEXT("smoke-seqgen-add-doc-binding-001"),
		TEXT("sequence_add_actor_binding"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"actor_id\":\"%s\",\"save_after_set\":false}"), *SequenceAssetPath, *DocActorLabel));
	FHttpSmokeResult AddDocBindingResult;
	TestTrue(TEXT("Sequence generic add documentation actor binding request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddDocBindingBody, AddDocBindingResult));
	TestEqual(TEXT("Sequence generic add documentation actor binding status code"), AddDocBindingResult.StatusCode, 200);

	TSharedPtr<FJsonObject> AddDocBindingJson;
	TestTrue(TEXT("Sequence generic add documentation actor binding parses JSON"), ParseJson(AddDocBindingResult.Body, AddDocBindingJson));
	FString DocBindingGuid;
	const TSharedPtr<FJsonObject>* AddDocBindingData = nullptr;
	TestTrue(TEXT("Sequence generic add documentation actor binding contains data"), AddDocBindingJson.IsValid() && AddDocBindingJson->TryGetObjectField(TEXT("data"), AddDocBindingData) && AddDocBindingData && AddDocBindingData->IsValid());
	if (AddDocBindingData && AddDocBindingData->IsValid())
	{
		DocBindingGuid = (*AddDocBindingData)->GetStringField(TEXT("binding_guid"));
	}
	TestTrue(TEXT("Sequence generic documentation binding guid non-empty"), !DocBindingGuid.IsEmpty());

	const FString AddCharacterBindingBody = MakeExecRequestBody(
		TEXT("smoke-seqgen-add-char-binding-001"),
		TEXT("sequence_add_actor_binding"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"actor_id\":\"%s\",\"save_after_set\":false}"), *SequenceAssetPath, *CharacterActorLabel));
	FHttpSmokeResult AddCharacterBindingResult;
	TestTrue(TEXT("Sequence generic add character actor binding request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddCharacterBindingBody, AddCharacterBindingResult));
	TestEqual(TEXT("Sequence generic add character actor binding status code"), AddCharacterBindingResult.StatusCode, 200);

	TSharedPtr<FJsonObject> AddCharacterBindingJson;
	TestTrue(TEXT("Sequence generic add character actor binding parses JSON"), ParseJson(AddCharacterBindingResult.Body, AddCharacterBindingJson));
	FString CharacterBindingGuid;
	const TSharedPtr<FJsonObject>* AddCharacterBindingData = nullptr;
	TestTrue(TEXT("Sequence generic add character actor binding contains data"), AddCharacterBindingJson.IsValid() && AddCharacterBindingJson->TryGetObjectField(TEXT("data"), AddCharacterBindingData) && AddCharacterBindingData && AddCharacterBindingData->IsValid());
	if (AddCharacterBindingData && AddCharacterBindingData->IsValid())
	{
		CharacterBindingGuid = (*AddCharacterBindingData)->GetStringField(TEXT("binding_guid"));
	}
	TestTrue(TEXT("Sequence generic character binding guid non-empty"), !CharacterBindingGuid.IsEmpty());

	const FString AddBonfireBindingBody = MakeExecRequestBody(
		TEXT("smoke-seqgen-add-bonfire-binding-001"),
		TEXT("sequence_add_actor_binding"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"actor_id\":\"%s\",\"save_after_set\":false}"), *SequenceAssetPath, *BonfireActorLabel));
	FHttpSmokeResult AddBonfireBindingResult;
	TestTrue(TEXT("Sequence generic add bonfire actor binding request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddBonfireBindingBody, AddBonfireBindingResult));
	TestEqual(TEXT("Sequence generic add bonfire actor binding status code"), AddBonfireBindingResult.StatusCode, 200);
	TSharedPtr<FJsonObject> AddBonfireBindingJson;
	TestTrue(TEXT("Sequence generic add bonfire actor binding parses JSON"), ParseJson(AddBonfireBindingResult.Body, AddBonfireBindingJson));
	FString BonfireBindingGuid;
	const TSharedPtr<FJsonObject>* AddBonfireBindingData = nullptr;
	TestTrue(TEXT("Sequence generic add bonfire actor binding contains data"), AddBonfireBindingJson.IsValid() && AddBonfireBindingJson->TryGetObjectField(TEXT("data"), AddBonfireBindingData) && AddBonfireBindingData && AddBonfireBindingData->IsValid());
	if (AddBonfireBindingData && AddBonfireBindingData->IsValid())
	{
		BonfireBindingGuid = (*AddBonfireBindingData)->GetStringField(TEXT("binding_guid"));
	}
	TestTrue(TEXT("Sequence generic bonfire binding guid non-empty"), !BonfireBindingGuid.IsEmpty());

	const FString AddDoorBindingBody = MakeExecRequestBody(
		TEXT("smoke-seqgen-add-door-binding-001"),
		TEXT("sequence_add_actor_binding"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"actor_id\":\"%s\",\"save_after_set\":false}"), *SequenceAssetPath, *DoorActorLabel));
	FHttpSmokeResult AddDoorBindingResult;
	TestTrue(TEXT("Sequence generic add door actor binding request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddDoorBindingBody, AddDoorBindingResult));
	TestEqual(TEXT("Sequence generic add door actor binding status code"), AddDoorBindingResult.StatusCode, 200);
	TSharedPtr<FJsonObject> AddDoorBindingJson;
	TestTrue(TEXT("Sequence generic add door actor binding parses JSON"), ParseJson(AddDoorBindingResult.Body, AddDoorBindingJson));
	FString DoorBindingGuid;
	const TSharedPtr<FJsonObject>* AddDoorBindingData = nullptr;
	TestTrue(TEXT("Sequence generic add door actor binding contains data"), AddDoorBindingJson.IsValid() && AddDoorBindingJson->TryGetObjectField(TEXT("data"), AddDoorBindingData) && AddDoorBindingData && AddDoorBindingData->IsValid());
	if (AddDoorBindingData && AddDoorBindingData->IsValid())
	{
		DoorBindingGuid = (*AddDoorBindingData)->GetStringField(TEXT("binding_guid"));
	}
	TestTrue(TEXT("Sequence generic door binding guid non-empty"), !DoorBindingGuid.IsEmpty());

	const FString AddBoolTrackBody = MakeExecRequestBody(
		TEXT("smoke-seqgen-add-bool-track-001"),
		TEXT("sequence_add_property_track"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"binding_guid\":\"%s\",\"property_type\":\"bool\",\"property_name\":\"bActorEnableCollision\",\"save_after_set\":false}"), *SequenceAssetPath, *BindingGuid));
	FHttpSmokeResult AddBoolTrackResult;
	TestTrue(TEXT("Sequence generic add_property_track bool request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddBoolTrackBody, AddBoolTrackResult));
	TestEqual(TEXT("Sequence generic add_property_track bool status code"), AddBoolTrackResult.StatusCode, 200);

	const FString AddBoolKeyBody = MakeExecRequestBody(
		TEXT("smoke-seqgen-add-bool-key-001"),
		TEXT("sequence_add_property_key"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"binding_guid\":\"%s\",\"property_type\":\"bool\",\"property_name\":\"bActorEnableCollision\",\"time_seconds\":0.25,\"value\":false,\"value_before_key\":true,\"save_after_set\":false}"), *SequenceAssetPath, *BindingGuid));
	FHttpSmokeResult AddBoolKeyResult;
	TestTrue(TEXT("Sequence generic add_property_key bool request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddBoolKeyBody, AddBoolKeyResult));
	TestEqual(TEXT("Sequence generic add_property_key bool status code"), AddBoolKeyResult.StatusCode, 200);

	const FString AddColorTrackBody = MakeExecRequestBody(
		TEXT("smoke-seqgen-add-color-track-001"),
		TEXT("sequence_add_property_track"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"binding_guid\":\"%s\",\"property_type\":\"color\",\"property_name\":\"LightColor\",\"property_path\":\"LightComponent.LightColor\",\"save_after_set\":false}"), *SequenceAssetPath, *BindingGuid));
	FHttpSmokeResult AddColorTrackResult;
	TestTrue(TEXT("Sequence generic add_property_track color request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddColorTrackBody, AddColorTrackResult));
	TestEqual(TEXT("Sequence generic add_property_track color status code"), AddColorTrackResult.StatusCode, 200);

	const FString AddColorKeyBody = MakeExecRequestBody(
		TEXT("smoke-seqgen-add-color-key-001"),
		TEXT("sequence_add_property_key"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"binding_guid\":\"%s\",\"property_type\":\"color\",\"property_name\":\"LightColor\",\"property_path\":\"LightComponent.LightColor\",\"time_seconds\":0.5,\"red\":0.2,\"green\":0.6,\"blue\":1.0,\"alpha\":1.0,\"save_after_set\":false}"), *SequenceAssetPath, *BindingGuid));
	FHttpSmokeResult AddColorKeyResult;
	TestTrue(TEXT("Sequence generic add_property_key color request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddColorKeyBody, AddColorKeyResult));
	TestEqual(TEXT("Sequence generic add_property_key color status code"), AddColorKeyResult.StatusCode, 200);

	const FString AddObjectTrackBody = MakeExecRequestBody(
		TEXT("smoke-seqgen-add-object-track-001"),
		TEXT("sequence_add_property_track"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"binding_guid\":\"%s\",\"property_type\":\"object\",\"property_name\":\"StaticMesh\",\"property_path\":\"StaticMeshComponent.StaticMesh\",\"property_class_path\":\"/Script/Engine.StaticMesh\",\"save_after_set\":false}"), *SequenceAssetPath, *MeshBindingGuid));
	FHttpSmokeResult AddObjectTrackResult;
	TestTrue(TEXT("Sequence generic add_property_track object request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddObjectTrackBody, AddObjectTrackResult));
	TestEqual(TEXT("Sequence generic add_property_track object status code"), AddObjectTrackResult.StatusCode, 200);

	const FString AddObjectKeyBody = MakeExecRequestBody(
		TEXT("smoke-seqgen-add-object-key-001"),
		TEXT("sequence_add_property_key"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"binding_guid\":\"%s\",\"property_type\":\"object\",\"property_name\":\"StaticMesh\",\"property_path\":\"StaticMeshComponent.StaticMesh\",\"property_class_path\":\"/Script/Engine.StaticMesh\",\"time_seconds\":0.75,\"value_path\":\"/Engine/BasicShapes/Sphere.Sphere\",\"value_before_key_path\":\"/Engine/BasicShapes/Cube.Cube\",\"save_after_set\":false}"), *SequenceAssetPath, *MeshBindingGuid));
	FHttpSmokeResult AddObjectKeyResult;
	TestTrue(TEXT("Sequence generic add_property_key object request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddObjectKeyBody, AddObjectKeyResult));
	TestEqual(TEXT("Sequence generic add_property_key object status code"), AddObjectKeyResult.StatusCode, 200);

	const FString AddVectorTrackBody = MakeExecRequestBody(
		TEXT("smoke-seqgen-add-vector-track-001"),
		TEXT("sequence_add_property_track"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"binding_guid\":\"%s\",\"property_type\":\"vector3d\",\"property_name\":\"RelativeScale3D\",\"property_path\":\"StaticMeshComponent.RelativeScale3D\",\"save_after_set\":false}"), *SequenceAssetPath, *MeshBindingGuid));
	FHttpSmokeResult AddVectorTrackResult;
	TestTrue(TEXT("Sequence generic add_property_track vector request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddVectorTrackBody, AddVectorTrackResult));
	TestEqual(TEXT("Sequence generic add_property_track vector status code"), AddVectorTrackResult.StatusCode, 200);

	const FString AddVectorKeyBody = MakeExecRequestBody(
		TEXT("smoke-seqgen-add-vector-key-001"),
		TEXT("sequence_add_property_key"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"binding_guid\":\"%s\",\"property_type\":\"vector3d\",\"property_name\":\"RelativeScale3D\",\"property_path\":\"StaticMeshComponent.RelativeScale3D\",\"time_seconds\":0.7,\"value\":{\"x\":1.5,\"y\":2.0,\"z\":0.75},\"value_before_key\":{\"x\":1.0,\"y\":1.0,\"z\":1.0},\"save_after_set\":false}"), *SequenceAssetPath, *MeshBindingGuid));
	FHttpSmokeResult AddVectorKeyResult;
	TestTrue(TEXT("Sequence generic add_property_key vector request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddVectorKeyBody, AddVectorKeyResult));
	TestEqual(TEXT("Sequence generic add_property_key vector status code"), AddVectorKeyResult.StatusCode, 200);

	const FString AddRotatorTrackBody = MakeExecRequestBody(
		TEXT("smoke-seqgen-add-rotator-track-001"),
		TEXT("sequence_add_property_track"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"binding_guid\":\"%s\",\"property_type\":\"rotator\",\"property_name\":\"RelativeRotation\",\"property_path\":\"StaticMeshComponent.RelativeRotation\",\"save_after_set\":false}"), *SequenceAssetPath, *MeshBindingGuid));
	FHttpSmokeResult AddRotatorTrackResult;
	TestTrue(TEXT("Sequence generic add_property_track rotator request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddRotatorTrackBody, AddRotatorTrackResult));
	TestEqual(TEXT("Sequence generic add_property_track rotator status code"), AddRotatorTrackResult.StatusCode, 200);

	const FString AddRotatorKeyBody = MakeExecRequestBody(
		TEXT("smoke-seqgen-add-rotator-key-001"),
		TEXT("sequence_add_property_key"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"binding_guid\":\"%s\",\"property_type\":\"rotator\",\"property_name\":\"RelativeRotation\",\"property_path\":\"StaticMeshComponent.RelativeRotation\",\"time_seconds\":0.72,\"value\":{\"pitch\":10.0,\"yaw\":45.0,\"roll\":5.0},\"value_before_key\":{\"pitch\":0.0,\"yaw\":0.0,\"roll\":0.0},\"save_after_set\":false}"), *SequenceAssetPath, *MeshBindingGuid));
	FHttpSmokeResult AddRotatorKeyResult;
	TestTrue(TEXT("Sequence generic add_property_key rotator request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddRotatorKeyBody, AddRotatorKeyResult));
	TestEqual(TEXT("Sequence generic add_property_key rotator status code"), AddRotatorKeyResult.StatusCode, 200);

	const FString AddDoubleTrackBody = MakeExecRequestBody(
		TEXT("smoke-seqgen-add-double-track-001"),
		TEXT("sequence_add_property_track"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"binding_guid\":\"%s\",\"property_type\":\"double\",\"property_name\":\"ReplicatedWorldTimeSecondsDouble\",\"save_after_set\":false}"), *SequenceAssetPath, *StateBindingGuid));
	FHttpSmokeResult AddDoubleTrackResult;
	TestTrue(TEXT("Sequence generic add_property_track double request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddDoubleTrackBody, AddDoubleTrackResult));
	TestEqual(TEXT("Sequence generic add_property_track double status code"), AddDoubleTrackResult.StatusCode, 200);

	const FString AddDoubleKeyBody = MakeExecRequestBody(
		TEXT("smoke-seqgen-add-double-key-001"),
		TEXT("sequence_add_property_key"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"binding_guid\":\"%s\",\"property_type\":\"double\",\"property_name\":\"ReplicatedWorldTimeSecondsDouble\",\"time_seconds\":0.6,\"value\":12.5,\"value_before_key\":0.0,\"save_after_set\":false}"), *SequenceAssetPath, *StateBindingGuid));
	FHttpSmokeResult AddDoubleKeyResult;
	TestTrue(TEXT("Sequence generic add_property_key double request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddDoubleKeyBody, AddDoubleKeyResult));
	TestEqual(TEXT("Sequence generic add_property_key double status code"), AddDoubleKeyResult.StatusCode, 200);

	const FString AddByteTrackBody = MakeExecRequestBody(
		TEXT("smoke-seqgen-add-byte-track-001"),
		TEXT("sequence_add_property_track"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"binding_guid\":\"%s\",\"property_type\":\"byte\",\"property_name\":\"TerminalAction\",\"enum_path\":\"/Script/GptProjectTest.EFacilityTerminalAction\",\"save_after_set\":false}"), *SequenceAssetPath, *TerminalBindingGuid));
	FHttpSmokeResult AddByteTrackResult;
	TestTrue(TEXT("Sequence generic add_property_track byte request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddByteTrackBody, AddByteTrackResult));
	TestEqual(TEXT("Sequence generic add_property_track byte status code"), AddByteTrackResult.StatusCode, 200);

	const FString AddByteKeyBody = MakeExecRequestBody(
		TEXT("smoke-seqgen-add-byte-key-001"),
		TEXT("sequence_add_property_key"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"binding_guid\":\"%s\",\"property_type\":\"byte\",\"property_name\":\"TerminalAction\",\"enum_path\":\"/Script/GptProjectTest.EFacilityTerminalAction\",\"time_seconds\":0.8,\"value_name\":\"MoveTargetToEnd\",\"value_before_key_name\":\"ActivateTarget\",\"save_after_set\":false}"), *SequenceAssetPath, *TerminalBindingGuid));
	FHttpSmokeResult AddByteKeyResult;
	TestTrue(TEXT("Sequence generic add_property_key byte request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddByteKeyBody, AddByteKeyResult));
	TestEqual(TEXT("Sequence generic add_property_key byte status code"), AddByteKeyResult.StatusCode, 200);

	const FString AddStringTrackBody = MakeExecRequestBody(
		TEXT("smoke-seqgen-add-string-track-001"),
		TEXT("sequence_add_property_track"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"binding_guid\":\"%s\",\"property_type\":\"string\",\"property_name\":\"DocumentLink\",\"save_after_set\":false}"), *SequenceAssetPath, *DocBindingGuid));
	FHttpSmokeResult AddStringTrackResult;
	TestTrue(TEXT("Sequence generic add_property_track string request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddStringTrackBody, AddStringTrackResult));
	TestEqual(TEXT("Sequence generic add_property_track string status code"), AddStringTrackResult.StatusCode, 200);

	const FString AddStringKeyBody = MakeExecRequestBody(
		TEXT("smoke-seqgen-add-string-key-001"),
		TEXT("sequence_add_property_key"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"binding_guid\":\"%s\",\"property_type\":\"string\",\"property_name\":\"DocumentLink\",\"time_seconds\":0.9,\"value\":\"https://dev.epicgames.com/updated\",\"value_before_key\":\"https://dev.epicgames.com/original\",\"save_after_set\":false}"), *SequenceAssetPath, *DocBindingGuid));
	FHttpSmokeResult AddStringKeyResult;
	TestTrue(TEXT("Sequence generic add_property_key string request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddStringKeyBody, AddStringKeyResult));
	TestEqual(TEXT("Sequence generic add_property_key string status code"), AddStringKeyResult.StatusCode, 200);

	const FString AddActorReferenceTrackBody = MakeExecRequestBody(
		TEXT("smoke-seqgen-add-actorref-track-001"),
		TEXT("sequence_add_property_track"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"binding_guid\":\"%s\",\"property_type\":\"actor_reference\",\"property_name\":\"CurrentInteractableFacility\",\"save_after_set\":false}"), *SequenceAssetPath, *CharacterBindingGuid));
	FHttpSmokeResult AddActorReferenceTrackResult;
	TestTrue(TEXT("Sequence generic add_property_track actor_reference request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddActorReferenceTrackBody, AddActorReferenceTrackResult));
	TestEqual(TEXT("Sequence generic add_property_track actor_reference status code"), AddActorReferenceTrackResult.StatusCode, 200);

	const FString AddActorReferenceKeyBody = MakeExecRequestBody(
		TEXT("smoke-seqgen-add-actorref-key-001"),
		TEXT("sequence_add_property_key"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"binding_guid\":\"%s\",\"property_type\":\"actor_reference\",\"property_name\":\"CurrentInteractableFacility\",\"time_seconds\":1.0,\"value_actor_id\":\"%s\",\"value_before_key_actor_id\":\"%s\",\"save_after_set\":false}"), *SequenceAssetPath, *CharacterBindingGuid, *BonfireActorLabel, *DoorActorLabel));
	FHttpSmokeResult AddActorReferenceKeyResult;
	TestTrue(TEXT("Sequence generic add_property_key actor_reference request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddActorReferenceKeyBody, AddActorReferenceKeyResult));
	TestEqual(TEXT("Sequence generic add_property_key actor_reference status code"), AddActorReferenceKeyResult.StatusCode, 200);

	const FString GetInfoBody = MakeExecRequestBody(
		TEXT("smoke-seqgen-info-001"),
		TEXT("sequence_get_level_sequence_info"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *SequenceAssetPath));
	FHttpSmokeResult GetInfoResult;
	TestTrue(TEXT("Sequence generic get_level_sequence_info request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, GetInfoBody, GetInfoResult));
	TestEqual(TEXT("Sequence generic get_level_sequence_info status code"), GetInfoResult.StatusCode, 200);

	TSharedPtr<FJsonObject> GetInfoJson;
	TestTrue(TEXT("Sequence generic get_level_sequence_info parses JSON"), ParseJson(GetInfoResult.Body, GetInfoJson));
	const TSharedPtr<FJsonObject>* GetInfoData = nullptr;
	TestTrue(TEXT("Sequence generic get_level_sequence_info contains data"), GetInfoJson.IsValid() && GetInfoJson->TryGetObjectField(TEXT("data"), GetInfoData) && GetInfoData && GetInfoData->IsValid());
	if (GetInfoData && GetInfoData->IsValid())
	{
		TSharedPtr<FJsonObject> BoolTrackObject;
		TestTrue(TEXT("Sequence generic bool track exists"), FindTrackByClassNeedle(GetInfoData, BindingGuid, TEXT("MovieSceneBoolTrack"), BoolTrackObject));
		if (BoolTrackObject.IsValid())
		{
			TestEqual(TEXT("Sequence generic bool property name matches"), BoolTrackObject->GetStringField(TEXT("property_name")), FString(TEXT("bActorEnableCollision")));
		}

		TSharedPtr<FJsonObject> ColorTrackObject;
		TestTrue(TEXT("Sequence generic color track exists"), FindTrackByClassNeedle(GetInfoData, BindingGuid, TEXT("MovieSceneColorTrack"), ColorTrackObject));
		if (ColorTrackObject.IsValid())
		{
			TestEqual(TEXT("Sequence generic color property name matches"), ColorTrackObject->GetStringField(TEXT("property_name")), FString(TEXT("LightColor")));
			TestEqual(TEXT("Sequence generic color property path matches"), ColorTrackObject->GetStringField(TEXT("property_path")), FString(TEXT("LightComponent.LightColor")));

			const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
			TestTrue(TEXT("Sequence generic color track contains sections"), ColorTrackObject->TryGetArrayField(TEXT("sections"), Sections) && Sections && Sections->Num() > 0);
			if (Sections && Sections->Num() > 0)
			{
				const TSharedPtr<FJsonObject>* SectionObject = nullptr;
				TestTrue(TEXT("Sequence generic color section parses"), (*Sections)[0].IsValid() && (*Sections)[0]->TryGetObject(SectionObject) && SectionObject && SectionObject->IsValid());
				if (SectionObject && SectionObject->IsValid())
				{
					TestEqual(TEXT("Sequence generic color section type"), (*SectionObject)->GetStringField(TEXT("section_type")), FString(TEXT("color")));
					TestEqual(TEXT("Sequence generic color red key count"), static_cast<int32>((*SectionObject)->GetIntegerField(TEXT("red_key_count"))), 1);
					TestEqual(TEXT("Sequence generic color alpha key count"), static_cast<int32>((*SectionObject)->GetIntegerField(TEXT("alpha_key_count"))), 1);
				}
			}
		}

		TSharedPtr<FJsonObject> ObjectTrackObject;
		TestTrue(TEXT("Sequence generic object track exists"), FindTrackByClassNeedle(GetInfoData, MeshBindingGuid, TEXT("MovieSceneObjectPropertyTrack"), ObjectTrackObject));
		if (ObjectTrackObject.IsValid())
		{
			TestEqual(TEXT("Sequence generic object property name matches"), ObjectTrackObject->GetStringField(TEXT("property_name")), FString(TEXT("StaticMesh")));
			TestEqual(TEXT("Sequence generic object property path matches"), ObjectTrackObject->GetStringField(TEXT("property_path")), FString(TEXT("StaticMeshComponent.StaticMesh")));
			TestEqual(TEXT("Sequence generic object property class path matches"), ObjectTrackObject->GetStringField(TEXT("property_class_path")), FString(TEXT("/Script/Engine.StaticMesh")));

			const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
			TestTrue(TEXT("Sequence generic object track contains sections"), ObjectTrackObject->TryGetArrayField(TEXT("sections"), Sections) && Sections && Sections->Num() > 0);
			if (Sections && Sections->Num() > 0)
			{
				const TSharedPtr<FJsonObject>* SectionObject = nullptr;
				TestTrue(TEXT("Sequence generic object section parses"), (*Sections)[0].IsValid() && (*Sections)[0]->TryGetObject(SectionObject) && SectionObject && SectionObject->IsValid());
				if (SectionObject && SectionObject->IsValid())
				{
					TestEqual(TEXT("Sequence generic object section type"), (*SectionObject)->GetStringField(TEXT("section_type")), FString(TEXT("object")));
					TestEqual(TEXT("Sequence generic object key count"), static_cast<int32>((*SectionObject)->GetIntegerField(TEXT("key_count"))), 2);
				}
			}
		}

		TSharedPtr<FJsonObject> VectorTrackObject;
		TestTrue(TEXT("Sequence generic vector track exists"), FindTrackByClassNeedle(GetInfoData, MeshBindingGuid, TEXT("MovieSceneDoubleVectorTrack"), VectorTrackObject));
		if (VectorTrackObject.IsValid())
		{
			TestEqual(TEXT("Sequence generic vector property name matches"), VectorTrackObject->GetStringField(TEXT("property_name")), FString(TEXT("RelativeScale3D")));
			TestEqual(TEXT("Sequence generic vector property path matches"), VectorTrackObject->GetStringField(TEXT("property_path")), FString(TEXT("StaticMeshComponent.RelativeScale3D")));
			TestEqual(TEXT("Sequence generic vector precision matches"), VectorTrackObject->GetStringField(TEXT("vector_precision")), FString(TEXT("double")));
			TestEqual(TEXT("Sequence generic vector channels used matches"), static_cast<int32>(VectorTrackObject->GetIntegerField(TEXT("channels_used"))), 3);

			const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
			TestTrue(TEXT("Sequence generic vector track contains sections"), VectorTrackObject->TryGetArrayField(TEXT("sections"), Sections) && Sections && Sections->Num() > 0);
			if (Sections && Sections->Num() > 0)
			{
				const TSharedPtr<FJsonObject>* SectionObject = nullptr;
				TestTrue(TEXT("Sequence generic vector section parses"), (*Sections)[0].IsValid() && (*Sections)[0]->TryGetObject(SectionObject) && SectionObject && SectionObject->IsValid());
				if (SectionObject && SectionObject->IsValid())
				{
					TestEqual(TEXT("Sequence generic vector section type"), (*SectionObject)->GetStringField(TEXT("section_type")), FString(TEXT("vector")));
					TestEqual(TEXT("Sequence generic vector section precision"), (*SectionObject)->GetStringField(TEXT("vector_precision")), FString(TEXT("double")));
					TestEqual(TEXT("Sequence generic vector channels used"), static_cast<int32>((*SectionObject)->GetIntegerField(TEXT("channels_used"))), 3);
					TestTrue(TEXT("Sequence generic vector x key count"), static_cast<int32>((*SectionObject)->GetIntegerField(TEXT("x_key_count"))) >= 1);
					TestTrue(TEXT("Sequence generic vector z key count"), static_cast<int32>((*SectionObject)->GetIntegerField(TEXT("z_key_count"))) >= 1);
				}
			}
		}

		TSharedPtr<FJsonObject> RotatorTrackObject;
		TestTrue(TEXT("Sequence generic rotator track exists"), FindTrackByClassNeedle(GetInfoData, MeshBindingGuid, TEXT("MovieSceneRotatorTrack"), RotatorTrackObject));
		if (RotatorTrackObject.IsValid())
		{
			TestEqual(TEXT("Sequence generic rotator property name matches"), RotatorTrackObject->GetStringField(TEXT("property_name")), FString(TEXT("RelativeRotation")));
			TestEqual(TEXT("Sequence generic rotator property path matches"), RotatorTrackObject->GetStringField(TEXT("property_path")), FString(TEXT("StaticMeshComponent.RelativeRotation")));

			const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
			TestTrue(TEXT("Sequence generic rotator track contains sections"), RotatorTrackObject->TryGetArrayField(TEXT("sections"), Sections) && Sections && Sections->Num() > 0);
			if (Sections && Sections->Num() > 0)
			{
				const TSharedPtr<FJsonObject>* SectionObject = nullptr;
				TestTrue(TEXT("Sequence generic rotator section parses"), (*Sections)[0].IsValid() && (*Sections)[0]->TryGetObject(SectionObject) && SectionObject && SectionObject->IsValid());
				if (SectionObject && SectionObject->IsValid())
				{
					TestEqual(TEXT("Sequence generic rotator section type"), (*SectionObject)->GetStringField(TEXT("section_type")), FString(TEXT("rotator")));
					TestTrue(TEXT("Sequence generic rotator pitch key count"), static_cast<int32>((*SectionObject)->GetIntegerField(TEXT("pitch_key_count"))) >= 1);
					TestTrue(TEXT("Sequence generic rotator yaw key count"), static_cast<int32>((*SectionObject)->GetIntegerField(TEXT("yaw_key_count"))) >= 1);
					TestTrue(TEXT("Sequence generic rotator roll key count"), static_cast<int32>((*SectionObject)->GetIntegerField(TEXT("roll_key_count"))) >= 1);
				}
			}
		}

		TSharedPtr<FJsonObject> DoubleTrackObject;
		TestTrue(TEXT("Sequence generic double track exists"), FindTrackByClassNeedle(GetInfoData, StateBindingGuid, TEXT("MovieSceneDoubleTrack"), DoubleTrackObject));
		if (DoubleTrackObject.IsValid())
		{
			TestEqual(TEXT("Sequence generic double property name matches"), DoubleTrackObject->GetStringField(TEXT("property_name")), FString(TEXT("ReplicatedWorldTimeSecondsDouble")));

			const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
			TestTrue(TEXT("Sequence generic double track contains sections"), DoubleTrackObject->TryGetArrayField(TEXT("sections"), Sections) && Sections && Sections->Num() > 0);
			if (Sections && Sections->Num() > 0)
			{
				const TSharedPtr<FJsonObject>* SectionObject = nullptr;
				TestTrue(TEXT("Sequence generic double section parses"), (*Sections)[0].IsValid() && (*Sections)[0]->TryGetObject(SectionObject) && SectionObject && SectionObject->IsValid());
				if (SectionObject && SectionObject->IsValid())
				{
					TestEqual(TEXT("Sequence generic double section type"), (*SectionObject)->GetStringField(TEXT("section_type")), FString(TEXT("double")));
					TestTrue(TEXT("Sequence generic double key count"), static_cast<int32>((*SectionObject)->GetIntegerField(TEXT("key_count"))) >= 1);
				}
			}
		}

		TSharedPtr<FJsonObject> ByteTrackObject;
		TestTrue(TEXT("Sequence generic byte track exists"), FindTrackByClassNeedle(GetInfoData, TerminalBindingGuid, TEXT("MovieSceneByteTrack"), ByteTrackObject));
		if (ByteTrackObject.IsValid())
		{
			TestEqual(TEXT("Sequence generic byte property name matches"), ByteTrackObject->GetStringField(TEXT("property_name")), FString(TEXT("TerminalAction")));
			TestEqual(TEXT("Sequence generic byte enum path matches"), ByteTrackObject->GetStringField(TEXT("enum_path")), FString(TEXT("/Script/GptProjectTest.EFacilityTerminalAction")));

			const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
			TestTrue(TEXT("Sequence generic byte track contains sections"), ByteTrackObject->TryGetArrayField(TEXT("sections"), Sections) && Sections && Sections->Num() > 0);
			if (Sections && Sections->Num() > 0)
			{
				const TSharedPtr<FJsonObject>* SectionObject = nullptr;
				TestTrue(TEXT("Sequence generic byte section parses"), (*Sections)[0].IsValid() && (*Sections)[0]->TryGetObject(SectionObject) && SectionObject && SectionObject->IsValid());
				if (SectionObject && SectionObject->IsValid())
				{
					TestEqual(TEXT("Sequence generic byte section type"), (*SectionObject)->GetStringField(TEXT("section_type")), FString(TEXT("byte")));
					TestTrue(TEXT("Sequence generic byte key count"), static_cast<int32>((*SectionObject)->GetIntegerField(TEXT("key_count"))) >= 1);
				}
			}
		}

		TSharedPtr<FJsonObject> StringTrackObject;
		TestTrue(TEXT("Sequence generic string track exists"), FindTrackByClassNeedle(GetInfoData, DocBindingGuid, TEXT("MovieSceneStringTrack"), StringTrackObject));
		if (StringTrackObject.IsValid())
		{
			TestEqual(TEXT("Sequence generic string property name matches"), StringTrackObject->GetStringField(TEXT("property_name")), FString(TEXT("DocumentLink")));

			const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
			TestTrue(TEXT("Sequence generic string track contains sections"), StringTrackObject->TryGetArrayField(TEXT("sections"), Sections) && Sections && Sections->Num() > 0);
			if (Sections && Sections->Num() > 0)
			{
				const TSharedPtr<FJsonObject>* SectionObject = nullptr;
				TestTrue(TEXT("Sequence generic string section parses"), (*Sections)[0].IsValid() && (*Sections)[0]->TryGetObject(SectionObject) && SectionObject && SectionObject->IsValid());
				if (SectionObject && SectionObject->IsValid())
				{
					TestEqual(TEXT("Sequence generic string section type"), (*SectionObject)->GetStringField(TEXT("section_type")), FString(TEXT("string")));
					TestTrue(TEXT("Sequence generic string key count"), static_cast<int32>((*SectionObject)->GetIntegerField(TEXT("key_count"))) >= 1);
				}
			}
		}

		TSharedPtr<FJsonObject> ActorReferenceTrackObject;
		TestTrue(TEXT("Sequence generic actor reference track exists"), FindTrackByClassNeedle(GetInfoData, CharacterBindingGuid, TEXT("MovieSceneActorReferenceTrack"), ActorReferenceTrackObject));
		if (ActorReferenceTrackObject.IsValid())
		{
			TestEqual(TEXT("Sequence generic actor reference property name matches"), ActorReferenceTrackObject->GetStringField(TEXT("property_name")), FString(TEXT("CurrentInteractableFacility")));

			const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
			TestTrue(TEXT("Sequence generic actor reference track contains sections"), ActorReferenceTrackObject->TryGetArrayField(TEXT("sections"), Sections) && Sections && Sections->Num() > 0);
			if (Sections && Sections->Num() > 0)
			{
				const TSharedPtr<FJsonObject>* SectionObject = nullptr;
				TestTrue(TEXT("Sequence generic actor reference section parses"), (*Sections)[0].IsValid() && (*Sections)[0]->TryGetObject(SectionObject) && SectionObject && SectionObject->IsValid());
				if (SectionObject && SectionObject->IsValid())
				{
					TestEqual(TEXT("Sequence generic actor reference section type"), (*SectionObject)->GetStringField(TEXT("section_type")), FString(TEXT("actor_reference")));
					TestTrue(TEXT("Sequence generic actor reference key count"), static_cast<int32>((*SectionObject)->GetIntegerField(TEXT("key_count"))) >= 1);
				}
			}
		}
	}

	const FString ExportFolderBody = MakeExecRequestBody(
		TEXT("smoke-seqgen-folder-export-001"),
		TEXT("sequence_export_folder"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"clean_output_dir\":true,\"include_validation\":true}"), *SequenceAssetPath));
	FHttpSmokeResult ExportFolderResult;
	TestTrue(TEXT("Sequence generic folder export request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ExportFolderBody, ExportFolderResult));
	TestEqual(TEXT("Sequence generic folder export status code"), ExportFolderResult.StatusCode, 200);

	TSharedPtr<FJsonObject> BindingsIndexJson;
	TestTrue(TEXT("Sequence generic folder bindings index load"), LoadJsonFile(FPaths::Combine(FolderPath, TEXT("bindings"), TEXT("index.json")), BindingsIndexJson));
	FString MeshBindingFolderName;
	FString TerminalBindingFolderName;
	FString DocBindingFolderName;
	FString CharacterBindingFolderName;
	TestTrue(TEXT("Sequence generic folder mesh binding folder found"), FindBindingFolderName(BindingsIndexJson, MeshBindingGuid, MeshBindingFolderName));
	TestTrue(TEXT("Sequence generic folder terminal binding folder found"), FindBindingFolderName(BindingsIndexJson, TerminalBindingGuid, TerminalBindingFolderName));
	TestTrue(TEXT("Sequence generic folder doc binding folder found"), FindBindingFolderName(BindingsIndexJson, DocBindingGuid, DocBindingFolderName));
	TestTrue(TEXT("Sequence generic folder character binding folder found"), FindBindingFolderName(BindingsIndexJson, CharacterBindingGuid, CharacterBindingFolderName));

	const FString MeshTracksDir = FPaths::Combine(FolderPath, TEXT("bindings"), MeshBindingFolderName, TEXT("tracks"));
	const FString TerminalTracksDir = FPaths::Combine(FolderPath, TEXT("bindings"), TerminalBindingFolderName, TEXT("tracks"));
	const FString DocTracksDir = FPaths::Combine(FolderPath, TEXT("bindings"), DocBindingFolderName, TEXT("tracks"));
	const FString CharacterTracksDir = FPaths::Combine(FolderPath, TEXT("bindings"), CharacterBindingFolderName, TEXT("tracks"));

	FString ObjectTrackFile;
	FString VectorTrackFile;
	FString RotatorTrackFile;
	FString ByteTrackFile;
	FString StringTrackFile;
	FString ActorReferenceTrackFile;
	TSharedPtr<FJsonObject> ObjectTrackJson;
	TSharedPtr<FJsonObject> VectorTrackJson;
	TSharedPtr<FJsonObject> RotatorTrackJson;
	TSharedPtr<FJsonObject> ByteTrackJson;
	TSharedPtr<FJsonObject> StringTrackJson;
	TSharedPtr<FJsonObject> ActorReferenceTrackJson;

	TestTrue(TEXT("Sequence generic folder object track file found"), FindTrackFileInDir(MeshTracksDir, [](const TSharedPtr<FJsonObject>& TrackJson){ return TrackJson->GetStringField(TEXT("track_type")) == TEXT("property") && TrackJson->GetStringField(TEXT("property_type")) == TEXT("object"); }, ObjectTrackFile, ObjectTrackJson));
	TestTrue(TEXT("Sequence generic folder vector track file found"), FindTrackFileInDir(MeshTracksDir, [](const TSharedPtr<FJsonObject>& TrackJson){ return TrackJson->GetStringField(TEXT("track_type")) == TEXT("property") && TrackJson->GetStringField(TEXT("property_type")) == TEXT("vector"); }, VectorTrackFile, VectorTrackJson));
	TestTrue(TEXT("Sequence generic folder rotator track file found"), FindTrackFileInDir(MeshTracksDir, [](const TSharedPtr<FJsonObject>& TrackJson){ return TrackJson->GetStringField(TEXT("track_type")) == TEXT("property") && TrackJson->GetStringField(TEXT("property_type")) == TEXT("rotator"); }, RotatorTrackFile, RotatorTrackJson));
	TestTrue(TEXT("Sequence generic folder byte track file found"), FindTrackFileInDir(TerminalTracksDir, [](const TSharedPtr<FJsonObject>& TrackJson){ return TrackJson->GetStringField(TEXT("track_type")) == TEXT("property") && TrackJson->GetStringField(TEXT("property_type")) == TEXT("byte"); }, ByteTrackFile, ByteTrackJson));
	TestTrue(TEXT("Sequence generic folder string track file found"), FindTrackFileInDir(DocTracksDir, [](const TSharedPtr<FJsonObject>& TrackJson){ return TrackJson->GetStringField(TEXT("track_type")) == TEXT("property") && TrackJson->GetStringField(TEXT("property_type")) == TEXT("string"); }, StringTrackFile, StringTrackJson));
	TestTrue(TEXT("Sequence generic folder actor reference track file found"), FindTrackFileInDir(CharacterTracksDir, [](const TSharedPtr<FJsonObject>& TrackJson){ return TrackJson->GetStringField(TEXT("track_type")) == TEXT("property") && TrackJson->GetStringField(TEXT("property_type")) == TEXT("actor_reference"); }, ActorReferenceTrackFile, ActorReferenceTrackJson));

	if (ObjectTrackJson.IsValid())
	{
		TestEqual(TEXT("Sequence generic folder object class path"), ObjectTrackJson->GetStringField(TEXT("property_class_path")), FString(TEXT("/Script/Engine.StaticMesh")));
	}
	if (VectorTrackJson.IsValid())
	{
		TestEqual(TEXT("Sequence generic folder vector precision"), VectorTrackJson->GetStringField(TEXT("vector_precision")), FString(TEXT("double")));
		TestEqual(TEXT("Sequence generic folder vector channels"), static_cast<int32>(VectorTrackJson->GetIntegerField(TEXT("channels_used"))), 3);
	}
	if (ByteTrackJson.IsValid())
	{
		TestEqual(TEXT("Sequence generic folder byte enum path"), ByteTrackJson->GetStringField(TEXT("enum_path")), FString(TEXT("/Script/GptProjectTest.EFacilityTerminalAction")));
	}

	if (ObjectTrackJson.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
		if (ObjectTrackJson->TryGetArrayField(TEXT("sections"), Sections) && Sections && Sections->Num() == 1)
		{
			const TSharedPtr<FJsonObject>* SectionObj = nullptr;
			if ((*Sections)[0]->TryGetObject(SectionObj) && SectionObj && (*SectionObj).IsValid())
			{
				const TArray<TSharedPtr<FJsonValue>>* Keys = nullptr;
				if ((*SectionObj)->TryGetArrayField(TEXT("keys"), Keys) && Keys && Keys->Num() >= 2)
				{
					const TSharedPtr<FJsonObject>* KeyObj = nullptr;
					if ((*Keys)[1]->TryGetObject(KeyObj) && KeyObj && (*KeyObj).IsValid())
					{
						(*KeyObj)->SetStringField(TEXT("value_path"), TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
					}
				}
			}
		}
		TestTrue(TEXT("Sequence generic folder object track save"), SaveJsonFile(FPaths::Combine(MeshTracksDir, ObjectTrackFile), ObjectTrackJson));
	}
	if (VectorTrackJson.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
		if (VectorTrackJson->TryGetArrayField(TEXT("sections"), Sections) && Sections && Sections->Num() == 1)
		{
			const TSharedPtr<FJsonObject>* SectionObj = nullptr;
			if ((*Sections)[0]->TryGetObject(SectionObj) && SectionObj && (*SectionObj).IsValid())
			{
				const TArray<TSharedPtr<FJsonValue>>* Keys = nullptr;
				if ((*SectionObj)->TryGetArrayField(TEXT("keys"), Keys) && Keys && Keys->Num() >= 2)
				{
					const TSharedPtr<FJsonObject>* KeyObj = nullptr;
					if ((*Keys)[1]->TryGetObject(KeyObj) && KeyObj && (*KeyObj).IsValid())
					{
						const TSharedPtr<FJsonObject>* ValueObj = nullptr;
						if ((*KeyObj)->TryGetObjectField(TEXT("value"), ValueObj) && ValueObj && (*ValueObj).IsValid())
						{
							(*ValueObj)->SetNumberField(TEXT("x"), 2.25);
						}
					}
				}
			}
		}
		TestTrue(TEXT("Sequence generic folder vector track save"), SaveJsonFile(FPaths::Combine(MeshTracksDir, VectorTrackFile), VectorTrackJson));
	}
	if (RotatorTrackJson.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
		if (RotatorTrackJson->TryGetArrayField(TEXT("sections"), Sections) && Sections && Sections->Num() == 1)
		{
			const TSharedPtr<FJsonObject>* SectionObj = nullptr;
			if ((*Sections)[0]->TryGetObject(SectionObj) && SectionObj && (*SectionObj).IsValid())
			{
				const TArray<TSharedPtr<FJsonValue>>* Keys = nullptr;
				if ((*SectionObj)->TryGetArrayField(TEXT("keys"), Keys) && Keys && Keys->Num() >= 2)
				{
					const TSharedPtr<FJsonObject>* KeyObj = nullptr;
					if ((*Keys)[1]->TryGetObject(KeyObj) && KeyObj && (*KeyObj).IsValid())
					{
						const TSharedPtr<FJsonObject>* ValueObj = nullptr;
						if ((*KeyObj)->TryGetObjectField(TEXT("value"), ValueObj) && ValueObj && (*ValueObj).IsValid())
						{
							(*ValueObj)->SetNumberField(TEXT("yaw"), 90.0);
						}
					}
				}
			}
		}
		TestTrue(TEXT("Sequence generic folder rotator track save"), SaveJsonFile(FPaths::Combine(MeshTracksDir, RotatorTrackFile), RotatorTrackJson));
	}
	if (ByteTrackJson.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
		if (ByteTrackJson->TryGetArrayField(TEXT("sections"), Sections) && Sections && Sections->Num() == 1)
		{
			const TSharedPtr<FJsonObject>* SectionObj = nullptr;
			if ((*Sections)[0]->TryGetObject(SectionObj) && SectionObj && (*SectionObj).IsValid())
			{
				const TArray<TSharedPtr<FJsonValue>>* Keys = nullptr;
				if ((*SectionObj)->TryGetArrayField(TEXT("keys"), Keys) && Keys && Keys->Num() >= 2)
				{
					const TSharedPtr<FJsonObject>* KeyObj = nullptr;
					if ((*Keys)[1]->TryGetObject(KeyObj) && KeyObj && (*KeyObj).IsValid())
					{
						(*KeyObj)->SetStringField(TEXT("value_name"), TEXT("ActivateTarget"));
					}
				}
			}
		}
		TestTrue(TEXT("Sequence generic folder byte track save"), SaveJsonFile(FPaths::Combine(TerminalTracksDir, ByteTrackFile), ByteTrackJson));
	}
	if (StringTrackJson.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
		if (StringTrackJson->TryGetArrayField(TEXT("sections"), Sections) && Sections && Sections->Num() == 1)
		{
			const TSharedPtr<FJsonObject>* SectionObj = nullptr;
			if ((*Sections)[0]->TryGetObject(SectionObj) && SectionObj && (*SectionObj).IsValid())
			{
				const TArray<TSharedPtr<FJsonValue>>* Keys = nullptr;
				if ((*SectionObj)->TryGetArrayField(TEXT("keys"), Keys) && Keys && Keys->Num() >= 2)
				{
					const TSharedPtr<FJsonObject>* KeyObj = nullptr;
					if ((*Keys)[1]->TryGetObject(KeyObj) && KeyObj && (*KeyObj).IsValid())
					{
						(*KeyObj)->SetStringField(TEXT("value"), TEXT("https://dev.epicgames.com/folder-updated"));
					}
				}
			}
		}
		TestTrue(TEXT("Sequence generic folder string track save"), SaveJsonFile(FPaths::Combine(DocTracksDir, StringTrackFile), StringTrackJson));
	}
	if (ActorReferenceTrackJson.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
		if (ActorReferenceTrackJson->TryGetArrayField(TEXT("sections"), Sections) && Sections && Sections->Num() == 1)
		{
			const TSharedPtr<FJsonObject>* SectionObj = nullptr;
			if ((*Sections)[0]->TryGetObject(SectionObj) && SectionObj && (*SectionObj).IsValid())
			{
				const TArray<TSharedPtr<FJsonValue>>* Keys = nullptr;
				if ((*SectionObj)->TryGetArrayField(TEXT("keys"), Keys) && Keys && Keys->Num() >= 2)
				{
					const TSharedPtr<FJsonObject>* KeyObj = nullptr;
					if ((*Keys)[1]->TryGetObject(KeyObj) && KeyObj && (*KeyObj).IsValid())
					{
						(*KeyObj)->SetStringField(TEXT("value_binding_guid"), DoorBindingGuid);
						(*KeyObj)->SetStringField(TEXT("component_name"), TEXT(""));
						(*KeyObj)->SetStringField(TEXT("socket_name"), TEXT(""));
					}
				}
			}
		}
		TestTrue(TEXT("Sequence generic folder actor reference track save"), SaveJsonFile(FPaths::Combine(CharacterTracksDir, ActorReferenceTrackFile), ActorReferenceTrackJson));
	}

	const FString ApplyFolderBody = MakeExecRequestBody(
		TEXT("smoke-seqgen-folder-apply-001"),
		TEXT("sequence_apply_folder"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"create_if_missing\":false,\"save_after_apply\":false}"), *SequenceAssetPath));
	FHttpSmokeResult ApplyFolderResult;
	TestTrue(TEXT("Sequence generic folder apply request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ApplyFolderBody, ApplyFolderResult));
	TestEqual(TEXT("Sequence generic folder apply status code"), ApplyFolderResult.StatusCode, 200);

	const FString ReExportFolderBody = MakeExecRequestBody(
		TEXT("smoke-seqgen-folder-export-002"),
		TEXT("sequence_export_folder"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"clean_output_dir\":true,\"include_validation\":true}"), *SequenceAssetPath));
	FHttpSmokeResult ReExportFolderResult;
	TestTrue(TEXT("Sequence generic folder re-export request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ReExportFolderBody, ReExportFolderResult));
	TestEqual(TEXT("Sequence generic folder re-export status code"), ReExportFolderResult.StatusCode, 200);

	TSharedPtr<FJsonObject> ReObjectTrackJson;
	TSharedPtr<FJsonObject> ReVectorTrackJson;
	TSharedPtr<FJsonObject> ReRotatorTrackJson;
	TSharedPtr<FJsonObject> ReByteTrackJson;
	TSharedPtr<FJsonObject> ReStringTrackJson;
	TSharedPtr<FJsonObject> ReActorReferenceTrackJson;
	TestTrue(TEXT("Sequence generic folder reload object track"), LoadJsonFile(FPaths::Combine(MeshTracksDir, ObjectTrackFile), ReObjectTrackJson));
	TestTrue(TEXT("Sequence generic folder reload vector track"), LoadJsonFile(FPaths::Combine(MeshTracksDir, VectorTrackFile), ReVectorTrackJson));
	TestTrue(TEXT("Sequence generic folder reload rotator track"), LoadJsonFile(FPaths::Combine(MeshTracksDir, RotatorTrackFile), ReRotatorTrackJson));
	TestTrue(TEXT("Sequence generic folder reload byte track"), LoadJsonFile(FPaths::Combine(TerminalTracksDir, ByteTrackFile), ReByteTrackJson));
	TestTrue(TEXT("Sequence generic folder reload string track"), LoadJsonFile(FPaths::Combine(DocTracksDir, StringTrackFile), ReStringTrackJson));
	TestTrue(TEXT("Sequence generic folder reload actor reference track"), LoadJsonFile(FPaths::Combine(CharacterTracksDir, ActorReferenceTrackFile), ReActorReferenceTrackJson));

	auto VerifySecondKeyString = [this](const TSharedPtr<FJsonObject>& TrackJson, const FString& ExpectedValue, const TCHAR* JsonField) -> bool
	{
		if (!TrackJson.IsValid())
		{
			return false;
		}
		const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
		if (!TrackJson->TryGetArrayField(TEXT("sections"), Sections) || !Sections || Sections->Num() != 1)
		{
			return false;
		}
		const TSharedPtr<FJsonObject>* SectionObj = nullptr;
		if (!(*Sections)[0]->TryGetObject(SectionObj) || !SectionObj || !(*SectionObj).IsValid())
		{
			return false;
		}
		const TArray<TSharedPtr<FJsonValue>>* Keys = nullptr;
		if (!(*SectionObj)->TryGetArrayField(TEXT("keys"), Keys) || !Keys || Keys->Num() < 2)
		{
			return false;
		}
		const TSharedPtr<FJsonObject>* KeyObj = nullptr;
		if (!(*Keys)[1]->TryGetObject(KeyObj) || !KeyObj || !(*KeyObj).IsValid())
		{
			return false;
		}
		return (*KeyObj)->GetStringField(JsonField) == ExpectedValue;
	};

	TestTrue(TEXT("Sequence generic folder object value updated"), VerifySecondKeyString(ReObjectTrackJson, FString(TEXT("/Engine/BasicShapes/Cylinder.Cylinder")), TEXT("value_path")));
	TestTrue(TEXT("Sequence generic folder byte value updated"), VerifySecondKeyString(ReByteTrackJson, FString(TEXT("ActivateTarget")), TEXT("value_name")));
	TestTrue(TEXT("Sequence generic folder string value updated"), VerifySecondKeyString(ReStringTrackJson, FString(TEXT("https://dev.epicgames.com/folder-updated")), TEXT("value")));
	TestTrue(TEXT("Sequence generic folder actor reference value updated"), VerifySecondKeyString(ReActorReferenceTrackJson, DoorBindingGuid, TEXT("value_binding_guid")));

	if (ReVectorTrackJson.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
		if (ReVectorTrackJson->TryGetArrayField(TEXT("sections"), Sections) && Sections && Sections->Num() == 1)
		{
			const TSharedPtr<FJsonObject>* SectionObj = nullptr;
			if ((*Sections)[0]->TryGetObject(SectionObj) && SectionObj && (*SectionObj).IsValid())
			{
				const TArray<TSharedPtr<FJsonValue>>* Keys = nullptr;
				if ((*SectionObj)->TryGetArrayField(TEXT("keys"), Keys) && Keys && Keys->Num() >= 2)
				{
					const TSharedPtr<FJsonObject>* KeyObj = nullptr;
					if ((*Keys)[1]->TryGetObject(KeyObj) && KeyObj && (*KeyObj).IsValid())
					{
						const TSharedPtr<FJsonObject>* ValueObj = nullptr;
						if ((*KeyObj)->TryGetObjectField(TEXT("value"), ValueObj) && ValueObj && (*ValueObj).IsValid())
						{
							TestTrue(TEXT("Sequence generic folder vector value updated"), FMath::IsNearlyEqual(static_cast<float>((*ValueObj)->GetNumberField(TEXT("x"))), 2.25f, 0.01f));
						}
					}
				}
			}
		}
	}

	if (ReRotatorTrackJson.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
		if (ReRotatorTrackJson->TryGetArrayField(TEXT("sections"), Sections) && Sections && Sections->Num() == 1)
		{
			const TSharedPtr<FJsonObject>* SectionObj = nullptr;
			if ((*Sections)[0]->TryGetObject(SectionObj) && SectionObj && (*SectionObj).IsValid())
			{
				const TArray<TSharedPtr<FJsonValue>>* Keys = nullptr;
				if ((*SectionObj)->TryGetArrayField(TEXT("keys"), Keys) && Keys && Keys->Num() >= 2)
				{
					const TSharedPtr<FJsonObject>* KeyObj = nullptr;
					if ((*Keys)[1]->TryGetObject(KeyObj) && KeyObj && (*KeyObj).IsValid())
					{
						const TSharedPtr<FJsonObject>* ValueObj = nullptr;
						if ((*KeyObj)->TryGetObjectField(TEXT("value"), ValueObj) && ValueObj && (*ValueObj).IsValid())
						{
							TestTrue(TEXT("Sequence generic folder rotator value updated"), FMath::IsNearlyEqual(static_cast<float>((*ValueObj)->GetNumberField(TEXT("yaw"))), 90.0f, 0.01f));
						}
					}
				}
			}
		}
	}

	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentInterfaceSequenceSkeletalAnimationSmokeTest,
	"GptProjectTest.UeAgentInterface.Smoke.SequenceSkeletalAnimationWorkflow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUeAgentInterfaceSequenceSkeletalAnimationSmokeTest::RunTest(const FString& Parameters)
{
	FScopedUeAgentHttpServer ServerScope;
	FString InitError;
	if (!ServerScope.Initialize(InitError))
	{
		AddError(FString::Printf(TEXT("Initialize failed: %s"), *InitError));
		return false;
	}

	const FString ActorLabel = FString::Printf(TEXT("SmokeSeqSkel_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8));
	const FString SequenceAssetPath = MakeAutomationAssetPath(TEXT("SeqSkel"));
	const FString AnimationAssetPath = TEXT("/Engine/Tutorial/SubEditors/TutorialAssets/Character/Tutorial_Idle.Tutorial_Idle");

	auto FindTrackByClassNeedle = [](const TSharedPtr<FJsonObject>* InfoData, const FString& BindingGuid, const FString& ClassNeedle, TSharedPtr<FJsonObject>& OutTrackObject) -> bool
	{
		if (!InfoData || !InfoData->IsValid())
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* Bindings = nullptr;
		if (!(*InfoData)->TryGetArrayField(TEXT("bindings"), Bindings) || !Bindings)
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& BindingValue : *Bindings)
		{
			const TSharedPtr<FJsonObject>* BindingObject = nullptr;
			if (!BindingValue.IsValid() || !BindingValue->TryGetObject(BindingObject) || !BindingObject || !BindingObject->IsValid())
			{
				continue;
			}
			if ((*BindingObject)->GetStringField(TEXT("guid")) != BindingGuid)
			{
				continue;
			}

			const TArray<TSharedPtr<FJsonValue>>* Tracks = nullptr;
			if (!(*BindingObject)->TryGetArrayField(TEXT("tracks"), Tracks) || !Tracks)
			{
				return false;
			}

			for (const TSharedPtr<FJsonValue>& TrackValue : *Tracks)
			{
				const TSharedPtr<FJsonObject>* TrackObject = nullptr;
				if (!TrackValue.IsValid() || !TrackValue->TryGetObject(TrackObject) || !TrackObject || !TrackObject->IsValid())
				{
					continue;
				}
				if ((*TrackObject)->GetStringField(TEXT("class")).Contains(ClassNeedle))
				{
					OutTrackObject = *TrackObject;
					return true;
				}
			}
		}

		return false;
	};

	const FString SpawnActorBody = MakeExecRequestBody(
		TEXT("smoke-seqskel-spawn-001"),
		TEXT("spawn_actor"),
		FString::Printf(TEXT("{\"class_path\":\"/Script/Engine.StaticMeshActor\",\"label\":\"%s\",\"static_mesh\":\"/Engine/BasicShapes/Cube.Cube\",\"location\":{\"x\":128,\"y\":-256,\"z\":96}}"), *ActorLabel));
	FHttpSmokeResult SpawnActorResult;
	TestTrue(TEXT("Sequence skeletal spawn_actor request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SpawnActorBody, SpawnActorResult));
	TestEqual(TEXT("Sequence skeletal spawn_actor status code"), SpawnActorResult.StatusCode, 200);

	const FString CreateSequenceBody = MakeExecRequestBody(
		TEXT("smoke-seqskel-create-seq-001"),
		TEXT("sequence_create_level_sequence"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"start_seconds\":0,\"duration_seconds\":2.0,\"display_rate_num\":30,\"display_rate_den\":1,\"open_editor\":false,\"save_after_create\":false}"), *SequenceAssetPath));
	FHttpSmokeResult CreateSequenceResult;
	TestTrue(TEXT("Sequence skeletal create_level_sequence request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, CreateSequenceBody, CreateSequenceResult));
	TestEqual(TEXT("Sequence skeletal create_level_sequence status code"), CreateSequenceResult.StatusCode, 200);

	const FString AddBindingBody = MakeExecRequestBody(
		TEXT("smoke-seqskel-add-binding-001"),
		TEXT("sequence_add_actor_binding"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"actor_id\":\"%s\",\"save_after_set\":false}"), *SequenceAssetPath, *ActorLabel));
	FHttpSmokeResult AddBindingResult;
	TestTrue(TEXT("Sequence skeletal add_actor_binding request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddBindingBody, AddBindingResult));
	TestEqual(TEXT("Sequence skeletal add_actor_binding status code"), AddBindingResult.StatusCode, 200);

	TSharedPtr<FJsonObject> AddBindingJson;
	TestTrue(TEXT("Sequence skeletal add_actor_binding parses JSON"), ParseJson(AddBindingResult.Body, AddBindingJson));
	FString BindingGuid;
	const TSharedPtr<FJsonObject>* AddBindingData = nullptr;
	TestTrue(TEXT("Sequence skeletal add_actor_binding contains data"), AddBindingJson.IsValid() && AddBindingJson->TryGetObjectField(TEXT("data"), AddBindingData) && AddBindingData && AddBindingData->IsValid());
	if (AddBindingData && AddBindingData->IsValid())
	{
		BindingGuid = (*AddBindingData)->GetStringField(TEXT("binding_guid"));
		TestTrue(TEXT("Sequence skeletal binding guid non-empty"), !BindingGuid.IsEmpty());
	}

	const FString AddTrackBody = MakeExecRequestBody(
		TEXT("smoke-seqskel-add-track-001"),
		TEXT("sequence_add_skeletal_animation_track"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"binding_guid\":\"%s\",\"save_after_set\":false}"), *SequenceAssetPath, *BindingGuid));
	FHttpSmokeResult AddTrackResult;
	TestTrue(TEXT("Sequence skeletal add_skeletal_animation_track request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddTrackBody, AddTrackResult));
	TestEqual(TEXT("Sequence skeletal add_skeletal_animation_track status code"), AddTrackResult.StatusCode, 200);

	const FString AddSectionBody = MakeExecRequestBody(
		TEXT("smoke-seqskel-add-section-001"),
		TEXT("sequence_add_skeletal_animation_section"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"binding_guid\":\"%s\",\"animation_asset\":\"%s\",\"start_seconds\":0.25,\"duration_seconds\":1.0,\"play_rate\":1.0,\"reverse\":false,\"save_after_set\":false}"), *SequenceAssetPath, *BindingGuid, *AnimationAssetPath));
	FHttpSmokeResult AddSectionResult;
	TestTrue(TEXT("Sequence skeletal add_skeletal_animation_section request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddSectionBody, AddSectionResult));
	TestEqual(TEXT("Sequence skeletal add_skeletal_animation_section status code"), AddSectionResult.StatusCode, 200);

	const FString UpdateSectionBody = MakeExecRequestBody(
		TEXT("smoke-seqskel-update-section-001"),
		TEXT("sequence_update_skeletal_animation_section"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"binding_guid\":\"%s\",\"section_index\":0,\"start_seconds\":0.4,\"duration_seconds\":0.75,\"play_rate\":1.25,\"slot_name\":\"UpperBody\",\"row_index\":2,\"skip_anim_notifiers\":true,\"force_custom_mode\":true,\"save_after_set\":false}"), *SequenceAssetPath, *BindingGuid));
	FHttpSmokeResult UpdateSectionResult;
	TestTrue(TEXT("Sequence skeletal update_skeletal_animation_section request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, UpdateSectionBody, UpdateSectionResult));
	TestEqual(TEXT("Sequence skeletal update_skeletal_animation_section status code"), UpdateSectionResult.StatusCode, 200);

	const FString AddVisibilityTrackBody = MakeExecRequestBody(
		TEXT("smoke-seqskel-add-visibility-track-001"),
		TEXT("sequence_add_visibility_track"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"binding_guid\":\"%s\",\"save_after_set\":false}"), *SequenceAssetPath, *BindingGuid));
	FHttpSmokeResult AddVisibilityTrackResult;
	TestTrue(TEXT("Sequence skeletal add visibility track request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddVisibilityTrackBody, AddVisibilityTrackResult));
	TestEqual(TEXT("Sequence skeletal add visibility track status code"), AddVisibilityTrackResult.StatusCode, 200);

	const FString AddVisibilityKeyBody = MakeExecRequestBody(
		TEXT("smoke-seqskel-add-visibility-key-001"),
		TEXT("sequence_add_visibility_key"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"binding_guid\":\"%s\",\"time_seconds\":0.6,\"visible\":false,\"visible_before_key\":true,\"save_after_set\":false}"), *SequenceAssetPath, *BindingGuid));
	FHttpSmokeResult AddVisibilityKeyResult;
	TestTrue(TEXT("Sequence skeletal add visibility key request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddVisibilityKeyBody, AddVisibilityKeyResult));
	TestEqual(TEXT("Sequence skeletal add visibility key status code"), AddVisibilityKeyResult.StatusCode, 200);

	const FString GetInfoBody = MakeExecRequestBody(
		TEXT("smoke-seqskel-info-001"),
		TEXT("sequence_get_level_sequence_info"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *SequenceAssetPath));
	FHttpSmokeResult GetInfoResult;
	TestTrue(TEXT("Sequence skeletal get_level_sequence_info request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, GetInfoBody, GetInfoResult));
	TestEqual(TEXT("Sequence skeletal get_level_sequence_info status code"), GetInfoResult.StatusCode, 200);

	TSharedPtr<FJsonObject> GetInfoJson;
	TestTrue(TEXT("Sequence skeletal get_level_sequence_info parses JSON"), ParseJson(GetInfoResult.Body, GetInfoJson));
	const TSharedPtr<FJsonObject>* GetInfoData = nullptr;
	TestTrue(TEXT("Sequence skeletal get_level_sequence_info contains data"), GetInfoJson.IsValid() && GetInfoJson->TryGetObjectField(TEXT("data"), GetInfoData) && GetInfoData && GetInfoData->IsValid());
	if (GetInfoData && GetInfoData->IsValid())
	{
		TSharedPtr<FJsonObject> TrackObject;
		TestTrue(TEXT("Sequence skeletal track summary exists"), FindTrackByClassNeedle(GetInfoData, BindingGuid, TEXT("MovieSceneSkeletalAnimationTrack"), TrackObject));
		if (TrackObject.IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
			TestTrue(TEXT("Sequence skeletal track summary contains sections"), TrackObject->TryGetArrayField(TEXT("sections"), Sections) && Sections != nullptr);
			if (Sections && Sections->Num() > 0)
			{
				const TSharedPtr<FJsonObject>* SectionObject = nullptr;
				TestTrue(TEXT("Sequence skeletal section summary parses"), (*Sections)[0].IsValid() && (*Sections)[0]->TryGetObject(SectionObject) && SectionObject && SectionObject->IsValid());
				if (SectionObject && SectionObject->IsValid())
				{
					TestEqual(TEXT("Sequence skeletal section animation asset"), (*SectionObject)->GetStringField(TEXT("animation_asset")), AnimationAssetPath);
					TestEqual(TEXT("Sequence skeletal section slot name updated"), (*SectionObject)->GetStringField(TEXT("slot_name")), FString(TEXT("UpperBody")));
					TestEqual(TEXT("Sequence skeletal section row index updated"), static_cast<int32>((*SectionObject)->GetIntegerField(TEXT("row_index"))), 2);
					TestEqual(TEXT("Sequence skeletal section skip notifiers updated"), (*SectionObject)->GetBoolField(TEXT("skip_anim_notifiers")), true);
					TestEqual(TEXT("Sequence skeletal section force custom mode updated"), (*SectionObject)->GetBoolField(TEXT("force_custom_mode")), true);
				}
			}
		}

		TSharedPtr<FJsonObject> VisibilityTrackObject;
		TestTrue(TEXT("Sequence visibility track summary exists"), FindTrackByClassNeedle(GetInfoData, BindingGuid, TEXT("MovieSceneVisibilityTrack"), VisibilityTrackObject));
		if (VisibilityTrackObject.IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
			TestTrue(TEXT("Sequence visibility track summary contains sections"), VisibilityTrackObject->TryGetArrayField(TEXT("sections"), Sections) && Sections != nullptr);
			if (Sections && Sections->Num() > 0)
			{
				const TSharedPtr<FJsonObject>* SectionObject = nullptr;
				TestTrue(TEXT("Sequence visibility section summary parses"), (*Sections)[0].IsValid() && (*Sections)[0]->TryGetObject(SectionObject) && SectionObject && SectionObject->IsValid());
				if (SectionObject && SectionObject->IsValid())
				{
					TestEqual(TEXT("Sequence visibility section type"), (*SectionObject)->GetStringField(TEXT("section_type")), FString(TEXT("visibility")));
					TestTrue(TEXT("Sequence visibility section has keys"), static_cast<int32>((*SectionObject)->GetIntegerField(TEXT("key_count"))) >= 1);
				}
			}
		}
	}

	const FString RemoveSectionBody = MakeExecRequestBody(
		TEXT("smoke-seqskel-remove-section-001"),
		TEXT("sequence_remove_skeletal_animation_section"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"binding_guid\":\"%s\",\"section_index\":0,\"save_after_remove\":false}"), *SequenceAssetPath, *BindingGuid));
	FHttpSmokeResult RemoveSectionResult;
	TestTrue(TEXT("Sequence skeletal remove_skeletal_animation_section request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, RemoveSectionBody, RemoveSectionResult));
	TestEqual(TEXT("Sequence skeletal remove_skeletal_animation_section status code"), RemoveSectionResult.StatusCode, 200);

	const FString GetInfoAfterRemoveBody = MakeExecRequestBody(
		TEXT("smoke-seqskel-info-002"),
		TEXT("sequence_get_level_sequence_info"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *SequenceAssetPath));
	FHttpSmokeResult GetInfoAfterRemoveResult;
	TestTrue(TEXT("Sequence skeletal get info after remove request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, GetInfoAfterRemoveBody, GetInfoAfterRemoveResult));
	TestEqual(TEXT("Sequence skeletal get info after remove status code"), GetInfoAfterRemoveResult.StatusCode, 200);

	TSharedPtr<FJsonObject> GetInfoAfterRemoveJson;
	TestTrue(TEXT("Sequence skeletal get info after remove parses JSON"), ParseJson(GetInfoAfterRemoveResult.Body, GetInfoAfterRemoveJson));
	const TSharedPtr<FJsonObject>* GetInfoAfterRemoveData = nullptr;
	TestTrue(TEXT("Sequence skeletal get info after remove contains data"), GetInfoAfterRemoveJson.IsValid() && GetInfoAfterRemoveJson->TryGetObjectField(TEXT("data"), GetInfoAfterRemoveData) && GetInfoAfterRemoveData && GetInfoAfterRemoveData->IsValid());
	if (GetInfoAfterRemoveData && GetInfoAfterRemoveData->IsValid())
	{
		TSharedPtr<FJsonObject> TrackObject;
		TestTrue(TEXT("Sequence skeletal track summary still exists after remove"), FindTrackByClassNeedle(GetInfoAfterRemoveData, BindingGuid, TEXT("MovieSceneSkeletalAnimationTrack"), TrackObject));
		if (TrackObject.IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
			TestTrue(TEXT("Sequence skeletal track summary contains sections array after remove"), TrackObject->TryGetArrayField(TEXT("sections"), Sections) && Sections != nullptr);
			if (Sections)
			{
				TestEqual(TEXT("Sequence skeletal section count after remove"), Sections->Num(), 0);
			}
		}
	}

	const FString DestroyActorBody = MakeExecRequestBody(
		TEXT("smoke-seqskel-destroy-001"),
		TEXT("destroy_actor"),
		FString::Printf(TEXT("{\"id\":\"%s\"}"), *ActorLabel));
	FHttpSmokeResult DestroyActorResult;
	TestTrue(TEXT("Sequence skeletal destroy_actor request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, DestroyActorBody, DestroyActorResult));
	TestEqual(TEXT("Sequence skeletal destroy_actor status code"), DestroyActorResult.StatusCode, 200);

	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentInterfaceSequenceUmgAnimationSmokeTest,
	"GptProjectTest.UeAgentInterface.Smoke.SequenceUmgAnimationWorkflow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUeAgentInterfaceSequenceUmgAnimationSmokeTest::RunTest(const FString& Parameters)
{
	FScopedUeAgentHttpServer ServerScope;
	FString InitError;
	if (!ServerScope.Initialize(InitError))
	{
		AddError(FString::Printf(TEXT("Initialize failed: %s"), *InitError));
		return false;
	}

	const FString WidgetBlueprintAssetPath = MakeAutomationAssetPath(TEXT("WBPSeqAnim"));
	const FString WidgetName = TEXT("BtnSeqAnim");
	const FString RequestedAnimationName = TEXT("IntroAnim");
	const FString RenamedAnimationName = TEXT("IntroAnimRenamed");

	auto FindBindingTrackByProperty = [](const TSharedPtr<FJsonObject>* InfoData, const FString& WidgetNameNeedle, const FString& PropertyName, TSharedPtr<FJsonObject>& OutTrackObject) -> bool
	{
		if (!InfoData || !InfoData->IsValid())
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* Bindings = nullptr;
		if (!(*InfoData)->TryGetArrayField(TEXT("bindings"), Bindings) || !Bindings)
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& BindingValue : *Bindings)
		{
			const TSharedPtr<FJsonObject>* BindingObject = nullptr;
			if (!BindingValue.IsValid() || !BindingValue->TryGetObject(BindingObject) || !BindingObject || !BindingObject->IsValid())
			{
				continue;
			}
			if ((*BindingObject)->GetStringField(TEXT("widget_name")) != WidgetNameNeedle)
			{
				continue;
			}

			const TArray<TSharedPtr<FJsonValue>>* Tracks = nullptr;
			if (!(*BindingObject)->TryGetArrayField(TEXT("tracks"), Tracks) || !Tracks)
			{
				return false;
			}

			for (const TSharedPtr<FJsonValue>& TrackValue : *Tracks)
			{
				const TSharedPtr<FJsonObject>* TrackObject = nullptr;
				if (!TrackValue.IsValid() || !TrackValue->TryGetObject(TrackObject) || !TrackObject || !TrackObject->IsValid())
				{
					continue;
				}

				if ((*TrackObject)->GetStringField(TEXT("property_name")).Equals(PropertyName, ESearchCase::IgnoreCase))
				{
					OutTrackObject = *TrackObject;
					return true;
				}
			}
		}

		return false;
	};

	auto ListContainsAnimation = [](const TSharedPtr<FJsonObject>* ListData, const FString& AnimationName, bool& bFoundOut) -> bool
	{
		bFoundOut = false;
		if (!ListData || !ListData->IsValid())
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* Animations = nullptr;
		if (!(*ListData)->TryGetArrayField(TEXT("animations"), Animations) || !Animations)
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& AnimationValue : *Animations)
		{
			const TSharedPtr<FJsonObject>* AnimationObject = nullptr;
			if (!AnimationValue.IsValid() || !AnimationValue->TryGetObject(AnimationObject) || !AnimationObject || !AnimationObject->IsValid())
			{
				continue;
			}
			if ((*AnimationObject)->GetStringField(TEXT("name")) == AnimationName)
			{
				bFoundOut = true;
				break;
			}
		}

		return true;
	};

	const FString CreateBody = MakeExecRequestBody(
		TEXT("smoke-umgseq-create-001"),
		TEXT("umg_create_widget_blueprint"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"parent_class\":\"/Script/UMG.UserWidget\",\"create_default_root\":true,\"compile_after_create\":false,\"open_editor\":false,\"save_after_create\":false}"), *WidgetBlueprintAssetPath));
	FHttpSmokeResult CreateResult;
	TestTrue(TEXT("Sequence UMG create widget blueprint request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, CreateBody, CreateResult));
	TestEqual(TEXT("Sequence UMG create widget blueprint status code"), CreateResult.StatusCode, 200);

	TSharedPtr<FJsonObject> CreateJson;
	TestTrue(TEXT("Sequence UMG create widget blueprint parses JSON"), ParseJson(CreateResult.Body, CreateJson));
	FString RootWidgetName;
	const TSharedPtr<FJsonObject>* CreateData = nullptr;
	TestTrue(TEXT("Sequence UMG create widget blueprint contains data"), CreateJson.IsValid() && CreateJson->TryGetObjectField(TEXT("data"), CreateData) && CreateData && CreateData->IsValid());
	if (CreateData && CreateData->IsValid())
	{
		RootWidgetName = (*CreateData)->GetStringField(TEXT("root_widget"));
	}

	const FString AddWidgetBody = MakeExecRequestBody(
		TEXT("smoke-umgseq-add-widget-001"),
		TEXT("umg_add_widget"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"widget_class\":\"/Script/UMG.Button\",\"widget_name\":\"%s\",\"parent_widget\":\"%s\",\"make_variable\":true,\"compile_after_add\":false,\"save_after_add\":false}"), *WidgetBlueprintAssetPath, *WidgetName, *RootWidgetName));
	FHttpSmokeResult AddWidgetResult;
	TestTrue(TEXT("Sequence UMG add widget request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddWidgetBody, AddWidgetResult));
	TestEqual(TEXT("Sequence UMG add widget status code"), AddWidgetResult.StatusCode, 200);

	const FString CreateAnimationBody = MakeExecRequestBody(
		TEXT("smoke-umgseq-create-anim-001"),
		TEXT("sequence_create_umg_animation"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"animation_name\":\"%s\",\"start_seconds\":0.0,\"duration_seconds\":1.0,\"compile_after_create\":false,\"save_after_create\":false}"), *WidgetBlueprintAssetPath, *RequestedAnimationName));
	FHttpSmokeResult CreateAnimationResult;
	TestTrue(TEXT("Sequence UMG create animation request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, CreateAnimationBody, CreateAnimationResult));
	TestEqual(TEXT("Sequence UMG create animation status code"), CreateAnimationResult.StatusCode, 200);

	TSharedPtr<FJsonObject> CreateAnimationJson;
	TestTrue(TEXT("Sequence UMG create animation parses JSON"), ParseJson(CreateAnimationResult.Body, CreateAnimationJson));
	FString AnimationName;
	const TSharedPtr<FJsonObject>* CreateAnimationData = nullptr;
	TestTrue(TEXT("Sequence UMG create animation contains data"), CreateAnimationJson.IsValid() && CreateAnimationJson->TryGetObjectField(TEXT("data"), CreateAnimationData) && CreateAnimationData && CreateAnimationData->IsValid());
	if (CreateAnimationData && CreateAnimationData->IsValid())
	{
		AnimationName = (*CreateAnimationData)->GetStringField(TEXT("animation_name"));
		TestTrue(TEXT("Sequence UMG create animation actual name non-empty"), !AnimationName.IsEmpty());
	}

	const FString ListAnimationsBody = MakeExecRequestBody(
		TEXT("smoke-umgseq-list-anim-001"),
		TEXT("sequence_list_umg_animations"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *WidgetBlueprintAssetPath));
	FHttpSmokeResult ListAnimationsResult;
	TestTrue(TEXT("Sequence UMG list animations request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ListAnimationsBody, ListAnimationsResult));
	TestEqual(TEXT("Sequence UMG list animations status code"), ListAnimationsResult.StatusCode, 200);

	TSharedPtr<FJsonObject> ListAnimationsJson;
	TestTrue(TEXT("Sequence UMG list animations parses JSON"), ParseJson(ListAnimationsResult.Body, ListAnimationsJson));
	const TSharedPtr<FJsonObject>* ListAnimationsData = nullptr;
	TestTrue(TEXT("Sequence UMG list animations contains data"), ListAnimationsJson.IsValid() && ListAnimationsJson->TryGetObjectField(TEXT("data"), ListAnimationsData) && ListAnimationsData && ListAnimationsData->IsValid());
	if (ListAnimationsData && ListAnimationsData->IsValid())
	{
		bool bFoundAnimation = false;
		TestTrue(TEXT("Sequence UMG list animations array exists"), ListContainsAnimation(ListAnimationsData, AnimationName, bFoundAnimation));
		TestTrue(TEXT("Sequence UMG list animations contains created animation"), bFoundAnimation);
	}

	const FString SetPlaybackRangeBody = MakeExecRequestBody(
		TEXT("smoke-umgseq-range-001"),
		TEXT("sequence_set_umg_animation_playback_range"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"animation_name\":\"%s\",\"start_seconds\":0.1,\"duration_seconds\":1.4,\"save_after_set\":false}"), *WidgetBlueprintAssetPath, *AnimationName));
	FHttpSmokeResult SetPlaybackRangeResult;
	TestTrue(TEXT("Sequence UMG set playback range request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SetPlaybackRangeBody, SetPlaybackRangeResult));
	TestEqual(TEXT("Sequence UMG set playback range status code"), SetPlaybackRangeResult.StatusCode, 200);

	const FString SetDisplayRateBody = MakeExecRequestBody(
		TEXT("smoke-umgseq-rate-001"),
		TEXT("sequence_set_umg_animation_display_rate"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"animation_name\":\"%s\",\"display_rate_num\":24,\"display_rate_den\":1,\"save_after_set\":false}"), *WidgetBlueprintAssetPath, *AnimationName));
	FHttpSmokeResult SetDisplayRateResult;
	TestTrue(TEXT("Sequence UMG set display rate request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, SetDisplayRateBody, SetDisplayRateResult));
	TestEqual(TEXT("Sequence UMG set display rate status code"), SetDisplayRateResult.StatusCode, 200);

	const FString RenameAnimationBody = MakeExecRequestBody(
		TEXT("smoke-umgseq-rename-001"),
		TEXT("sequence_rename_umg_animation"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"animation_name\":\"%s\",\"new_animation_name\":\"%s\",\"compile_after_rename\":false,\"save_after_rename\":false}"), *WidgetBlueprintAssetPath, *AnimationName, *RenamedAnimationName));
	FHttpSmokeResult RenameAnimationResult;
	TestTrue(TEXT("Sequence UMG rename animation request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, RenameAnimationBody, RenameAnimationResult));
	TestEqual(TEXT("Sequence UMG rename animation status code"), RenameAnimationResult.StatusCode, 200);
	AnimationName = RenamedAnimationName;

	const FString AddTransformKeyBody = MakeExecRequestBody(
		TEXT("smoke-umgseq-transform-key-001"),
		TEXT("sequence_add_umg_widget_transform_key"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"animation_name\":\"%s\",\"widget_name\":\"%s\",\"time_seconds\":0.0,\"translation_x\":16.0,\"translation_y\":8.0,\"scale_x\":1.1,\"scale_y\":0.9,\"angle\":12.0,\"compile_after_set\":false,\"save_after_set\":false}"), *WidgetBlueprintAssetPath, *AnimationName, *WidgetName));
	FHttpSmokeResult AddTransformKeyResult;
	TestTrue(TEXT("Sequence UMG add transform key request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddTransformKeyBody, AddTransformKeyResult));
	TestEqual(TEXT("Sequence UMG add transform key status code"), AddTransformKeyResult.StatusCode, 200);

	const FString AddOpacityKeyBody = MakeExecRequestBody(
		TEXT("smoke-umgseq-opacity-key-001"),
		TEXT("sequence_add_umg_widget_opacity_key"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"animation_name\":\"%s\",\"widget_name\":\"%s\",\"time_seconds\":0.25,\"opacity\":0.4,\"compile_after_set\":false,\"save_after_set\":false}"), *WidgetBlueprintAssetPath, *AnimationName, *WidgetName));
	FHttpSmokeResult AddOpacityKeyResult;
	TestTrue(TEXT("Sequence UMG add opacity key request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddOpacityKeyBody, AddOpacityKeyResult));
	TestEqual(TEXT("Sequence UMG add opacity key status code"), AddOpacityKeyResult.StatusCode, 200);

	const FString AddColorKeyBody = MakeExecRequestBody(
		TEXT("smoke-umgseq-color-key-001"),
		TEXT("sequence_add_umg_widget_color_key"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"animation_name\":\"%s\",\"widget_name\":\"%s\",\"time_seconds\":0.5,\"red\":0.2,\"green\":0.4,\"blue\":0.6,\"alpha\":0.8,\"compile_after_set\":false,\"save_after_set\":false}"), *WidgetBlueprintAssetPath, *AnimationName, *WidgetName));
	FHttpSmokeResult AddColorKeyResult;
	TestTrue(TEXT("Sequence UMG add color key request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, AddColorKeyBody, AddColorKeyResult));
	TestEqual(TEXT("Sequence UMG add color key status code"), AddColorKeyResult.StatusCode, 200);

	const FString GetAnimationInfoBody = MakeExecRequestBody(
		TEXT("smoke-umgseq-info-001"),
		TEXT("sequence_get_umg_animation_info"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"animation_name\":\"%s\"}"), *WidgetBlueprintAssetPath, *AnimationName));
	FHttpSmokeResult GetAnimationInfoResult;
	TestTrue(TEXT("Sequence UMG get animation info request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, GetAnimationInfoBody, GetAnimationInfoResult));
	TestEqual(TEXT("Sequence UMG get animation info status code"), GetAnimationInfoResult.StatusCode, 200);

	TSharedPtr<FJsonObject> GetAnimationInfoJson;
	TestTrue(TEXT("Sequence UMG get animation info parses JSON"), ParseJson(GetAnimationInfoResult.Body, GetAnimationInfoJson));
	const TSharedPtr<FJsonObject>* GetAnimationInfoData = nullptr;
	TestTrue(TEXT("Sequence UMG get animation info contains data"), GetAnimationInfoJson.IsValid() && GetAnimationInfoJson->TryGetObjectField(TEXT("data"), GetAnimationInfoData) && GetAnimationInfoData && GetAnimationInfoData->IsValid());
	if (GetAnimationInfoData && GetAnimationInfoData->IsValid())
	{
		TestEqual(TEXT("Sequence UMG info display rate updated"), (*GetAnimationInfoData)->GetStringField(TEXT("display_rate")), FString(TEXT("24 fps")));
		TSharedPtr<FJsonObject> TransformTrackObject;
		TSharedPtr<FJsonObject> OpacityTrackObject;
		TSharedPtr<FJsonObject> ColorTrackObject;
		TestTrue(TEXT("Sequence UMG transform track exists"), FindBindingTrackByProperty(GetAnimationInfoData, WidgetName, TEXT("RenderTransform"), TransformTrackObject));
		TestTrue(TEXT("Sequence UMG opacity track exists"), FindBindingTrackByProperty(GetAnimationInfoData, WidgetName, TEXT("RenderOpacity"), OpacityTrackObject));
		TestTrue(TEXT("Sequence UMG color track exists"), FindBindingTrackByProperty(GetAnimationInfoData, WidgetName, TEXT("ColorAndOpacity"), ColorTrackObject));
		if (ColorTrackObject.IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
			TestTrue(TEXT("Sequence UMG color track contains sections"), ColorTrackObject->TryGetArrayField(TEXT("sections"), Sections) && Sections != nullptr);
			if (Sections && Sections->Num() > 0)
			{
				const TSharedPtr<FJsonObject>* SectionObject = nullptr;
				TestTrue(TEXT("Sequence UMG color section parses"), (*Sections)[0].IsValid() && (*Sections)[0]->TryGetObject(SectionObject) && SectionObject && SectionObject->IsValid());
				if (SectionObject && SectionObject->IsValid())
				{
					TestEqual(TEXT("Sequence UMG color section red key count"), static_cast<int32>((*SectionObject)->GetIntegerField(TEXT("red_key_count"))), 1);
					TestEqual(TEXT("Sequence UMG color section alpha key count"), static_cast<int32>((*SectionObject)->GetIntegerField(TEXT("alpha_key_count"))), 1);
				}
			}
		}
	}

	const FString RemoveAnimationBody = MakeExecRequestBody(
		TEXT("smoke-umgseq-remove-001"),
		TEXT("sequence_remove_umg_animation"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\",\"animation_name\":\"%s\",\"compile_after_remove\":false,\"save_after_remove\":false}"), *WidgetBlueprintAssetPath, *AnimationName));
	FHttpSmokeResult RemoveAnimationResult;
	TestTrue(TEXT("Sequence UMG remove animation request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, RemoveAnimationBody, RemoveAnimationResult));
	TestEqual(TEXT("Sequence UMG remove animation status code"), RemoveAnimationResult.StatusCode, 200);

	const FString ListAnimationsAfterRemoveBody = MakeExecRequestBody(
		TEXT("smoke-umgseq-list-anim-002"),
		TEXT("sequence_list_umg_animations"),
		FString::Printf(TEXT("{\"asset_path\":\"%s\"}"), *WidgetBlueprintAssetPath));
	FHttpSmokeResult ListAnimationsAfterRemoveResult;
	TestTrue(TEXT("Sequence UMG list animations after remove request succeeded"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, ListAnimationsAfterRemoveBody, ListAnimationsAfterRemoveResult));
	TestEqual(TEXT("Sequence UMG list animations after remove status code"), ListAnimationsAfterRemoveResult.StatusCode, 200);

	TSharedPtr<FJsonObject> ListAnimationsAfterRemoveJson;
	TestTrue(TEXT("Sequence UMG list animations after remove parses JSON"), ParseJson(ListAnimationsAfterRemoveResult.Body, ListAnimationsAfterRemoveJson));
	const TSharedPtr<FJsonObject>* ListAnimationsAfterRemoveData = nullptr;
	TestTrue(TEXT("Sequence UMG list animations after remove contains data"), ListAnimationsAfterRemoveJson.IsValid() && ListAnimationsAfterRemoveJson->TryGetObjectField(TEXT("data"), ListAnimationsAfterRemoveData) && ListAnimationsAfterRemoveData && ListAnimationsAfterRemoveData->IsValid());
	if (ListAnimationsAfterRemoveData && ListAnimationsAfterRemoveData->IsValid())
	{
		bool bFoundAnimationAfterRemove = false;
		TestTrue(TEXT("Sequence UMG list animations after remove array exists"), ListContainsAnimation(ListAnimationsAfterRemoveData, AnimationName, bFoundAnimationAfterRemove));
		TestFalse(TEXT("Sequence UMG animation removed from list"), bFoundAnimationAfterRemove);
	}

	return !HasAnyErrors();
}

#endif

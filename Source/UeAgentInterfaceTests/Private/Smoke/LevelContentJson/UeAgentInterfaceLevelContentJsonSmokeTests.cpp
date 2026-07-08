#if WITH_DEV_AUTOMATION_TESTS

#include "../UeAgentSmokeTestSharedIncludes.inl"
#include "Serialization/JsonWriter.h"

namespace
{
	static bool ExecLevelContentSmokeCommand(
		FAutomationTestBase* Test,
		const FScopedUeAgentHttpServer& ServerScope,
		const FString& RequestId,
		const FString& Command,
		const FString& ParamsJson,
		TSharedPtr<FJsonObject>& OutData,
		bool bExpectOk = true,
		double TimeoutSeconds = 30.0)
	{
		const FString Body = MakeExecRequestBody(RequestId, Command, ParamsJson);
		FHttpSmokeResult Result;
		if (!Test->TestTrue(Command + TEXT(" request completed"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, Body, Result, TimeoutSeconds)))
		{
			return false;
		}

		Test->TestEqual(Command + TEXT(" status code"), Result.StatusCode, bExpectOk ? 200 : 400);
		TSharedPtr<FJsonObject> Root;
		if (!Test->TestTrue(Command + TEXT(" response parses"), ParseJson(Result.Body, Root)) || !Root.IsValid())
		{
			return false;
		}
		bool bOk = false;
		Root->TryGetBoolField(TEXT("ok"), bOk);
		Test->TestEqual(Command + TEXT(" ok flag"), bOk, bExpectOk);

		const TSharedPtr<FJsonObject>* Data = nullptr;
		if (!Test->TestTrue(Command + TEXT(" has data"), Root->TryGetObjectField(TEXT("data"), Data) && Data && Data->IsValid()))
		{
			return false;
		}
		OutData = *Data;
		FString EnvelopeError;
		if (!bExpectOk && Root->TryGetStringField(TEXT("error"), EnvelopeError))
		{
			OutData->SetStringField(TEXT("error"), EnvelopeError);
		}
		return true;
	}

	static bool JsonArrayContainsCode(const TArray<TSharedPtr<FJsonValue>>* Issues, const FString& Code)
	{
		if (!Issues)
		{
			return false;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Issues)
		{
			const TSharedPtr<FJsonObject> Issue = Value.IsValid() ? Value->AsObject() : nullptr;
			if (Issue.IsValid() && Issue->GetStringField(TEXT("code")).Equals(Code, ESearchCase::IgnoreCase))
			{
				return true;
			}
		}
		return false;
	}

	static bool JsonArrayContainsStringField(const TArray<TSharedPtr<FJsonValue>>* Items, const TCHAR* FieldName, const FString& ExpectedValue)
	{
		if (!Items)
		{
			return false;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Items)
		{
			const TSharedPtr<FJsonObject> Item = Value.IsValid() ? Value->AsObject() : nullptr;
			if (Item.IsValid() && Item->GetStringField(FieldName).Equals(ExpectedValue, ESearchCase::IgnoreCase))
			{
				return true;
			}
		}
		return false;
	}

	static TSharedPtr<FJsonObject> FindJsonObjectByStringField(const TArray<TSharedPtr<FJsonValue>>* Items, const TCHAR* FieldName, const FString& ExpectedValue)
	{
		if (!Items)
		{
			return nullptr;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Items)
		{
			const TSharedPtr<FJsonObject> Item = Value.IsValid() ? Value->AsObject() : nullptr;
			FString ActualValue;
			if (Item.IsValid() && Item->TryGetStringField(FieldName, ActualValue) && ActualValue.Equals(ExpectedValue, ESearchCase::IgnoreCase))
			{
				return Item;
			}
		}
		return nullptr;
	}

	static bool ResourcePropertyResultHasBoolFlag(const TArray<TSharedPtr<FJsonValue>>* ResourceResults, const FString& ResourceId, const FString& PropertyPath, const TCHAR* FlagName)
	{
		const TSharedPtr<FJsonObject> ResourceResult = FindJsonObjectByStringField(ResourceResults, TEXT("id"), ResourceId);
		if (!ResourceResult.IsValid())
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* PropertyResults = nullptr;
		if (!ResourceResult->TryGetArrayField(TEXT("property_results"), PropertyResults) || !PropertyResults)
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& Value : *PropertyResults)
		{
			const TSharedPtr<FJsonObject> PropertyResult = Value.IsValid() ? Value->AsObject() : nullptr;
			FString ActualPath;
			bool bFlagValue = false;
			if (PropertyResult.IsValid()
				&& PropertyResult->TryGetStringField(TEXT("property_path"), ActualPath)
				&& ActualPath.Equals(PropertyPath, ESearchCase::IgnoreCase)
				&& PropertyResult->TryGetBoolField(FlagName, bFlagValue)
				&& bFlagValue)
			{
				return true;
			}
		}
		return false;
	}

	static bool TryGetResourcePropertyResultStringField(
		const TArray<TSharedPtr<FJsonValue>>* ResourceResults,
		const FString& ResourceId,
		const FString& PropertyPath,
		const TCHAR* FieldName,
		FString& OutValue)
	{
		const TSharedPtr<FJsonObject> ResourceResult = FindJsonObjectByStringField(ResourceResults, TEXT("id"), ResourceId);
		if (!ResourceResult.IsValid())
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* PropertyResults = nullptr;
		if (!ResourceResult->TryGetArrayField(TEXT("property_results"), PropertyResults) || !PropertyResults)
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& Value : *PropertyResults)
		{
			const TSharedPtr<FJsonObject> PropertyResult = Value.IsValid() ? Value->AsObject() : nullptr;
			FString ActualPath;
			if (PropertyResult.IsValid()
				&& PropertyResult->TryGetStringField(TEXT("property_path"), ActualPath)
				&& ActualPath.Equals(PropertyPath, ESearchCase::IgnoreCase)
				&& PropertyResult->TryGetStringField(FieldName, OutValue))
			{
				return true;
			}
		}
		return false;
	}

	static bool BoundsExtentExceeds(const TSharedPtr<FJsonObject>& Resource, double MinX, double MinY, double MinZ)
	{
		if (!Resource.IsValid())
		{
			return false;
		}
		const TSharedPtr<FJsonObject>* Bounds = nullptr;
		const TSharedPtr<FJsonObject>* Extent = nullptr;
		bool bValidBounds = false;
		if (!Resource->TryGetObjectField(TEXT("bounds"), Bounds) || !Bounds || !Bounds->IsValid())
		{
			return false;
		}
		(*Bounds)->TryGetBoolField(TEXT("valid"), bValidBounds);
		if (!bValidBounds || !(*Bounds)->TryGetObjectField(TEXT("extent"), Extent) || !Extent || !Extent->IsValid())
		{
			return false;
		}
		return (*Extent)->GetNumberField(TEXT("x")) > MinX
			&& (*Extent)->GetNumberField(TEXT("y")) > MinY
			&& (*Extent)->GetNumberField(TEXT("z")) > MinZ;
	}

	static bool ResourceHasComponentWithSplinePointCount(const TArray<TSharedPtr<FJsonValue>>* Resources, const FString& ResourceId, const FString& ComponentName, int32 ExpectedPointCount)
	{
		const TSharedPtr<FJsonObject> Resource = FindJsonObjectByStringField(Resources, TEXT("id"), ResourceId);
		if (!Resource.IsValid())
		{
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* Components = nullptr;
		if (!Resource->TryGetArrayField(TEXT("components"), Components) || !Components)
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& Value : *Components)
		{
			const TSharedPtr<FJsonObject> Component = Value.IsValid() ? Value->AsObject() : nullptr;
			FString ActualName;
			if (Component.IsValid()
				&& Component->TryGetStringField(TEXT("name"), ActualName)
				&& ActualName.Equals(ComponentName, ESearchCase::IgnoreCase)
				&& Component->GetIntegerField(TEXT("spline_point_count")) == ExpectedPointCount
				&& Component->HasTypedField<EJson::Array>(TEXT("spline_points")))
			{
				return true;
			}
		}
		return false;
	}

	static bool ResourceLocationMatches(const TArray<TSharedPtr<FJsonValue>>* Resources, const FString& ResourceId, double ExpectedX, double ExpectedY, double ExpectedZ, double Tolerance = 0.1)
	{
		const TSharedPtr<FJsonObject> Resource = FindJsonObjectByStringField(Resources, TEXT("id"), ResourceId);
		if (!Resource.IsValid())
		{
			return false;
		}

		const TSharedPtr<FJsonObject>* Transform = nullptr;
		const TSharedPtr<FJsonObject>* Location = nullptr;
		if (!Resource->TryGetObjectField(TEXT("transform"), Transform) || !Transform || !Transform->IsValid() ||
			!(*Transform)->TryGetObjectField(TEXT("location"), Location) || !Location || !Location->IsValid())
		{
			return false;
		}

		return FMath::Abs((*Location)->GetNumberField(TEXT("x")) - ExpectedX) <= Tolerance
			&& FMath::Abs((*Location)->GetNumberField(TEXT("y")) - ExpectedY) <= Tolerance
			&& FMath::Abs((*Location)->GetNumberField(TEXT("z")) - ExpectedZ) <= Tolerance;
	}

	static bool ResourceTransformHasSpaceContract(const TArray<TSharedPtr<FJsonValue>>* Resources, const FString& ResourceId, const FString& ExpectedReadFrame)
	{
		const TSharedPtr<FJsonObject> Resource = FindJsonObjectByStringField(Resources, TEXT("id"), ResourceId);
		if (!Resource.IsValid())
		{
			return false;
		}

		const TSharedPtr<FJsonObject>* Transform = nullptr;
		const TSharedPtr<FJsonObject>* SpaceContract = nullptr;
		if (!Resource->TryGetObjectField(TEXT("transform"), Transform) || !Transform || !Transform->IsValid() ||
			!(*Transform)->TryGetObjectField(TEXT("space_contract"), SpaceContract) || !SpaceContract || !SpaceContract->IsValid())
		{
			return false;
		}

		return (*SpaceContract)->GetStringField(TEXT("Fread")) == ExpectedReadFrame
			&& (*SpaceContract)->GetStringField(TEXT("Fwrite")) == ExpectedReadFrame;
	}

	static FString JsonObjectToCompactString(const TSharedPtr<FJsonObject>& Object)
	{
		if (!Object.IsValid())
		{
			return TEXT("<invalid-json-object>");
		}

		FString Out;
		TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
		FJsonSerializer::Serialize(Object.ToSharedRef(), Writer);
		return Out;
	}

	static FString SmokeJsonString(const FString& Value)
	{
		FString Escaped = Value;
		Escaped.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
		Escaped.ReplaceInline(TEXT("\""), TEXT("\\\""));
		return FString::Printf(TEXT("\"%s\""), *Escaped);
	}

	static bool ActorSummaryMatchesLabel(const TSharedPtr<FJsonObject>& ActorSummary, const FString& ExpectedLabel)
	{
		if (!ActorSummary.IsValid())
		{
			return false;
		}

		FString Label;
		if (ActorSummary->TryGetStringField(TEXT("label"), Label) && Label.Equals(ExpectedLabel, ESearchCase::IgnoreCase))
		{
			return true;
		}

		FString Name;
		return ActorSummary->TryGetStringField(TEXT("name"), Name) && Name.Equals(ExpectedLabel, ESearchCase::IgnoreCase);
	}

	static bool TraceResultContainsActorLabel(const TSharedPtr<FJsonObject>& TraceData, const FString& ExpectedLabel)
	{
		if (!TraceData.IsValid())
		{
			return false;
		}

		FString ActorId;
		if (TraceData->TryGetStringField(TEXT("actor_id"), ActorId) && ActorId.Equals(ExpectedLabel, ESearchCase::IgnoreCase))
		{
			return true;
		}

		const TSharedPtr<FJsonObject>* ActorObject = nullptr;
		if (TraceData->TryGetObjectField(TEXT("actor"), ActorObject) && ActorObject && ActorSummaryMatchesLabel(*ActorObject, ExpectedLabel))
		{
			return true;
		}

		const TArray<TSharedPtr<FJsonValue>>* Hits = nullptr;
		if (!TraceData->TryGetArrayField(TEXT("hits"), Hits) || !Hits)
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& HitValue : *Hits)
		{
			const TSharedPtr<FJsonObject> HitObject = HitValue.IsValid() ? HitValue->AsObject() : nullptr;
			if (!HitObject.IsValid())
			{
				continue;
			}

			FString HitActorId;
			if (HitObject->TryGetStringField(TEXT("actor_id"), HitActorId) && HitActorId.Equals(ExpectedLabel, ESearchCase::IgnoreCase))
			{
				return true;
			}

			const TSharedPtr<FJsonObject>* HitActorObject = nullptr;
			if (HitObject->TryGetObjectField(TEXT("actor"), HitActorObject) && HitActorObject && ActorSummaryMatchesLabel(*HitActorObject, ExpectedLabel))
			{
				return true;
			}
		}

		return false;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentInterfaceLevelContentJsonCrudSmokeTest,
	"GptProjectTest.UeAgentInterface.Smoke.LevelContentJson.CrudTasksValidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUeAgentInterfaceLevelContentJsonCrudSmokeTest::RunTest(const FString& Parameters)
{
	FScopedUeAgentHttpServer ServerScope;
	FString InitError;
	if (!ServerScope.Initialize(InitError))
	{
		AddError(FString::Printf(TEXT("Initialize failed: %s"), *InitError));
		return false;
	}

	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString FolderRoot = FString::Printf(TEXT("UAI/LevelContentJson/%s"), *Suffix);
	const FString CrateId = FString::Printf(TEXT("lcj_crate_%s"), *Suffix);
	const FString CrateLabel = FString::Printf(TEXT("LCJ_Crate_%s"), *Suffix);
	const FString LightId = FString::Printf(TEXT("lcj_light_%s"), *Suffix);
	const FString CameraId = FString::Printf(TEXT("lcj_camera_%s"), *Suffix);
	const FString AudioId = FString::Printf(TEXT("lcj_audio_%s"), *Suffix);
	const FString TriggerId = FString::Printf(TEXT("lcj_trigger_%s"), *Suffix);
	const FString NavId = FString::Printf(TEXT("lcj_nav_%s"), *Suffix);
	const FString ProbeId = FString::Printf(TEXT("lcj_probe_%s"), *Suffix);
	const FString TextId = FString::Printf(TEXT("lcj_text_%s"), *Suffix);
	const FString CustomId = FString::Printf(TEXT("lcj_custom_%s"), *Suffix);

	const FString ContentJson = FString::Printf(
		TEXT("{")
		TEXT("\"schema\":\"uai.level_content.v1\",")
		TEXT("\"scope\":{\"folder_root\":\"%s\",\"managed_tag\":\"UAI.Managed\",\"streaming\":{\"required_loaded_levels\":[]},\"world_partition\":{\"enabled\":\"auto\",\"allow_unloaded_actor_changes\":false}},")
		TEXT("\"defaults\":{\"folder_path\":\"%s\",\"tags\":[\"UAI.Managed\",\"LCJ.Smoke\"]},")
		TEXT("\"resources\":[")
		TEXT("{\"id\":\"%s\",\"kind\":\"static_mesh\",\"label\":\"LCJ_Crate_%s\",\"tags\":[\"DataLayer:SmokeDataLayer\"],\"asset\":{\"static_mesh\":\"/Engine/BasicShapes/Cube.Cube\"},\"transform\":{\"location\":[61000,61000,5000],\"rotation\":[0,0,0],\"scale\":[1,1,1]},\"properties\":{\"bHidden\":false,\"StaticMeshComponent.BodyInstance.CollisionProfileName\":\"BlockAll\"},\"components\":[{\"id\":\"ExtraScene\",\"name\":\"ExtraScene\",\"class\":\"/Script/Engine.SceneComponent\",\"transform\":{\"relative_location\":[0,0,50]}},{\"id\":\"FenceSpline\",\"name\":\"FenceSpline\",\"class\":\"/Script/Engine.SplineComponent\",\"spline_closed_loop\":false,\"spline_points\":[{\"location\":[0,0,0],\"type\":\"Linear\"},{\"location\":[250,0,0],\"type\":\"Linear\"},{\"location\":[350,120,0],\"type\":\"Linear\"}]}]},")
		TEXT("{\"id\":\"%s\",\"kind\":\"point_light\",\"label\":\"LCJ_Light_%s\",\"transform\":{\"location\":[61200,61000,250],\"scale\":[1,1,1]},\"properties\":{\"PointLightComponent.Intensity\":1250.0}},")
		TEXT("{\"id\":\"%s\",\"kind\":\"camera\",\"label\":\"LCJ_Camera_%s\",\"transform\":{\"location\":[61400,61000,240],\"rotation\":[0,180,0],\"scale\":[1,1,1]},\"properties\":{\"CameraComponent.FieldOfView\":62.0}},")
		TEXT("{\"id\":\"%s\",\"kind\":\"ambient_sound\",\"label\":\"LCJ_Audio_%s\",\"transform\":{\"location\":[61600,61000,120],\"scale\":[1,1,1]}},")
		TEXT("{\"id\":\"%s\",\"kind\":\"trigger_box\",\"label\":\"LCJ_Trigger_%s\",\"transform\":{\"location\":[61800,61000,120],\"scale\":[1,1,1]}},")
		TEXT("{\"id\":\"%s\",\"kind\":\"nav_mesh_bounds_volume\",\"label\":\"LCJ_Nav_%s\",\"transform\":{\"location\":[62000,61000,120],\"scale\":[1,1,1]},\"bounds\":{\"center\":[62000,61000,120],\"extent\":[300,300,200]}},")
		TEXT("{\"id\":\"%s\",\"kind\":\"probe\",\"label\":\"LCJ_Probe_%s\",\"transform\":{\"location\":[62200,61000,120],\"scale\":[1,1,1]}},")
		TEXT("{\"id\":\"%s\",\"kind\":\"custom\",\"class_path\":\"/Script/Engine.TextRenderActor\",\"label\":\"LCJ_Text_%s\",\"transform\":{\"location\":[62300,61000,180],\"rotation\":[0,180,0],\"scale\":[1,1,1]},\"properties\":{\"TextRenderComponent.Text\":\"LCJ Hello\",\"TextRenderComponent.WorldSize\":64.0}},")
		TEXT("{\"id\":\"%s\",\"kind\":\"custom\",\"class_path\":\"/Script/Engine.Actor\",\"label\":\"LCJ_Custom_%s\",\"transform\":{\"location\":[62400,61000,120],\"scale\":[1,1,1]},\"components\":[{\"name\":\"CustomRoot\",\"class\":\"/Script/Engine.SceneComponent\"}]}")
		TEXT("],")
		TEXT("\"organization\":{\"hlod\":{\"assignments\":[{\"resource\":\"%s\",\"layer\":\"SmokeHLOD\"}]},\"data_layers\":[{\"name\":\"SmokeDataLayer\",\"resources\":[\"%s\"]}]},")
		TEXT("\"tasks\":[{\"type\":\"navmesh_build\",\"timeout_seconds\":30},{\"type\":\"hlod_build\",\"required\":false}],")
		TEXT("\"validation\":[{\"type\":\"actor_exists\",\"actor_id\":\"%s\"},{\"type\":\"property_readback\",\"actor_id\":\"%s\",\"property\":\"bHidden\"},{\"type\":\"property_readback\",\"actor_id\":\"%s\",\"property\":\"StaticMeshComponent.BodyInstance.CollisionProfileName\"},{\"type\":\"property_readback\",\"actor_id\":\"%s\",\"property\":\"PointLightComponent.Intensity\"},{\"type\":\"property_readback\",\"actor_id\":\"%s\",\"property\":\"CameraComponent.FieldOfView\"},{\"type\":\"property_readback\",\"actor_id\":\"%s\",\"property\":\"TextRenderComponent.WorldSize\"},{\"type\":\"trace\",\"start\":[61000,61000,5400],\"end\":[61000,61000,5300],\"expected_hit\":false},{\"type\":\"overlap\",\"center\":[61000,61000,5000],\"extent\":[80,80,80],\"min_count\":1},{\"type\":\"nav_path\",\"start\":[62000,61000,120],\"end\":[62010,61010,120]},{\"type\":\"streaming_state\",\"level\":\"__UAI_MissingStreamingLevel__\"},{\"type\":\"feature_gate\",\"feature\":\"hlod\",\"required\":false}],")
		TEXT("\"evidence\":{\"report\":true},")
		TEXT("\"save_policy\":{\"save_level\":false}")
		TEXT("}"),
		*FolderRoot,
		*FolderRoot,
		*CrateId, *Suffix,
		*LightId, *Suffix,
		*CameraId, *Suffix,
		*AudioId, *Suffix,
		*TriggerId, *Suffix,
		*NavId, *Suffix,
		*ProbeId, *Suffix,
		*TextId, *Suffix,
		*CustomId, *Suffix,
		*CrateId, *CrateId,
		*CrateId, *CrateId, *CrateId, *LightId, *CameraId, *TextId);

	TSharedPtr<FJsonObject> ValidateData;
	TestTrue(TEXT("level_content_validate_json succeeds"), ExecLevelContentSmokeCommand(this, ServerScope, TEXT("lcj-validate"), TEXT("level_content_validate_json"), FString::Printf(TEXT("{\"content\":%s,\"strict\":true}"), *ContentJson), ValidateData));
	TestTrue(TEXT("validation valid despite gated optional HLOD/DataLayer warnings"), ValidateData.IsValid() && ValidateData->GetBoolField(TEXT("valid")));
	const TArray<TSharedPtr<FJsonValue>>* ValidateIssues = nullptr;
	TestTrue(TEXT("validation exposes feature gate warnings"), ValidateData->TryGetArrayField(TEXT("json_issues"), ValidateIssues) && JsonArrayContainsCode(ValidateIssues, TEXT("feature_unavailable_hlod_assignment_report_only")));

	TSharedPtr<FJsonObject> PlanData;
	TestTrue(TEXT("level_content_plan_json succeeds"), ExecLevelContentSmokeCommand(this, ServerScope, TEXT("lcj-plan"), TEXT("level_content_plan_json"), FString::Printf(TEXT("{\"content\":%s,\"mode\":\"upsert\"}"), *ContentJson), PlanData));
	TestTrue(TEXT("plan has steps"), PlanData.IsValid() && PlanData->GetIntegerField(TEXT("step_count")) >= 7);

	TSharedPtr<FJsonObject> ApplyData;
	TestTrue(TEXT("level_content_apply_json succeeds"), ExecLevelContentSmokeCommand(this, ServerScope, TEXT("lcj-apply"), TEXT("level_content_apply_json"), FString::Printf(TEXT("{\"content\":%s,\"mode\":\"upsert\",\"transaction\":true,\"preflight\":true,\"validate_after_apply\":true}"), *ContentJson), ApplyData, true, 60.0));
	TestEqual(TEXT("apply schema"), ApplyData->GetStringField(TEXT("schema")), FString(TEXT("uai.level_content.apply_result.v1")));
	TestEqual(TEXT("apply status"), ApplyData->GetStringField(TEXT("status")), FString(TEXT("applied")));
	TestTrue(TEXT("apply success field"), ApplyData->GetBoolField(TEXT("success")));
	TestFalse(TEXT("apply reports non-dry run"), ApplyData->GetBoolField(TEXT("dry_run")));
	TestTrue(TEXT("apply has resource_results alias"), ApplyData->HasTypedField<EJson::Array>(TEXT("resource_results")));
	TestTrue(TEXT("apply has validation results"), ApplyData->HasTypedField<EJson::Array>(TEXT("validation_results")));
	TestTrue(TEXT("apply has evidence results"), ApplyData->HasTypedField<EJson::Object>(TEXT("evidence_results")));
	const TArray<TSharedPtr<FJsonValue>>* ApplyResourceResults = nullptr;
	TestTrue(TEXT("apply exposes resource results"), ApplyData->TryGetArrayField(TEXT("resource_results"), ApplyResourceResults));
	TestTrue(TEXT("component collision profile setter is applied"), ResourcePropertyResultHasBoolFlag(ApplyResourceResults, CrateId, TEXT("StaticMeshComponent.BodyInstance.CollisionProfileName"), TEXT("collision_profile_setter_applied")));
	TestTrue(TEXT("component collision state refresh is applied"), ResourcePropertyResultHasBoolFlag(ApplyResourceResults, CrateId, TEXT("StaticMeshComponent.BodyInstance.CollisionProfileName"), TEXT("primitive_collision_state_refreshed")));
	FString LightIntensityReadBack;
	TestTrue(TEXT("normal property readback is reported"), TryGetResourcePropertyResultStringField(ApplyResourceResults, LightId, TEXT("PointLightComponent.Intensity"), TEXT("property_value_read_back"), LightIntensityReadBack));
	int32 FirstDot = INDEX_NONE;
	int32 LastDot = INDEX_NONE;
	const bool bHasDot = LightIntensityReadBack.FindChar(TEXT('.'), FirstDot);
	LightIntensityReadBack.FindLastChar(TEXT('.'), LastDot);
	TestTrue(TEXT("normal property readback is not concatenated"), LightIntensityReadBack.StartsWith(TEXT("1250")) && (!bHasDot || FirstDot == LastDot));
	FString TextWorldSizeReadBack;
	TestTrue(TEXT("TextRenderActor component property readback is reported"), TryGetResourcePropertyResultStringField(ApplyResourceResults, TextId, TEXT("TextRenderComponent.WorldSize"), TEXT("property_value_read_back"), TextWorldSizeReadBack));
	TestTrue(TEXT("TextRenderActor WorldSize is applied"), TextWorldSizeReadBack.StartsWith(TEXT("64")));
	const TSharedPtr<FJsonObject> NavApplyResult = FindJsonObjectByStringField(ApplyResourceResults, TEXT("id"), NavId);
	bool bNavBoundsUpdated = false;
	TestTrue(TEXT("nav mesh bounds volume creates brush bounds and updates navigation"), NavApplyResult.IsValid() && NavApplyResult->TryGetBoolField(TEXT("nav_bounds_updated"), bNavBoundsUpdated) && bNavBoundsUpdated && BoundsExtentExceeds(NavApplyResult, 250.0, 250.0, 150.0));
	const TArray<TSharedPtr<FJsonValue>>* ApplyValidationResults = nullptr;
	TestTrue(TEXT("feature_gate validation executed"), ApplyData->TryGetArrayField(TEXT("validation_results"), ApplyValidationResults) && JsonArrayContainsStringField(ApplyValidationResults, TEXT("validation"), TEXT("feature_gate")));

	const FString QueryOutputFile = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), FString::Printf(TEXT("Automation/UAI/level_content_query_%s.json"), *Suffix)));
	TSharedPtr<FJsonObject> QueryData;
	TestTrue(TEXT("level_content_query_json succeeds"), ExecLevelContentSmokeCommand(this, ServerScope, TEXT("lcj-query"), TEXT("level_content_query_json"), FString::Printf(TEXT("{\"selection\":{\"mode\":\"folder\",\"folder_path\":\"%s\",\"include_child_folders\":true},\"projection\":{\"include_components\":true,\"include_bounds\":true},\"output_file\":%s}"), *FolderRoot, *SmokeJsonString(QueryOutputFile)), QueryData));
	TestTrue(TEXT("query returns all created resources"), QueryData.IsValid() && QueryData->GetIntegerField(TEXT("resource_count")) >= 9);
	TestEqual(TEXT("query output file uses structured compact format"), QueryData->GetStringField(TEXT("output_format")), FString(TEXT("structured_compact")));
	FString QueryOutputText;
	TSharedPtr<FJsonObject> QueryOutputJson;
	TestTrue(TEXT("query output file reads"), FFileHelper::LoadFileToString(QueryOutputText, *QueryOutputFile));
	TestTrue(TEXT("query output file parses"), ParseJson(QueryOutputText, QueryOutputJson) && QueryOutputJson.IsValid());
	TestTrue(TEXT("query output keeps short location object inline"), QueryOutputText.Contains(TEXT("\"location\": {\"x\"")));
	TestFalse(TEXT("query output does not pretty-expand location object"), QueryOutputText.Contains(TEXT("\"location\":\n")));
	const TArray<TSharedPtr<FJsonValue>>* QueryResources = nullptr;
	TestTrue(TEXT("query exposes resources"), QueryData->TryGetArrayField(TEXT("resources"), QueryResources));
	TestTrue(TEXT("query reads back non-zero nav mesh bounds"), BoundsExtentExceeds(FindJsonObjectByStringField(QueryResources, TEXT("id"), NavId), 250.0, 250.0, 150.0));
	TestTrue(TEXT("query reads back spline component points"), ResourceHasComponentWithSplinePointCount(QueryResources, CrateId, TEXT("FenceSpline"), 3));
	TestTrue(TEXT("rootless custom actor keeps transform after root component creation"), ResourceLocationMatches(QueryResources, CustomId, 62400.0, 61000.0, 120.0));
	TestTrue(TEXT("query actor transform has space contract"), ResourceTransformHasSpaceContract(QueryResources, CrateId, TEXT("ue_actor_world_transform")));

	TSharedPtr<FJsonObject> ExactLabelQueryData;
	TestTrue(TEXT("level_content_query_json exact label filter succeeds"), ExecLevelContentSmokeCommand(this, ServerScope, TEXT("lcj-exact-label-query"), TEXT("level_content_query_json"), FString::Printf(TEXT("{\"selection\":{\"label\":\"LCJ_Crate_%s\"},\"projection\":{\"include_bounds\":true}}"), *Suffix), ExactLabelQueryData));
	TestEqual(TEXT("exact label query returns one resource"), ExactLabelQueryData->GetIntegerField(TEXT("resource_count")), 1);

	TSharedPtr<FJsonObject> FilteredQueryData;
	TestTrue(TEXT("level_content_query_json supports label/bounds/data layer filters"), ExecLevelContentSmokeCommand(this, ServerScope, TEXT("lcj-filtered-query"), TEXT("level_content_query_json"), FString::Printf(TEXT("{\"selection\":{\"mode\":\"folder\",\"folder_path\":\"%s\",\"include_child_folders\":true,\"label_contains\":\"LCJ_Crate\",\"data_layers\":[\"SmokeDataLayer\"],\"bounds\":{\"center\":[61000,61000,5000],\"extent\":[200,200,200]}},\"projection\":{\"include_bounds\":true}}"), *FolderRoot), FilteredQueryData));
	TestTrue(TEXT("filtered query returns crate"), FilteredQueryData.IsValid() && FilteredQueryData->GetIntegerField(TEXT("resource_count")) >= 1);

	TSharedPtr<FJsonObject> SnapshotData;
	TestTrue(TEXT("level_content_snapshot_json succeeds"), ExecLevelContentSmokeCommand(this, ServerScope, TEXT("lcj-snapshot"), TEXT("level_content_snapshot_json"), FString::Printf(TEXT("{\"selection\":{\"mode\":\"folder\",\"folder_path\":\"%s\",\"include_child_folders\":true}}"), *FolderRoot), SnapshotData));
	TestEqual(TEXT("snapshot schema"), SnapshotData->GetStringField(TEXT("schema")), FString(TEXT("uai.level_content.snapshot.v1")));

	TSharedPtr<FJsonObject> DiffData;
	TestTrue(TEXT("level_content_diff_json succeeds"), ExecLevelContentSmokeCommand(this, ServerScope, TEXT("lcj-diff"), TEXT("level_content_diff_json"), FString::Printf(TEXT("{\"content\":%s,\"delete_policy\":\"none\"}"), *ContentJson), DiffData));
	TestTrue(TEXT("diff is empty after apply"), DiffData.IsValid() && DiffData->GetBoolField(TEXT("empty")));
	TestTrue(TEXT("diff exposes diffs array"), DiffData->HasTypedField<EJson::Array>(TEXT("diffs")));
	TestEqual(TEXT("diff target count"), DiffData->GetIntegerField(TEXT("target_count")), 9);
	TestEqual(TEXT("diff current count"), DiffData->GetIntegerField(TEXT("current_count")), 9);

	TSharedPtr<FJsonObject> ActorAliasData;
	TestTrue(TEXT("level_actor_export_json succeeds"), ExecLevelContentSmokeCommand(this, ServerScope, TEXT("lcj-actor-export"), TEXT("level_actor_export_json"), FString::Printf(TEXT("{\"selection\":{\"mode\":\"folder\",\"folder_path\":\"%s\",\"include_child_folders\":true}}"), *FolderRoot), ActorAliasData));
	TestTrue(TEXT("actor alias marks actor_only"), ActorAliasData.IsValid() && ActorAliasData->GetBoolField(TEXT("actor_only")));

	TSharedPtr<FJsonObject> ActorAliasApplyData;
	TestTrue(TEXT("level_actor_apply_json succeeds"), ExecLevelContentSmokeCommand(this, ServerScope, TEXT("lcj-actor-apply"), TEXT("level_actor_apply_json"), FString::Printf(TEXT("{\"content\":%s,\"mode\":\"upsert\",\"preflight\":true,\"validate_after_apply\":true}"), *ContentJson), ActorAliasApplyData, true, 60.0));
	TestEqual(TEXT("actor alias apply schema"), ActorAliasApplyData->GetStringField(TEXT("schema")), FString(TEXT("uai.level_content.apply_result.v1")));
	TestEqual(TEXT("actor alias apply status"), ActorAliasApplyData->GetStringField(TEXT("status")), FString(TEXT("applied")));

	TSharedPtr<FJsonObject> ActorAliasSnapshotData;
	TestTrue(TEXT("level_actor_snapshot_json succeeds"), ExecLevelContentSmokeCommand(this, ServerScope, TEXT("lcj-actor-snapshot"), TEXT("level_actor_snapshot_json"), FString::Printf(TEXT("{\"selection\":{\"mode\":\"folder\",\"folder_path\":\"%s\",\"include_child_folders\":true}}"), *FolderRoot), ActorAliasSnapshotData));
	TestTrue(TEXT("actor alias snapshot marks actor_only"), ActorAliasSnapshotData.IsValid() && ActorAliasSnapshotData->GetBoolField(TEXT("actor_only")));
	TestEqual(TEXT("actor alias snapshot schema"), ActorAliasSnapshotData->GetStringField(TEXT("schema")), FString(TEXT("uai.level_content.snapshot.v1")));

	TSharedPtr<FJsonObject> ActorAliasDiffData;
	TestTrue(TEXT("level_actor_diff_json succeeds"), ExecLevelContentSmokeCommand(this, ServerScope, TEXT("lcj-actor-diff"), TEXT("level_actor_diff_json"), FString::Printf(TEXT("{\"content\":%s,\"delete_policy\":\"none\"}"), *ContentJson), ActorAliasDiffData));
	TestTrue(TEXT("actor alias diff is empty after alias apply"), ActorAliasDiffData.IsValid() && ActorAliasDiffData->GetBoolField(TEXT("empty")));

	TSharedPtr<FJsonObject> BlockAllTraceData;
	TestTrue(TEXT("level_trace_world_ray before NoCollision patch succeeds"), ExecLevelContentSmokeCommand(this, ServerScope, TEXT("lcj-collision-trace-before"), TEXT("level_trace_world_ray"), TEXT("{\"start\":[61000,61000,4800],\"direction\":[0,0,1],\"trace_distance\":400,\"trace_channel\":\"Visibility\",\"trace_complex\":false,\"include_all_hits\":true,\"max_hits\":16,\"include_actor_folder_tags\":true}"), BlockAllTraceData));
	TestTrue(TEXT("BlockAll baseline trace hits the managed crate"), TraceResultContainsActorLabel(BlockAllTraceData, CrateLabel));

	const FString NoCollisionPatchJson = FString::Printf(
		TEXT("{\"schema\":\"uai.level_content.v1\",\"scope\":{\"folder_root\":\"%s\",\"managed_tag\":\"UAI.Managed\"},\"defaults\":{\"folder_path\":\"%s\",\"tags\":[\"UAI.Managed\",\"LCJ.Smoke\"]},\"resources\":[{\"id\":\"%s\",\"kind\":\"static_mesh\",\"properties\":{\"StaticMeshComponent.BodyInstance.CollisionProfileName\":\"NoCollision\"}}],\"save_policy\":{\"save_level\":false}}"),
		*FolderRoot,
		*FolderRoot,
		*CrateId);
	TSharedPtr<FJsonObject> NoCollisionPatchData;
	TestTrue(TEXT("level_content_apply_json can patch collision profile"), ExecLevelContentSmokeCommand(this, ServerScope, TEXT("lcj-collision-patch"), TEXT("level_content_apply_json"), FString::Printf(TEXT("{\"content\":%s,\"mode\":\"patch\",\"transaction\":true,\"preflight\":true,\"validate_after_apply\":true}"), *NoCollisionPatchJson), NoCollisionPatchData, true, 60.0));
	const TArray<TSharedPtr<FJsonValue>>* PatchResourceResults = nullptr;
	TestTrue(TEXT("collision patch exposes resource results"), NoCollisionPatchData->TryGetArrayField(TEXT("resource_results"), PatchResourceResults));
	TestTrue(TEXT("collision patch uses profile setter"), ResourcePropertyResultHasBoolFlag(PatchResourceResults, CrateId, TEXT("StaticMeshComponent.BodyInstance.CollisionProfileName"), TEXT("collision_profile_setter_applied")));
	TestTrue(TEXT("collision patch refreshes primitive physics state"), ResourcePropertyResultHasBoolFlag(PatchResourceResults, CrateId, TEXT("StaticMeshComponent.BodyInstance.CollisionProfileName"), TEXT("primitive_collision_state_refreshed")));
	FString CollisionProfileReadBack;
	TestTrue(TEXT("collision profile readback is reported"), TryGetResourcePropertyResultStringField(PatchResourceResults, CrateId, TEXT("StaticMeshComponent.BodyInstance.CollisionProfileName"), TEXT("property_value_read_back"), CollisionProfileReadBack));
	TestEqual(TEXT("collision profile readback is not concatenated"), CollisionProfileReadBack, FString(TEXT("NoCollision")));

	TSharedPtr<FJsonObject> NoCollisionTraceData;
	TestTrue(TEXT("level_trace_world_ray after NoCollision patch succeeds"), ExecLevelContentSmokeCommand(this, ServerScope, TEXT("lcj-collision-trace"), TEXT("level_trace_world_ray"), TEXT("{\"start\":[61000,61000,4800],\"direction\":[0,0,1],\"trace_distance\":400,\"trace_channel\":\"Visibility\",\"trace_complex\":false,\"include_all_hits\":true,\"max_hits\":16,\"include_actor_folder_tags\":true}"), NoCollisionTraceData));
	const bool bStillHitsCrateAfterNoCollision = TraceResultContainsActorLabel(NoCollisionTraceData, CrateLabel);
	if (bStillHitsCrateAfterNoCollision)
	{
		AddError(FString::Printf(TEXT("NoCollision trace still hit crate. Trace=%s"), *JsonObjectToCompactString(NoCollisionTraceData)));
	}
	TestFalse(TEXT("NoCollision patch removes the managed crate from physics trace immediately"), bStillHitsCrateAfterNoCollision);

	TSharedPtr<FJsonObject> DeleteDryRunData;
	TestTrue(TEXT("level_content_delete_scope dry run succeeds"), ExecLevelContentSmokeCommand(this, ServerScope, TEXT("lcj-delete-dry"), TEXT("level_content_delete_scope"), FString::Printf(TEXT("{\"folder_root\":\"%s\",\"managed_tag\":\"UAI.Managed\",\"dry_run\":true}"), *FolderRoot), DeleteDryRunData));
	TestTrue(TEXT("delete dry run sees resources"), DeleteDryRunData.IsValid() && DeleteDryRunData->GetIntegerField(TEXT("matched_count")) >= 8);
	TestEqual(TEXT("delete dry run deleted count"), DeleteDryRunData->GetIntegerField(TEXT("deleted_count")), 0);
	TestTrue(TEXT("delete dry run exposes actors"), DeleteDryRunData->HasTypedField<EJson::Array>(TEXT("actors")));
	TestTrue(TEXT("delete dry run exposes blocked"), DeleteDryRunData->HasTypedField<EJson::Array>(TEXT("blocked")));

	TSharedPtr<FJsonObject> DeleteData;
	TestTrue(TEXT("level_content_delete_scope succeeds"), ExecLevelContentSmokeCommand(this, ServerScope, TEXT("lcj-delete"), TEXT("level_content_delete_scope"), FString::Printf(TEXT("{\"folder_root\":\"%s\",\"managed_tag\":\"UAI.Managed\",\"dry_run\":false}"), *FolderRoot), DeleteData));
	TestTrue(TEXT("delete removed resources"), DeleteData.IsValid() && DeleteData->GetIntegerField(TEXT("matched_count")) >= 8);
	TestTrue(TEXT("delete reports deleted count"), DeleteData->GetIntegerField(TEXT("deleted_count")) >= 8);

	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentInterfaceLevelContentJsonGuardSmokeTest,
	"GptProjectTest.UeAgentInterface.Smoke.LevelContentJson.GuardsMergeRepair",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUeAgentInterfaceLevelContentJsonGuardSmokeTest::RunTest(const FString& Parameters)
{
	FScopedUeAgentHttpServer ServerScope;
	FString InitError;
	if (!ServerScope.Initialize(InitError))
	{
		AddError(FString::Printf(TEXT("Initialize failed: %s"), *InitError));
		return false;
	}

	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString FolderRoot = FString::Printf(TEXT("UAI/LevelContentJsonGuards/%s"), *Suffix);
	const FString ProtectedId = FString::Printf(TEXT("lcj_protected_%s"), *Suffix);
	const FString CloneId = FString::Printf(TEXT("lcj_clone_%s"), *Suffix);

	const FString DuplicateContent = TEXT("{\"schema\":\"uai.level_content.v1\",\"resources\":[{\"id\":\"dup\",\"kind\":\"static_mesh\"},{\"id\":\"dup\",\"kind\":\"static_mesh\"}]}");
	TSharedPtr<FJsonObject> DuplicateData;
	TestTrue(TEXT("duplicate validate command completes"), ExecLevelContentSmokeCommand(this, ServerScope, TEXT("lcj-dup"), TEXT("level_content_validate_json"), FString::Printf(TEXT("{\"content\":%s}"), *DuplicateContent), DuplicateData));
	TestFalse(TEXT("duplicate ids invalid"), DuplicateData->GetBoolField(TEXT("valid")));
	const TArray<TSharedPtr<FJsonValue>>* DuplicateIssues = nullptr;
	TestTrue(TEXT("duplicate_resource_id issue emitted"), DuplicateData->TryGetArrayField(TEXT("json_issues"), DuplicateIssues) && JsonArrayContainsCode(DuplicateIssues, TEXT("duplicate_resource_id")));

	const FString ActorAliasBadContent = TEXT("{\"schema\":\"uai.level_content.v1\",\"resources\":[{\"id\":\"stream_ref\",\"category\":\"level_container\",\"kind\":\"streaming_level\",\"operation\":\"upsert\"}]}");
	TSharedPtr<FJsonObject> ActorAliasBadData;
	TestTrue(TEXT("actor alias rejects non actor resource structurally"), ExecLevelContentSmokeCommand(this, ServerScope, TEXT("lcj-alias-bad"), TEXT("level_actor_validate_json"), FString::Printf(TEXT("{\"content\":%s}"), *ActorAliasBadContent), ActorAliasBadData));
	TestFalse(TEXT("actor alias bad content invalid"), ActorAliasBadData->GetBoolField(TEXT("valid")));
	const TArray<TSharedPtr<FJsonValue>>* ActorAliasIssues = nullptr;
	TestTrue(TEXT("actor_alias_non_actor_resource issue emitted"), ActorAliasBadData->TryGetArrayField(TEXT("json_issues"), ActorAliasIssues) && JsonArrayContainsCode(ActorAliasIssues, TEXT("actor_alias_non_actor_resource")));

	const FString BoundaryContent = TEXT("{\"schema\":\"uai.level_content.v1\",\"resources\":[{\"id\":\"bad_class\",\"kind\":\"custom\",\"class_path\":\"/Script/Engine.DoesNotExist\"},{\"id\":\"bad_lod\",\"kind\":\"static_mesh\",\"lods\":[{\"screen_size\":0.5}]},{\"id\":\"strict_adapter\",\"kind\":\"custom\",\"class_path\":\"/Script/Engine.TargetPoint\",\"adapter\":\"MissingAdapter\",\"strict_adapter\":true},{\"id\":\"custom_fallback\",\"kind\":\"custom\",\"class_path\":\"/Script/Engine.TargetPoint\"}]}");
	TSharedPtr<FJsonObject> BoundaryData;
	TestTrue(TEXT("boundary validate command completes"), ExecLevelContentSmokeCommand(this, ServerScope, TEXT("lcj-boundary"), TEXT("level_content_validate_json"), FString::Printf(TEXT("{\"content\":%s,\"strict\":true}"), *BoundaryContent), BoundaryData));
	TestFalse(TEXT("boundary content invalid"), BoundaryData->GetBoolField(TEXT("valid")));
	const TArray<TSharedPtr<FJsonValue>>* BoundaryIssues = nullptr;
	TestTrue(TEXT("boundary issues emitted"), BoundaryData->TryGetArrayField(TEXT("json_issues"), BoundaryIssues));
	TestTrue(TEXT("missing class issue emitted"), JsonArrayContainsCode(BoundaryIssues, TEXT("actor_class_not_found")));
	TestTrue(TEXT("asset lod boundary issue emitted"), JsonArrayContainsCode(BoundaryIssues, TEXT("asset_lod_out_of_level_scope")));
	TestTrue(TEXT("strict custom adapter issue emitted"), JsonArrayContainsCode(BoundaryIssues, TEXT("custom_adapter_not_registered")));
	TestTrue(TEXT("custom reflection fallback warning emitted"), JsonArrayContainsCode(BoundaryIssues, TEXT("custom_actor_reflection_fallback")));

	const FString ProtectedCreate = FString::Printf(
		TEXT("{\"schema\":\"uai.level_content.v1\",\"scope\":{\"folder_root\":\"%s\",\"managed_tag\":\"UAI.Managed\"},\"defaults\":{\"folder_path\":\"%s\",\"tags\":[\"UAI.Managed\"]},\"resources\":[{\"id\":\"%s\",\"kind\":\"static_mesh\",\"label\":\"LCJ_Protected_%s\",\"tags\":[\"NoDelete\"],\"static_mesh\":\"/Engine/BasicShapes/Cube.Cube\",\"transform\":{\"location\":[63000,63000,100],\"scale\":[1,1,1]}}]}"),
		*FolderRoot,
		*FolderRoot,
		*ProtectedId,
		*Suffix);
	TSharedPtr<FJsonObject> ProtectedCreateData;
	TestTrue(TEXT("protected actor create succeeds"), ExecLevelContentSmokeCommand(this, ServerScope, TEXT("lcj-protect-create"), TEXT("level_content_apply_json"), FString::Printf(TEXT("{\"content\":%s,\"mode\":\"upsert\"}"), *ProtectedCreate), ProtectedCreateData));

	const FString MaintenanceOps = FString::Printf(
		TEXT("{\"schema\":\"uai.level_content.v1\",\"scope\":{\"folder_root\":\"%s\",\"managed_tag\":\"UAI.Managed\"},\"operations\":[")
		TEXT("{\"type\":\"clone\",\"from_actor\":\"%s\",\"id\":\"%s\",\"label\":\"LCJ_Clone_%s\",\"folder_path\":\"%s/Ops\",\"offset\":[100,0,0],\"tags\":[\"OpsTag\"]},")
		TEXT("{\"type\":\"align\",\"source_actor\":\"%s\",\"target_actor\":\"%s\",\"method\":\"bounds_center\",\"offset\":[120,0,0]},")
		TEXT("{\"type\":\"attach\",\"child\":\"%s\",\"parent\":\"%s\"},")
		TEXT("{\"type\":\"detach\",\"actor\":\"%s\"},")
		TEXT("{\"type\":\"set_folder\",\"actor\":\"%s\",\"folder_path\":\"%s/Ops/Sub\"},")
		TEXT("{\"type\":\"add_tag\",\"actor\":\"%s\",\"tag\":\"OpsTag2\"},")
		TEXT("{\"type\":\"remove_tag\",\"actor\":\"%s\",\"tag\":\"OpsTag2\"},")
		TEXT("{\"type\":\"convert\",\"from_kind\":\"static_mesh\",\"to_kind\":\"blueprint_actor\"},")
		TEXT("{\"type\":\"convert_report\",\"from_kind\":\"static_mesh\",\"to_kind\":\"level_instance\"},")
		TEXT("{\"type\":\"remap\",\"property\":\"bHidden\",\"from\":\"False\",\"to\":\"True\",\"max_resources_touched\":10},")
		TEXT("{\"type\":\"remap_report\",\"property\":\"bHidden\",\"from\":\"False\",\"to\":\"True\"},")
		TEXT("{\"type\":\"repair\"},")
		TEXT("{\"type\":\"repair_report\"}")
		TEXT("]}"),
		*FolderRoot,
		*ProtectedId, *CloneId, *Suffix, *FolderRoot,
		*CloneId, *ProtectedId,
		*CloneId, *ProtectedId,
		*CloneId,
		*CloneId, *FolderRoot,
		*CloneId,
		*CloneId);
	TSharedPtr<FJsonObject> MaintenanceData;
	TestTrue(TEXT("convert/remap/repair operation apply succeeds"), ExecLevelContentSmokeCommand(this, ServerScope, TEXT("lcj-maintenance-ops"), TEXT("level_content_apply_json"), FString::Printf(TEXT("{\"content\":%s,\"mode\":\"upsert\",\"transaction\":true}"), *MaintenanceOps), MaintenanceData));
	const TArray<TSharedPtr<FJsonValue>>* OperationResults = nullptr;
	TestTrue(TEXT("maintenance operation results emitted"), MaintenanceData->TryGetArrayField(TEXT("operation_results"), OperationResults) && OperationResults && OperationResults->Num() == 13);
	bool bSawClone = false;
	bool bSawAlign = false;
	bool bSawAttach = false;
	bool bSawDetach = false;
	bool bSawFolder = false;
	bool bSawAddTag = false;
	bool bSawRemoveTag = false;
	bool bSawConvertReport = false;
	bool bSawConvertReportAlias = false;
	bool bSawRemapDryRun = false;
	bool bSawRemapReportAlias = false;
	bool bSawRepairDelegation = false;
	bool bSawRepairReportAlias = false;
	if (OperationResults)
	{
		for (const TSharedPtr<FJsonValue>& Value : *OperationResults)
		{
			const TSharedPtr<FJsonObject> Result = Value.IsValid() ? Value->AsObject() : nullptr;
			if (!Result.IsValid())
			{
				continue;
			}
			const FString Operation = Result->GetStringField(TEXT("operation"));
			const FString Status = Result->GetStringField(TEXT("status"));
			bSawClone = bSawClone || (Operation == TEXT("clone") && Status == TEXT("cloned"));
			bSawAlign = bSawAlign || (Operation == TEXT("align") && Status == TEXT("aligned"));
			bSawAttach = bSawAttach || (Operation == TEXT("attach") && Status == TEXT("attached"));
			bSawDetach = bSawDetach || (Operation == TEXT("detach") && Status == TEXT("detached"));
			bSawFolder = bSawFolder || (Operation == TEXT("set_folder") && Status == TEXT("folder_set"));
			bSawAddTag = bSawAddTag || (Operation == TEXT("add_tag") && Status == TEXT("tag_added"));
			bSawRemoveTag = bSawRemoveTag || (Operation == TEXT("remove_tag") && Status == TEXT("tag_removed"));
			bSawConvertReport = bSawConvertReport || (Operation == TEXT("convert") && Status == TEXT("report_only"));
			bSawConvertReportAlias = bSawConvertReportAlias || (Operation == TEXT("convert_report") && Status == TEXT("report_only"));
			bSawRemapDryRun = bSawRemapDryRun || (Operation == TEXT("remap") && Status == TEXT("applied"));
			bSawRemapReportAlias = bSawRemapReportAlias || (Operation == TEXT("remap_report") && Status == TEXT("report_only"));
			bSawRepairDelegation = bSawRepairDelegation || (Operation == TEXT("repair") && Status == TEXT("delegated_to_level_content_repair_json"));
			bSawRepairReportAlias = bSawRepairReportAlias || (Operation == TEXT("repair_report") && Status == TEXT("delegated_to_level_content_repair_json"));
		}
	}
	TestTrue(TEXT("clone operation status"), bSawClone);
	TestTrue(TEXT("align operation status"), bSawAlign);
	TestTrue(TEXT("attach operation status"), bSawAttach);
	TestTrue(TEXT("detach operation status"), bSawDetach);
	TestTrue(TEXT("set_folder operation status"), bSawFolder);
	TestTrue(TEXT("add_tag operation status"), bSawAddTag);
	TestTrue(TEXT("remove_tag operation status"), bSawRemoveTag);
	TestTrue(TEXT("convert operation report-only status"), bSawConvertReport);
	TestTrue(TEXT("convert_report operation alias status"), bSawConvertReportAlias);
	TestTrue(TEXT("remap operation applied status"), bSawRemapDryRun);
	TestTrue(TEXT("remap_report operation alias status"), bSawRemapReportAlias);
	TestTrue(TEXT("repair operation delegation status"), bSawRepairDelegation);
	TestTrue(TEXT("repair_report operation alias status"), bSawRepairReportAlias);

	const FString ProtectedDelete = FString::Printf(
		TEXT("{\"schema\":\"uai.level_content.v1\",\"scope\":{\"folder_root\":\"%s\",\"managed_tag\":\"UAI.Managed\"},\"operations\":[{\"type\":\"protect\",\"tags\":[\"NoDelete\"]}],\"resources\":[{\"id\":\"%s\",\"operation\":\"delete\",\"kind\":\"static_mesh\"}]}"),
		*FolderRoot,
		*ProtectedId);
	TSharedPtr<FJsonObject> ProtectedDeleteData;
	TestTrue(TEXT("protected delete returns controlled failure"), ExecLevelContentSmokeCommand(this, ServerScope, TEXT("lcj-protect-delete"), TEXT("level_content_apply_json"), FString::Printf(TEXT("{\"content\":%s,\"mode\":\"delete_only\",\"transaction\":true}"), *ProtectedDelete), ProtectedDeleteData, false));
	TestEqual(TEXT("protected delete error"), ProtectedDeleteData->GetStringField(TEXT("error")), FString(TEXT("actor_protected")));

	TSharedPtr<FJsonObject> ProtectedQueryData;
	TestTrue(TEXT("protected actor remains after failed delete"), ExecLevelContentSmokeCommand(this, ServerScope, TEXT("lcj-protect-query"), TEXT("level_content_query_json"), FString::Printf(TEXT("{\"selection\":{\"mode\":\"folder\",\"folder_path\":\"%s\",\"include_child_folders\":true,\"tags_all\":[\"NoDelete\"]}}"), *FolderRoot), ProtectedQueryData));
	TestTrue(TEXT("protected actor still exists"), ProtectedQueryData->GetIntegerField(TEXT("resource_count")) >= 1);

	const FString MergeJson = TEXT("{\"base\":{\"schema\":\"uai.level_content.v1\",\"resources\":[{\"id\":\"a\",\"kind\":\"static_mesh\",\"label\":\"A\"}]},\"current\":{\"schema\":\"uai.level_content.v1\",\"resources\":[{\"id\":\"a\",\"kind\":\"static_mesh\",\"label\":\"Current\"}]},\"target\":{\"schema\":\"uai.level_content.v1\",\"resources\":[{\"id\":\"a\",\"kind\":\"static_mesh\",\"label\":\"Target\"}]}}");
	TSharedPtr<FJsonObject> MergeData;
	TestTrue(TEXT("merge command succeeds"), ExecLevelContentSmokeCommand(this, ServerScope, TEXT("lcj-merge"), TEXT("level_content_merge_json"), MergeJson, MergeData));
	TestTrue(TEXT("merge reports conflict"), MergeData->GetIntegerField(TEXT("conflict_count")) == 1);

	TSharedPtr<FJsonObject> RepairData;
	TestTrue(TEXT("repair command succeeds"), ExecLevelContentSmokeCommand(this, ServerScope, TEXT("lcj-repair"), TEXT("level_content_repair_json"), FString::Printf(TEXT("{\"folder_root\":\"%s\",\"apply_safe\":false}"), *FolderRoot), RepairData));
	TestEqual(TEXT("repair schema"), RepairData->GetStringField(TEXT("schema")), FString(TEXT("uai.level_content.repair.v1")));

	const FString UnmanagedFolderRoot = FString::Printf(TEXT("UAI/LevelContentJsonUnmanagedDelete/%s"), *Suffix);
	const FString UnmanagedLabel = FString::Printf(TEXT("LCJ_Unmanaged_%s"), *Suffix);
	TSharedPtr<FJsonObject> UnmanagedSpawnData;
	TestTrue(TEXT("unmanaged actor spawn succeeds"), ExecLevelContentSmokeCommand(
		this,
		ServerScope,
		TEXT("lcj-unmanaged-spawn"),
		TEXT("spawn_actor"),
		FString::Printf(
			TEXT("{\"class_path\":\"/Script/Engine.TargetPoint\",\"label\":\"%s\",\"folder_path\":\"%s\",\"location\":{\"x\":64000,\"y\":64000,\"z\":100}}"),
			*UnmanagedLabel,
			*UnmanagedFolderRoot),
		UnmanagedSpawnData));

	TSharedPtr<FJsonObject> UnmanagedBlockedData;
	TestTrue(TEXT("delete scope blocks unmanaged actor when managed tag is required"), ExecLevelContentSmokeCommand(
		this,
		ServerScope,
		TEXT("lcj-unmanaged-delete-blocked"),
		TEXT("level_content_delete_scope"),
		FString::Printf(TEXT("{\"folder_path\":\"%s\",\"managed_tag\":\"UAI.Managed\",\"dry_run\":true}"), *UnmanagedFolderRoot),
		UnmanagedBlockedData,
		false));
	TestEqual(TEXT("unmanaged blocked delete reports blocked_count"), UnmanagedBlockedData->GetIntegerField(TEXT("blocked_count")), 1);
	TestEqual(TEXT("unmanaged blocked delete reports deletable_count"), UnmanagedBlockedData->GetIntegerField(TEXT("deletable_count")), 0);

	TSharedPtr<FJsonObject> UnmanagedDeleteData;
	TestTrue(TEXT("delete scope can explicitly delete unmanaged actor folder"), ExecLevelContentSmokeCommand(
		this,
		ServerScope,
		TEXT("lcj-unmanaged-delete-allow"),
		TEXT("level_content_delete_scope"),
		FString::Printf(TEXT("{\"folder_path\":\"%s\",\"dry_run\":false,\"allow_unmanaged_delete\":true,\"delete_empty_folders\":true}"), *UnmanagedFolderRoot),
		UnmanagedDeleteData));
	TestEqual(TEXT("unmanaged delete removes one actor"), UnmanagedDeleteData->GetIntegerField(TEXT("deleted_count")), 1);
	TestEqual(TEXT("unmanaged delete leaves no remaining actor"), UnmanagedDeleteData->GetIntegerField(TEXT("remaining_actor_count")), 0);
	TestTrue(TEXT("unmanaged delete reports folder cleanup"), UnmanagedDeleteData->GetIntegerField(TEXT("folder_delete_count")) >= 1);

	TSharedPtr<FJsonObject> CleanupData;
	ExecLevelContentSmokeCommand(this, ServerScope, TEXT("lcj-guard-cleanup"), TEXT("level_content_delete_scope"), FString::Printf(TEXT("{\"folder_root\":\"%s\",\"managed_tag\":\"UAI.Managed\",\"dry_run\":false,\"protected_tags\":[]}"), *FolderRoot), CleanupData, true, 30.0);

	return !HasAnyErrors();
}

#endif

#if WITH_DEV_AUTOMATION_TESTS

#include "Server/UeAgentHttpServer.h"
#include "UeAgentInterfaceSettings.h"

#include "Async/TaskGraphInterfaces.h"
#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "HAL/PlatformApplicationMisc.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HttpManager.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Misc/AutomationTest.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/CoreDelegates.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

DEFINE_LOG_CATEGORY_STATIC(LogUeAgentInterfaceSmoke, Log, All);

#include "UeAgentInterfaceSmokeTestCommon.inl"

namespace
{
	static FString JsonPath(const FString& FilePath)
	{
		return FilePath.Replace(TEXT("\\"), TEXT("/"));
	}

	static FString JsonString(const FString& Text)
	{
		FString Out;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
		TSharedPtr<FJsonValueString> Value = MakeShared<FJsonValueString>(Text);
		FJsonSerializer::Serialize(Value, TEXT(""), Writer);
		return Out;
	}

	static FString JsonArray(const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> JsonValues;
		for (const FString& Value : Values)
		{
			JsonValues.Add(MakeShared<FJsonValueString>(Value));
		}
		FString Out;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(JsonValues, Writer);
		return Out;
	}

	static bool ExecTextureCommand(
		FAutomationTestBase* Test,
		FScopedUeAgentHttpServer& ServerScope,
		const FString& RequestId,
		const FString& Command,
		const FString& ParamsJson,
		TSharedPtr<FJsonObject>& OutData,
		double TimeoutSeconds = 45.0)
	{
		FHttpSmokeResult Result;
		const FString Body = MakeExecRequestBody(RequestId, Command, ParamsJson);
		if (!Test->TestTrue(Command + TEXT(" request completed"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, Body, Result, TimeoutSeconds)))
		{
			return false;
		}
		if (!Test->TestEqual(Command + TEXT(" http status"), Result.StatusCode, 200))
		{
			Test->AddError(Result.Body.Left(1024));
			return false;
		}
		TSharedPtr<FJsonObject> Root;
		if (!Test->TestTrue(Command + TEXT(" response parses"), ParseJson(Result.Body, Root)) || !Root.IsValid())
		{
			return false;
		}
		const bool bOk = Root->GetBoolField(TEXT("ok"));
		if (!Test->TestTrue(Command + TEXT(" ok=true"), bOk))
		{
			Test->AddError(Result.Body.Left(2048));
			return false;
		}
		const TSharedPtr<FJsonObject>* Data = nullptr;
		OutData = (Root->TryGetObjectField(TEXT("data"), Data) && Data && Data->IsValid()) ? *Data : MakeShared<FJsonObject>();
		return true;
	}

	static bool ExecTextureCommandExpectFailure(
		FAutomationTestBase* Test,
		FScopedUeAgentHttpServer& ServerScope,
		const FString& RequestId,
		const FString& Command,
		const FString& ParamsJson,
		const FString& ExpectedToken,
		double TimeoutSeconds = 45.0)
	{
		FHttpSmokeResult Result;
		const FString Body = MakeExecRequestBody(RequestId, Command, ParamsJson);
		if (!Test->TestTrue(Command + TEXT(" failure request completed"), ExecuteHttpJsonRequest(TEXT("POST"), ServerScope.BaseUrl + TEXT("/api/exec"), ServerScope.Token, Body, Result, TimeoutSeconds)))
		{
			return false;
		}
		TSharedPtr<FJsonObject> Root;
		if (!Test->TestTrue(Command + TEXT(" failure response parses"), ParseJson(Result.Body, Root)) || !Root.IsValid())
		{
			return false;
		}
		bool bOk = true;
		Root->TryGetBoolField(TEXT("ok"), bOk);
		if (!Test->TestFalse(Command + TEXT(" ok=false"), bOk))
		{
			Test->AddError(Result.Body.Left(2048));
			return false;
		}
		FString Error;
		Root->TryGetStringField(TEXT("error"), Error);
		const bool bMatches = ExpectedToken.IsEmpty() || Error.Contains(ExpectedToken);
		if (!Test->TestTrue(Command + TEXT(" expected error token ") + ExpectedToken, bMatches))
		{
			Test->AddError(Result.Body.Left(2048));
			return false;
		}
		return true;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentInterfaceTextureRenderTargetSmokeTest,
	"GptProjectTest.UeAgentInterface.Smoke.TextureRenderTargetMediaWorkflow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUeAgentInterfaceTextureRenderTargetSmokeTest::RunTest(const FString& Parameters)
{
	FScopedUeAgentHttpServer ServerScope;
	FString InitError;
	if (!ServerScope.Initialize(InitError))
	{
		AddError(FString::Printf(TEXT("Initialize failed: %s"), *InitError));
		return false;
	}

	const FString RootId = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString RT = MakeAutomationAssetPath(TEXT("RTTex"));
	const FString StaticTexture = MakeAutomationAssetPath(TEXT("TTex"));
	const FString StaticTextureDestinationPath = FPackageName::GetLongPackagePath(StaticTexture);
	const FString StaticTextureDestinationName = FPackageName::GetLongPackageAssetName(StaticTexture);
	const FString TextureArray = MakeAutomationAssetPath(TEXT("TArr"));
	const FString TextureCube = MakeAutomationAssetPath(TEXT("TCube"));
	const FString TextureCubeArray = MakeAutomationAssetPath(TEXT("TCubeArr"));
	const FString VolumeTexture = MakeAutomationAssetPath(TEXT("TVol"));
	const FString RVT = MakeAutomationAssetPath(TEXT("RVT"));
	const FString MediaSource = MakeAutomationAssetPath(TEXT("MSrc"));
	const FString MediaPlayer = MakeAutomationAssetPath(TEXT("MPlayer"));
	const FString MediaTextureAuto = MakeAutomationAssetPath(TEXT("MTexAuto"));
	const FString MediaTexture = MakeAutomationAssetPath(TEXT("MTex"));
	const FString SubUV = MakeAutomationAssetPath(TEXT("SubUV"));
	const FString TextureCollection = MakeAutomationAssetPath(TEXT("TCollection"));
	const FString TextureGraph = MakeAutomationAssetPath(TEXT("TGraph"));
	const FString PaperSprite = MakeAutomationAssetPath(TEXT("Sprite"));
	const FString PaperFlipbook = MakeAutomationAssetPath(TEXT("Flipbook"));
	const FString LightProfile = MakeAutomationAssetPath(TEXT("IES"));
	const FString LightProfileDestinationPath = FPackageName::GetLongPackagePath(LightProfile);
	const FString LightProfileDestinationName = FPackageName::GetLongPackageAssetName(LightProfile);
	const FString SavedRoot = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UeAgentInterfaceTextureSmoke"), RootId);
	const FString ExportPng = JsonPath(FPaths::Combine(SavedRoot, TEXT("rt.png")));
	const FString TexturePng = JsonPath(FPaths::Combine(SavedRoot, TEXT("texture.png")));
	const FString RTJson = JsonPath(FPaths::Combine(SavedRoot, TEXT("rt.json")));
	const FString RVTJson = JsonPath(FPaths::Combine(SavedRoot, TEXT("rvt.json")));
	const FString MediaSourceJson = JsonPath(FPaths::Combine(SavedRoot, TEXT("media_source.json")));
	const FString MediaPlayerJson = JsonPath(FPaths::Combine(SavedRoot, TEXT("media_player.json")));
	const FString MediaTextureJson = JsonPath(FPaths::Combine(SavedRoot, TEXT("media_texture.json")));
	const FString SubUVJson = JsonPath(FPaths::Combine(SavedRoot, TEXT("subuv.json")));
	const FString SpriteJson = JsonPath(FPaths::Combine(SavedRoot, TEXT("sprite.json")));
	const FString FlipbookJson = JsonPath(FPaths::Combine(SavedRoot, TEXT("flipbook.json")));
	const FString CollectionJson = JsonPath(FPaths::Combine(SavedRoot, TEXT("collection.json")));
	const FString ArrayFolder = JsonPath(FPaths::Combine(SavedRoot, TEXT("array")));
	const FString CubeFolder = JsonPath(FPaths::Combine(SavedRoot, TEXT("cube")));
	const FString CubeArrayFolder = JsonPath(FPaths::Combine(SavedRoot, TEXT("cube_array")));
	const FString VolumeFolder = JsonPath(FPaths::Combine(SavedRoot, TEXT("volume")));
	const FString GraphFolder = JsonPath(FPaths::Combine(SavedRoot, TEXT("graph")));
	const FString IesFile = JsonPath(FPaths::Combine(SavedRoot, TEXT("valid.ies")));
	const bool bCanRender = FApp::CanEverRender();
	const FString DefaultTexture = TEXT("/Engine/EngineResources/DefaultTexture");
	const FString SeedTexture = bCanRender ? StaticTexture : DefaultTexture;
	const FString DefaultMaterial = TEXT("/Engine/EngineMaterials/DefaultMaterial");

	TSharedPtr<FJsonObject> Data;
	TestTrue(TEXT("render_target_create"), ExecTextureCommand(this, ServerScope, RootId + TEXT("_rt_create"), TEXT("render_target_create"),
		FString::Printf(TEXT("{\"asset_path\":%s,\"target_type\":\"2d\",\"width\":64,\"height\":64,\"format\":\"RTF_RGBA8\",\"save_after_create\":true}"), *JsonString(RT)), Data));
	TestTrue(TEXT("render_target_get_info"), ExecTextureCommand(this, ServerScope, RootId + TEXT("_rt_info"), TEXT("render_target_get_info"),
		FString::Printf(TEXT("{\"asset_path\":%s,\"include_pixel_stats\":true}"), *JsonString(RT)), Data));
	TestTrue(TEXT("render_target_get_info returns resource_valid"), Data.IsValid() && Data->HasField(TEXT("resource_valid")));
	TestTrue(TEXT("scene_capture_validate_render_target"), ExecTextureCommand(this, ServerScope, RootId + TEXT("_scene_validate"), TEXT("scene_capture_validate_render_target"),
		FString::Printf(TEXT("{\"expected_render_target\":%s,\"expected_capture_source\":\"SCS_FinalColorLDR\"}"), *JsonString(RT)), Data));
	if (bCanRender)
	{
		TestTrue(TEXT("render_target_clear"), ExecTextureCommand(this, ServerScope, RootId + TEXT("_rt_clear"), TEXT("render_target_clear"),
			FString::Printf(TEXT("{\"asset_path\":%s,\"clear_color\":{\"r\":1,\"g\":0,\"b\":0,\"a\":1}}"), *JsonString(RT)), Data));
		TestTrue(TEXT("render_target_sample_uv"), ExecTextureCommand(this, ServerScope, RootId + TEXT("_rt_sample"), TEXT("render_target_sample_uv"),
			FString::Printf(TEXT("{\"asset_path\":%s,\"uvs\":[{\"u\":0.5,\"v\":0.5},{\"u\":0.25,\"v\":0.25}],\"linear\":true}"), *JsonString(RT)), Data));
		TestTrue(TEXT("render_target_sample_uv returns linear"), Data.IsValid() && Data->GetBoolField(TEXT("linear")));
		TestTrue(TEXT("render_target_read_pixels"), ExecTextureCommand(this, ServerScope, RootId + TEXT("_rt_read"), TEXT("render_target_read_pixels"),
			FString::Printf(TEXT("{\"asset_path\":%s,\"rect\":{\"x\":0,\"y\":0,\"width\":8,\"height\":8},\"max_pixels\":4,\"format\":\"rgba8\"}"), *JsonString(RT)), Data));
		TestTrue(TEXT("render_target_probe"), ExecTextureCommand(this, ServerScope, RootId + TEXT("_rt_probe"), TEXT("render_target_probe"),
			FString::Printf(TEXT("{\"asset_path\":%s,\"expected_stats\":{\"min_non_black_pixel_count\":1},\"cleanup\":false}"), *JsonString(RT)), Data));
		TestTrue(TEXT("render_target_probe returns probe_passed"), Data.IsValid() && Data->GetBoolField(TEXT("probe_passed")));
		TestTrue(TEXT("render_target_probe returns steps"), Data.IsValid() && Data->HasField(TEXT("steps")));
		TestTrue(TEXT("render_target_export_image"), ExecTextureCommand(this, ServerScope, RootId + TEXT("_rt_export_image"), TEXT("render_target_export_image"),
			FString::Printf(TEXT("{\"asset_path\":%s,\"output_file\":%s,\"format\":\"png\"}"), *JsonString(RT), *JsonString(ExportPng)), Data));
	}
	else
	{
		TestTrue(TEXT("render_target_clear NullRHI guard"), ExecTextureCommandExpectFailure(this, ServerScope, RootId + TEXT("_rt_clear"), TEXT("render_target_clear"),
			FString::Printf(TEXT("{\"asset_path\":%s,\"clear_color\":{\"r\":1,\"g\":0,\"b\":0,\"a\":1}}"), *JsonString(RT)), TEXT("render_resource_unavailable_null_rhi")));
		TestTrue(TEXT("render_target_sample_uv NullRHI guard"), ExecTextureCommandExpectFailure(this, ServerScope, RootId + TEXT("_rt_sample"), TEXT("render_target_sample_uv"),
			FString::Printf(TEXT("{\"asset_path\":%s,\"u\":0.5,\"v\":0.5}"), *JsonString(RT)), TEXT("render_resource_unavailable_null_rhi")));
		TestTrue(TEXT("render_target_read_pixels NullRHI guard"), ExecTextureCommandExpectFailure(this, ServerScope, RootId + TEXT("_rt_read"), TEXT("render_target_read_pixels"),
			FString::Printf(TEXT("{\"asset_path\":%s}"), *JsonString(RT)), TEXT("render_resource_unavailable_null_rhi")));
		TestTrue(TEXT("render_target_probe NullRHI guard"), ExecTextureCommandExpectFailure(this, ServerScope, RootId + TEXT("_rt_probe"), TEXT("render_target_probe"),
			FString::Printf(TEXT("{\"asset_path\":%s}"), *JsonString(RT)), TEXT("render_resource_unavailable_null_rhi")));
		TestTrue(TEXT("render_target_export_image NullRHI guard"), ExecTextureCommandExpectFailure(this, ServerScope, RootId + TEXT("_rt_export_image"), TEXT("render_target_export_image"),
			FString::Printf(TEXT("{\"asset_path\":%s,\"output_file\":%s,\"format\":\"png\"}"), *JsonString(RT), *JsonString(ExportPng)), TEXT("render_resource_unavailable_null_rhi")));
	}
	TestTrue(TEXT("render_target_export_json"), ExecTextureCommand(this, ServerScope, RootId + TEXT("_rt_export_json"), TEXT("render_target_export_json"),
		FString::Printf(TEXT("{\"asset_path\":%s,\"output_file\":%s}"), *JsonString(RT), *JsonString(RTJson)), Data));
	TestTrue(TEXT("render_target_validate_json"), ExecTextureCommand(this, ServerScope, RootId + TEXT("_rt_validate_json"), TEXT("render_target_validate_json"),
		FString::Printf(TEXT("{\"json_file\":%s}"), *JsonString(RTJson)), Data));
	TestTrue(TEXT("render_target_apply_json"), ExecTextureCommand(this, ServerScope, RootId + TEXT("_rt_apply_json"), TEXT("render_target_apply_json"),
		FString::Printf(TEXT("{\"json_file\":%s,\"asset_path\":%s,\"save_after_apply\":true}"), *JsonString(RTJson), *JsonString(RT)), Data));
	TestTrue(TEXT("render_target_resize rejects missing allow"), ExecTextureCommandExpectFailure(this, ServerScope, RootId + TEXT("_rt_resize_fail"), TEXT("render_target_resize"),
		FString::Printf(TEXT("{\"asset_path\":%s,\"width\":32,\"height\":32}"), *JsonString(RT)), TEXT("resize_requires_allow_resize")));
	TestTrue(TEXT("render_target_resize"), ExecTextureCommand(this, ServerScope, RootId + TEXT("_rt_resize"), TEXT("render_target_resize"),
		FString::Printf(TEXT("{\"asset_path\":%s,\"width\":32,\"height\":32,\"allow_resize\":true,\"preserve_contents\":true,\"save_after_resize\":true}"), *JsonString(RT)), Data));
	if (bCanRender)
	{
		TestTrue(TEXT("render_target_create_static_texture"), ExecTextureCommand(this, ServerScope, RootId + TEXT("_rt_static"), TEXT("render_target_create_static_texture"),
			FString::Printf(TEXT("{\"render_target\":%s,\"destination_path\":%s,\"destination_name\":%s,\"save_after_create\":true}"), *JsonString(RT), *JsonString(StaticTextureDestinationPath), *JsonString(StaticTextureDestinationName)), Data));
		TestTrue(TEXT("render_target_draw_material"), ExecTextureCommand(this, ServerScope, RootId + TEXT("_rt_draw"), TEXT("render_target_draw_material"),
			FString::Printf(TEXT("{\"asset_path\":%s,\"material\":%s,\"parameters\":{\"UnusedScalar\":0.5},\"feature_level\":\"editor\"}"), *JsonString(RT), *JsonString(DefaultMaterial)), Data));
		TestTrue(TEXT("render_target_draw_material returns drawn"), Data.IsValid() && Data->GetBoolField(TEXT("drawn")));
		TestTrue(TEXT("render_target_draw_material returns stats_after"), Data.IsValid() && Data->HasField(TEXT("stats_after")));
		TestTrue(TEXT("render_target_copy_texture"), ExecTextureCommand(this, ServerScope, RootId + TEXT("_rt_copy"), TEXT("render_target_copy_texture"),
			FString::Printf(TEXT("{\"target_render_target\":%s,\"source_texture\":%s}"), *JsonString(RT), *JsonString(SeedTexture)), Data));
		TestTrue(TEXT("render_target_capture_scene"), ExecTextureCommand(this, ServerScope, RootId + TEXT("_rt_capture"), TEXT("render_target_capture_scene"),
			FString::Printf(TEXT("{\"asset_path\":%s,\"camera\":{\"location\":{\"x\":0,\"y\":-300,\"z\":200},\"rotation\":{\"pitch\":-20,\"yaw\":0,\"roll\":0},\"fov\":75},\"capture_source\":\"SCS_FinalColorLDR\",\"actors_only\":[],\"hidden_actors\":[],\"show_flags\":{}}"), *JsonString(RT)), Data));
		TestTrue(TEXT("render_target_capture_scene returns captured"), Data.IsValid() && Data->GetBoolField(TEXT("captured")));
		TestTrue(TEXT("render_target_capture_scene returns stats_after"), Data.IsValid() && Data->HasField(TEXT("stats_after")));
		TestTrue(TEXT("scene_capture_runtime_probe"), ExecTextureCommand(this, ServerScope, RootId + TEXT("_scene_probe"), TEXT("scene_capture_runtime_probe"),
			FString::Printf(TEXT("{\"render_target\":%s,\"expected_changed\":false}"), *JsonString(RT)), Data));
	}
	else
	{
		TestTrue(TEXT("render_target_create_static_texture NullRHI guard"), ExecTextureCommandExpectFailure(this, ServerScope, RootId + TEXT("_rt_static"), TEXT("render_target_create_static_texture"),
			FString::Printf(TEXT("{\"render_target\":%s,\"destination_path\":%s,\"destination_name\":%s,\"save_after_create\":true}"), *JsonString(RT), *JsonString(StaticTextureDestinationPath), *JsonString(StaticTextureDestinationName)), TEXT("render_resource_unavailable_null_rhi")));
		TestTrue(TEXT("render_target_draw_material NullRHI guard"), ExecTextureCommandExpectFailure(this, ServerScope, RootId + TEXT("_rt_draw"), TEXT("render_target_draw_material"),
			FString::Printf(TEXT("{\"asset_path\":%s,\"material\":%s}"), *JsonString(RT), *JsonString(DefaultMaterial)), TEXT("render_resource_unavailable_null_rhi")));
		TestTrue(TEXT("render_target_copy_texture NullRHI guard"), ExecTextureCommandExpectFailure(this, ServerScope, RootId + TEXT("_rt_copy"), TEXT("render_target_copy_texture"),
			FString::Printf(TEXT("{\"target_render_target\":%s,\"source_texture\":%s}"), *JsonString(RT), *JsonString(SeedTexture)), TEXT("render_resource_unavailable_null_rhi")));
		TestTrue(TEXT("render_target_capture_scene NullRHI guard"), ExecTextureCommandExpectFailure(this, ServerScope, RootId + TEXT("_rt_capture"), TEXT("render_target_capture_scene"),
			FString::Printf(TEXT("{\"asset_path\":%s}"), *JsonString(RT)), TEXT("render_resource_unavailable_null_rhi")));
		TestTrue(TEXT("scene_capture_runtime_probe NullRHI guard"), ExecTextureCommandExpectFailure(this, ServerScope, RootId + TEXT("_scene_probe"), TEXT("scene_capture_runtime_probe"),
			FString::Printf(TEXT("{\"asset_path\":%s}"), *JsonString(RT)), TEXT("render_resource_unavailable_null_rhi")));
	}

	TestTrue(TEXT("texture_get_info"), ExecTextureCommand(this, ServerScope, RootId + TEXT("_tex_info"), TEXT("texture_get_info"),
		FString::Printf(TEXT("{\"asset_path\":%s,\"include_pixel_stats\":true,\"include_build_settings\":true,\"include_platform_data\":true}"), *JsonString(SeedTexture)), Data));
	TestTrue(TEXT("texture_get_info returns compression"), Data.IsValid() && Data->HasField(TEXT("compression")));
	TestTrue(TEXT("texture_validate_settings"), ExecTextureCommand(this, ServerScope, RootId + TEXT("_tex_validate"), TEXT("texture_validate_settings"),
		FString::Printf(TEXT("{\"asset_path\":%s,\"expected_usage\":\"color\"}"), *JsonString(SeedTexture)), Data));
	TestTrue(TEXT("texture_pixel_probe"), ExecTextureCommand(this, ServerScope, RootId + TEXT("_tex_probe"), TEXT("texture_pixel_probe"),
		FString::Printf(TEXT("{\"asset_path\":%s,\"uvs\":[{\"u\":0.5,\"v\":0.5}],\"channels\":\"rgba\",\"expected_stats\":{\"min_non_black_pixel_count\":0}}"), *JsonString(SeedTexture)), Data));
	TestTrue(TEXT("texture_pixel_probe returns samples"), Data.IsValid() && Data->HasField(TEXT("samples")));
	TestTrue(TEXT("texture_export_image"), ExecTextureCommand(this, ServerScope, RootId + TEXT("_tex_export"), TEXT("texture_export_image"),
		FString::Printf(TEXT("{\"asset_path\":%s,\"output_file\":%s,\"format\":\"png\"}"), *JsonString(SeedTexture), *JsonString(TexturePng)), Data));
	TestTrue(TEXT("texture_screenshot"), ExecTextureCommand(this, ServerScope, RootId + TEXT("_tex_shot"), TEXT("texture_screenshot"),
		FString::Printf(TEXT("{\"asset_path\":%s,\"format\":\"png\"}"), *JsonString(SeedTexture)), Data));
	TestTrue(TEXT("texture_screenshot returns channel_stats"), Data.IsValid() && Data->HasField(TEXT("channel_stats")));
	if (bCanRender)
	{
		TestTrue(TEXT("texture_reimport"), ExecTextureCommand(this, ServerScope, RootId + TEXT("_tex_reimport"), TEXT("texture_reimport"),
			FString::Printf(TEXT("{\"asset_path\":%s,\"source_filename\":%s,\"save_after_reimport\":true}"), *JsonString(SeedTexture), *JsonString(TexturePng)), Data));
	}
	else
	{
		TestTrue(TEXT("texture_reimport missing source failure path"), ExecTextureCommandExpectFailure(this, ServerScope, RootId + TEXT("_tex_reimport_fail"), TEXT("texture_reimport"),
			FString::Printf(TEXT("{\"asset_path\":%s,\"source_filename\":%s}"), *JsonString(SeedTexture), *JsonString(JsonPath(FPaths::Combine(SavedRoot, TEXT("missing.png"))))), TEXT("source_file_not_found")));
	}

	const FString SingleTextureArray = JsonArray({ SeedTexture });
	const FString SixFacesObject = FString::Printf(TEXT("{\"+X\":%s,\"-X\":%s,\"+Y\":%s,\"-Y\":%s,\"+Z\":%s,\"-Z\":%s}"),
		*JsonString(SeedTexture), *JsonString(SeedTexture), *JsonString(SeedTexture), *JsonString(SeedTexture), *JsonString(SeedTexture), *JsonString(SeedTexture));
	TestTrue(TEXT("texture_array_create"), ExecTextureCommand(this, ServerScope, RootId + TEXT("_arr_create"), TEXT("texture_array_create"),
		FString::Printf(TEXT("{\"asset_path\":%s,\"source_textures\":%s,\"save_after_create\":true}"), *JsonString(TextureArray), *SingleTextureArray), Data));
	TestTrue(TEXT("texture_array_export_folder"), ExecTextureCommand(this, ServerScope, RootId + TEXT("_arr_export"), TEXT("texture_array_export_folder"),
		FString::Printf(TEXT("{\"asset_path\":%s,\"folder_path\":%s}"), *JsonString(TextureArray), *JsonString(ArrayFolder)), Data));
	FString ArrayAssetJsonText;
	TSharedPtr<FJsonObject> ArrayAssetJson;
	TestTrue(TEXT("texture_array_export_folder asset.json loads"), FFileHelper::LoadFileToString(ArrayAssetJsonText, *FPaths::Combine(ArrayFolder, TEXT("asset.json"))) && ParseJson(ArrayAssetJsonText, ArrayAssetJson));
	TestTrue(TEXT("texture_array_export_folder asset details ref"), ArrayAssetJson.IsValid() && ArrayAssetJson->HasTypedField<EJson::Object>(TEXT("details")));
	TestFalse(TEXT("texture_array_export_folder omits root raw_properties.json"), FPaths::FileExists(FPaths::Combine(ArrayFolder, TEXT("raw_properties.json"))));
	TestTrue(TEXT("texture_array_apply_folder"), ExecTextureCommand(this, ServerScope, RootId + TEXT("_arr_apply"), TEXT("texture_array_apply_folder"),
		FString::Printf(TEXT("{\"folder_path\":%s,\"asset_path\":%s,\"save_after_apply\":true}"), *JsonString(ArrayFolder), *JsonString(TextureArray)), Data));
	TestTrue(TEXT("texture_array_apply_folder returns slice_results"), Data.IsValid() && Data->HasField(TEXT("slice_results")));

	TestTrue(TEXT("texture_cube_create"), ExecTextureCommand(this, ServerScope, RootId + TEXT("_cube_create"), TEXT("texture_cube_create"),
		FString::Printf(TEXT("{\"asset_path\":%s,\"faces\":%s,\"save_after_create\":true}"), *JsonString(TextureCube), *SixFacesObject), Data));
	TestTrue(TEXT("texture_cube_export_folder"), ExecTextureCommand(this, ServerScope, RootId + TEXT("_cube_export"), TEXT("texture_cube_export_folder"),
		FString::Printf(TEXT("{\"asset_path\":%s,\"folder_path\":%s}"), *JsonString(TextureCube), *JsonString(CubeFolder)), Data));
	TestTrue(TEXT("texture_cube_apply_folder"), ExecTextureCommand(this, ServerScope, RootId + TEXT("_cube_apply"), TEXT("texture_cube_apply_folder"),
		FString::Printf(TEXT("{\"folder_path\":%s,\"asset_path\":%s,\"save_after_apply\":true}"), *JsonString(CubeFolder), *JsonString(TextureCube)), Data));
	TestTrue(TEXT("texture_cube_apply_folder returns face_results"), Data.IsValid() && Data->HasField(TEXT("face_results")));
	TestTrue(TEXT("texture_cube_array_create"), ExecTextureCommand(this, ServerScope, RootId + TEXT("_cubearr_create"), TEXT("texture_cube_array_create"),
		FString::Printf(TEXT("{\"asset_path\":%s,\"source_cubes\":%s,\"save_after_create\":true}"), *JsonString(TextureCubeArray), *JsonArray({ TextureCube })), Data));
	TestTrue(TEXT("texture_cube_array_export_folder"), ExecTextureCommand(this, ServerScope, RootId + TEXT("_cubearr_export"), TEXT("texture_cube_array_export_folder"),
		FString::Printf(TEXT("{\"asset_path\":%s,\"folder_path\":%s}"), *JsonString(TextureCubeArray), *JsonString(CubeArrayFolder)), Data));
	TestTrue(TEXT("texture_cube_array_apply_folder"), ExecTextureCommand(this, ServerScope, RootId + TEXT("_cubearr_apply"), TEXT("texture_cube_array_apply_folder"),
		FString::Printf(TEXT("{\"folder_path\":%s,\"asset_path\":%s}"), *JsonString(CubeArrayFolder), *JsonString(TextureCubeArray)), Data));
	TestTrue(TEXT("texture_cube_array_apply_folder returns property_results"), Data.IsValid() && Data->HasField(TEXT("property_results")));
	TestTrue(TEXT("volume_texture_create"), ExecTextureCommand(this, ServerScope, RootId + TEXT("_volume_create"), TEXT("volume_texture_create"),
		FString::Printf(TEXT("{\"asset_path\":%s,\"source_textures\":%s,\"depth\":1,\"save_after_create\":true}"), *JsonString(VolumeTexture), *SingleTextureArray), Data));
	TestTrue(TEXT("volume_texture_export_folder"), ExecTextureCommand(this, ServerScope, RootId + TEXT("_volume_export"), TEXT("volume_texture_export_folder"),
		FString::Printf(TEXT("{\"asset_path\":%s,\"folder_path\":%s}"), *JsonString(VolumeTexture), *JsonString(VolumeFolder)), Data));
	TestTrue(TEXT("volume_texture_apply_folder"), ExecTextureCommand(this, ServerScope, RootId + TEXT("_volume_apply"), TEXT("volume_texture_apply_folder"),
		FString::Printf(TEXT("{\"folder_path\":%s,\"asset_path\":%s}"), *JsonString(VolumeFolder), *JsonString(VolumeTexture)), Data));
	TestTrue(TEXT("volume_texture_apply_folder returns slice_results"), Data.IsValid() && Data->HasField(TEXT("slice_results")));

	TestTrue(TEXT("virtual_texture_get_info"), ExecTextureCommand(this, ServerScope, RootId + TEXT("_vt_info"), TEXT("virtual_texture_get_info"),
		FString::Printf(TEXT("{\"asset_path\":%s,\"include_project_settings\":true}"), *JsonString(SeedTexture)), Data));
	TestTrue(TEXT("virtual_texture_get_info returns project_vt_enabled"), Data.IsValid() && Data->HasField(TEXT("project_vt_enabled")));
	TestTrue(TEXT("virtual_texture_validate_setup"), ExecTextureCommand(this, ServerScope, RootId + TEXT("_vt_validate"), TEXT("virtual_texture_validate_setup"),
		FString::Printf(TEXT("{\"asset_path\":%s}"), *JsonString(SeedTexture)), Data));
	TestTrue(TEXT("runtime_virtual_texture_create"), ExecTextureCommand(this, ServerScope, RootId + TEXT("_rvt_create"), TEXT("runtime_virtual_texture_create"),
		FString::Printf(TEXT("{\"asset_path\":%s,\"save_after_create\":true}"), *JsonString(RVT)), Data));
	TestTrue(TEXT("runtime_virtual_texture_get_info"), ExecTextureCommand(this, ServerScope, RootId + TEXT("_rvt_info"), TEXT("runtime_virtual_texture_get_info"),
		FString::Printf(TEXT("{\"asset_path\":%s}"), *JsonString(RVT)), Data));
	TestTrue(TEXT("runtime_virtual_texture_export_json"), ExecTextureCommand(this, ServerScope, RootId + TEXT("_rvt_export"), TEXT("runtime_virtual_texture_export_json"),
		FString::Printf(TEXT("{\"asset_path\":%s,\"output_file\":%s}"), *JsonString(RVT), *JsonString(RVTJson)), Data));
	TestTrue(TEXT("runtime_virtual_texture_apply_json"), ExecTextureCommand(this, ServerScope, RootId + TEXT("_rvt_apply"), TEXT("runtime_virtual_texture_apply_json"),
		FString::Printf(TEXT("{\"json_file\":%s,\"asset_path\":%s,\"save_after_apply\":true}"), *JsonString(RVTJson), *JsonString(RVT)), Data));
	TestTrue(TEXT("runtime_virtual_texture_validate_setup"), ExecTextureCommand(this, ServerScope, RootId + TEXT("_rvt_validate"), TEXT("runtime_virtual_texture_validate_setup"),
		FString::Printf(TEXT("{\"asset_path\":%s}"), *JsonString(RVT)), Data));

	TestTrue(TEXT("media_source_create"), ExecTextureCommand(this, ServerScope, RootId + TEXT("_ms_create"), TEXT("media_source_create"),
		FString::Printf(TEXT("{\"asset_path\":%s,\"source_type\":\"file\",\"url_or_file\":%s,\"save_after_create\":true}"), *JsonString(MediaSource), *JsonString(JsonPath(FPaths::Combine(SavedRoot, TEXT("missing.mp4"))))), Data));
	TestTrue(TEXT("media_source_get_info"), ExecTextureCommand(this, ServerScope, RootId + TEXT("_ms_info"), TEXT("media_source_get_info"),
		FString::Printf(TEXT("{\"asset_path\":%s}"), *JsonString(MediaSource)), Data));
	TestTrue(TEXT("media_source_export_json"), ExecTextureCommand(this, ServerScope, RootId + TEXT("_ms_export"), TEXT("media_source_export_json"),
		FString::Printf(TEXT("{\"asset_path\":%s,\"output_file\":%s}"), *JsonString(MediaSource), *JsonString(MediaSourceJson)), Data));
	const TArray<TSharedPtr<FJsonValue>>* MediaSourceProperties = nullptr;
	TestTrue(TEXT("media_source_export_json exposes authoring properties"), Data.IsValid() && Data->TryGetArrayField(TEXT("properties"), MediaSourceProperties) && MediaSourceProperties && MediaSourceProperties->Num() > 0);
	if (MediaSourceProperties && MediaSourceProperties->Num() > 0)
	{
		const TSharedPtr<FJsonObject>* FirstProperty = nullptr;
		if ((*MediaSourceProperties)[0].IsValid() && (*MediaSourceProperties)[0]->TryGetObject(FirstProperty) && FirstProperty && FirstProperty->IsValid())
		{
			TestTrue(TEXT("media_source property has property_path"), (*FirstProperty)->HasField(TEXT("property_path")));
			TestTrue(TEXT("media_source property has selector"), (*FirstProperty)->HasField(TEXT("selector")));
			TestTrue(TEXT("media_source property has availability"), (*FirstProperty)->HasField(TEXT("availability")));
			TestTrue(TEXT("media_source property has operations"), (*FirstProperty)->HasField(TEXT("operations")));
			TestFalse(TEXT("media_source property omits legacy name"), (*FirstProperty)->HasField(TEXT("name")));
			TestFalse(TEXT("media_source property omits cpp_type"), (*FirstProperty)->HasField(TEXT("cpp_type")));
		}
	}
	TestTrue(TEXT("media_source_apply_json"), ExecTextureCommand(this, ServerScope, RootId + TEXT("_ms_apply"), TEXT("media_source_apply_json"),
		FString::Printf(TEXT("{\"json_file\":%s,\"asset_path\":%s,\"save_after_apply\":true}"), *JsonString(MediaSourceJson), *JsonString(MediaSource)), Data));
	TestTrue(TEXT("media_player_create"), ExecTextureCommand(this, ServerScope, RootId + TEXT("_mp_create"), TEXT("media_player_create"),
		FString::Printf(TEXT("{\"asset_path\":%s,\"video_output_texture\":%s,\"save_after_create\":true}"), *JsonString(MediaPlayer), *JsonString(MediaTextureAuto)), Data));
	TestTrue(TEXT("media_player_create returns created_media_texture"), Data.IsValid() && Data->GetStringField(TEXT("created_media_texture")) == MediaTextureAuto);
	TestTrue(TEXT("media_player_export_json"), ExecTextureCommand(this, ServerScope, RootId + TEXT("_mp_export"), TEXT("media_player_export_json"),
		FString::Printf(TEXT("{\"asset_path\":%s,\"output_file\":%s}"), *JsonString(MediaPlayer), *JsonString(MediaPlayerJson)), Data));
	TestTrue(TEXT("media_player_export_json returns tracks"), Data.IsValid() && Data->HasField(TEXT("tracks")));
	TestTrue(TEXT("media_player_apply_json"), ExecTextureCommand(this, ServerScope, RootId + TEXT("_mp_apply"), TEXT("media_player_apply_json"),
		FString::Printf(TEXT("{\"json_file\":%s,\"asset_path\":%s,\"save_after_apply\":true}"), *JsonString(MediaPlayerJson), *JsonString(MediaPlayer)), Data));
	TestTrue(TEXT("media_texture_create"), ExecTextureCommand(this, ServerScope, RootId + TEXT("_mt_create"), TEXT("media_texture_create"),
		FString::Printf(TEXT("{\"asset_path\":%s,\"media_player\":%s,\"address_x\":\"TA_Clamp\",\"address_y\":\"TA_Clamp\",\"clear_color\":{\"r\":0,\"g\":0,\"b\":0,\"a\":1},\"save_after_create\":true}"), *JsonString(MediaTexture), *JsonString(MediaPlayer)), Data));
	TestTrue(TEXT("media_texture_create returns property_results"), Data.IsValid() && Data->HasField(TEXT("property_results")));
	TestTrue(TEXT("media_texture_export_json"), ExecTextureCommand(this, ServerScope, RootId + TEXT("_mt_export"), TEXT("media_texture_export_json"),
		FString::Printf(TEXT("{\"asset_path\":%s,\"output_file\":%s}"), *JsonString(MediaTexture), *JsonString(MediaTextureJson)), Data));
	TestTrue(TEXT("media_texture_apply_json"), ExecTextureCommand(this, ServerScope, RootId + TEXT("_mt_apply"), TEXT("media_texture_apply_json"),
		FString::Printf(TEXT("{\"json_file\":%s,\"asset_path\":%s,\"save_after_apply\":true}"), *JsonString(MediaTextureJson), *JsonString(MediaTexture)), Data));
	TestTrue(TEXT("media_runtime_probe"), ExecTextureCommand(this, ServerScope, RootId + TEXT("_media_probe"), TEXT("media_runtime_probe"),
		FString::Printf(TEXT("{\"media_player\":%s,\"media_source\":%s,\"media_texture\":%s,\"duration\":0.1,\"expected_min_frame_changes\":0,\"attempt_open\":false}"), *JsonString(MediaPlayer), *JsonString(MediaSource), *JsonString(MediaTexture)), Data));

	TestTrue(TEXT("texture_collection_create"), ExecTextureCommand(this, ServerScope, RootId + TEXT("_collection_create"), TEXT("texture_collection_create"),
		FString::Printf(TEXT("{\"asset_path\":%s,\"textures\":%s,\"save_after_create\":true}"), *JsonString(TextureCollection), *JsonArray({ SeedTexture, MediaTexture })), Data));
	TestTrue(TEXT("texture_collection_get_info"), ExecTextureCommand(this, ServerScope, RootId + TEXT("_collection_info"), TEXT("texture_collection_get_info"),
		FString::Printf(TEXT("{\"asset_path\":%s}"), *JsonString(TextureCollection)), Data));
	TestTrue(TEXT("texture_collection_export_json"), ExecTextureCommand(this, ServerScope, RootId + TEXT("_collection_export"), TEXT("texture_collection_export_json"),
		FString::Printf(TEXT("{\"asset_path\":%s,\"output_file\":%s}"), *JsonString(TextureCollection), *JsonString(CollectionJson)), Data));
	TestTrue(TEXT("texture_collection_apply_json"), ExecTextureCommand(this, ServerScope, RootId + TEXT("_collection_apply"), TEXT("texture_collection_apply_json"),
		FString::Printf(TEXT("{\"json_file\":%s,\"asset_path\":%s,\"allow_destructive\":true,\"save_after_apply\":true}"), *JsonString(CollectionJson), *JsonString(TextureCollection)), Data));
	TestTrue(TEXT("texture_collection_apply_json returns texture_results"), Data.IsValid() && Data->HasField(TEXT("texture_results")));
	TestTrue(TEXT("texture_collection_validate_usage"), ExecTextureCommand(this, ServerScope, RootId + TEXT("_collection_validate_usage"), TEXT("texture_collection_validate_usage"),
		FString::Printf(TEXT("{\"asset_path\":%s}"), *JsonString(TextureCollection)), Data));

	TestTrue(TEXT("subuv_animation_create"), ExecTextureCommand(this, ServerScope, RootId + TEXT("_subuv_create"), TEXT("subuv_animation_create"),
		FString::Printf(TEXT("{\"asset_path\":%s,\"source_texture\":%s,\"horizontal_images\":1,\"vertical_images\":1,\"save_after_create\":true}"), *JsonString(SubUV), *JsonString(SeedTexture)), Data));
	TestTrue(TEXT("subuv_animation_get_info"), ExecTextureCommand(this, ServerScope, RootId + TEXT("_subuv_info"), TEXT("subuv_animation_get_info"),
		FString::Printf(TEXT("{\"asset_path\":%s}"), *JsonString(SubUV)), Data));
	TestTrue(TEXT("subuv_animation_export_json"), ExecTextureCommand(this, ServerScope, RootId + TEXT("_subuv_export"), TEXT("subuv_animation_export_json"),
		FString::Printf(TEXT("{\"asset_path\":%s,\"output_file\":%s}"), *JsonString(SubUV), *JsonString(SubUVJson)), Data));
	TestTrue(TEXT("subuv_animation_apply_json"), ExecTextureCommand(this, ServerScope, RootId + TEXT("_subuv_apply"), TEXT("subuv_animation_apply_json"),
		FString::Printf(TEXT("{\"json_file\":%s,\"asset_path\":%s,\"save_after_apply\":true}"), *JsonString(SubUVJson), *JsonString(SubUV)), Data));
	TestTrue(TEXT("subuv_animation_preview_probe"), ExecTextureCommand(this, ServerScope, RootId + TEXT("_subuv_probe"), TEXT("subuv_animation_preview_probe"),
		FString::Printf(TEXT("{\"asset_path\":%s,\"frame_indices\":[0],\"expected_non_empty\":false}"), *JsonString(SubUV)), Data));
	TestTrue(TEXT("subuv_animation_preview_probe returns frame_stats"), Data.IsValid() && Data->HasField(TEXT("frame_stats")));

	TestTrue(TEXT("texture_graph_create"), ExecTextureCommand(this, ServerScope, RootId + TEXT("_tg_create"), TEXT("texture_graph_create"),
		FString::Printf(TEXT("{\"asset_path\":%s,\"template\":\"empty\",\"save_after_create\":true}"), *JsonString(TextureGraph)), Data));
	TestTrue(TEXT("texture_graph_create returns template"), Data.IsValid() && Data->GetStringField(TEXT("template")) == TEXT("empty"));
	TestTrue(TEXT("texture_graph_get_info"), ExecTextureCommand(this, ServerScope, RootId + TEXT("_tg_info"), TEXT("texture_graph_get_info"),
		FString::Printf(TEXT("{\"asset_path\":%s}"), *JsonString(TextureGraph)), Data));
	TestTrue(TEXT("texture_graph_export_folder"), ExecTextureCommand(this, ServerScope, RootId + TEXT("_tg_export"), TEXT("texture_graph_export_folder"),
		FString::Printf(TEXT("{\"asset_path\":%s,\"folder_path\":%s}"), *JsonString(TextureGraph), *JsonString(GraphFolder)), Data));
	FString TextureGraphAssetJsonText;
	TSharedPtr<FJsonObject> TextureGraphAssetJson;
	TestTrue(TEXT("texture_graph_export_folder asset.json loads"), FFileHelper::LoadFileToString(TextureGraphAssetJsonText, *FPaths::Combine(GraphFolder, TEXT("asset.json"))) && ParseJson(TextureGraphAssetJsonText, TextureGraphAssetJson));
	TestTrue(TEXT("texture_graph_export_folder asset details ref"), TextureGraphAssetJson.IsValid() && TextureGraphAssetJson->HasTypedField<EJson::Object>(TEXT("details")));
	TestFalse(TEXT("texture_graph_export_folder omits root raw_properties.json"), FPaths::FileExists(FPaths::Combine(GraphFolder, TEXT("raw_properties.json"))));
	TestTrue(TEXT("texture_graph_validate_folder"), ExecTextureCommand(this, ServerScope, RootId + TEXT("_tg_validate"), TEXT("texture_graph_validate_folder"),
		FString::Printf(TEXT("{\"folder_path\":%s,\"asset_path\":%s}"), *JsonString(GraphFolder), *JsonString(TextureGraph)), Data));
	TestTrue(TEXT("texture_graph_apply_folder"), ExecTextureCommand(this, ServerScope, RootId + TEXT("_tg_apply"), TEXT("texture_graph_apply_folder"),
		FString::Printf(TEXT("{\"folder_path\":%s,\"asset_path\":%s,\"save_after_apply\":true}"), *JsonString(GraphFolder), *JsonString(TextureGraph)), Data));
	if (bCanRender)
	{
		TestTrue(TEXT("texture_graph_bake_outputs"), ExecTextureCommand(this, ServerScope, RootId + TEXT("_tg_bake"), TEXT("texture_graph_bake_outputs"),
			FString::Printf(TEXT("{\"asset_path\":%s,\"destination_path\":\"/Game/__UeAgentInterfaceSmoke\"}"), *JsonString(TextureGraph)), Data));
		TestTrue(TEXT("texture_graph_runtime_probe"), ExecTextureCommand(this, ServerScope, RootId + TEXT("_tg_probe"), TEXT("texture_graph_runtime_probe"),
			FString::Printf(TEXT("{\"asset_path\":%s,\"parameters\":{},\"output_name\":\"default\",\"expected_stats\":{\"min_non_black_pixel_count\":0}}"), *JsonString(TextureGraph)), Data));
		TestTrue(TEXT("texture_graph_runtime_probe returns parameters"), Data.IsValid() && Data->HasField(TEXT("parameters")));
	}
	else
	{
		TestTrue(TEXT("texture_graph_bake_outputs NullRHI guard"), ExecTextureCommandExpectFailure(this, ServerScope, RootId + TEXT("_tg_bake"), TEXT("texture_graph_bake_outputs"),
			FString::Printf(TEXT("{\"asset_path\":%s,\"destination_path\":\"/Game/__UeAgentInterfaceSmoke\"}"), *JsonString(TextureGraph)), TEXT("texture_graph_render_unavailable_null_rhi")));
		TestTrue(TEXT("texture_graph_runtime_probe NullRHI guard"), ExecTextureCommandExpectFailure(this, ServerScope, RootId + TEXT("_tg_probe"), TEXT("texture_graph_runtime_probe"),
			FString::Printf(TEXT("{\"asset_path\":%s}"), *JsonString(TextureGraph)), TEXT("texture_graph_render_unavailable_null_rhi")));
	}

	TestTrue(TEXT("paper_sprite_create"), ExecTextureCommand(this, ServerScope, RootId + TEXT("_sprite_create"), TEXT("paper_sprite_create"),
		FString::Printf(TEXT("{\"asset_path\":%s,\"source_texture\":%s,\"save_after_create\":true}"), *JsonString(PaperSprite), *JsonString(SeedTexture)), Data));
	TestTrue(TEXT("paper_sprite_export_json"), ExecTextureCommand(this, ServerScope, RootId + TEXT("_sprite_export"), TEXT("paper_sprite_export_json"),
		FString::Printf(TEXT("{\"asset_path\":%s,\"output_file\":%s}"), *JsonString(PaperSprite), *JsonString(SpriteJson)), Data));
	TestTrue(TEXT("paper_sprite_apply_json"), ExecTextureCommand(this, ServerScope, RootId + TEXT("_sprite_apply"), TEXT("paper_sprite_apply_json"),
		FString::Printf(TEXT("{\"json_file\":%s,\"asset_path\":%s,\"save_after_apply\":true}"), *JsonString(SpriteJson), *JsonString(PaperSprite)), Data));
	TestTrue(TEXT("paper_flipbook_create"), ExecTextureCommand(this, ServerScope, RootId + TEXT("_flip_create"), TEXT("paper_flipbook_create"),
		FString::Printf(TEXT("{\"asset_path\":%s,\"frames_per_second\":12,\"save_after_create\":true}"), *JsonString(PaperFlipbook)), Data));
	TestTrue(TEXT("paper_flipbook_export_json"), ExecTextureCommand(this, ServerScope, RootId + TEXT("_flip_export"), TEXT("paper_flipbook_export_json"),
		FString::Printf(TEXT("{\"asset_path\":%s,\"output_file\":%s}"), *JsonString(PaperFlipbook), *JsonString(FlipbookJson)), Data));
	TestTrue(TEXT("paper_flipbook_apply_json"), ExecTextureCommand(this, ServerScope, RootId + TEXT("_flip_apply"), TEXT("paper_flipbook_apply_json"),
		FString::Printf(TEXT("{\"json_file\":%s,\"asset_path\":%s,\"save_after_apply\":true}"), *JsonString(FlipbookJson), *JsonString(PaperFlipbook)), Data));
	TestTrue(TEXT("paper2d_preview_probe"), ExecTextureCommand(this, ServerScope, RootId + TEXT("_paper_probe"), TEXT("paper2d_preview_probe"),
		FString::Printf(TEXT("{\"asset_path\":%s,\"expected_bounds\":{},\"expected_non_empty\":false}"), *JsonString(PaperSprite)), Data));
	TestTrue(TEXT("paper2d_preview_probe returns bounds"), Data.IsValid() && Data->HasField(TEXT("bounds")));
	TestTrue(TEXT("paper_tileset_export_folder failure path"), ExecTextureCommandExpectFailure(this, ServerScope, RootId + TEXT("_tileset_export_fail"), TEXT("paper_tileset_export_folder"),
		FString::Printf(TEXT("{\"asset_path\":\"/Game/__UeAgentInterfaceSmoke/MissingTileSet\",\"folder_path\":%s}"), *JsonString(JsonPath(FPaths::Combine(SavedRoot, TEXT("tileset"))))), TEXT("asset_not_found")));
	TestTrue(TEXT("paper_tileset_apply_folder failure path"), ExecTextureCommandExpectFailure(this, ServerScope, RootId + TEXT("_tileset_apply_fail"), TEXT("paper_tileset_apply_folder"),
		FString::Printf(TEXT("{\"folder_path\":%s,\"asset_path\":\"/Game/__UeAgentInterfaceSmoke/MissingTileSet\"}"), *JsonString(JsonPath(FPaths::Combine(SavedRoot, TEXT("tileset_missing"))))), TEXT("json_file_not_found")));
	TestTrue(TEXT("paper_tilemap_export_folder failure path"), ExecTextureCommandExpectFailure(this, ServerScope, RootId + TEXT("_tilemap_export_fail"), TEXT("paper_tilemap_export_folder"),
		FString::Printf(TEXT("{\"asset_path\":\"/Game/__UeAgentInterfaceSmoke/MissingTileMap\",\"folder_path\":%s}"), *JsonString(JsonPath(FPaths::Combine(SavedRoot, TEXT("tilemap"))))), TEXT("asset_not_found")));
	TestTrue(TEXT("paper_tilemap_apply_folder failure path"), ExecTextureCommandExpectFailure(this, ServerScope, RootId + TEXT("_tilemap_apply_fail"), TEXT("paper_tilemap_apply_folder"),
		FString::Printf(TEXT("{\"folder_path\":%s,\"asset_path\":\"/Game/__UeAgentInterfaceSmoke/MissingTileMap\"}"), *JsonString(JsonPath(FPaths::Combine(SavedRoot, TEXT("tilemap_missing"))))), TEXT("json_file_not_found")));

	TestTrue(TEXT("texture_usage_get_references"), ExecTextureCommand(this, ServerScope, RootId + TEXT("_usage_refs"), TEXT("texture_usage_get_references"),
		FString::Printf(TEXT("{\"texture_asset\":%s}"), *JsonString(SeedTexture)), Data));
	TestTrue(TEXT("texture_usage_validate"), ExecTextureCommand(this, ServerScope, RootId + TEXT("_usage_validate"), TEXT("texture_usage_validate"),
		FString::Printf(TEXT("{\"texture_asset\":%s,\"usage_type\":\"any\",\"strict\":true}"), *JsonString(SeedTexture)), Data));
	TestTrue(TEXT("texture_usage_validate returns usage_type"), Data.IsValid() && Data->GetStringField(TEXT("usage_type")) == TEXT("any"));
	TestTrue(TEXT("texture_usage_probe_media"), ExecTextureCommand(this, ServerScope, RootId + TEXT("_usage_media"), TEXT("texture_usage_probe_media"),
		FString::Printf(TEXT("{\"asset_path\":%s,\"expected_media_texture\":%s,\"expected_media_player\":%s}"), *JsonString(MediaTexture), *JsonString(MediaTexture), *JsonString(MediaPlayer)), Data));
	TestTrue(TEXT("texture_usage_probe_media returns expected_media_player"), Data.IsValid() && Data->HasField(TEXT("expected_media_player")));
	TestTrue(TEXT("texture_usage_probe_sparse_volume failure path"), ExecTextureCommandExpectFailure(this, ServerScope, RootId + TEXT("_usage_svt_fail"), TEXT("texture_usage_probe_sparse_volume"),
		TEXT("{\"asset_path\":\"/Game/__UeAgentInterfaceSmoke/MissingSVT\"}"), TEXT("asset_not_found")));
	const FString IesText =
		TEXT("IESNA:LM-63-2002\n")
		TEXT("[TEST] UAI Smoke\n")
		TEXT("[MANUFAC] UeAgentInterface\n")
		TEXT("TILT=NONE\n")
		TEXT("1 1000 1 3 3 1 1 0.1 0.1 0.1\n")
		TEXT("1 1 1\n")
		TEXT("0 45 90\n")
		TEXT("0 90 180\n")
		TEXT("1000 800 400 800 600 300 400 300 100\n");
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(IesFile), true);
	TestTrue(TEXT("valid IES fixture saved"), FFileHelper::SaveStringToFile(IesText, *IesFile, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));
	TestTrue(TEXT("light_profile_texture_import"), ExecTextureCommand(this, ServerScope, RootId + TEXT("_ies_import"), TEXT("light_profile_texture_import"),
		FString::Printf(TEXT("{\"source_file\":%s,\"destination_path\":%s,\"destination_name\":%s,\"save_after_import\":true}"), *JsonString(IesFile), *JsonString(LightProfileDestinationPath), *JsonString(LightProfileDestinationName)), Data));
	TestTrue(TEXT("light_profile_texture_get_info"), ExecTextureCommand(this, ServerScope, RootId + TEXT("_ies_info"), TEXT("light_profile_texture_get_info"),
		FString::Printf(TEXT("{\"asset_path\":%s}"), *JsonString(LightProfile)), Data));
	TestTrue(TEXT("sparse_volume_texture_import failure path"), ExecTextureCommandExpectFailure(this, ServerScope, RootId + TEXT("_svt_import_fail"), TEXT("sparse_volume_texture_import"),
		FString::Printf(TEXT("{\"source_file\":%s,\"destination_path\":\"/Game/__UeAgentInterfaceSmoke\"}"), *JsonString(JsonPath(FPaths::Combine(SavedRoot, TEXT("missing.vdb"))))), TEXT("source_file_not_found")));
	TestTrue(TEXT("sparse_volume_texture_reimport failure path"), ExecTextureCommandExpectFailure(this, ServerScope, RootId + TEXT("_svt_reimport_fail"), TEXT("sparse_volume_texture_reimport"),
		TEXT("{\"asset_path\":\"/Game/__UeAgentInterfaceSmoke/MissingSVT\"}"), TEXT("sparse_volume_texture_not_found")));
	TestTrue(TEXT("sparse_volume_texture_get_info failure path"), ExecTextureCommandExpectFailure(this, ServerScope, RootId + TEXT("_svt_info_fail"), TEXT("sparse_volume_texture_get_info"),
		TEXT("{\"asset_path\":\"/Game/__UeAgentInterfaceSmoke/MissingSVT\"}"), TEXT("sparse_volume_texture_not_found")));
	TestTrue(TEXT("sparse_volume_texture_export_json failure path"), ExecTextureCommandExpectFailure(this, ServerScope, RootId + TEXT("_svt_export_fail"), TEXT("sparse_volume_texture_export_json"),
		FString::Printf(TEXT("{\"asset_path\":\"/Game/__UeAgentInterfaceSmoke/MissingSVT\",\"output_file\":%s}"), *JsonString(JsonPath(FPaths::Combine(SavedRoot, TEXT("svt.json"))))), TEXT("sparse_volume_texture_not_found")));
	TestTrue(TEXT("sparse_volume_texture_apply_json failure path"), ExecTextureCommandExpectFailure(this, ServerScope, RootId + TEXT("_svt_apply_fail"), TEXT("sparse_volume_texture_apply_json"),
		FString::Printf(TEXT("{\"json_file\":%s,\"asset_path\":\"/Game/__UeAgentInterfaceSmoke/MissingSVT\"}"), *JsonString(JsonPath(FPaths::Combine(SavedRoot, TEXT("missing_svt.json"))))), TEXT("json_file_not_found")));
	TestTrue(TEXT("sparse_volume_texture_preview_probe failure path"), ExecTextureCommandExpectFailure(this, ServerScope, RootId + TEXT("_svt_preview_fail"), TEXT("sparse_volume_texture_preview_probe"),
		TEXT("{\"asset_path\":\"/Game/__UeAgentInterfaceSmoke/MissingSVT\"}"), TEXT("sparse_volume_texture_not_found")));
	TestTrue(TEXT("sparse_volume_texture_validate_setup failure path"), ExecTextureCommandExpectFailure(this, ServerScope, RootId + TEXT("_svt_validate_fail"), TEXT("sparse_volume_texture_validate_setup"),
		TEXT("{\"asset_path\":\"/Game/__UeAgentInterfaceSmoke/MissingSVT\"}"), TEXT("sparse_volume_texture_not_found")));
	TestTrue(TEXT("light_profile_texture_import failure path"), ExecTextureCommandExpectFailure(this, ServerScope, RootId + TEXT("_ies_import_fail"), TEXT("light_profile_texture_import"),
		FString::Printf(TEXT("{\"source_file\":%s,\"destination_path\":\"/Game/__UeAgentInterfaceSmoke\"}"), *JsonString(JsonPath(FPaths::Combine(SavedRoot, TEXT("missing.ies"))))), TEXT("source_file_not_found")));
	TestTrue(TEXT("light_profile_texture_get_info failure path"), ExecTextureCommandExpectFailure(this, ServerScope, RootId + TEXT("_ies_info_fail"), TEXT("light_profile_texture_get_info"),
		TEXT("{\"asset_path\":\"/Game/__UeAgentInterfaceSmoke/MissingIES\"}"), TEXT("texture_light_profile_not_found")));
	TestTrue(TEXT("light_profile_validate_setup failure path"), ExecTextureCommandExpectFailure(this, ServerScope, RootId + TEXT("_ies_validate_fail"), TEXT("light_profile_validate_setup"),
		TEXT("{\"expected_profile\":\"/Game/__UeAgentInterfaceSmoke/MissingIES\"}"), TEXT("texture_light_profile_not_found")));

	return !HasAnyErrors();
}

#endif

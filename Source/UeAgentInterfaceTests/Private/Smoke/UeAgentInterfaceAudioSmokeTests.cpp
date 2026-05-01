#if WITH_DEV_AUTOMATION_TESTS

#include "Server/UeAgentHttpServer.h"
#include "UeAgentInterfaceSettings.h"

#include "Components/AudioComponent.h"
#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/World.h"
#include "HAL/PlatformApplicationMisc.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HttpManager.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Misc/AutomationTest.h"
#include "Misc/CoreDelegates.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/Parse.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Sound/AmbientSound.h"
#include "Sound/AudioVolume.h"
#include "Sound/SoundSubmix.h"

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

	static bool SaveSmokeUtf8File(FAutomationTestBase* Test, const FString& FilePath, const FString& Text)
	{
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(FilePath), true);
		return Test->TestTrue(FString::Printf(TEXT("save smoke file %s"), *FilePath), FFileHelper::SaveStringToFile(Text, *FilePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));
	}

	static void AppendLe16(TArray<uint8>& Bytes, uint16 Value)
	{
		Bytes.Add(static_cast<uint8>(Value & 0xff));
		Bytes.Add(static_cast<uint8>((Value >> 8) & 0xff));
	}

	static void AppendLe32(TArray<uint8>& Bytes, uint32 Value)
	{
		Bytes.Add(static_cast<uint8>(Value & 0xff));
		Bytes.Add(static_cast<uint8>((Value >> 8) & 0xff));
		Bytes.Add(static_cast<uint8>((Value >> 16) & 0xff));
		Bytes.Add(static_cast<uint8>((Value >> 24) & 0xff));
	}

	static void AppendAscii(TArray<uint8>& Bytes, const ANSICHAR* Text)
	{
		while (*Text)
		{
			Bytes.Add(static_cast<uint8>(*Text++));
		}
	}

	static bool SaveToneWav(FAutomationTestBase* Test, const FString& FilePath)
	{
		const uint32 SampleRate = 44100;
		const uint16 Channels = 1;
		const uint16 BitsPerSample = 16;
		const float DurationSeconds = 0.25f;
		const int32 SampleCount = static_cast<int32>(SampleRate * DurationSeconds);
		TArray<uint8> Pcm;
		Pcm.Reserve(SampleCount * 2);
		for (int32 Index = 0; Index < SampleCount; ++Index)
		{
			const float T = static_cast<float>(Index) / static_cast<float>(SampleRate);
			const int16 Sample = static_cast<int16>(FMath::Sin(2.0f * PI * 440.0f * T) * 16000.0f);
			AppendLe16(Pcm, static_cast<uint16>(Sample));
		}

		TArray<uint8> Bytes;
		AppendAscii(Bytes, "RIFF");
		AppendLe32(Bytes, 36 + Pcm.Num());
		AppendAscii(Bytes, "WAVE");
		AppendAscii(Bytes, "fmt ");
		AppendLe32(Bytes, 16);
		AppendLe16(Bytes, 1);
		AppendLe16(Bytes, Channels);
		AppendLe32(Bytes, SampleRate);
		AppendLe32(Bytes, SampleRate * Channels * BitsPerSample / 8);
		AppendLe16(Bytes, Channels * BitsPerSample / 8);
		AppendLe16(Bytes, BitsPerSample);
		AppendAscii(Bytes, "data");
		AppendLe32(Bytes, Pcm.Num());
		Bytes.Append(Pcm);

		IFileManager::Get().MakeDirectory(*FPaths::GetPath(FilePath), true);
		return Test->TestTrue(TEXT("save generated wav"), FFileHelper::SaveArrayToFile(Bytes, *FilePath));
	}

	static bool ExecAudioCommand(
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

	static bool ExecAudioCommandAllowUnavailable(
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
		TSharedPtr<FJsonObject> Root;
		if (!Test->TestTrue(Command + TEXT(" response parses"), ParseJson(Result.Body, Root)) || !Root.IsValid())
		{
			return false;
		}
		bool bOk = false;
		Root->TryGetBoolField(TEXT("ok"), bOk);
		const TSharedPtr<FJsonObject>* Data = nullptr;
		OutData = (Root->TryGetObjectField(TEXT("data"), Data) && Data && Data->IsValid()) ? *Data : MakeShared<FJsonObject>();
		if (bOk)
		{
			return true;
		}
		FString Error;
		Root->TryGetStringField(TEXT("error"), Error);
		const bool bAllowed = Error.Contains(TEXT("audio_device_unavailable"));
		if (!Test->TestTrue(Command + TEXT(" allowed explicit audio unavailable"), bAllowed))
		{
			Test->AddError(Result.Body.Left(2048));
			return false;
		}
		return true;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeAgentInterfaceAudioSmokeTest,
	"GptProjectTest.UeAgentInterface.Smoke.AudioCommands",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FUeAgentInterfaceAudioSmokeTest::RunTest(const FString& Parameters)
{
	FScopedUeAgentHttpServer ServerScope;
	FString InitError;
	if (!ServerScope.Initialize(InitError))
	{
		AddError(FString::Printf(TEXT("Initialize failed: %s"), *InitError));
		return false;
	}

	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!TestNotNull(TEXT("Editor world"), World))
	{
		return false;
	}

	const FString RootId = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString SavedRoot = JsonPath(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UeAgentInterfaceAudioSmoke"), RootId));
	const FString SourceWav = JsonPath(FPaths::Combine(SavedRoot, TEXT("tone.wav")));
	if (!SaveToneWav(this, SourceWav))
	{
		return false;
	}

	const FString Wave = MakeAutomationAssetPath(TEXT("SW_Audio"));
	const FString WaveDestPath = FPackageName::GetLongPackagePath(Wave);
	const FString WaveDestName = FPackageName::GetLongPackageAssetName(Wave);
	const FString Cue = MakeAutomationAssetPath(TEXT("SC_Audio"));
	const FString CueFolder = JsonPath(FPaths::Combine(SavedRoot, TEXT("cue")));
	const FString CueExportFolder = JsonPath(FPaths::Combine(SavedRoot, TEXT("cue_export")));
	const FString MetaSound = MakeAutomationAssetPath(TEXT("MS_Audio"));
	const FString MetaSoundFolder = JsonPath(FPaths::Combine(SavedRoot, TEXT("metasound")));
	const FString Attenuation = MakeAutomationAssetPath(TEXT("ATT_Audio"));
	const FString Concurrency = MakeAutomationAssetPath(TEXT("CON_Audio"));
	const FString SoundClass = MakeAutomationAssetPath(TEXT("CLS_Audio"));
	const FString SoundMix = MakeAutomationAssetPath(TEXT("MIX_Audio"));
	const FString Submix = MakeAutomationAssetPath(TEXT("SUB_Audio"));
	const FString EffectPreset = MakeAutomationAssetPath(TEXT("FX_Audio"));
	const FString AttSemanticJson = JsonPath(FPaths::Combine(SavedRoot, TEXT("attenuation_semantic.json")));
	const FString AttJson = JsonPath(FPaths::Combine(SavedRoot, TEXT("attenuation.json")));
	const FString ConJson = JsonPath(FPaths::Combine(SavedRoot, TEXT("concurrency.json")));
	const FString ClassJson = JsonPath(FPaths::Combine(SavedRoot, TEXT("class.json")));
	const FString MixJson = JsonPath(FPaths::Combine(SavedRoot, TEXT("mix.json")));
	const FString EffectJson = JsonPath(FPaths::Combine(SavedRoot, TEXT("effect.json")));
	const FString SubmixFolder = JsonPath(FPaths::Combine(SavedRoot, TEXT("submix")));

	TSharedPtr<FJsonObject> Data;
	TestTrue(TEXT("sound_wave_import"), ExecAudioCommand(this, ServerScope, RootId + TEXT("_sw_import"), TEXT("sound_wave_import"),
		FString::Printf(TEXT("{\"source_file\":%s,\"destination_path\":%s,\"destination_name\":%s,\"replace_existing\":true,\"save_after_import\":true}"), *JsonString(SourceWav), *JsonString(WaveDestPath), *JsonString(WaveDestName)), Data));
	TestTrue(TEXT("sound_wave_get_info"), ExecAudioCommand(this, ServerScope, RootId + TEXT("_sw_info"), TEXT("sound_wave_get_info"),
		FString::Printf(TEXT("{\"asset_path\":%s,\"include_cue_points\":true,\"include_analysis\":true}"), *JsonString(Wave)), Data));
	TestTrue(TEXT("sound_wave_validate_setup"), ExecAudioCommand(this, ServerScope, RootId + TEXT("_sw_validate"), TEXT("sound_wave_validate_setup"),
		FString::Printf(TEXT("{\"asset_path\":%s}"), *JsonString(Wave)), Data));
	TestTrue(TEXT("sound_wave_preview"), ExecAudioCommandAllowUnavailable(this, ServerScope, RootId + TEXT("_sw_preview"), TEXT("sound_wave_preview"),
		FString::Printf(TEXT("{\"asset_path\":%s,\"duration\":0.05,\"expected_min_peak\":0.01}"), *JsonString(Wave)), Data));
	TestTrue(TEXT("sound_wave_reimport"), ExecAudioCommand(this, ServerScope, RootId + TEXT("_sw_reimport"), TEXT("sound_wave_reimport"),
		FString::Printf(TEXT("{\"asset_path\":%s,\"source_file\":%s,\"save_after_reimport\":true}"), *JsonString(Wave), *JsonString(SourceWav)), Data));

	TestTrue(TEXT("sound_cue_create"), ExecAudioCommand(this, ServerScope, RootId + TEXT("_cue_create"), TEXT("sound_cue_create"),
		FString::Printf(TEXT("{\"asset_path\":%s,\"save_after_create\":true}"), *JsonString(Cue)), Data));
	SaveSmokeUtf8File(this, FPaths::Combine(CueFolder, TEXT("asset.json")), FString::Printf(TEXT("{\"profile\":\"ue_agent_interface.sound_cue.v1\",\"schema_version\":1,\"asset_path\":%s}"), *JsonString(Cue)));
	SaveSmokeUtf8File(this, FPaths::Combine(CueFolder, TEXT("nodes.json")), FString::Printf(TEXT("{\"root_node_id\":\"root\",\"nodes\":[{\"id\":\"root\",\"type\":\"wave_player\",\"sound_wave\":%s,\"looping\":false}]}"), *JsonString(Wave)));
	SaveSmokeUtf8File(this, FPaths::Combine(CueFolder, TEXT("edges.json")), TEXT("{\"edges\":[]}"));
	TestTrue(TEXT("sound_cue_validate_folder"), ExecAudioCommand(this, ServerScope, RootId + TEXT("_cue_validate"), TEXT("sound_cue_validate_folder"),
		FString::Printf(TEXT("{\"folder_path\":%s}"), *JsonString(CueFolder)), Data));
	TestTrue(TEXT("sound_cue_apply_folder"), ExecAudioCommand(this, ServerScope, RootId + TEXT("_cue_apply"), TEXT("sound_cue_apply_folder"),
		FString::Printf(TEXT("{\"folder_path\":%s,\"asset_path\":%s,\"allow_destructive\":true,\"save_after_apply\":true}"), *JsonString(CueFolder), *JsonString(Cue)), Data));
	TestTrue(TEXT("sound_cue_get_info"), ExecAudioCommand(this, ServerScope, RootId + TEXT("_cue_info"), TEXT("sound_cue_get_info"),
		FString::Printf(TEXT("{\"asset_path\":%s,\"include_nodes\":true,\"include_validation\":true}"), *JsonString(Cue)), Data));
	TestTrue(TEXT("sound_cue_export_folder"), ExecAudioCommand(this, ServerScope, RootId + TEXT("_cue_export"), TEXT("sound_cue_export_folder"),
		FString::Printf(TEXT("{\"asset_path\":%s,\"folder_path\":%s,\"clean_output_dir\":true,\"include_raw_properties\":true}"), *JsonString(Cue), *JsonString(CueExportFolder)), Data));
	TestTrue(TEXT("sound_cue_open_editor"), ExecAudioCommand(this, ServerScope, RootId + TEXT("_cue_open"), TEXT("sound_cue_open_editor"),
		FString::Printf(TEXT("{\"asset_path\":%s,\"bring_to_front\":false}"), *JsonString(Cue)), Data));
	TestTrue(TEXT("sound_cue_screenshot"), ExecAudioCommand(this, ServerScope, RootId + TEXT("_cue_shot"), TEXT("sound_cue_screenshot"),
		FString::Printf(TEXT("{\"asset_path\":%s,\"format\":\"png\",\"max_size\":128,\"require_non_black\":true}"), *JsonString(Cue)), Data));
	TestTrue(TEXT("sound_cue_preview"), ExecAudioCommandAllowUnavailable(this, ServerScope, RootId + TEXT("_cue_preview"), TEXT("sound_cue_preview"),
		FString::Printf(TEXT("{\"asset_path\":%s,\"duration\":0.05,\"expected_min_peak\":0.01}"), *JsonString(Cue)), Data));

	TestTrue(TEXT("metasound_create"), ExecAudioCommand(this, ServerScope, RootId + TEXT("_ms_create"), TEXT("metasound_create"),
		FString::Printf(TEXT("{\"asset_path\":%s,\"asset_type\":\"source\",\"save_after_create\":true}"), *JsonString(MetaSound)), Data));
	TestTrue(TEXT("metasound_get_info"), ExecAudioCommand(this, ServerScope, RootId + TEXT("_ms_info"), TEXT("metasound_get_info"),
		FString::Printf(TEXT("{\"asset_path\":%s,\"include_graph\":true,\"include_interfaces\":true,\"include_validation\":true}"), *JsonString(MetaSound)), Data));
	TestTrue(TEXT("metasound_export_folder"), ExecAudioCommand(this, ServerScope, RootId + TEXT("_ms_export"), TEXT("metasound_export_folder"),
		FString::Printf(TEXT("{\"asset_path\":%s,\"folder_path\":%s,\"clean_output_dir\":true,\"include_raw_properties\":true}"), *JsonString(MetaSound), *JsonString(MetaSoundFolder)), Data));
	TestTrue(TEXT("metasound_validate_folder"), ExecAudioCommand(this, ServerScope, RootId + TEXT("_ms_validate"), TEXT("metasound_validate_folder"),
		FString::Printf(TEXT("{\"folder_path\":%s}"), *JsonString(MetaSoundFolder)), Data));
	TestTrue(TEXT("metasound_apply_folder"), ExecAudioCommand(this, ServerScope, RootId + TEXT("_ms_apply"), TEXT("metasound_apply_folder"),
		FString::Printf(TEXT("{\"folder_path\":%s,\"asset_path\":%s,\"create_if_missing\":true,\"save_after_apply\":true,\"compile_after_apply\":true}"), *JsonString(MetaSoundFolder), *JsonString(MetaSound)), Data));
	TestTrue(TEXT("metasound_compile"), ExecAudioCommand(this, ServerScope, RootId + TEXT("_ms_compile"), TEXT("metasound_compile"),
		FString::Printf(TEXT("{\"asset_path\":%s,\"include_messages\":true,\"save_after_compile\":true}"), *JsonString(MetaSound)), Data));
	TestTrue(TEXT("metasound_open_editor"), ExecAudioCommand(this, ServerScope, RootId + TEXT("_ms_open"), TEXT("metasound_open_editor"),
		FString::Printf(TEXT("{\"asset_path\":%s,\"bring_to_front\":false}"), *JsonString(MetaSound)), Data));
	TestTrue(TEXT("metasound_screenshot"), ExecAudioCommand(this, ServerScope, RootId + TEXT("_ms_shot"), TEXT("metasound_screenshot"),
		FString::Printf(TEXT("{\"asset_path\":%s,\"format\":\"png\",\"max_size\":128,\"require_non_black\":true}"), *JsonString(MetaSound)), Data));
	TestTrue(TEXT("metasound_preview"), ExecAudioCommandAllowUnavailable(this, ServerScope, RootId + TEXT("_ms_preview"), TEXT("metasound_preview"),
		FString::Printf(TEXT("{\"asset_path\":%s,\"duration\":0.05}"), *JsonString(MetaSound)), Data));

	TestTrue(TEXT("sound_attenuation_create"), ExecAudioCommand(this, ServerScope, RootId + TEXT("_att_create"), TEXT("sound_attenuation_create"),
		FString::Printf(TEXT("{\"asset_path\":%s,\"save_after_create\":true}"), *JsonString(Attenuation)), Data));
	SaveSmokeUtf8File(this, AttSemanticJson, FString::Printf(TEXT("{\"profile\":\"ue_agent_interface.sound_attenuation.v1\",\"schema_version\":1,\"asset_path\":%s,\"attenuation\":{\"attenuate\":true,\"shape\":\"Sphere\",\"inner_radius\":220.0,\"falloff_distance\":1400.0,\"distance_algorithm\":\"NaturalSound\"},\"spatialization\":{\"spatialize\":true,\"algorithm\":\"SPATIALIZATION_Default\",\"stereo_spread\":0.0},\"occlusion\":{\"enabled\":true,\"trace_channel\":\"ECC_Visibility\",\"low_pass_filter_frequency\":1800.0,\"volume_attenuation\":0.65,\"interpolation_time\":0.15}}"), *JsonString(Attenuation)));
	TestTrue(TEXT("sound_attenuation_apply_json semantic"), ExecAudioCommand(this, ServerScope, RootId + TEXT("_att_apply_semantic"), TEXT("sound_attenuation_apply_json"),
		FString::Printf(TEXT("{\"json_file\":%s,\"asset_path\":%s,\"save_after_apply\":true}"), *JsonString(AttSemanticJson), *JsonString(Attenuation)), Data));
	TestTrue(TEXT("sound_attenuation_get_info"), ExecAudioCommand(this, ServerScope, RootId + TEXT("_att_info"), TEXT("sound_attenuation_get_info"),
		FString::Printf(TEXT("{\"asset_path\":%s,\"include_raw_properties\":true}"), *JsonString(Attenuation)), Data));
	TestTrue(TEXT("sound_attenuation_export_json"), ExecAudioCommand(this, ServerScope, RootId + TEXT("_att_export"), TEXT("sound_attenuation_export_json"),
		FString::Printf(TEXT("{\"asset_path\":%s,\"output_file\":%s,\"include_validation\":true}"), *JsonString(Attenuation), *JsonString(AttJson)), Data));
	TestTrue(TEXT("sound_attenuation_validate_json"), ExecAudioCommand(this, ServerScope, RootId + TEXT("_att_validate"), TEXT("sound_attenuation_validate_json"),
		FString::Printf(TEXT("{\"json_file\":%s}"), *JsonString(AttJson)), Data));
	TestTrue(TEXT("sound_attenuation_apply_json"), ExecAudioCommand(this, ServerScope, RootId + TEXT("_att_apply"), TEXT("sound_attenuation_apply_json"),
		FString::Printf(TEXT("{\"json_file\":%s,\"asset_path\":%s,\"save_after_apply\":true}"), *JsonString(AttJson), *JsonString(Attenuation)), Data));

	TestTrue(TEXT("sound_concurrency_create"), ExecAudioCommand(this, ServerScope, RootId + TEXT("_con_create"), TEXT("sound_concurrency_create"),
		FString::Printf(TEXT("{\"asset_path\":%s,\"max_count\":2,\"save_after_create\":true}"), *JsonString(Concurrency)), Data));
	TestTrue(TEXT("sound_concurrency_get_info"), ExecAudioCommand(this, ServerScope, RootId + TEXT("_con_info"), TEXT("sound_concurrency_get_info"),
		FString::Printf(TEXT("{\"asset_path\":%s,\"include_raw_properties\":true}"), *JsonString(Concurrency)), Data));
	TestTrue(TEXT("sound_concurrency_export_json"), ExecAudioCommand(this, ServerScope, RootId + TEXT("_con_export"), TEXT("sound_concurrency_export_json"),
		FString::Printf(TEXT("{\"asset_path\":%s,\"output_file\":%s}"), *JsonString(Concurrency), *JsonString(ConJson)), Data));
	TestTrue(TEXT("sound_concurrency_validate_json"), ExecAudioCommand(this, ServerScope, RootId + TEXT("_con_validate"), TEXT("sound_concurrency_validate_json"),
		FString::Printf(TEXT("{\"json_file\":%s}"), *JsonString(ConJson)), Data));
	TestTrue(TEXT("sound_concurrency_apply_json"), ExecAudioCommand(this, ServerScope, RootId + TEXT("_con_apply"), TEXT("sound_concurrency_apply_json"),
		FString::Printf(TEXT("{\"json_file\":%s,\"asset_path\":%s,\"save_after_apply\":true}"), *JsonString(ConJson), *JsonString(Concurrency)), Data));

	TestTrue(TEXT("sound_class_create"), ExecAudioCommand(this, ServerScope, RootId + TEXT("_class_create"), TEXT("sound_class_create"),
		FString::Printf(TEXT("{\"asset_path\":%s,\"save_after_create\":true}"), *JsonString(SoundClass)), Data));
	TestTrue(TEXT("sound_class_get_info"), ExecAudioCommand(this, ServerScope, RootId + TEXT("_class_info"), TEXT("sound_class_get_info"),
		FString::Printf(TEXT("{\"asset_path\":%s,\"include_children\":true,\"include_raw_properties\":true}"), *JsonString(SoundClass)), Data));
	TestTrue(TEXT("sound_class_export_json"), ExecAudioCommand(this, ServerScope, RootId + TEXT("_class_export"), TEXT("sound_class_export_json"),
		FString::Printf(TEXT("{\"asset_path\":%s,\"output_file\":%s}"), *JsonString(SoundClass), *JsonString(ClassJson)), Data));
	TestTrue(TEXT("sound_class_validate_json"), ExecAudioCommand(this, ServerScope, RootId + TEXT("_class_validate"), TEXT("sound_class_validate_json"),
		FString::Printf(TEXT("{\"json_file\":%s}"), *JsonString(ClassJson)), Data));
	TestTrue(TEXT("sound_class_apply_json"), ExecAudioCommand(this, ServerScope, RootId + TEXT("_class_apply"), TEXT("sound_class_apply_json"),
		FString::Printf(TEXT("{\"json_file\":%s,\"asset_path\":%s,\"save_after_apply\":true}"), *JsonString(ClassJson), *JsonString(SoundClass)), Data));

	TestTrue(TEXT("sound_mix_create"), ExecAudioCommand(this, ServerScope, RootId + TEXT("_mix_create"), TEXT("sound_mix_create"),
		FString::Printf(TEXT("{\"asset_path\":%s,\"save_after_create\":true}"), *JsonString(SoundMix)), Data));
	TestTrue(TEXT("sound_mix_export_json"), ExecAudioCommand(this, ServerScope, RootId + TEXT("_mix_export"), TEXT("sound_mix_export_json"),
		FString::Printf(TEXT("{\"asset_path\":%s,\"output_file\":%s}"), *JsonString(SoundMix), *JsonString(MixJson)), Data));
	TestTrue(TEXT("sound_mix_apply_json"), ExecAudioCommand(this, ServerScope, RootId + TEXT("_mix_apply"), TEXT("sound_mix_apply_json"),
		FString::Printf(TEXT("{\"json_file\":%s,\"asset_path\":%s,\"save_after_apply\":true}"), *JsonString(MixJson), *JsonString(SoundMix)), Data));

	TestTrue(TEXT("sound_submix_create"), ExecAudioCommand(this, ServerScope, RootId + TEXT("_sub_create"), TEXT("sound_submix_create"),
		FString::Printf(TEXT("{\"asset_path\":%s,\"save_after_create\":true}"), *JsonString(Submix)), Data));
	TestTrue(TEXT("sound_submix_get_info"), ExecAudioCommand(this, ServerScope, RootId + TEXT("_sub_info"), TEXT("sound_submix_get_info"),
		FString::Printf(TEXT("{\"asset_path\":%s,\"include_children\":true,\"include_effect_chain\":true}"), *JsonString(Submix)), Data));
	TestTrue(TEXT("sound_submix_export_folder"), ExecAudioCommand(this, ServerScope, RootId + TEXT("_sub_export"), TEXT("sound_submix_export_folder"),
		FString::Printf(TEXT("{\"asset_path\":%s,\"folder_path\":%s,\"clean_output_dir\":true,\"include_raw_properties\":true}"), *JsonString(Submix), *JsonString(SubmixFolder)), Data));
	TestTrue(TEXT("sound_submix_validate_folder"), ExecAudioCommand(this, ServerScope, RootId + TEXT("_sub_validate"), TEXT("sound_submix_validate_folder"),
		FString::Printf(TEXT("{\"folder_path\":%s}"), *JsonString(SubmixFolder)), Data));
	TestTrue(TEXT("sound_submix_apply_folder"), ExecAudioCommand(this, ServerScope, RootId + TEXT("_sub_apply"), TEXT("sound_submix_apply_folder"),
		FString::Printf(TEXT("{\"folder_path\":%s,\"asset_path\":%s,\"save_after_apply\":true}"), *JsonString(SubmixFolder), *JsonString(Submix)), Data));

	TestTrue(TEXT("audio_effect_preset_create"), ExecAudioCommand(this, ServerScope, RootId + TEXT("_fx_create"), TEXT("audio_effect_preset_create"),
		FString::Printf(TEXT("{\"asset_path\":%s,\"preset_class\":\"/Script/AudioMixer.SubmixEffectDynamicsProcessorPreset\",\"save_after_create\":true}"), *JsonString(EffectPreset)), Data));
	TestTrue(TEXT("audio_effect_preset_export_json"), ExecAudioCommand(this, ServerScope, RootId + TEXT("_fx_export"), TEXT("audio_effect_preset_export_json"),
		FString::Printf(TEXT("{\"asset_path\":%s,\"output_file\":%s}"), *JsonString(EffectPreset), *JsonString(EffectJson)), Data));
	TestTrue(TEXT("audio_effect_preset_apply_json"), ExecAudioCommand(this, ServerScope, RootId + TEXT("_fx_apply"), TEXT("audio_effect_preset_apply_json"),
		FString::Printf(TEXT("{\"json_file\":%s,\"asset_path\":%s,\"save_after_apply\":true}"), *JsonString(EffectJson), *JsonString(EffectPreset)), Data));

	FActorSpawnParameters SpawnParams;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AActor* AudioActor = World->SpawnActor<AActor>(AActor::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator, SpawnParams);
	TestNotNull(TEXT("Audio actor"), AudioActor);
	AudioActor->SetActorLabel(TEXT("UAI_AudioSmokeActor_") + RootId.Left(8));
	UAudioComponent* AudioComponent = NewObject<UAudioComponent>(AudioActor, TEXT("UAI_AudioSmokeComponent"));
	AudioComponent->SetSound(LoadObject<USoundBase>(nullptr, *FString::Printf(TEXT("%s.%s"), *Wave, *FPackageName::GetLongPackageAssetName(Wave))));
	AudioComponent->RegisterComponent();
	AudioActor->AddInstanceComponent(AudioComponent);

	AAudioVolume* AudioVolume = World->SpawnActor<AAudioVolume>(AAudioVolume::StaticClass(), FVector(100, 0, 0), FRotator::ZeroRotator, SpawnParams);
	TestNotNull(TEXT("Audio volume"), AudioVolume);
	AudioVolume->SetActorLabel(TEXT("UAI_AudioSmokeVolume_") + RootId.Left(8));
	AAmbientSound* AmbientSound = World->SpawnActor<AAmbientSound>(AAmbientSound::StaticClass(), FVector(200, 0, 0), FRotator::ZeroRotator, SpawnParams);
	TestNotNull(TEXT("Ambient sound"), AmbientSound);
	AmbientSound->SetActorLabel(TEXT("UAI_AudioSmokeAmbient_") + RootId.Left(8));
	AmbientSound->GetAudioComponent()->SetSound(AudioComponent->Sound);

	TestTrue(TEXT("audio_component_validate_setup"), ExecAudioCommand(this, ServerScope, RootId + TEXT("_comp_validate"), TEXT("audio_component_validate_setup"),
		FString::Printf(TEXT("{\"actor\":%s,\"component_name\":\"UAI_AudioSmokeComponent\",\"expected_sound\":%s}"), *JsonString(AudioActor->GetActorLabel()), *JsonString(Wave)), Data));
	TestTrue(TEXT("audio_component_runtime_snapshot"), ExecAudioCommand(this, ServerScope, RootId + TEXT("_comp_snap"), TEXT("audio_component_runtime_snapshot"),
		FString::Printf(TEXT("{\"actor\":%s,\"component_name\":\"UAI_AudioSmokeComponent\"}"), *JsonString(AudioActor->GetActorLabel())), Data));
	TestTrue(TEXT("audio_component_runtime_probe"), ExecAudioCommandAllowUnavailable(this, ServerScope, RootId + TEXT("_comp_probe"), TEXT("audio_component_runtime_probe"),
		FString::Printf(TEXT("{\"actor\":%s,\"component_name\":\"UAI_AudioSmokeComponent\",\"duration\":0.05}"), *JsonString(AudioActor->GetActorLabel())), Data));
	TestTrue(TEXT("audio_volume_validate_setup"), ExecAudioCommand(this, ServerScope, RootId + TEXT("_vol_validate"), TEXT("audio_volume_validate_setup"),
		FString::Printf(TEXT("{\"actor\":%s}"), *JsonString(AudioVolume->GetActorLabel())), Data));
	TestTrue(TEXT("ambient_sound_validate_setup"), ExecAudioCommand(this, ServerScope, RootId + TEXT("_amb_validate"), TEXT("ambient_sound_validate_setup"),
		FString::Printf(TEXT("{\"actor\":%s}"), *JsonString(AmbientSound->GetActorLabel())), Data));
	TestTrue(TEXT("audio_runtime_snapshot"), ExecAudioCommand(this, ServerScope, RootId + TEXT("_runtime_snap"), TEXT("audio_runtime_snapshot"), TEXT("{}"), Data));
	TestTrue(TEXT("audio_runtime_probe"), ExecAudioCommandAllowUnavailable(this, ServerScope, RootId + TEXT("_runtime_probe"), TEXT("audio_runtime_probe"),
		FString::Printf(TEXT("{\"sound\":%s,\"duration\":0.05,\"expected_min_peak\":0.01}"), *JsonString(Wave)), Data));
	TestTrue(TEXT("audio_submix_meter_probe"), ExecAudioCommandAllowUnavailable(this, ServerScope, RootId + TEXT("_meter_probe"), TEXT("audio_submix_meter_probe"),
		FString::Printf(TEXT("{\"submix\":%s,\"duration\":0.05,\"frequencies\":[100,1000,4000]}"), *JsonString(Submix)), Data));
	if (ExecAudioCommandAllowUnavailable(this, ServerScope, RootId + TEXT("_record_start"), TEXT("audio_submix_record_start"),
		FString::Printf(TEXT("{\"session_id\":%s,\"submix\":%s,\"expected_duration\":0.1,\"name\":\"uai_audio_smoke\",\"path\":%s}"), *JsonString(RootId + TEXT("_rec")), *JsonString(Submix), *JsonString(SavedRoot)), Data))
	{
		ExecAudioCommandAllowUnavailable(this, ServerScope, RootId + TEXT("_record_stop"), TEXT("audio_submix_record_stop"),
			FString::Printf(TEXT("{\"session_id\":%s,\"export_type\":\"wav_file\"}"), *JsonString(RootId + TEXT("_rec"))), Data);
	}
	if (ExecAudioCommandAllowUnavailable(this, ServerScope, RootId + TEXT("_record_cancel_start"), TEXT("audio_submix_record_start"),
		FString::Printf(TEXT("{\"session_id\":%s,\"submix\":%s,\"expected_duration\":0.1}"), *JsonString(RootId + TEXT("_rec_cancel")), *JsonString(Submix)), Data))
	{
		TestTrue(TEXT("audio_submix_record_cancel"), ExecAudioCommandAllowUnavailable(this, ServerScope, RootId + TEXT("_record_cancel"), TEXT("audio_submix_record_cancel"),
			FString::Printf(TEXT("{\"session_id\":%s}"), *JsonString(RootId + TEXT("_rec_cancel"))), Data));
	}
	TestTrue(TEXT("audio_stop_all_preview_sounds"), ExecAudioCommand(this, ServerScope, RootId + TEXT("_stop_all"), TEXT("audio_stop_all_preview_sounds"), TEXT("{}"), Data));

	return true;
}

#endif

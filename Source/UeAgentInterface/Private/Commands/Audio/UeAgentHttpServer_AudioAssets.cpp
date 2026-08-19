// Copyright Epic Games, Inc. All Rights Reserved.

#include "Server/UeAgentHttpServer.h"
#include "Commands/Audio/UeAgentAudioFolderCommandBridge.h"
#include "Commands/Audio/UeAgentHttpServer_AudioOps.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Sound/SoundAttenuation.h"
#include "Sound/SoundClass.h"
#include "Sound/SoundConcurrency.h"
#include "Sound/SoundEffectBase.h"
#include "Sound/SoundEffectSource.h"
#include "Sound/SoundEffectSubmix.h"
#include "Sound/SoundMix.h"
#include "Sound/SoundSubmix.h"
#include "Sound/SoundSubmixSend.h"
#include "SubmixEffects/AudioMixerSubmixEffectDynamicsProcessor.h"

bool FUeAgentHttpServer::CmdSoundAttenuationCreate(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentAudioOps;
	FString AssetPath;
	if (!GetRequiredString(Ctx, TEXT("asset_path"), AssetPath, OutError))
	{
		return false;
	}
	bool bReplace = false;
	bool bSave = true;
	GetOptionalBool(Ctx.Params, TEXT("replace_existing"), bReplace, OutError);
	GetOptionalBool(Ctx.Params, TEXT("save_after_create"), bSave, OutError);
	USoundAttenuation* Asset = Cast<USoundAttenuation>(CreateAssetObject(USoundAttenuation::StaticClass(), AssetPath, bReplace, OutError));
	if (!Asset)
	{
		return false;
	}
	const TSharedPtr<FJsonObject>* Template = nullptr;
	if (Ctx.Params->TryGetObjectField(TEXT("template"), Template) && Template && Template->IsValid())
	{
		TArray<TSharedPtr<FJsonValue>> Results;
		FString Error;
		if (!ApplySoundAttenuationSemanticJson(Asset, *Template, Results, false, Error)
			|| !ApplyPropertiesArray(Asset, *Template, Results, false, Error))
		{
			OutError = Error;
			return false;
		}
	}
	if (bSave && !SavePackagesForObjects({ Asset }, OutError))
	{
		return false;
	}
	OutData = BuildSoundAttenuationInfo(Asset, true);
	OutData->SetBoolField(TEXT("created_asset"), true);
	OutData->SetObjectField(TEXT("coverage_report"), MakeCoverageReport(SoundAttenuationProfile));
	return true;
}
bool FUeAgentHttpServer::CmdSoundAttenuationGetInfo(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentAudioOps;
	FString AssetPath;
	if (!GetRequiredString(Ctx, TEXT("asset_path"), AssetPath, OutError))
	{
		return false;
	}
	USoundAttenuation* Asset = Cast<USoundAttenuation>(LoadAssetObject(AssetPath));
	if (!Asset)
	{
		OutError = TEXT("sound_attenuation_not_found");
		return false;
	}
	bool bIncludeRaw = false;
	GetOptionalBool(Ctx.Params, TEXT("include_raw_properties"), bIncludeRaw, OutError);
	OutData = BuildSoundAttenuationInfo(Asset, bIncludeRaw);
	return true;
}
bool FUeAgentHttpServer::CmdSoundAttenuationExportJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentAudioOps;
	FString AssetPath;
	FString OutputFile;
	if (!GetRequiredString(Ctx, TEXT("asset_path"), AssetPath, OutError) || !GetRequiredString(Ctx, TEXT("output_file"), OutputFile, OutError))
	{
		return false;
	}
	USoundAttenuation* Asset = Cast<USoundAttenuation>(LoadAssetObject(AssetPath));
	if (!Asset)
	{
		OutError = TEXT("sound_attenuation_not_found");
		return false;
	}
	TSharedPtr<FJsonObject> Root = BuildGenericAssetJson(Asset, SoundAttenuationProfile, true);
	Root->SetObjectField(TEXT("summary"), BuildSoundAttenuationInfo(Asset, false));
	Root->SetObjectField(TEXT("attenuation"), BuildSoundAttenuationInfo(Asset, false));
	FString SavedPath;
	if (!SaveAuthoringFile(OutputFile, Root, SavedPath, OutError))
	{
		return false;
	}
	OutData = Root;
	OutData->SetStringField(TEXT("output_file"), SavedPath);
	return true;
}
bool FUeAgentHttpServer::CmdSoundAttenuationValidateJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentAudioOps;
	FString JsonFile;
	if (!GetRequiredString(Ctx, TEXT("json_file"), JsonFile, OutError))
	{
		return false;
	}
	TSharedPtr<FJsonObject> Root;
	FString ResolvedPath;
	if (!LoadAuthoringFile(JsonFile, Root, ResolvedPath, OutError))
	{
		return false;
	}
	TArray<TSharedPtr<FJsonValue>> Issues;
	const TSharedPtr<FJsonObject>* Summary = nullptr;
	if (Root->TryGetObjectField(TEXT("attenuation"), Summary) && Summary && Summary->IsValid())
	{
		double Falloff = 0.0;
		if ((*Summary)->TryGetNumberField(TEXT("falloff_distance"), Falloff) && Falloff < 0.0)
		{
			AddIssue(Issues, TEXT("falloff_distance_negative"), TEXT("error"), TEXT("falloff_distance must be non-negative."), TEXT("attenuation.falloff_distance"));
		}
	}
	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("json_file"), ResolvedPath);
	OutData->SetStringField(TEXT("status"), Issues.Num() == 0 ? TEXT("ok") : TEXT("failed"));
	OutData->SetArrayField(TEXT("json_issues"), Issues);
	OutData->SetObjectField(TEXT("coverage_report"), MakeCoverageReport(SoundAttenuationProfile));
	return true;
}
bool FUeAgentHttpServer::CmdSoundAttenuationApplyJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentAudioOps;
	FString JsonFile;
	if (!GetRequiredString(Ctx, TEXT("json_file"), JsonFile, OutError))
	{
		return false;
	}
	TSharedPtr<FJsonObject> Root;
	FString ResolvedPath;
	if (!LoadAuthoringFile(JsonFile, Root, ResolvedPath, OutError))
	{
		return false;
	}
	FString AssetPath;
	Ctx.Params->TryGetStringField(TEXT("asset_path"), AssetPath);
	if (AssetPath.IsEmpty())
	{
		Root->TryGetStringField(TEXT("asset_path"), AssetPath);
	}
	bool bCreateIfMissing = true;
	bool bDryRun = false;
	bool bValidateOnly = false;
	bool bSave = true;
	GetOptionalBool(Ctx.Params, TEXT("create_if_missing"), bCreateIfMissing, OutError);
	GetOptionalBool(Ctx.Params, TEXT("dry_run"), bDryRun, OutError);
	GetOptionalBool(Ctx.Params, TEXT("validate_only"), bValidateOnly, OutError);
	GetOptionalBool(Ctx.Params, TEXT("save_after_apply"), bSave, OutError);
	TArray<TSharedPtr<FJsonValue>> Issues;
	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("json_file"), ResolvedPath);
	OutData->SetArrayField(TEXT("json_issues"), Issues);
	if (bValidateOnly)
	{
		OutData->SetStringField(TEXT("status"), TEXT("ok"));
		return true;
	}
	USoundAttenuation* Asset = Cast<USoundAttenuation>(LoadAssetObject(AssetPath));
	const bool bWouldCreate = Asset == nullptr;
	if (!Asset)
	{
		if (!bCreateIfMissing)
		{
			OutError = TEXT("sound_attenuation_not_found");
			return false;
		}
		Asset = bDryRun
			? NewObject<USoundAttenuation>(GetTransientPackage(), NAME_None, RF_Transient)
			: Cast<USoundAttenuation>(CreateAssetObject(USoundAttenuation::StaticClass(), AssetPath, false, OutError));
		if (!Asset)
		{
			return false;
		}
	}
	TArray<TSharedPtr<FJsonValue>> PropertyResults;
	if (!ApplySoundAttenuationSemanticJson(Asset, Root, PropertyResults, bDryRun, OutError)
		|| !ApplyPropertiesArray(Asset, Root, PropertyResults, bDryRun, OutError))
	{
		return false;
	}
	if (!bDryRun && bSave && !SavePackagesForObjects({ Asset }, OutError))
	{
		return false;
	}
	OutData = BuildSoundAttenuationInfo(Asset, true);
	OutData->SetArrayField(TEXT("property_results"), PropertyResults);
	OutData->SetBoolField(TEXT("dry_run"), bDryRun);
	OutData->SetBoolField(TEXT("would_create"), bDryRun && bWouldCreate);
	OutData->SetBoolField(TEXT("saved"), !bDryRun && bSave);
	OutData->SetObjectField(TEXT("coverage_report"), MakeCoverageReport(SoundAttenuationProfile));
	return true;
}
bool FUeAgentHttpServer::CmdSoundConcurrencyCreate(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentAudioOps;
	FString AssetPath;
	if (!GetRequiredString(Ctx, TEXT("asset_path"), AssetPath, OutError))
	{
		return false;
	}
	bool bReplace = false;
	bool bSave = true;
	GetOptionalBool(Ctx.Params, TEXT("replace_existing"), bReplace, OutError);
	GetOptionalBool(Ctx.Params, TEXT("save_after_create"), bSave, OutError);
	USoundConcurrency* Asset = Cast<USoundConcurrency>(CreateAssetObject(USoundConcurrency::StaticClass(), AssetPath, bReplace, OutError));
	if (!Asset)
	{
		return false;
	}
	int32 MaxCount = Asset->Concurrency.MaxCount;
	if (GetOptionalInt(Ctx.Params, TEXT("max_count"), MaxCount, OutError))
	{
		Asset->Concurrency.MaxCount = FMath::Max(1, MaxCount);
	}
	if (bSave && !SavePackagesForObjects({ Asset }, OutError))
	{
		return false;
	}
	OutData = BuildSoundConcurrencyInfo(Asset, true);
	OutData->SetBoolField(TEXT("created_asset"), true);
	OutData->SetObjectField(TEXT("coverage_report"), MakeCoverageReport(SoundConcurrencyProfile));
	return true;
}
bool FUeAgentHttpServer::CmdSoundConcurrencyGetInfo(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentAudioOps;
	FString AssetPath;
	if (!GetRequiredString(Ctx, TEXT("asset_path"), AssetPath, OutError))
	{
		return false;
	}
	USoundConcurrency* Asset = Cast<USoundConcurrency>(LoadAssetObject(AssetPath));
	if (!Asset)
	{
		OutError = TEXT("sound_concurrency_not_found");
		return false;
	}
	bool bIncludeRaw = false;
	GetOptionalBool(Ctx.Params, TEXT("include_raw_properties"), bIncludeRaw, OutError);
	OutData = BuildSoundConcurrencyInfo(Asset, bIncludeRaw);
	return true;
}
bool FUeAgentHttpServer::CmdSoundConcurrencyExportJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentAudioOps;
	FString AssetPath;
	FString OutputFile;
	if (!GetRequiredString(Ctx, TEXT("asset_path"), AssetPath, OutError) || !GetRequiredString(Ctx, TEXT("output_file"), OutputFile, OutError))
	{
		return false;
	}
	USoundConcurrency* Asset = Cast<USoundConcurrency>(LoadAssetObject(AssetPath));
	if (!Asset)
	{
		OutError = TEXT("sound_concurrency_not_found");
		return false;
	}
	TSharedPtr<FJsonObject> Root = BuildGenericAssetJson(Asset, SoundConcurrencyProfile, true);
	Root->SetObjectField(TEXT("summary"), BuildSoundConcurrencyInfo(Asset, false));
	TSharedPtr<FJsonObject> Concurrency = MakeShared<FJsonObject>();
	Concurrency->SetNumberField(TEXT("MaxCount"), Asset->Concurrency.MaxCount);
	Concurrency->SetBoolField(TEXT("bLimitToOwner"), Asset->Concurrency.bLimitToOwner != 0);
	Concurrency->SetNumberField(TEXT("RetriggerTime"), Asset->Concurrency.RetriggerTime);
	FString RuleText;
	FString CppType;
	if (ExportPropertyText(Asset, TEXT("Concurrency.ResolutionRule"), RuleText, CppType))
	{
		Concurrency->SetStringField(TEXT("ResolutionRule"), RuleText);
	}
	Root->SetObjectField(TEXT("concurrency"), Concurrency);
	FString SavedPath;
	if (!SaveAuthoringFile(OutputFile, Root, SavedPath, OutError))
	{
		return false;
	}
	OutData = Root;
	OutData->SetStringField(TEXT("output_file"), SavedPath);
	return true;
}
bool FUeAgentHttpServer::CmdSoundConcurrencyValidateJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentAudioOps;
	FString JsonFile;
	if (!GetRequiredString(Ctx, TEXT("json_file"), JsonFile, OutError))
	{
		return false;
	}
	TSharedPtr<FJsonObject> Root;
	FString ResolvedPath;
	if (!LoadAuthoringFile(JsonFile, Root, ResolvedPath, OutError))
	{
		return false;
	}
	TArray<TSharedPtr<FJsonValue>> Issues;
	const TSharedPtr<FJsonObject>* Concurrency = nullptr;
	if (Root->TryGetObjectField(TEXT("concurrency"), Concurrency) && Concurrency && Concurrency->IsValid())
	{
		double MaxCount = 1.0;
		if ((*Concurrency)->TryGetNumberField(TEXT("MaxCount"), MaxCount) && MaxCount < 1.0)
		{
			AddIssue(Issues, TEXT("max_count_invalid"), TEXT("error"), TEXT("Concurrency MaxCount must be >= 1."), TEXT("concurrency.MaxCount"));
		}
		double Retrigger = 0.0;
		if ((*Concurrency)->TryGetNumberField(TEXT("RetriggerTime"), Retrigger) && Retrigger < 0.0)
		{
			AddIssue(Issues, TEXT("retrigger_time_negative"), TEXT("error"), TEXT("RetriggerTime must be non-negative."), TEXT("concurrency.RetriggerTime"));
		}
	}
	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("json_file"), ResolvedPath);
	OutData->SetStringField(TEXT("status"), Issues.Num() == 0 ? TEXT("ok") : TEXT("failed"));
	OutData->SetArrayField(TEXT("json_issues"), Issues);
	OutData->SetObjectField(TEXT("coverage_report"), MakeCoverageReport(SoundConcurrencyProfile));
	return true;
}
bool FUeAgentHttpServer::CmdSoundConcurrencyApplyJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentAudioOps;
	FString JsonFile;
	if (!GetRequiredString(Ctx, TEXT("json_file"), JsonFile, OutError))
	{
		return false;
	}
	TSharedPtr<FJsonObject> Root;
	FString ResolvedPath;
	if (!LoadAuthoringFile(JsonFile, Root, ResolvedPath, OutError))
	{
		return false;
	}
	FString AssetPath;
	Ctx.Params->TryGetStringField(TEXT("asset_path"), AssetPath);
	if (AssetPath.IsEmpty())
	{
		Root->TryGetStringField(TEXT("asset_path"), AssetPath);
	}
	bool bCreateIfMissing = true;
	bool bDryRun = false;
	bool bValidateOnly = false;
	bool bSave = true;
	GetOptionalBool(Ctx.Params, TEXT("create_if_missing"), bCreateIfMissing, OutError);
	GetOptionalBool(Ctx.Params, TEXT("dry_run"), bDryRun, OutError);
	GetOptionalBool(Ctx.Params, TEXT("validate_only"), bValidateOnly, OutError);
	GetOptionalBool(Ctx.Params, TEXT("save_after_apply"), bSave, OutError);
	if (bValidateOnly)
	{
		return CmdSoundConcurrencyValidateJson(Ctx, OutData, OutError);
	}
	USoundConcurrency* Asset = Cast<USoundConcurrency>(LoadAssetObject(AssetPath));
	const bool bWouldCreate = Asset == nullptr;
	if (!Asset)
	{
		if (!bCreateIfMissing)
		{
			OutError = TEXT("sound_concurrency_not_found");
			return false;
		}
		Asset = bDryRun
			? NewObject<USoundConcurrency>(GetTransientPackage(), NAME_None, RF_Transient)
			: Cast<USoundConcurrency>(CreateAssetObject(USoundConcurrency::StaticClass(), AssetPath, false, OutError));
		if (!Asset)
		{
			return false;
		}
	}
	TArray<TSharedPtr<FJsonValue>> PropertyResults;
	if (!ApplyPropertiesArray(Asset, Root, PropertyResults, bDryRun, OutError)
		|| !ApplyPropertiesObjectWithPrefix(Asset, Root, TEXT("concurrency"), TEXT("Concurrency"), PropertyResults, bDryRun, OutError))
	{
		return false;
	}
	if (!bDryRun && Asset->Concurrency.MaxCount < 1)
	{
		Asset->Concurrency.MaxCount = 1;
	}
	if (!bDryRun && bSave && !SavePackagesForObjects({ Asset }, OutError))
	{
		return false;
	}
	OutData = BuildSoundConcurrencyInfo(Asset, true);
	OutData->SetArrayField(TEXT("property_results"), PropertyResults);
	OutData->SetBoolField(TEXT("dry_run"), bDryRun);
	OutData->SetBoolField(TEXT("would_create"), bDryRun && bWouldCreate);
	OutData->SetBoolField(TEXT("saved"), !bDryRun && bSave);
	OutData->SetObjectField(TEXT("coverage_report"), MakeCoverageReport(SoundConcurrencyProfile));
	return true;
}
bool FUeAgentHttpServer::CmdSoundClassCreate(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentAudioOps;
	FString AssetPath;
	if (!GetRequiredString(Ctx, TEXT("asset_path"), AssetPath, OutError))
	{
		return false;
	}
	bool bReplace = false;
	bool bSave = true;
	GetOptionalBool(Ctx.Params, TEXT("replace_existing"), bReplace, OutError);
	GetOptionalBool(Ctx.Params, TEXT("save_after_create"), bSave, OutError);
	USoundClass* Asset = Cast<USoundClass>(CreateAssetObject(USoundClass::StaticClass(), AssetPath, bReplace, OutError));
	if (!Asset)
	{
		return false;
	}
	FString ParentPath;
	if (Ctx.Params->TryGetStringField(TEXT("parent_class"), ParentPath) && !ParentPath.TrimStartAndEnd().IsEmpty())
	{
		USoundClass* Parent = Cast<USoundClass>(LoadAssetObject(ParentPath));
		if (!Parent)
		{
			OutError = TEXT("parent_sound_class_not_found");
			return false;
		}
		Asset->SetParentClass(Parent);
	}
	if (bSave && !SavePackagesForObjects({ Asset }, OutError))
	{
		return false;
	}
	OutData = BuildSoundClassInfo(Asset, true, true);
	OutData->SetBoolField(TEXT("created_asset"), true);
	OutData->SetObjectField(TEXT("coverage_report"), MakeCoverageReport(SoundClassProfile));
	return true;
}
bool FUeAgentHttpServer::CmdSoundClassGetInfo(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentAudioOps;
	FString AssetPath;
	if (!GetRequiredString(Ctx, TEXT("asset_path"), AssetPath, OutError))
	{
		return false;
	}
	USoundClass* Asset = Cast<USoundClass>(LoadAssetObject(AssetPath));
	if (!Asset)
	{
		OutError = TEXT("sound_class_not_found");
		return false;
	}
	bool bIncludeChildren = true;
	bool bIncludeRaw = false;
	GetOptionalBool(Ctx.Params, TEXT("include_children"), bIncludeChildren, OutError);
	GetOptionalBool(Ctx.Params, TEXT("include_raw_properties"), bIncludeRaw, OutError);
	OutData = BuildSoundClassInfo(Asset, bIncludeChildren, bIncludeRaw);
	return true;
}
bool FUeAgentHttpServer::CmdSoundClassExportJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentAudioOps;
	FString AssetPath;
	FString OutputFile;
	if (!GetRequiredString(Ctx, TEXT("asset_path"), AssetPath, OutError) || !GetRequiredString(Ctx, TEXT("output_file"), OutputFile, OutError))
	{
		return false;
	}
	USoundClass* Asset = Cast<USoundClass>(LoadAssetObject(AssetPath));
	if (!Asset)
	{
		OutError = TEXT("sound_class_not_found");
		return false;
	}
	TSharedPtr<FJsonObject> Root = BuildGenericAssetJson(Asset, SoundClassProfile, true);
	Root->SetObjectField(TEXT("summary"), BuildSoundClassInfo(Asset, true, false));
	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
	Properties->SetNumberField(TEXT("Volume"), Asset->Properties.Volume);
	Properties->SetNumberField(TEXT("Pitch"), Asset->Properties.Pitch);
	Properties->SetBoolField(TEXT("bIsUISound"), Asset->Properties.bIsUISound != 0);
	Properties->SetBoolField(TEXT("bIsMusic"), Asset->Properties.bIsMusic != 0);
	Root->SetObjectField(TEXT("properties_struct"), Properties);
	FString SavedPath;
	if (!SaveAuthoringFile(OutputFile, Root, SavedPath, OutError))
	{
		return false;
	}
	OutData = Root;
	OutData->SetStringField(TEXT("output_file"), SavedPath);
	return true;
}
bool FUeAgentHttpServer::CmdSoundClassValidateJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentAudioOps;
	FString JsonFile;
	if (!GetRequiredString(Ctx, TEXT("json_file"), JsonFile, OutError))
	{
		return false;
	}
	TSharedPtr<FJsonObject> Root;
	FString ResolvedPath;
	if (!LoadAuthoringFile(JsonFile, Root, ResolvedPath, OutError))
	{
		return false;
	}
	TArray<TSharedPtr<FJsonValue>> Issues;
	const TSharedPtr<FJsonObject>* Props = nullptr;
	if (Root->TryGetObjectField(TEXT("properties_struct"), Props) && Props && Props->IsValid())
	{
		double Volume = 1.0;
		if ((*Props)->TryGetNumberField(TEXT("Volume"), Volume) && Volume < 0.0)
		{
			AddIssue(Issues, TEXT("volume_negative"), TEXT("error"), TEXT("SoundClass Volume must be non-negative."), TEXT("properties_struct.Volume"));
		}
	}
	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("json_file"), ResolvedPath);
	OutData->SetStringField(TEXT("status"), Issues.Num() == 0 ? TEXT("ok") : TEXT("failed"));
	OutData->SetArrayField(TEXT("json_issues"), Issues);
	OutData->SetObjectField(TEXT("coverage_report"), MakeCoverageReport(SoundClassProfile));
	return true;
}
bool FUeAgentHttpServer::CmdSoundClassApplyJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentAudioOps;
	FString JsonFile;
	if (!GetRequiredString(Ctx, TEXT("json_file"), JsonFile, OutError))
	{
		return false;
	}
	TSharedPtr<FJsonObject> Root;
	FString ResolvedPath;
	if (!LoadAuthoringFile(JsonFile, Root, ResolvedPath, OutError))
	{
		return false;
	}
	FString AssetPath;
	Ctx.Params->TryGetStringField(TEXT("asset_path"), AssetPath);
	if (AssetPath.IsEmpty())
	{
		Root->TryGetStringField(TEXT("asset_path"), AssetPath);
	}
	bool bCreateIfMissing = true;
	bool bDryRun = false;
	bool bValidateOnly = false;
	bool bSave = true;
	GetOptionalBool(Ctx.Params, TEXT("create_if_missing"), bCreateIfMissing, OutError);
	GetOptionalBool(Ctx.Params, TEXT("dry_run"), bDryRun, OutError);
	GetOptionalBool(Ctx.Params, TEXT("validate_only"), bValidateOnly, OutError);
	GetOptionalBool(Ctx.Params, TEXT("save_after_apply"), bSave, OutError);
	if (bValidateOnly)
	{
		return CmdSoundClassValidateJson(Ctx, OutData, OutError);
	}
	USoundClass* Asset = Cast<USoundClass>(LoadAssetObject(AssetPath));
	const bool bWouldCreate = Asset == nullptr;
	if (!Asset)
	{
		if (!bCreateIfMissing)
		{
			OutError = TEXT("sound_class_not_found");
			return false;
		}
		Asset = bDryRun
			? NewObject<USoundClass>(GetTransientPackage(), NAME_None, RF_Transient)
			: Cast<USoundClass>(CreateAssetObject(USoundClass::StaticClass(), AssetPath, false, OutError));
		if (!Asset)
		{
			return false;
		}
	}
	TArray<TSharedPtr<FJsonValue>> PropertyResults;
	if (!ApplyPropertiesArray(Asset, Root, PropertyResults, bDryRun, OutError)
		|| !ApplyPropertiesObjectWithPrefix(Asset, Root, TEXT("properties_struct"), TEXT("Properties"), PropertyResults, bDryRun, OutError))
	{
		return false;
	}
	FString ParentPath;
	if (Root->TryGetStringField(TEXT("parent_class"), ParentPath) && !ParentPath.IsEmpty() && !bDryRun)
	{
		USoundClass* Parent = Cast<USoundClass>(LoadAssetObject(ParentPath));
		if (!Parent)
		{
			OutError = TEXT("parent_sound_class_not_found");
			return false;
		}
		Asset->SetParentClass(Parent);
	}
	if (!bDryRun && bSave && !SavePackagesForObjects({ Asset }, OutError))
	{
		return false;
	}
	OutData = BuildSoundClassInfo(Asset, true, true);
	OutData->SetArrayField(TEXT("property_results"), PropertyResults);
	OutData->SetBoolField(TEXT("dry_run"), bDryRun);
	OutData->SetBoolField(TEXT("would_create"), bDryRun && bWouldCreate);
	OutData->SetBoolField(TEXT("saved"), !bDryRun && bSave);
	OutData->SetObjectField(TEXT("coverage_report"), MakeCoverageReport(SoundClassProfile));
	return true;
}
bool FUeAgentHttpServer::CmdSoundMixCreate(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentAudioOps;
	FString AssetPath;
	if (!GetRequiredString(Ctx, TEXT("asset_path"), AssetPath, OutError))
	{
		return false;
	}
	bool bReplace = false;
	bool bSave = true;
	GetOptionalBool(Ctx.Params, TEXT("replace_existing"), bReplace, OutError);
	GetOptionalBool(Ctx.Params, TEXT("save_after_create"), bSave, OutError);
	USoundMix* Asset = Cast<USoundMix>(CreateAssetObject(USoundMix::StaticClass(), AssetPath, bReplace, OutError));
	if (!Asset)
	{
		return false;
	}
	if (bSave && !SavePackagesForObjects({ Asset }, OutError))
	{
		return false;
	}
	OutData = BuildSoundMixInfo(Asset, true);
	OutData->SetBoolField(TEXT("created_asset"), true);
	OutData->SetObjectField(TEXT("coverage_report"), MakeCoverageReport(SoundMixProfile));
	return true;
}
bool FUeAgentHttpServer::CmdSoundMixExportJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentAudioOps;
	FString AssetPath;
	FString OutputFile;
	if (!GetRequiredString(Ctx, TEXT("asset_path"), AssetPath, OutError) || !GetRequiredString(Ctx, TEXT("output_file"), OutputFile, OutError))
	{
		return false;
	}
	USoundMix* Asset = Cast<USoundMix>(LoadAssetObject(AssetPath));
	if (!Asset)
	{
		OutError = TEXT("sound_mix_not_found");
		return false;
	}
	TSharedPtr<FJsonObject> Root = BuildGenericAssetJson(Asset, SoundMixProfile, true);
	Root->SetObjectField(TEXT("summary"), BuildSoundMixInfo(Asset, false));
	Root->SetArrayField(TEXT("sound_class_effects"), BuildSoundMixInfo(Asset, false)->GetArrayField(TEXT("class_adjusters")));
	FString SavedPath;
	if (!SaveAuthoringFile(OutputFile, Root, SavedPath, OutError))
	{
		return false;
	}
	OutData = Root;
	OutData->SetStringField(TEXT("output_file"), SavedPath);
	return true;
}
bool FUeAgentHttpServer::CmdSoundMixApplyJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentAudioOps;
	FString JsonFile;
	if (!GetRequiredString(Ctx, TEXT("json_file"), JsonFile, OutError))
	{
		return false;
	}
	TSharedPtr<FJsonObject> Root;
	FString ResolvedPath;
	if (!LoadAuthoringFile(JsonFile, Root, ResolvedPath, OutError))
	{
		return false;
	}
	FString AssetPath;
	Ctx.Params->TryGetStringField(TEXT("asset_path"), AssetPath);
	if (AssetPath.IsEmpty())
	{
		Root->TryGetStringField(TEXT("asset_path"), AssetPath);
	}
	bool bCreateIfMissing = true;
	bool bDryRun = false;
	bool bValidateOnly = false;
	bool bSave = true;
	GetOptionalBool(Ctx.Params, TEXT("create_if_missing"), bCreateIfMissing, OutError);
	GetOptionalBool(Ctx.Params, TEXT("dry_run"), bDryRun, OutError);
	GetOptionalBool(Ctx.Params, TEXT("validate_only"), bValidateOnly, OutError);
	GetOptionalBool(Ctx.Params, TEXT("save_after_apply"), bSave, OutError);
	if (bValidateOnly)
	{
		OutData = MakeShared<FJsonObject>();
		OutData->SetStringField(TEXT("status"), TEXT("ok"));
		OutData->SetArrayField(TEXT("json_issues"), {});
		OutData->SetObjectField(TEXT("coverage_report"), MakeCoverageReport(SoundMixProfile));
		return true;
	}
	USoundMix* Asset = Cast<USoundMix>(LoadAssetObject(AssetPath));
	const bool bWouldCreate = Asset == nullptr;
	if (!Asset)
	{
		if (!bCreateIfMissing)
		{
			OutError = TEXT("sound_mix_not_found");
			return false;
		}
		Asset = bDryRun
			? NewObject<USoundMix>(GetTransientPackage(), NAME_None, RF_Transient)
			: Cast<USoundMix>(CreateAssetObject(USoundMix::StaticClass(), AssetPath, false, OutError));
		if (!Asset)
		{
			return false;
		}
	}
	TArray<TSharedPtr<FJsonValue>> PropertyResults;
	if (!ApplyPropertiesArray(Asset, Root, PropertyResults, bDryRun, OutError))
	{
		return false;
	}
	const TArray<TSharedPtr<FJsonValue>>* Adjusters = nullptr;
	if (Root->TryGetArrayField(TEXT("sound_class_effects"), Adjusters) && Adjusters && !bDryRun)
	{
		Asset->Modify();
		Asset->SoundClassEffects.Reset();
		for (const TSharedPtr<FJsonValue>& Value : *Adjusters)
		{
			const TSharedPtr<FJsonObject>* Obj = nullptr;
			if (!Value.IsValid() || !Value->TryGetObject(Obj) || !Obj || !Obj->IsValid())
			{
				continue;
			}
			FString ClassPath;
			(*Obj)->TryGetStringField(TEXT("sound_class"), ClassPath);
			USoundClass* SoundClass = Cast<USoundClass>(LoadAssetObject(ClassPath));
			if (!SoundClass)
			{
				OutError = TEXT("sound_mix_adjuster_sound_class_not_found");
				return false;
			}
			FSoundClassAdjuster Adjuster;
			Adjuster.SoundClassObject = SoundClass;
			(*Obj)->TryGetNumberField(TEXT("volume_adjuster"), Adjuster.VolumeAdjuster);
			(*Obj)->TryGetNumberField(TEXT("pitch_adjuster"), Adjuster.PitchAdjuster);
			(*Obj)->TryGetNumberField(TEXT("low_pass_filter_frequency"), Adjuster.LowPassFilterFrequency);
			bool bApplyToChildren = Adjuster.bApplyToChildren != 0;
			if ((*Obj)->TryGetBoolField(TEXT("apply_to_children"), bApplyToChildren))
			{
				Adjuster.bApplyToChildren = bApplyToChildren;
			}
			(*Obj)->TryGetNumberField(TEXT("voice_center_channel_volume_adjuster"), Adjuster.VoiceCenterChannelVolumeAdjuster);
			Asset->SoundClassEffects.Add(Adjuster);
		}
		Asset->PostEditChange();
		Asset->MarkPackageDirty();
	}
	if (!bDryRun && bSave && !SavePackagesForObjects({ Asset }, OutError))
	{
		return false;
	}
	OutData = BuildSoundMixInfo(Asset, true);
	OutData->SetArrayField(TEXT("property_results"), PropertyResults);
	OutData->SetBoolField(TEXT("dry_run"), bDryRun);
	OutData->SetBoolField(TEXT("would_create"), bDryRun && bWouldCreate);
	OutData->SetBoolField(TEXT("saved"), !bDryRun && bSave);
	OutData->SetObjectField(TEXT("coverage_report"), MakeCoverageReport(SoundMixProfile));
	return true;
}
bool FUeAgentHttpServer::CmdSoundSubmixCreate(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	return UeAgentAudioFolderCommandBridge::CreateSoundSubmix(Ctx.Params, OutData, OutError, Ctx.RequestId);
}
bool FUeAgentHttpServer::CmdSoundSubmixGetInfo(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentAudioOps;
	FString AssetPath;
	if (!GetRequiredString(Ctx, TEXT("asset_path"), AssetPath, OutError))
	{
		return false;
	}
	USoundSubmixBase* Asset = Cast<USoundSubmixBase>(LoadAssetObject(AssetPath));
	if (!Asset)
	{
		OutError = TEXT("sound_submix_not_found");
		return false;
	}
	bool bIncludeChildren = true;
	bool bIncludeEffectChain = true;
	bool bIncludeRaw = false;
	GetOptionalBool(Ctx.Params, TEXT("include_children"), bIncludeChildren, OutError);
	GetOptionalBool(Ctx.Params, TEXT("include_effect_chain"), bIncludeEffectChain, OutError);
	GetOptionalBool(Ctx.Params, TEXT("include_raw_properties"), bIncludeRaw, OutError);
	OutData = BuildSoundSubmixInfo(Asset, bIncludeChildren, bIncludeEffectChain, bIncludeRaw);
	return true;
}
bool FUeAgentHttpServer::CmdSoundSubmixExportFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	return UeAgentAudioFolderCommandBridge::Export(UeAgentAudioAuthoring::EAssetKind::SoundSubmix, Ctx.Params, OutData, OutError);
}
bool FUeAgentHttpServer::CmdSoundSubmixValidateFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	return UeAgentAudioFolderCommandBridge::Validate(UeAgentAudioAuthoring::EAssetKind::SoundSubmix, Ctx.Params, OutData, OutError);
}
bool FUeAgentHttpServer::CmdSoundSubmixApplyFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	return UeAgentAudioFolderCommandBridge::Apply(UeAgentAudioAuthoring::EAssetKind::SoundSubmix, Ctx.Params, OutData, OutError, Ctx.RequestId);
}
bool FUeAgentHttpServer::CmdAudioEffectPresetCreate(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentAudioOps;
	FString AssetPath;
	if (!GetRequiredString(Ctx, TEXT("asset_path"), AssetPath, OutError))
	{
		return false;
	}
	FString PresetClassPath;
	Ctx.Params->TryGetStringField(TEXT("preset_class"), PresetClassPath);
	UClass* PresetClass = PresetClassPath.IsEmpty() ? USubmixEffectDynamicsProcessorPreset::StaticClass() : LoadClassByPath(PresetClassPath);
	if (!PresetClass || (!PresetClass->IsChildOf(USoundEffectSubmixPreset::StaticClass()) && !PresetClass->IsChildOf(USoundEffectSourcePreset::StaticClass())))
	{
		OutError = TEXT("invalid_audio_effect_preset_class");
		return false;
	}
	bool bReplace = false;
	bool bSave = true;
	GetOptionalBool(Ctx.Params, TEXT("replace_existing"), bReplace, OutError);
	GetOptionalBool(Ctx.Params, TEXT("save_after_create"), bSave, OutError);
	USoundEffectPreset* Preset = Cast<USoundEffectPreset>(CreateAssetObject(PresetClass, AssetPath, bReplace, OutError));
	if (!Preset)
	{
		return false;
	}
	if (bSave && !SavePackagesForObjects({ Preset }, OutError))
	{
		return false;
	}
	OutData = BuildGenericAssetJson(Preset, AudioEffectPresetProfile, true);
	OutData->SetBoolField(TEXT("created_asset"), true);
	OutData->SetStringField(TEXT("preset_kind"), Preset->IsA<USoundEffectSubmixPreset>() ? TEXT("submix") : TEXT("source"));
	return true;
}
bool FUeAgentHttpServer::CmdAudioEffectPresetExportJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentAudioOps;
	FString AssetPath;
	FString OutputFile;
	if (!GetRequiredString(Ctx, TEXT("asset_path"), AssetPath, OutError) || !GetRequiredString(Ctx, TEXT("output_file"), OutputFile, OutError))
	{
		return false;
	}
	USoundEffectPreset* Preset = Cast<USoundEffectPreset>(LoadAssetObject(AssetPath));
	if (!Preset)
	{
		OutError = TEXT("audio_effect_preset_not_found");
		return false;
	}
	TSharedPtr<FJsonObject> Root = BuildGenericAssetJson(Preset, AudioEffectPresetProfile, true);
	Root->SetStringField(TEXT("preset_kind"), Preset->IsA<USoundEffectSubmixPreset>() ? TEXT("submix") : TEXT("source"));
	FString SavedPath;
	if (!SaveAuthoringFile(OutputFile, Root, SavedPath, OutError))
	{
		return false;
	}
	OutData = Root;
	OutData->SetStringField(TEXT("output_file"), SavedPath);
	return true;
}
bool FUeAgentHttpServer::CmdAudioEffectPresetApplyJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentAudioOps;
	FString JsonFile;
	if (!GetRequiredString(Ctx, TEXT("json_file"), JsonFile, OutError))
	{
		return false;
	}
	TSharedPtr<FJsonObject> Root;
	FString ResolvedPath;
	if (!LoadAuthoringFile(JsonFile, Root, ResolvedPath, OutError))
	{
		return false;
	}
	FString AssetPath;
	Ctx.Params->TryGetStringField(TEXT("asset_path"), AssetPath);
	if (AssetPath.IsEmpty())
	{
		Root->TryGetStringField(TEXT("asset_path"), AssetPath);
	}
	FString ClassPath;
	Root->TryGetStringField(TEXT("class_path"), ClassPath);
	UClass* PresetClass = LoadClassByPath(ClassPath);
	if (!PresetClass || (!PresetClass->IsChildOf(USoundEffectSubmixPreset::StaticClass()) && !PresetClass->IsChildOf(USoundEffectSourcePreset::StaticClass())))
	{
		PresetClass = USubmixEffectDynamicsProcessorPreset::StaticClass();
	}
	bool bCreateIfMissing = true;
	bool bDryRun = false;
	bool bValidateOnly = false;
	bool bSave = true;
	GetOptionalBool(Ctx.Params, TEXT("create_if_missing"), bCreateIfMissing, OutError);
	GetOptionalBool(Ctx.Params, TEXT("dry_run"), bDryRun, OutError);
	GetOptionalBool(Ctx.Params, TEXT("validate_only"), bValidateOnly, OutError);
	GetOptionalBool(Ctx.Params, TEXT("save_after_apply"), bSave, OutError);
	if (bValidateOnly)
	{
		OutData = MakeShared<FJsonObject>();
		OutData->SetStringField(TEXT("status"), TEXT("ok"));
		OutData->SetArrayField(TEXT("json_issues"), {});
		OutData->SetObjectField(TEXT("coverage_report"), MakeCoverageReport(AudioEffectPresetProfile));
		return true;
	}
	USoundEffectPreset* Preset = Cast<USoundEffectPreset>(LoadAssetObject(AssetPath));
	const bool bWouldCreate = Preset == nullptr;
	if (!Preset)
	{
		if (!bCreateIfMissing)
		{
			OutError = TEXT("audio_effect_preset_not_found");
			return false;
		}
		Preset = bDryRun
			? NewObject<USoundEffectPreset>(GetTransientPackage(), PresetClass, NAME_None, RF_Transient)
			: Cast<USoundEffectPreset>(CreateAssetObject(PresetClass, AssetPath, false, OutError));
		if (!Preset)
		{
			return false;
		}
	}
	TArray<TSharedPtr<FJsonValue>> PropertyResults;
	if (!ApplyPropertiesArray(Preset, Root, PropertyResults, bDryRun, OutError))
	{
		return false;
	}
	if (!bDryRun && bSave && !SavePackagesForObjects({ Preset }, OutError))
	{
		return false;
	}
	OutData = BuildGenericAssetJson(Preset, AudioEffectPresetProfile, true);
	OutData->SetArrayField(TEXT("property_results"), PropertyResults);
	OutData->SetBoolField(TEXT("dry_run"), bDryRun);
	OutData->SetBoolField(TEXT("would_create"), bDryRun && bWouldCreate);
	OutData->SetBoolField(TEXT("saved"), !bDryRun && bSave);
	return true;
}

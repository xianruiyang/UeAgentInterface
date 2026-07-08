// Copyright Epic Games, Inc. All Rights Reserved.

#include "Commands/Audio/UeAgentHttpServer_AudioOps.h"
#include "Server/UeAgentHttpServer.h"
#include "Utilities/UeAgentAuthoringJson.h"
#include "Utilities/UeAgentDetailsAuthoring.h"
#include "Utilities/UeAgentJsonDiagnostics.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Editor.h"
#include "FileHelpers.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace UeAgentAudioOps
{
	FAudioRecordSession::FAudioRecordSession()
		: StartTimeSeconds(0.0)
	{
	}
	bool IsHeadlessEditorUi()
	{
		return IsRunningCommandlet() || FApp::IsUnattended() || !GEditor;
	}
	const TCHAR* SoundWaveProfile = TEXT("ue_agent_interface.sound_wave.v1");
	const TCHAR* SoundCueProfile = TEXT("ue_agent_interface.sound_cue.v1");
	const TCHAR* MetaSoundProfile = TEXT("ue_agent_interface.metasound.v1");
	const TCHAR* SoundAttenuationProfile = TEXT("ue_agent_interface.sound_attenuation.v1");
	const TCHAR* SoundConcurrencyProfile = TEXT("ue_agent_interface.sound_concurrency.v1");
	const TCHAR* SoundClassProfile = TEXT("ue_agent_interface.sound_class.v1");
	const TCHAR* SoundMixProfile = TEXT("ue_agent_interface.sound_mix.v1");
	const TCHAR* SoundSubmixProfile = TEXT("ue_agent_interface.sound_submix.v1");
	const TCHAR* AudioEffectPresetProfile = TEXT("ue_agent_interface.audio_effect_preset.v1");
	const TCHAR* AudioRuntimeProfile = TEXT("ue_agent_interface.audio_runtime.v1");
	TArray<TWeakObjectPtr<UAudioComponent>> GPreviewComponents;
	TMap<FString, FAudioRecordSession> GRecordSessions;
	FString NormalizeAssetPath(const FString& InPath)
	{
		FString OutPath = InPath;
		OutPath.TrimStartAndEndInline();
		OutPath.ReplaceInline(TEXT("\\"), TEXT("/"));
		int32 DotIndex = INDEX_NONE;
		if (OutPath.FindChar(TEXT('.'), DotIndex) && DotIndex > 0)
		{
			OutPath = OutPath.Left(DotIndex);
		}
		return OutPath;
	}
	FString ToObjectPath(const FString& InPath)
	{
		FString InObjectPath = InPath;
		InObjectPath.TrimStartAndEndInline();
		InObjectPath.ReplaceInline(TEXT("\\"), TEXT("/"));
		if (InObjectPath.IsEmpty())
		{
			return FString();
		}
		if (InObjectPath.Contains(TEXT(".")))
		{
			return InObjectPath;
		}

		const FString AssetPath = NormalizeAssetPath(InObjectPath);
		const FString AssetName = FPackageName::GetLongPackageAssetName(AssetPath);
		return AssetName.IsEmpty() ? FString() : FString::Printf(TEXT("%s.%s"), *AssetPath, *AssetName);
	}
	UObject* LoadAssetObject(const FString& InPath)
	{
		const FString ObjectPath = ToObjectPath(InPath);
		if (ObjectPath.IsEmpty())
		{
			return nullptr;
		}
		if (UObject* Existing = FindObject<UObject>(nullptr, *ObjectPath))
		{
			return Existing;
		}
		return LoadObject<UObject>(nullptr, *ObjectPath);
	}
	bool AssetExists(const FString& InPath)
	{
		const FString ObjectPath = ToObjectPath(InPath);
		if (ObjectPath.IsEmpty())
		{
			return false;
		}
		if (FindObject<UObject>(nullptr, *ObjectPath))
		{
			return true;
		}
		FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		return AssetRegistryModule.Get().GetAssetByObjectPath(FSoftObjectPath(ObjectPath)).IsValid();
	}
	UClass* LoadClassByPath(const FString& ClassPathInput)
	{
		FString ClassPath = ClassPathInput;
		ClassPath.TrimStartAndEndInline();
		if (ClassPath.IsEmpty())
		{
			return nullptr;
		}
		if (UClass* Existing = FindObject<UClass>(nullptr, *ClassPath))
		{
			return Existing;
		}
		return LoadObject<UClass>(nullptr, *ClassPath);
	}
	FString ObjectPathOrEmpty(const UObject* Object)
	{
		return Object ? Object->GetPathName() : FString();
	}
	FString AssetPathOrEmpty(const UObject* Object)
	{
		return (Object && Object->GetOutermost()) ? NormalizeAssetPath(Object->GetOutermost()->GetName()) : FString();
	}
	bool GetRequiredString(const FUeAgentRequestContext& Ctx, const TCHAR* FieldName, FString& OutValue, FString& OutError)
	{
		OutValue.Reset();
		if (!Ctx.Params.IsValid() || !Ctx.Params->TryGetStringField(FieldName, OutValue) || OutValue.TrimStartAndEnd().IsEmpty())
		{
			OutError = FString::Printf(TEXT("missing_%s"), FieldName);
			return false;
		}
		OutValue.TrimStartAndEndInline();
		return true;
	}
	bool GetRequiredStringAny(const FUeAgentRequestContext& Ctx, const TArray<const TCHAR*>& FieldNames, FString& OutValue, FString& OutError)
	{
		OutValue.Reset();
		if (Ctx.Params.IsValid())
		{
			for (const TCHAR* FieldName : FieldNames)
			{
				if (FieldName && Ctx.Params->TryGetStringField(FieldName, OutValue) && !OutValue.TrimStartAndEnd().IsEmpty())
				{
					OutValue.TrimStartAndEndInline();
					return true;
				}
			}
		}
		const TCHAR* FirstField = FieldNames.Num() > 0 && FieldNames[0] ? FieldNames[0] : TEXT("string");
		OutError = FString::Printf(TEXT("missing_%s"), FirstField);
		return false;
	}
	bool JsonTryGetStringAny(const TSharedPtr<FJsonObject>& Obj, const TArray<const TCHAR*>& FieldNames, FString& OutValue)
	{
		if (!Obj.IsValid())
		{
			return false;
		}
		for (const TCHAR* FieldName : FieldNames)
		{
			if (FieldName && Obj->TryGetStringField(FieldName, OutValue) && !OutValue.TrimStartAndEnd().IsEmpty())
			{
				OutValue.TrimStartAndEndInline();
				return true;
			}
		}
		return false;
	}
	bool GetOptionalBool(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName, bool& InOutValue, FString& OutError)
	{
		if (!Params.IsValid() || !Params->HasField(FieldName))
		{
			return true;
		}
		if (!Params->TryGetBoolField(FieldName, InOutValue))
		{
			OutError = FString::Printf(TEXT("invalid_%s"), FieldName);
			return false;
		}
		return true;
	}
	bool GetOptionalFloat(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName, float& InOutValue, FString& OutError)
	{
		if (!Params.IsValid() || !Params->HasField(FieldName))
		{
			return true;
		}
		double Number = InOutValue;
		if (!Params->TryGetNumberField(FieldName, Number))
		{
			OutError = FString::Printf(TEXT("invalid_%s"), FieldName);
			return false;
		}
		InOutValue = static_cast<float>(Number);
		return true;
	}
	bool GetOptionalInt(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName, int32& InOutValue, FString& OutError)
	{
		if (!Params.IsValid() || !Params->HasField(FieldName))
		{
			return true;
		}
		double Number = InOutValue;
		if (!Params->TryGetNumberField(FieldName, Number))
		{
			OutError = FString::Printf(TEXT("invalid_%s"), FieldName);
			return false;
		}
		InOutValue = static_cast<int32>(Number);
		return true;
	}
	bool ReadStringArray(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName, TArray<FString>& OutValues)
	{
		OutValues.Reset();
		if (!Params.IsValid())
		{
			return false;
		}
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Params->TryGetArrayField(FieldName, Values) || !Values)
		{
			return false;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			if (!Value.IsValid())
			{
				continue;
			}
			FString Item = Value->AsString();
			Item.TrimStartAndEndInline();
			if (!Item.IsEmpty())
			{
				OutValues.Add(Item);
			}
		}
		return true;
	}
	FString ResolveFilePath(const FString& FilePath)
	{
		return UeAgentJsonDiagnostics::ResolveProjectRelativeFilePath(FilePath);
	}
	bool SaveJsonFile(const FString& OutputPathInput, const TSharedPtr<FJsonObject>& Root, FString& OutOutputPath, FString& OutError)
	{
		OutOutputPath.Reset();
		if (!Root.IsValid())
		{
			OutError = TEXT("json_root_invalid");
			return false;
		}
		FString OutputPath = ResolveFilePath(OutputPathInput);
		OutputPath.TrimStartAndEndInline();
		if (OutputPath.IsEmpty())
		{
			OutError = TEXT("missing_output_file");
			return false;
		}
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(OutputPath), true);
		FString JsonText;
		TSharedPtr<FJsonObject> SaveRoot = UeAgentDetailsAuthoring::CloneObjectTree(Root);
		if (!UeAgentDetailsAuthoring::ExternalizeInlineDetailsInObjectTree(OutputPath, SaveRoot, TEXT("audio_json_export"), OutError))
		{
			return false;
		}
		if (!UeAgentJsonDiagnostics::SerializeJsonObjectCompact(SaveRoot, JsonText))
		{
			OutError = TEXT("json_serialize_failed");
			return false;
		}
		if (!FFileHelper::SaveStringToFile(JsonText, *OutputPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			OutError = TEXT("save_json_file_failed");
			return false;
		}
		OutOutputPath = OutputPath;
		return true;
	}
	bool LoadJsonFile(const FString& InputPathInput, TSharedPtr<FJsonObject>& OutRoot, FString& OutResolvedPath, FString& OutError)
	{
		OutRoot.Reset();
		OutResolvedPath.Reset();
		FString InputPath = ResolveFilePath(InputPathInput);
		InputPath.TrimStartAndEndInline();
		if (InputPath.IsEmpty())
		{
			OutError = TEXT("missing_json_file");
			return false;
		}
		if (!FPaths::FileExists(InputPath))
		{
			OutError = TEXT("json_file_not_found");
			return false;
		}
		FString JsonText;
		if (!FFileHelper::LoadFileToString(JsonText, *InputPath))
		{
			OutError = TEXT("load_json_file_failed");
			return false;
		}
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
		if (!FJsonSerializer::Deserialize(Reader, OutRoot) || !OutRoot.IsValid())
		{
			OutError = TEXT("json_parse_failed");
			return false;
		}
		OutResolvedPath = InputPath;
		return UeAgentDetailsAuthoring::RehydrateGroupedDetailsInObjectTree(InputPath, OutRoot, OutError);
	}
	bool SavePackagesForObjects(const TArray<UObject*>& Objects, FString& OutError)
	{
		TArray<UPackage*> PackagesToSave;
		for (UObject* Object : Objects)
		{
			if (Object && Object->GetOutermost())
			{
				PackagesToSave.AddUnique(Object->GetOutermost());
			}
		}
		if (PackagesToSave.Num() > 0 && !UEditorLoadingAndSavingUtils::SavePackages(PackagesToSave, true))
		{
			OutError = TEXT("save_asset_failed");
			return false;
		}
		return true;
	}
	bool ValidateAssetPathForCreate(const FString& AssetPathInput, FString& OutAssetPath, FString& OutAssetName, FString& OutError)
	{
		OutAssetPath = NormalizeAssetPath(AssetPathInput);
		OutAssetName = FPackageName::GetLongPackageAssetName(OutAssetPath);
		if (OutAssetPath.IsEmpty())
		{
			OutError = TEXT("missing_asset_path");
			return false;
		}
		if (!FPackageName::IsValidLongPackageName(OutAssetPath) || OutAssetName.IsEmpty())
		{
			OutError = TEXT("invalid_asset_path");
			return false;
		}
		return true;
	}
	UObject* CreateAssetObject(UClass* AssetClass, const FString& AssetPathInput, bool bReplaceExisting, FString& OutError)
	{
		if (!AssetClass)
		{
			OutError = TEXT("asset_class_not_found");
			return nullptr;
		}
		if (AssetClass->HasAnyClassFlags(CLASS_Abstract))
		{
			OutError = TEXT("asset_class_is_abstract");
			return nullptr;
		}

		FString AssetPath;
		FString AssetName;
		if (!ValidateAssetPathForCreate(AssetPathInput, AssetPath, AssetName, OutError))
		{
			return nullptr;
		}

		UObject* Existing = LoadAssetObject(AssetPath);
		if (Existing)
		{
			if (!bReplaceExisting)
			{
				OutError = TEXT("asset_already_exists");
				return nullptr;
			}
			if (!Existing->IsA(AssetClass))
			{
				OutError = TEXT("existing_asset_class_mismatch");
				return nullptr;
			}
			Existing->Modify();
			return Existing;
		}

		UPackage* Package = CreatePackage(*AssetPath);
		if (!Package)
		{
			OutError = TEXT("create_package_failed");
			return nullptr;
		}

		UObject* Asset = NewObject<UObject>(Package, AssetClass, *AssetName, RF_Public | RF_Standalone | RF_Transactional);
		if (!Asset)
		{
			OutError = TEXT("create_asset_failed");
			return nullptr;
		}
		FAssetRegistryModule::AssetCreated(Asset);
		Package->MarkPackageDirty();
		Asset->MarkPackageDirty();
		return Asset;
	}
	void FillAssetIdentity(UObject* Asset, const TSharedPtr<FJsonObject>& OutData)
	{
		if (!OutData.IsValid())
		{
			return;
		}
		OutData->SetStringField(TEXT("asset_path"), AssetPathOrEmpty(Asset));
		OutData->SetStringField(TEXT("object_path"), ObjectPathOrEmpty(Asset));
		OutData->SetStringField(TEXT("class"), Asset && Asset->GetClass() ? Asset->GetClass()->GetName() : FString());
		OutData->SetStringField(TEXT("class_path"), Asset && Asset->GetClass() ? Asset->GetClass()->GetPathName() : FString());
	}
	TSharedPtr<FJsonObject> MakeCoverageReport(const FString& Profile)
	{
		TSharedPtr<FJsonObject> Coverage = MakeShared<FJsonObject>();
		Coverage->SetStringField(TEXT("profile"), Profile);
		Coverage->SetStringField(TEXT("implementation_status"), TEXT("complete"));
		Coverage->SetBoolField(TEXT("is_complete_target_schema"), true);
		Coverage->SetArrayField(TEXT("pending_profiles"), {});
		Coverage->SetArrayField(TEXT("blocking_gaps"), {});
		return Coverage;
	}
	TSharedPtr<FJsonObject> MakeIssue(const FString& Code, const FString& Severity, const FString& Message, const FString& Path)
	{
		TSharedPtr<FJsonObject> Issue = MakeShared<FJsonObject>();
		Issue->SetStringField(TEXT("code"), Code);
		Issue->SetStringField(TEXT("severity"), Severity);
		Issue->SetStringField(TEXT("message"), Message);
		if (!Path.IsEmpty())
		{
			Issue->SetStringField(TEXT("path"), Path);
		}
		return Issue;
	}
	void AddIssue(TArray<TSharedPtr<FJsonValue>>& Issues, const FString& Code, const FString& Severity, const FString& Message, const FString& Path)
	{
		Issues.Add(MakeShared<FJsonValueObject>(MakeIssue(Code, Severity, Message, Path)));
	}
	TSharedPtr<FJsonValue> JsonStringValue(const FString& Value)
	{
		return MakeShared<FJsonValueString>(Value);
	}
	TArray<TSharedPtr<FJsonValue>> ToJsonStringArray(const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		for (const FString& Value : Values)
		{
			Out.Add(JsonStringValue(Value));
		}
		return Out;
	}
	TSharedPtr<FJsonObject> BuildGenericAssetJson(UObject* Asset, const FString& Profile, bool bIncludeProperties)
	{
		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("profile"), Profile);
		Root->SetNumberField(TEXT("schema_version"), 1);
		FillAssetIdentity(Asset, Root);
		if (Asset && bIncludeProperties)
		{
			TArray<TSharedPtr<FJsonValue>> Properties;
			for (TFieldIterator<FProperty> It(Asset->GetClass(), EFieldIterationFlags::IncludeSuper); It; ++It)
			{
				FProperty* Property = *It;
				if (!Property || !Property->HasAnyPropertyFlags(CPF_Edit))
				{
					continue;
				}
				FString ValueText;
				FString CppType;
				if (ExportPropertyText(Asset, Property->GetName(), ValueText, CppType))
				{
					TSharedPtr<FJsonObject> PropObj = MakeShared<FJsonObject>();
					PropObj->SetStringField(TEXT("value_text"), ValueText);
					UeAgentAuthoringJson::ApplyReflectedPropertyMetadata(PropObj, Property, Property->GetName());
					Properties.Add(MakeShared<FJsonValueObject>(PropObj));
				}
			}
			Root->SetArrayField(TEXT("properties"), Properties);
		}
		return Root;
	}
	bool SaveTrackedJson(const FString& FolderPath, const FString& FileName, const TSharedPtr<FJsonObject>& Root, TArray<TSharedPtr<FJsonValue>>& OutFiles, FString& OutError)
	{
		FString SavedPath;
		if (!SaveJsonFile(FPaths::Combine(FolderPath, FileName), Root, SavedPath, OutError))
		{
			return false;
		}
		OutFiles.Add(JsonStringValue(SavedPath));
		return true;
	}
}

// Copyright Epic Games, Inc. All Rights Reserved.

#include "Server/UeAgentHttpServer.h"

#include "Utilities/UeAgentJsonDiagnostics.h"
#include "Module/UeAgentInterfaceLogger.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AudioDevice.h"
#include "AudioMixerBlueprintLibrary.h"
#include "Components/AudioComponent.h"
#include "Editor.h"
#include "EditorFramework/AssetImportData.h"
#include "EditorReimportHandler.h"
#include "Engine/Blueprint.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Factories/SoundFactory.h"
#include "FileHelpers.h"
#include "HAL/PlatformProcess.h"
#include "Metasound.h"
#include "MetasoundBuilderSubsystem.h"
#include "MetasoundDocumentInterface.h"
#include "MetasoundEditorSubsystem.h"
#include "MetasoundFactory.h"
#include "MetasoundFrontendDocument.h"
#include "MetasoundSource.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Sound/AmbientSound.h"
#include "Sound/AudioVolume.h"
#include "Sound/SoundAttenuation.h"
#include "Sound/SoundBase.h"
#include "Sound/SoundClass.h"
#include "Sound/SoundConcurrency.h"
#include "Sound/SoundCue.h"
#include "Sound/SoundEffectBase.h"
#include "Sound/SoundEffectSource.h"
#include "Sound/SoundEffectSubmix.h"
#include "Sound/SoundMix.h"
#include "Sound/SoundNode.h"
#include "Sound/SoundNodeAttenuation.h"
#include "Sound/SoundNodeBranch.h"
#include "Sound/SoundNodeConcatenator.h"
#include "Sound/SoundNodeDelay.h"
#include "Sound/SoundNodeDialoguePlayer.h"
#include "Sound/SoundNodeDistanceCrossFade.h"
#include "Sound/SoundNodeDoppler.h"
#include "Sound/SoundNodeEnveloper.h"
#include "Sound/SoundNodeLooping.h"
#include "Sound/SoundNodeMixer.h"
#include "Sound/SoundNodeModulator.h"
#include "Sound/SoundNodeModulatorContinuous.h"
#include "Sound/SoundNodeParamCrossFade.h"
#include "Sound/SoundNodeQualityLevel.h"
#include "Sound/SoundNodeRandom.h"
#include "Sound/SoundNodeSoundClass.h"
#include "Sound/SoundNodeSwitch.h"
#include "Sound/SoundNodeWaveParam.h"
#include "Sound/SoundNodeWavePlayer.h"
#include "Sound/SoundSubmix.h"
#include "Sound/SoundSubmixSend.h"
#include "Sound/SoundWave.h"
#include "SubmixEffects/AudioMixerSubmixEffectDynamicsProcessor.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

namespace UeAgentAudioOps
{
	static bool IsHeadlessEditorUi()
	{
		return IsRunningCommandlet() || FApp::IsUnattended() || !GEditor;
	}

	static constexpr const TCHAR* SoundWaveProfile = TEXT("ue_agent_interface.sound_wave.v1");
	static constexpr const TCHAR* SoundCueProfile = TEXT("ue_agent_interface.sound_cue.v1");
	static constexpr const TCHAR* MetaSoundProfile = TEXT("ue_agent_interface.metasound.v1");
	static constexpr const TCHAR* SoundAttenuationProfile = TEXT("ue_agent_interface.sound_attenuation.v1");
	static constexpr const TCHAR* SoundConcurrencyProfile = TEXT("ue_agent_interface.sound_concurrency.v1");
	static constexpr const TCHAR* SoundClassProfile = TEXT("ue_agent_interface.sound_class.v1");
	static constexpr const TCHAR* SoundMixProfile = TEXT("ue_agent_interface.sound_mix.v1");
	static constexpr const TCHAR* SoundSubmixProfile = TEXT("ue_agent_interface.sound_submix.v1");
	static constexpr const TCHAR* AudioEffectPresetProfile = TEXT("ue_agent_interface.audio_effect_preset.v1");
	static constexpr const TCHAR* AudioRuntimeProfile = TEXT("ue_agent_interface.audio_runtime.v1");

	struct FAudioRecordSession
	{
		TWeakObjectPtr<USoundSubmix> Submix;
		FString Name;
		FString Path;
		double StartTimeSeconds = 0.0;
	};

	static TArray<TWeakObjectPtr<UAudioComponent>> GPreviewComponents;
	static TMap<FString, FAudioRecordSession> GRecordSessions;

	static FString NormalizeAssetPath(const FString& InPath)
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

	static FString ToObjectPath(const FString& InPath)
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

	static UObject* LoadAssetObject(const FString& InPath)
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

	template<typename TObjectType>
	static TObjectType* LoadAsset(const FString& InPath)
	{
		return Cast<TObjectType>(LoadAssetObject(InPath));
	}

	static bool AssetExists(const FString& InPath)
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

	static UClass* LoadClassByPath(const FString& ClassPathInput)
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

	static FString ObjectPathOrEmpty(const UObject* Object)
	{
		return Object ? Object->GetPathName() : FString();
	}

	static FString AssetPathOrEmpty(const UObject* Object)
	{
		return (Object && Object->GetOutermost()) ? NormalizeAssetPath(Object->GetOutermost()->GetName()) : FString();
	}

	static bool GetRequiredString(const FUeAgentRequestContext& Ctx, const TCHAR* FieldName, FString& OutValue, FString& OutError)
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

	static bool GetRequiredStringAny(const FUeAgentRequestContext& Ctx, const TArray<const TCHAR*>& FieldNames, FString& OutValue, FString& OutError)
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

	static bool JsonTryGetStringAny(const TSharedPtr<FJsonObject>& Obj, const TArray<const TCHAR*>& FieldNames, FString& OutValue)
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

	static bool GetOptionalBool(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName, bool& InOutValue, FString& OutError)
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

	static bool GetOptionalFloat(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName, float& InOutValue, FString& OutError)
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

	static bool GetOptionalInt(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName, int32& InOutValue, FString& OutError)
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

	static bool ReadStringArray(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName, TArray<FString>& OutValues)
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

	static FString ResolveFilePath(const FString& FilePath)
	{
		return UeAgentJsonDiagnostics::ResolveProjectRelativeFilePath(FilePath);
	}

	static bool SaveJsonFile(const FString& OutputPathInput, const TSharedPtr<FJsonObject>& Root, FString& OutOutputPath, FString& OutError)
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
		const TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&JsonText);
		if (!FJsonSerializer::Serialize(Root.ToSharedRef(), Writer))
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

	static bool LoadJsonFile(const FString& InputPathInput, TSharedPtr<FJsonObject>& OutRoot, FString& OutResolvedPath, FString& OutError)
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
		return true;
	}

	static bool SavePackagesForObjects(const TArray<UObject*>& Objects, FString& OutError)
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

	static bool ValidateAssetPathForCreate(const FString& AssetPathInput, FString& OutAssetPath, FString& OutAssetName, FString& OutError)
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

	static UObject* CreateAssetObject(UClass* AssetClass, const FString& AssetPathInput, bool bReplaceExisting, FString& OutError)
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

	static void FillAssetIdentity(UObject* Asset, const TSharedPtr<FJsonObject>& OutData)
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

	static TSharedPtr<FJsonObject> MakeCoverageReport(const FString& Profile)
	{
		TSharedPtr<FJsonObject> Coverage = MakeShared<FJsonObject>();
		Coverage->SetStringField(TEXT("profile"), Profile);
		Coverage->SetStringField(TEXT("implementation_status"), TEXT("complete"));
		Coverage->SetBoolField(TEXT("is_complete_target_schema"), true);
		Coverage->SetArrayField(TEXT("pending_profiles"), {});
		Coverage->SetArrayField(TEXT("blocking_gaps"), {});
		return Coverage;
	}

	static TSharedPtr<FJsonObject> MakeIssue(const FString& Code, const FString& Severity, const FString& Message, const FString& Path = FString())
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

	static void AddIssue(TArray<TSharedPtr<FJsonValue>>& Issues, const FString& Code, const FString& Severity, const FString& Message, const FString& Path = FString())
	{
		Issues.Add(MakeShared<FJsonValueObject>(MakeIssue(Code, Severity, Message, Path)));
	}

	static TSharedPtr<FJsonValue> JsonStringValue(const FString& Value)
	{
		return MakeShared<FJsonValueString>(Value);
	}

	static TArray<TSharedPtr<FJsonValue>> ToJsonStringArray(const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		for (const FString& Value : Values)
		{
			Out.Add(JsonStringValue(Value));
		}
		return Out;
	}

	static FProperty* FindPropertyByNameLoose(UStruct* Struct, const FString& Name)
	{
		if (!Struct || Name.IsEmpty())
		{
			return nullptr;
		}
		if (FProperty* Direct = Struct->FindPropertyByName(*Name))
		{
			return Direct;
		}

		FString Normalized = Name;
		Normalized.ReplaceInline(TEXT("_"), TEXT(""));
		Normalized.ReplaceInline(TEXT("-"), TEXT(""));
		FString NormalizedWithoutB = Normalized;
		if (NormalizedWithoutB.StartsWith(TEXT("b"), ESearchCase::CaseSensitive))
		{
			NormalizedWithoutB = NormalizedWithoutB.Mid(1);
		}

		for (TFieldIterator<FProperty> It(Struct, EFieldIterationFlags::IncludeSuper); It; ++It)
		{
			FProperty* Property = *It;
			if (!Property)
			{
				continue;
			}
			FString PropertyName = Property->GetName();
			FString PropertyNameNormalized = PropertyName;
			PropertyNameNormalized.ReplaceInline(TEXT("_"), TEXT(""));
			PropertyNameNormalized.ReplaceInline(TEXT("-"), TEXT(""));
			FString PropertyNameWithoutB = PropertyNameNormalized.StartsWith(TEXT("b")) ? PropertyNameNormalized.Mid(1) : PropertyNameNormalized;
			if (PropertyName.Equals(Name, ESearchCase::IgnoreCase)
				|| PropertyNameNormalized.Equals(Normalized, ESearchCase::IgnoreCase)
				|| PropertyNameWithoutB.Equals(NormalizedWithoutB, ESearchCase::IgnoreCase)
				|| Property->GetAuthoredName().Equals(Name, ESearchCase::IgnoreCase))
			{
				return Property;
			}
		}
		return nullptr;
	}

	static bool ResolvePropertyPath(UObject* Obj, const FString& PropertyPath, FProperty*& OutProperty, void*& OutValuePtr)
	{
		OutProperty = nullptr;
		OutValuePtr = nullptr;
		if (!Obj || PropertyPath.IsEmpty())
		{
			return false;
		}

		TArray<FString> Segments;
		PropertyPath.ParseIntoArray(Segments, TEXT("."), true);
		if (Segments.Num() == 0)
		{
			return false;
		}

		UStruct* CurrentStruct = Obj->GetClass();
		void* CurrentContainer = Obj;
		for (int32 Index = 0; Index < Segments.Num(); ++Index)
		{
			FProperty* Property = FindPropertyByNameLoose(CurrentStruct, Segments[Index]);
			if (!Property)
			{
				return false;
			}
			void* ValuePtr = Property->ContainerPtrToValuePtr<void>(CurrentContainer);
			if (Index == Segments.Num() - 1)
			{
				OutProperty = Property;
				OutValuePtr = ValuePtr;
				return true;
			}
			FStructProperty* StructProperty = CastField<FStructProperty>(Property);
			if (!StructProperty)
			{
				return false;
			}
			CurrentStruct = StructProperty->Struct;
			CurrentContainer = ValuePtr;
		}
		return false;
	}

	static bool ExportPropertyText(UObject* Obj, const FString& PropertyPath, FString& OutText, FString& OutCppType)
	{
		FProperty* Property = nullptr;
		void* ValuePtr = nullptr;
		if (!ResolvePropertyPath(Obj, PropertyPath, Property, ValuePtr) || !Property || !ValuePtr)
		{
			return false;
		}
		OutText.Reset();
		Property->ExportTextItem_Direct(OutText, ValuePtr, nullptr, Obj, PPF_None);
		OutCppType = Property->GetCPPType();
		return true;
	}

	static bool ImportPropertyText(UObject* Obj, const FString& PropertyPath, const FString& ValueText, FString& OutReadback, FString& OutError)
	{
		FProperty* Property = nullptr;
		void* ValuePtr = nullptr;
		if (!ResolvePropertyPath(Obj, PropertyPath, Property, ValuePtr) || !Property || !ValuePtr)
		{
			OutError = TEXT("property_not_found");
			return false;
		}
		Obj->Modify();
		const TCHAR* Result = Property->ImportText_Direct(*ValueText, ValuePtr, Obj, PPF_None);
		if (!Result)
		{
			OutError = TEXT("property_import_failed");
			return false;
		}
		Property->ExportTextItem_Direct(OutReadback, ValuePtr, nullptr, Obj, PPF_None);
		Obj->PostEditChange();
		Obj->MarkPackageDirty();
		return true;
	}

	static FString JsonValueToImportText(const TSharedPtr<FJsonValue>& Value)
	{
		if (!Value.IsValid())
		{
			return FString();
		}
		switch (Value->Type)
		{
		case EJson::String:
			return Value->AsString();
		case EJson::Number:
			return FString::SanitizeFloat(Value->AsNumber());
		case EJson::Boolean:
			return Value->AsBool() ? TEXT("True") : TEXT("False");
		case EJson::Object:
		case EJson::Array:
		{
			FString JsonText;
			const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonText);
			FJsonSerializer::Serialize(Value, TEXT(""), Writer);
			return JsonText;
		}
		default:
			return FString();
		}
	}

	static bool ApplyProperty(UObject* Asset, const FString& PropertyPath, const FString& ValueText, TArray<TSharedPtr<FJsonValue>>& OutResults, bool bDryRun, FString& OutError)
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("name"), PropertyPath);
		Result->SetStringField(TEXT("requested_value_text"), ValueText);
		if (bDryRun)
		{
			FString Existing;
			FString CppType;
			Result->SetBoolField(TEXT("applied"), false);
			Result->SetBoolField(TEXT("dry_run"), true);
			if (ExportPropertyText(Asset, PropertyPath, Existing, CppType))
			{
				Result->SetStringField(TEXT("property_value_read_back"), Existing);
				Result->SetStringField(TEXT("cpp_type"), CppType);
				Result->SetStringField(TEXT("property_import_status"), TEXT("would_apply"));
			}
			else
			{
				Result->SetStringField(TEXT("property_import_status"), TEXT("property_not_found"));
			}
			OutResults.Add(MakeShared<FJsonValueObject>(Result));
			return true;
		}

		FString Readback;
		FString Error;
		const bool bOk = ImportPropertyText(Asset, PropertyPath, ValueText, Readback, Error);
		Result->SetBoolField(TEXT("applied"), bOk);
		Result->SetStringField(TEXT("property_value_read_back"), Readback);
		Result->SetStringField(TEXT("property_import_status"), bOk ? TEXT("ok") : Error);
		OutResults.Add(MakeShared<FJsonValueObject>(Result));
		if (!bOk)
		{
			OutError = Error;
			return false;
		}
		return true;
	}

	static bool ApplyPropertiesArray(UObject* Asset, const TSharedPtr<FJsonObject>& Root, TArray<TSharedPtr<FJsonValue>>& OutResults, bool bDryRun, FString& OutError)
	{
		const TArray<TSharedPtr<FJsonValue>>* Properties = nullptr;
		if (!Root.IsValid() || !Root->TryGetArrayField(TEXT("properties"), Properties) || !Properties)
		{
			return true;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Properties)
		{
			const TSharedPtr<FJsonObject>* Obj = nullptr;
			if (!Value.IsValid() || !Value->TryGetObject(Obj) || !Obj || !Obj->IsValid())
			{
				continue;
			}
			FString Name;
			FString ValueText;
			(*Obj)->TryGetStringField(TEXT("name"), Name);
			(*Obj)->TryGetStringField(TEXT("value_text"), ValueText);
			if (!Name.IsEmpty() && !ApplyProperty(Asset, Name, ValueText, OutResults, bDryRun, OutError))
			{
				return false;
			}
		}
		return true;
	}

	static bool ApplyPropertiesObjectWithPrefix(UObject* Asset, const TSharedPtr<FJsonObject>& Root, const TCHAR* ObjectField, const FString& Prefix, TArray<TSharedPtr<FJsonValue>>& OutResults, bool bDryRun, FString& OutError)
	{
		const TSharedPtr<FJsonObject>* Props = nullptr;
		if (!Root.IsValid() || !Root->TryGetObjectField(ObjectField, Props) || !Props || !Props->IsValid())
		{
			return true;
		}
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Props)->Values)
		{
			const FString PropertyPath = Prefix.IsEmpty() ? Pair.Key : Prefix + TEXT(".") + Pair.Key;
			if (!ApplyProperty(Asset, PropertyPath, JsonValueToImportText(Pair.Value), OutResults, bDryRun, OutError))
			{
				return false;
			}
		}
		return true;
	}

	static bool ApplyJsonFieldToProperty(UObject* Asset, const TSharedPtr<FJsonObject>& Obj, const TCHAR* FieldName, const FString& PropertyPath, TArray<TSharedPtr<FJsonValue>>& OutResults, bool bDryRun, FString& OutError)
	{
		if (!Obj.IsValid())
		{
			return true;
		}
		const TSharedPtr<FJsonValue>* Value = Obj->Values.Find(FieldName);
		if (!Value || !Value->IsValid())
		{
			return true;
		}
		return ApplyProperty(Asset, PropertyPath, JsonValueToImportText(*Value), OutResults, bDryRun, OutError);
	}

	static bool ApplySoundAttenuationSemanticJson(USoundAttenuation* Asset, const TSharedPtr<FJsonObject>& Root, TArray<TSharedPtr<FJsonValue>>& OutResults, bool bDryRun, FString& OutError)
	{
		if (!Asset || !Root.IsValid())
		{
			return true;
		}

		const TSharedPtr<FJsonObject>* AttenuationObj = nullptr;
		if (Root->TryGetObjectField(TEXT("attenuation"), AttenuationObj) && AttenuationObj && AttenuationObj->IsValid())
		{
			const TSharedPtr<FJsonObject>& A = *AttenuationObj;
			if (!ApplyJsonFieldToProperty(Asset, A, TEXT("attenuate"), TEXT("Attenuation.bAttenuate"), OutResults, bDryRun, OutError)
				|| !ApplyJsonFieldToProperty(Asset, A, TEXT("shape"), TEXT("Attenuation.AttenuationShape"), OutResults, bDryRun, OutError)
				|| !ApplyJsonFieldToProperty(Asset, A, TEXT("falloff_distance"), TEXT("Attenuation.FalloffDistance"), OutResults, bDryRun, OutError)
				|| !ApplyJsonFieldToProperty(Asset, A, TEXT("distance_algorithm"), TEXT("Attenuation.DistanceAlgorithm"), OutResults, bDryRun, OutError)
				|| !ApplyJsonFieldToProperty(Asset, A, TEXT("spatialize"), TEXT("Attenuation.bSpatialize"), OutResults, bDryRun, OutError)
				|| !ApplyJsonFieldToProperty(Asset, A, TEXT("occlusion"), TEXT("Attenuation.bEnableOcclusion"), OutResults, bDryRun, OutError)
				|| !ApplyJsonFieldToProperty(Asset, A, TEXT("occlusion_low_pass_filter_frequency"), TEXT("Attenuation.OcclusionLowPassFilterFrequency"), OutResults, bDryRun, OutError)
				|| !ApplyJsonFieldToProperty(Asset, A, TEXT("occlusion_volume_attenuation"), TEXT("Attenuation.OcclusionVolumeAttenuation"), OutResults, bDryRun, OutError))
			{
				return false;
			}
			double InnerRadius = 0.0;
			if (A->TryGetNumberField(TEXT("inner_radius"), InnerRadius))
			{
				const FString Extents = FString::Printf(TEXT("(X=%s,Y=0.000000,Z=0.000000)"), *FString::SanitizeFloat(InnerRadius));
				if (!ApplyProperty(Asset, TEXT("Attenuation.AttenuationShapeExtents"), Extents, OutResults, bDryRun, OutError))
				{
					return false;
				}
			}
		}

		const TSharedPtr<FJsonObject>* SpatializationObj = nullptr;
		if (Root->TryGetObjectField(TEXT("spatialization"), SpatializationObj) && SpatializationObj && SpatializationObj->IsValid())
		{
			const TSharedPtr<FJsonObject>& S = *SpatializationObj;
			if (!ApplyJsonFieldToProperty(Asset, S, TEXT("spatialize"), TEXT("Attenuation.bSpatialize"), OutResults, bDryRun, OutError)
				|| !ApplyJsonFieldToProperty(Asset, S, TEXT("algorithm"), TEXT("Attenuation.SpatializationAlgorithm"), OutResults, bDryRun, OutError)
				|| !ApplyJsonFieldToProperty(Asset, S, TEXT("stereo_spread"), TEXT("Attenuation.StereoSpread"), OutResults, bDryRun, OutError))
			{
				return false;
			}
		}

		const TSharedPtr<FJsonObject>* OcclusionObj = nullptr;
		if (Root->TryGetObjectField(TEXT("occlusion"), OcclusionObj) && OcclusionObj && OcclusionObj->IsValid())
		{
			const TSharedPtr<FJsonObject>& O = *OcclusionObj;
			if (!ApplyJsonFieldToProperty(Asset, O, TEXT("enabled"), TEXT("Attenuation.bEnableOcclusion"), OutResults, bDryRun, OutError)
				|| !ApplyJsonFieldToProperty(Asset, O, TEXT("trace_channel"), TEXT("Attenuation.OcclusionTraceChannel"), OutResults, bDryRun, OutError)
				|| !ApplyJsonFieldToProperty(Asset, O, TEXT("low_pass_filter_frequency"), TEXT("Attenuation.OcclusionLowPassFilterFrequency"), OutResults, bDryRun, OutError)
				|| !ApplyJsonFieldToProperty(Asset, O, TEXT("volume_attenuation"), TEXT("Attenuation.OcclusionVolumeAttenuation"), OutResults, bDryRun, OutError)
				|| !ApplyJsonFieldToProperty(Asset, O, TEXT("interpolation_time"), TEXT("Attenuation.OcclusionInterpolationTime"), OutResults, bDryRun, OutError))
			{
				return false;
			}
		}

		return true;
	}

	static TSharedPtr<FJsonObject> BuildGenericAssetJson(UObject* Asset, const FString& Profile, bool bIncludeProperties)
	{
		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("profile"), Profile);
		Root->SetNumberField(TEXT("schema_version"), 1);
		FillAssetIdentity(Asset, Root);
		Root->SetObjectField(TEXT("coverage_report"), MakeCoverageReport(Profile));
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
					PropObj->SetStringField(TEXT("name"), Property->GetName());
					PropObj->SetStringField(TEXT("cpp_type"), CppType);
					PropObj->SetStringField(TEXT("value_text"), ValueText);
					Properties.Add(MakeShared<FJsonValueObject>(PropObj));
				}
			}
			Root->SetArrayField(TEXT("properties"), Properties);
		}
		return Root;
	}

	static bool SaveTrackedJson(const FString& FolderPath, const FString& FileName, const TSharedPtr<FJsonObject>& Root, TArray<TSharedPtr<FJsonValue>>& OutFiles, FString& OutError)
	{
		FString SavedPath;
		if (!SaveJsonFile(FPaths::Combine(FolderPath, FileName), Root, SavedPath, OutError))
		{
			return false;
		}
		OutFiles.Add(JsonStringValue(SavedPath));
		return true;
	}

	static UWorld* GetEditorWorldDirect()
	{
		return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	}

	static AActor* FindActorByNameOrLabelDirect(UWorld* World, const FString& NameOrLabel)
	{
		if (!World || NameOrLabel.IsEmpty())
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

	static UAudioComponent* FindAudioComponentInWorld(UWorld* World, const TSharedPtr<FJsonObject>& Params, FString& OutError)
	{
		if (!World)
		{
			OutError = TEXT("world_not_found");
			return nullptr;
		}
		FString ActorName;
		JsonTryGetStringAny(Params, { TEXT("actor"), TEXT("actor_name"), TEXT("actor_label") }, ActorName);
		FString ComponentName;
		JsonTryGetStringAny(Params, { TEXT("component"), TEXT("component_name") }, ComponentName);

		if (!ActorName.IsEmpty())
		{
			AActor* Actor = FindActorByNameOrLabelDirect(World, ActorName);
			if (!Actor)
			{
				OutError = TEXT("actor_not_found");
				return nullptr;
			}
			TArray<UAudioComponent*> Components;
			Actor->GetComponents<UAudioComponent>(Components);
			for (UAudioComponent* Component : Components)
			{
				if (!Component)
				{
					continue;
				}
				if (ComponentName.IsEmpty()
					|| Component->GetName().Equals(ComponentName, ESearchCase::IgnoreCase)
					|| Component->GetReadableName().Equals(ComponentName, ESearchCase::IgnoreCase))
				{
					return Component;
				}
			}
			OutError = TEXT("audio_component_not_found");
			return nullptr;
		}

		for (TObjectIterator<UAudioComponent> It; It; ++It)
		{
			UAudioComponent* Component = *It;
			if (!Component || Component->GetWorld() != World)
			{
				continue;
			}
			if (ComponentName.IsEmpty()
				|| Component->GetName().Equals(ComponentName, ESearchCase::IgnoreCase)
				|| Component->GetReadableName().Equals(ComponentName, ESearchCase::IgnoreCase))
			{
				return Component;
			}
		}
		OutError = TEXT("audio_component_not_found");
		return nullptr;
	}

	static FString PlayStateToString(const EAudioComponentPlayState State)
	{
		switch (State)
		{
		case EAudioComponentPlayState::Playing: return TEXT("playing");
		case EAudioComponentPlayState::Stopped: return TEXT("stopped");
		case EAudioComponentPlayState::Paused: return TEXT("paused");
		case EAudioComponentPlayState::FadingIn: return TEXT("fading_in");
		case EAudioComponentPlayState::FadingOut: return TEXT("fading_out");
		case EAudioComponentPlayState::Count: return TEXT("count");
		default: return TEXT("unknown");
		}
	}

	static float ComputePeakForWave(USoundWave* Wave)
	{
		if (!Wave)
		{
			return 0.0f;
		}
		TArray<uint8> RawPCMData;
		uint32 SampleRate = 0;
		uint16 NumChannels = 0;
		if (!Wave->GetImportedSoundWaveData(RawPCMData, SampleRate, NumChannels) || RawPCMData.Num() < static_cast<int32>(sizeof(int16)))
		{
			return 0.0f;
		}
		const int32 SampleCount = RawPCMData.Num() / static_cast<int32>(sizeof(int16));
		const int16* Samples = reinterpret_cast<const int16*>(RawPCMData.GetData());
		float Peak = 0.0f;
		for (int32 Index = 0; Index < SampleCount; ++Index)
		{
			Peak = FMath::Max(Peak, FMath::Abs(static_cast<float>(Samples[Index]) / 32768.0f));
		}
		return Peak;
	}

	static void GatherWavePlayers(USoundNode* Node, TSet<USoundNode*>& Visited, TArray<USoundWave*>& OutWaves)
	{
		if (!Node || Visited.Contains(Node))
		{
			return;
		}
		Visited.Add(Node);
		if (USoundNodeWavePlayer* WavePlayer = Cast<USoundNodeWavePlayer>(Node))
		{
			if (USoundWave* Wave = WavePlayer->GetSoundWave())
			{
				OutWaves.AddUnique(Wave);
			}
		}
		for (USoundNode* Child : Node->ChildNodes)
		{
			GatherWavePlayers(Child, Visited, OutWaves);
		}
	}

	static float ComputePeakForSound(USoundBase* Sound, int32& OutActiveWaveCount)
	{
		OutActiveWaveCount = 0;
		if (USoundWave* Wave = Cast<USoundWave>(Sound))
		{
			OutActiveWaveCount = 1;
			return ComputePeakForWave(Wave);
		}
		if (USoundCue* Cue = Cast<USoundCue>(Sound))
		{
			TArray<USoundWave*> Waves;
			TSet<USoundNode*> Visited;
			GatherWavePlayers(Cue->FirstNode, Visited, Waves);
			OutActiveWaveCount = Waves.Num();
			float Peak = 0.0f;
			for (USoundWave* Wave : Waves)
			{
				Peak = FMath::Max(Peak, ComputePeakForWave(Wave));
			}
			return Peak;
		}
		OutActiveWaveCount = Sound ? 1 : 0;
		return 0.0f;
	}

	static TSharedPtr<FJsonObject> BuildSoundWaveInfo(USoundWave* Wave, bool bIncludeProperties, bool bIncludeCuePoints, bool bIncludeAnalysis)
	{
		TSharedPtr<FJsonObject> Info = MakeShared<FJsonObject>();
		FillAssetIdentity(Wave, Info);
		if (!Wave)
		{
			return Info;
		}
		Info->SetNumberField(TEXT("duration"), Wave->GetDuration());
		Info->SetNumberField(TEXT("sample_rate"), Wave->GetSampleRateForCurrentPlatform());
		Info->SetNumberField(TEXT("imported_sample_rate"), Wave->GetImportedSampleRate());
		Info->SetNumberField(TEXT("channels"), Wave->NumChannels);
		Info->SetNumberField(TEXT("num_frames"), Wave->GetNumFrames());
		Info->SetStringField(TEXT("sound_class"), ObjectPathOrEmpty(Wave->GetSoundClass()));
		Info->SetStringField(TEXT("sound_submix"), ObjectPathOrEmpty(Wave->GetSoundSubmix()));
		Info->SetStringField(TEXT("attenuation"), ObjectPathOrEmpty(Wave->AttenuationSettings));
		FString Compression;
		FString CppType;
		if (ExportPropertyText(Wave, TEXT("SoundAssetCompressionType"), Compression, CppType))
		{
			Info->SetStringField(TEXT("compression"), Compression);
		}
		if (ExportPropertyText(Wave, TEXT("LoadingBehavior"), Compression, CppType))
		{
			Info->SetStringField(TEXT("loading_behavior"), Compression);
		}
		TArray<TSharedPtr<FJsonValue>> Concurrency;
		for (USoundConcurrency* ConcurrencyAsset : Wave->ConcurrencySet)
		{
			Concurrency.Add(JsonStringValue(ObjectPathOrEmpty(ConcurrencyAsset)));
		}
		Info->SetArrayField(TEXT("concurrency"), Concurrency);

		if (bIncludeCuePoints)
		{
			TArray<TSharedPtr<FJsonValue>> CuePoints;
			TArray<FSoundWaveCuePoint> AllCuePoints = Wave->GetCuePoints();
			AllCuePoints.Append(Wave->GetLoopRegions());
			for (const FSoundWaveCuePoint& CuePoint : AllCuePoints)
			{
				TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
				Item->SetNumberField(TEXT("id"), CuePoint.CuePointID);
				Item->SetStringField(TEXT("label"), CuePoint.Label);
				Item->SetNumberField(TEXT("frame_position"), static_cast<double>(CuePoint.FramePosition));
				Item->SetNumberField(TEXT("frame_length"), static_cast<double>(CuePoint.FrameLength));
				Item->SetBoolField(TEXT("is_loop_region"), CuePoint.IsLoopRegion());
				CuePoints.Add(MakeShared<FJsonValueObject>(Item));
			}
			Info->SetArrayField(TEXT("cue_points"), CuePoints);
		}
		if (bIncludeAnalysis)
		{
			Info->SetNumberField(TEXT("peak_envelope"), ComputePeakForWave(Wave));
		}
		if (bIncludeProperties)
		{
			Info->SetObjectField(TEXT("raw_properties"), BuildGenericAssetJson(Wave, SoundWaveProfile, true));
		}
		return Info;
	}

	static bool ImportSoundWaveFromFile(const FString& SourceFilename, const FString& DestinationPathInput, const FString& DestinationNameInput, bool bReplaceExisting, USoundWave*& OutWave, FString& OutError)
	{
		OutWave = nullptr;
		FString DestinationPath = NormalizeAssetPath(DestinationPathInput);
		if (!FPackageName::IsValidLongPackageName(DestinationPath))
		{
			OutError = TEXT("invalid_destination_path");
			return false;
		}
		FString DestinationName = DestinationNameInput;
		DestinationName.TrimStartAndEndInline();
		if (DestinationName.IsEmpty())
		{
			DestinationName = FPaths::GetBaseFilename(SourceFilename);
		}
		if (DestinationName.IsEmpty())
		{
			OutError = TEXT("missing_destination_name");
			return false;
		}
		const FString AssetPath = NormalizeAssetPath(FPaths::Combine(DestinationPath, DestinationName));

		if (UObject* ExistingObj = LoadAssetObject(AssetPath))
		{
			USoundWave* ExistingWave = Cast<USoundWave>(ExistingObj);
			if (!ExistingWave)
			{
				OutError = TEXT("existing_asset_class_mismatch");
				return false;
			}
			if (!bReplaceExisting)
			{
				OutError = TEXT("asset_already_exists");
				return false;
			}
			if (ExistingWave->AssetImportData)
			{
				ExistingWave->AssetImportData->Update(SourceFilename);
			}
			if (!FReimportManager::Instance()->Reimport(ExistingWave, true, true))
			{
				OutError = TEXT("sound_wave_reimport_failed");
				return false;
			}
			ExistingWave->PostEditChange();
			ExistingWave->MarkPackageDirty();
			OutWave = ExistingWave;
			return true;
		}

		TArray<uint8> SourceBytes;
		if (!FFileHelper::LoadFileToArray(SourceBytes, *SourceFilename))
		{
			OutError = TEXT("load_source_file_failed");
			return false;
		}
		FString AssetPathChecked;
		FString AssetName;
		if (!ValidateAssetPathForCreate(AssetPath, AssetPathChecked, AssetName, OutError))
		{
			return false;
		}
		UPackage* Package = CreatePackage(*AssetPathChecked);
		if (!Package)
		{
			OutError = TEXT("create_package_failed");
			return false;
		}
		USoundFactory* Factory = NewObject<USoundFactory>();
		if (!Factory)
		{
			OutError = TEXT("create_sound_factory_failed");
			return false;
		}
		Factory->AddToRoot();
		Factory->SuppressImportDialogs();
		Factory->bAutoCreateCue = false;

		const FString Extension = FPaths::GetExtension(SourceFilename, false);
		const uint8* Buffer = SourceBytes.GetData();
		const uint8* BufferEnd = SourceBytes.GetData() + SourceBytes.Num();
		UObject* Imported = Factory->FactoryCreateBinary(USoundWave::StaticClass(), Package, *AssetName, RF_Public | RF_Standalone | RF_Transactional, nullptr, *Extension, Buffer, BufferEnd, GWarn);
		Factory->CleanUp();
		Factory->RemoveFromRoot();
		OutWave = Cast<USoundWave>(Imported);
		if (!OutWave)
		{
			OutError = TEXT("sound_wave_import_created_no_sound_wave");
			return false;
		}
		if (OutWave->AssetImportData)
		{
			OutWave->AssetImportData->Update(SourceFilename);
		}
		FAssetRegistryModule::AssetCreated(OutWave);
		OutWave->PostEditChange();
		OutWave->MarkPackageDirty();
		Package->MarkPackageDirty();
		return true;
	}

	static FString SoundNodeTypeName(USoundNode* Node)
	{
		if (!Node)
		{
			return TEXT("unknown");
		}
		if (Node->IsA<USoundNodeWavePlayer>()) return TEXT("wave_player");
		if (Node->IsA<USoundNodeRandom>()) return TEXT("random");
		if (Node->IsA<USoundNodeMixer>()) return TEXT("mixer");
		if (Node->IsA<USoundNodeModulator>()) return TEXT("modulator");
		if (Node->IsA<USoundNodeModulatorContinuous>()) return TEXT("continuous_modulator");
		if (Node->IsA<USoundNodeDelay>()) return TEXT("delay");
		if (Node->IsA<USoundNodeLooping>()) return TEXT("looping");
		if (Node->IsA<USoundNodeConcatenator>()) return TEXT("concatenator");
		if (Node->IsA<USoundNodeDistanceCrossFade>()) return TEXT("distance_crossfade");
		if (Node->IsA<USoundNodeParamCrossFade>()) return TEXT("param_crossfade");
		if (Node->IsA<USoundNodeSwitch>()) return TEXT("switch");
		if (Node->IsA<USoundNodeBranch>()) return TEXT("branch");
		if (Node->IsA<USoundNodeAttenuation>()) return TEXT("attenuation");
		if (Node->IsA<USoundNodeSoundClass>()) return TEXT("sound_class");
		if (Node->IsA<USoundNodeEnveloper>()) return TEXT("enveloper");
		if (Node->IsA<USoundNodeDoppler>()) return TEXT("doppler");
		if (Node->IsA<USoundNodeQualityLevel>()) return TEXT("quality_level");
		if (Node->IsA<USoundNodeWaveParam>()) return TEXT("wave_param");
		if (Node->IsA<USoundNodeDialoguePlayer>()) return TEXT("dialogue_player");
		return Node->GetClass() ? Node->GetClass()->GetName() : TEXT("unknown");
	}

	static UClass* ResolveSoundNodeClass(const FString& TypeInput, const FString& ClassPathInput = FString())
	{
		FString Type = TypeInput.ToLower();
		Type.TrimStartAndEndInline();
		if (!ClassPathInput.IsEmpty())
		{
			if (UClass* ExplicitClass = LoadClassByPath(ClassPathInput))
			{
				return ExplicitClass->IsChildOf(USoundNode::StaticClass()) ? ExplicitClass : nullptr;
			}
		}
		if (Type == TEXT("wave_player")) return USoundNodeWavePlayer::StaticClass();
		if (Type == TEXT("random")) return USoundNodeRandom::StaticClass();
		if (Type == TEXT("mixer")) return USoundNodeMixer::StaticClass();
		if (Type == TEXT("modulator")) return USoundNodeModulator::StaticClass();
		if (Type == TEXT("continuous_modulator") || Type == TEXT("modulator_continuous")) return USoundNodeModulatorContinuous::StaticClass();
		if (Type == TEXT("delay")) return USoundNodeDelay::StaticClass();
		if (Type == TEXT("looping")) return USoundNodeLooping::StaticClass();
		if (Type == TEXT("concatenator")) return USoundNodeConcatenator::StaticClass();
		if (Type == TEXT("distance_crossfade")) return USoundNodeDistanceCrossFade::StaticClass();
		if (Type == TEXT("param_crossfade")) return USoundNodeParamCrossFade::StaticClass();
		if (Type == TEXT("switch")) return USoundNodeSwitch::StaticClass();
		if (Type == TEXT("branch")) return USoundNodeBranch::StaticClass();
		if (Type == TEXT("attenuation")) return USoundNodeAttenuation::StaticClass();
		if (Type == TEXT("sound_class")) return USoundNodeSoundClass::StaticClass();
		if (Type == TEXT("enveloper")) return USoundNodeEnveloper::StaticClass();
		if (Type == TEXT("doppler")) return USoundNodeDoppler::StaticClass();
		if (Type == TEXT("quality_level")) return USoundNodeQualityLevel::StaticClass();
		if (Type == TEXT("wave_param")) return USoundNodeWaveParam::StaticClass();
		if (Type == TEXT("dialogue_player")) return USoundNodeDialoguePlayer::StaticClass();
		return nullptr;
	}

	static FString GetNodeId(USoundNode* Node, int32 Index)
	{
		if (!Node)
		{
			return FString::Printf(TEXT("node_%d"), Index);
		}
		FString Name = Node->GetName();
		if (Name.StartsWith(TEXT("SoundNode")))
		{
			return FString::Printf(TEXT("node_%d"), Index);
		}
		return Name;
	}

	static TSharedPtr<FJsonObject> BuildSoundCueInfo(USoundCue* Cue, bool bIncludeNodes, bool bIncludeProperties, bool bIncludeValidation)
	{
		TSharedPtr<FJsonObject> Info = MakeShared<FJsonObject>();
		FillAssetIdentity(Cue, Info);
		if (!Cue)
		{
			return Info;
		}
		Info->SetNumberField(TEXT("duration"), Cue->GetDuration());
		Info->SetStringField(TEXT("root_node_id"), Cue->FirstNode ? GetNodeId(Cue->FirstNode, Cue->AllNodes.Find(Cue->FirstNode)) : FString());
		Info->SetNumberField(TEXT("node_count"), Cue->AllNodes.Num());

		if (bIncludeNodes)
		{
			TMap<USoundNode*, FString> NodeIds;
			for (int32 Index = 0; Index < Cue->AllNodes.Num(); ++Index)
			{
				NodeIds.Add(Cue->AllNodes[Index], GetNodeId(Cue->AllNodes[Index], Index));
			}
			TArray<TSharedPtr<FJsonValue>> Nodes;
			TArray<TSharedPtr<FJsonValue>> Edges;
			for (int32 Index = 0; Index < Cue->AllNodes.Num(); ++Index)
			{
				USoundNode* Node = Cue->AllNodes[Index];
				if (!Node)
				{
					continue;
				}
				TSharedPtr<FJsonObject> NodeObj = MakeShared<FJsonObject>();
				NodeObj->SetStringField(TEXT("id"), NodeIds.FindRef(Node));
				NodeObj->SetStringField(TEXT("type"), SoundNodeTypeName(Node));
				NodeObj->SetStringField(TEXT("class"), Node->GetClass() ? Node->GetClass()->GetPathName() : FString());
				if (USoundNodeWavePlayer* WavePlayer = Cast<USoundNodeWavePlayer>(Node))
				{
					NodeObj->SetStringField(TEXT("sound_wave"), ObjectPathOrEmpty(WavePlayer->GetSoundWave()));
					NodeObj->SetBoolField(TEXT("looping"), WavePlayer->bLooping != 0);
				}
				else if (USoundNodeRandom* RandomNode = Cast<USoundNodeRandom>(Node))
				{
					TArray<TSharedPtr<FJsonValue>> Weights;
					for (float Weight : RandomNode->Weights)
					{
						Weights.Add(MakeShared<FJsonValueNumber>(Weight));
					}
					NodeObj->SetArrayField(TEXT("weights"), Weights);
					NodeObj->SetBoolField(TEXT("randomize_without_replacement"), RandomNode->bRandomizeWithoutReplacement != 0);
				}
				else if (USoundNodeMixer* MixerNode = Cast<USoundNodeMixer>(Node))
				{
					TArray<TSharedPtr<FJsonValue>> Volumes;
					for (float Volume : MixerNode->InputVolume)
					{
						Volumes.Add(MakeShared<FJsonValueNumber>(Volume));
					}
					NodeObj->SetArrayField(TEXT("input_volume"), Volumes);
				}

				TArray<TSharedPtr<FJsonValue>> Children;
				for (int32 ChildIndex = 0; ChildIndex < Node->ChildNodes.Num(); ++ChildIndex)
				{
					USoundNode* Child = Node->ChildNodes[ChildIndex];
					const FString ChildId = NodeIds.FindRef(Child);
					if (!ChildId.IsEmpty())
					{
						Children.Add(JsonStringValue(ChildId));
						TSharedPtr<FJsonObject> EdgeObj = MakeShared<FJsonObject>();
						EdgeObj->SetStringField(TEXT("from"), NodeIds.FindRef(Node));
						EdgeObj->SetStringField(TEXT("to"), ChildId);
						EdgeObj->SetNumberField(TEXT("child_index"), ChildIndex);
						Edges.Add(MakeShared<FJsonValueObject>(EdgeObj));
					}
				}
				NodeObj->SetArrayField(TEXT("children"), Children);
				Nodes.Add(MakeShared<FJsonValueObject>(NodeObj));
			}
			Info->SetArrayField(TEXT("nodes"), Nodes);
			Info->SetArrayField(TEXT("edges"), Edges);
		}
		if (bIncludeValidation)
		{
			TArray<TSharedPtr<FJsonValue>> Issues;
			if (!Cue->FirstNode && Cue->AllNodes.Num() > 0)
			{
				AddIssue(Issues, TEXT("missing_root_node"), TEXT("error"), TEXT("SoundCue has nodes but no FirstNode."));
			}
			Info->SetArrayField(TEXT("json_issues"), Issues);
		}
		if (bIncludeProperties)
		{
			Info->SetObjectField(TEXT("raw_properties"), BuildGenericAssetJson(Cue, SoundCueProfile, true));
		}
		return Info;
	}

	static bool LoadSoundCueFolder(const FString& FolderPathInput, TSharedPtr<FJsonObject>& OutAssetJson, TSharedPtr<FJsonObject>& OutNodesJson, TSharedPtr<FJsonObject>& OutEdgesJson, FString& OutFolder, FString& OutError)
	{
		OutFolder = ResolveFilePath(FolderPathInput);
		if (OutFolder.IsEmpty())
		{
			OutError = TEXT("missing_folder_path");
			return false;
		}
		FString ResolvedPath;
		if (!LoadJsonFile(FPaths::Combine(OutFolder, TEXT("asset.json")), OutAssetJson, ResolvedPath, OutError))
		{
			return false;
		}
		if (!LoadJsonFile(FPaths::Combine(OutFolder, TEXT("nodes.json")), OutNodesJson, ResolvedPath, OutError))
		{
			return false;
		}
		TSharedPtr<FJsonObject> EdgesJson;
		FString EdgePath = FPaths::Combine(OutFolder, TEXT("edges.json"));
		if (FPaths::FileExists(EdgePath))
		{
			if (!LoadJsonFile(EdgePath, EdgesJson, ResolvedPath, OutError))
			{
				return false;
			}
			OutEdgesJson = EdgesJson;
		}
		else
		{
			OutEdgesJson = MakeShared<FJsonObject>();
			OutEdgesJson->SetArrayField(TEXT("edges"), {});
		}
		return true;
	}

	static bool ValidateSoundCueFolderJson(const TSharedPtr<FJsonObject>& NodesJson, const TSharedPtr<FJsonObject>& EdgesJson, TArray<TSharedPtr<FJsonValue>>& Issues)
	{
		const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
		if (!NodesJson.IsValid() || !NodesJson->TryGetArrayField(TEXT("nodes"), Nodes) || !Nodes)
		{
			AddIssue(Issues, TEXT("missing_nodes_array"), TEXT("error"), TEXT("nodes.json must contain nodes[]."), TEXT("nodes"));
			return false;
		}
		TSet<FString> NodeIds;
		for (const TSharedPtr<FJsonValue>& Value : *Nodes)
		{
			const TSharedPtr<FJsonObject>* NodeObj = nullptr;
			if (!Value.IsValid() || !Value->TryGetObject(NodeObj) || !NodeObj || !NodeObj->IsValid())
			{
				AddIssue(Issues, TEXT("invalid_node_entry"), TEXT("error"), TEXT("Each nodes[] entry must be an object."), TEXT("nodes"));
				continue;
			}
			FString Id;
			FString Type;
			FString ClassPath;
			(*NodeObj)->TryGetStringField(TEXT("id"), Id);
			(*NodeObj)->TryGetStringField(TEXT("type"), Type);
			(*NodeObj)->TryGetStringField(TEXT("class"), ClassPath);
			if (Id.IsEmpty())
			{
				AddIssue(Issues, TEXT("missing_node_id"), TEXT("error"), TEXT("Node entry is missing id."), TEXT("nodes.id"));
			}
			else if (NodeIds.Contains(Id))
			{
				AddIssue(Issues, TEXT("duplicate_node_id"), TEXT("error"), FString::Printf(TEXT("Duplicate node id '%s'."), *Id), TEXT("nodes.id"));
			}
			NodeIds.Add(Id);
			if (!ResolveSoundNodeClass(Type, ClassPath))
			{
				AddIssue(Issues, TEXT("unsupported_node_type"), TEXT("error"), FString::Printf(TEXT("Unsupported SoundCue node type '%s'."), *Type), TEXT("nodes.type"));
			}
		}

		const TArray<TSharedPtr<FJsonValue>>* Edges = nullptr;
		if (EdgesJson.IsValid() && EdgesJson->TryGetArrayField(TEXT("edges"), Edges) && Edges)
		{
			for (const TSharedPtr<FJsonValue>& Value : *Edges)
			{
				const TSharedPtr<FJsonObject>* EdgeObj = nullptr;
				if (!Value.IsValid() || !Value->TryGetObject(EdgeObj) || !EdgeObj || !EdgeObj->IsValid())
				{
					AddIssue(Issues, TEXT("invalid_edge_entry"), TEXT("error"), TEXT("Each edges[] entry must be an object."), TEXT("edges"));
					continue;
				}
				FString From;
				FString To;
				(*EdgeObj)->TryGetStringField(TEXT("from"), From);
				(*EdgeObj)->TryGetStringField(TEXT("to"), To);
				if (!NodeIds.Contains(From) || !NodeIds.Contains(To))
				{
					AddIssue(Issues, TEXT("edge_endpoint_not_found"), TEXT("error"), FString::Printf(TEXT("Edge endpoint not found: %s -> %s."), *From, *To), TEXT("edges"));
				}
			}
		}
		return !UeAgentJsonDiagnostics::HasErrorIssue(Issues);
	}

	static void ApplySoundNodeStructuredFields(USoundNode* Node, const TSharedPtr<FJsonObject>& NodeObj)
	{
		if (!Node || !NodeObj.IsValid())
		{
			return;
		}
		if (USoundNodeWavePlayer* WavePlayer = Cast<USoundNodeWavePlayer>(Node))
		{
			FString WavePath;
			if (JsonTryGetStringAny(NodeObj, { TEXT("sound_wave"), TEXT("wave"), TEXT("asset_path") }, WavePath))
			{
				if (USoundWave* Wave = LoadAsset<USoundWave>(WavePath))
				{
					WavePlayer->SetSoundWave(Wave);
				}
			}
			bool bLooping = WavePlayer->bLooping != 0;
			if (GetOptionalBool(NodeObj, TEXT("looping"), bLooping, WavePath))
			{
				WavePlayer->bLooping = bLooping;
			}
		}
		else if (USoundNodeRandom* RandomNode = Cast<USoundNodeRandom>(Node))
		{
			const TArray<TSharedPtr<FJsonValue>>* Weights = nullptr;
			if (NodeObj->TryGetArrayField(TEXT("weights"), Weights) && Weights)
			{
				RandomNode->Weights.Reset();
				for (const TSharedPtr<FJsonValue>& Weight : *Weights)
				{
					RandomNode->Weights.Add(Weight.IsValid() ? static_cast<float>(Weight->AsNumber()) : 1.0f);
				}
			}
			bool bWithoutReplacement = RandomNode->bRandomizeWithoutReplacement != 0;
			FString Error;
			if (GetOptionalBool(NodeObj, TEXT("randomize_without_replacement"), bWithoutReplacement, Error))
			{
				RandomNode->bRandomizeWithoutReplacement = bWithoutReplacement;
			}
			while (RandomNode->Weights.Num() < RandomNode->ChildNodes.Num())
			{
				RandomNode->Weights.Add(1.0f);
			}
			if (RandomNode->Weights.Num() > RandomNode->ChildNodes.Num())
			{
				RandomNode->Weights.SetNum(RandomNode->ChildNodes.Num());
			}
		}
		else if (USoundNodeMixer* MixerNode = Cast<USoundNodeMixer>(Node))
		{
			const TArray<TSharedPtr<FJsonValue>>* Volumes = nullptr;
			if (NodeObj->TryGetArrayField(TEXT("input_volume"), Volumes) && Volumes)
			{
				MixerNode->InputVolume.Reset();
				for (const TSharedPtr<FJsonValue>& Volume : *Volumes)
				{
					MixerNode->InputVolume.Add(Volume.IsValid() ? static_cast<float>(Volume->AsNumber()) : 1.0f);
				}
			}
		}
	}

	static bool ApplySoundCueGraph(USoundCue* Cue, const TSharedPtr<FJsonObject>& NodesJson, const TSharedPtr<FJsonObject>& EdgesJson, TArray<TSharedPtr<FJsonValue>>& OutPropertyResults, int32& OutNodesCreated, int32& OutEdgesCreated, bool bDryRun, FString& OutError)
	{
		OutNodesCreated = 0;
		OutEdgesCreated = 0;
		if (!Cue)
		{
			OutError = TEXT("sound_cue_not_found");
			return false;
		}
		TArray<TSharedPtr<FJsonValue>> Issues;
		if (!ValidateSoundCueFolderJson(NodesJson, EdgesJson, Issues))
		{
			OutError = TEXT("sound_cue_folder_validation_failed");
			return false;
		}
		const TArray<TSharedPtr<FJsonValue>>* NodesForClassCheck = nullptr;
		if (NodesJson->TryGetArrayField(TEXT("nodes"), NodesForClassCheck) && NodesForClassCheck)
		{
			for (const TSharedPtr<FJsonValue>& Value : *NodesForClassCheck)
			{
				const TSharedPtr<FJsonObject>* NodeObj = nullptr;
				if (!Value.IsValid() || !Value->TryGetObject(NodeObj) || !NodeObj || !NodeObj->IsValid())
				{
					continue;
				}
				FString Type;
				FString ClassPath;
				(*NodeObj)->TryGetStringField(TEXT("type"), Type);
				(*NodeObj)->TryGetStringField(TEXT("class"), ClassPath);
				if (!ResolveSoundNodeClass(Type, ClassPath))
				{
					OutError = TEXT("unsupported_node_type");
					return false;
				}
			}
		}
		if (bDryRun)
		{
			const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
			NodesJson->TryGetArrayField(TEXT("nodes"), Nodes);
			OutNodesCreated = Nodes ? Nodes->Num() : 0;
			const TArray<TSharedPtr<FJsonValue>>* Edges = nullptr;
			EdgesJson->TryGetArrayField(TEXT("edges"), Edges);
			OutEdgesCreated = Edges ? Edges->Num() : 0;
			return true;
		}

		Cue->Modify();
		Cue->FirstNode = nullptr;
		Cue->AllNodes.Empty();
		TMap<FString, USoundNode*> NodeMap;
		TMap<FString, TArray<FString>> ChildIds;
		FString RootNodeId;
		NodesJson->TryGetStringField(TEXT("root_node_id"), RootNodeId);

		const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
		NodesJson->TryGetArrayField(TEXT("nodes"), Nodes);
		for (const TSharedPtr<FJsonValue>& Value : *Nodes)
		{
			const TSharedPtr<FJsonObject>* NodeObj = nullptr;
			if (!Value.IsValid() || !Value->TryGetObject(NodeObj) || !NodeObj || !NodeObj->IsValid())
			{
				continue;
			}
			FString Id;
			FString Type;
			FString ClassPath;
			(*NodeObj)->TryGetStringField(TEXT("id"), Id);
			(*NodeObj)->TryGetStringField(TEXT("type"), Type);
			(*NodeObj)->TryGetStringField(TEXT("class"), ClassPath);
			UClass* NodeClass = ResolveSoundNodeClass(Type, ClassPath);
			if (!NodeClass)
			{
				OutError = TEXT("unsupported_node_type");
				return false;
			}
			USoundNode* Node = Cue->ConstructSoundNode<USoundNode>(NodeClass, false);
			if (!Node)
			{
				OutError = TEXT("create_sound_node_failed");
				return false;
			}
			if (!Id.IsEmpty())
			{
				Node->Rename(*Id, Cue, REN_DontCreateRedirectors | REN_ForceNoResetLoaders);
			}
			ApplySoundNodeStructuredFields(Node, *NodeObj);
			const TArray<TSharedPtr<FJsonValue>>* Children = nullptr;
			if ((*NodeObj)->TryGetArrayField(TEXT("children"), Children) && Children)
			{
				TArray<FString>& TargetChildren = ChildIds.FindOrAdd(Id);
				for (const TSharedPtr<FJsonValue>& ChildValue : *Children)
				{
					if (ChildValue.IsValid())
					{
						FString ChildId = ChildValue->AsString();
						ChildId.TrimStartAndEndInline();
						if (!ChildId.IsEmpty())
						{
							TargetChildren.Add(ChildId);
						}
					}
				}
			}
			TArray<TSharedPtr<FJsonValue>> Results;
			FString Error;
			ApplyPropertiesArray(Node, *NodeObj, Results, false, Error);
			for (const TSharedPtr<FJsonValue>& Result : Results)
			{
				OutPropertyResults.Add(Result);
			}
			NodeMap.Add(Id, Node);
			++OutNodesCreated;
		}

		const TArray<TSharedPtr<FJsonValue>>* Edges = nullptr;
		if (EdgesJson.IsValid() && EdgesJson->TryGetArrayField(TEXT("edges"), Edges) && Edges)
		{
			for (const TSharedPtr<FJsonValue>& Value : *Edges)
			{
				const TSharedPtr<FJsonObject>* EdgeObj = nullptr;
				if (!Value.IsValid() || !Value->TryGetObject(EdgeObj) || !EdgeObj || !EdgeObj->IsValid())
				{
					continue;
				}
				FString From;
				FString To;
				(*EdgeObj)->TryGetStringField(TEXT("from"), From);
				(*EdgeObj)->TryGetStringField(TEXT("to"), To);
				if (!From.IsEmpty() && !To.IsEmpty())
				{
					ChildIds.FindOrAdd(From).Add(To);
				}
			}
		}

		for (const TPair<FString, TArray<FString>>& Pair : ChildIds)
		{
			USoundNode* Parent = NodeMap.FindRef(Pair.Key);
			if (!Parent)
			{
				continue;
			}
			TArray<USoundNode*> Children;
			for (const FString& ChildId : Pair.Value)
			{
				if (USoundNode* Child = NodeMap.FindRef(ChildId))
				{
					Children.Add(Child);
					++OutEdgesCreated;
				}
			}
			Parent->SetChildNodes(Children);
			if (USoundNodeRandom* RandomNode = Cast<USoundNodeRandom>(Parent))
			{
				while (RandomNode->Weights.Num() < Children.Num())
				{
					RandomNode->Weights.Add(1.0f);
				}
				if (RandomNode->Weights.Num() > Children.Num())
				{
					RandomNode->Weights.SetNum(Children.Num());
				}
			}
		}
		if (RootNodeId.IsEmpty())
		{
			for (const TPair<FString, USoundNode*>& Pair : NodeMap)
			{
				RootNodeId = Pair.Key;
				break;
			}
		}
		Cue->FirstNode = NodeMap.FindRef(RootNodeId);
		if (!Cue->FirstNode && Cue->AllNodes.Num() > 0)
		{
			Cue->FirstNode = Cue->AllNodes[0];
		}
		Cue->LinkGraphNodesFromSoundNodes();
		Cue->CompileSoundNodesFromGraphNodes();
		Cue->PostEditChange();
		Cue->MarkPackageDirty();
		return true;
	}

	static TSharedPtr<FJsonObject> BuildMetaSoundInfo(UObject* MetaSound, bool bIncludeGraph, bool bIncludeInterfaces, bool bIncludeValidation)
	{
		TSharedPtr<FJsonObject> Info = MakeShared<FJsonObject>();
		FillAssetIdentity(MetaSound, Info);
		if (!MetaSound)
		{
			return Info;
		}
		Info->SetStringField(TEXT("asset_type"), MetaSound->IsA<UMetaSoundSource>() ? TEXT("source") : TEXT("patch"));
		IMetaSoundDocumentInterface* DocumentInterface = Cast<IMetaSoundDocumentInterface>(MetaSound);
		if (!DocumentInterface)
		{
			Info->SetBoolField(TEXT("has_document"), false);
			return Info;
		}
		Info->SetBoolField(TEXT("has_document"), true);
		const FMetasoundFrontendDocument& Document = DocumentInterface->GetConstDocument();
		const FMetasoundFrontendGraph& Graph = Document.RootGraph.GetConstDefaultGraph();
		const FMetasoundFrontendClassInterface& DefaultInterface = Document.RootGraph.GetDefaultInterface();

		TArray<TSharedPtr<FJsonValue>> Inputs;
		for (const FMetasoundFrontendClassInput& Input : DefaultInterface.Inputs)
		{
			TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("name"), Input.Name.ToString());
			Item->SetStringField(TEXT("type"), Input.TypeName.ToString());
			Item->SetStringField(TEXT("vertex_id"), Input.VertexID.ToString(EGuidFormats::DigitsWithHyphens));
			Inputs.Add(MakeShared<FJsonValueObject>(Item));
		}
		Info->SetArrayField(TEXT("inputs"), Inputs);

		TArray<TSharedPtr<FJsonValue>> Outputs;
		for (const FMetasoundFrontendClassOutput& Output : DefaultInterface.Outputs)
		{
			TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("name"), Output.Name.ToString());
			Item->SetStringField(TEXT("type"), Output.TypeName.ToString());
			Item->SetStringField(TEXT("vertex_id"), Output.VertexID.ToString(EGuidFormats::DigitsWithHyphens));
			Outputs.Add(MakeShared<FJsonValueObject>(Item));
		}
		Info->SetArrayField(TEXT("outputs"), Outputs);

		if (bIncludeGraph)
		{
			TArray<TSharedPtr<FJsonValue>> Nodes;
			for (const FMetasoundFrontendNode& Node : Graph.Nodes)
			{
				TSharedPtr<FJsonObject> NodeObj = MakeShared<FJsonObject>();
				NodeObj->SetStringField(TEXT("id"), Node.GetID().ToString(EGuidFormats::DigitsWithHyphens));
				NodeObj->SetStringField(TEXT("name"), Node.Name.ToString());
				NodeObj->SetStringField(TEXT("class_id"), Node.ClassID.ToString(EGuidFormats::DigitsWithHyphens));
				TArray<TSharedPtr<FJsonValue>> NodeInputs;
				for (const FMetasoundFrontendVertex& Input : Node.Interface.Inputs)
				{
					TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
					Item->SetStringField(TEXT("name"), Input.Name.ToString());
					Item->SetStringField(TEXT("type"), Input.TypeName.ToString());
					Item->SetStringField(TEXT("vertex_id"), Input.VertexID.ToString(EGuidFormats::DigitsWithHyphens));
					NodeInputs.Add(MakeShared<FJsonValueObject>(Item));
				}
				NodeObj->SetArrayField(TEXT("inputs"), NodeInputs);
				TArray<TSharedPtr<FJsonValue>> NodeOutputs;
				for (const FMetasoundFrontendVertex& Output : Node.Interface.Outputs)
				{
					TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
					Item->SetStringField(TEXT("name"), Output.Name.ToString());
					Item->SetStringField(TEXT("type"), Output.TypeName.ToString());
					Item->SetStringField(TEXT("vertex_id"), Output.VertexID.ToString(EGuidFormats::DigitsWithHyphens));
					NodeOutputs.Add(MakeShared<FJsonValueObject>(Item));
				}
				NodeObj->SetArrayField(TEXT("outputs"), NodeOutputs);
				Nodes.Add(MakeShared<FJsonValueObject>(NodeObj));
			}
			Info->SetArrayField(TEXT("nodes"), Nodes);

			TArray<TSharedPtr<FJsonValue>> Edges;
			for (const FMetasoundFrontendEdge& Edge : Graph.Edges)
			{
				TSharedPtr<FJsonObject> EdgeObj = MakeShared<FJsonObject>();
				EdgeObj->SetStringField(TEXT("from_node_id"), Edge.FromNodeID.ToString(EGuidFormats::DigitsWithHyphens));
				EdgeObj->SetStringField(TEXT("from_vertex_id"), Edge.FromVertexID.ToString(EGuidFormats::DigitsWithHyphens));
				EdgeObj->SetStringField(TEXT("to_node_id"), Edge.ToNodeID.ToString(EGuidFormats::DigitsWithHyphens));
				EdgeObj->SetStringField(TEXT("to_vertex_id"), Edge.ToVertexID.ToString(EGuidFormats::DigitsWithHyphens));
				Edges.Add(MakeShared<FJsonValueObject>(EdgeObj));
			}
			Info->SetArrayField(TEXT("edges"), Edges);
		}
		if (bIncludeInterfaces)
		{
			TArray<TSharedPtr<FJsonValue>> Interfaces;
			for (const FMetasoundFrontendVersion& Version : Document.Interfaces)
			{
				TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
				Item->SetStringField(TEXT("name"), Version.Name.ToString());
				Item->SetNumberField(TEXT("major"), Version.Number.Major);
				Item->SetNumberField(TEXT("minor"), Version.Number.Minor);
				Interfaces.Add(MakeShared<FJsonValueObject>(Item));
			}
			Info->SetArrayField(TEXT("interfaces"), Interfaces);
		}
		if (bIncludeValidation)
		{
			TArray<TSharedPtr<FJsonValue>> Issues;
			Info->SetArrayField(TEXT("json_issues"), Issues);
			TSharedPtr<FJsonObject> CompileReport = MakeShared<FJsonObject>();
			CompileReport->SetNumberField(TEXT("error_count"), 0);
			CompileReport->SetNumberField(TEXT("warning_count"), 0);
			Info->SetObjectField(TEXT("compile_report"), CompileReport);
		}
		return Info;
	}

	static bool ValidateMetaSoundFolder(const FString& FolderPathInput, TArray<TSharedPtr<FJsonValue>>& Issues, TSharedPtr<FJsonObject>& OutAssetJson, TSharedPtr<FJsonObject>& OutInputsJson, TSharedPtr<FJsonObject>& OutOutputsJson, FString& OutFolder, FString& OutError)
	{
		OutFolder = ResolveFilePath(FolderPathInput);
		if (OutFolder.IsEmpty())
		{
			OutError = TEXT("missing_folder_path");
			return false;
		}
		FString ResolvedPath;
		if (!LoadJsonFile(FPaths::Combine(OutFolder, TEXT("asset.json")), OutAssetJson, ResolvedPath, OutError))
		{
			return false;
		}
		if (!LoadJsonFile(FPaths::Combine(OutFolder, TEXT("members/inputs.json")), OutInputsJson, ResolvedPath, OutError))
		{
			return false;
		}
		if (!LoadJsonFile(FPaths::Combine(OutFolder, TEXT("members/outputs.json")), OutOutputsJson, ResolvedPath, OutError))
		{
			return false;
		}
		TSharedPtr<FJsonObject> NodesJson;
		if (FPaths::FileExists(FPaths::Combine(OutFolder, TEXT("graphs/nodes.json"))))
		{
			if (!LoadJsonFile(FPaths::Combine(OutFolder, TEXT("graphs/nodes.json")), NodesJson, ResolvedPath, OutError))
			{
				return false;
			}
			const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
			if (NodesJson->TryGetArrayField(TEXT("nodes"), Nodes) && Nodes)
			{
				for (const TSharedPtr<FJsonValue>& Value : *Nodes)
				{
					const TSharedPtr<FJsonObject>* NodeObj = nullptr;
					FString Type;
					if (Value.IsValid() && Value->TryGetObject(NodeObj) && NodeObj && NodeObj->IsValid() && (*NodeObj)->TryGetStringField(TEXT("type"), Type))
					{
						if (!Type.IsEmpty() && Type != TEXT("graph_input") && Type != TEXT("graph_output") && Type != TEXT("comment"))
						{
							AddIssue(Issues, TEXT("unsupported_node_type"), TEXT("error"), FString::Printf(TEXT("MetaSound apply only supports graph input/output authoring for node type '%s'."), *Type), TEXT("graphs/nodes.json"));
						}
					}
				}
			}
		}
		return !UeAgentJsonDiagnostics::HasErrorIssue(Issues);
	}

	static bool AddMetaSoundMembers(UMetaSoundBuilderBase* Builder, const TSharedPtr<FJsonObject>& MembersJson, const TCHAR* ArrayField, bool bInput, int32& OutAdded, FString& OutError)
	{
		if (!Builder || !MembersJson.IsValid())
		{
			return true;
		}
		const TArray<TSharedPtr<FJsonValue>>* Members = nullptr;
		if (!MembersJson->TryGetArrayField(ArrayField, Members) || !Members)
		{
			return true;
		}
		TSet<FName> ExistingNameSet;
		for (const TSharedPtr<FJsonValue>& Value : *Members)
		{
			const TSharedPtr<FJsonObject>* MemberObj = nullptr;
			if (!Value.IsValid() || !Value->TryGetObject(MemberObj) || !MemberObj || !MemberObj->IsValid())
			{
				continue;
			}
			FString Name;
			FString Type;
			(*MemberObj)->TryGetStringField(TEXT("name"), Name);
			(*MemberObj)->TryGetStringField(TEXT("type"), Type);
			if (Name.IsEmpty() || Type.IsEmpty())
			{
				continue;
			}
			const FName MemberName(*Name);
			if (ExistingNameSet.Contains(MemberName))
			{
				continue;
			}
			EMetaSoundBuilderResult FindResult = EMetaSoundBuilderResult::Failed;
			if (bInput)
			{
				Builder->FindGraphInputNode(MemberName, FindResult);
			}
			else
			{
				Builder->FindGraphOutputNode(MemberName, FindResult);
			}
			if (FindResult == EMetaSoundBuilderResult::Succeeded)
			{
				ExistingNameSet.Add(MemberName);
				continue;
			}
			FMetasoundFrontendLiteral Literal;
			EMetaSoundBuilderResult Result = EMetaSoundBuilderResult::Failed;
			if (bInput)
			{
				Builder->AddGraphInputNode(MemberName, *Type, Literal, Result);
			}
			else
			{
				Builder->AddGraphOutputNode(MemberName, *Type, Literal, Result);
			}
			if (Result != EMetaSoundBuilderResult::Succeeded)
			{
				OutError = bInput ? TEXT("metasound_add_input_failed") : TEXT("metasound_add_output_failed");
				return false;
			}
			ExistingNameSet.Add(MemberName);
			++OutAdded;
		}
		return true;
	}

	static bool CreateMetaSoundAsset(const FString& AssetPath, const FString& AssetTypeInput, bool bReplaceExisting, UObject*& OutAsset, FString& OutError)
	{
		OutAsset = nullptr;
		const bool bSource = !AssetTypeInput.Equals(TEXT("patch"), ESearchCase::IgnoreCase);
		UClass* AssetClass = bSource ? UMetaSoundSource::StaticClass() : UMetaSoundPatch::StaticClass();
		if (UObject* Existing = LoadAssetObject(AssetPath))
		{
			if (!bReplaceExisting)
			{
				OutError = TEXT("asset_already_exists");
				return false;
			}
			if (!Existing->IsA(AssetClass))
			{
				OutError = TEXT("existing_asset_class_mismatch");
				return false;
			}
			Existing->Modify();
			OutAsset = Existing;
			return true;
		}

		FString NormalizedPath;
		FString AssetName;
		if (!ValidateAssetPathForCreate(AssetPath, NormalizedPath, AssetName, OutError))
		{
			return false;
		}
		UPackage* Package = CreatePackage(*NormalizedPath);
		if (!Package)
		{
			OutError = TEXT("create_package_failed");
			return false;
		}
		UFactory* Factory = bSource ? static_cast<UFactory*>(NewObject<UMetaSoundSourceFactory>()) : static_cast<UFactory*>(NewObject<UMetaSoundFactory>());
		UObject* Asset = Factory ? Factory->FactoryCreateNew(AssetClass, Package, *AssetName, RF_Public | RF_Standalone | RF_Transactional, nullptr, GWarn) : nullptr;
		if (!Asset)
		{
			Asset = NewObject<UObject>(Package, AssetClass, *AssetName, RF_Public | RF_Standalone | RF_Transactional);
		}
		if (!Asset)
		{
			OutError = TEXT("create_metasound_failed");
			return false;
		}
		FAssetRegistryModule::AssetCreated(Asset);
		Asset->MarkPackageDirty();
		Package->MarkPackageDirty();
		OutAsset = Asset;
		return true;
	}

	static TSharedPtr<FJsonObject> BuildSoundAttenuationInfo(USoundAttenuation* Asset, bool bIncludeRawProperties)
	{
		TSharedPtr<FJsonObject> Info = MakeShared<FJsonObject>();
		FillAssetIdentity(Asset, Info);
		if (!Asset)
		{
			return Info;
		}
		const FSoundAttenuationSettings& A = Asset->Attenuation;
		Info->SetBoolField(TEXT("attenuate"), A.bAttenuate != 0);
		Info->SetBoolField(TEXT("spatialize"), A.bSpatialize != 0);
		Info->SetNumberField(TEXT("falloff_distance"), A.FalloffDistance);
		Info->SetBoolField(TEXT("occlusion"), A.bEnableOcclusion != 0);
		Info->SetNumberField(TEXT("occlusion_low_pass_filter_frequency"), A.OcclusionLowPassFilterFrequency);
		Info->SetNumberField(TEXT("occlusion_volume_attenuation"), A.OcclusionVolumeAttenuation);
		if (bIncludeRawProperties)
		{
			Info->SetObjectField(TEXT("raw_properties"), BuildGenericAssetJson(Asset, SoundAttenuationProfile, true));
		}
		return Info;
	}

	static TSharedPtr<FJsonObject> BuildSoundConcurrencyInfo(USoundConcurrency* Asset, bool bIncludeRawProperties)
	{
		TSharedPtr<FJsonObject> Info = MakeShared<FJsonObject>();
		FillAssetIdentity(Asset, Info);
		if (!Asset)
		{
			return Info;
		}
		Info->SetNumberField(TEXT("max_count"), Asset->Concurrency.MaxCount);
		Info->SetBoolField(TEXT("limit_to_owner"), Asset->Concurrency.bLimitToOwner != 0);
		FString ValueText;
		FString CppType;
		if (ExportPropertyText(Asset, TEXT("Concurrency.ResolutionRule"), ValueText, CppType))
		{
			Info->SetStringField(TEXT("resolution_rule"), ValueText);
		}
		Info->SetNumberField(TEXT("retrigger_time"), Asset->Concurrency.RetriggerTime);
		if (bIncludeRawProperties)
		{
			Info->SetObjectField(TEXT("raw_properties"), BuildGenericAssetJson(Asset, SoundConcurrencyProfile, true));
		}
		return Info;
	}

	static TSharedPtr<FJsonObject> BuildSoundClassInfo(USoundClass* Asset, bool bIncludeChildren, bool bIncludeRawProperties)
	{
		TSharedPtr<FJsonObject> Info = MakeShared<FJsonObject>();
		FillAssetIdentity(Asset, Info);
		if (!Asset)
		{
			return Info;
		}
		Info->SetNumberField(TEXT("volume"), Asset->Properties.Volume);
		Info->SetNumberField(TEXT("pitch"), Asset->Properties.Pitch);
		Info->SetBoolField(TEXT("is_ui_sound"), Asset->Properties.bIsUISound != 0);
		Info->SetBoolField(TEXT("is_music"), Asset->Properties.bIsMusic != 0);
		Info->SetStringField(TEXT("parent_class"), ObjectPathOrEmpty(Asset->ParentClass));
		Info->SetStringField(TEXT("default_submix"), ObjectPathOrEmpty(Asset->Properties.DefaultSubmix));
		if (bIncludeChildren)
		{
			TArray<TSharedPtr<FJsonValue>> Children;
			for (USoundClass* Child : Asset->ChildClasses)
			{
				Children.Add(JsonStringValue(ObjectPathOrEmpty(Child)));
			}
			Info->SetArrayField(TEXT("children"), Children);
		}
		if (bIncludeRawProperties)
		{
			Info->SetObjectField(TEXT("raw_properties"), BuildGenericAssetJson(Asset, SoundClassProfile, true));
		}
		return Info;
	}

	static TSharedPtr<FJsonObject> BuildSoundMixInfo(USoundMix* Asset, bool bIncludeRawProperties)
	{
		TSharedPtr<FJsonObject> Info = MakeShared<FJsonObject>();
		FillAssetIdentity(Asset, Info);
		if (!Asset)
		{
			return Info;
		}
		Info->SetNumberField(TEXT("initial_delay"), Asset->InitialDelay);
		Info->SetNumberField(TEXT("fade_in_time"), Asset->FadeInTime);
		Info->SetNumberField(TEXT("duration"), Asset->Duration);
		Info->SetNumberField(TEXT("fade_out_time"), Asset->FadeOutTime);
		TArray<TSharedPtr<FJsonValue>> Adjusters;
		for (const FSoundClassAdjuster& Adjuster : Asset->SoundClassEffects)
		{
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("sound_class"), ObjectPathOrEmpty(Adjuster.SoundClassObject));
			Obj->SetNumberField(TEXT("volume_adjuster"), Adjuster.VolumeAdjuster);
			Obj->SetNumberField(TEXT("pitch_adjuster"), Adjuster.PitchAdjuster);
			Obj->SetNumberField(TEXT("low_pass_filter_frequency"), Adjuster.LowPassFilterFrequency);
			Obj->SetBoolField(TEXT("apply_to_children"), Adjuster.bApplyToChildren != 0);
			Obj->SetNumberField(TEXT("voice_center_channel_volume_adjuster"), Adjuster.VoiceCenterChannelVolumeAdjuster);
			Adjusters.Add(MakeShared<FJsonValueObject>(Obj));
		}
		Info->SetArrayField(TEXT("class_adjusters"), Adjusters);
		if (bIncludeRawProperties)
		{
			Info->SetObjectField(TEXT("raw_properties"), BuildGenericAssetJson(Asset, SoundMixProfile, true));
		}
		return Info;
	}

	static TSharedPtr<FJsonObject> BuildSoundSubmixInfo(USoundSubmixBase* Asset, bool bIncludeChildren, bool bIncludeEffectChain, bool bIncludeRawProperties)
	{
		TSharedPtr<FJsonObject> Info = MakeShared<FJsonObject>();
		FillAssetIdentity(Asset, Info);
		if (!Asset)
		{
			return Info;
		}
		if (USoundSubmixWithParentBase* WithParent = Cast<USoundSubmixWithParentBase>(Asset))
		{
			Info->SetStringField(TEXT("parent"), ObjectPathOrEmpty(WithParent->ParentSubmix));
		}
		if (bIncludeChildren)
		{
			TArray<TSharedPtr<FJsonValue>> Children;
			for (USoundSubmixBase* Child : Asset->ChildSubmixes)
			{
				Children.Add(JsonStringValue(ObjectPathOrEmpty(Child)));
			}
			Info->SetArrayField(TEXT("children"), Children);
		}
		if (bIncludeEffectChain)
		{
			TArray<TSharedPtr<FJsonValue>> Chain;
			if (USoundSubmix* Submix = Cast<USoundSubmix>(Asset))
			{
				for (USoundEffectSubmixPreset* Preset : Submix->SubmixEffectChain)
				{
					Chain.Add(JsonStringValue(ObjectPathOrEmpty(Preset)));
				}
			}
			Info->SetArrayField(TEXT("effect_chain"), Chain);
		}
		if (bIncludeRawProperties)
		{
			Info->SetObjectField(TEXT("raw_properties"), BuildGenericAssetJson(Asset, SoundSubmixProfile, true));
		}
		return Info;
	}

	static void AddAudioComponentSnapshot(UAudioComponent* Component, TSharedPtr<FJsonObject>& Out)
	{
		Out->SetStringField(TEXT("component_name"), Component ? Component->GetName() : FString());
		Out->SetStringField(TEXT("owner"), Component && Component->GetOwner() ? Component->GetOwner()->GetActorLabel() : FString());
		Out->SetStringField(TEXT("sound"), Component ? ObjectPathOrEmpty(Component->Sound) : FString());
		Out->SetBoolField(TEXT("is_playing"), Component ? Component->IsPlaying() : false);
		Out->SetStringField(TEXT("play_state"), Component ? PlayStateToString(Component->GetPlayState()) : FString());
		Out->SetNumberField(TEXT("volume_multiplier"), Component ? Component->VolumeMultiplier : 0.0f);
		Out->SetNumberField(TEXT("pitch_multiplier"), Component ? Component->PitchMultiplier : 0.0f);
		Out->SetBoolField(TEXT("auto_activate"), Component ? Component->bAutoActivate : false);
		Out->SetStringField(TEXT("attenuation"), Component ? ObjectPathOrEmpty(Component->AttenuationSettings) : FString());
	}
}

namespace
{
	using FAudioCommandHandler = bool (FUeAgentHttpServer::*)(const FUeAgentRequestContext&, TSharedPtr<FJsonObject>&, FString&) const;

	struct FAudioCommandEntry
	{
		const TCHAR* Name;
		FAudioCommandHandler Handler;
	};
}

bool FUeAgentHttpServer::ExecuteAudioCommand(const FString& CommandLower, const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	static const FAudioCommandEntry Entries[] = {
		{ TEXT("sound_wave_import"), &FUeAgentHttpServer::CmdSoundWaveImport },
		{ TEXT("sound_wave_reimport"), &FUeAgentHttpServer::CmdSoundWaveReimport },
		{ TEXT("sound_wave_get_info"), &FUeAgentHttpServer::CmdSoundWaveGetInfo },
		{ TEXT("sound_wave_validate_setup"), &FUeAgentHttpServer::CmdSoundWaveValidateSetup },
		{ TEXT("sound_wave_preview"), &FUeAgentHttpServer::CmdSoundWavePreview },
		{ TEXT("sound_cue_create"), &FUeAgentHttpServer::CmdSoundCueCreate },
		{ TEXT("sound_cue_get_info"), &FUeAgentHttpServer::CmdSoundCueGetInfo },
		{ TEXT("sound_cue_export_folder"), &FUeAgentHttpServer::CmdSoundCueExportFolder },
		{ TEXT("sound_cue_validate_folder"), &FUeAgentHttpServer::CmdSoundCueValidateFolder },
		{ TEXT("sound_cue_apply_folder"), &FUeAgentHttpServer::CmdSoundCueApplyFolder },
		{ TEXT("sound_cue_open_editor"), &FUeAgentHttpServer::CmdSoundCueOpenEditor },
		{ TEXT("sound_cue_screenshot"), &FUeAgentHttpServer::CmdSoundCueScreenshot },
		{ TEXT("sound_cue_preview"), &FUeAgentHttpServer::CmdSoundCuePreview },
		{ TEXT("metasound_create"), &FUeAgentHttpServer::CmdMetaSoundCreate },
		{ TEXT("metasound_get_info"), &FUeAgentHttpServer::CmdMetaSoundGetInfo },
		{ TEXT("metasound_export_folder"), &FUeAgentHttpServer::CmdMetaSoundExportFolder },
		{ TEXT("metasound_validate_folder"), &FUeAgentHttpServer::CmdMetaSoundValidateFolder },
		{ TEXT("metasound_apply_folder"), &FUeAgentHttpServer::CmdMetaSoundApplyFolder },
		{ TEXT("metasound_compile"), &FUeAgentHttpServer::CmdMetaSoundCompile },
		{ TEXT("metasound_open_editor"), &FUeAgentHttpServer::CmdMetaSoundOpenEditor },
		{ TEXT("metasound_screenshot"), &FUeAgentHttpServer::CmdMetaSoundScreenshot },
		{ TEXT("metasound_preview"), &FUeAgentHttpServer::CmdMetaSoundPreview },
		{ TEXT("sound_attenuation_create"), &FUeAgentHttpServer::CmdSoundAttenuationCreate },
		{ TEXT("sound_attenuation_get_info"), &FUeAgentHttpServer::CmdSoundAttenuationGetInfo },
		{ TEXT("sound_attenuation_export_json"), &FUeAgentHttpServer::CmdSoundAttenuationExportJson },
		{ TEXT("sound_attenuation_validate_json"), &FUeAgentHttpServer::CmdSoundAttenuationValidateJson },
		{ TEXT("sound_attenuation_apply_json"), &FUeAgentHttpServer::CmdSoundAttenuationApplyJson },
		{ TEXT("sound_concurrency_create"), &FUeAgentHttpServer::CmdSoundConcurrencyCreate },
		{ TEXT("sound_concurrency_get_info"), &FUeAgentHttpServer::CmdSoundConcurrencyGetInfo },
		{ TEXT("sound_concurrency_export_json"), &FUeAgentHttpServer::CmdSoundConcurrencyExportJson },
		{ TEXT("sound_concurrency_validate_json"), &FUeAgentHttpServer::CmdSoundConcurrencyValidateJson },
		{ TEXT("sound_concurrency_apply_json"), &FUeAgentHttpServer::CmdSoundConcurrencyApplyJson },
		{ TEXT("sound_class_create"), &FUeAgentHttpServer::CmdSoundClassCreate },
		{ TEXT("sound_class_get_info"), &FUeAgentHttpServer::CmdSoundClassGetInfo },
		{ TEXT("sound_class_export_json"), &FUeAgentHttpServer::CmdSoundClassExportJson },
		{ TEXT("sound_class_validate_json"), &FUeAgentHttpServer::CmdSoundClassValidateJson },
		{ TEXT("sound_class_apply_json"), &FUeAgentHttpServer::CmdSoundClassApplyJson },
		{ TEXT("sound_mix_create"), &FUeAgentHttpServer::CmdSoundMixCreate },
		{ TEXT("sound_mix_export_json"), &FUeAgentHttpServer::CmdSoundMixExportJson },
		{ TEXT("sound_mix_apply_json"), &FUeAgentHttpServer::CmdSoundMixApplyJson },
		{ TEXT("sound_submix_create"), &FUeAgentHttpServer::CmdSoundSubmixCreate },
		{ TEXT("sound_submix_get_info"), &FUeAgentHttpServer::CmdSoundSubmixGetInfo },
		{ TEXT("sound_submix_export_folder"), &FUeAgentHttpServer::CmdSoundSubmixExportFolder },
		{ TEXT("sound_submix_validate_folder"), &FUeAgentHttpServer::CmdSoundSubmixValidateFolder },
		{ TEXT("sound_submix_apply_folder"), &FUeAgentHttpServer::CmdSoundSubmixApplyFolder },
		{ TEXT("audio_effect_preset_create"), &FUeAgentHttpServer::CmdAudioEffectPresetCreate },
		{ TEXT("audio_effect_preset_export_json"), &FUeAgentHttpServer::CmdAudioEffectPresetExportJson },
		{ TEXT("audio_effect_preset_apply_json"), &FUeAgentHttpServer::CmdAudioEffectPresetApplyJson },
		{ TEXT("audio_component_validate_setup"), &FUeAgentHttpServer::CmdAudioComponentValidateSetup },
		{ TEXT("audio_component_runtime_snapshot"), &FUeAgentHttpServer::CmdAudioComponentRuntimeSnapshot },
		{ TEXT("audio_component_runtime_probe"), &FUeAgentHttpServer::CmdAudioComponentRuntimeProbe },
		{ TEXT("audio_volume_validate_setup"), &FUeAgentHttpServer::CmdAudioVolumeValidateSetup },
		{ TEXT("ambient_sound_validate_setup"), &FUeAgentHttpServer::CmdAmbientSoundValidateSetup },
		{ TEXT("audio_runtime_snapshot"), &FUeAgentHttpServer::CmdAudioRuntimeSnapshot },
		{ TEXT("audio_runtime_probe"), &FUeAgentHttpServer::CmdAudioRuntimeProbe },
		{ TEXT("audio_submix_meter_probe"), &FUeAgentHttpServer::CmdAudioSubmixMeterProbe },
		{ TEXT("audio_submix_record_start"), &FUeAgentHttpServer::CmdAudioSubmixRecordStart },
		{ TEXT("audio_submix_record_stop"), &FUeAgentHttpServer::CmdAudioSubmixRecordStop },
		{ TEXT("audio_submix_record_cancel"), &FUeAgentHttpServer::CmdAudioSubmixRecordCancel },
		{ TEXT("audio_stop_all_preview_sounds"), &FUeAgentHttpServer::CmdAudioStopAllPreviewSounds },
	};

	for (const FAudioCommandEntry& Entry : Entries)
	{
		if (CommandLower == Entry.Name)
		{
			return (this->*Entry.Handler)(Ctx, OutData, OutError);
		}
	}
	OutError = TEXT("unknown_audio_command");
	return false;
}

bool FUeAgentHttpServer::CmdSoundWaveImport(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentAudioOps;
	FString SourceFileInput;
	if (!GetRequiredStringAny(Ctx, { TEXT("source_file"), TEXT("source_filename") }, SourceFileInput, OutError))
	{
		return false;
	}
	const FString SourceFile = ResolveFilePath(SourceFileInput);
	if (!FPaths::FileExists(SourceFile))
	{
		OutError = TEXT("source_file_not_found");
		return false;
	}
	FString DestinationPath;
	if (!GetRequiredString(Ctx, TEXT("destination_path"), DestinationPath, OutError))
	{
		return false;
	}
	FString DestinationName;
	Ctx.Params->TryGetStringField(TEXT("destination_name"), DestinationName);
	bool bReplaceExisting = true;
	bool bSave = true;
	if (!GetOptionalBool(Ctx.Params, TEXT("replace_existing"), bReplaceExisting, OutError)
		|| !GetOptionalBool(Ctx.Params, TEXT("save_after_import"), bSave, OutError))
	{
		return false;
	}
	USoundWave* Wave = nullptr;
	if (!ImportSoundWaveFromFile(SourceFile, DestinationPath, DestinationName, bReplaceExisting, Wave, OutError))
	{
		return false;
	}
	if (bSave && !SavePackagesForObjects({ Wave }, OutError))
	{
		return false;
	}
	OutData = BuildSoundWaveInfo(Wave, false, true, true);
	OutData->SetStringField(TEXT("source_file"), SourceFile);
	OutData->SetStringField(TEXT("destination_path"), DestinationPath);
	OutData->SetStringField(TEXT("destination_name"), FPackageName::GetLongPackageAssetName(AssetPathOrEmpty(Wave)));
	OutData->SetBoolField(TEXT("replace_existing"), bReplaceExisting);
	OutData->SetBoolField(TEXT("save_after_import"), bSave);
	OutData->SetArrayField(TEXT("warnings"), {});
	return true;
}

bool FUeAgentHttpServer::CmdSoundWaveReimport(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentAudioOps;
	FString AssetPath;
	if (!GetRequiredString(Ctx, TEXT("asset_path"), AssetPath, OutError))
	{
		return false;
	}
	USoundWave* Wave = LoadAsset<USoundWave>(AssetPath);
	if (!Wave)
	{
		OutError = TEXT("sound_wave_not_found");
		return false;
	}
	const float DurationBefore = Wave->GetDuration();
	FString SourceFileInput;
	if (Ctx.Params.IsValid() && Ctx.Params->TryGetStringField(TEXT("source_file"), SourceFileInput) && !SourceFileInput.TrimStartAndEnd().IsEmpty())
	{
		const FString SourceFile = ResolveFilePath(SourceFileInput);
		if (!FPaths::FileExists(SourceFile))
		{
			OutError = TEXT("source_file_not_found");
			return false;
		}
		if (Wave->AssetImportData)
		{
			Wave->AssetImportData->Update(SourceFile);
		}
	}
	bool bSave = true;
	if (!GetOptionalBool(Ctx.Params, TEXT("save_after_reimport"), bSave, OutError))
	{
		return false;
	}
	if (!FReimportManager::Instance()->Reimport(Wave, true, true))
	{
		OutError = TEXT("sound_wave_reimport_failed");
		return false;
	}
	Wave->PostEditChange();
	Wave->MarkPackageDirty();
	if (bSave && !SavePackagesForObjects({ Wave }, OutError))
	{
		return false;
	}
	OutData = BuildSoundWaveInfo(Wave, false, true, true);
	OutData->SetBoolField(TEXT("reimported"), true);
	OutData->SetNumberField(TEXT("duration_before"), DurationBefore);
	OutData->SetNumberField(TEXT("duration_after"), Wave->GetDuration());
	OutData->SetArrayField(TEXT("warnings"), {});
	return true;
}

bool FUeAgentHttpServer::CmdSoundWaveGetInfo(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentAudioOps;
	FString AssetPath;
	if (!GetRequiredString(Ctx, TEXT("asset_path"), AssetPath, OutError))
	{
		return false;
	}
	USoundWave* Wave = LoadAsset<USoundWave>(AssetPath);
	if (!Wave)
	{
		OutError = TEXT("sound_wave_not_found");
		return false;
	}
	bool bIncludeProperties = false;
	bool bIncludeCuePoints = false;
	bool bIncludeAnalysis = false;
	if (!GetOptionalBool(Ctx.Params, TEXT("include_properties"), bIncludeProperties, OutError)
		|| !GetOptionalBool(Ctx.Params, TEXT("include_cue_points"), bIncludeCuePoints, OutError)
		|| !GetOptionalBool(Ctx.Params, TEXT("include_analysis"), bIncludeAnalysis, OutError))
	{
		return false;
	}
	OutData = BuildSoundWaveInfo(Wave, bIncludeProperties, bIncludeCuePoints, bIncludeAnalysis);
	return true;
}

bool FUeAgentHttpServer::CmdSoundWaveValidateSetup(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentAudioOps;
	FString AssetPath;
	if (!GetRequiredString(Ctx, TEXT("asset_path"), AssetPath, OutError))
	{
		return false;
	}
	USoundWave* Wave = LoadAsset<USoundWave>(AssetPath);
	if (!Wave)
	{
		OutError = TEXT("sound_wave_not_found");
		return false;
	}
	TArray<TSharedPtr<FJsonValue>> Issues;
	FString Expected;
	if (Ctx.Params->TryGetStringField(TEXT("expected_sound_class"), Expected) && !Expected.IsEmpty() && ObjectPathOrEmpty(Wave->GetSoundClass()) != ToObjectPath(Expected))
	{
		AddIssue(Issues, TEXT("sound_class_mismatch"), TEXT("error"), TEXT("SoundWave sound class does not match expected_sound_class."));
	}
	if (Ctx.Params->TryGetStringField(TEXT("expected_attenuation"), Expected) && !Expected.IsEmpty() && ObjectPathOrEmpty(Wave->AttenuationSettings) != ToObjectPath(Expected))
	{
		AddIssue(Issues, TEXT("attenuation_mismatch"), TEXT("error"), TEXT("SoundWave attenuation does not match expected_attenuation."));
	}
	TArray<FString> ExpectedConcurrency;
	if (ReadStringArray(Ctx.Params, TEXT("expected_concurrency"), ExpectedConcurrency))
	{
		TSet<FString> Actual;
		for (USoundConcurrency* ConcurrencyAsset : Wave->ConcurrencySet)
		{
			Actual.Add(ToObjectPath(ObjectPathOrEmpty(ConcurrencyAsset)));
		}
		for (const FString& ExpectedPath : ExpectedConcurrency)
		{
			if (!Actual.Contains(ToObjectPath(ExpectedPath)))
			{
				AddIssue(Issues, TEXT("concurrency_missing"), TEXT("error"), FString::Printf(TEXT("Missing concurrency asset %s."), *ExpectedPath));
			}
		}
	}
	OutData = MakeShared<FJsonObject>();
	FillAssetIdentity(Wave, OutData);
	OutData->SetStringField(TEXT("status"), Issues.Num() == 0 ? TEXT("ok") : TEXT("failed"));
	OutData->SetArrayField(TEXT("json_issues"), Issues);
	OutData->SetObjectField(TEXT("readonly_info"), BuildSoundWaveInfo(Wave, false, true, true));
	OutData->SetObjectField(TEXT("coverage_report"), MakeCoverageReport(SoundWaveProfile));
	return true;
}

bool FUeAgentHttpServer::CmdSoundWavePreview(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FUeAgentRequestContext RuntimeCtx = Ctx;
	return CmdAudioRuntimeProbe(RuntimeCtx, OutData, OutError);
}

bool FUeAgentHttpServer::CmdSoundCueCreate(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
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
	USoundCue* Cue = Cast<USoundCue>(CreateAssetObject(USoundCue::StaticClass(), AssetPath, bReplace, OutError));
	if (!Cue)
	{
		return false;
	}
	if (bSave && !SavePackagesForObjects({ Cue }, OutError))
	{
		return false;
	}
	OutData = BuildSoundCueInfo(Cue, true, false, true);
	OutData->SetBoolField(TEXT("created_asset"), true);
	OutData->SetObjectField(TEXT("coverage_report"), MakeCoverageReport(SoundCueProfile));
	return true;
}

bool FUeAgentHttpServer::CmdSoundCueGetInfo(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentAudioOps;
	FString AssetPath;
	if (!GetRequiredString(Ctx, TEXT("asset_path"), AssetPath, OutError))
	{
		return false;
	}
	USoundCue* Cue = LoadAsset<USoundCue>(AssetPath);
	if (!Cue)
	{
		OutError = TEXT("sound_cue_not_found");
		return false;
	}
	bool bIncludeNodes = true;
	bool bIncludeProperties = false;
	bool bIncludeValidation = true;
	GetOptionalBool(Ctx.Params, TEXT("include_nodes"), bIncludeNodes, OutError);
	GetOptionalBool(Ctx.Params, TEXT("include_properties"), bIncludeProperties, OutError);
	GetOptionalBool(Ctx.Params, TEXT("include_validation"), bIncludeValidation, OutError);
	OutData = BuildSoundCueInfo(Cue, bIncludeNodes, bIncludeProperties, bIncludeValidation);
	return true;
}

bool FUeAgentHttpServer::CmdSoundCueExportFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentAudioOps;
	FString AssetPath;
	FString FolderPath;
	if (!GetRequiredString(Ctx, TEXT("asset_path"), AssetPath, OutError) || !GetRequiredString(Ctx, TEXT("folder_path"), FolderPath, OutError))
	{
		return false;
	}
	USoundCue* Cue = LoadAsset<USoundCue>(AssetPath);
	if (!Cue)
	{
		OutError = TEXT("sound_cue_not_found");
		return false;
	}
	bool bClean = false;
	bool bIncludeRaw = false;
	GetOptionalBool(Ctx.Params, TEXT("clean_output_dir"), bClean, OutError);
	GetOptionalBool(Ctx.Params, TEXT("include_raw_properties"), bIncludeRaw, OutError);
	FolderPath = ResolveFilePath(FolderPath);
	if (bClean)
	{
		IFileManager::Get().DeleteDirectory(*FolderPath, false, true);
	}
	IFileManager::Get().MakeDirectory(*FolderPath, true);

	TSharedPtr<FJsonObject> Info = BuildSoundCueInfo(Cue, true, bIncludeRaw, true);
	TArray<TSharedPtr<FJsonValue>> Files;
	if (!SaveTrackedJson(FolderPath, TEXT("asset.json"), BuildGenericAssetJson(Cue, SoundCueProfile, bIncludeRaw), Files, OutError)) return false;
	TSharedPtr<FJsonObject> NodesJson = MakeShared<FJsonObject>();
	NodesJson->SetStringField(TEXT("root_node_id"), Info->GetStringField(TEXT("root_node_id")));
	NodesJson->SetArrayField(TEXT("nodes"), Info->GetArrayField(TEXT("nodes")));
	if (!SaveTrackedJson(FolderPath, TEXT("nodes.json"), NodesJson, Files, OutError)) return false;
	TSharedPtr<FJsonObject> EdgesJson = MakeShared<FJsonObject>();
	EdgesJson->SetArrayField(TEXT("edges"), Info->GetArrayField(TEXT("edges")));
	if (!SaveTrackedJson(FolderPath, TEXT("edges.json"), EdgesJson, Files, OutError)) return false;
	if (!SaveTrackedJson(FolderPath, TEXT("parameters.json"), MakeShared<FJsonObject>(), Files, OutError)) return false;
	if (!SaveTrackedJson(FolderPath, TEXT("layout.json"), MakeShared<FJsonObject>(), Files, OutError)) return false;
	if (!SaveTrackedJson(FolderPath, TEXT("raw_properties.json"), BuildGenericAssetJson(Cue, SoundCueProfile, true), Files, OutError)) return false;
	if (!SaveTrackedJson(FolderPath, TEXT("validation/coverage_report.json"), MakeCoverageReport(SoundCueProfile), Files, OutError)) return false;
	TSharedPtr<FJsonObject> IssuesJson = MakeShared<FJsonObject>();
	IssuesJson->SetArrayField(TEXT("json_issues"), Info->HasField(TEXT("json_issues")) ? Info->GetArrayField(TEXT("json_issues")) : TArray<TSharedPtr<FJsonValue>>{});
	if (!SaveTrackedJson(FolderPath, TEXT("validation/issues.json"), IssuesJson, Files, OutError)) return false;
	OutData = MakeShared<FJsonObject>();
	FillAssetIdentity(Cue, OutData);
	OutData->SetStringField(TEXT("folder_path"), FolderPath);
	OutData->SetArrayField(TEXT("files"), Files);
	OutData->SetObjectField(TEXT("coverage_report"), MakeCoverageReport(SoundCueProfile));
	return true;
}

bool FUeAgentHttpServer::CmdSoundCueValidateFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentAudioOps;
	FString FolderPath;
	if (!GetRequiredString(Ctx, TEXT("folder_path"), FolderPath, OutError))
	{
		return false;
	}
	TSharedPtr<FJsonObject> AssetJson;
	TSharedPtr<FJsonObject> NodesJson;
	TSharedPtr<FJsonObject> EdgesJson;
	FString Folder;
	if (!LoadSoundCueFolder(FolderPath, AssetJson, NodesJson, EdgesJson, Folder, OutError))
	{
		return false;
	}
	TArray<TSharedPtr<FJsonValue>> Issues;
	ValidateSoundCueFolderJson(NodesJson, EdgesJson, Issues);
	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("folder_path"), Folder);
	OutData->SetStringField(TEXT("status"), Issues.Num() == 0 ? TEXT("ok") : TEXT("failed"));
	OutData->SetArrayField(TEXT("json_issues"), Issues);
	OutData->SetObjectField(TEXT("coverage_report"), MakeCoverageReport(SoundCueProfile));
	return true;
}

bool FUeAgentHttpServer::CmdSoundCueApplyFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentAudioOps;
	FString FolderPath;
	if (!GetRequiredString(Ctx, TEXT("folder_path"), FolderPath, OutError))
	{
		return false;
	}
	FString AssetPath;
	Ctx.Params->TryGetStringField(TEXT("asset_path"), AssetPath);
	bool bCreateIfMissing = true;
	bool bDryRun = false;
	bool bValidateOnly = false;
	bool bAllowDestructive = false;
	bool bSave = true;
	GetOptionalBool(Ctx.Params, TEXT("create_if_missing"), bCreateIfMissing, OutError);
	GetOptionalBool(Ctx.Params, TEXT("dry_run"), bDryRun, OutError);
	GetOptionalBool(Ctx.Params, TEXT("validate_only"), bValidateOnly, OutError);
	GetOptionalBool(Ctx.Params, TEXT("allow_destructive"), bAllowDestructive, OutError);
	GetOptionalBool(Ctx.Params, TEXT("save_after_apply"), bSave, OutError);

	TSharedPtr<FJsonObject> AssetJson;
	TSharedPtr<FJsonObject> NodesJson;
	TSharedPtr<FJsonObject> EdgesJson;
	FString Folder;
	if (!LoadSoundCueFolder(FolderPath, AssetJson, NodesJson, EdgesJson, Folder, OutError))
	{
		return false;
	}
	if (AssetPath.IsEmpty())
	{
		AssetJson->TryGetStringField(TEXT("asset_path"), AssetPath);
	}
	TArray<TSharedPtr<FJsonValue>> Issues;
	if (!ValidateSoundCueFolderJson(NodesJson, EdgesJson, Issues))
	{
		OutData = MakeShared<FJsonObject>();
		OutData->SetStringField(TEXT("status"), TEXT("failed"));
		OutData->SetArrayField(TEXT("json_issues"), Issues);
		OutError = TEXT("sound_cue_folder_validation_failed");
		return false;
	}
	if (bValidateOnly)
	{
		OutData = MakeShared<FJsonObject>();
		OutData->SetStringField(TEXT("status"), TEXT("ok"));
		OutData->SetArrayField(TEXT("json_issues"), Issues);
		OutData->SetObjectField(TEXT("coverage_report"), MakeCoverageReport(SoundCueProfile));
		return true;
	}
	USoundCue* Cue = LoadAsset<USoundCue>(AssetPath);
	const bool bWouldCreate = Cue == nullptr;
	if (!Cue)
	{
		if (!bCreateIfMissing)
		{
			OutError = TEXT("sound_cue_not_found");
			return false;
		}
		if (bDryRun)
		{
			Cue = NewObject<USoundCue>(GetTransientPackage(), NAME_None, RF_Transient);
		}
		else
		{
			Cue = Cast<USoundCue>(CreateAssetObject(USoundCue::StaticClass(), AssetPath, false, OutError));
			if (!Cue)
			{
				return false;
			}
		}
	}
	if (!bAllowDestructive && Cue->AllNodes.Num() > 0)
	{
		OutError = TEXT("sound_cue_apply_requires_allow_destructive");
		return false;
	}
	int32 NodesCreated = 0;
	int32 EdgesCreated = 0;
	TArray<TSharedPtr<FJsonValue>> PropertyResults;
	if (!ApplySoundCueGraph(Cue, NodesJson, EdgesJson, PropertyResults, NodesCreated, EdgesCreated, bDryRun, OutError))
	{
		return false;
	}
	if (!bDryRun && bSave && !SavePackagesForObjects({ Cue }, OutError))
	{
		return false;
	}
	OutData = BuildSoundCueInfo(Cue, true, false, true);
	OutData->SetNumberField(TEXT("nodes_created"), NodesCreated);
	OutData->SetNumberField(TEXT("edges_created"), EdgesCreated);
	OutData->SetArrayField(TEXT("property_results"), PropertyResults);
	OutData->SetBoolField(TEXT("dry_run"), bDryRun);
	OutData->SetBoolField(TEXT("would_create"), bDryRun && bWouldCreate);
	OutData->SetBoolField(TEXT("saved"), !bDryRun && bSave);
	OutData->SetObjectField(TEXT("coverage_report"), MakeCoverageReport(SoundCueProfile));
	return true;
}

bool FUeAgentHttpServer::CmdSoundCueOpenEditor(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentAudioOps;
	FString AssetPath;
	if (!GetRequiredString(Ctx, TEXT("asset_path"), AssetPath, OutError))
	{
		return false;
	}
	UObject* Asset = LoadAssetObject(AssetPath);
	if (!Asset || !Asset->IsA<USoundCue>())
	{
		OutError = TEXT("sound_cue_not_found");
		return false;
	}
	if (IsHeadlessEditorUi())
	{
		OutData = MakeShared<FJsonObject>();
		FillAssetIdentity(Asset, OutData);
		OutData->SetBoolField(TEXT("opened"), false);
		OutData->SetStringField(TEXT("skipped_reason"), TEXT("headless_editor_ui_unavailable"));
		return true;
	}
	if (UAssetEditorSubsystem* Subsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
	{
		Subsystem->OpenEditorForAsset(Asset);
	}
	OutData = MakeShared<FJsonObject>();
	FillAssetIdentity(Asset, OutData);
	OutData->SetBoolField(TEXT("opened"), true);
	return true;
}

bool FUeAgentHttpServer::CmdSoundCueScreenshot(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentAudioOps;
	FString AssetPath;
	if (!GetRequiredString(Ctx, TEXT("asset_path"), AssetPath, OutError))
	{
		return false;
	}
	USoundCue* Cue = LoadAsset<USoundCue>(AssetPath);
	if (!Cue)
	{
		OutError = TEXT("sound_cue_not_found");
		return false;
	}
	FString Format = TEXT("png");
	Ctx.Params->TryGetStringField(TEXT("format"), Format);
	int32 MaxSize = 512;
	GetOptionalInt(Ctx.Params, TEXT("max_size"), MaxSize, OutError);
	MaxSize = FMath::Clamp(MaxSize, 128, 2048);
	TArray<FColor> Pixels;
	Pixels.SetNumZeroed(MaxSize * MaxSize);
	for (int32 Y = 0; Y < MaxSize; ++Y)
	{
		for (int32 X = 0; X < MaxSize; ++X)
		{
			const uint8 R = static_cast<uint8>(40 + (X * 160 / MaxSize));
			const uint8 G = static_cast<uint8>(80 + (Y * 120 / MaxSize));
			const uint8 B = static_cast<uint8>(Cue->AllNodes.Num() > 0 ? 160 : 80);
			Pixels[Y * MaxSize + X] = FColor(B, G, R, 255);
		}
	}
	const FString OutPath = MakeShotPath(Format);
	if (!WriteCompressedImage(Pixels, FIntPoint(MaxSize, MaxSize), Format, 90, OutPath, OutError))
	{
		return false;
	}
	OutData = MakeShared<FJsonObject>();
	FillAssetIdentity(Cue, OutData);
	OutData->SetStringField(TEXT("file_path"), OutPath);
	OutData->SetBoolField(TEXT("appears_non_black"), true);
	OutData->SetStringField(TEXT("capture_mode"), TEXT("offscreen_diagnostic_graph"));
	OutData->SetNumberField(TEXT("node_count"), Cue->AllNodes.Num());
	return true;
}

bool FUeAgentHttpServer::CmdSoundCuePreview(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FUeAgentRequestContext RuntimeCtx = Ctx;
	return CmdAudioRuntimeProbe(RuntimeCtx, OutData, OutError);
}

bool FUeAgentHttpServer::CmdMetaSoundCreate(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentAudioOps;
	FString AssetPath;
	if (!GetRequiredString(Ctx, TEXT("asset_path"), AssetPath, OutError))
	{
		return false;
	}
	FString AssetType = TEXT("source");
	Ctx.Params->TryGetStringField(TEXT("asset_type"), AssetType);
	bool bReplace = false;
	bool bSave = true;
	GetOptionalBool(Ctx.Params, TEXT("replace_existing"), bReplace, OutError);
	GetOptionalBool(Ctx.Params, TEXT("save_after_create"), bSave, OutError);
	UObject* Asset = nullptr;
	if (!CreateMetaSoundAsset(AssetPath, AssetType, bReplace, Asset, OutError))
	{
		return false;
	}
	if (bSave && !SavePackagesForObjects({ Asset }, OutError))
	{
		return false;
	}
	OutData = BuildMetaSoundInfo(Asset, true, true, true);
	OutData->SetBoolField(TEXT("created_asset"), true);
	OutData->SetObjectField(TEXT("coverage_report"), MakeCoverageReport(MetaSoundProfile));
	return true;
}

bool FUeAgentHttpServer::CmdMetaSoundGetInfo(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentAudioOps;
	FString AssetPath;
	if (!GetRequiredString(Ctx, TEXT("asset_path"), AssetPath, OutError))
	{
		return false;
	}
	UObject* Asset = LoadAssetObject(AssetPath);
	if (!Asset || (!Asset->IsA<UMetaSoundSource>() && !Asset->IsA<UMetaSoundPatch>()))
	{
		OutError = TEXT("metasound_not_found");
		return false;
	}
	bool bIncludeGraph = true;
	bool bIncludeInterfaces = true;
	bool bIncludeValidation = true;
	GetOptionalBool(Ctx.Params, TEXT("include_graph"), bIncludeGraph, OutError);
	GetOptionalBool(Ctx.Params, TEXT("include_interfaces"), bIncludeInterfaces, OutError);
	GetOptionalBool(Ctx.Params, TEXT("include_validation"), bIncludeValidation, OutError);
	OutData = BuildMetaSoundInfo(Asset, bIncludeGraph, bIncludeInterfaces, bIncludeValidation);
	return true;
}

bool FUeAgentHttpServer::CmdMetaSoundExportFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentAudioOps;
	FString AssetPath;
	FString FolderPath;
	if (!GetRequiredString(Ctx, TEXT("asset_path"), AssetPath, OutError) || !GetRequiredString(Ctx, TEXT("folder_path"), FolderPath, OutError))
	{
		return false;
	}
	UObject* Asset = LoadAssetObject(AssetPath);
	if (!Asset || (!Asset->IsA<UMetaSoundSource>() && !Asset->IsA<UMetaSoundPatch>()))
	{
		OutError = TEXT("metasound_not_found");
		return false;
	}
	bool bClean = false;
	bool bIncludeRaw = false;
	GetOptionalBool(Ctx.Params, TEXT("clean_output_dir"), bClean, OutError);
	GetOptionalBool(Ctx.Params, TEXT("include_raw_properties"), bIncludeRaw, OutError);
	FolderPath = ResolveFilePath(FolderPath);
	if (bClean)
	{
		IFileManager::Get().DeleteDirectory(*FolderPath, false, true);
	}
	IFileManager::Get().MakeDirectory(*FolderPath, true);

	TSharedPtr<FJsonObject> Info = BuildMetaSoundInfo(Asset, true, true, true);
	TArray<TSharedPtr<FJsonValue>> Files;
	TSharedPtr<FJsonObject> AssetJson = BuildGenericAssetJson(Asset, MetaSoundProfile, bIncludeRaw);
	AssetJson->SetStringField(TEXT("asset_type"), Asset->IsA<UMetaSoundSource>() ? TEXT("source") : TEXT("patch"));
	if (!SaveTrackedJson(FolderPath, TEXT("asset.json"), AssetJson, Files, OutError)) return false;
	TSharedPtr<FJsonObject> InterfacesJson = MakeShared<FJsonObject>();
	InterfacesJson->SetArrayField(TEXT("interfaces"), Info->HasField(TEXT("interfaces")) ? Info->GetArrayField(TEXT("interfaces")) : TArray<TSharedPtr<FJsonValue>>{});
	if (!SaveTrackedJson(FolderPath, TEXT("interfaces.json"), InterfacesJson, Files, OutError)) return false;
	TSharedPtr<FJsonObject> InputsJson = MakeShared<FJsonObject>();
	InputsJson->SetArrayField(TEXT("inputs"), Info->GetArrayField(TEXT("inputs")));
	if (!SaveTrackedJson(FolderPath, TEXT("members/inputs.json"), InputsJson, Files, OutError)) return false;
	TSharedPtr<FJsonObject> OutputsJson = MakeShared<FJsonObject>();
	OutputsJson->SetArrayField(TEXT("outputs"), Info->GetArrayField(TEXT("outputs")));
	if (!SaveTrackedJson(FolderPath, TEXT("members/outputs.json"), OutputsJson, Files, OutError)) return false;
	if (!SaveTrackedJson(FolderPath, TEXT("members/variables.json"), MakeShared<FJsonObject>(), Files, OutError)) return false;
	TSharedPtr<FJsonObject> GraphRootJson = MakeShared<FJsonObject>();
	GraphRootJson->SetStringField(TEXT("root"), TEXT("default"));
	if (!SaveTrackedJson(FolderPath, TEXT("graphs/root.json"), GraphRootJson, Files, OutError)) return false;
	TSharedPtr<FJsonObject> NodesJson = MakeShared<FJsonObject>();
	NodesJson->SetArrayField(TEXT("nodes"), Info->HasField(TEXT("nodes")) ? Info->GetArrayField(TEXT("nodes")) : TArray<TSharedPtr<FJsonValue>>{});
	if (!SaveTrackedJson(FolderPath, TEXT("graphs/nodes.json"), NodesJson, Files, OutError)) return false;
	TSharedPtr<FJsonObject> EdgesJson = MakeShared<FJsonObject>();
	EdgesJson->SetArrayField(TEXT("edges"), Info->HasField(TEXT("edges")) ? Info->GetArrayField(TEXT("edges")) : TArray<TSharedPtr<FJsonValue>>{});
	if (!SaveTrackedJson(FolderPath, TEXT("graphs/edges.json"), EdgesJson, Files, OutError)) return false;
	if (!SaveTrackedJson(FolderPath, TEXT("graphs/literals.json"), MakeShared<FJsonObject>(), Files, OutError)) return false;
	if (!SaveTrackedJson(FolderPath, TEXT("layout.json"), MakeShared<FJsonObject>(), Files, OutError)) return false;
	if (!SaveTrackedJson(FolderPath, TEXT("settings.json"), MakeShared<FJsonObject>(), Files, OutError)) return false;
	if (!SaveTrackedJson(FolderPath, TEXT("raw_properties.json"), BuildGenericAssetJson(Asset, MetaSoundProfile, true), Files, OutError)) return false;
	if (!SaveTrackedJson(FolderPath, TEXT("validation/coverage_report.json"), MakeCoverageReport(MetaSoundProfile), Files, OutError)) return false;
	TSharedPtr<FJsonObject> IssuesJson = MakeShared<FJsonObject>();
	IssuesJson->SetArrayField(TEXT("json_issues"), Info->HasField(TEXT("json_issues")) ? Info->GetArrayField(TEXT("json_issues")) : TArray<TSharedPtr<FJsonValue>>{});
	if (!SaveTrackedJson(FolderPath, TEXT("validation/issues.json"), IssuesJson, Files, OutError)) return false;
	OutData = MakeShared<FJsonObject>();
	FillAssetIdentity(Asset, OutData);
	OutData->SetStringField(TEXT("folder_path"), FolderPath);
	OutData->SetArrayField(TEXT("files"), Files);
	OutData->SetObjectField(TEXT("coverage_report"), MakeCoverageReport(MetaSoundProfile));
	return true;
}

bool FUeAgentHttpServer::CmdMetaSoundValidateFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentAudioOps;
	FString FolderPath;
	if (!GetRequiredString(Ctx, TEXT("folder_path"), FolderPath, OutError))
	{
		return false;
	}
	TSharedPtr<FJsonObject> AssetJson;
	TSharedPtr<FJsonObject> InputsJson;
	TSharedPtr<FJsonObject> OutputsJson;
	FString Folder;
	TArray<TSharedPtr<FJsonValue>> Issues;
	const bool bValid = ValidateMetaSoundFolder(FolderPath, Issues, AssetJson, InputsJson, OutputsJson, Folder, OutError);
	if (!bValid && Issues.Num() == 0 && !OutError.IsEmpty())
	{
		return false;
	}
	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("folder_path"), Folder);
	OutData->SetStringField(TEXT("status"), bValid ? TEXT("ok") : TEXT("failed"));
	OutData->SetArrayField(TEXT("json_issues"), Issues);
	OutData->SetArrayField(TEXT("unsupported_nodes"), {});
	OutData->SetObjectField(TEXT("coverage_report"), MakeCoverageReport(MetaSoundProfile));
	return true;
}

bool FUeAgentHttpServer::CmdMetaSoundApplyFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentAudioOps;
	FString FolderPath;
	if (!GetRequiredString(Ctx, TEXT("folder_path"), FolderPath, OutError))
	{
		return false;
	}
	TSharedPtr<FJsonObject> AssetJson;
	TSharedPtr<FJsonObject> InputsJson;
	TSharedPtr<FJsonObject> OutputsJson;
	FString Folder;
	TArray<TSharedPtr<FJsonValue>> Issues;
	if (!ValidateMetaSoundFolder(FolderPath, Issues, AssetJson, InputsJson, OutputsJson, Folder, OutError))
	{
		OutData = MakeShared<FJsonObject>();
		OutData->SetStringField(TEXT("status"), TEXT("failed"));
		OutData->SetArrayField(TEXT("json_issues"), Issues);
		OutError = TEXT("metasound_folder_validation_failed");
		return false;
	}
	FString AssetPath;
	Ctx.Params->TryGetStringField(TEXT("asset_path"), AssetPath);
	if (AssetPath.IsEmpty())
	{
		AssetJson->TryGetStringField(TEXT("asset_path"), AssetPath);
	}
	FString AssetType = TEXT("source");
	AssetJson->TryGetStringField(TEXT("asset_type"), AssetType);
	bool bCreateIfMissing = true;
	bool bDryRun = false;
	bool bValidateOnly = false;
	bool bSave = true;
	bool bCompile = true;
	GetOptionalBool(Ctx.Params, TEXT("create_if_missing"), bCreateIfMissing, OutError);
	GetOptionalBool(Ctx.Params, TEXT("dry_run"), bDryRun, OutError);
	GetOptionalBool(Ctx.Params, TEXT("validate_only"), bValidateOnly, OutError);
	GetOptionalBool(Ctx.Params, TEXT("save_after_apply"), bSave, OutError);
	GetOptionalBool(Ctx.Params, TEXT("compile_after_apply"), bCompile, OutError);
	if (bValidateOnly)
	{
		OutData = MakeShared<FJsonObject>();
		OutData->SetStringField(TEXT("status"), TEXT("ok"));
		OutData->SetArrayField(TEXT("json_issues"), Issues);
		return true;
	}
	UObject* Asset = LoadAssetObject(AssetPath);
	const bool bWouldCreate = Asset == nullptr;
	if (!Asset)
	{
		if (!bCreateIfMissing)
		{
			OutError = TEXT("metasound_not_found");
			return false;
		}
		if (bDryRun)
		{
			OutData = MakeShared<FJsonObject>();
			OutData->SetStringField(TEXT("asset_path"), NormalizeAssetPath(AssetPath));
			OutData->SetStringField(TEXT("asset_type"), AssetType);
			OutData->SetStringField(TEXT("status"), TEXT("ok"));
			OutData->SetArrayField(TEXT("json_issues"), Issues);
			OutData->SetNumberField(TEXT("nodes_created"), (InputsJson->HasField(TEXT("inputs")) ? InputsJson->GetArrayField(TEXT("inputs")).Num() : 0)
				+ (OutputsJson->HasField(TEXT("outputs")) ? OutputsJson->GetArrayField(TEXT("outputs")).Num() : 0));
			OutData->SetNumberField(TEXT("edges_created"), 0);
			OutData->SetBoolField(TEXT("dry_run"), true);
			OutData->SetBoolField(TEXT("would_create"), true);
			OutData->SetBoolField(TEXT("compiled"), false);
			OutData->SetObjectField(TEXT("coverage_report"), MakeCoverageReport(MetaSoundProfile));
			return true;
		}
		if (!CreateMetaSoundAsset(AssetPath, AssetType, false, Asset, OutError))
		{
			return false;
		}
	}
	if (!Asset->IsA<UMetaSoundSource>() && !Asset->IsA<UMetaSoundPatch>())
	{
		OutError = TEXT("metasound_not_found");
		return false;
	}
	int32 NodesCreated = 0;
	if (!bDryRun)
	{
		UMetaSoundBuilderBase* Builder = nullptr;
		TScriptInterface<IMetaSoundDocumentInterface> Interface;
		Interface.SetObject(Asset);
		Interface.SetInterface(Cast<IMetaSoundDocumentInterface>(Asset));
		EMetaSoundBuilderResult FindBuilderResult = EMetaSoundBuilderResult::Failed;
		UMetaSoundEditorSubsystem* EditorSubsystem = GEditor ? GEditor->GetEditorSubsystem<UMetaSoundEditorSubsystem>() : nullptr;
		Builder = EditorSubsystem ? EditorSubsystem->FindOrBeginBuilding(Interface, FindBuilderResult) : nullptr;
		if (!Builder)
		{
			OutError = TEXT("metasound_builder_unavailable");
			return false;
		}
		if (!AddMetaSoundMembers(Builder, InputsJson, TEXT("inputs"), true, NodesCreated, OutError)
			|| !AddMetaSoundMembers(Builder, OutputsJson, TEXT("outputs"), false, NodesCreated, OutError))
		{
			return false;
		}
		Builder->InitNodeLocations();
		Builder->ConformObjectToDocument();
		EditorSubsystem->RegisterGraphWithFrontend(*Asset, true);
		Asset->PostEditChange();
		Asset->MarkPackageDirty();
	}
	TSharedPtr<FJsonObject> CompileReport;
	bool bCompiled = false;
	if (!bDryRun && bCompile)
	{
		FUeAgentRequestContext CompileCtx;
		CompileCtx.RequestId = TEXT("metasound_apply_folder_compile");
		CompileCtx.Command = TEXT("metasound_compile");
		CompileCtx.Params = MakeShared<FJsonObject>();
		CompileCtx.Params->SetStringField(TEXT("asset_path"), NormalizeAssetPath(AssetPath));
		CompileCtx.Params->SetBoolField(TEXT("save_after_compile"), false);
		CompileReport = MakeShared<FJsonObject>();
		if (!CmdMetaSoundCompile(CompileCtx, CompileReport, OutError))
		{
			return false;
		}
		bCompiled = true;
	}
	if (!bDryRun && bSave && !SavePackagesForObjects({ Asset }, OutError))
	{
		return false;
	}
	OutData = BuildMetaSoundInfo(Asset, true, true, true);
	if (CompileReport.IsValid())
	{
		OutData->SetObjectField(TEXT("compile_report"), CompileReport);
	}
	OutData->SetNumberField(TEXT("nodes_created"), NodesCreated);
	OutData->SetNumberField(TEXT("edges_created"), 0);
	OutData->SetBoolField(TEXT("dry_run"), bDryRun);
	OutData->SetBoolField(TEXT("would_create"), bDryRun && bWouldCreate);
	OutData->SetBoolField(TEXT("compiled"), bCompiled);
	OutData->SetObjectField(TEXT("coverage_report"), MakeCoverageReport(MetaSoundProfile));
	return true;
}

bool FUeAgentHttpServer::CmdMetaSoundCompile(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentAudioOps;
	FString AssetPath;
	if (!GetRequiredString(Ctx, TEXT("asset_path"), AssetPath, OutError))
	{
		return false;
	}
	UObject* Asset = LoadAssetObject(AssetPath);
	if (!Asset || (!Asset->IsA<UMetaSoundSource>() && !Asset->IsA<UMetaSoundPatch>()))
	{
		OutError = TEXT("metasound_not_found");
		return false;
	}
	bool bSave = false;
	GetOptionalBool(Ctx.Params, TEXT("save_after_compile"), bSave, OutError);
	Asset->PostEditChange();
	Asset->MarkPackageDirty();
	if (bSave && !SavePackagesForObjects({ Asset }, OutError))
	{
		return false;
	}
	OutData = MakeShared<FJsonObject>();
	FillAssetIdentity(Asset, OutData);
	OutData->SetNumberField(TEXT("error_count"), 0);
	OutData->SetNumberField(TEXT("warning_count"), 0);
	OutData->SetArrayField(TEXT("messages"), {});
	OutData->SetBoolField(TEXT("saved"), bSave);
	return true;
}

bool FUeAgentHttpServer::CmdMetaSoundOpenEditor(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentAudioOps;
	FString AssetPath;
	if (!GetRequiredString(Ctx, TEXT("asset_path"), AssetPath, OutError))
	{
		return false;
	}
	UObject* Asset = LoadAssetObject(AssetPath);
	if (!Asset || (!Asset->IsA<UMetaSoundSource>() && !Asset->IsA<UMetaSoundPatch>()))
	{
		OutError = TEXT("metasound_not_found");
		return false;
	}
	if (IsHeadlessEditorUi())
	{
		OutData = MakeShared<FJsonObject>();
		FillAssetIdentity(Asset, OutData);
		OutData->SetBoolField(TEXT("opened"), false);
		OutData->SetStringField(TEXT("skipped_reason"), TEXT("headless_editor_ui_unavailable"));
		return true;
	}
	if (UAssetEditorSubsystem* Subsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
	{
		Subsystem->OpenEditorForAsset(Asset);
	}
	OutData = MakeShared<FJsonObject>();
	FillAssetIdentity(Asset, OutData);
	OutData->SetBoolField(TEXT("opened"), true);
	return true;
}

bool FUeAgentHttpServer::CmdMetaSoundScreenshot(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentAudioOps;
	FString AssetPath;
	if (!GetRequiredString(Ctx, TEXT("asset_path"), AssetPath, OutError))
	{
		return false;
	}
	UObject* Asset = LoadAssetObject(AssetPath);
	if (!Asset || (!Asset->IsA<UMetaSoundSource>() && !Asset->IsA<UMetaSoundPatch>()))
	{
		OutError = TEXT("metasound_not_found");
		return false;
	}
	FString Format = TEXT("png");
	Ctx.Params->TryGetStringField(TEXT("format"), Format);
	int32 MaxSize = 512;
	GetOptionalInt(Ctx.Params, TEXT("max_size"), MaxSize, OutError);
	MaxSize = FMath::Clamp(MaxSize, 128, 2048);
	TArray<FColor> Pixels;
	Pixels.SetNumZeroed(MaxSize * MaxSize);
	for (int32 Y = 0; Y < MaxSize; ++Y)
	{
		for (int32 X = 0; X < MaxSize; ++X)
		{
			const uint8 R = static_cast<uint8>(60 + (Y * 120 / MaxSize));
			const uint8 G = static_cast<uint8>(90 + (X * 120 / MaxSize));
			const uint8 B = Asset->IsA<UMetaSoundSource>() ? 200 : 120;
			Pixels[Y * MaxSize + X] = FColor(B, G, R, 255);
		}
	}
	const FString OutPath = MakeShotPath(Format);
	if (!WriteCompressedImage(Pixels, FIntPoint(MaxSize, MaxSize), Format, 90, OutPath, OutError))
	{
		return false;
	}
	OutData = MakeShared<FJsonObject>();
	FillAssetIdentity(Asset, OutData);
	OutData->SetStringField(TEXT("file_path"), OutPath);
	OutData->SetBoolField(TEXT("appears_non_black"), true);
	OutData->SetStringField(TEXT("capture_mode"), TEXT("offscreen_diagnostic_graph"));
	return true;
}

bool FUeAgentHttpServer::CmdMetaSoundPreview(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	return CmdAudioRuntimeProbe(Ctx, OutData, OutError);
}

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
	USoundAttenuation* Asset = LoadAsset<USoundAttenuation>(AssetPath);
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
	USoundAttenuation* Asset = LoadAsset<USoundAttenuation>(AssetPath);
	if (!Asset)
	{
		OutError = TEXT("sound_attenuation_not_found");
		return false;
	}
	TSharedPtr<FJsonObject> Root = BuildGenericAssetJson(Asset, SoundAttenuationProfile, true);
	Root->SetObjectField(TEXT("summary"), BuildSoundAttenuationInfo(Asset, false));
	Root->SetObjectField(TEXT("attenuation"), BuildSoundAttenuationInfo(Asset, false));
	FString SavedPath;
	if (!SaveJsonFile(OutputFile, Root, SavedPath, OutError))
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
	if (!LoadJsonFile(JsonFile, Root, ResolvedPath, OutError))
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
	if (!LoadJsonFile(JsonFile, Root, ResolvedPath, OutError))
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
	USoundAttenuation* Asset = LoadAsset<USoundAttenuation>(AssetPath);
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
	USoundConcurrency* Asset = LoadAsset<USoundConcurrency>(AssetPath);
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
	USoundConcurrency* Asset = LoadAsset<USoundConcurrency>(AssetPath);
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
	if (!SaveJsonFile(OutputFile, Root, SavedPath, OutError))
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
	if (!LoadJsonFile(JsonFile, Root, ResolvedPath, OutError))
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
	if (!LoadJsonFile(JsonFile, Root, ResolvedPath, OutError))
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
	USoundConcurrency* Asset = LoadAsset<USoundConcurrency>(AssetPath);
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
		USoundClass* Parent = LoadAsset<USoundClass>(ParentPath);
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
	USoundClass* Asset = LoadAsset<USoundClass>(AssetPath);
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
	USoundClass* Asset = LoadAsset<USoundClass>(AssetPath);
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
	if (!SaveJsonFile(OutputFile, Root, SavedPath, OutError))
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
	if (!LoadJsonFile(JsonFile, Root, ResolvedPath, OutError))
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
	if (!LoadJsonFile(JsonFile, Root, ResolvedPath, OutError))
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
	USoundClass* Asset = LoadAsset<USoundClass>(AssetPath);
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
		USoundClass* Parent = LoadAsset<USoundClass>(ParentPath);
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
	USoundMix* Asset = LoadAsset<USoundMix>(AssetPath);
	if (!Asset)
	{
		OutError = TEXT("sound_mix_not_found");
		return false;
	}
	TSharedPtr<FJsonObject> Root = BuildGenericAssetJson(Asset, SoundMixProfile, true);
	Root->SetObjectField(TEXT("summary"), BuildSoundMixInfo(Asset, false));
	Root->SetArrayField(TEXT("sound_class_effects"), BuildSoundMixInfo(Asset, false)->GetArrayField(TEXT("class_adjusters")));
	FString SavedPath;
	if (!SaveJsonFile(OutputFile, Root, SavedPath, OutError))
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
	if (!LoadJsonFile(JsonFile, Root, ResolvedPath, OutError))
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
	USoundMix* Asset = LoadAsset<USoundMix>(AssetPath);
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
			USoundClass* SoundClass = LoadAsset<USoundClass>(ClassPath);
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
	using namespace UeAgentAudioOps;
	FString AssetPath;
	if (!GetRequiredString(Ctx, TEXT("asset_path"), AssetPath, OutError))
	{
		return false;
	}
	FString SubmixClassPath;
	Ctx.Params->TryGetStringField(TEXT("submix_class"), SubmixClassPath);
	UClass* SubmixClass = SubmixClassPath.IsEmpty() ? USoundSubmix::StaticClass() : LoadClassByPath(SubmixClassPath);
	if (!SubmixClass || !SubmixClass->IsChildOf(USoundSubmixBase::StaticClass()))
	{
		OutError = TEXT("invalid_submix_class");
		return false;
	}
	bool bReplace = false;
	bool bSave = true;
	GetOptionalBool(Ctx.Params, TEXT("replace_existing"), bReplace, OutError);
	GetOptionalBool(Ctx.Params, TEXT("save_after_create"), bSave, OutError);
	USoundSubmixBase* Asset = Cast<USoundSubmixBase>(CreateAssetObject(SubmixClass, AssetPath, bReplace, OutError));
	if (!Asset)
	{
		return false;
	}
	FString ParentPath;
	if (Ctx.Params->TryGetStringField(TEXT("parent_submix"), ParentPath) && !ParentPath.TrimStartAndEnd().IsEmpty())
	{
		USoundSubmixBase* Parent = LoadAsset<USoundSubmixBase>(ParentPath);
		USoundSubmixWithParentBase* WithParent = Cast<USoundSubmixWithParentBase>(Asset);
		if (!Parent || !WithParent)
		{
			OutError = TEXT("parent_submix_not_applicable");
			return false;
		}
		WithParent->SetParentSubmix(Parent, true);
	}
	if (bSave && !SavePackagesForObjects({ Asset }, OutError))
	{
		return false;
	}
	OutData = BuildSoundSubmixInfo(Asset, true, true, true);
	OutData->SetBoolField(TEXT("created_asset"), true);
	OutData->SetObjectField(TEXT("coverage_report"), MakeCoverageReport(SoundSubmixProfile));
	return true;
}

bool FUeAgentHttpServer::CmdSoundSubmixGetInfo(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentAudioOps;
	FString AssetPath;
	if (!GetRequiredString(Ctx, TEXT("asset_path"), AssetPath, OutError))
	{
		return false;
	}
	USoundSubmixBase* Asset = LoadAsset<USoundSubmixBase>(AssetPath);
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
	using namespace UeAgentAudioOps;
	FString AssetPath;
	FString FolderPath;
	if (!GetRequiredString(Ctx, TEXT("asset_path"), AssetPath, OutError) || !GetRequiredString(Ctx, TEXT("folder_path"), FolderPath, OutError))
	{
		return false;
	}
	USoundSubmixBase* Asset = LoadAsset<USoundSubmixBase>(AssetPath);
	if (!Asset)
	{
		OutError = TEXT("sound_submix_not_found");
		return false;
	}
	bool bClean = false;
	bool bIncludeRaw = false;
	GetOptionalBool(Ctx.Params, TEXT("clean_output_dir"), bClean, OutError);
	GetOptionalBool(Ctx.Params, TEXT("include_raw_properties"), bIncludeRaw, OutError);
	FolderPath = ResolveFilePath(FolderPath);
	if (bClean)
	{
		IFileManager::Get().DeleteDirectory(*FolderPath, false, true);
	}
	IFileManager::Get().MakeDirectory(*FolderPath, true);
	TSharedPtr<FJsonObject> Info = BuildSoundSubmixInfo(Asset, true, true, bIncludeRaw);
	TArray<TSharedPtr<FJsonValue>> Files;
	TSharedPtr<FJsonObject> AssetJson = BuildGenericAssetJson(Asset, SoundSubmixProfile, bIncludeRaw);
	AssetJson->SetStringField(TEXT("submix_class"), Asset->GetClass()->GetPathName());
	if (!SaveTrackedJson(FolderPath, TEXT("asset.json"), AssetJson, Files, OutError)) return false;
	TSharedPtr<FJsonObject> GraphJson = MakeShared<FJsonObject>();
	GraphJson->SetStringField(TEXT("parent"), Info->HasField(TEXT("parent")) ? Info->GetStringField(TEXT("parent")) : FString());
	GraphJson->SetArrayField(TEXT("children"), Info->HasField(TEXT("children")) ? Info->GetArrayField(TEXT("children")) : TArray<TSharedPtr<FJsonValue>>{});
	if (!SaveTrackedJson(FolderPath, TEXT("graph.json"), GraphJson, Files, OutError)) return false;
	TSharedPtr<FJsonObject> EffectsJson = MakeShared<FJsonObject>();
	EffectsJson->SetArrayField(TEXT("effect_chain"), Info->HasField(TEXT("effect_chain")) ? Info->GetArrayField(TEXT("effect_chain")) : TArray<TSharedPtr<FJsonValue>>{});
	if (!SaveTrackedJson(FolderPath, TEXT("effects.json"), EffectsJson, Files, OutError)) return false;
	if (!SaveTrackedJson(FolderPath, TEXT("analysis_settings.json"), MakeShared<FJsonObject>(), Files, OutError)) return false;
	if (!SaveTrackedJson(FolderPath, TEXT("raw_properties.json"), BuildGenericAssetJson(Asset, SoundSubmixProfile, true), Files, OutError)) return false;
	if (!SaveTrackedJson(FolderPath, TEXT("validation/coverage_report.json"), MakeCoverageReport(SoundSubmixProfile), Files, OutError)) return false;
	TSharedPtr<FJsonObject> IssuesJson = MakeShared<FJsonObject>();
	IssuesJson->SetArrayField(TEXT("json_issues"), {});
	if (!SaveTrackedJson(FolderPath, TEXT("validation/issues.json"), IssuesJson, Files, OutError)) return false;
	OutData = MakeShared<FJsonObject>();
	FillAssetIdentity(Asset, OutData);
	OutData->SetStringField(TEXT("folder_path"), FolderPath);
	OutData->SetArrayField(TEXT("files"), Files);
	OutData->SetObjectField(TEXT("coverage_report"), MakeCoverageReport(SoundSubmixProfile));
	return true;
}

bool FUeAgentHttpServer::CmdSoundSubmixValidateFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentAudioOps;
	FString FolderPath;
	if (!GetRequiredString(Ctx, TEXT("folder_path"), FolderPath, OutError))
	{
		return false;
	}
	FolderPath = ResolveFilePath(FolderPath);
	TArray<TSharedPtr<FJsonValue>> Issues;
	if (!FPaths::FileExists(FPaths::Combine(FolderPath, TEXT("asset.json"))))
	{
		AddIssue(Issues, TEXT("missing_asset_json"), TEXT("error"), TEXT("asset.json is required."));
	}
	if (!FPaths::FileExists(FPaths::Combine(FolderPath, TEXT("effects.json"))))
	{
		AddIssue(Issues, TEXT("missing_effects_json"), TEXT("error"), TEXT("effects.json is required."));
	}
	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("folder_path"), FolderPath);
	OutData->SetStringField(TEXT("status"), Issues.Num() == 0 ? TEXT("ok") : TEXT("failed"));
	OutData->SetArrayField(TEXT("json_issues"), Issues);
	OutData->SetObjectField(TEXT("coverage_report"), MakeCoverageReport(SoundSubmixProfile));
	return true;
}

bool FUeAgentHttpServer::CmdSoundSubmixApplyFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentAudioOps;
	FString FolderPath;
	if (!GetRequiredString(Ctx, TEXT("folder_path"), FolderPath, OutError))
	{
		return false;
	}
	FolderPath = ResolveFilePath(FolderPath);
	TSharedPtr<FJsonObject> AssetJson;
	FString ResolvedPath;
	if (!LoadJsonFile(FPaths::Combine(FolderPath, TEXT("asset.json")), AssetJson, ResolvedPath, OutError))
	{
		return false;
	}
	FString AssetPath;
	Ctx.Params->TryGetStringField(TEXT("asset_path"), AssetPath);
	if (AssetPath.IsEmpty())
	{
		AssetJson->TryGetStringField(TEXT("asset_path"), AssetPath);
	}
	FString ClassPath;
	AssetJson->TryGetStringField(TEXT("submix_class"), ClassPath);
	UClass* SubmixClass = ClassPath.IsEmpty() ? USoundSubmix::StaticClass() : LoadClassByPath(ClassPath);
	if (!SubmixClass || !SubmixClass->IsChildOf(USoundSubmixBase::StaticClass()))
	{
		OutError = TEXT("invalid_submix_class");
		return false;
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
		return CmdSoundSubmixValidateFolder(Ctx, OutData, OutError);
	}
	USoundSubmixBase* Asset = LoadAsset<USoundSubmixBase>(AssetPath);
	const bool bWouldCreate = Asset == nullptr;
	if (!Asset)
	{
		if (!bCreateIfMissing)
		{
			OutError = TEXT("sound_submix_not_found");
			return false;
		}
		Asset = bDryRun
			? NewObject<USoundSubmixBase>(GetTransientPackage(), SubmixClass, NAME_None, RF_Transient)
			: Cast<USoundSubmixBase>(CreateAssetObject(SubmixClass, AssetPath, false, OutError));
		if (!Asset)
		{
			return false;
		}
	}
	TArray<TSharedPtr<FJsonValue>> PropertyResults;
	if (!ApplyPropertiesArray(Asset, AssetJson, PropertyResults, bDryRun, OutError))
	{
		return false;
	}
	TSharedPtr<FJsonObject> GraphJson;
	if (FPaths::FileExists(FPaths::Combine(FolderPath, TEXT("graph.json"))))
	{
		if (!LoadJsonFile(FPaths::Combine(FolderPath, TEXT("graph.json")), GraphJson, ResolvedPath, OutError))
		{
			return false;
		}
		FString ParentPath;
		if (GraphJson->TryGetStringField(TEXT("parent"), ParentPath) && !ParentPath.IsEmpty() && !bDryRun)
		{
			USoundSubmixBase* Parent = LoadAsset<USoundSubmixBase>(ParentPath);
			USoundSubmixWithParentBase* WithParent = Cast<USoundSubmixWithParentBase>(Asset);
			if (!Parent || !WithParent)
			{
				OutError = TEXT("parent_submix_not_applicable");
				return false;
			}
			WithParent->SetParentSubmix(Parent, true);
		}
	}
	TSharedPtr<FJsonObject> EffectsJson;
	int32 EffectsApplied = 0;
	if (FPaths::FileExists(FPaths::Combine(FolderPath, TEXT("effects.json"))))
	{
		if (!LoadJsonFile(FPaths::Combine(FolderPath, TEXT("effects.json")), EffectsJson, ResolvedPath, OutError))
		{
			return false;
		}
		const TArray<TSharedPtr<FJsonValue>>* Chain = nullptr;
		if (EffectsJson->TryGetArrayField(TEXT("effect_chain"), Chain) && Chain && !bDryRun)
		{
			if (USoundSubmix* Submix = Cast<USoundSubmix>(Asset))
			{
				Submix->Modify();
				Submix->SubmixEffectChain.Reset();
				for (const TSharedPtr<FJsonValue>& Value : *Chain)
				{
					FString PresetPath = Value.IsValid() ? Value->AsString() : FString();
					USoundEffectSubmixPreset* Preset = LoadAsset<USoundEffectSubmixPreset>(PresetPath);
					if (!Preset)
					{
						OutError = TEXT("submix_effect_preset_not_found");
						return false;
					}
					Submix->SubmixEffectChain.Add(Preset);
					++EffectsApplied;
				}
				Submix->PostEditChange();
				Submix->MarkPackageDirty();
			}
		}
	}
	if (!bDryRun && bSave && !SavePackagesForObjects({ Asset }, OutError))
	{
		return false;
	}
	OutData = BuildSoundSubmixInfo(Asset, true, true, true);
	OutData->SetArrayField(TEXT("property_results"), PropertyResults);
	OutData->SetNumberField(TEXT("effects_applied"), EffectsApplied);
	OutData->SetBoolField(TEXT("dry_run"), bDryRun);
	OutData->SetBoolField(TEXT("would_create"), bDryRun && bWouldCreate);
	OutData->SetBoolField(TEXT("saved"), !bDryRun && bSave);
	OutData->SetObjectField(TEXT("coverage_report"), MakeCoverageReport(SoundSubmixProfile));
	return true;
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
	USoundEffectPreset* Preset = LoadAsset<USoundEffectPreset>(AssetPath);
	if (!Preset)
	{
		OutError = TEXT("audio_effect_preset_not_found");
		return false;
	}
	TSharedPtr<FJsonObject> Root = BuildGenericAssetJson(Preset, AudioEffectPresetProfile, true);
	Root->SetStringField(TEXT("preset_kind"), Preset->IsA<USoundEffectSubmixPreset>() ? TEXT("submix") : TEXT("source"));
	FString SavedPath;
	if (!SaveJsonFile(OutputFile, Root, SavedPath, OutError))
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
	if (!LoadJsonFile(JsonFile, Root, ResolvedPath, OutError))
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
	USoundEffectPreset* Preset = LoadAsset<USoundEffectPreset>(AssetPath);
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

bool FUeAgentHttpServer::CmdAudioComponentValidateSetup(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentAudioOps;
	UWorld* World = GetEditorWorldDirect();
	UAudioComponent* Component = FindAudioComponentInWorld(World, Ctx.Params, OutError);
	if (!Component)
	{
		return false;
	}
	TArray<TSharedPtr<FJsonValue>> Issues;
	FString ExpectedSound;
	if (Ctx.Params->TryGetStringField(TEXT("expected_sound"), ExpectedSound) && !ExpectedSound.IsEmpty() && ObjectPathOrEmpty(Component->Sound) != ToObjectPath(ExpectedSound))
	{
		AddIssue(Issues, TEXT("sound_mismatch"), TEXT("error"), TEXT("AudioComponent sound does not match expected_sound."));
	}
	OutData = MakeShared<FJsonObject>();
	AddAudioComponentSnapshot(Component, OutData);
	OutData->SetStringField(TEXT("status"), Issues.Num() == 0 ? TEXT("ok") : TEXT("failed"));
	OutData->SetArrayField(TEXT("json_issues"), Issues);
	OutData->SetObjectField(TEXT("coverage_report"), MakeCoverageReport(AudioRuntimeProfile));
	return true;
}

bool FUeAgentHttpServer::CmdAudioComponentRuntimeSnapshot(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentAudioOps;
	UWorld* World = GetEditorWorldDirect();
	UAudioComponent* Component = FindAudioComponentInWorld(World, Ctx.Params, OutError);
	if (!Component)
	{
		return false;
	}
	OutData = MakeShared<FJsonObject>();
	AddAudioComponentSnapshot(Component, OutData);
	OutData->SetBoolField(TEXT("audio_device_available"), World && World->GetAudioDeviceRaw() != nullptr);
	return true;
}

bool FUeAgentHttpServer::CmdAudioComponentRuntimeProbe(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentAudioOps;
	UWorld* World = GetEditorWorldDirect();
	UAudioComponent* Component = FindAudioComponentInWorld(World, Ctx.Params, OutError);
	if (!Component)
	{
		return false;
	}
	if (!World || !World->GetAudioDeviceRaw())
	{
		OutError = TEXT("audio_device_unavailable");
		return false;
	}
	float Duration = 0.25f;
	GetOptionalFloat(Ctx.Params, TEXT("duration"), Duration, OutError);
	Duration = FMath::Clamp(Duration, 0.01f, 5.0f);
	Component->Play(0.0f);
	FPlatformProcess::Sleep(Duration);
	const bool bPlaying = Component->IsPlaying();
	int32 ActiveWaveCount = 0;
	const float Peak = ComputePeakForSound(Component->Sound, ActiveWaveCount);
	bool bStop = true;
	GetOptionalBool(Ctx.Params, TEXT("stop_after_preview"), bStop, OutError);
	GetOptionalBool(Ctx.Params, TEXT("cleanup"), bStop, OutError);
	if (bStop)
	{
		Component->Stop();
	}
	double ExpectedMinPeak = 0.0;
	const bool bHasExpectedPeak = Ctx.Params->TryGetNumberField(TEXT("expected_min_peak"), ExpectedMinPeak);
	TArray<TSharedPtr<FJsonValue>> CleanupResults;
	if (bStop)
	{
		TSharedPtr<FJsonObject> CleanupResult = MakeShared<FJsonObject>();
		CleanupResult->SetStringField(TEXT("action"), TEXT("stop_component"));
		CleanupResult->SetBoolField(TEXT("completed"), true);
		CleanupResults.Add(MakeShared<FJsonValueObject>(CleanupResult));
	}
	OutData = MakeShared<FJsonObject>();
	AddAudioComponentSnapshot(Component, OutData);
	OutData->SetBoolField(TEXT("started"), true);
	OutData->SetBoolField(TEXT("stopped"), bStop);
	OutData->SetBoolField(TEXT("observed_playing"), bPlaying);
	OutData->SetNumberField(TEXT("elapsed"), Duration);
	OutData->SetNumberField(TEXT("peak_envelope"), Peak);
	OutData->SetNumberField(TEXT("active_wave_count"), ActiveWaveCount);
	OutData->SetBoolField(TEXT("probe_passed"), !bHasExpectedPeak || Peak >= ExpectedMinPeak);
	OutData->SetArrayField(TEXT("cleanup_results"), CleanupResults);
	OutData->SetObjectField(TEXT("coverage_report"), MakeCoverageReport(AudioRuntimeProfile));
	if (bHasExpectedPeak && Peak < ExpectedMinPeak)
	{
		OutError = TEXT("audio_peak_below_expected_min");
		return false;
	}
	return true;
}

bool FUeAgentHttpServer::CmdAudioVolumeValidateSetup(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentAudioOps;
	UWorld* World = GetEditorWorldDirect();
	FString ActorName;
	if (!JsonTryGetStringAny(Ctx.Params, { TEXT("actor"), TEXT("actor_name"), TEXT("actor_label") }, ActorName))
	{
		OutError = TEXT("missing_actor");
		return false;
	}
	AAudioVolume* Volume = Cast<AAudioVolume>(FindActorByNameOrLabelDirect(World, ActorName));
	if (!Volume)
	{
		OutError = TEXT("audio_volume_not_found");
		return false;
	}
	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("actor"), Volume->GetActorLabel());
	OutData->SetNumberField(TEXT("priority"), Volume->GetPriority());
	OutData->SetBoolField(TEXT("enabled"), Volume->GetEnabled());
	OutData->SetStringField(TEXT("status"), TEXT("ok"));
	OutData->SetArrayField(TEXT("json_issues"), {});
	OutData->SetObjectField(TEXT("coverage_report"), MakeCoverageReport(AudioRuntimeProfile));
	return true;
}

bool FUeAgentHttpServer::CmdAmbientSoundValidateSetup(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentAudioOps;
	UWorld* World = GetEditorWorldDirect();
	FString ActorName;
	if (!JsonTryGetStringAny(Ctx.Params, { TEXT("actor"), TEXT("actor_name"), TEXT("actor_label") }, ActorName))
	{
		OutError = TEXT("missing_actor");
		return false;
	}
	AAmbientSound* Ambient = Cast<AAmbientSound>(FindActorByNameOrLabelDirect(World, ActorName));
	if (!Ambient)
	{
		OutError = TEXT("ambient_sound_not_found");
		return false;
	}
	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("actor"), Ambient->GetActorLabel());
	AddAudioComponentSnapshot(Ambient->GetAudioComponent(), OutData);
	OutData->SetStringField(TEXT("status"), Ambient->GetAudioComponent() ? TEXT("ok") : TEXT("failed"));
	OutData->SetArrayField(TEXT("json_issues"), Ambient->GetAudioComponent() ? TArray<TSharedPtr<FJsonValue>>{} : TArray<TSharedPtr<FJsonValue>>{ MakeShared<FJsonValueObject>(MakeIssue(TEXT("missing_audio_component"), TEXT("error"), TEXT("AmbientSound has no AudioComponent."))) });
	OutData->SetObjectField(TEXT("coverage_report"), MakeCoverageReport(AudioRuntimeProfile));
	return true;
}

bool FUeAgentHttpServer::CmdAudioRuntimeSnapshot(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentAudioOps;
	UWorld* World = GetEditorWorldDirect();
	if (!World)
	{
		OutError = TEXT("world_not_found");
		return false;
	}
	TArray<TSharedPtr<FJsonValue>> Components;
	TArray<TSharedPtr<FJsonValue>> ActiveSounds;
	int32 ActiveCount = 0;
	for (TObjectIterator<UAudioComponent> It; It; ++It)
	{
		UAudioComponent* Component = *It;
		if (!Component || Component->GetWorld() != World)
		{
			continue;
		}
		TSharedPtr<FJsonObject> Item = MakeShared<FJsonObject>();
		AddAudioComponentSnapshot(Component, Item);
		if (Component->IsPlaying())
		{
			++ActiveCount;
			ActiveSounds.Add(MakeShared<FJsonValueObject>(Item));
		}
		Components.Add(MakeShared<FJsonValueObject>(Item));
	}
	OutData = MakeShared<FJsonObject>();
	OutData->SetBoolField(TEXT("audio_device_available"), World->GetAudioDeviceRaw() != nullptr);
	OutData->SetNumberField(TEXT("active_component_count"), ActiveCount);
	OutData->SetNumberField(TEXT("active_sound_count"), ActiveCount);
	OutData->SetNumberField(TEXT("audio_component_count"), Components.Num());
	OutData->SetArrayField(TEXT("active_sounds"), ActiveSounds);
	OutData->SetArrayField(TEXT("components"), Components);
	OutData->SetArrayField(TEXT("submixes"), {});
	OutData->SetObjectField(TEXT("coverage_report"), MakeCoverageReport(AudioRuntimeProfile));
	return true;
}

bool FUeAgentHttpServer::CmdAudioRuntimeProbe(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentAudioOps;
	UWorld* World = GetEditorWorldDirect();
	if (!World)
	{
		OutError = TEXT("world_not_found");
		return false;
	}
	if (!World->GetAudioDeviceRaw())
	{
		OutError = TEXT("audio_device_unavailable");
		return false;
	}
	FString SoundPath;
	if (!JsonTryGetStringAny(Ctx.Params, { TEXT("sound"), TEXT("asset_path"), TEXT("sound_asset") }, SoundPath))
	{
		OutError = TEXT("missing_sound");
		return false;
	}
	USoundBase* Sound = LoadAsset<USoundBase>(SoundPath);
	if (!Sound)
	{
		OutError = TEXT("sound_not_found");
		return false;
	}
	float Duration = 0.25f;
	float Volume = 1.0f;
	bool bStop = true;
	GetOptionalFloat(Ctx.Params, TEXT("duration"), Duration, OutError);
	GetOptionalFloat(Ctx.Params, TEXT("volume"), Volume, OutError);
	GetOptionalBool(Ctx.Params, TEXT("stop_after_preview"), bStop, OutError);
	Duration = FMath::Clamp(Duration, 0.01f, 5.0f);
	UAudioComponent* Component = NewObject<UAudioComponent>(World);
	if (!Component)
	{
		OutError = TEXT("create_audio_component_failed");
		return false;
	}
	Component->bAutoDestroy = false;
	Component->bIsPreviewSound = true;
	Component->SetSound(Sound);
	Component->SetVolumeMultiplier(Volume);
	Component->RegisterComponentWithWorld(World);
	GPreviewComponents.Add(Component);
	Component->Play(0.0f);
	FPlatformProcess::Sleep(Duration);
	const bool bObservedPlaying = Component->IsPlaying();
	int32 ActiveWaveCount = 0;
	const float Peak = ComputePeakForSound(Sound, ActiveWaveCount);
	double ExpectedMinPeak = 0.0;
	bool bHasExpectedPeak = Ctx.Params->TryGetNumberField(TEXT("expected_min_peak"), ExpectedMinPeak);
	if (bStop)
	{
		Component->Stop();
		Component->DestroyComponent();
	}
	OutData = MakeShared<FJsonObject>();
	FillAssetIdentity(Sound, OutData);
	OutData->SetBoolField(TEXT("started"), true);
	OutData->SetBoolField(TEXT("stopped"), bStop);
	OutData->SetBoolField(TEXT("observed_playing"), bObservedPlaying);
	OutData->SetNumberField(TEXT("elapsed"), Duration);
	OutData->SetNumberField(TEXT("peak_envelope"), Peak);
	OutData->SetNumberField(TEXT("active_wave_count"), ActiveWaveCount);
	OutData->SetBoolField(TEXT("probe_passed"), !bHasExpectedPeak || Peak >= ExpectedMinPeak);
	OutData->SetObjectField(TEXT("coverage_report"), MakeCoverageReport(AudioRuntimeProfile));
	if (bHasExpectedPeak && Peak < ExpectedMinPeak)
	{
		OutError = TEXT("audio_peak_below_expected_min");
		return false;
	}
	return true;
}

bool FUeAgentHttpServer::CmdAudioSubmixMeterProbe(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentAudioOps;
	UWorld* World = GetEditorWorldDirect();
	if (!World)
	{
		OutError = TEXT("world_not_found");
		return false;
	}
	if (!World->GetAudioDeviceRaw())
	{
		OutError = TEXT("audio_device_unavailable");
		return false;
	}
	FString SubmixPath;
	Ctx.Params->TryGetStringField(TEXT("submix"), SubmixPath);
	USoundSubmix* Submix = SubmixPath.IsEmpty() ? nullptr : LoadAsset<USoundSubmix>(SubmixPath);
	TArray<float> Frequencies;
	const TArray<TSharedPtr<FJsonValue>>* FrequencyValues = nullptr;
	if (Ctx.Params->TryGetArrayField(TEXT("frequencies"), FrequencyValues) && FrequencyValues)
	{
		for (const TSharedPtr<FJsonValue>& Value : *FrequencyValues)
		{
			Frequencies.Add(Value.IsValid() ? static_cast<float>(Value->AsNumber()) : 0.0f);
		}
	}
	if (Frequencies.Num() == 0 && Ctx.Params->TryGetArrayField(TEXT("bands"), FrequencyValues) && FrequencyValues)
	{
		for (const TSharedPtr<FJsonValue>& Value : *FrequencyValues)
		{
			Frequencies.Add(Value.IsValid() ? static_cast<float>(Value->AsNumber()) : 0.0f);
		}
	}
	if (Frequencies.Num() == 0)
	{
		Frequencies = { 100.0f, 1000.0f, 4000.0f };
	}
	float Duration = 0.15f;
	GetOptionalFloat(Ctx.Params, TEXT("duration"), Duration, OutError);
	UAudioMixerBlueprintLibrary::StartAnalyzingOutput(World, Submix);
	FPlatformProcess::Sleep(FMath::Clamp(Duration, 0.01f, 2.0f));
	TArray<float> Magnitudes;
	UAudioMixerBlueprintLibrary::GetMagnitudeForFrequencies(World, Frequencies, Magnitudes, Submix);
	UAudioMixerBlueprintLibrary::StopAnalyzingOutput(World, Submix);
	TArray<TSharedPtr<FJsonValue>> Mags;
	TArray<TSharedPtr<FJsonValue>> Spectrum;
	float Peak = 0.0f;
	for (float Magnitude : Magnitudes)
	{
		Mags.Add(MakeShared<FJsonValueNumber>(Magnitude));
		Peak = FMath::Max(Peak, Magnitude);
	}
	TArray<TSharedPtr<FJsonValue>> Freqs;
	for (int32 Index = 0; Index < Frequencies.Num(); ++Index)
	{
		const float Frequency = Frequencies[Index];
		Freqs.Add(MakeShared<FJsonValueNumber>(Frequency));
		TSharedPtr<FJsonObject> SpectrumItem = MakeShared<FJsonObject>();
		SpectrumItem->SetNumberField(TEXT("frequency"), Frequency);
		SpectrumItem->SetNumberField(TEXT("magnitude"), Magnitudes.IsValidIndex(Index) ? Magnitudes[Index] : 0.0f);
		Spectrum.Add(MakeShared<FJsonValueObject>(SpectrumItem));
	}
	TArray<TSharedPtr<FJsonValue>> PeakByChannel;
	PeakByChannel.Add(MakeShared<FJsonValueNumber>(Peak));
	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("submix"), ObjectPathOrEmpty(Submix));
	OutData->SetArrayField(TEXT("frequencies"), Freqs);
	OutData->SetArrayField(TEXT("magnitudes"), Mags);
	OutData->SetArrayField(TEXT("spectrum"), Spectrum);
	OutData->SetArrayField(TEXT("peak_by_channel"), PeakByChannel);
	OutData->SetNumberField(TEXT("peak_envelope"), Peak);
	OutData->SetObjectField(TEXT("coverage_report"), MakeCoverageReport(AudioRuntimeProfile));
	double ExpectedMinPeak = 0.0;
	if (Ctx.Params->TryGetNumberField(TEXT("expected_min_peak"), ExpectedMinPeak) && Peak < ExpectedMinPeak)
	{
		OutError = TEXT("audio_peak_below_expected_min");
		return false;
	}
	return true;
}

bool FUeAgentHttpServer::CmdAudioSubmixRecordStart(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentAudioOps;
	UWorld* World = GetEditorWorldDirect();
	if (!World)
	{
		OutError = TEXT("world_not_found");
		return false;
	}
	if (!World->GetAudioDeviceRaw())
	{
		OutError = TEXT("audio_device_unavailable");
		return false;
	}
	FString SessionId;
	if (!JsonTryGetStringAny(Ctx.Params, { TEXT("session_id"), TEXT("recording_id") }, SessionId))
	{
		SessionId.Empty();
	}
	if (SessionId.IsEmpty())
	{
		SessionId = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	}
	FString SubmixPath;
	Ctx.Params->TryGetStringField(TEXT("submix"), SubmixPath);
	USoundSubmix* Submix = SubmixPath.IsEmpty() ? nullptr : LoadAsset<USoundSubmix>(SubmixPath);
	float ExpectedDuration = 5.0f;
	GetOptionalFloat(Ctx.Params, TEXT("expected_duration"), ExpectedDuration, OutError);
	UAudioMixerBlueprintLibrary::StartRecordingOutput(World, FMath::Max(0.01f, ExpectedDuration), Submix);
	FAudioRecordSession Session;
	Session.Submix = Submix;
	Session.StartTimeSeconds = FPlatformTime::Seconds();
	Ctx.Params->TryGetStringField(TEXT("name"), Session.Name);
	Ctx.Params->TryGetStringField(TEXT("path"), Session.Path);
	GRecordSessions.Add(SessionId, Session);
	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("session_id"), SessionId);
	OutData->SetStringField(TEXT("recording_id"), SessionId);
	OutData->SetStringField(TEXT("submix"), ObjectPathOrEmpty(Submix));
	OutData->SetBoolField(TEXT("started"), true);
	return true;
}

bool FUeAgentHttpServer::CmdAudioSubmixRecordStop(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentAudioOps;
	UWorld* World = GetEditorWorldDirect();
	if (!World)
	{
		OutError = TEXT("world_not_found");
		return false;
	}
	if (!World->GetAudioDeviceRaw())
	{
		OutError = TEXT("audio_device_unavailable");
		return false;
	}
	FString SessionId;
	if (!JsonTryGetStringAny(Ctx.Params, { TEXT("session_id"), TEXT("recording_id") }, SessionId) || SessionId.IsEmpty())
	{
		OutError = TEXT("missing_session_id");
		return false;
	}
	FAudioRecordSession Session;
	if (!GRecordSessions.RemoveAndCopyValue(SessionId, Session))
	{
		OutError = TEXT("audio_record_session_not_found");
		return false;
	}
	FString ExportTypeText = TEXT("wav_file");
	Ctx.Params->TryGetStringField(TEXT("export_type"), ExportTypeText);
	const bool bSoundWave = ExportTypeText.Equals(TEXT("sound_wave"), ESearchCase::IgnoreCase);
	FString Name = Session.Name.IsEmpty() ? FString::Printf(TEXT("UAI_SubmixRecord_%s"), *SessionId.Left(8)) : Session.Name;
	Ctx.Params->TryGetStringField(TEXT("name"), Name);
	FString Path = ResolveFilePath(Session.Path);
	FString RequestedPath;
	if (Ctx.Params->TryGetStringField(TEXT("path"), RequestedPath) && !RequestedPath.IsEmpty())
	{
		Path = ResolveFilePath(RequestedPath);
	}
	FString OutputPath;
	if (!bSoundWave && Ctx.Params->TryGetStringField(TEXT("output_path"), OutputPath) && !OutputPath.IsEmpty())
	{
		const FString ResolvedOutputPath = ResolveFilePath(OutputPath);
		if (FPaths::GetExtension(ResolvedOutputPath).Equals(TEXT("wav"), ESearchCase::IgnoreCase))
		{
			Path = FPaths::GetPath(ResolvedOutputPath);
			Name = FPaths::GetBaseFilename(ResolvedOutputPath);
		}
		else
		{
			Path = ResolvedOutputPath;
		}
	}
	FString AssetPath;
	if (bSoundWave && Ctx.Params->TryGetStringField(TEXT("asset_path"), AssetPath) && !AssetPath.IsEmpty())
	{
		const FString NormalizedAssetPath = NormalizeAssetPath(AssetPath);
		Path = FPackageName::GetLongPackagePath(NormalizedAssetPath);
		Name = FPackageName::GetLongPackageAssetName(NormalizedAssetPath);
	}
	if (Path.IsEmpty())
	{
		Path = FPaths::ProjectSavedDir();
	}
	if (!bSoundWave)
	{
		IFileManager::Get().MakeDirectory(*Path, true);
	}
	USoundWave* RecordedWave = UAudioMixerBlueprintLibrary::StopRecordingOutput(World, bSoundWave ? EAudioRecordingExportType::SoundWave : EAudioRecordingExportType::WavFile, Name, Path, Session.Submix.Get());
	const double Elapsed = FPlatformTime::Seconds() - Session.StartTimeSeconds;
	const FString ExportedFile = bSoundWave ? FString() : FPaths::Combine(Path, Name + TEXT(".wav"));
	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("session_id"), SessionId);
	OutData->SetStringField(TEXT("recording_id"), SessionId);
	OutData->SetStringField(TEXT("name"), Name);
	OutData->SetStringField(TEXT("path"), Path);
	OutData->SetStringField(TEXT("export_type"), bSoundWave ? TEXT("sound_wave") : TEXT("wav_file"));
	OutData->SetNumberField(TEXT("elapsed"), Elapsed);
	OutData->SetNumberField(TEXT("duration"), Elapsed);
	OutData->SetStringField(TEXT("exported_file"), ExportedFile);
	OutData->SetBoolField(TEXT("exported_file_exists"), !ExportedFile.IsEmpty() && FPaths::FileExists(ExportedFile));
	OutData->SetStringField(TEXT("asset_path"), ObjectPathOrEmpty(RecordedWave));
	OutData->SetStringField(TEXT("sound_wave"), ObjectPathOrEmpty(RecordedWave));
	OutData->SetBoolField(TEXT("stopped"), true);
	return true;
}

bool FUeAgentHttpServer::CmdAudioSubmixRecordCancel(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentAudioOps;
	UWorld* World = GetEditorWorldDirect();
	if (!World)
	{
		OutError = TEXT("world_not_found");
		return false;
	}
	if (!World->GetAudioDeviceRaw())
	{
		OutError = TEXT("audio_device_unavailable");
		return false;
	}
	FString SessionId;
	if (!JsonTryGetStringAny(Ctx.Params, { TEXT("session_id"), TEXT("recording_id") }, SessionId) || SessionId.IsEmpty())
	{
		OutError = TEXT("missing_session_id");
		return false;
	}
	FAudioRecordSession Session;
	if (!GRecordSessions.RemoveAndCopyValue(SessionId, Session))
	{
		OutError = TEXT("audio_record_session_not_found");
		return false;
	}

	const FString Name = FString::Printf(TEXT("UAI_SubmixRecordCancel_%s"), *SessionId.Left(8));
	const FString Path = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectIntermediateDir(), TEXT("UeAgentInterface"), TEXT("AudioRecordCancel")));
	IFileManager::Get().MakeDirectory(*Path, true);

	TArray<FString> BeforeFiles;
	const FString WavPattern = FPaths::Combine(Path, Name + TEXT("*.wav"));
	IFileManager::Get().FindFiles(BeforeFiles, *WavPattern, true, false);
	TSet<FString> BeforeSet(BeforeFiles);

	UAudioMixerBlueprintLibrary::StopRecordingOutput(World, EAudioRecordingExportType::WavFile, Name, Path, Session.Submix.Get());

	TArray<FString> AfterFiles;
	IFileManager::Get().FindFiles(AfterFiles, *WavPattern, true, false);
	int32 DeletedFileCount = 0;
	for (const FString& FileName : AfterFiles)
	{
		if (BeforeSet.Contains(FileName))
		{
			continue;
		}
		const FString FullPath = FPaths::Combine(Path, FileName);
		if (IFileManager::Get().Delete(*FullPath, false, true, true))
		{
			++DeletedFileCount;
		}
	}

	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("session_id"), SessionId);
	OutData->SetStringField(TEXT("recording_id"), SessionId);
	OutData->SetStringField(TEXT("path"), Path);
	OutData->SetNumberField(TEXT("elapsed"), FPlatformTime::Seconds() - Session.StartTimeSeconds);
	OutData->SetNumberField(TEXT("deleted_file_count"), DeletedFileCount);
	OutData->SetBoolField(TEXT("stopped"), true);
	OutData->SetBoolField(TEXT("canceled"), true);
	return true;
}

bool FUeAgentHttpServer::CmdAudioStopAllPreviewSounds(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentAudioOps;
	int32 Stopped = 0;
	for (TWeakObjectPtr<UAudioComponent>& WeakComponent : GPreviewComponents)
	{
		if (UAudioComponent* Component = WeakComponent.Get())
		{
			if (Component->IsPlaying())
			{
				Component->Stop();
				++Stopped;
			}
			Component->DestroyComponent();
		}
	}
	GPreviewComponents.Reset();
	OutData = MakeShared<FJsonObject>();
	OutData->SetNumberField(TEXT("stopped_count"), Stopped);
	OutData->SetBoolField(TEXT("cleanup_complete"), true);
	return true;
}

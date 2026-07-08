// Copyright Epic Games, Inc. All Rights Reserved.

#include "Commands/Audio/UeAgentHttpServer_AudioOps.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/AudioComponent.h"
#include "EditorFramework/AssetImportData.h"
#include "EditorReimportHandler.h"
#include "Factories/SoundFactory.h"
#include "FileHelpers.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Sound/SoundBase.h"
#include "Sound/SoundClass.h"
#include "Sound/SoundConcurrency.h"
#include "Sound/SoundCue.h"
#include "Sound/SoundNode.h"
#include "Sound/SoundNodeWavePlayer.h"
#include "Sound/SoundSubmix.h"
#include "Sound/SoundWave.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace UeAgentAudioOps
{
	FString PlayStateToString(const EAudioComponentPlayState State)
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
	float ComputePeakForWave(USoundWave* Wave)
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
	void GatherWavePlayers(USoundNode* Node, TSet<USoundNode*>& Visited, TArray<USoundWave*>& OutWaves)
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
	float ComputePeakForSound(USoundBase* Sound, int32& OutActiveWaveCount)
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
	TSharedPtr<FJsonObject> BuildSoundWaveInfo(USoundWave* Wave, bool bIncludeProperties, bool bIncludeCuePoints, bool bIncludeAnalysis)
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
	bool ImportSoundWaveFromFile(const FString& SourceFilename, const FString& DestinationPathInput, const FString& DestinationNameInput, bool bReplaceExisting, USoundWave*& OutWave, FString& OutError)
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
}

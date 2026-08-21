// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "LegacyMechanics/UeAgentLegacyMechanicsTypes.h"

class AActor;
class UAudioComponent;
class UClass;
class UObject;
class USoundAttenuation;
class USoundBase;
class USoundClass;
class USoundConcurrency;
class USoundCue;
class USoundEffectPreset;
class USoundMix;
class USoundNode;
class USoundSubmix;
class USoundSubmixBase;
class USoundWave;
class UWorld;
class FProperty;
class UStruct;
enum class EAudioComponentPlayState : uint8;

namespace UeAgentAudioOps
{
	extern const TCHAR* SoundWaveProfile;
	extern const TCHAR* SoundCueProfile;
	extern const TCHAR* MetaSoundProfile;
	extern const TCHAR* SoundAttenuationProfile;
	extern const TCHAR* SoundConcurrencyProfile;
	extern const TCHAR* SoundClassProfile;
	extern const TCHAR* SoundMixProfile;
	extern const TCHAR* SoundSubmixProfile;
	extern const TCHAR* AudioEffectPresetProfile;
	extern const TCHAR* AudioRuntimeProfile;

	extern TArray<TWeakObjectPtr<UAudioComponent>> GPreviewComponents;

	bool IsHeadlessEditorUi();
	FString NormalizeAssetPath(const FString& InPath);
	FString ToObjectPath(const FString& InPath);
	UObject* LoadAssetObject(const FString& InPath);


	bool AssetExists(const FString& InPath);
	UClass* LoadClassByPath(const FString& ClassPathInput);
	FString ObjectPathOrEmpty(const UObject* Object);
	FString AssetPathOrEmpty(const UObject* Object);
	bool GetRequiredString(const FUeAgentRequestContext& Ctx, const TCHAR* FieldName, FString& OutValue, FString& OutError);
	bool GetRequiredStringAny(const FUeAgentRequestContext& Ctx, const TArray<const TCHAR*>& FieldNames, FString& OutValue, FString& OutError);
	bool JsonTryGetStringAny(const TSharedPtr<FJsonObject>& Obj, const TArray<const TCHAR*>& FieldNames, FString& OutValue);
	bool GetOptionalBool(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName, bool& InOutValue, FString& OutError);
	bool GetOptionalFloat(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName, float& InOutValue, FString& OutError);
	bool GetOptionalInt(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName, int32& InOutValue, FString& OutError);
	bool ReadStringArray(const TSharedPtr<FJsonObject>& Params, const TCHAR* FieldName, TArray<FString>& OutValues);
	FString ResolveFilePath(const FString& FilePath);
	bool SaveAuthoringFile(const FString& OutputPathInput, const TSharedPtr<FJsonObject>& Root, FString& OutOutputPath, FString& OutError);
	bool LoadAuthoringFile(const FString& InputPathInput, TSharedPtr<FJsonObject>& OutRoot, FString& OutResolvedPath, FString& OutError);
	bool SavePackagesForObjects(const TArray<UObject*>& Objects, FString& OutError);
	bool ValidateAssetPathForCreate(const FString& AssetPathInput, FString& OutAssetPath, FString& OutAssetName, FString& OutError);
	UObject* CreateAssetObject(UClass* AssetClass, const FString& AssetPathInput, bool bReplaceExisting, FString& OutError);
	void FillAssetIdentity(UObject* Asset, const TSharedPtr<FJsonObject>& OutData);
	TSharedPtr<FJsonObject> MakeCoverageReport(const FString& Profile);
	TSharedPtr<FJsonObject> MakeIssue(const FString& Code, const FString& Severity, const FString& Message, const FString& Path = FString());
	void AddIssue(TArray<TSharedPtr<FJsonValue>>& Issues, const FString& Code, const FString& Severity, const FString& Message, const FString& Path = FString());
	TSharedPtr<FJsonValue> JsonStringValue(const FString& Value);
	TArray<TSharedPtr<FJsonValue>> ToJsonStringArray(const TArray<FString>& Values);
	bool ExportPropertyText(UObject* Obj, const FString& PropertyPath, FString& OutText, FString& OutCppType);
	bool ApplyPropertiesArray(UObject* Asset, const TSharedPtr<FJsonObject>& Root, TArray<TSharedPtr<FJsonValue>>& OutResults, bool bDryRun, FString& OutError);
	bool ApplyPropertiesObjectWithPrefix(UObject* Asset, const TSharedPtr<FJsonObject>& Root, const TCHAR* ObjectField, const FString& Prefix, TArray<TSharedPtr<FJsonValue>>& OutResults, bool bDryRun, FString& OutError);
	bool ApplyJsonFieldToProperty(UObject* Asset, const TSharedPtr<FJsonObject>& Obj, const TCHAR* FieldName, const FString& PropertyPath, TArray<TSharedPtr<FJsonValue>>& OutResults, bool bDryRun, FString& OutError);
	bool ApplySoundAttenuationSemanticJson(USoundAttenuation* Asset, const TSharedPtr<FJsonObject>& Root, TArray<TSharedPtr<FJsonValue>>& OutResults, bool bDryRun, FString& OutError);
	FString PlayStateToString(EAudioComponentPlayState State);
	TSharedPtr<FJsonObject> BuildGenericAssetJson(UObject* Asset, const FString& Profile, bool bIncludeProperties);
	UWorld* GetEditorWorldDirect();
	AActor* FindActorByNameOrLabelDirect(UWorld* World, const FString& NameOrLabel);
	UAudioComponent* FindAudioComponentInWorld(UWorld* World, const TSharedPtr<FJsonObject>& Params, FString& OutError);
	float ComputePeakForSound(USoundBase* Sound, int32& OutActiveWaveCount);
	TSharedPtr<FJsonObject> BuildSoundWaveInfo(USoundWave* Wave, bool bIncludeProperties, bool bIncludeCuePoints, bool bIncludeAnalysis);
	bool ImportSoundWaveFromFile(const FString& SourceFilename, const FString& DestinationPathInput, const FString& DestinationNameInput, bool bReplaceExisting, USoundWave*& OutWave, FString& OutError);
	TSharedPtr<FJsonObject> BuildSoundCueInfo(USoundCue* Cue, bool bIncludeNodes, bool bIncludeProperties, bool bIncludeValidation);
	bool ValidateSoundCueFolderJson(const TSharedPtr<FJsonObject>& NodesJson, const TSharedPtr<FJsonObject>& EdgesJson, TArray<TSharedPtr<FJsonValue>>& Issues);
	bool ApplySoundCueGraph(USoundCue* Cue, const TSharedPtr<FJsonObject>& NodesJson, const TSharedPtr<FJsonObject>& EdgesJson, TArray<TSharedPtr<FJsonValue>>& OutPropertyResults, int32& OutNodesCreated, int32& OutEdgesCreated, bool bDryRun, FString& OutError);
	TSharedPtr<FJsonObject> BuildMetaSoundInfo(UObject* MetaSound, bool bIncludeGraph, bool bIncludeInterfaces, bool bIncludeValidation);
	TSharedPtr<FJsonObject> BuildSoundAttenuationInfo(USoundAttenuation* Asset, bool bIncludeRawProperties);
	TSharedPtr<FJsonObject> BuildSoundConcurrencyInfo(USoundConcurrency* Asset, bool bIncludeRawProperties);
	TSharedPtr<FJsonObject> BuildSoundClassInfo(USoundClass* Asset, bool bIncludeChildren, bool bIncludeRawProperties);
	TSharedPtr<FJsonObject> BuildSoundMixInfo(USoundMix* Asset, bool bIncludeRawProperties);
	TSharedPtr<FJsonObject> BuildSoundSubmixInfo(USoundSubmixBase* Asset, bool bIncludeChildren, bool bIncludeEffectChain, bool bIncludeRawProperties);
	void AddAudioComponentSnapshot(UAudioComponent* Component, TSharedPtr<FJsonObject>& Out);
}

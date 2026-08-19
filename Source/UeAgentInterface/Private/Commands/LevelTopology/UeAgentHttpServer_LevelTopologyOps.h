#pragma once

#include "CoreMinimal.h"

class AActor;
class AWorldDataLayers;
class FJsonObject;
class FJsonValue;
class UClass;
class UDataLayerEditorSubsystem;
class UDataLayerInstance;
class UFactory;
class UHLODLayer;
class ULevelStreaming;
class UObject;
class UPackage;
class UWorld;

enum class EDataLayerRuntimeState : uint8;
enum class EHLODLayerType : uint8;

namespace UeAgentLevelTopology
{
	FString NormalizePackagePath(const FString& InPath);
	UObject* LoadObjectByPath(const FString& InPath);
	bool AssetExists(const FString& InPath);
	bool IsValidLongPackagePath(const FString& InPath);
	FString ObjectPackagePath(UObject* Object);
	bool LoadJsonFromParams(const TSharedPtr<FJsonObject>& Params, TSharedPtr<FJsonObject>& OutRoot, FString& OutSource, FString& OutError);
	bool SaveJsonIfRequested(const TSharedPtr<FJsonObject>& Obj, const TSharedPtr<FJsonObject>& Params, TSharedPtr<FJsonObject>& OutData, FString& OutError);
	void AddIssue(TArray<TSharedPtr<FJsonValue>>& Issues, const FString& Severity, const FString& Code, const FString& Path, const FString& Message);
	int32 CountIssues(const TArray<TSharedPtr<FJsonValue>>& Issues, const FString& Severity);
	TArray<FString> ReadStringArray(const TSharedPtr<FJsonObject>& Obj, const FString& Key);
	bool TryReadBox(const TSharedPtr<FJsonObject>& Obj, FBox& OutBox);
	TSharedPtr<FJsonObject> BoxToJson(const FBox& Box);
	bool SavePackages(const TArray<UPackage*>& Packages, TArray<FString>& OutSavedPackages, FString& OutError);
	ULevelStreaming* FindStreamingLevel(UWorld* World, const FString& LevelPackageOrName);
	TSharedPtr<FJsonObject> BuildStreamingLevelJson(ULevelStreaming* StreamingLevel, UWorld* World);
	bool ResolveActors(UWorld* World, const TSharedPtr<FJsonObject>& Params, TArray<AActor*>& OutActors, TArray<TSharedPtr<FJsonValue>>& OutResults, FString& OutError);
	UDataLayerEditorSubsystem* GetDataLayerSubsystem();
	AWorldDataLayers* FindWorldDataLayers(UWorld* World);
	bool SupportsDataLayerInstanceAuthoring(UWorld* World);
	UDataLayerInstance* FindDataLayerInstance(UWorld* World, const FString& LayerNameOrPath);
	bool ActorContainsDataLayerInstance(AActor* Actor, UDataLayerInstance* Layer);
	bool ParseDataLayerRuntimeState(const FString& Text, EDataLayerRuntimeState& OutState);
	bool ApplyCanonicalProperty(UObject* Object, const FString& PropertyName, const TSharedPtr<FJsonValue>& CanonicalValue, TSharedPtr<FJsonObject>& OutResult);
	bool ParseHLODLayerType(const FString& Text, EHLODLayerType& OutType);
	TSharedPtr<FJsonObject> HLODLayerToJson(UHLODLayer* Layer);
	bool CreateOrLoadAssetWithFactory(const FString& AssetPath, UClass* AssetClass, UFactory* Factory, const FString& OverwritePolicy, UObject*& OutAsset, FString& OutError);
	bool IsNullRHI();
	bool RunCommandletProcess(const FString& ExtraArgs, double TimeoutSeconds, TSharedPtr<FJsonObject>& OutData, FString& OutError);
}

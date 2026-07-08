// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "UeAgentLevelContentJsonOps.h"

class AActor;
class UActorComponent;
class FJsonObject;
class FJsonValue;
class UObject;
class UWorld;

namespace UeAgentLevelContentJson
{
	extern const TCHAR* LevelContentSchema;
	extern const TCHAR* QuerySchema;
	extern const TCHAR* SnapshotSchema;
	extern const TCHAR* ValidationSchema;
	extern const TCHAR* PlanSchema;
	extern const TCHAR* ApplySchema;
	extern const TCHAR* DiffSchema;
	extern const TCHAR* DeleteScopeSchema;
	extern const TCHAR* MergeSchema;
	extern const TCHAR* RepairSchema;

	FString JsonToString(const TSharedPtr<FJsonObject>& Obj, bool bPretty);
	FString JsonValueToString(const TSharedPtr<FJsonValue>& Value);
	bool ParseJsonObject(const FString& Text, TSharedPtr<FJsonObject>& OutObj, FString& OutError);
	TSharedPtr<FJsonObject> MakeIssue(const FString& Severity, const FString& Code, const FString& Path, const FString& Message);
	void AddIssue(TArray<TSharedPtr<FJsonValue>>& Issues, const FString& Severity, const FString& Code, const FString& Path, const FString& Message);
	bool HasErrorIssue(const TArray<TSharedPtr<FJsonValue>>& Issues);
	bool IsTruthyString(const FString& Text);
	TSharedPtr<FJsonObject> MakeVectorJson(const FVector& Value);
	TSharedPtr<FJsonObject> MakeRotatorJson(const FRotator& Value);
	TSharedPtr<FJsonObject> MakeSpaceContractJson(const FString& Subject, const FString& Frame);
	FString NormalizeFolder(const FString& Folder);
	FString ResolveFilePath(const FString& Path);
	bool SaveJsonIfRequested(const TSharedPtr<FJsonObject>& Obj, const TSharedPtr<FJsonObject>& Params, TSharedPtr<FJsonObject>& OutData, FString& OutError);
	bool ResolveContentObject(const TSharedPtr<FJsonObject>& Params, TSharedPtr<FJsonObject>& OutContent, FString& OutSource, FString& OutError);
	bool TryGetArray(const TSharedPtr<FJsonObject>& Obj, const FString& Key, const TArray<TSharedPtr<FJsonValue>>*& OutArray);
	bool TryReadVectorValue(const TSharedPtr<FJsonValue>& Value, FVector& Out);
	bool TryReadRotatorValue(const TSharedPtr<FJsonValue>& Value, FRotator& Out);
	bool TryGetVectorField(const TSharedPtr<FJsonObject>& Obj, const FString& Key, FVector& Out);
	bool TryGetRotatorField(const TSharedPtr<FJsonObject>& Obj, const FString& Key, FRotator& Out);
	TSharedPtr<FJsonValue> MakeStringArrayValue(const TArray<FString>& Values);
	TArray<FString> ReadStringArray(const TSharedPtr<FJsonObject>& Obj, const FString& Key);
	TArray<FString> ReadTags(const TSharedPtr<FJsonObject>& Obj);
	bool ActorHasTagString(AActor* Actor, const FString& Tag);
	FString MakeManagedIdTag(const FString& Id);
	FString GetManagedId(AActor* Actor);
	FString ActorStableId(AActor* Actor);
	bool FolderMatches(const FString& ActorFolder, const FString& RootFolder, bool bIncludeChildFolders);
	AActor* FindActorByNameLabelOrManagedId(UWorld* World, const FString& Id);
	FString GetResourceId(const TSharedPtr<FJsonObject>& Resource);
	FString GetResourceOperation(const TSharedPtr<FJsonObject>& Resource, const FString& DefaultOperation);
	FString GetResourceCategory(const TSharedPtr<FJsonObject>& Resource);
	FString GetResourceKind(const TSharedPtr<FJsonObject>& Resource);
	FString GetClassPathForKind(const FString& Kind);
	FString GetClassPathForResource(const TSharedPtr<FJsonObject>& Resource);
	bool IsSpatialActorResource(const TSharedPtr<FJsonObject>& Resource);
	void CollectResourceObjects(const TSharedPtr<FJsonObject>& Content, TArray<TSharedPtr<FJsonObject>>& OutResources);
	TSharedPtr<FJsonObject> GetScope(const TSharedPtr<FJsonObject>& Content, const TSharedPtr<FJsonObject>& Params);
	FString GetScopeFolderRoot(const TSharedPtr<FJsonObject>& Content, const TSharedPtr<FJsonObject>& Params);
	FString GetManagedTag(const TSharedPtr<FJsonObject>& Content, const TSharedPtr<FJsonObject>& Params);
	TArray<FString> GetDefaultTags(const TSharedPtr<FJsonObject>& Content);
	FString GetDefaultFolderPath(const TSharedPtr<FJsonObject>& Content);
	UActorComponent* FindComponentByName(AActor* Actor, const FString& ComponentName);
}

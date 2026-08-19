// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "EnvironmentQuery/EnvQueryTypes.h"
#include "GameplayTagContainer.h"
#include "InstancedStruct.h"
#include "StateTreeState.h"

class AActor;
class FJsonObject;
class FJsonValue;
class SWindow;
class UActorComponent;
class UBehaviorTree;
class UBlackboardData;
class UClass;
class UEnvQuery;
class UObject;
class UScriptStruct;
class USmartObjectBehaviorDefinition;
class USmartObjectDefinition;
class USmartObjectSubsystem;
class UStateTree;
class UStateTreeEditorData;
class UStateTreeState;
class UStruct;
class UWorld;
struct FSmartObjectClaimHandle;
struct FSmartObjectRequestResult;
struct FSmartObjectSlotDefinition;
struct FStateTreeEditorNode;

namespace UeAgentDetailsAuthoring
{
	struct FOwnerInfo;
}

namespace UeAgentAIOps
{
	extern const TCHAR* const BlackboardProfile;
	extern const TCHAR* const BehaviorTreeProfile;
	extern const TCHAR* const StateTreeProfile;
	extern const TCHAR* const EqsProfile;
	extern const TCHAR* const SmartObjectDefinitionProfile;
	extern const TCHAR* const AIStackProfile;

	FString NormalizeAssetPath(const FString& InPath);
	UObject* LoadAssetObject(const FString& InPath);
	UObject* CreateAssetObject(const FString& AssetPathInput, UClass* AssetClass, FString& OutError);
	bool SaveAssetPackage(UObject* Asset, FString& OutError);
	UClass* ResolveClass(const FString& ClassPathInput);
	UScriptStruct* ResolveScriptStruct(const FString& StructPathInput);
	FString ResolveFolderPath(const FString& FolderPathInput, const FString& Profile, const FString& AssetPath);
	TSharedPtr<FJsonObject> VecToJson(const FVector& V);
	TSharedPtr<FJsonObject> RotToJson(const FRotator& R);
	TSharedPtr<FJsonObject> TransformToJson(const FTransform& Transform);
	bool JsonTryGetVector(const TSharedPtr<FJsonObject>& Obj, const FString& Key, FVector& OutValue);
	bool JsonTryGetRotator(const TSharedPtr<FJsonObject>& Obj, const FString& Key, FRotator& OutValue);
	bool TryGetTransform(const TSharedPtr<FJsonObject>& Obj, const FString& Key, FTransform& OutTransform);
	void AddIssue(TArray<TSharedPtr<FJsonValue>>& Issues, const FString& Code, const FString& Message, const FString& JsonPath);
	void AddIssue(TArray<TSharedPtr<FJsonValue>>& Issues, const FString& Code, const FString& Message);
	void AttachCoverageReport(TSharedPtr<FJsonObject>& OutData);
	void SetDiagnostics(TSharedPtr<FJsonObject>& OutData, const TArray<TSharedPtr<FJsonValue>>& Issues);
	bool SaveAuthoringFile(const FString& OutputPathInput, const TSharedPtr<FJsonObject>& Root, FString& OutOutputPath, FString& OutError);
	bool LoadAuthoringFile(const FString& InputPathInput, TSharedPtr<FJsonObject>& OutRoot, FString& OutResolvedPath, FString& OutError);
	bool LoadAuthoringFile(const FString& InputPathInput, TSharedPtr<FJsonObject>& OutRoot, FString& OutResolvedPath, FString& OutError, bool bRehydrateGroupedDetails);
	bool LoadFolderAuthoringFile(const FString& FolderPath, const FString& FileName, TSharedPtr<FJsonObject>& OutRoot, FString& OutError);
	bool LoadFolderAuthoringFile(const FString& FolderPath, const FString& FileName, TSharedPtr<FJsonObject>& OutRoot, FString& OutError, bool bRequired);
	bool LoadFolderAuthoringFile(const FString& FolderPath, const FString& FileName, TSharedPtr<FJsonObject>& OutRoot, FString& OutError, bool bRequired, bool bRehydrateGroupedDetails);
	bool SaveFolderAuthoringFile(const FString& FolderPath, const FString& FileName, const TSharedPtr<FJsonObject>& Root, FString& OutError);
	bool SaveTrackedFolderAuthoringFile(const FString& FolderPath, const FString& RelativeFile, const TSharedPtr<FJsonObject>& Root, TArray<TSharedPtr<FJsonValue>>& WrittenFiles, FString& OutError);
	FString AssetFolderNameFromPath(const FString& AssetPath);
	TArray<TSharedPtr<FJsonValue>> GameplayTagsToJson(const FGameplayTagContainer& Tags);
	FGameplayTagContainer GameplayTagsFromJsonArray(const TArray<TSharedPtr<FJsonValue>>* Values);
	bool ExportFolderOwnerDetails(UObject* Object, const FString& EntryDirectory, const FString& SourceCommand, const FString& OwnerKind, const FString& OwnerId, const FString& DetailsCategory, const TSharedPtr<FJsonObject>& EntryObject, FString& OutError);
	UeAgentDetailsAuthoring::FOwnerInfo MakeApplyOwnerInfo(const FString& OwnerKind, const FString& OwnerId, const FString& SourceCommand);
	UeAgentDetailsAuthoring::FOwnerInfo MakeApplyOwnerInfo(const FString& OwnerKind, const FString& OwnerId, const FString& SourceCommand, UObject* Object);
	TSharedPtr<FJsonObject> MakeCoverageReport();
	TSharedPtr<FJsonObject> MakeValidationChecks(const TArray<TSharedPtr<FJsonValue>>& Issues);
	TSharedPtr<FJsonObject> MakeProfileArrayJson(const FString& Profile, const FString& FieldName, const TArray<TSharedPtr<FJsonValue>>& Items);
	bool ApplyFolderOwnerDetailsIfPresent(UObject* Object, const FString& EntryDirectory, const TSharedPtr<FJsonObject>& EntryObject, const FString& OwnerKind, const FString& OwnerId, const FString& SourceCommand, FString& OutError, int32& InOutAppliedCount);
	bool ImportPropertyValue(UStruct* Struct, void* Container, UObject* Owner, const FString& PropertyPath, const TSharedPtr<FJsonObject>& Descriptor, FString& OutError);
	bool StrictCheckFields(const TSharedPtr<FJsonObject>& Obj, const TSet<FString>& AllowedFields, const FString& Path, TArray<TSharedPtr<FJsonValue>>& Issues);
	bool ReadReflectedBool(UStruct* Struct, const void* Container, const FName PropertyName, bool& OutValue);
	bool ReadReflectedFloat(UStruct* Struct, const void* Container, const FName PropertyName, float& OutValue);
	bool ReadReflectedClassArray(UStruct* Struct, const void* Container, const FName PropertyName, TArray<UClass*>& OutClasses);
	AActor* ResolveActorOrFirst(UWorld* World, const FString& ActorName, UClass* RequiredComponentClass, UActorComponent*& OutComponent);
	UClass* ResolveBTClass(const FString& ClassOrKind, const UClass* RequiredBase);
	TSharedPtr<FJsonObject> MakeBTAssetJson(UBehaviorTree* Tree);
	TSharedPtr<FJsonObject> ExportBlackboard(UBlackboardData* Blackboard, bool bIncludeParentKeys);
	TSharedPtr<FJsonObject> ExportBehaviorTree(UBehaviorTree* Tree, bool bIncludeNodes);
	TSharedPtr<FJsonObject> ExportBehaviorTree(UBehaviorTree* Tree, bool bIncludeNodes, const FString* DetailsEntryDirectory, FString* OutError);
	TSharedPtr<FJsonObject> ExportStateTree(UStateTree* Tree, bool bIncludeStates);
	TSharedPtr<FJsonObject> ExportStateTree(UStateTree* Tree, bool bIncludeStates, const FString* DetailsEntryDirectory, FString* OutError);
	TSharedPtr<FJsonObject> ExportEqs(UEnvQuery* Query, bool bIncludeOptions);
	TSharedPtr<FJsonObject> ExportEqs(UEnvQuery* Query, bool bIncludeOptions, const FString* DetailsEntryDirectory, FString* OutError);
	TSharedPtr<FJsonObject> ExportSmartObjectDefinition(USmartObjectDefinition* Definition, bool bIncludeSlots);
	TSharedPtr<FJsonObject> ExportSmartObjectDefinition(USmartObjectDefinition* Definition, bool bIncludeSlots, const FString* DetailsEntryDirectory, FString* OutError);
	bool ValidateBTFolderJson(const TSharedPtr<FJsonObject>& AssetJson, const TSharedPtr<FJsonObject>& NodesJson, TArray<TSharedPtr<FJsonValue>>& Issues);
	bool ValidateBTFolderJson(const TSharedPtr<FJsonObject>& AssetJson, const TSharedPtr<FJsonObject>& NodesJson, TArray<TSharedPtr<FJsonValue>>& Issues, bool bStrict, bool bValidateBlackboardKeys);
	bool ValidateStateTreeFolderJson(const TSharedPtr<FJsonObject>& AssetJson, const TSharedPtr<FJsonObject>& StatesJson, TArray<TSharedPtr<FJsonValue>>& Issues);
	bool ValidateEqsFolderJson(const TSharedPtr<FJsonObject>& AssetJson, const TSharedPtr<FJsonObject>& QueryJson, TArray<TSharedPtr<FJsonValue>>& Issues);
	bool ValidateSmartObjectDefinitionJson(const TSharedPtr<FJsonObject>& Root, TArray<TSharedPtr<FJsonValue>>& Issues);
	bool SaveBehaviorTreeFolderContents(const FString& FolderPath, UBehaviorTree* Tree, TArray<TSharedPtr<FJsonValue>>& WrittenFiles, FString& OutError);
	bool SaveStateTreeFolderContents(const FString& FolderPath, UStateTree* Tree, TArray<TSharedPtr<FJsonValue>>& WrittenFiles, FString& OutError);
	bool SaveEqsFolderContents(const FString& FolderPath, UEnvQuery* Query, TArray<TSharedPtr<FJsonValue>>& WrittenFiles, FString& OutError);
	bool ResolveGenericAssetEditorWindow(UObject* Asset, bool bOpenIfNeeded, TSharedPtr<SWindow>& OutWindow, FString& OutError);
	bool ScreenshotSlateWindow(const TSharedPtr<SWindow>& Window, TArray<FColor>& OutPixels, FIntPoint& OutSize, FString& OutError);
	bool IsSlateWindowBackbufferCaptureUnsafe(const TSharedPtr<SWindow>& Window, FString& OutUnsafeReason);
	int32 CountNonBlackPixels(const TArray<FColor>& Pixels);
	void AddPixelStats(const TArray<FColor>& Pixels, TSharedPtr<FJsonObject>& OutData);
	UStateTreeEditorData* GetStateTreeEditorData(UStateTree* Tree);
	bool EnsureStateTreeEditorData(UStateTree* Tree, UClass* SchemaClass, FString& OutError);
	bool ConfigureStateTreeEditorNode(FStateTreeEditorNode& Node, UObject* Outer, const TSharedPtr<FJsonObject>& NodeObj, UScriptStruct* NodeBaseStruct, UClass* BlueprintBaseClass, TArray<TSharedPtr<FJsonValue>>& OutPropertyResults, FString& OutError);
	bool CompileStateTree(UStateTree* Tree, TSharedPtr<FJsonObject>& OutReport);
	EStateTreeStateType ParseStateType(const FString& TypeText);
	EEnvQueryRunMode::Type ParseEqsRunMode(const FString& RunModeText);
	bool ResetSmartObjectSlots(USmartObjectDefinition* Definition, FString& OutError);
	FSmartObjectSlotDefinition& AddSmartObjectSlot(USmartObjectDefinition* Definition);
	bool AddSmartObjectBehaviorDefinitions(UObject* Outer, const TArray<TSharedPtr<FJsonValue>>* Behaviors, TArray<TObjectPtr<USmartObjectBehaviorDefinition>>& Target, TArray<TSharedPtr<FJsonValue>>& PropertyResults, FString& OutError);
	bool ApplyDefaultSmartObjectBehaviorDefinitions(USmartObjectDefinition* Definition, const TArray<TSharedPtr<FJsonValue>>* Behaviors, TArray<TSharedPtr<FJsonValue>>& PropertyResults, FString& OutError);
	void RegisterSmartObjectComponentsInWorld(UWorld* World, USmartObjectSubsystem* Subsystem);
	bool ParseSmartObjectQueryBox(UWorld* World, const TSharedPtr<FJsonObject>& Params, FBox& OutBox, AActor*& OutAgent);
	void CollectSmartObjectActorsForQuery(UWorld* World, USmartObjectSubsystem* Subsystem, const FBox& QueryBox, TArray<AActor*>& OutActors, TArray<TSharedPtr<FJsonValue>>& OutDiagnostics);
	int32 CreateSmartObjectRuntimeEntriesForQuery(UWorld* World, USmartObjectSubsystem* Subsystem, const FBox& QueryBox, const FConstStructView UserData);
	TSharedPtr<FJsonObject> SmartObjectRequestResultToJson(const FSmartObjectRequestResult& Result, USmartObjectSubsystem* Subsystem, const FString& ResultId);

	template<typename T>
	T* LoadAsset(const FString& InPath)
	{
		return Cast<T>(LoadAssetObject(InPath));
	}

	template<typename T>
	bool CreateAsset(const FString& AssetPathInput, T*& OutAsset, FString& OutError)
	{
		OutAsset = Cast<T>(CreateAssetObject(AssetPathInput, T::StaticClass(), OutError));
		return OutAsset != nullptr;
	}

	template<typename EnumType>
	EnumType EnumFromString(const FString& Text, EnumType DefaultValue)
	{
		if (Text.IsEmpty())
		{
			return DefaultValue;
		}
		if (const UEnum* Enum = StaticEnum<EnumType>())
		{
			const int64 Value = Enum->GetValueByNameString(Text);
			if (Value != INDEX_NONE)
			{
				return static_cast<EnumType>(Value);
			}
			const int64 ShortValue = Enum->GetValueByNameString(Enum->GenerateFullEnumName(*Text));
			if (ShortValue != INDEX_NONE)
			{
				return static_cast<EnumType>(ShortValue);
			}
		}
		return DefaultValue;
	}

	template<typename EnumType>
	FString EnumToString(EnumType Value)
	{
		if (const UEnum* Enum = StaticEnum<EnumType>())
		{
			return Enum->GetNameStringByValue(static_cast<int64>(Value));
		}
		return FString::FromInt(static_cast<int32>(Value));
	}
}

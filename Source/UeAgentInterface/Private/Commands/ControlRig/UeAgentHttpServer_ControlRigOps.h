// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "ControlRigGizmoLibrary.h"

class FCompilerResultsLog;
class FJsonObject;
class FJsonValue;
class UObject;
class UControlRigBlueprint;
class UControlRigShapeLibrary;
enum class EControlRigType : uint8;
enum class ERigElementType : uint8;

namespace UeAgentControlRigOps
{
	struct FControlRigVariableApplyStats
	{
		FControlRigVariableApplyStats();

		int32 Added;
		int32 DefaultsUpdated;
		int32 FlagsUpdated;
		int32 Existing;
	};

	FString NormalizeAssetPath(const FString& InPath);
	UObject* LoadAssetObject(const FString& InPath);
	UControlRigBlueprint* LoadControlRigBlueprint(const FString& InPath);
	UControlRigShapeLibrary* LoadShapeLibrary(const FString& InPath);
	bool SaveAssetPackage(UObject* Asset, FString& OutError);
	bool HasBlockingIssue(const TArray<TSharedPtr<FJsonValue>>& Issues);
	int32 CountIssuesBySeverity(const TArray<TSharedPtr<FJsonValue>>& Issues, const FString& SeverityToMatch);
	bool SaveJsonFile(const FString& FilePath, const TSharedPtr<FJsonObject>& Obj, FString& OutError);
	bool LoadJsonFile(const FString& FilePath, TSharedPtr<FJsonObject>& OutObj, TArray<TSharedPtr<FJsonValue>>& Issues, FString& OutError, const bool bOptional);
	FString DefaultFolderForAsset(const FString& AssetPath);
	bool ReadJsonValueAsDefaultText(const TSharedPtr<FJsonValue>& Value, FString& OutText);
	TSharedPtr<FJsonObject> MakeTransformJson(const FTransform& Value);
	FString ElementTypeToString(ERigElementType Type);
	TSharedPtr<FJsonObject> MakeCompileReportJson(UControlRigBlueprint* Blueprint);
	TSharedPtr<FJsonObject> MakeCompileReportJson(UControlRigBlueprint* Blueprint, const FCompilerResultsLog* CompilerLog);
	UControlRigBlueprint* CreateControlRigAsset(const FString& AssetPath, bool bModularRig, const FString& PreviewSkeletalMeshPath, bool bImportHierarchyFromPreview, FString& OutError);
	bool ParseControlRigTypeText(const FString& InText, EControlRigType& OutType);
	TSharedPtr<FJsonObject> BuildVariablesJson(UControlRigBlueprint* Blueprint);
	void ApplyVariablesJson(UControlRigBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Obj, const FString& Path, TArray<TSharedPtr<FJsonValue>>& Issues, FControlRigVariableApplyStats& Stats);
	void MergeStructuredIkNodesJson(TSharedPtr<FJsonObject> GraphsJson, const TSharedPtr<FJsonObject>& IkNodesJson, const FString& FilePath, TArray<TSharedPtr<FJsonValue>>& Issues);
	TSharedPtr<FJsonObject> BuildControlRigInfo(UControlRigBlueprint* Blueprint);
	bool ApplyPreviewJson(UControlRigBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Obj, const FString& Path, TArray<TSharedPtr<FJsonValue>>& Issues);
	bool ApplyShapeLibrariesJson(UControlRigBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Obj, const FString& Path, TArray<TSharedPtr<FJsonValue>>& Issues);
	void ApplyBonesJson(UControlRigBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Obj, const FString& Path, TArray<TSharedPtr<FJsonValue>>& Issues);
	void ApplyNullsJson(UControlRigBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Obj, const FString& Path, TArray<TSharedPtr<FJsonValue>>& Issues);
	void ApplyControlsJson(UControlRigBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Obj, const FString& Path, TArray<TSharedPtr<FJsonValue>>& Issues);
	void ApplyCurvesJson(UControlRigBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Obj, const FString& Path, TArray<TSharedPtr<FJsonValue>>& Issues);
	bool ApplyGraphsJson(UControlRigBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Obj, const FString& Path, TArray<TSharedPtr<FJsonValue>>& Issues);
	void MarkControlRigModified(UControlRigBlueprint* Blueprint);
	TArray<TSharedPtr<FJsonValue>> MakeStringArray(std::initializer_list<const TCHAR*> Items);
	TSharedPtr<FJsonObject> MakeEmptyItemsJson(const FString& Schema);
	TSharedPtr<FJsonObject> MakeEmptyItemsJson(const FString& Schema, const FString& FieldName);
	TSharedPtr<FJsonObject> MakeUnsupportedProfilesJson();
	TSharedPtr<FJsonObject> MakeRuntimeProbeReportJson();
	TSharedPtr<FJsonObject> MakeRuntimeProbeReportJson(const FString& Mode);
	TSharedPtr<FJsonObject> MakeRigVMUnitRegistryJson();
	void AddUnsupportedApplyIssueIfNeeded(const TSharedPtr<FJsonObject>& Obj, const FString& FilePath, const FString& Profile, TArray<TSharedPtr<FJsonValue>>& Issues);
	TSharedPtr<FJsonObject> MakeCoverageJson();
	bool ApplyShapeDefinitionJson(const TSharedPtr<FJsonObject>& Obj, const FString& Path, FControlRigShapeDefinition& OutShape, TArray<TSharedPtr<FJsonValue>>& Issues);
	TSharedPtr<FJsonObject> BuildShapeLibraryInfo(UControlRigShapeLibrary* Library);
	UControlRigShapeLibrary* CreateShapeLibraryAsset(const FString& AssetPath, FString& OutError);
}

// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Core/Contract/UeAgentDiagnostic.h"

#include "CoreMinimal.h"

#include "ControlRigGizmoLibrary.h"

class FCompilerResultsLog;
class FJsonObject;
class FJsonValue;
class UObject;
class UBlueprint;
class UControlRigBlueprint;
class UControlRigShapeLibrary;
class URigVMNode;
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
	bool SaveAssetPackage(UObject* Asset, UE::UAI::Contract::FDiagnostic& OutError);
	bool HasBlockingIssue(const TArray<UE::UAI::Contract::FDiagnostic>& Issues);
	int32 CountIssuesBySeverity(const TArray<UE::UAI::Contract::FDiagnostic>& Issues, const FString& SeverityToMatch);
	bool SaveAuthoringFile(const FString& FilePath, const TSharedPtr<FJsonObject>& Obj, UE::UAI::Contract::FDiagnostic& OutError);
	bool LoadAuthoringFile(const FString& FilePath, TSharedPtr<FJsonObject>& OutObj, TArray<UE::UAI::Contract::FDiagnostic>& Issues, UE::UAI::Contract::FDiagnostic& OutError, const bool bOptional);
	FString DefaultFolderForAsset(const FString& AssetPath);
	bool ReadJsonValueAsDefaultText(const TSharedPtr<FJsonValue>& Value, FString& OutText);
	TSharedPtr<FJsonObject> MakeTransformJson(const FTransform& Value);
	FString ElementTypeToString(ERigElementType Type);
	TSharedPtr<FJsonObject> MakeCompileReportJson(UControlRigBlueprint* Blueprint);
	TSharedPtr<FJsonObject> MakeCompileReportJson(UControlRigBlueprint* Blueprint, const FCompilerResultsLog* CompilerLog);
	UControlRigBlueprint* CreateControlRigAsset(const FString& AssetPath, bool bModularRig, const FString& PreviewSkeletalMeshPath, bool bImportHierarchyFromPreview, UE::UAI::Contract::FDiagnostic& OutError);
	bool ParseControlRigTypeText(const FString& InText, EControlRigType& OutType);
	TSharedPtr<FJsonObject> BuildVariablesJson(UControlRigBlueprint* Blueprint);
	bool CollectVariableTypeChangeDependents(UControlRigBlueprint& Blueprint,
		const FJsonObject& Variables, TArray<UBlueprint*>& OutBlueprints, UE::UAI::Contract::FDiagnostic& OutError);
	void ApplyVariablesJson(UControlRigBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Obj, const FString& Path, TArray<UE::UAI::Contract::FDiagnostic>& Issues, FControlRigVariableApplyStats& Stats,
		const FString& SourcePointerPrefix = FString());
	void MergeStructuredIkNodesJson(TSharedPtr<FJsonObject> GraphsJson, const TSharedPtr<FJsonObject>& IkNodesJson, const FString& FilePath, TArray<UE::UAI::Contract::FDiagnostic>& Issues);
	TSharedPtr<FJsonObject> BuildControlRigInfo(UControlRigBlueprint* Blueprint);
	bool ApplyPreviewJson(UControlRigBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Obj, const FString& Path, TArray<UE::UAI::Contract::FDiagnostic>& Issues);
	bool ApplyShapeLibrariesJson(UControlRigBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Obj, const FString& Path, TArray<UE::UAI::Contract::FDiagnostic>& Issues);
	void ApplyBonesJson(UControlRigBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Obj, const FString& Path, TArray<UE::UAI::Contract::FDiagnostic>& Issues);
	void ApplyNullsJson(UControlRigBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Obj, const FString& Path, TArray<UE::UAI::Contract::FDiagnostic>& Issues);
	void ApplyControlsJson(UControlRigBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Obj, const FString& Path, TArray<UE::UAI::Contract::FDiagnostic>& Issues);
	void ApplyCurvesJson(UControlRigBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Obj, const FString& Path, TArray<UE::UAI::Contract::FDiagnostic>& Issues);
	bool ApplyGraphsJson(UControlRigBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Obj, TArray<UE::UAI::Contract::FDiagnostic>& Issues,
		const TSet<const URigVMNode*>* AddedNodes = nullptr);
	void MarkControlRigModified(UControlRigBlueprint* Blueprint);
	TArray<TSharedPtr<FJsonValue>> MakeStringArray(std::initializer_list<const TCHAR*> Items);
	TSharedPtr<FJsonObject> MakeEmptyItemsJson(const FString& Schema);
	TSharedPtr<FJsonObject> MakeEmptyItemsJson(const FString& Schema, const FString& FieldName);
	TSharedPtr<FJsonObject> MakeUnsupportedProfilesJson();
	TSharedPtr<FJsonObject> MakeRuntimeProbeReportJson();
	TSharedPtr<FJsonObject> MakeRuntimeProbeReportJson(const FString& Mode);
	TSharedPtr<FJsonObject> MakeRigVMUnitRegistryJson();
	bool ApplyShapeDefinitionJson(const TSharedPtr<FJsonObject>& Obj, const FString& Path, FControlRigShapeDefinition& OutShape, TArray<UE::UAI::Contract::FDiagnostic>& Issues);
	TSharedPtr<FJsonObject> BuildShapeLibraryInfo(UControlRigShapeLibrary* Library);
	UControlRigShapeLibrary* CreateShapeLibraryAsset(const FString& AssetPath, UE::UAI::Contract::FDiagnostic& OutError);
}

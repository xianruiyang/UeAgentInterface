// Copyright Epic Games, Inc. All Rights Reserved.

#include "UeAgentHttpServer_Material.h"
#include "Commands/Assets/UeAgentReflectionAuthoringCommandBridge.h"
#include "UeAgentHttpServer_MaterialOps.h"
#include "Domains/Material/Authoring/UeAgentMaterialAuthoringApplyProvider.h"
#include "Domains/Material/Authoring/UeAgentMaterialAuthoringExport.h"
#include "Utilities/UeAgentAuthoringFileIO.h"
#include "Utilities/UeAgentAuthoringFolderIO.h"
#include "Utilities/UeAgentAuthoringFolderStats.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringWorkspacePaths.h"
#include "Utilities/UeAgentDetailsAuthoring.h"
#include "Utilities/UeAgentJsonDiagnostics.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "MaterialEditingLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialInstanceConstant.h"
#include "MaterialShared.h"
#include "Misc/Paths.h"
namespace UeAgentMaterialFolderOps
{
	static FString GetFolderRootAbsolute()
	{
		return FPaths::ConvertRelativePathToFull(
			FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UeAssetFolders"), TEXT("MaterialGraph")));
	}

	static bool ResolveFolderForAsset(
		const FString& InAssetPath,
		FString& OutFolderPath,
		FString& OutError)
	{
		return UeAgentAuthoringWorkspacePaths::ResolveAssetWorkspaceFolder(
			GetFolderRootAbsolute(),
			UeAgentMaterialOps::NormalizeAssetPath(InAssetPath),
			OutFolderPath,
			OutError);
	}

	static UeAgentDetailsAuthoring::FOwnerInfo MakeApplyOwnerInfo(
		const FString& OwnerKind,
		const FString& OwnerId,
		const FString& SourceCommand,
		UObject* Object = nullptr)
	{
		return UeAgentDetailsAuthoring::MakeOwnerInfoForObject(
			Object,
			OwnerKind,
			OwnerId,
			SourceCommand);
	}

	static void SetUnifiedApplyFailure(
		const UeAgentMaterialAuthoring::FAssetApplyResult& Result,
		TSharedPtr<FJsonObject>& OutData,
		FString& OutError)
	{
		if (!OutData.IsValid()) OutData = MakeShared<FJsonObject>();
		OutData->SetStringField(TEXT("status"), Result.Status);
		OutData->SetStringField(TEXT("phase"), Result.Phase);
		OutData->SetStringField(TEXT("error_code"), Result.ErrorCode);
		OutData->SetStringField(TEXT("error_detail"), Result.ErrorDetail);
		OutData->SetBoolField(TEXT("rollback_attempted"), Result.bRollbackAttempted);
		OutData->SetBoolField(TEXT("rollback_succeeded"), Result.bRollbackSucceeded);
		OutError = UeAgentAuthoringDiagnostics::GetActionableFailureMessage(
			Result.ErrorCode,
			Result.ErrorDetail,
			TEXT("material_unified_apply_failed"));
	}

	static bool TryGetString(
		const TSharedPtr<FJsonObject>& Object,
		const TCHAR* Key,
		FString& OutValue)
	{
		return Object.IsValid() && Object->TryGetStringField(Key, OutValue);
	}

	static bool TryGetBool(
		const TSharedPtr<FJsonObject>& Object,
		const TCHAR* Key,
		bool& OutValue)
	{
		return Object.IsValid() && Object->TryGetBoolField(Key, OutValue);
	}

	static bool TryGetNumber(
		const TSharedPtr<FJsonObject>& Object,
		const TCHAR* Key,
		double& OutValue)
	{
		return Object.IsValid() && Object->TryGetNumberField(Key, OutValue);
	}

	static bool TryGetObjectField(
		const TSharedPtr<FJsonObject>& Object,
		const TCHAR* Key,
		TSharedPtr<FJsonObject>& OutValue)
	{
		const TSharedPtr<FJsonObject>* FoundObject = nullptr;
		if (!Object.IsValid() || !Object->TryGetObjectField(Key, FoundObject)
			|| !FoundObject || !FoundObject->IsValid())
		{
			return false;
		}
		OutValue = *FoundObject;
		return true;
	}

	static bool CompileDataHasError(const TSharedPtr<FJsonObject>& CompileData)
	{
		if (!CompileData.IsValid()) return false;
		bool bHasError = false;
		if (CompileData->TryGetBoolField(TEXT("has_error"), bHasError) && bHasError) return true;
		double ErrorCount = 0.0;
		return CompileData->TryGetNumberField(TEXT("error_count"), ErrorCount) && ErrorCount > 0.0;
	}
}

bool FUeAgentLegacyMechanics::CmdMaterialExportFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	const FString AssetPath = UeAgentMaterialOps::NormalizeAssetPath(AssetPathInput);
	UMaterial* Material = UeAgentMaterialOps::LoadMaterialAsset(AssetPath);
	if (!Material)
	{
		OutError = TEXT("material_not_found");
		return false;
	}

	FString FolderPath;
	if ((!JsonTryGetString(Ctx.Params, TEXT("output_folder"), FolderPath)
			|| FolderPath.TrimStartAndEnd().IsEmpty())
		&& !UeAgentMaterialFolderOps::ResolveFolderForAsset(AssetPath, FolderPath, OutError))
	{
		return false;
	}

	FString Profile = TEXT("llm_authoring");
	JsonTryGetString(Ctx.Params, TEXT("profile"), Profile);
	UeAgentExportWorkspaceWriter::FWorkspaceResult Result;
	if (!UeAgentMaterialAuthoring::ExportAssetWorkspace(
		*Material, FolderPath, Profile, Result, OutError))
	{
		return false;
	}
	OutData = Result.ToJsonObject();
	OutData->SetStringField(TEXT("asset_path"), AssetPath);
	OutData->SetStringField(TEXT("asset_kind"), TEXT("material"));
	OutData->SetStringField(TEXT("folder_path"), FolderPath);
	OutData->SetNumberField(TEXT("file_count"), Result.WrittenFiles.Num());
	return true;
}

bool FUeAgentLegacyMechanics::CmdMaterialApplyFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	if (!OutData.IsValid()) OutData = MakeShared<FJsonObject>();

	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}
	const FString AssetPath = UeAgentMaterialOps::NormalizeAssetPath(AssetPathInput);

	FString FolderPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("folder_path"), FolderPath) || FolderPath.IsEmpty())
	{
		if (!UeAgentMaterialFolderOps::ResolveFolderForAsset(AssetPath, FolderPath, OutError)) return false;
	}

	UeAgentMaterialAuthoring::FAssetApplyResult Result;
	if (!UeAgentMaterialAuthoring::ApplyAssetWorkspace(
		FolderPath, TEXT("material"), AssetPath, Result, Ctx.RequestId))
	{
		UeAgentMaterialFolderOps::SetUnifiedApplyFailure(Result, OutData, OutError);
		return false;
	}

	OutData->SetStringField(TEXT("status"), Result.Status);
	OutData->SetStringField(TEXT("phase"), Result.Phase);
	OutData->SetStringField(TEXT("asset_path"), Result.AssetPath);
	OutData->SetStringField(TEXT("asset_kind"), Result.AssetKind);
	OutData->SetStringField(TEXT("object_path"), Result.ObjectPath);
	OutData->SetStringField(TEXT("folder_path"), FolderPath);
	OutData->SetStringField(TEXT("resolved_operation"), Result.ResolvedOperation);
	OutData->SetNumberField(TEXT("expressions_created"), Result.ExpressionsCreated);
	OutData->SetNumberField(TEXT("edges_created"), Result.EdgesCreated);
	OutData->SetNumberField(TEXT("root_inputs_connected"), Result.RootInputsConnected);
	OutData->SetNumberField(TEXT("units_checked"), Result.UnitsChecked);
	OutData->SetNumberField(TEXT("mutations_applied"), Result.MutationsApplied);
	OutData->SetBoolField(TEXT("rollback_attempted"), Result.bRollbackAttempted);
	OutData->SetBoolField(TEXT("rollback_succeeded"), Result.bRollbackSucceeded);
	return true;
}

namespace UeAgentMaterialInstanceFolderOps
{
	static FString GetFolderRootAbsolute()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UeAssetFolders"), TEXT("MaterialInstance")));
	}

	static bool ResolveFolderForAsset(const FString& InAssetPath, FString& OutFolderPath, FString& OutError)
	{
		return UeAgentAuthoringWorkspacePaths::ResolveAssetWorkspaceFolder(
			GetFolderRootAbsolute(),
			UeAgentMaterialOps::NormalizeAssetPath(InAssetPath),
			OutFolderPath,
			OutError);
	}

	static TSharedPtr<FJsonObject> ExportAssetJson(UMaterialInstanceConstant* MaterialInstance)
	{
		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		const FString AssetPath = MaterialInstance ? MaterialInstance->GetOutermost()->GetName() : FString();
		Root->SetNumberField(TEXT("format_version"), 1);
		Root->SetStringField(TEXT("asset_kind"), TEXT("material_instance"));
		Root->SetStringField(TEXT("asset_path"), AssetPath);
		Root->SetNumberField(TEXT("profile_version"), 1);
		Root->SetStringField(TEXT("instance_subkind"), TEXT("material_instance_constant"));
		return Root;
	}

	static TSharedPtr<FJsonObject> ExportParentJson(UMaterialInstanceConstant* MaterialInstance)
	{
		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(
			TEXT("parent_material_path"),
			(MaterialInstance && MaterialInstance->Parent) ? MaterialInstance->Parent->GetPathName() : FString());
		return Root;
	}

	static TSharedPtr<FJsonObject> MakeScalarParameterObject(const FScalarParameterValue& Parameter)
	{
		TSharedPtr<FJsonObject> ParamObj = MakeShared<FJsonObject>();
		ParamObj->SetStringField(TEXT("name"), Parameter.ParameterInfo.Name.ToString());
		ParamObj->SetStringField(TEXT("parameter_type"), TEXT("scalar"));
		ParamObj->SetNumberField(TEXT("value"), Parameter.ParameterValue);
		return ParamObj;
	}

	static TSharedPtr<FJsonObject> MakeVectorParameterObject(const FVectorParameterValue& Parameter)
	{
		TSharedPtr<FJsonObject> ParamObj = MakeShared<FJsonObject>();
		ParamObj->SetStringField(TEXT("name"), Parameter.ParameterInfo.Name.ToString());
		ParamObj->SetStringField(TEXT("parameter_type"), TEXT("vector"));
		TSharedPtr<FJsonObject> ValueObj = MakeShared<FJsonObject>();
		ValueObj->SetNumberField(TEXT("r"), Parameter.ParameterValue.R);
		ValueObj->SetNumberField(TEXT("g"), Parameter.ParameterValue.G);
		ValueObj->SetNumberField(TEXT("b"), Parameter.ParameterValue.B);
		ValueObj->SetNumberField(TEXT("a"), Parameter.ParameterValue.A);
		ParamObj->SetObjectField(TEXT("value"), ValueObj);
		return ParamObj;
	}

	static TSharedPtr<FJsonObject> MakeTextureParameterObject(const FTextureParameterValue& Parameter)
	{
		TSharedPtr<FJsonObject> ParamObj = MakeShared<FJsonObject>();
		ParamObj->SetStringField(TEXT("name"), Parameter.ParameterInfo.Name.ToString());
		ParamObj->SetStringField(TEXT("parameter_type"), TEXT("texture"));
		ParamObj->SetStringField(TEXT("texture_path"), Parameter.ParameterValue ? Parameter.ParameterValue->GetPathName() : FString());
		return ParamObj;
	}

	static TSharedPtr<FJsonObject> MakeStaticSwitchParameterObject(const FStaticSwitchParameter& Parameter)
	{
		TSharedPtr<FJsonObject> ParamObj = MakeShared<FJsonObject>();
		ParamObj->SetStringField(TEXT("name"), Parameter.ParameterInfo.Name.ToString());
		ParamObj->SetStringField(TEXT("parameter_type"), TEXT("static_switch"));
		ParamObj->SetBoolField(TEXT("value"), Parameter.Value);
		return ParamObj;
	}

	static TSharedPtr<FJsonObject> MakeStaticComponentMaskParameterObject(const FStaticComponentMaskParameter& Parameter)
	{
		TSharedPtr<FJsonObject> ParamObj = MakeShared<FJsonObject>();
		ParamObj->SetStringField(TEXT("name"), Parameter.ParameterInfo.Name.ToString());
		ParamObj->SetStringField(TEXT("parameter_type"), TEXT("static_component_mask"));
		TSharedPtr<FJsonObject> ValueObj = MakeShared<FJsonObject>();
		ValueObj->SetBoolField(TEXT("r"), Parameter.R);
		ValueObj->SetBoolField(TEXT("g"), Parameter.G);
		ValueObj->SetBoolField(TEXT("b"), Parameter.B);
		ValueObj->SetBoolField(TEXT("a"), Parameter.A);
		ParamObj->SetObjectField(TEXT("value"), ValueObj);
		return ParamObj;
	}

	static TSharedPtr<FJsonObject> ExportOverridesJson(UMaterialInstanceConstant* MaterialInstance)
	{
		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> ParametersArray;
		if (!MaterialInstance)
		{
			Root->SetArrayField(TEXT("parameters"), ParametersArray);
			return Root;
		}

		for (const FScalarParameterValue& Parameter : MaterialInstance->ScalarParameterValues)
		{
			ParametersArray.Add(MakeShared<FJsonValueObject>(MakeScalarParameterObject(Parameter)));
		}

		for (const FVectorParameterValue& Parameter : MaterialInstance->VectorParameterValues)
		{
			ParametersArray.Add(MakeShared<FJsonValueObject>(MakeVectorParameterObject(Parameter)));
		}

		for (const FTextureParameterValue& Parameter : MaterialInstance->TextureParameterValues)
		{
			ParametersArray.Add(MakeShared<FJsonValueObject>(MakeTextureParameterObject(Parameter)));
		}

		const FStaticParameterSet StaticParameters = MaterialInstance->GetStaticParameters();
		for (const FStaticSwitchParameter& Parameter : StaticParameters.StaticSwitchParameters)
		{
			if (!Parameter.bOverride)
			{
				continue;
			}
			ParametersArray.Add(MakeShared<FJsonValueObject>(MakeStaticSwitchParameterObject(Parameter)));
		}

		for (const FStaticComponentMaskParameter& Parameter : StaticParameters.EditorOnly.StaticComponentMaskParameters)
		{
			if (!Parameter.bOverride)
			{
				continue;
			}
			ParametersArray.Add(MakeShared<FJsonValueObject>(MakeStaticComponentMaskParameterObject(Parameter)));
		}

		Root->SetArrayField(TEXT("parameters"), ParametersArray);
		return Root;
	}

	static TSharedPtr<FJsonObject> ExportValidationJson()
	{
		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> Checks;

		TSharedPtr<FJsonObject> CompileCheck = MakeShared<FJsonObject>();
		CompileCheck->SetStringField(TEXT("kind"), TEXT("compile_success"));
		Checks.Add(MakeShared<FJsonValueObject>(CompileCheck));

		TSharedPtr<FJsonObject> ParentCheck = MakeShared<FJsonObject>();
		ParentCheck->SetStringField(TEXT("kind"), TEXT("parent_exists"));
		Checks.Add(MakeShared<FJsonValueObject>(ParentCheck));

		Root->SetArrayField(TEXT("checks"), Checks);
		return Root;
	}
}

bool FUeAgentLegacyMechanics::CmdMaterialInstanceExportFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	return UeAgentReflectionAuthoringCommandBridge::Export(TEXT("material_instance"), Ctx.Params, OutData, OutError);
}

bool FUeAgentLegacyMechanics::CmdMaterialInstanceApplyFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	return UeAgentReflectionAuthoringCommandBridge::Apply(TEXT("material_instance"), Ctx.Params, OutData, OutError, Ctx.RequestId);
}

namespace UeAgentMaterialFunctionFolderOps
{
	static FString GetFolderRootAbsolute()
	{
		return FPaths::ConvertRelativePathToFull(
			FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UeAssetFolders"), TEXT("MaterialFunction")));
	}

	static bool ResolveFolderForAsset(
		const FString& InAssetPath,
		FString& OutFolderPath,
		FString& OutError)
	{
		return UeAgentAuthoringWorkspacePaths::ResolveAssetWorkspaceFolder(
			GetFolderRootAbsolute(),
			UeAgentMaterialOps::NormalizeAssetPath(InAssetPath),
			OutFolderPath,
			OutError);
	}
}

bool FUeAgentLegacyMechanics::CmdMaterialFunctionExportFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	UMaterialFunction* MaterialFunction = UeAgentMaterialOps::LoadMaterialFunctionAsset(AssetPathInput);
	if (!MaterialFunction)
	{
		OutError = TEXT("material_function_not_found");
		return false;
	}

	const FString AssetPath = MaterialFunction->GetOutermost()->GetName();
	FString FolderPath;
	if ((!JsonTryGetString(Ctx.Params, TEXT("output_folder"), FolderPath)
			|| FolderPath.TrimStartAndEnd().IsEmpty())
		&& !UeAgentMaterialFunctionFolderOps::ResolveFolderForAsset(AssetPath, FolderPath, OutError))
	{
		return false;
	}

	FString Profile = TEXT("llm_authoring");
	JsonTryGetString(Ctx.Params, TEXT("profile"), Profile);
	UeAgentExportWorkspaceWriter::FWorkspaceResult Result;
	if (!UeAgentMaterialAuthoring::ExportAssetWorkspace(
		*MaterialFunction, FolderPath, Profile, Result, OutError))
	{
		return false;
	}
	OutData = Result.ToJsonObject();
	OutData->SetStringField(TEXT("asset_path"), AssetPath);
	OutData->SetStringField(TEXT("asset_kind"), TEXT("material_function"));
	OutData->SetStringField(TEXT("folder_path"), FolderPath);
	OutData->SetNumberField(TEXT("file_count"), Result.WrittenFiles.Num());
	return true;
}

bool FUeAgentLegacyMechanics::CmdMaterialFunctionApplyFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	if (!OutData.IsValid()) OutData = MakeShared<FJsonObject>();

	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}
	const FString AssetPath = UeAgentMaterialOps::NormalizeAssetPath(AssetPathInput);

	FString FolderPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("folder_path"), FolderPath) || FolderPath.IsEmpty())
	{
		if (!UeAgentMaterialFunctionFolderOps::ResolveFolderForAsset(AssetPath, FolderPath, OutError)) return false;
	}

	UeAgentMaterialAuthoring::FAssetApplyResult Result;
	if (!UeAgentMaterialAuthoring::ApplyAssetWorkspace(
		FolderPath, TEXT("material_function"), AssetPath, Result, Ctx.RequestId))
	{
		UeAgentMaterialFolderOps::SetUnifiedApplyFailure(Result, OutData, OutError);
		return false;
	}

	OutData->SetStringField(TEXT("status"), Result.Status);
	OutData->SetStringField(TEXT("phase"), Result.Phase);
	OutData->SetStringField(TEXT("asset_path"), Result.AssetPath);
	OutData->SetStringField(TEXT("asset_kind"), Result.AssetKind);
	OutData->SetStringField(TEXT("object_path"), Result.ObjectPath);
	OutData->SetStringField(TEXT("folder_path"), FolderPath);
	OutData->SetStringField(TEXT("resolved_operation"), Result.ResolvedOperation);
	OutData->SetNumberField(TEXT("expressions_created"), Result.ExpressionsCreated);
	OutData->SetNumberField(TEXT("edges_created"), Result.EdgesCreated);
	OutData->SetNumberField(TEXT("units_checked"), Result.UnitsChecked);
	OutData->SetNumberField(TEXT("mutations_applied"), Result.MutationsApplied);
	OutData->SetBoolField(TEXT("rollback_attempted"), Result.bRollbackAttempted);
	OutData->SetBoolField(TEXT("rollback_succeeded"), Result.bRollbackSucceeded);
	return true;
}

// Copyright Epic Games, Inc. All Rights Reserved.

#include "UeAgentHttpServer_ControlRig.h"
#include "Domains/Rigging/UeAgentControlRigDomainBackend.h"
#include "Utilities/UeAgentJsonDiagnostics.h"
#include "Utilities/UeAgentStructuredAuthoringIO.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Materials/Material.h"
#include "Misc/Paths.h"

namespace
{
	UMaterial* LoadShapeLibraryMaterial(const FString& AssetPath)
	{
		return Cast<UMaterial>(UeAgentControlRigOps::LoadAssetObject(AssetPath));
	}
}

bool FUeAgentLegacyMechanics::CmdControlRigShapeLibraryCreate(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("asset_path_required");
		return false;
	}

	UControlRigShapeLibrary* Library = UeAgentControlRigOps::CreateShapeLibraryAsset(AssetPath, OutError);
	if (!Library)
	{
		return false;
	}

	bool bSaveAfterCreate = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_create"), bSaveAfterCreate);
	if (bSaveAfterCreate && !UeAgentControlRigOps::SaveAssetPackage(Library, OutError))
	{
		return false;
	}

	OutData = UeAgentControlRigOps::BuildShapeLibraryInfo(Library);
	OutData->SetBoolField(TEXT("created"), true);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterCreate);
	return true;
}

bool FUeAgentLegacyMechanics::CmdControlRigShapeLibraryGetInfo(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath) || AssetPath.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("asset_path_required");
		return false;
	}

	UControlRigShapeLibrary* Library = UeAgentControlRigOps::LoadShapeLibrary(AssetPath);
	if (!Library)
	{
		OutError = TEXT("control_rig_shape_library_not_found");
		return false;
	}

	OutData = UeAgentControlRigOps::BuildShapeLibraryInfo(Library);
	return true;
}

bool FUeAgentLegacyMechanics::CmdControlRigShapeLibraryExportJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	if (!CmdControlRigShapeLibraryGetInfo(Ctx, OutData, OutError))
	{
		return false;
	}

	FString OutputFile;
	JsonTryGetString(Ctx.Params, TEXT("output_file"), OutputFile);
	if (OutputFile.TrimStartAndEnd().IsEmpty())
	{
		FString AssetPath;
		JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath);
		FString SafeName = UeAgentControlRigOps::NormalizeAssetPath(AssetPath);
		SafeName.RemoveFromStart(TEXT("/"));
		SafeName.ReplaceInline(TEXT("/"), TEXT("__"));
		OutputFile = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("UeAssetFolders"), TEXT("ControlRigShapeLibrary"), SafeName + TEXT(".hjson"));
	}
	OutputFile = UeAgentStructuredAuthoringIO::NormalizeAuthoringPath(OutputFile);

	if (!UeAgentControlRigOps::SaveAuthoringFile(OutputFile, OutData, OutError))
	{
		return false;
	}
	OutData->SetStringField(TEXT("json_file"), OutputFile);
	return true;
}

bool FUeAgentLegacyMechanics::CmdControlRigShapeLibraryValidateJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString JsonFile;
	if (!JsonTryGetString(Ctx.Params, TEXT("json_file"), JsonFile) || JsonFile.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("json_file_required");
		return false;
	}

	TArray<TSharedPtr<FJsonValue>> Issues;
	TSharedPtr<FJsonObject> Root;
	UeAgentControlRigOps::LoadAuthoringFile(JsonFile, Root, Issues, OutError, false);
	if (Root.IsValid())
	{
		UeAgentJsonDiagnostics::WarnUnknownFields(Root, TEXT("shape_library"), { TEXT("schema"), TEXT("asset_path"), TEXT("object_path"), TEXT("asset_class"), TEXT("default_shape"), TEXT("default_material"), TEXT("xray_material"), TEXT("material_color_parameter"), TEXT("shapes"), TEXT("shape_count") }, Issues);
		FString AssetPath;
		UeAgentJsonDiagnostics::ReadStringField(Root, TEXT("asset_path"), TEXT("shape_library.asset_path"), AssetPath, Issues, true);
		const TArray<TSharedPtr<FJsonValue>>* Shapes = nullptr;
		if (UeAgentJsonDiagnostics::ReadArrayField(Root, TEXT("shapes"), TEXT("shape_library.shapes"), Shapes, Issues, false) && Shapes)
		{
			TSet<FString> ShapeNames;
			for (int32 Index = 0; Index < Shapes->Num(); ++Index)
			{
				TSharedPtr<FJsonObject> ShapeObj;
				const FString ShapePath = FString::Printf(TEXT("shape_library.shapes[%d]"), Index);
				if (UeAgentJsonDiagnostics::ReadObjectFromValue((*Shapes)[Index], ShapePath, ShapeObj, Issues))
				{
					FControlRigShapeDefinition Dummy;
					UeAgentControlRigOps::ApplyShapeDefinitionJson(ShapeObj, ShapePath, Dummy, Issues);
					const FString ShapeName = Dummy.ShapeName.ToString();
					if (ShapeNames.Contains(ShapeName))
					{
						UeAgentJsonDiagnostics::AddIssue(Issues, TEXT("warning"), TEXT("duplicate_shape_name"), ShapePath + TEXT(".shape_name"), FString::Printf(TEXT("Duplicate shape name '%s'."), *ShapeName));
					}
					ShapeNames.Add(ShapeName);
				}
			}
		}
	}

	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("json_file"), UeAgentJsonDiagnostics::ResolveProjectRelativeFilePath(JsonFile));
	OutData->SetArrayField(TEXT("issues"), Issues);
	OutData->SetBoolField(TEXT("valid"), !UeAgentControlRigOps::HasBlockingIssue(Issues));
	return true;
}

bool FUeAgentLegacyMechanics::CmdControlRigShapeLibraryApplyJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString JsonFile;
	if (!JsonTryGetString(Ctx.Params, TEXT("json_file"), JsonFile) || JsonFile.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("json_file_required");
		return false;
	}

	TArray<TSharedPtr<FJsonValue>> Issues;
	TSharedPtr<FJsonObject> Root;
	if (!UeAgentControlRigOps::LoadAuthoringFile(JsonFile, Root, Issues, OutError, false) || !Root.IsValid())
	{
		OutData = MakeShared<FJsonObject>();
		OutData->SetArrayField(TEXT("issues"), Issues);
		return false;
	}
	UeAgentJsonDiagnostics::WarnUnknownFields(Root, TEXT("shape_library"), { TEXT("schema"), TEXT("asset_path"), TEXT("object_path"), TEXT("asset_class"), TEXT("default_shape"), TEXT("default_material"), TEXT("xray_material"), TEXT("material_color_parameter"), TEXT("shapes"), TEXT("shape_count") }, Issues);

	FString AssetPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPath))
	{
		UeAgentJsonDiagnostics::ReadStringField(Root, TEXT("asset_path"), TEXT("shape_library.asset_path"), AssetPath, Issues, true);
	}
	if (AssetPath.TrimStartAndEnd().IsEmpty())
	{
		OutError = TEXT("asset_path_required");
		OutData = MakeShared<FJsonObject>();
		OutData->SetArrayField(TEXT("issues"), Issues);
		return false;
	}

	bool bCreateIfMissing = false;
	JsonTryGetBool(Ctx.Params, TEXT("create_if_missing"), bCreateIfMissing);
	UControlRigShapeLibrary* Library = UeAgentControlRigOps::LoadShapeLibrary(AssetPath);
	if (!Library && bCreateIfMissing)
	{
		Library = UeAgentControlRigOps::CreateShapeLibraryAsset(AssetPath, OutError);
	}
	if (!Library)
	{
		OutError = TEXT("control_rig_shape_library_not_found");
		OutData = MakeShared<FJsonObject>();
		OutData->SetArrayField(TEXT("issues"), Issues);
		return false;
	}

	const TSharedPtr<FJsonObject>* DefaultShapeObj = nullptr;
	if (Root->TryGetObjectField(TEXT("default_shape"), DefaultShapeObj) && DefaultShapeObj && DefaultShapeObj->IsValid())
	{
		UeAgentControlRigOps::ApplyShapeDefinitionJson(*DefaultShapeObj, TEXT("shape_library.default_shape"), Library->DefaultShape, Issues);
	}

	FString MaterialPath;
	if (Root->TryGetStringField(TEXT("default_material"), MaterialPath))
	{
		Library->DefaultMaterial = MaterialPath.TrimStartAndEnd().IsEmpty() ? nullptr : LoadShapeLibraryMaterial(MaterialPath);
		if (!MaterialPath.TrimStartAndEnd().IsEmpty() && Library->DefaultMaterial.IsNull())
		{
			UeAgentJsonDiagnostics::AddIssue(Issues, TEXT("warning"), TEXT("asset_reference_not_found"), TEXT("shape_library.default_material"), FString::Printf(TEXT("Material '%s' could not be loaded."), *MaterialPath));
		}
	}
	if (Root->TryGetStringField(TEXT("xray_material"), MaterialPath))
	{
		Library->XRayMaterial = MaterialPath.TrimStartAndEnd().IsEmpty() ? nullptr : LoadShapeLibraryMaterial(MaterialPath);
		if (!MaterialPath.TrimStartAndEnd().IsEmpty() && Library->XRayMaterial.IsNull())
		{
			UeAgentJsonDiagnostics::AddIssue(Issues, TEXT("warning"), TEXT("asset_reference_not_found"), TEXT("shape_library.xray_material"), FString::Printf(TEXT("Material '%s' could not be loaded."), *MaterialPath));
		}
	}
	FString ColorParameter;
	if (Root->TryGetStringField(TEXT("material_color_parameter"), ColorParameter))
	{
		Library->MaterialColorParameter = FName(*ColorParameter);
	}

	const TArray<TSharedPtr<FJsonValue>>* Shapes = nullptr;
	if (UeAgentJsonDiagnostics::ReadArrayField(Root, TEXT("shapes"), TEXT("shape_library.shapes"), Shapes, Issues, false) && Shapes)
	{
		Library->Shapes.Reset();
		for (int32 Index = 0; Index < Shapes->Num(); ++Index)
		{
			TSharedPtr<FJsonObject> ShapeObj;
			const FString ShapePath = FString::Printf(TEXT("shape_library.shapes[%d]"), Index);
			if (UeAgentJsonDiagnostics::ReadObjectFromValue((*Shapes)[Index], ShapePath, ShapeObj, Issues))
			{
				FControlRigShapeDefinition Shape;
				UeAgentControlRigOps::ApplyShapeDefinitionJson(ShapeObj, ShapePath, Shape, Issues);
				Library->Shapes.Add(Shape);
			}
		}
	}

	Library->MarkPackageDirty();
	bool bSaveAfterApply = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_apply"), bSaveAfterApply);
	if (bSaveAfterApply && !UeAgentControlRigOps::SaveAssetPackage(Library, OutError))
	{
		return false;
	}

	OutData = UeAgentControlRigOps::BuildShapeLibraryInfo(Library);
	OutData->SetStringField(TEXT("json_file"), UeAgentJsonDiagnostics::ResolveProjectRelativeFilePath(JsonFile));
	OutData->SetArrayField(TEXT("issues"), Issues);
	OutData->SetBoolField(TEXT("valid"), !UeAgentControlRigOps::HasBlockingIssue(Issues));
	OutData->SetBoolField(TEXT("saved"), bSaveAfterApply);
	return !UeAgentControlRigOps::HasBlockingIssue(Issues);
}

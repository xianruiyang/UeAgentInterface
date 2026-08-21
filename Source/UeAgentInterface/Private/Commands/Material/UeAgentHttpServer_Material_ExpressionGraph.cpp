// Copyright Epic Games, Inc. All Rights Reserved.

#include "UeAgentHttpServer_Material.h"

#include "UeAgentHttpServer_MaterialOps.h"

#include "Domains/Material/Authoring/UeAgentMaterialGraphAuthoring.h"
#include "Domains/Material/Authoring/UeAgentMaterialGraphMutationApplyProvider.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "Engine/Texture.h"
#include "MaterialEditingLibrary.h"
#include "MaterialGraph/MaterialGraph.h"
#include "MaterialGraph/MaterialGraphNode.h"
#include "MaterialGraph/MaterialGraphSchema.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialFunctionInterface.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInterface.h"
#include "MaterialShared.h"
#include "Misc/PackageName.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "UObject/UnrealType.h"

#include "Utilities/AuthoringCore/UeAgentAuthoringTargetLocator.h"
#include "Utilities/UeAgentPropertyValueCodec.h"

namespace
{
	bool ReadExpressionSelector(
		const TSharedPtr<FJsonObject>& Params,
		const FString& Prefix,
		UeAgentMaterialAuthoring::FExpressionSelector& OutSelector,
		FString& OutError)
	{
		OutSelector = UeAgentMaterialAuthoring::FExpressionSelector();
		const FString GuidKey = Prefix.IsEmpty() ? TEXT("expression_guid") : Prefix + TEXT("_guid");
		const FString NameKey = Prefix.IsEmpty() ? TEXT("expression_name") : Prefix + TEXT("_name");
		const FString IndexKey = Prefix.IsEmpty() ? TEXT("expression_index") : Prefix + TEXT("_index");
		FString GuidText;
		if (Params.IsValid() && Params->TryGetStringField(GuidKey, GuidText)
			&& !GuidText.IsEmpty() && !FGuid::Parse(GuidText, OutSelector.ExpressionGuid))
		{
			OutError = TEXT("invalid_") + GuidKey;
			return false;
		}
		if (Params.IsValid())
		{
			Params->TryGetStringField(NameKey, OutSelector.ExpressionName);
			double Index = INDEX_NONE;
			if (Params->TryGetNumberField(IndexKey, Index)) OutSelector.ExpressionIndex = static_cast<int32>(Index);
		}
		if (!OutSelector.HasAny())
		{
			OutError = Prefix.IsEmpty()
				? TEXT("missing_expression_selector")
				: TEXT("missing_") + Prefix + TEXT("_selector");
			return false;
		}
		return true;
	}

	void SetMutationFailure(
		const UeAgentMaterialAuthoring::FGraphMutationResult& Result,
		const TSharedPtr<FJsonObject>& OutData)
	{
		OutData->SetStringField(TEXT("apply_status"), Result.Status);
		OutData->SetStringField(TEXT("apply_phase"), Result.Phase);
		OutData->SetBoolField(TEXT("rollback_attempted"), Result.bRollbackAttempted);
		OutData->SetBoolField(TEXT("rollback_succeeded"), Result.bRollbackSucceeded);
		if (!Result.ErrorDetail.IsEmpty()) OutData->SetStringField(TEXT("error_detail"), Result.ErrorDetail);
	}

	UMaterialExpression* ResolveMaterialExpression(
		const FString& AssetPath,
		const UeAgentMaterialAuthoring::FExpressionSelector& Selector,
		FString& OutError)
	{
		UMaterial* Material = UeAgentMaterialOps::LoadMaterialAsset(AssetPath);
		if (!Material)
		{
			OutError = TEXT("material_not_found");
			return nullptr;
		}
		return UeAgentMaterialAuthoring::ResolveExpressionUnique(*Material, Selector, OutError);
	}

	bool BuildDeprecatedMaterialExpressionPropertyDescriptor(
		UMaterialExpression& Expression,
		const TSharedPtr<FJsonObject>& Params,
		const FString& PropertyPath,
		TSharedPtr<FJsonValue>& OutDescriptor,
		FString& OutError)
	{
		TSharedRef<FJsonObject> Descriptor = MakeShared<FJsonObject>();
		Descriptor->SetStringField(TEXT("property_path"), PropertyPath);
		const TSharedPtr<FJsonValue>* StructuredValue = Params->Values.Find(TEXT("value"));
		if (StructuredValue && StructuredValue->IsValid())
		{
			Descriptor->SetField(TEXT("value"), *StructuredValue);
		}
		else
		{
			FString ValueText;
			if (!Params->TryGetStringField(TEXT("value_text"), ValueText))
			{
				OutError = TEXT("missing_value");
				return false;
			}
			UeAgentAuthoringTargetLocator::FResolvedPropertyTarget Target;
			UeAgentAuthoringTargetLocator::FResolveResult ResolveResult;
			if (!UeAgentAuthoringTargetLocator::ResolvePropertyPath(
				&Expression, PropertyPath, Target, ResolveResult))
			{
				OutError = ResolveResult.ErrorCode.IsEmpty() ? TEXT("property_not_found") : ResolveResult.ErrorCode;
				return false;
			}
			if (CastField<FObjectPropertyBase>(Target.Property) || CastField<FSoftObjectProperty>(Target.Property))
			{
				Descriptor->SetStringField(TEXT("value"), ValueText);
			}
			else
			{
				void* TemporaryValue = FMemory::Malloc(Target.Property->GetSize(), Target.Property->GetMinAlignment());
				Target.Property->InitializeValue(TemporaryValue);
				const bool bImported = Target.Property->ImportText_Direct(
					*ValueText, TemporaryValue, &Expression, PPF_None, GLog) != nullptr;
				if (!bImported)
				{
					Target.Property->DestroyValue(TemporaryValue);
					FMemory::Free(TemporaryValue);
					OutError = TEXT("property_import_failed:") + PropertyPath;
					return false;
				}
				Descriptor->SetField(TEXT("value"), UeAgentPropertyValueCodec::Encode(
					Target.Property,
					TemporaryValue,
					&Expression,
					UeAgentPropertyValueCodec::FOptions()));
				Target.Property->DestroyValue(TemporaryValue);
				FMemory::Free(TemporaryValue);
			}
		}
		static const TArray<FString> AssertionFields = {
			TEXT("value_kind"), TEXT("struct_path"), TEXT("enum_path"), TEXT("element_type"),
			TEXT("key_type"), TEXT("mapped_type"), TEXT("inner_type"), TEXT("fixed_length"), TEXT("allowed_class")
		};
		for (const FString& Field : AssertionFields)
		{
			if (const TSharedPtr<FJsonValue>* Value = Params->Values.Find(Field)) Descriptor->SetField(Field, *Value);
		}
		OutDescriptor = MakeShared<FJsonValueObject>(Descriptor);
		return true;
	}
}

bool FUeAgentLegacyMechanics::CmdMaterialListExpressions(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	UMaterial* Material = UeAgentMaterialOps::LoadMaterialAsset(AssetPathInput);
	if (!Material)
	{
		OutError = TEXT("material_not_found");
		return false;
	}

	TArray<TSharedPtr<FJsonValue>> ExpressionArray;
	for (UMaterialExpression* Expression : Material->GetExpressions())
	{
		if (!Expression)
		{
			continue;
		}
		ExpressionArray.Add(MakeShared<FJsonValueObject>(UeAgentMaterialOps::ExpressionToJson(Expression)));
	}

	OutData->SetStringField(TEXT("asset_path"), Material->GetOutermost()->GetName());
	OutData->SetArrayField(TEXT("expressions"), ExpressionArray);
	OutData->SetNumberField(TEXT("count"), ExpressionArray.Num());
	return true;
}

bool FUeAgentLegacyMechanics::CmdMaterialAddExpression(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}
	FString ExpressionClassPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("expression_class"), ExpressionClassPath) || ExpressionClassPath.IsEmpty())
	{
		OutError = TEXT("missing_expression_class");
		return false;
	}

	double NodePosX = 0.0;
	double NodePosY = 0.0;
	JsonTryGetNumber(Ctx.Params, TEXT("node_pos_x"), NodePosX);
	JsonTryGetNumber(Ctx.Params, TEXT("node_pos_y"), NodePosY);
	FString ExpressionName;
	JsonTryGetString(Ctx.Params, TEXT("expression_name"), ExpressionName);

	bool bCompileAfterAdd = true;
	JsonTryGetBool(Ctx.Params, TEXT("compile_after_add"), bCompileAfterAdd);
	bool bSaveAfterAdd = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_add"), bSaveAfterAdd);

	UeAgentMaterialAuthoring::FGraphMutationRequest Request;
	Request.Operation = UeAgentApplyInputContract::EOperation::Create;
	Request.Kind = UeAgentMaterialAuthoring::EGraphMutationKind::Expression;
	Request.AssetPath = AssetPathInput;
	Request.ExpressionClassPath = ExpressionClassPath;
	Request.NewExpressionGuid = FGuid::NewGuid();
	Request.SuggestedName = ExpressionName;
	Request.NodePosX = static_cast<int32>(NodePosX);
	Request.NodePosY = static_cast<int32>(NodePosY);
	Request.bCompile = bCompileAfterAdd;
	Request.bSave = bSaveAfterAdd;
	const TArray<TSharedPtr<FJsonValue>>* Properties = nullptr;
	if (Ctx.Params->TryGetArrayField(TEXT("properties"), Properties) && Properties) Request.Properties = *Properties;
	UeAgentMaterialAuthoring::FGraphMutationResult Result;
	if (!UeAgentMaterialAuthoring::ApplyGraphMutation(Request, Result, Ctx.RequestId))
	{
		SetMutationFailure(Result, OutData);
		OutError = Result.ErrorCode.IsEmpty() ? TEXT("create_expression_failed") : Result.ErrorCode;
		return false;
	}
	UeAgentMaterialAuthoring::FExpressionSelector Selector;
	Selector.ExpressionGuid = Request.NewExpressionGuid;
	UMaterialExpression* NewExpression = ResolveMaterialExpression(AssetPathInput, Selector, OutError);
	if (!NewExpression)
	{
		OutError = OutError.IsEmpty() ? TEXT("create_expression_readback_failed") : OutError;
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), NewExpression->GetOutermost()->GetName());
	OutData->SetObjectField(TEXT("expression"), UeAgentMaterialOps::ExpressionToJson(NewExpression));
	OutData->SetBoolField(TEXT("compiled"), bCompileAfterAdd);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterAdd);
	OutData->SetStringField(TEXT("apply_status"), Result.Status);
	OutData->SetStringField(TEXT("resolved_operation"), Result.ResolvedOperation);
	return true;
}

bool FUeAgentLegacyMechanics::CmdMaterialSetExpressionProperty(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}
	FString PropertyPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("property_path"), PropertyPath))
	{
		JsonTryGetString(Ctx.Params, TEXT("property_name"), PropertyPath);
	}
	if (PropertyPath.IsEmpty())
	{
		OutError = TEXT("missing_property_path");
		return false;
	}

	UeAgentMaterialAuthoring::FExpressionSelector Selector;
	if (!ReadExpressionSelector(Ctx.Params, FString(), Selector, OutError)) return false;
	UMaterialExpression* Expression = ResolveMaterialExpression(AssetPathInput, Selector, OutError);
	if (!Expression)
	{
		if (OutError.IsEmpty()) OutError = TEXT("material_expression_not_found");
		return false;
	}
	TSharedPtr<FJsonValue> PropertyDescriptor;
	if (!BuildDeprecatedMaterialExpressionPropertyDescriptor(*Expression, Ctx.Params, PropertyPath, PropertyDescriptor, OutError))
	{
		return false;
	}

	bool bCompileAfterSet = true;
	JsonTryGetBool(Ctx.Params, TEXT("compile_after_set"), bCompileAfterSet);
	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);

	UeAgentMaterialAuthoring::FGraphMutationRequest Request;
	Request.Operation = UeAgentApplyInputContract::EOperation::Update;
	Request.Kind = UeAgentMaterialAuthoring::EGraphMutationKind::Expression;
	Request.AssetPath = AssetPathInput;
	Request.Expression = Selector;
	Request.Properties = { PropertyDescriptor };
	Request.bCompile = bCompileAfterSet;
	Request.bSave = bSaveAfterSet;
	UeAgentMaterialAuthoring::FGraphMutationResult Result;
	if (!UeAgentMaterialAuthoring::ApplyGraphMutation(Request, Result, Ctx.RequestId))
	{
		SetMutationFailure(Result, OutData);
		OutError = Result.ErrorCode.IsEmpty() ? TEXT("set_expression_property_failed") : Result.ErrorCode;
		return false;
	}
	Expression = ResolveMaterialExpression(AssetPathInput, Selector, OutError);
	if (!Expression)
	{
		OutError = OutError.IsEmpty() ? TEXT("material_expression_readback_failed") : OutError;
		return false;
	}
	UeAgentAuthoringTargetLocator::FResolvedPropertyTarget PropertyTarget;
	UeAgentAuthoringTargetLocator::FResolveResult PropertyResolveResult;
	if (!UeAgentAuthoringTargetLocator::ResolvePropertyPath(
		Expression, PropertyPath, PropertyTarget, PropertyResolveResult))
	{
		OutError = PropertyResolveResult.ErrorCode.IsEmpty() ? TEXT("property_readback_failed") : PropertyResolveResult.ErrorCode;
		return false;
	}
	FString AppliedValueText;
	PropertyTarget.Property->ExportTextItem_Direct(
		AppliedValueText, PropertyTarget.ValuePtr, nullptr, Expression, PPF_None);
	FString InputValueText;
	JsonTryGetString(Ctx.Params, TEXT("value_text"), InputValueText);

	OutData->SetStringField(TEXT("asset_path"), Expression->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("expression_guid"), Expression->MaterialExpressionGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
	OutData->SetStringField(TEXT("expression_name"), Expression->GetName());
	OutData->SetStringField(TEXT("property_name"), PropertyPath);
	OutData->SetStringField(TEXT("property_path"), PropertyPath);
	if (!InputValueText.IsEmpty()) OutData->SetStringField(TEXT("value_text"), InputValueText);
	OutData->SetStringField(TEXT("applied_value_text"), AppliedValueText);
	OutData->SetStringField(TEXT("cpp_type"), PropertyTarget.Property->GetCPPType());
	OutData->SetBoolField(TEXT("compiled"), bCompileAfterSet);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	OutData->SetStringField(TEXT("apply_status"), Result.Status);
	OutData->SetStringField(TEXT("resolved_operation"), Result.ResolvedOperation);
	return true;
}

bool FUeAgentLegacyMechanics::CmdMaterialConnectExpressions(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	UeAgentMaterialAuthoring::FExpressionSelector FromSelector;
	UeAgentMaterialAuthoring::FExpressionSelector ToSelector;
	if (!ReadExpressionSelector(Ctx.Params, TEXT("from_expression"), FromSelector, OutError)
		|| !ReadExpressionSelector(Ctx.Params, TEXT("to_expression"), ToSelector, OutError)) return false;

	FString FromOutputName;
	FString ToInputName;
	JsonTryGetString(Ctx.Params, TEXT("from_output_name"), FromOutputName);
	JsonTryGetString(Ctx.Params, TEXT("to_input_name"), ToInputName);
	FromOutputName = UeAgentMaterialAuthoring::NormalizeExpressionPinName(FromOutputName);
	ToInputName = UeAgentMaterialAuthoring::NormalizeExpressionPinName(ToInputName);

	bool bCompileAfterConnect = true;
	JsonTryGetBool(Ctx.Params, TEXT("compile_after_connect"), bCompileAfterConnect);
	bool bSaveAfterConnect = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_connect"), bSaveAfterConnect);

	UeAgentMaterialAuthoring::FGraphMutationRequest Request;
	Request.Operation = UeAgentApplyInputContract::EOperation::Upsert;
	Request.Kind = UeAgentMaterialAuthoring::EGraphMutationKind::Connection;
	Request.AssetPath = AssetPathInput;
	Request.FromExpression = FromSelector;
	Request.ToExpression = ToSelector;
	Request.FromOutputName = FromOutputName;
	Request.ToInputName = ToInputName;
	Request.bCompile = bCompileAfterConnect;
	Request.bSave = bSaveAfterConnect;
	UeAgentMaterialAuthoring::FGraphMutationResult Result;
	if (!UeAgentMaterialAuthoring::ApplyGraphMutation(Request, Result, Ctx.RequestId))
	{
		SetMutationFailure(Result, OutData);
		OutError = Result.ErrorCode.IsEmpty() ? TEXT("connect_expressions_failed") : Result.ErrorCode;
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), UeAgentMaterialOps::NormalizeAssetPath(AssetPathInput));
	OutData->SetStringField(TEXT("from_expression_guid"), Result.FromExpressionGuid);
	OutData->SetStringField(TEXT("to_expression_guid"), Result.ToExpressionGuid);
	OutData->SetStringField(TEXT("from_output_name"), FromOutputName);
	OutData->SetStringField(TEXT("to_input_name"), ToInputName);
	OutData->SetBoolField(TEXT("compiled"), bCompileAfterConnect);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterConnect);
	OutData->SetStringField(TEXT("apply_status"), Result.Status);
	return true;
}

bool FUeAgentLegacyMechanics::CmdMaterialConnectExpressionToProperty(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}
	FString MaterialPropertyName;
	if (!JsonTryGetString(Ctx.Params, TEXT("material_property"), MaterialPropertyName) || MaterialPropertyName.IsEmpty())
	{
		OutError = TEXT("missing_material_property");
		return false;
	}

	UeAgentMaterialAuthoring::FExpressionSelector Selector;
	if (!ReadExpressionSelector(Ctx.Params, FString(), Selector, OutError)) return false;

	FString OutputName;
	JsonTryGetString(Ctx.Params, TEXT("output_name"), OutputName);
	OutputName = UeAgentMaterialAuthoring::NormalizeExpressionPinName(OutputName);

	bool bCompileAfterConnect = true;
	JsonTryGetBool(Ctx.Params, TEXT("compile_after_connect"), bCompileAfterConnect);
	bool bSaveAfterConnect = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_connect"), bSaveAfterConnect);

	UeAgentMaterialAuthoring::FGraphMutationRequest Request;
	Request.Operation = UeAgentApplyInputContract::EOperation::Upsert;
	Request.Kind = UeAgentMaterialAuthoring::EGraphMutationKind::RootInput;
	Request.AssetPath = AssetPathInput;
	Request.Expression = Selector;
	Request.FromOutputName = OutputName;
	Request.MaterialProperty = MaterialPropertyName;
	Request.bCompile = bCompileAfterConnect;
	Request.bSave = bSaveAfterConnect;
	UeAgentMaterialAuthoring::FGraphMutationResult Result;
	if (!UeAgentMaterialAuthoring::ApplyGraphMutation(Request, Result, Ctx.RequestId))
	{
		SetMutationFailure(Result, OutData);
		OutError = Result.ErrorCode.IsEmpty() ? TEXT("connect_expression_failed") : Result.ErrorCode;
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), UeAgentMaterialOps::NormalizeAssetPath(AssetPathInput));
	OutData->SetStringField(TEXT("material_property"), MaterialPropertyName);
	OutData->SetStringField(TEXT("expression_guid"), Result.ExpressionGuid);
	OutData->SetStringField(TEXT("expression_name"), Result.ExpressionName);
	OutData->SetStringField(TEXT("output_name"), OutputName);
	OutData->SetBoolField(TEXT("compiled"), bCompileAfterConnect);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterConnect);
	OutData->SetStringField(TEXT("apply_status"), Result.Status);
	return true;
}

bool FUeAgentLegacyMechanics::CmdMaterialDeleteExpression(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	UeAgentMaterialAuthoring::FExpressionSelector Selector;
	if (!ReadExpressionSelector(Ctx.Params, FString(), Selector, OutError)) return false;
	UMaterialExpression* Expression = ResolveMaterialExpression(AssetPathInput, Selector, OutError);
	if (!Expression)
	{
		if (OutError.IsEmpty()) OutError = TEXT("material_expression_not_found");
		return false;
	}
	const bool bWasRootedBeforeDelete = Expression->IsRooted();

	bool bCompileAfterDelete = true;
	JsonTryGetBool(Ctx.Params, TEXT("compile_after_delete"), bCompileAfterDelete);
	bool bSaveAfterDelete = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_delete"), bSaveAfterDelete);

	UeAgentMaterialAuthoring::FGraphMutationRequest Request;
	Request.Operation = UeAgentApplyInputContract::EOperation::Remove;
	Request.Kind = UeAgentMaterialAuthoring::EGraphMutationKind::Expression;
	Request.AssetPath = AssetPathInput;
	Request.Expression = Selector;
	Request.bCompile = bCompileAfterDelete;
	Request.bSave = bSaveAfterDelete;
	UeAgentMaterialAuthoring::FGraphMutationResult Result;
	if (!UeAgentMaterialAuthoring::ApplyGraphMutation(Request, Result, Ctx.RequestId))
	{
		SetMutationFailure(Result, OutData);
		OutError = Result.ErrorCode.IsEmpty() ? TEXT("delete_expression_failed") : Result.ErrorCode;
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), UeAgentMaterialOps::NormalizeAssetPath(AssetPathInput));
	OutData->SetStringField(TEXT("deleted_expression_guid"), Result.ExpressionGuid);
	OutData->SetStringField(TEXT("deleted_expression_name"), Result.ExpressionName);
	OutData->SetBoolField(TEXT("was_rooted_before_delete"), bWasRootedBeforeDelete);
	OutData->SetBoolField(TEXT("removed_root_before_delete"), bWasRootedBeforeDelete);
	OutData->SetBoolField(TEXT("compiled"), bCompileAfterDelete);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterDelete);
	OutData->SetStringField(TEXT("apply_status"), Result.Status);
	return true;
}

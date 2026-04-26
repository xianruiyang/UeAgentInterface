// Copyright Epic Games, Inc. All Rights Reserved.

#include "UeAgentHttpServer_EnhancedInput.h"

#include "UeAgentInterfaceLogger.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Editor.h"
#include "FileHelpers.h"
#include "InputAction.h"
#include "InputActionValue.h"
#include "InputCoreTypes.h"
#include "InputMappingContext.h"
#include "InputModifiers.h"
#include "InputTriggers.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace UeAgentEnhancedInputOps
{
	static FString NormalizeAssetPath(const FString& InPath)
	{
		FString OutPath = InPath;
		OutPath.TrimStartAndEndInline();

		int32 DotIndex = INDEX_NONE;
		if (OutPath.FindChar(TEXT('.'), DotIndex) && DotIndex > 0)
		{
			OutPath = OutPath.Left(DotIndex);
		}
		return OutPath;
	}

	static FString ToObjectPath(const FString& InPath)
	{
		FString InObjectPath = InPath;
		InObjectPath.TrimStartAndEndInline();
		if (InObjectPath.IsEmpty())
		{
			return FString();
		}
		if (InObjectPath.Contains(TEXT(".")))
		{
			return InObjectPath;
		}

		const FString AssetPath = NormalizeAssetPath(InObjectPath);
		const FString AssetName = FPackageName::GetLongPackageAssetName(AssetPath);
		if (AssetName.IsEmpty())
		{
			return FString();
		}
		return FString::Printf(TEXT("%s.%s"), *AssetPath, *AssetName);
	}

	static UObject* LoadAssetObject(const FString& InPath)
	{
		const FString ObjectPath = ToObjectPath(InPath);
		if (ObjectPath.IsEmpty())
		{
			return nullptr;
		}
		if (UObject* Existing = FindObject<UObject>(nullptr, *ObjectPath))
		{
			return Existing;
		}
		return LoadObject<UObject>(nullptr, *ObjectPath);
	}

	static bool AssetExists(const FString& InPath)
	{
		const FString ObjectPath = ToObjectPath(InPath);
		if (ObjectPath.IsEmpty())
		{
			return false;
		}

		if (FindObject<UObject>(nullptr, *ObjectPath))
		{
			return true;
		}

		FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		return AssetRegistryModule.Get().GetAssetByObjectPath(FSoftObjectPath(ObjectPath)).IsValid();
	}

	static UInputAction* LoadInputAction(const FString& InPath)
	{
		return Cast<UInputAction>(LoadAssetObject(InPath));
	}

	static UInputMappingContext* LoadInputMappingContext(const FString& InPath)
	{
		return Cast<UInputMappingContext>(LoadAssetObject(InPath));
	}

	static bool SaveJsonFile(const FString& OutputPathInput, const TSharedPtr<FJsonObject>& Root, FString& OutOutputPath, FString& OutError)
	{
		OutOutputPath.Reset();
		if (!Root.IsValid())
		{
			OutError = TEXT("json_root_invalid");
			return false;
		}

		FString OutputPath = OutputPathInput;
		OutputPath.TrimStartAndEndInline();
		if (OutputPath.IsEmpty())
		{
			OutError = TEXT("missing_output_file");
			return false;
		}

		OutputPath = FPaths::ConvertRelativePathToFull(OutputPath);
		const FString OutputDir = FPaths::GetPath(OutputPath);
		if (!OutputDir.IsEmpty() && !IFileManager::Get().DirectoryExists(*OutputDir))
		{
			IFileManager::Get().MakeDirectory(*OutputDir, true);
		}

		FString JsonText;
		const TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&JsonText);
		if (!FJsonSerializer::Serialize(Root.ToSharedRef(), Writer))
		{
			OutError = TEXT("json_serialize_failed");
			return false;
		}

		if (!FFileHelper::SaveStringToFile(JsonText, *OutputPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			OutError = TEXT("save_json_file_failed");
			return false;
		}

		OutOutputPath = OutputPath;
		return true;
	}

	static bool LoadJsonFile(const FString& InputPathInput, TSharedPtr<FJsonObject>& OutRoot, FString& OutResolvedPath, FString& OutError)
	{
		OutRoot.Reset();
		OutResolvedPath.Reset();

		FString InputPath = InputPathInput;
		InputPath.TrimStartAndEndInline();
		if (InputPath.IsEmpty())
		{
			OutError = TEXT("missing_json_file");
			return false;
		}

		InputPath = FPaths::ConvertRelativePathToFull(InputPath);
		if (!FPaths::FileExists(InputPath))
		{
			OutError = TEXT("json_file_not_found");
			return false;
		}

		FString JsonText;
		if (!FFileHelper::LoadFileToString(JsonText, *InputPath))
		{
			OutError = TEXT("load_json_file_failed");
			return false;
		}

		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
		if (!FJsonSerializer::Deserialize(Reader, OutRoot) || !OutRoot.IsValid())
		{
			OutError = TEXT("json_parse_failed");
			return false;
		}

		OutResolvedPath = InputPath;
		return true;
	}

	static bool SaveAssetPackage(UObject* Asset, FString& OutError)
	{
		if (!Asset)
		{
			OutError = TEXT("asset_not_found");
			return false;
		}

		TArray<UPackage*> PackagesToSave;
		PackagesToSave.Add(Asset->GetOutermost());
		if (!UEditorLoadingAndSavingUtils::SavePackages(PackagesToSave, false))
		{
			OutError = TEXT("save_asset_failed");
			return false;
		}
		return true;
	}

	static bool CreateInputActionAsset(const FString& AssetPathInput, UInputAction*& OutInputAction, FString& OutError)
	{
		OutInputAction = nullptr;

		const FString PackageName = NormalizeAssetPath(AssetPathInput);
		if (!FPackageName::IsValidLongPackageName(PackageName))
		{
			OutError = TEXT("invalid_asset_path");
			return false;
		}

		const FString AssetName = FPackageName::GetLongPackageAssetName(PackageName);
		if (AssetName.IsEmpty())
		{
			OutError = TEXT("invalid_asset_name");
			return false;
		}

		const FString ObjectPath = FString::Printf(TEXT("%s.%s"), *PackageName, *AssetName);
		if (AssetExists(ObjectPath))
		{
			OutError = TEXT("asset_already_exists");
			return false;
		}

		UPackage* Package = CreatePackage(*PackageName);
		if (!Package)
		{
			OutError = TEXT("create_package_failed");
			return false;
		}

		UInputAction* InputAction = NewObject<UInputAction>(Package, *AssetName, RF_Public | RF_Standalone | RF_Transactional);
		if (!InputAction)
		{
			OutError = TEXT("create_input_action_failed");
			return false;
		}

		FAssetRegistryModule::AssetCreated(InputAction);
		InputAction->MarkPackageDirty();
		Package->MarkPackageDirty();
		OutInputAction = InputAction;
		return true;
	}

	static bool CreateMappingContextAsset(const FString& AssetPathInput, UInputMappingContext*& OutMappingContext, FString& OutError)
	{
		OutMappingContext = nullptr;

		const FString PackageName = NormalizeAssetPath(AssetPathInput);
		if (!FPackageName::IsValidLongPackageName(PackageName))
		{
			OutError = TEXT("invalid_asset_path");
			return false;
		}

		const FString AssetName = FPackageName::GetLongPackageAssetName(PackageName);
		if (AssetName.IsEmpty())
		{
			OutError = TEXT("invalid_asset_name");
			return false;
		}

		const FString ObjectPath = FString::Printf(TEXT("%s.%s"), *PackageName, *AssetName);
		if (AssetExists(ObjectPath))
		{
			OutError = TEXT("asset_already_exists");
			return false;
		}

		UPackage* Package = CreatePackage(*PackageName);
		if (!Package)
		{
			OutError = TEXT("create_package_failed");
			return false;
		}

		UInputMappingContext* MappingContext = NewObject<UInputMappingContext>(Package, *AssetName, RF_Public | RF_Standalone | RF_Transactional);
		if (!MappingContext)
		{
			OutError = TEXT("create_mapping_context_failed");
			return false;
		}

		FAssetRegistryModule::AssetCreated(MappingContext);
		MappingContext->MarkPackageDirty();
		Package->MarkPackageDirty();
		MappingContext->PostEditChange();
		OutMappingContext = MappingContext;
		return true;
	}

	static FString ValueTypeToString(const EInputActionValueType InValueType)
	{
		switch (InValueType)
		{
		case EInputActionValueType::Boolean: return TEXT("Boolean");
		case EInputActionValueType::Axis1D: return TEXT("Axis1D");
		case EInputActionValueType::Axis2D: return TEXT("Axis2D");
		case EInputActionValueType::Axis3D: return TEXT("Axis3D");
		default: return TEXT("Boolean");
		}
	}

	static bool ParseValueType(const FString& InValueType, EInputActionValueType& OutValueType)
	{
		const FString ValueTypeLower = InValueType.TrimStartAndEnd().ToLower();
		if (ValueTypeLower.IsEmpty())
		{
			return false;
		}
		if (ValueTypeLower == TEXT("boolean") || ValueTypeLower == TEXT("bool") || ValueTypeLower == TEXT("digital"))
		{
			OutValueType = EInputActionValueType::Boolean;
			return true;
		}
		if (ValueTypeLower == TEXT("axis1d") || ValueTypeLower == TEXT("1d"))
		{
			OutValueType = EInputActionValueType::Axis1D;
			return true;
		}
		if (ValueTypeLower == TEXT("axis2d") || ValueTypeLower == TEXT("2d"))
		{
			OutValueType = EInputActionValueType::Axis2D;
			return true;
		}
		if (ValueTypeLower == TEXT("axis3d") || ValueTypeLower == TEXT("3d"))
		{
			OutValueType = EInputActionValueType::Axis3D;
			return true;
		}
		return false;
	}

	static void AppendModifierClassPaths(const TArray<TObjectPtr<UInputModifier>>& Modifiers, TArray<TSharedPtr<FJsonValue>>& OutModifierClasses)
	{
		OutModifierClasses.Reset();
		for (const TObjectPtr<UInputModifier>& Modifier : Modifiers)
		{
			if (!Modifier)
			{
				continue;
			}

			OutModifierClasses.Add(MakeShared<FJsonValueString>(Modifier->GetClass()->GetPathName()));
		}
	}

	static void AppendTriggerClassPaths(const TArray<TObjectPtr<UInputTrigger>>& Triggers, TArray<TSharedPtr<FJsonValue>>& OutTriggerClasses)
	{
		OutTriggerClasses.Reset();
		for (const TObjectPtr<UInputTrigger>& Trigger : Triggers)
		{
			if (!Trigger)
			{
				continue;
			}

			OutTriggerClasses.Add(MakeShared<FJsonValueString>(Trigger->GetClass()->GetPathName()));
		}
	}

	static bool ResolveInputKey(const FString& InKeyName, FKey& OutKey)
	{
		const FString KeyName = InKeyName.TrimStartAndEnd();
		if (KeyName.IsEmpty())
		{
			return false;
		}

		const FKey ParsedKey{ FName(*KeyName) };
		if (!ParsedKey.IsValid())
		{
			return false;
		}

		OutKey = ParsedKey;
		return true;
	}

	static UClass* ResolveClassByPath(const FString& InClassPath)
	{
		FString ClassPath = InClassPath;
		ClassPath.TrimStartAndEndInline();
		if (ClassPath.IsEmpty())
		{
			return nullptr;
		}

		if (UClass* Existing = FindObject<UClass>(nullptr, *ClassPath))
		{
			return Existing;
		}
		if (UClass* Loaded = LoadObject<UClass>(nullptr, *ClassPath))
		{
			return Loaded;
		}
		if (!ClassPath.EndsWith(TEXT("_C")))
		{
			const FString GeneratedClassPath = ClassPath + TEXT("_C");
			if (UClass* GeneratedClass = LoadObject<UClass>(nullptr, *GeneratedClassPath))
			{
				return GeneratedClass;
			}
		}
		return nullptr;
	}

	static bool PopulateActionModifiersFromClassArray(
		const TArray<TSharedPtr<FJsonValue>>& ModifierClassArray,
		UObject* Outer,
		TArray<TObjectPtr<UInputModifier>>& OutModifiers,
		FString& OutError)
	{
		OutModifiers.Reset();
		for (const TSharedPtr<FJsonValue>& ModifierClassValue : ModifierClassArray)
		{
			FString ModifierClassPath;
			if (!ModifierClassValue.IsValid() || !ModifierClassValue->TryGetString(ModifierClassPath))
			{
				OutError = TEXT("invalid_modifier_class_item");
				return false;
			}

			UClass* ModifierClass = ResolveClassByPath(ModifierClassPath);
			if (!ModifierClass || !ModifierClass->IsChildOf(UInputModifier::StaticClass()))
			{
				OutError = TEXT("invalid_modifier_class");
				return false;
			}

			UInputModifier* Modifier = NewObject<UInputModifier>(Outer, ModifierClass, NAME_None, RF_Transactional);
			if (!Modifier)
			{
				OutError = TEXT("create_modifier_failed");
				return false;
			}

			OutModifiers.Add(Modifier);
		}

		return true;
	}

	static bool PopulateActionTriggersFromClassArray(
		const TArray<TSharedPtr<FJsonValue>>& TriggerClassArray,
		UObject* Outer,
		TArray<TObjectPtr<UInputTrigger>>& OutTriggers,
		FString& OutError)
	{
		OutTriggers.Reset();
		for (const TSharedPtr<FJsonValue>& TriggerClassValue : TriggerClassArray)
		{
			FString TriggerClassPath;
			if (!TriggerClassValue.IsValid() || !TriggerClassValue->TryGetString(TriggerClassPath))
			{
				OutError = TEXT("invalid_trigger_class_item");
				return false;
			}

			UClass* TriggerClass = ResolveClassByPath(TriggerClassPath);
			if (!TriggerClass || !TriggerClass->IsChildOf(UInputTrigger::StaticClass()))
			{
				OutError = TEXT("invalid_trigger_class");
				return false;
			}

			UInputTrigger* Trigger = NewObject<UInputTrigger>(Outer, TriggerClass, NAME_None, RF_Transactional);
			if (!Trigger)
			{
				OutError = TEXT("create_trigger_failed");
				return false;
			}

			OutTriggers.Add(Trigger);
		}

		return true;
	}

	static FProperty* FindPropertyByNameLoose(UStruct* StructType, const FString& Segment)
	{
		if (!StructType || Segment.IsEmpty())
		{
			return nullptr;
		}

		const FString Normalized = Segment.TrimStartAndEnd();
		const FString NormalizedWithoutB = Normalized.StartsWith(TEXT("b")) ? Normalized.Mid(1) : Normalized;
		for (TFieldIterator<FProperty> It(StructType, EFieldIterationFlags::IncludeSuper); It; ++It)
		{
			FProperty* Property = *It;
			const FString PropertyName = Property->GetName();
			const FString PropertyNameWithoutB = PropertyName.StartsWith(TEXT("b")) ? PropertyName.Mid(1) : PropertyName;
			if (PropertyName.Equals(Normalized, ESearchCase::IgnoreCase) ||
				PropertyNameWithoutB.Equals(NormalizedWithoutB, ESearchCase::IgnoreCase) ||
				Property->GetAuthoredName().Equals(Normalized, ESearchCase::IgnoreCase))
			{
				return Property;
			}
		}
		return nullptr;
	}

	static bool ResolvePropertyPath(UObject* Obj, const FString& PropertyPath, FProperty*& OutProperty, void*& OutValuePtr)
	{
		OutProperty = nullptr;
		OutValuePtr = nullptr;
		if (!Obj || PropertyPath.IsEmpty())
		{
			return false;
		}

		TArray<FString> Segments;
		PropertyPath.ParseIntoArray(Segments, TEXT("."), true);
		if (Segments.Num() == 0)
		{
			return false;
		}

		UStruct* CurrentStruct = Obj->GetClass();
		void* CurrentContainer = Obj;
		for (int32 Index = 0; Index < Segments.Num(); ++Index)
		{
			FProperty* Property = FindPropertyByNameLoose(CurrentStruct, Segments[Index]);
			if (!Property)
			{
				return false;
			}

			void* ValuePtr = Property->ContainerPtrToValuePtr<void>(CurrentContainer);
			if (Index == Segments.Num() - 1)
			{
				OutProperty = Property;
				OutValuePtr = ValuePtr;
				return true;
			}

			FStructProperty* StructProperty = CastField<FStructProperty>(Property);
			if (!StructProperty)
			{
				return false;
			}

			CurrentStruct = StructProperty->Struct;
			CurrentContainer = ValuePtr;
		}
		return false;
	}

	static TSharedPtr<FJsonObject> MappingToJson(const FEnhancedActionKeyMapping& Mapping, int32 Index, const bool bUseNormalizedActionPath = false)
	{
		TSharedPtr<FJsonObject> MappingObj = MakeShared<FJsonObject>();
		MappingObj->SetNumberField(TEXT("index"), Index);
		MappingObj->SetStringField(TEXT("key"), Mapping.Key.ToString());
		MappingObj->SetStringField(TEXT("action_name"), Mapping.Action ? Mapping.Action->GetName() : TEXT(""));
		MappingObj->SetStringField(
			TEXT("action_path"),
			Mapping.Action
				? (bUseNormalizedActionPath ? NormalizeAssetPath(Mapping.Action->GetOutermost()->GetName()) : Mapping.Action->GetPathName())
				: TEXT(""));
		MappingObj->SetNumberField(TEXT("modifier_count"), Mapping.Modifiers.Num());
		MappingObj->SetNumberField(TEXT("trigger_count"), Mapping.Triggers.Num());

		TArray<TSharedPtr<FJsonValue>> ModifierClasses;
		AppendModifierClassPaths(Mapping.Modifiers, ModifierClasses);
		MappingObj->SetArrayField(TEXT("modifier_classes"), ModifierClasses);

		TArray<TSharedPtr<FJsonValue>> TriggerClasses;
		AppendTriggerClassPaths(Mapping.Triggers, TriggerClasses);
		MappingObj->SetArrayField(TEXT("trigger_classes"), TriggerClasses);

		return MappingObj;
	}

	static TSharedPtr<FJsonObject> ExportActionJson(const UInputAction* InputAction)
	{
		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.enhanced_input.action.v1"));
		Root->SetNumberField(TEXT("format_version"), 1);
		Root->SetStringField(TEXT("asset_type"), TEXT("InputAction"));
		Root->SetStringField(TEXT("asset_path"), InputAction ? NormalizeAssetPath(InputAction->GetOutermost()->GetName()) : TEXT(""));
		Root->SetStringField(TEXT("object_path"), InputAction ? InputAction->GetPathName() : TEXT(""));
		Root->SetStringField(TEXT("value_type"), InputAction ? ValueTypeToString(InputAction->ValueType) : TEXT("Boolean"));
		Root->SetBoolField(TEXT("consume_input"), InputAction ? InputAction->bConsumeInput : true);
		Root->SetBoolField(TEXT("trigger_when_paused"), InputAction ? InputAction->bTriggerWhenPaused : false);
		Root->SetBoolField(TEXT("reserve_all_mappings"), InputAction ? InputAction->bReserveAllMappings : false);

		TArray<TSharedPtr<FJsonValue>> ModifierClasses;
		TArray<TSharedPtr<FJsonValue>> TriggerClasses;
		if (InputAction)
		{
			AppendModifierClassPaths(InputAction->Modifiers, ModifierClasses);
			AppendTriggerClassPaths(InputAction->Triggers, TriggerClasses);
		}
		Root->SetArrayField(TEXT("modifier_classes"), ModifierClasses);
		Root->SetArrayField(TEXT("trigger_classes"), TriggerClasses);
		Root->SetNumberField(TEXT("modifier_count"), ModifierClasses.Num());
		Root->SetNumberField(TEXT("trigger_count"), TriggerClasses.Num());
		return Root;
	}

	static TSharedPtr<FJsonObject> ExportMappingContextJson(const UInputMappingContext* MappingContext)
	{
		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.enhanced_input.mapping_context.v1"));
		Root->SetNumberField(TEXT("format_version"), 1);
		Root->SetStringField(TEXT("asset_type"), TEXT("InputMappingContext"));
		Root->SetStringField(TEXT("asset_path"), MappingContext ? NormalizeAssetPath(MappingContext->GetOutermost()->GetName()) : TEXT(""));
		Root->SetStringField(TEXT("object_path"), MappingContext ? MappingContext->GetPathName() : TEXT(""));

		TArray<TSharedPtr<FJsonValue>> MappingsArray;
		if (MappingContext)
		{
			const TArray<FEnhancedActionKeyMapping>& Mappings = MappingContext->GetMappings();
			for (int32 MappingIndex = 0; MappingIndex < Mappings.Num(); ++MappingIndex)
			{
				MappingsArray.Add(MakeShared<FJsonValueObject>(MappingToJson(Mappings[MappingIndex], MappingIndex, true)));
			}
			Root->SetNumberField(TEXT("mapping_count"), Mappings.Num());
		}
		else
		{
			Root->SetNumberField(TEXT("mapping_count"), 0);
		}

		Root->SetArrayField(TEXT("mappings"), MappingsArray);
		return Root;
	}
}

bool FUeAgentHttpServer::ExecuteEnhancedInputCommand(const FString& CommandLower, const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	if (CommandLower == TEXT("enhanced_input_create_action")) return CmdEnhancedInputCreateAction(Ctx, OutData, OutError);
	if (CommandLower == TEXT("enhanced_input_get_action_info")) return CmdEnhancedInputGetActionInfo(Ctx, OutData, OutError);
	if (CommandLower == TEXT("enhanced_input_set_action_property")) return CmdEnhancedInputSetActionProperty(Ctx, OutData, OutError);
	if (CommandLower == TEXT("enhanced_input_export_action_json")) return CmdEnhancedInputExportActionJson(Ctx, OutData, OutError);
	if (CommandLower == TEXT("enhanced_input_apply_action_json")) return CmdEnhancedInputApplyActionJson(Ctx, OutData, OutError);
	if (CommandLower == TEXT("enhanced_input_create_mapping_context")) return CmdEnhancedInputCreateMappingContext(Ctx, OutData, OutError);
	if (CommandLower == TEXT("enhanced_input_get_mapping_context_info")) return CmdEnhancedInputGetMappingContextInfo(Ctx, OutData, OutError);
	if (CommandLower == TEXT("enhanced_input_set_mapping_context_property")) return CmdEnhancedInputSetMappingContextProperty(Ctx, OutData, OutError);
	if (CommandLower == TEXT("enhanced_input_add_mapping")) return CmdEnhancedInputAddMapping(Ctx, OutData, OutError);
	if (CommandLower == TEXT("enhanced_input_remove_mapping")) return CmdEnhancedInputRemoveMapping(Ctx, OutData, OutError);
	if (CommandLower == TEXT("enhanced_input_clear_mappings")) return CmdEnhancedInputClearMappings(Ctx, OutData, OutError);
	if (CommandLower == TEXT("enhanced_input_export_mapping_context_json")) return CmdEnhancedInputExportMappingContextJson(Ctx, OutData, OutError);
	if (CommandLower == TEXT("enhanced_input_apply_mapping_context_json")) return CmdEnhancedInputApplyMappingContextJson(Ctx, OutData, OutError);

	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("command"), CommandLower);
	OutData->SetStringField(TEXT("category"), TEXT("enhanced_input"));
	OutError = TEXT("unknown_enhanced_input_command");
	return false;
}

bool FUeAgentHttpServer::CmdEnhancedInputCreateAction(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	UInputAction* InputAction = nullptr;
	if (!UeAgentEnhancedInputOps::CreateInputActionAsset(AssetPathInput, InputAction, OutError) || !InputAction)
	{
		return false;
	}

	FString ValueTypeText;
	if (JsonTryGetString(Ctx.Params, TEXT("value_type"), ValueTypeText))
	{
		EInputActionValueType ParsedValueType = EInputActionValueType::Boolean;
		if (!UeAgentEnhancedInputOps::ParseValueType(ValueTypeText, ParsedValueType))
		{
			OutError = TEXT("invalid_value_type");
			return false;
		}
		InputAction->ValueType = ParsedValueType;
	}

	bool bSaveAfterCreate = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_create"), bSaveAfterCreate);
	if (bSaveAfterCreate && !UeAgentEnhancedInputOps::SaveAssetPackage(InputAction, OutError))
	{
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), UeAgentEnhancedInputOps::NormalizeAssetPath(InputAction->GetOutermost()->GetName()));
	OutData->SetStringField(TEXT("object_path"), InputAction->GetPathName());
	OutData->SetStringField(TEXT("value_type"), UeAgentEnhancedInputOps::ValueTypeToString(InputAction->ValueType));
	OutData->SetBoolField(TEXT("saved"), bSaveAfterCreate);

	FUeAgentInterfaceLogger::Log(TEXT("EnhancedInput: created input action %s (saved=%s)"), *UeAgentEnhancedInputOps::NormalizeAssetPath(InputAction->GetOutermost()->GetName()), bSaveAfterCreate ? TEXT("true") : TEXT("false"));
	return true;
}

bool FUeAgentHttpServer::CmdEnhancedInputGetActionInfo(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	UInputAction* InputAction = UeAgentEnhancedInputOps::LoadInputAction(AssetPathInput);
	if (!InputAction)
	{
		OutError = TEXT("input_action_not_found");
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), InputAction->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("object_path"), InputAction->GetPathName());
	OutData->SetBoolField(TEXT("dirty"), InputAction->GetOutermost()->IsDirty());
	OutData->SetStringField(TEXT("value_type"), UeAgentEnhancedInputOps::ValueTypeToString(InputAction->ValueType));
	OutData->SetBoolField(TEXT("consume_input"), InputAction->bConsumeInput);
	OutData->SetBoolField(TEXT("trigger_when_paused"), InputAction->bTriggerWhenPaused);
	OutData->SetBoolField(TEXT("reserve_all_mappings"), InputAction->bReserveAllMappings);
	return true;
}

bool FUeAgentHttpServer::CmdEnhancedInputSetActionProperty(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}
	FString PropertyName;
	if (!JsonTryGetString(Ctx.Params, TEXT("property_name"), PropertyName) || PropertyName.IsEmpty())
	{
		OutError = TEXT("missing_property_name");
		return false;
	}
	FString ValueText;
	if (!JsonTryGetString(Ctx.Params, TEXT("value_text"), ValueText))
	{
		OutError = TEXT("missing_value_text");
		return false;
	}

	UInputAction* InputAction = UeAgentEnhancedInputOps::LoadInputAction(AssetPathInput);
	if (!InputAction)
	{
		OutError = TEXT("input_action_not_found");
		return false;
	}

	FProperty* Property = nullptr;
	void* ValuePtr = nullptr;
	if (!UeAgentEnhancedInputOps::ResolvePropertyPath(InputAction, PropertyName, Property, ValuePtr) || !Property || !ValuePtr)
	{
		OutError = TEXT("property_not_found");
		return false;
	}

	InputAction->Modify();
	if (!Property->ImportText_Direct(*ValueText, ValuePtr, InputAction, PPF_None))
	{
		const FString ImportError = FString::Printf(TEXT("property_import_failed:%s:%s"), *PropertyName, *ValueText);
		OutData->SetStringField(TEXT("property_name"), PropertyName);
		OutData->SetStringField(TEXT("value_text"), ValueText);
		SetPropertyImportResultFields(OutData, Property, ValueText, TEXT(""), TEXT("import_failed"), ImportError);
		OutError = ImportError;
		return false;
	}

	InputAction->MarkPackageDirty();
	InputAction->PostEditChange();

	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (bSaveAfterSet && !UeAgentEnhancedInputOps::SaveAssetPackage(InputAction, OutError))
	{
		return false;
	}

	FString ExportedValue;
	Property->ExportTextItem_Direct(ExportedValue, ValuePtr, nullptr, InputAction, PPF_None);
	OutData->SetStringField(TEXT("asset_path"), InputAction->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("property_name"), PropertyName);
	OutData->SetStringField(TEXT("value_text"), ValueText);
	OutData->SetStringField(TEXT("applied_value_text"), ExportedValue);
	SetPropertyImportResultFields(OutData, Property, ValueText, ExportedValue, TEXT("imported_and_read_back"));
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentHttpServer::CmdEnhancedInputExportActionJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	UInputAction* InputAction = UeAgentEnhancedInputOps::LoadInputAction(AssetPathInput);
	if (!InputAction)
	{
		OutError = TEXT("input_action_not_found");
		return false;
	}

	OutData = UeAgentEnhancedInputOps::ExportActionJson(InputAction);

	FString OutputFile;
	JsonTryGetString(Ctx.Params, TEXT("output_file"), OutputFile);
	OutputFile.TrimStartAndEndInline();
	FString SavedOutputFile;
	if (!OutputFile.IsEmpty())
	{
		if (!UeAgentEnhancedInputOps::SaveJsonFile(OutputFile, OutData, SavedOutputFile, OutError))
		{
			return false;
		}
		OutData->SetStringField(TEXT("output_file"), SavedOutputFile);
	}

	return true;
}

bool FUeAgentHttpServer::CmdEnhancedInputApplyActionJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	TSharedPtr<FJsonObject> JsonRoot;
	FString JsonFile;
	FString ResolvedJsonFile;
	JsonTryGetString(Ctx.Params, TEXT("json_file"), JsonFile);
	JsonFile.TrimStartAndEndInline();
	if (!JsonFile.IsEmpty())
	{
		if (!UeAgentEnhancedInputOps::LoadJsonFile(JsonFile, JsonRoot, ResolvedJsonFile, OutError))
		{
			return false;
		}
	}

	const TSharedPtr<FJsonObject> SourceRoot = JsonRoot.IsValid() ? JsonRoot : Ctx.Params;
	if (!SourceRoot.IsValid())
	{
		OutError = TEXT("missing_json_payload");
		return false;
	}

	FString AssetPathInput;
	JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput);
	AssetPathInput.TrimStartAndEndInline();
	if (AssetPathInput.IsEmpty())
	{
		SourceRoot->TryGetStringField(TEXT("asset_path"), AssetPathInput);
	}
	if (AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	UInputAction* InputAction = UeAgentEnhancedInputOps::LoadInputAction(AssetPathInput);
	bool bCreated = false;
	bool bCreateIfMissing = true;
	JsonTryGetBool(Ctx.Params, TEXT("create_if_missing"), bCreateIfMissing);
	if (!InputAction)
	{
		if (!bCreateIfMissing)
		{
			OutError = TEXT("input_action_not_found");
			return false;
		}

		if (!UeAgentEnhancedInputOps::CreateInputActionAsset(AssetPathInput, InputAction, OutError) || !InputAction)
		{
			return false;
		}
		bCreated = true;
	}

	InputAction->Modify();

	FString ValueTypeText;
	if (SourceRoot->TryGetStringField(TEXT("value_type"), ValueTypeText) && !ValueTypeText.IsEmpty())
	{
		EInputActionValueType ParsedValueType = EInputActionValueType::Boolean;
		if (!UeAgentEnhancedInputOps::ParseValueType(ValueTypeText, ParsedValueType))
		{
			OutError = TEXT("invalid_value_type");
			return false;
		}
		InputAction->ValueType = ParsedValueType;
	}

	bool bBoolValue = false;
	if (SourceRoot->TryGetBoolField(TEXT("consume_input"), bBoolValue))
	{
		InputAction->bConsumeInput = bBoolValue;
	}
	if (SourceRoot->TryGetBoolField(TEXT("trigger_when_paused"), bBoolValue))
	{
		InputAction->bTriggerWhenPaused = bBoolValue;
	}
	if (SourceRoot->TryGetBoolField(TEXT("reserve_all_mappings"), bBoolValue))
	{
		InputAction->bReserveAllMappings = bBoolValue;
	}

	const TArray<TSharedPtr<FJsonValue>>* ModifierClasses = nullptr;
	if (SourceRoot->TryGetArrayField(TEXT("modifier_classes"), ModifierClasses) && ModifierClasses)
	{
		if (!UeAgentEnhancedInputOps::PopulateActionModifiersFromClassArray(*ModifierClasses, InputAction, InputAction->Modifiers, OutError))
		{
			return false;
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* TriggerClasses = nullptr;
	if (SourceRoot->TryGetArrayField(TEXT("trigger_classes"), TriggerClasses) && TriggerClasses)
	{
		if (!UeAgentEnhancedInputOps::PopulateActionTriggersFromClassArray(*TriggerClasses, InputAction, InputAction->Triggers, OutError))
		{
			return false;
		}
	}

	InputAction->MarkPackageDirty();
	InputAction->PostEditChange();

	bool bSaveAfterApply = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_apply"), bSaveAfterApply);
	if (bSaveAfterApply && !UeAgentEnhancedInputOps::SaveAssetPackage(InputAction, OutError))
	{
		return false;
	}

	OutData = UeAgentEnhancedInputOps::ExportActionJson(InputAction);
	OutData->SetBoolField(TEXT("created"), bCreated);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterApply);
	if (!ResolvedJsonFile.IsEmpty())
	{
		OutData->SetStringField(TEXT("json_file"), ResolvedJsonFile);
	}
	return true;
}

bool FUeAgentHttpServer::CmdEnhancedInputCreateMappingContext(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	UInputMappingContext* MappingContext = nullptr;
	if (!UeAgentEnhancedInputOps::CreateMappingContextAsset(AssetPathInput, MappingContext, OutError) || !MappingContext)
	{
		return false;
	}

	bool bSaveAfterCreate = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_create"), bSaveAfterCreate);
	if (bSaveAfterCreate && !UeAgentEnhancedInputOps::SaveAssetPackage(MappingContext, OutError))
	{
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), UeAgentEnhancedInputOps::NormalizeAssetPath(MappingContext->GetOutermost()->GetName()));
	OutData->SetStringField(TEXT("object_path"), MappingContext->GetPathName());
	OutData->SetNumberField(TEXT("mapping_count"), MappingContext->GetMappings().Num());
	OutData->SetBoolField(TEXT("saved"), bSaveAfterCreate);

	FUeAgentInterfaceLogger::Log(TEXT("EnhancedInput: created mapping context %s (saved=%s)"), *UeAgentEnhancedInputOps::NormalizeAssetPath(MappingContext->GetOutermost()->GetName()), bSaveAfterCreate ? TEXT("true") : TEXT("false"));
	return true;
}

bool FUeAgentHttpServer::CmdEnhancedInputGetMappingContextInfo(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	UInputMappingContext* MappingContext = UeAgentEnhancedInputOps::LoadInputMappingContext(AssetPathInput);
	if (!MappingContext)
	{
		OutError = TEXT("mapping_context_not_found");
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), MappingContext->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("object_path"), MappingContext->GetPathName());
	OutData->SetBoolField(TEXT("dirty"), MappingContext->GetOutermost()->IsDirty());

	const TArray<FEnhancedActionKeyMapping>& Mappings = MappingContext->GetMappings();
	TArray<TSharedPtr<FJsonValue>> MappingsArray;
	for (int32 MappingIndex = 0; MappingIndex < Mappings.Num(); ++MappingIndex)
	{
		MappingsArray.Add(MakeShared<FJsonValueObject>(UeAgentEnhancedInputOps::MappingToJson(Mappings[MappingIndex], MappingIndex)));
	}
	OutData->SetArrayField(TEXT("mappings"), MappingsArray);
	OutData->SetNumberField(TEXT("mapping_count"), Mappings.Num());
	return true;
}

bool FUeAgentHttpServer::CmdEnhancedInputSetMappingContextProperty(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}
	FString PropertyName;
	if (!JsonTryGetString(Ctx.Params, TEXT("property_name"), PropertyName) || PropertyName.IsEmpty())
	{
		OutError = TEXT("missing_property_name");
		return false;
	}
	FString ValueText;
	if (!JsonTryGetString(Ctx.Params, TEXT("value_text"), ValueText))
	{
		OutError = TEXT("missing_value_text");
		return false;
	}

	UInputMappingContext* MappingContext = UeAgentEnhancedInputOps::LoadInputMappingContext(AssetPathInput);
	if (!MappingContext)
	{
		OutError = TEXT("mapping_context_not_found");
		return false;
	}

	FProperty* Property = nullptr;
	void* ValuePtr = nullptr;
	if (!UeAgentEnhancedInputOps::ResolvePropertyPath(MappingContext, PropertyName, Property, ValuePtr) || !Property || !ValuePtr)
	{
		OutError = TEXT("property_not_found");
		return false;
	}

	MappingContext->Modify();
	if (!Property->ImportText_Direct(*ValueText, ValuePtr, MappingContext, PPF_None))
	{
		const FString ImportError = FString::Printf(TEXT("property_import_failed:%s:%s"), *PropertyName, *ValueText);
		OutData->SetStringField(TEXT("property_name"), PropertyName);
		OutData->SetStringField(TEXT("value_text"), ValueText);
		SetPropertyImportResultFields(OutData, Property, ValueText, TEXT(""), TEXT("import_failed"), ImportError);
		OutError = ImportError;
		return false;
	}

	MappingContext->MarkPackageDirty();
	MappingContext->PostEditChange();

	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (bSaveAfterSet && !UeAgentEnhancedInputOps::SaveAssetPackage(MappingContext, OutError))
	{
		return false;
	}

	FString ExportedValue;
	Property->ExportTextItem_Direct(ExportedValue, ValuePtr, nullptr, MappingContext, PPF_None);
	OutData->SetStringField(TEXT("asset_path"), MappingContext->GetOutermost()->GetName());
	OutData->SetStringField(TEXT("property_name"), PropertyName);
	OutData->SetStringField(TEXT("value_text"), ValueText);
	OutData->SetStringField(TEXT("applied_value_text"), ExportedValue);
	SetPropertyImportResultFields(OutData, Property, ValueText, ExportedValue, TEXT("imported_and_read_back"));
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentHttpServer::CmdEnhancedInputAddMapping(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString ContextPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("mapping_context_path"), ContextPath) || ContextPath.IsEmpty())
	{
		OutError = TEXT("missing_mapping_context_path");
		return false;
	}
	FString ActionPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("action_path"), ActionPath) || ActionPath.IsEmpty())
	{
		OutError = TEXT("missing_action_path");
		return false;
	}
	FString KeyText;
	if (!JsonTryGetString(Ctx.Params, TEXT("key"), KeyText) || KeyText.IsEmpty())
	{
		OutError = TEXT("missing_key");
		return false;
	}

	UInputMappingContext* MappingContext = UeAgentEnhancedInputOps::LoadInputMappingContext(ContextPath);
	if (!MappingContext)
	{
		OutError = TEXT("mapping_context_not_found");
		return false;
	}
	UInputAction* InputAction = UeAgentEnhancedInputOps::LoadInputAction(ActionPath);
	if (!InputAction)
	{
		OutError = TEXT("input_action_not_found");
		return false;
	}

	FKey InputKey;
	if (!UeAgentEnhancedInputOps::ResolveInputKey(KeyText, InputKey))
	{
		OutError = TEXT("invalid_key");
		return false;
	}

	MappingContext->Modify();
	FEnhancedActionKeyMapping& Mapping = MappingContext->MapKey(InputAction, InputKey);

	const TArray<TSharedPtr<FJsonValue>>* ModifierClassArray = nullptr;
	if (Ctx.Params.IsValid() && Ctx.Params->TryGetArrayField(TEXT("modifier_classes"), ModifierClassArray) && ModifierClassArray)
	{
		Mapping.Modifiers.Reset();
		for (const TSharedPtr<FJsonValue>& ModifierClassValue : *ModifierClassArray)
		{
			FString ModifierClassPath;
			if (!ModifierClassValue.IsValid() || !ModifierClassValue->TryGetString(ModifierClassPath))
			{
				OutError = TEXT("invalid_modifier_class_item");
				return false;
			}

			UClass* ModifierClass = UeAgentEnhancedInputOps::ResolveClassByPath(ModifierClassPath);
			if (!ModifierClass || !ModifierClass->IsChildOf(UInputModifier::StaticClass()))
			{
				OutError = TEXT("invalid_modifier_class");
				return false;
			}

			UInputModifier* Modifier = NewObject<UInputModifier>(MappingContext, ModifierClass, NAME_None, RF_Transactional);
			if (!Modifier)
			{
				OutError = TEXT("create_modifier_failed");
				return false;
			}
			Mapping.Modifiers.Add(Modifier);
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* TriggerClassArray = nullptr;
	if (Ctx.Params.IsValid() && Ctx.Params->TryGetArrayField(TEXT("trigger_classes"), TriggerClassArray) && TriggerClassArray)
	{
		Mapping.Triggers.Reset();
		for (const TSharedPtr<FJsonValue>& TriggerClassValue : *TriggerClassArray)
		{
			FString TriggerClassPath;
			if (!TriggerClassValue.IsValid() || !TriggerClassValue->TryGetString(TriggerClassPath))
			{
				OutError = TEXT("invalid_trigger_class_item");
				return false;
			}

			UClass* TriggerClass = UeAgentEnhancedInputOps::ResolveClassByPath(TriggerClassPath);
			if (!TriggerClass || !TriggerClass->IsChildOf(UInputTrigger::StaticClass()))
			{
				OutError = TEXT("invalid_trigger_class");
				return false;
			}

			UInputTrigger* Trigger = NewObject<UInputTrigger>(MappingContext, TriggerClass, NAME_None, RF_Transactional);
			if (!Trigger)
			{
				OutError = TEXT("create_trigger_failed");
				return false;
			}
			Mapping.Triggers.Add(Trigger);
		}
	}

	MappingContext->MarkPackageDirty();
	MappingContext->PostEditChange();

	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (bSaveAfterSet && !UeAgentEnhancedInputOps::SaveAssetPackage(MappingContext, OutError))
	{
		return false;
	}

	const int32 MappingIndex = MappingContext->GetMappings().Num() - 1;
	OutData->SetStringField(TEXT("asset_path"), MappingContext->GetOutermost()->GetName());
	OutData->SetObjectField(TEXT("mapping"), UeAgentEnhancedInputOps::MappingToJson(Mapping, MappingIndex));
	OutData->SetNumberField(TEXT("mapping_count"), MappingContext->GetMappings().Num());
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);

	FUeAgentInterfaceLogger::Log(TEXT("EnhancedInput: added mapping context=%s action=%s key=%s"),
		*MappingContext->GetOutermost()->GetName(),
		*InputAction->GetOutermost()->GetName(),
		*InputKey.ToString());
	return true;
}

bool FUeAgentHttpServer::CmdEnhancedInputRemoveMapping(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString ContextPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("mapping_context_path"), ContextPath) || ContextPath.IsEmpty())
	{
		OutError = TEXT("missing_mapping_context_path");
		return false;
	}
	FString ActionPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("action_path"), ActionPath) || ActionPath.IsEmpty())
	{
		OutError = TEXT("missing_action_path");
		return false;
	}

	UInputMappingContext* MappingContext = UeAgentEnhancedInputOps::LoadInputMappingContext(ContextPath);
	if (!MappingContext)
	{
		OutError = TEXT("mapping_context_not_found");
		return false;
	}
	UInputAction* InputAction = UeAgentEnhancedInputOps::LoadInputAction(ActionPath);
	if (!InputAction)
	{
		OutError = TEXT("input_action_not_found");
		return false;
	}

	MappingContext->Modify();

	FString KeyText;
	const bool bHasKey = JsonTryGetString(Ctx.Params, TEXT("key"), KeyText) && !KeyText.IsEmpty();
	if (bHasKey)
	{
		FKey InputKey;
		if (!UeAgentEnhancedInputOps::ResolveInputKey(KeyText, InputKey))
		{
			OutError = TEXT("invalid_key");
			return false;
		}
		MappingContext->UnmapKey(InputAction, InputKey);
	}
	else
	{
		MappingContext->UnmapAllKeysFromAction(InputAction);
	}

	MappingContext->MarkPackageDirty();
	MappingContext->PostEditChange();

	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (bSaveAfterSet && !UeAgentEnhancedInputOps::SaveAssetPackage(MappingContext, OutError))
	{
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), MappingContext->GetOutermost()->GetName());
	OutData->SetNumberField(TEXT("mapping_count"), MappingContext->GetMappings().Num());
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentHttpServer::CmdEnhancedInputClearMappings(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString ContextPath;
	if (!JsonTryGetString(Ctx.Params, TEXT("mapping_context_path"), ContextPath) || ContextPath.IsEmpty())
	{
		OutError = TEXT("missing_mapping_context_path");
		return false;
	}

	UInputMappingContext* MappingContext = UeAgentEnhancedInputOps::LoadInputMappingContext(ContextPath);
	if (!MappingContext)
	{
		OutError = TEXT("mapping_context_not_found");
		return false;
	}

	MappingContext->Modify();
	MappingContext->UnmapAll();
	MappingContext->MarkPackageDirty();
	MappingContext->PostEditChange();

	bool bSaveAfterSet = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_set"), bSaveAfterSet);
	if (bSaveAfterSet && !UeAgentEnhancedInputOps::SaveAssetPackage(MappingContext, OutError))
	{
		return false;
	}

	OutData->SetStringField(TEXT("asset_path"), MappingContext->GetOutermost()->GetName());
	OutData->SetNumberField(TEXT("mapping_count"), MappingContext->GetMappings().Num());
	OutData->SetBoolField(TEXT("saved"), bSaveAfterSet);
	return true;
}

bool FUeAgentHttpServer::CmdEnhancedInputExportMappingContextJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	FString AssetPathInput;
	if (!JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput) || AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	UInputMappingContext* MappingContext = UeAgentEnhancedInputOps::LoadInputMappingContext(AssetPathInput);
	if (!MappingContext)
	{
		OutError = TEXT("mapping_context_not_found");
		return false;
	}

	OutData = UeAgentEnhancedInputOps::ExportMappingContextJson(MappingContext);

	FString OutputFile;
	JsonTryGetString(Ctx.Params, TEXT("output_file"), OutputFile);
	OutputFile.TrimStartAndEndInline();
	FString SavedOutputFile;
	if (!OutputFile.IsEmpty())
	{
		if (!UeAgentEnhancedInputOps::SaveJsonFile(OutputFile, OutData, SavedOutputFile, OutError))
		{
			return false;
		}
		OutData->SetStringField(TEXT("output_file"), SavedOutputFile);
	}

	return true;
}

bool FUeAgentHttpServer::CmdEnhancedInputApplyMappingContextJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	TSharedPtr<FJsonObject> JsonRoot;
	FString JsonFile;
	FString ResolvedJsonFile;
	JsonTryGetString(Ctx.Params, TEXT("json_file"), JsonFile);
	JsonFile.TrimStartAndEndInline();
	if (!JsonFile.IsEmpty())
	{
		if (!UeAgentEnhancedInputOps::LoadJsonFile(JsonFile, JsonRoot, ResolvedJsonFile, OutError))
		{
			return false;
		}
	}

	const TSharedPtr<FJsonObject> SourceRoot = JsonRoot.IsValid() ? JsonRoot : Ctx.Params;
	if (!SourceRoot.IsValid())
	{
		OutError = TEXT("missing_json_payload");
		return false;
	}

	FString AssetPathInput;
	JsonTryGetString(Ctx.Params, TEXT("asset_path"), AssetPathInput);
	AssetPathInput.TrimStartAndEndInline();
	if (AssetPathInput.IsEmpty())
	{
		SourceRoot->TryGetStringField(TEXT("asset_path"), AssetPathInput);
	}
	if (AssetPathInput.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	UInputMappingContext* MappingContext = UeAgentEnhancedInputOps::LoadInputMappingContext(AssetPathInput);
	bool bCreated = false;
	bool bCreateIfMissing = true;
	JsonTryGetBool(Ctx.Params, TEXT("create_if_missing"), bCreateIfMissing);
	if (!MappingContext)
	{
		if (!bCreateIfMissing)
		{
			OutError = TEXT("mapping_context_not_found");
			return false;
		}

		if (!UeAgentEnhancedInputOps::CreateMappingContextAsset(AssetPathInput, MappingContext, OutError) || !MappingContext)
		{
			return false;
		}
		bCreated = true;
	}

	const TArray<TSharedPtr<FJsonValue>>* MappingsArray = nullptr;
	const bool bHasMappings = SourceRoot->TryGetArrayField(TEXT("mappings"), MappingsArray) && MappingsArray;

	MappingContext->Modify();
	if (bHasMappings)
	{
		MappingContext->UnmapAll();

		for (const TSharedPtr<FJsonValue>& MappingValue : *MappingsArray)
		{
			const TSharedPtr<FJsonObject>* MappingObjPtr = nullptr;
			if (!MappingValue.IsValid() || !MappingValue->TryGetObject(MappingObjPtr) || !MappingObjPtr || !MappingObjPtr->IsValid())
			{
				OutError = TEXT("invalid_mapping_item");
				return false;
			}

			const TSharedPtr<FJsonObject>& MappingObj = *MappingObjPtr;
			FString ActionPath;
			if (!MappingObj->TryGetStringField(TEXT("action_path"), ActionPath) || ActionPath.IsEmpty())
			{
				OutError = TEXT("missing_mapping_action_path");
				return false;
			}

			FString KeyText;
			if (!MappingObj->TryGetStringField(TEXT("key"), KeyText) || KeyText.IsEmpty())
			{
				OutError = TEXT("missing_mapping_key");
				return false;
			}

			UInputAction* InputAction = UeAgentEnhancedInputOps::LoadInputAction(ActionPath);
			if (!InputAction)
			{
				OutError = TEXT("mapping_action_not_found");
				return false;
			}

			FKey InputKey;
			if (!UeAgentEnhancedInputOps::ResolveInputKey(KeyText, InputKey))
			{
				OutError = TEXT("invalid_key");
				return false;
			}

			FEnhancedActionKeyMapping& Mapping = MappingContext->MapKey(InputAction, InputKey);

			const TArray<TSharedPtr<FJsonValue>>* ModifierClasses = nullptr;
			if (MappingObj->TryGetArrayField(TEXT("modifier_classes"), ModifierClasses) && ModifierClasses)
			{
				if (!UeAgentEnhancedInputOps::PopulateActionModifiersFromClassArray(*ModifierClasses, MappingContext, Mapping.Modifiers, OutError))
				{
					return false;
				}
			}

			const TArray<TSharedPtr<FJsonValue>>* TriggerClasses = nullptr;
			if (MappingObj->TryGetArrayField(TEXT("trigger_classes"), TriggerClasses) && TriggerClasses)
			{
				if (!UeAgentEnhancedInputOps::PopulateActionTriggersFromClassArray(*TriggerClasses, MappingContext, Mapping.Triggers, OutError))
				{
					return false;
				}
			}
		}
	}

	MappingContext->MarkPackageDirty();
	MappingContext->PostEditChange();

	bool bSaveAfterApply = false;
	JsonTryGetBool(Ctx.Params, TEXT("save_after_apply"), bSaveAfterApply);
	if (bSaveAfterApply && !UeAgentEnhancedInputOps::SaveAssetPackage(MappingContext, OutError))
	{
		return false;
	}

	OutData = UeAgentEnhancedInputOps::ExportMappingContextJson(MappingContext);
	OutData->SetBoolField(TEXT("created"), bCreated);
	OutData->SetBoolField(TEXT("saved"), bSaveAfterApply);
	if (!ResolvedJsonFile.IsEmpty())
	{
		OutData->SetStringField(TEXT("json_file"), ResolvedJsonFile);
	}
	return true;
}

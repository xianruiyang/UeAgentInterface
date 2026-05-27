// Copyright Epic Games, Inc. All Rights Reserved.

#include "Server/UeAgentHttpServer.h"

#include "Module/UeAgentInterfaceLogger.h"
#include "Utilities/UeAgentJsonDiagnostics.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Async/TaskGraphInterfaces.h"
#include "EdGraph/EdGraph.h"
#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "FileHelpers.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "PCGAssetExporter.h"
#include "PCGComponent.h"
#include "PCGDataAsset.h"
#include "PCGEdge.h"
#include "PCGGraph.h"
#include "PCGGraphExecutionInspection.h"
#include "PCGLevelToAsset.h"
#include "PCGNode.h"
#include "PCGPin.h"
#include "PCGSettings.h"
#include "PCGSubsystem.h"
#include "PCGWorldActor.h"
#include "Data/PCGBasePointData.h"
#include "Elements/PCGExecuteBlueprint.h"
#include "Elements/PCGStaticMeshSpawner.h"
#include "Elements/PCGUserParameterGet.h"
#include "Grid/PCGPartitionActor.h"
#include "MeshSelectors/PCGMeshSelectorBase.h"
#include "RHI.h"
#include "ScopedTransaction.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "StructUtils/PropertyBag.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"

namespace UeAgentPCG
{
	static constexpr const TCHAR* SchemaCapabilities = TEXT("ue_agent_interface.pcg.capabilities.v1");
	static constexpr const TCHAR* SchemaCatalog = TEXT("ue_agent_interface.pcg.node_catalog.v1");
	static constexpr const TCHAR* SchemaFolder = TEXT("ue_agent_interface.pcg.graph_folder.v1");
	static constexpr const TCHAR* SchemaComponent = TEXT("ue_agent_interface.pcg.component.v1");
	static constexpr const TCHAR* SchemaData = TEXT("ue_agent_interface.pcg.data.v1");
	static constexpr const TCHAR* InputNodeId = TEXT("__input__");
	static constexpr const TCHAR* OutputNodeId = TEXT("__output__");

	static FString NormalizeAssetPath(const FString& InPath)
	{
		FString OutPath = InPath;
		OutPath.TrimStartAndEndInline();
		OutPath.ReplaceInline(TEXT("\\"), TEXT("/"));
		int32 DotIndex = INDEX_NONE;
		if (OutPath.FindChar(TEXT('.'), DotIndex) && DotIndex > 0)
		{
			OutPath = OutPath.Left(DotIndex);
		}
		return OutPath;
	}

	static FString ToObjectPath(const FString& InPath)
	{
		FString ObjectPath = InPath;
		ObjectPath.TrimStartAndEndInline();
		ObjectPath.ReplaceInline(TEXT("\\"), TEXT("/"));
		if (ObjectPath.IsEmpty())
		{
			return FString();
		}
		if (ObjectPath.Contains(TEXT(".")))
		{
			return ObjectPath;
		}
		const FString AssetPath = NormalizeAssetPath(ObjectPath);
		const FString AssetName = FPackageName::GetLongPackageAssetName(AssetPath);
		return AssetName.IsEmpty() ? FString() : FString::Printf(TEXT("%s.%s"), *AssetPath, *AssetName);
	}

	static FString ResolveFolderPath(const FString& InPath)
	{
		FString Folder = InPath;
		Folder.TrimStartAndEndInline();
		Folder.ReplaceInline(TEXT("\\"), TEXT("/"));
		return UeAgentJsonDiagnostics::ResolveProjectRelativeFilePath(Folder);
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
		const FString PackagePath = NormalizeAssetPath(InPath);
		if (!FPackageName::DoesPackageExist(PackagePath))
		{
			return nullptr;
		}
		return LoadObject<UObject>(nullptr, *ObjectPath);
	}

	template<typename TObjectType>
	static TObjectType* LoadAsset(const FString& InPath)
	{
		return Cast<TObjectType>(LoadAssetObject(InPath));
	}

	static bool SavePackagesForObjects(const TArray<UObject*>& Objects, FString& OutError)
	{
		TArray<UPackage*> PackagesToSave;
		for (UObject* Object : Objects)
		{
			if (Object && Object->GetOutermost())
			{
				PackagesToSave.AddUnique(Object->GetOutermost());
			}
		}
		if (PackagesToSave.Num() > 0 && !UEditorLoadingAndSavingUtils::SavePackages(PackagesToSave, false))
		{
			OutError = TEXT("save_package_failed");
			return false;
		}
		return true;
	}

	static bool SaveJsonFile(const FString& OutputPathInput, const TSharedPtr<FJsonObject>& Root, FString& OutOutputPath, FString& OutError)
	{
		OutOutputPath.Reset();
		if (!Root.IsValid())
		{
			OutError = TEXT("json_root_invalid");
			return false;
		}

		FString OutputPath = UeAgentJsonDiagnostics::ResolveProjectRelativeFilePath(OutputPathInput);
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

	static bool SaveTrackedJson(
		const FString& FolderPath,
		const FString& RelativePath,
		const TSharedPtr<FJsonObject>& Root,
		TArray<TSharedPtr<FJsonValue>>& OutFiles,
		FString& OutError)
	{
		FString SavedPath;
		if (!SaveJsonFile(FPaths::Combine(FolderPath, RelativePath), Root, SavedPath, OutError))
		{
			return false;
		}
		TSharedPtr<FJsonObject> FileObj = MakeShared<FJsonObject>();
		FileObj->SetStringField(TEXT("path"), SavedPath);
		FileObj->SetStringField(TEXT("relative_path"), RelativePath.Replace(TEXT("\\"), TEXT("/")));
		OutFiles.Add(MakeShared<FJsonValueObject>(FileObj));
		return true;
	}

	static bool PCGGraphHasMaterializedEditorGraph(const UPCGGraph* Graph)
	{
		if (!Graph)
		{
			return false;
		}
		TArray<UObject*> ChildObjects;
		GetObjectsWithOuter(Graph, ChildObjects, true);
		for (UObject* Child : ChildObjects)
		{
			if (UEdGraph* EditorGraph = Cast<UEdGraph>(Child))
			{
				if (EditorGraph->Nodes.Num() > 0)
				{
					return true;
				}
			}
		}
		return false;
	}

	static void PumpSlateForPCGEditorGraphInit(const int32 FrameCount = 4)
	{
		if (!FSlateApplication::IsInitialized())
		{
			return;
		}
		for (int32 FrameIndex = 0; FrameIndex < FrameCount; ++FrameIndex)
		{
			FSlateApplication::Get().PumpMessages();
			FSlateApplication::Get().Tick();
			FPlatformProcess::Sleep(0.01f);
		}
	}

	static bool EnsurePCGEditorGraphMaterialized(UPCGGraph* Graph, bool& bOutOpenedEditor)
	{
		bOutOpenedEditor = false;
		if (!Graph)
		{
			return false;
		}
		if (PCGGraphHasMaterializedEditorGraph(Graph))
		{
			return true;
		}
		if (!GEditor)
		{
			return false;
		}

		UAssetEditorSubsystem* AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
		if (!AssetEditorSubsystem)
		{
			return false;
		}
		if (!AssetEditorSubsystem->FindEditorForAsset(Graph, false))
		{
			if (!AssetEditorSubsystem->OpenEditorForAsset(Graph))
			{
				return false;
			}
			bOutOpenedEditor = true;
		}
		PumpSlateForPCGEditorGraphInit(8);
		return PCGGraphHasMaterializedEditorGraph(Graph);
	}

	static bool LoadJsonFile(const FString& FilePath, TSharedPtr<FJsonObject>& OutRoot, FString& OutError)
	{
		TArray<TSharedPtr<FJsonValue>> Issues;
		return UeAgentJsonDiagnostics::LoadJsonObjectFile(FilePath, OutRoot, &Issues, OutError, false);
	}

	static TSharedPtr<FJsonObject> MakeIssue(const FString& Severity, const FString& Code, const FString& Path, const FString& Message)
	{
		return UeAgentJsonDiagnostics::MakeIssue(Severity, Code, Path, Message);
	}

	static void AddIssue(TArray<TSharedPtr<FJsonValue>>& Issues, const FString& Severity, const FString& Code, const FString& Path, const FString& Message)
	{
		Issues.Add(MakeShared<FJsonValueObject>(MakeIssue(Severity, Code, Path, Message)));
	}

	static int32 CountErrors(const TArray<TSharedPtr<FJsonValue>>& Issues)
	{
		return UeAgentJsonDiagnostics::CountIssuesBySeverity(Issues, TEXT("error"));
	}

	static FString EnumValueToString(const UEnum* Enum, int64 Value)
	{
		if (!Enum)
		{
			return FString::FromInt(Value);
		}
		return Enum->GetNameStringByValue(Value);
	}

	static TSharedPtr<FJsonObject> VecToJson(const FVector& V)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("x"), V.X);
		Obj->SetNumberField(TEXT("y"), V.Y);
		Obj->SetNumberField(TEXT("z"), V.Z);
		return Obj;
	}

	static TSharedPtr<FJsonObject> RotToJson(const FRotator& R)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("pitch"), R.Pitch);
		Obj->SetNumberField(TEXT("yaw"), R.Yaw);
		Obj->SetNumberField(TEXT("roll"), R.Roll);
		return Obj;
	}

	static FString JsonValueToImportText(const TSharedPtr<FJsonValue>& Value)
	{
		if (!Value.IsValid() || Value->Type == EJson::Null)
		{
			return TEXT("None");
		}
		FString StringValue;
		if (Value->TryGetString(StringValue))
		{
			return StringValue;
		}
		double NumberValue = 0.0;
		if (Value->TryGetNumber(NumberValue))
		{
			return FString::SanitizeFloat(NumberValue);
		}
		bool BoolValue = false;
		if (Value->TryGetBool(BoolValue))
		{
			return BoolValue ? TEXT("True") : TEXT("False");
		}

		FString JsonText;
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&JsonText);
		FJsonSerializer::Serialize(Value, FString(), Writer);
		return JsonText;
	}

	static FString ExportPropertyValue(UObject* Object, const FProperty* Property)
	{
		if (!Object || !Property)
		{
			return FString();
		}
		FString ValueText;
		Property->ExportText_InContainer(0, ValueText, Object, Object, Object, PPF_None);
		return ValueText;
	}

	static bool ImportPropertyValue(UObject* Object, FProperty* Property, const FString& ValueText, FString& OutError)
	{
		if (!Object || !Property)
		{
			OutError = TEXT("property_not_found");
			return false;
		}
		const TCHAR* ImportResult = Property->ImportText_InContainer(*ValueText, Object, Object, PPF_None);
		if (!ImportResult)
		{
			OutError = FString::Printf(TEXT("import_failed:%s"), *Property->GetName());
			return false;
		}
		return true;
	}

	static void ExportEditablePropertiesRecursive(UObject* Object, const FString& NamePrefix, int32 RemainingObjectDepth, TSet<const UObject*>& VisitedObjects, TArray<TSharedPtr<FJsonValue>>& Properties)
	{
		if (!Object)
		{
			return;
		}
		if (VisitedObjects.Contains(Object))
		{
			return;
		}
		VisitedObjects.Add(Object);

		for (TFieldIterator<FProperty> It(Object->GetClass()); It; ++It)
		{
			FProperty* Property = *It;
			if (!Property || !Property->HasAnyPropertyFlags(CPF_Edit))
			{
				continue;
			}
			if (Property->HasAnyPropertyFlags(CPF_Deprecated | CPF_Transient | CPF_DisableEditOnInstance))
			{
				continue;
			}

			TSharedPtr<FJsonObject> PropertyObj = MakeShared<FJsonObject>();
			PropertyObj->SetStringField(TEXT("name"), NamePrefix + Property->GetName());
			PropertyObj->SetStringField(TEXT("cpp_type"), Property->GetCPPType());
			PropertyObj->SetStringField(TEXT("value_text"), ExportPropertyValue(Object, Property));
			Properties.Add(MakeShared<FJsonValueObject>(PropertyObj));

			if (RemainingObjectDepth > 0)
			{
				if (const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
				{
					UObject* SubObject = ObjectProperty->GetObjectPropertyValue_InContainer(Object);
					if (SubObject && SubObject->IsIn(Object))
					{
						ExportEditablePropertiesRecursive(SubObject, NamePrefix + Property->GetName() + TEXT("."), RemainingObjectDepth - 1, VisitedObjects, Properties);
					}
				}
			}
		}
	}

	static TArray<TSharedPtr<FJsonValue>> ExportEditableProperties(UObject* Object)
	{
		TArray<TSharedPtr<FJsonValue>> Properties;
		TSet<const UObject*> VisitedObjects;
		ExportEditablePropertiesRecursive(Object, FString(), 1, VisitedObjects, Properties);
		return Properties;
	}

	static bool ResolvePropertyPath(UObject* RootObject, const FString& PropertyPath, UObject*& OutObject, FProperty*& OutProperty, FString& OutError)
	{
		OutObject = RootObject;
		OutProperty = nullptr;
		if (!OutObject)
		{
			OutError = TEXT("property_owner_not_found");
			return false;
		}

		FString Remaining = PropertyPath;
		FString Segment;
		FString Rest;
		while (Remaining.Split(TEXT("."), &Segment, &Rest))
		{
			FProperty* SegmentProperty = OutObject->GetClass()->FindPropertyByName(*Segment);
			const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(SegmentProperty);
			if (!ObjectProperty)
			{
				OutError = FString::Printf(TEXT("nested_property_not_object:%s"), *Segment);
				return false;
			}

			if (UPCGStaticMeshSpawnerSettings* StaticMeshSpawnerSettings = Cast<UPCGStaticMeshSpawnerSettings>(OutObject))
			{
				if (Segment == TEXT("MeshSelectorParameters"))
				{
					StaticMeshSpawnerSettings->SetMeshSelectorType(StaticMeshSpawnerSettings->MeshSelectorType);
				}
				else if (Segment == TEXT("InstanceDataPackerParameters"))
				{
					StaticMeshSpawnerSettings->SetInstancePackerType(StaticMeshSpawnerSettings->InstanceDataPackerType);
				}
			}

			UObject* SubObject = ObjectProperty->GetObjectPropertyValue_InContainer(OutObject);
			if (!SubObject || !SubObject->IsIn(OutObject))
			{
				OutError = FString::Printf(TEXT("nested_property_target_not_instanced:%s"), *Segment);
				return false;
			}

			OutObject = SubObject;
			Remaining = Rest;
		}

		OutProperty = OutObject->GetClass()->FindPropertyByName(*Remaining);
		if (!OutProperty)
		{
			OutError = TEXT("property_not_found");
			return false;
		}
		return true;
	}

	static bool ShouldSkipDirectInstancedSubobjectImport(const FString& PropertyName, const FProperty* Property)
	{
		return Property &&
			!PropertyName.Contains(TEXT(".")) &&
			Property->HasAnyPropertyFlags(CPF_InstancedReference);
	}

	static UClass* ResolveClassImportText(const FString& ValueText)
	{
		FString ClassPath = ValueText;
		ClassPath.TrimStartAndEndInline();
		if (ClassPath.IsEmpty() || ClassPath == TEXT("None"))
		{
			return nullptr;
		}

		FString QuotedPath;
		if (ClassPath.Split(TEXT("'"), nullptr, &QuotedPath, ESearchCase::CaseSensitive) && QuotedPath.EndsWith(TEXT("'")))
		{
			QuotedPath.LeftChopInline(1);
			ClassPath = QuotedPath;
		}

		if (UClass* Class = LoadObject<UClass>(nullptr, *ClassPath))
		{
			return Class;
		}
		return FindObject<UClass>(nullptr, *ClassPath);
	}

	static bool TryApplyPCGSpecialProperty(UObject* Object, const FString& PropertyName, const FString& ValueText, bool& bOutHandled, FString& OutError)
	{
		bOutHandled = false;
		if (UPCGBlueprintSettings* BlueprintSettings = Cast<UPCGBlueprintSettings>(Object))
		{
			if (PropertyName == TEXT("BlueprintElementType"))
			{
				bOutHandled = true;
				UClass* ElementClass = ResolveClassImportText(ValueText);
				if (!ElementClass && !ValueText.TrimStartAndEnd().IsEmpty() && ValueText.TrimStartAndEnd() != TEXT("None"))
				{
					OutError = FString::Printf(TEXT("blueprint_element_class_not_found:%s"), *ValueText);
					return false;
				}
				if (ElementClass && !ElementClass->IsChildOf(UPCGBlueprintElement::StaticClass()))
				{
					OutError = FString::Printf(TEXT("blueprint_element_class_invalid:%s"), *ValueText);
					return false;
				}

				BlueprintSettings->Modify();
				UPCGBlueprintElement* ElementInstance = nullptr;
				BlueprintSettings->SetElementType(ElementClass, ElementInstance);
				if (ElementClass && !ElementInstance)
				{
					OutError = FString::Printf(TEXT("blueprint_element_instance_not_created:%s"), *ValueText);
					return false;
				}
				return true;
			}
		}
		if (UPCGStaticMeshSpawnerSettings* StaticMeshSpawnerSettings = Cast<UPCGStaticMeshSpawnerSettings>(Object))
		{
			if (PropertyName == GET_MEMBER_NAME_STRING_CHECKED(UPCGStaticMeshSpawnerSettings, MeshSelectorType))
			{
				bOutHandled = true;
				UClass* MeshSelectorClass = ResolveClassImportText(ValueText);
				if (!MeshSelectorClass || !MeshSelectorClass->IsChildOf(UPCGMeshSelectorBase::StaticClass()))
				{
					OutError = FString::Printf(TEXT("mesh_selector_class_not_found:%s"), *ValueText);
					return false;
				}
				StaticMeshSpawnerSettings->SetMeshSelectorType(MeshSelectorClass);
				return true;
			}
		}
		return false;
	}

	static void RefreshPCGImportedPropertySideEffects(UObject* Object, const FString& PropertyName)
	{
		if (UPCGStaticMeshSpawnerSettings* StaticMeshSpawnerSettings = Cast<UPCGStaticMeshSpawnerSettings>(Object))
		{
			if (PropertyName == GET_MEMBER_NAME_STRING_CHECKED(UPCGStaticMeshSpawnerSettings, MeshSelectorType))
			{
				StaticMeshSpawnerSettings->SetMeshSelectorType(StaticMeshSpawnerSettings->MeshSelectorType);
			}
			else if (PropertyName == GET_MEMBER_NAME_STRING_CHECKED(UPCGStaticMeshSpawnerSettings, InstanceDataPackerType))
			{
				StaticMeshSpawnerSettings->SetInstancePackerType(StaticMeshSpawnerSettings->InstanceDataPackerType);
			}
		}
	}

	static FString FirstPinLabelFromNodeJson(const TSharedPtr<FJsonObject>& NodeObj, const TCHAR* PinArrayField)
	{
		const TArray<TSharedPtr<FJsonValue>>* PinValues = nullptr;
		if (!NodeObj.IsValid() || !NodeObj->TryGetArrayField(PinArrayField, PinValues))
		{
			return FString();
		}

		for (const TSharedPtr<FJsonValue>& PinValue : *PinValues)
		{
			const TSharedPtr<FJsonObject>* PinObj = nullptr;
			FString Label;
			if (PinValue.IsValid() &&
				PinValue->TryGetObject(PinObj) &&
				PinObj &&
				PinObj->IsValid() &&
				(*PinObj)->TryGetStringField(TEXT("label"), Label) &&
				!Label.IsEmpty())
			{
				return Label;
			}
		}
		return FString();
	}

	static void ApplyNodeJsonDerivedSettings(UPCGSettings* Settings, const TSharedPtr<FJsonObject>& NodeObj)
	{
		if (UPCGUserParameterGetSettings* UserParameterGetSettings = Cast<UPCGUserParameterGetSettings>(Settings))
		{
			const FString OutputPinLabel = FirstPinLabelFromNodeJson(NodeObj, TEXT("output_pins"));
			if (!OutputPinLabel.IsEmpty())
			{
				UserParameterGetSettings->PropertyName = FName(*OutputPinLabel);
			}
		}
	}

	static int32 ApplyPropertyArray(UObject* Object, const TArray<TSharedPtr<FJsonValue>>* PropertyValues, TArray<TSharedPtr<FJsonValue>>& Issues, const FString& PathPrefix)
	{
		if (!Object || !PropertyValues)
		{
			return 0;
		}

		int32 AppliedCount = 0;
		for (const TSharedPtr<FJsonValue>& Value : *PropertyValues)
		{
			const TSharedPtr<FJsonObject>* PropertyObj = nullptr;
			if (!Value.IsValid() || !Value->TryGetObject(PropertyObj) || !PropertyObj || !PropertyObj->IsValid())
			{
				AddIssue(Issues, TEXT("error"), TEXT("invalid_property_entry"), PathPrefix, TEXT("Property entry must be a JSON object."));
				continue;
			}

			FString PropertyName;
			if (!(*PropertyObj)->TryGetStringField(TEXT("name"), PropertyName) || PropertyName.IsEmpty())
			{
				AddIssue(Issues, TEXT("error"), TEXT("missing_property_name"), PathPrefix, TEXT("Property entry is missing name."));
				continue;
			}

			FString ValueText;
			if (!(*PropertyObj)->TryGetStringField(TEXT("value_text"), ValueText))
			{
				const TSharedPtr<FJsonValue>* RawValue = (*PropertyObj)->Values.Find(TEXT("value"));
				ValueText = RawValue ? JsonValueToImportText(*RawValue) : FString();
			}

			UObject* TargetObject = nullptr;
			FProperty* Property = nullptr;
			FString ImportError;
			bool bHandledSpecialProperty = false;
			if (TryApplyPCGSpecialProperty(Object, PropertyName, ValueText, bHandledSpecialProperty, ImportError))
			{
				AppliedCount++;
				continue;
			}
			if (bHandledSpecialProperty)
			{
				AddIssue(Issues, TEXT("error"), TEXT("property_import_failed"), FString::Printf(TEXT("%s.%s"), *PathPrefix, *PropertyName), ImportError);
				continue;
			}
			if (!ResolvePropertyPath(Object, PropertyName, TargetObject, Property, ImportError))
			{
				AddIssue(Issues, TEXT("error"), TEXT("property_import_failed"), FString::Printf(TEXT("%s.%s"), *PathPrefix, *PropertyName), ImportError);
				continue;
			}
			if (ShouldSkipDirectInstancedSubobjectImport(PropertyName, Property))
			{
				continue;
			}
			if (!ImportPropertyValue(TargetObject, Property, ValueText, ImportError))
			{
				AddIssue(Issues, TEXT("error"), TEXT("property_import_failed"), FString::Printf(TEXT("%s.%s"), *PathPrefix, *PropertyName), ImportError);
				continue;
			}
			RefreshPCGImportedPropertySideEffects(Object, PropertyName);
			AppliedCount++;
		}
		return AppliedCount;
	}

	static bool ShouldSkipPCGGraphSettingsApplyProperty(const FString& PropertyName)
	{
		return PropertyName == TEXT("Nodes") ||
			PropertyName == TEXT("InputNode") ||
			PropertyName.StartsWith(TEXT("InputNode.")) ||
			PropertyName == TEXT("OutputNode") ||
			PropertyName.StartsWith(TEXT("OutputNode."));
	}

	static int32 ApplyGraphSettingsPropertyArray(UPCGGraph* Graph, const TArray<TSharedPtr<FJsonValue>>* PropertyValues, TArray<TSharedPtr<FJsonValue>>& Issues, const FString& PathPrefix)
	{
		if (!Graph || !PropertyValues)
		{
			return 0;
		}

		TArray<TSharedPtr<FJsonValue>> FilteredValues;
		FilteredValues.Reserve(PropertyValues->Num());
		for (const TSharedPtr<FJsonValue>& Value : *PropertyValues)
		{
			const TSharedPtr<FJsonObject>* PropertyObj = nullptr;
			FString PropertyName;
			if (Value.IsValid() &&
				Value->TryGetObject(PropertyObj) &&
				PropertyObj &&
				PropertyObj->IsValid() &&
				(*PropertyObj)->TryGetStringField(TEXT("name"), PropertyName) &&
				ShouldSkipPCGGraphSettingsApplyProperty(PropertyName))
			{
				continue;
			}

			FilteredValues.Add(Value);
		}

		return ApplyPropertyArray(Graph, &FilteredValues, Issues, PathPrefix);
	}

	static TSharedPtr<FJsonObject> PinPropertiesToJson(const FPCGPinProperties& PinProperties)
	{
		TSharedPtr<FJsonObject> PinObj = MakeShared<FJsonObject>();
		PinObj->SetStringField(TEXT("label"), PinProperties.Label.ToString());
		PinObj->SetStringField(TEXT("allowed_types"), EnumValueToString(StaticEnum<EPCGDataType>(), static_cast<int64>(PinProperties.AllowedTypes)));
		PinObj->SetStringField(TEXT("usage"), EnumValueToString(StaticEnum<EPCGPinUsage>(), static_cast<int64>(PinProperties.Usage)));
		PinObj->SetStringField(TEXT("status"), EnumValueToString(StaticEnum<EPCGPinStatus>(), static_cast<int64>(PinProperties.PinStatus)));
		PinObj->SetBoolField(TEXT("allow_multiple_data"), PinProperties.bAllowMultipleData);
		PinObj->SetBoolField(TEXT("allow_multiple_connections"), PinProperties.AllowsMultipleConnections());
		PinObj->SetBoolField(TEXT("advanced"), PinProperties.IsAdvancedPin());
		PinObj->SetBoolField(TEXT("required"), PinProperties.IsRequiredPin());
		return PinObj;
	}

	static TArray<TSharedPtr<FJsonValue>> PinsToJson(const TArray<FPCGPinProperties>& Pins)
	{
		TArray<TSharedPtr<FJsonValue>> PinArray;
		for (const FPCGPinProperties& Pin : Pins)
		{
			PinArray.Add(MakeShared<FJsonValueObject>(PinPropertiesToJson(Pin)));
		}
		return PinArray;
	}

	static UClass* ResolvePCGSettingsClass(const FString& ClassPathInput)
	{
		FString ClassPath = ClassPathInput;
		ClassPath.TrimStartAndEndInline();
		if (ClassPath.IsEmpty())
		{
			return nullptr;
		}

		if (UClass* LoadedClass = LoadObject<UClass>(nullptr, *ClassPath))
		{
			if (LoadedClass->IsChildOf(UPCGSettings::StaticClass()))
			{
				return LoadedClass;
			}
		}

		for (TObjectIterator<UClass> It; It; ++It)
		{
			UClass* Class = *It;
			if (!Class || !Class->IsChildOf(UPCGSettings::StaticClass()))
			{
				continue;
			}
			if (Class->GetPathName() == ClassPath || Class->GetName() == ClassPath || Class->GetFullName() == ClassPath)
			{
				return Class;
			}
		}
		return nullptr;
	}

	static TSharedPtr<FJsonObject> SettingsClassToCatalogJson(UClass* Class)
	{
		TSharedPtr<FJsonObject> NodeObj = MakeShared<FJsonObject>();
		if (!Class)
		{
			return NodeObj;
		}

		const UPCGSettings* CDO = Class->GetDefaultObject<UPCGSettings>();
		NodeObj->SetStringField(TEXT("class_path"), Class->GetPathName());
		NodeObj->SetStringField(TEXT("class_name"), Class->GetName());
		NodeObj->SetStringField(TEXT("module"), Class->GetOutermost() ? Class->GetOutermost()->GetName() : FString());
		NodeObj->SetBoolField(TEXT("abstract"), Class->HasAnyClassFlags(CLASS_Abstract));
		NodeObj->SetBoolField(TEXT("deprecated"), Class->HasAnyClassFlags(CLASS_Deprecated | CLASS_NewerVersionExists));
		if (CDO)
		{
#if WITH_EDITOR
			NodeObj->SetStringField(TEXT("title"), CDO->GetDefaultNodeTitle().ToString());
			NodeObj->SetStringField(TEXT("category"), EnumValueToString(StaticEnum<EPCGSettingsType>(), static_cast<int64>(CDO->GetType())));
			NodeObj->SetBoolField(TEXT("supports_gpu_toggle"), CDO->DisplayExecuteOnGPUSetting());
#endif
			NodeObj->SetBoolField(TEXT("should_execute_on_gpu_default"), CDO->ShouldExecuteOnGPU());
			NodeObj->SetArrayField(TEXT("input_pins"), PinsToJson(CDO->AllInputPinProperties()));
			NodeObj->SetArrayField(TEXT("output_pins"), PinsToJson(CDO->AllOutputPinProperties()));
		}
		return NodeObj;
	}

	static FString MakeStableNodeId(UPCGGraph* Graph, UPCGNode* Node, TSet<FString>& UsedIds)
	{
		if (!Graph || !Node)
		{
			return FString();
		}
		if (Node == Graph->GetInputNode())
		{
			return InputNodeId;
		}
		if (Node == Graph->GetOutputNode())
		{
			return OutputNodeId;
		}

		FString Base = Node->GetAuthoredTitleName() != NAME_None ? Node->GetAuthoredTitleName().ToString() : Node->GetName();
		Base.TrimStartAndEndInline();
		if (Base.IsEmpty())
		{
			Base = TEXT("node");
		}
		Base.ReplaceInline(TEXT(" "), TEXT("_"));
		Base.ReplaceInline(TEXT("."), TEXT("_"));
		Base.ReplaceInline(TEXT("/"), TEXT("_"));
		Base.ReplaceInline(TEXT("\\"), TEXT("_"));

		FString Candidate = Base;
		int32 Suffix = 2;
		while (UsedIds.Contains(Candidate))
		{
			Candidate = FString::Printf(TEXT("%s_%d"), *Base, Suffix++);
		}
		UsedIds.Add(Candidate);
		return Candidate;
	}

	static TSharedPtr<FJsonObject> NodeToJson(UPCGGraph* Graph, UPCGNode* Node, const FString& NodeId)
	{
		TSharedPtr<FJsonObject> NodeObj = MakeShared<FJsonObject>();
		if (!Graph || !Node)
		{
			return NodeObj;
		}

		NodeObj->SetStringField(TEXT("id"), NodeId);
		NodeObj->SetStringField(TEXT("object_name"), Node->GetName());
		NodeObj->SetStringField(TEXT("title"), Node->GetNodeTitle(EPCGNodeTitleType::ListView).ToString());
		NodeObj->SetStringField(TEXT("authored_title"), Node->GetAuthoredTitleName().ToString());
		if (Node == Graph->GetInputNode())
		{
			NodeObj->SetStringField(TEXT("role"), TEXT("input"));
		}
		else if (Node == Graph->GetOutputNode())
		{
			NodeObj->SetStringField(TEXT("role"), TEXT("output"));
		}
		else
		{
			NodeObj->SetStringField(TEXT("role"), TEXT("settings"));
		}

#if WITH_EDITOR
		int32 PosX = 0;
		int32 PosY = 0;
		Node->GetNodePosition(PosX, PosY);
		NodeObj->SetNumberField(TEXT("pos_x"), PosX);
		NodeObj->SetNumberField(TEXT("pos_y"), PosY);
#endif

		UPCGSettings* Settings = Node->GetSettings();
		if (Settings)
		{
			NodeObj->SetStringField(TEXT("settings_class"), Settings->GetClass()->GetPathName());
			NodeObj->SetStringField(TEXT("settings_name"), Settings->GetName());
			NodeObj->SetArrayField(TEXT("settings_properties"), ExportEditableProperties(Settings));
			NodeObj->SetBoolField(TEXT("enabled"), Settings->bEnabled);
			NodeObj->SetBoolField(TEXT("debug"), Settings->bDebug);
#if WITH_EDITOR
			NodeObj->SetBoolField(TEXT("inspect"), Settings->bIsInspecting);
			NodeObj->SetBoolField(TEXT("supports_gpu_toggle"), Settings->DisplayExecuteOnGPUSetting());
#endif
			NodeObj->SetBoolField(TEXT("should_execute_on_gpu"), Settings->ShouldExecuteOnGPU());
			NodeObj->SetArrayField(TEXT("input_pins"), PinsToJson(Settings->AllInputPinProperties()));
			NodeObj->SetArrayField(TEXT("output_pins"), PinsToJson(Settings->AllOutputPinProperties()));
		}
		else
		{
			NodeObj->SetArrayField(TEXT("input_pins"), PinsToJson(Node->InputPinProperties()));
			NodeObj->SetArrayField(TEXT("output_pins"), PinsToJson(Node->OutputPinProperties()));
		}

		return NodeObj;
	}

	static TSharedPtr<FJsonObject> EdgeToJson(const FString& FromId, const FString& FromPin, const FString& ToId, const FString& ToPin)
	{
		TSharedPtr<FJsonObject> EdgeObj = MakeShared<FJsonObject>();
		EdgeObj->SetStringField(TEXT("from"), FromId);
		EdgeObj->SetStringField(TEXT("from_pin"), FromPin);
		EdgeObj->SetStringField(TEXT("to"), ToId);
		EdgeObj->SetStringField(TEXT("to_pin"), ToPin);
		return EdgeObj;
	}

	static UPCGPin* ResolvePCGEdgePin(UPCGNode* Node, const FString& RequestedLabel, bool bOutputPin, FString& OutResolvedLabel)
	{
		OutResolvedLabel = RequestedLabel;
		if (!Node)
		{
			return nullptr;
		}

		UPCGPin* Pin = RequestedLabel.IsEmpty()
			? nullptr
			: (bOutputPin ? Node->GetOutputPin(FName(*RequestedLabel)) : Node->GetInputPin(FName(*RequestedLabel)));
		if (!Pin && RequestedLabel.IsEmpty())
		{
			const TArray<TObjectPtr<UPCGPin>>& Pins = bOutputPin ? Node->GetOutputPins() : Node->GetInputPins();
			if (Pins.Num() == 1)
			{
				Pin = Pins[0].Get();
			}
		}
		if (Pin)
		{
			OutResolvedLabel = Pin->Properties.Label.ToString();
		}
		return Pin;
	}

	static bool HasPCGEdge(UPCGPin* FromPin, UPCGPin* ToPin)
	{
		if (!FromPin || !ToPin)
		{
			return false;
		}
		for (const TObjectPtr<UPCGEdge>& EdgePtr : FromPin->Edges)
		{
			UPCGEdge* Edge = EdgePtr.Get();
			if (Edge && Edge->InputPin == FromPin && Edge->OutputPin == ToPin)
			{
				return true;
			}
		}
		return false;
	}

	static TSharedPtr<FJsonObject> ExportParameterBagToJson(const FInstancedPropertyBag* Bag);

	static TSharedPtr<FJsonObject> ExportGraphToJson(UPCGGraph* Graph)
	{
		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("schema"), SchemaFolder);
		if (!Graph)
		{
			return Root;
		}

		Root->SetStringField(TEXT("asset_path"), Graph->GetOutermost() ? Graph->GetOutermost()->GetName() : FString());
		Root->SetStringField(TEXT("object_path"), Graph->GetPathName());
		Root->SetStringField(TEXT("asset_class"), Graph->GetClass()->GetPathName());

		TSharedPtr<FJsonObject> GraphSettings = MakeShared<FJsonObject>();
		GraphSettings->SetBoolField(TEXT("landscape_uses_metadata"), Graph->bLandscapeUsesMetadata);
		GraphSettings->SetArrayField(TEXT("properties"), ExportEditableProperties(Graph));
		Root->SetObjectField(TEXT("graph_settings"), GraphSettings);
		Root->SetObjectField(TEXT("parameters"), ExportParameterBagToJson(Graph->GetUserParametersStruct()));

		TArray<TSharedPtr<FJsonValue>> Nodes;
		TArray<TSharedPtr<FJsonValue>> Edges;
		TMap<UPCGNode*, FString> NodeIds;
		TSet<FString> UsedIds;
		TArray<UPCGNode*> ExportNodes;
		TSet<UPCGNode*> SeenExportNodes;

		auto AddExportNode = [&ExportNodes, &SeenExportNodes](UPCGNode* Node)
		{
			if (Node && !SeenExportNodes.Contains(Node))
			{
				SeenExportNodes.Add(Node);
				ExportNodes.Add(Node);
			}
		};

		AddExportNode(Graph->GetInputNode());
		for (UPCGNode* Node : Graph->GetNodes())
		{
			AddExportNode(Node);
		}
		AddExportNode(Graph->GetOutputNode());

		for (UPCGNode* Node : ExportNodes)
		{
			const FString NodeId = MakeStableNodeId(Graph, Node, UsedIds);
			NodeIds.Add(Node, NodeId);
		}

		for (UPCGNode* Node : ExportNodes)
		{
			const FString* NodeId = NodeIds.Find(Node);
			Nodes.Add(MakeShared<FJsonValueObject>(NodeToJson(Graph, Node, NodeId ? *NodeId : FString())));
		}

		for (UPCGNode* Node : ExportNodes)
		{
			const FString* FromId = NodeIds.Find(Node);
			if (!FromId)
			{
				continue;
			}
			for (const TObjectPtr<UPCGPin>& PinPtr : Node->GetOutputPins())
			{
				UPCGPin* Pin = PinPtr.Get();
				if (!Pin)
				{
					continue;
				}
				for (const TObjectPtr<UPCGEdge>& EdgePtr : Pin->Edges)
				{
					UPCGEdge* Edge = EdgePtr.Get();
					if (!Edge || Edge->InputPin != Pin || !Edge->OutputPin || !Edge->OutputPin->Node)
					{
						continue;
					}
					const FString* ToId = NodeIds.Find(Edge->OutputPin->Node);
					if (!ToId)
					{
						continue;
					}
					Edges.Add(MakeShared<FJsonValueObject>(EdgeToJson(
						*FromId,
						Pin->Properties.Label.ToString(),
						*ToId,
						Edge->OutputPin->Properties.Label.ToString())));
				}
			}
		}

		Root->SetArrayField(TEXT("nodes"), Nodes);
		Root->SetArrayField(TEXT("edges"), Edges);
		Root->SetNumberField(TEXT("node_count"), Nodes.Num());
		Root->SetNumberField(TEXT("edge_count"), Edges.Num());
		return Root;
	}

	static TSharedPtr<FJsonObject> ExportParameterBagToJson(const FInstancedPropertyBag* Bag)
	{
		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> Parameters;
		if (!Bag || !Bag->IsValid() || !Bag->GetPropertyBagStruct())
		{
			Root->SetArrayField(TEXT("parameters"), Parameters);
			return Root;
		}

		for (const FPropertyBagPropertyDesc& Desc : Bag->GetPropertyBagStruct()->GetPropertyDescs())
		{
			TSharedPtr<FJsonObject> ParamObj = MakeShared<FJsonObject>();
			ParamObj->SetStringField(TEXT("name"), Desc.Name.ToString());
			ParamObj->SetStringField(TEXT("type"), EnumValueToString(StaticEnum<EPropertyBagPropertyType>(), static_cast<int64>(Desc.ValueType)));
			ParamObj->SetStringField(TEXT("container_type"), Desc.ContainerTypes.IsEmpty() ? TEXT("None") : EnumValueToString(StaticEnum<EPropertyBagContainerType>(), static_cast<int64>(Desc.ContainerTypes.GetFirstContainerType())));
			TArray<TSharedPtr<FJsonValue>> ContainerTypes;
			for (const EPropertyBagContainerType ContainerType : Desc.ContainerTypes)
			{
				ContainerTypes.Add(MakeShared<FJsonValueString>(EnumValueToString(StaticEnum<EPropertyBagContainerType>(), static_cast<int64>(ContainerType))));
			}
			ParamObj->SetArrayField(TEXT("container_types"), ContainerTypes);
			ParamObj->SetStringField(TEXT("type_object"), Desc.ValueTypeObject ? Desc.ValueTypeObject->GetPathName() : FString());
			if (TValueOrError<FString, EPropertyBagResult> Serialized = Bag->GetValueSerializedString(Desc.Name); Serialized.IsValid())
			{
				ParamObj->SetStringField(TEXT("value_text"), Serialized.GetValue());
			}
			Parameters.Add(MakeShared<FJsonValueObject>(ParamObj));
		}
		Root->SetArrayField(TEXT("parameters"), Parameters);
		Root->SetNumberField(TEXT("parameter_count"), Parameters.Num());
		return Root;
	}

	static EPropertyBagPropertyType ParseBagPropertyType(const FString& TypeText)
	{
		if (TypeText.IsEmpty())
		{
			return EPropertyBagPropertyType::String;
		}
		const UEnum* TypeEnum = StaticEnum<EPropertyBagPropertyType>();
		const int64 Value = TypeEnum ? TypeEnum->GetValueByNameString(TypeText) : INDEX_NONE;
		if (Value != INDEX_NONE)
		{
			return static_cast<EPropertyBagPropertyType>(Value);
		}
		if (TypeText.Equals(TEXT("bool"), ESearchCase::IgnoreCase)) return EPropertyBagPropertyType::Bool;
		if (TypeText.Equals(TEXT("int"), ESearchCase::IgnoreCase) || TypeText.Equals(TEXT("int32"), ESearchCase::IgnoreCase)) return EPropertyBagPropertyType::Int32;
		if (TypeText.Equals(TEXT("float"), ESearchCase::IgnoreCase)) return EPropertyBagPropertyType::Float;
		if (TypeText.Equals(TEXT("double"), ESearchCase::IgnoreCase)) return EPropertyBagPropertyType::Double;
		if (TypeText.Equals(TEXT("name"), ESearchCase::IgnoreCase)) return EPropertyBagPropertyType::Name;
		if (TypeText.Equals(TEXT("text"), ESearchCase::IgnoreCase)) return EPropertyBagPropertyType::Text;
		return EPropertyBagPropertyType::String;
	}

	static EPropertyBagContainerType ParseBagContainerType(const FString& TypeText)
	{
		if (TypeText.IsEmpty())
		{
			return EPropertyBagContainerType::None;
		}
		const UEnum* TypeEnum = StaticEnum<EPropertyBagContainerType>();
		const int64 Value = TypeEnum ? TypeEnum->GetValueByNameString(TypeText) : INDEX_NONE;
		if (Value != INDEX_NONE)
		{
			return static_cast<EPropertyBagContainerType>(Value);
		}
		if (TypeText.Equals(TEXT("array"), ESearchCase::IgnoreCase))
		{
			return EPropertyBagContainerType::Array;
		}
		if (TypeText.Equals(TEXT("set"), ESearchCase::IgnoreCase))
		{
			return EPropertyBagContainerType::Set;
		}
		return EPropertyBagContainerType::None;
	}

	static FPropertyBagContainerTypes ParseBagContainerTypesFromJson(const TSharedPtr<FJsonObject>& ParamObj)
	{
		FPropertyBagContainerTypes ContainerTypes;
		if (!ParamObj.IsValid())
		{
			return ContainerTypes;
		}

		const TArray<TSharedPtr<FJsonValue>>* ContainerTypeValues = nullptr;
		if (ParamObj->TryGetArrayField(TEXT("container_types"), ContainerTypeValues) && ContainerTypeValues)
		{
			for (const TSharedPtr<FJsonValue>& Value : *ContainerTypeValues)
			{
				FString TypeText;
				if (Value.IsValid() && Value->TryGetString(TypeText))
				{
					const EPropertyBagContainerType ContainerType = ParseBagContainerType(TypeText);
					if (ContainerType != EPropertyBagContainerType::None)
					{
						ContainerTypes.Add(ContainerType);
					}
				}
			}
			return ContainerTypes;
		}

		FString ContainerTypeText;
		if (ParamObj->TryGetStringField(TEXT("container_type"), ContainerTypeText))
		{
			const EPropertyBagContainerType ContainerType = ParseBagContainerType(ContainerTypeText);
			if (ContainerType != EPropertyBagContainerType::None)
			{
				ContainerTypes.Add(ContainerType);
			}
		}
		return ContainerTypes;
	}

	static int32 ApplyParameterArray(
		FInstancedPropertyBag& Bag,
		const TArray<TSharedPtr<FJsonValue>>* Parameters,
		TArray<TSharedPtr<FJsonValue>>& Issues,
		TArray<FName>* OutAppliedNames = nullptr,
		bool bAllowAddMissingParameters = true)
	{
		if (!Parameters)
		{
			return 0;
		}
		int32 AppliedCount = 0;
		for (const TSharedPtr<FJsonValue>& Value : *Parameters)
		{
			const TSharedPtr<FJsonObject>* ParamObj = nullptr;
			if (!Value.IsValid() || !Value->TryGetObject(ParamObj) || !ParamObj || !ParamObj->IsValid())
			{
				AddIssue(Issues, TEXT("error"), TEXT("invalid_parameter_entry"), TEXT("parameters"), TEXT("Parameter entry must be a JSON object."));
				continue;
			}

			FString NameText;
			if (!(*ParamObj)->TryGetStringField(TEXT("name"), NameText) || NameText.IsEmpty())
			{
				AddIssue(Issues, TEXT("error"), TEXT("missing_parameter_name"), TEXT("parameters"), TEXT("Parameter entry is missing name."));
				continue;
			}

			FString TypeText;
			(*ParamObj)->TryGetStringField(TEXT("type"), TypeText);
			FString TypeObjectPath;
			(*ParamObj)->TryGetStringField(TEXT("type_object"), TypeObjectPath);
			const FName ParamName(*NameText);
			const EPropertyBagPropertyType BagType = ParseBagPropertyType(TypeText);
			const FPropertyBagContainerTypes ContainerTypes = ParseBagContainerTypesFromJson(*ParamObj);
			UObject* TypeObject = TypeObjectPath.IsEmpty() ? nullptr : LoadObject<UObject>(nullptr, *TypeObjectPath);
			if (!TypeObjectPath.IsEmpty() && !TypeObject)
			{
				AddIssue(Issues, TEXT("error"), TEXT("parameter_type_object_not_found"), FString::Printf(TEXT("parameters.%s.type_object"), *NameText), TypeObjectPath);
				continue;
			}

			const FPropertyBagPropertyDesc* ExistingDesc = Bag.FindPropertyDescByName(ParamName);
			if (!ExistingDesc)
			{
				if (!bAllowAddMissingParameters)
				{
					AddIssue(Issues, TEXT("error"), TEXT("unknown_parameter"), FString::Printf(TEXT("parameters.%s"), *NameText), TEXT("Graph instance parameters must already exist on the parent graph."));
					continue;
				}
				const EPropertyBagAlterationResult AddResult = ContainerTypes.IsEmpty()
					? Bag.AddProperty(ParamName, BagType, TypeObject, true)
					: Bag.AddContainerProperty(ParamName, ContainerTypes, BagType, TypeObject, true);
				if (AddResult != EPropertyBagAlterationResult::Success)
				{
					AddIssue(Issues, TEXT("error"), TEXT("parameter_add_failed"), FString::Printf(TEXT("parameters.%s"), *NameText), EnumValueToString(StaticEnum<EPropertyBagAlterationResult>(), static_cast<int64>(AddResult)));
					continue;
				}
			}
			else if (bAllowAddMissingParameters && (ExistingDesc->ValueType != BagType || ExistingDesc->ValueTypeObject.Get() != TypeObject || ExistingDesc->ContainerTypes != ContainerTypes))
			{
				const EPropertyBagAlterationResult AddResult = ContainerTypes.IsEmpty()
					? Bag.AddProperty(ParamName, BagType, TypeObject, true)
					: Bag.AddContainerProperty(ParamName, ContainerTypes, BagType, TypeObject, true);
				if (AddResult != EPropertyBagAlterationResult::Success)
				{
					AddIssue(Issues, TEXT("error"), TEXT("parameter_retype_failed"), FString::Printf(TEXT("parameters.%s"), *NameText), EnumValueToString(StaticEnum<EPropertyBagAlterationResult>(), static_cast<int64>(AddResult)));
					continue;
				}
			}

			FString ValueText;
			if (!(*ParamObj)->TryGetStringField(TEXT("value_text"), ValueText))
			{
				const TSharedPtr<FJsonValue>* RawValue = (*ParamObj)->Values.Find(TEXT("value"));
				ValueText = RawValue ? JsonValueToImportText(*RawValue) : FString();
			}
			if (Bag.SetValueSerializedString(ParamName, ValueText) != EPropertyBagResult::Success)
			{
				AddIssue(Issues, TEXT("error"), TEXT("parameter_import_failed"), FString::Printf(TEXT("parameters.%s"), *NameText), TEXT("SetValueSerializedString failed."));
				continue;
			}
			AppliedCount++;
			if (OutAppliedNames)
			{
				OutAppliedNames->AddUnique(ParamName);
			}
		}
		return AppliedCount;
	}

	static int32 PreflightParameterArray(
		const FInstancedPropertyBag* SourceBag,
		const TArray<TSharedPtr<FJsonValue>>* Parameters,
		TArray<TSharedPtr<FJsonValue>>& Issues,
		bool bAllowAddMissingParameters)
	{
		FInstancedPropertyBag PreviewBag;
		if (SourceBag)
		{
			PreviewBag = *SourceBag;
		}
		TArray<FName> PreviewAppliedNames;
		return ApplyParameterArray(PreviewBag, Parameters, Issues, &PreviewAppliedNames, bAllowAddMissingParameters);
	}

	static void RefreshGraphInstanceParameters(UPCGGraphInstance* Instance)
	{
		if (Instance)
		{
			Instance->OnGraphParametersChanged(EPCGGraphParameterEvent::GraphPostLoad, NAME_None);
		}
	}

	static void NotifyGraphInstanceParameterValuesChanged(UPCGGraphInstance* Instance, const TArray<FName>& ParameterNames)
	{
		if (!Instance)
		{
			return;
		}
		for (const FName& ParamName : ParameterNames)
		{
			if (!ParamName.IsNone())
			{
				Instance->OnGraphParametersChanged(EPCGGraphParameterEvent::ValueModifiedLocally, ParamName);
			}
		}
	}

	static UPCGGraph* CreateOrLoadGraphAsset(const FString& AssetPathInput, bool bCreateIfMissing, bool& bOutCreated, FString& OutError)
	{
		bOutCreated = false;
		const FString AssetPath = NormalizeAssetPath(AssetPathInput);
		if (UPCGGraph* ExistingGraph = LoadAsset<UPCGGraph>(AssetPath))
		{
			return ExistingGraph;
		}
		if (!bCreateIfMissing)
		{
			OutError = TEXT("pcg_graph_asset_not_found");
			return nullptr;
		}
		if (!FPackageName::IsValidLongPackageName(AssetPath))
		{
			OutError = TEXT("invalid_asset_path");
			return nullptr;
		}

		const FString AssetName = FPackageName::GetLongPackageAssetName(AssetPath);
		UPackage* Package = CreatePackage(*AssetPath);
		if (!Package)
		{
			OutError = TEXT("create_package_failed");
			return nullptr;
		}

		UPCGGraph* Graph = NewObject<UPCGGraph>(Package, UPCGGraph::StaticClass(), *AssetName, RF_Public | RF_Standalone | RF_Transactional);
		if (!Graph)
		{
			OutError = TEXT("create_pcg_graph_failed");
			return nullptr;
		}
		FAssetRegistryModule::AssetCreated(Graph);
		Graph->MarkPackageDirty();
		Package->MarkPackageDirty();
		bOutCreated = true;
		return Graph;
	}

	static bool ApplyGraphJson(UPCGGraph* Graph, const TSharedPtr<FJsonObject>& GraphJson, bool bReplaceNodes, TArray<TSharedPtr<FJsonValue>>& Issues, int32& OutAppliedNodes, int32& OutAppliedEdges)
	{
		OutAppliedNodes = 0;
		OutAppliedEdges = 0;
		if (!Graph || !GraphJson.IsValid())
		{
			AddIssue(Issues, TEXT("error"), TEXT("invalid_graph_apply_input"), TEXT("graph"), TEXT("Graph asset or graph JSON is invalid."));
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* NodeValues = nullptr;
		GraphJson->TryGetArrayField(TEXT("nodes"), NodeValues);
		const TArray<TSharedPtr<FJsonValue>>* EdgeValues = nullptr;
		GraphJson->TryGetArrayField(TEXT("edges"), EdgeValues);

		if (bReplaceNodes)
		{
			TArray<UPCGNode*> NodesToRemove;
			for (UPCGNode* Node : Graph->GetNodes())
			{
				if (Node && Node != Graph->GetInputNode() && Node != Graph->GetOutputNode())
				{
					NodesToRemove.Add(Node);
				}
			}
			if (NodesToRemove.Num() > 0)
			{
				Graph->RemoveNodes(NodesToRemove);
			}
		}

		TMap<FString, UPCGNode*> NodeById;
		NodeById.Add(InputNodeId, Graph->GetInputNode());
		NodeById.Add(OutputNodeId, Graph->GetOutputNode());

		if (NodeValues)
		{
			for (const TSharedPtr<FJsonValue>& Value : *NodeValues)
			{
				const TSharedPtr<FJsonObject>* NodeObj = nullptr;
				if (!Value.IsValid() || !Value->TryGetObject(NodeObj) || !NodeObj || !NodeObj->IsValid())
				{
					AddIssue(Issues, TEXT("error"), TEXT("invalid_node_entry"), TEXT("nodes"), TEXT("Node entry must be a JSON object."));
					continue;
				}

				FString Id;
				(*NodeObj)->TryGetStringField(TEXT("id"), Id);
				if (Id.IsEmpty())
				{
					AddIssue(Issues, TEXT("error"), TEXT("missing_node_id"), TEXT("nodes"), TEXT("Node entry is missing id."));
					continue;
				}
				if (Id == InputNodeId || Id == OutputNodeId)
				{
					UPCGNode* Node = (Id == InputNodeId) ? Graph->GetInputNode() : Graph->GetOutputNode();
					UPCGSettings* Settings = Node ? Node->GetSettings() : nullptr;
					if (!Node || !Settings)
					{
						AddIssue(Issues, TEXT("error"), TEXT("input_output_node_not_found"), FString::Printf(TEXT("nodes.%s"), *Id), TEXT("Graph input/output node or settings is unavailable."));
						continue;
					}

					FString AuthoredTitle;
					if ((*NodeObj)->TryGetStringField(TEXT("authored_title"), AuthoredTitle) && !AuthoredTitle.IsEmpty() && AuthoredTitle != TEXT("None"))
					{
						Node->NodeTitle = FName(*AuthoredTitle);
					}

#if WITH_EDITOR
					double PosX = 0.0;
					double PosY = 0.0;
					if ((*NodeObj)->TryGetNumberField(TEXT("pos_x"), PosX) || (*NodeObj)->TryGetNumberField(TEXT("x"), PosX))
					{
						(*NodeObj)->TryGetNumberField(TEXT("pos_y"), PosY);
						Node->SetNodePosition(static_cast<int32>(PosX), static_cast<int32>(PosY));
					}
#endif

					const TArray<TSharedPtr<FJsonValue>>* PropertyValues = nullptr;
					if ((*NodeObj)->TryGetArrayField(TEXT("settings_properties"), PropertyValues))
					{
						ApplyPropertyArray(Settings, PropertyValues, Issues, FString::Printf(TEXT("nodes.%s.settings_properties"), *Id));
					}
					ApplyNodeJsonDerivedSettings(Settings, *NodeObj);

					bool bBoolValue = false;
					if ((*NodeObj)->TryGetBoolField(TEXT("enabled"), bBoolValue))
					{
						Settings->SetEnabled(bBoolValue);
					}
					if ((*NodeObj)->TryGetBoolField(TEXT("debug"), bBoolValue))
					{
						Settings->bDebug = bBoolValue;
					}
#if WITH_EDITOR
					if ((*NodeObj)->TryGetBoolField(TEXT("inspect"), bBoolValue))
					{
						Settings->bIsInspecting = bBoolValue;
					}
#endif
					Node->UpdateAfterSettingsChangeDuringCreation();
					continue;
				}

				FString SettingsClassPath;
				if (!(*NodeObj)->TryGetStringField(TEXT("settings_class"), SettingsClassPath) || SettingsClassPath.IsEmpty())
				{
					AddIssue(Issues, TEXT("error"), TEXT("missing_settings_class"), FString::Printf(TEXT("nodes.%s"), *Id), TEXT("Settings node is missing settings_class."));
					continue;
				}

				UClass* SettingsClass = ResolvePCGSettingsClass(SettingsClassPath);
				if (!SettingsClass)
				{
					AddIssue(Issues, TEXT("error"), TEXT("settings_class_not_found"), FString::Printf(TEXT("nodes.%s"), *Id), SettingsClassPath);
					continue;
				}

				UPCGSettings* DefaultSettings = nullptr;
				UPCGNode* Node = Graph->AddNodeOfType(SettingsClass, DefaultSettings);
				if (!Node || !DefaultSettings)
				{
					AddIssue(Issues, TEXT("error"), TEXT("add_node_failed"), FString::Printf(TEXT("nodes.%s"), *Id), SettingsClassPath);
					continue;
				}

				FString AuthoredTitle;
				if ((*NodeObj)->TryGetStringField(TEXT("authored_title"), AuthoredTitle) && !AuthoredTitle.IsEmpty() && AuthoredTitle != TEXT("None"))
				{
					Node->NodeTitle = FName(*AuthoredTitle);
				}

#if WITH_EDITOR
				double PosX = 0.0;
				double PosY = 0.0;
				if ((*NodeObj)->TryGetNumberField(TEXT("pos_x"), PosX) || (*NodeObj)->TryGetNumberField(TEXT("x"), PosX))
				{
					(*NodeObj)->TryGetNumberField(TEXT("pos_y"), PosY);
					Node->SetNodePosition(static_cast<int32>(PosX), static_cast<int32>(PosY));
				}
#endif

				const TArray<TSharedPtr<FJsonValue>>* PropertyValues = nullptr;
				if ((*NodeObj)->TryGetArrayField(TEXT("settings_properties"), PropertyValues))
				{
					ApplyPropertyArray(DefaultSettings, PropertyValues, Issues, FString::Printf(TEXT("nodes.%s.settings_properties"), *Id));
				}
				ApplyNodeJsonDerivedSettings(DefaultSettings, *NodeObj);
				bool bBoolValue = false;
				if ((*NodeObj)->TryGetBoolField(TEXT("enabled"), bBoolValue))
				{
					DefaultSettings->SetEnabled(bBoolValue);
				}
				if ((*NodeObj)->TryGetBoolField(TEXT("debug"), bBoolValue))
				{
					DefaultSettings->bDebug = bBoolValue;
				}
#if WITH_EDITOR
				if ((*NodeObj)->TryGetBoolField(TEXT("inspect"), bBoolValue))
				{
					DefaultSettings->bIsInspecting = bBoolValue;
				}
#endif
				Node->UpdateAfterSettingsChangeDuringCreation();
				NodeById.Add(Id, Node);
				OutAppliedNodes++;
			}
		}

		if (EdgeValues)
		{
			for (const TSharedPtr<FJsonValue>& Value : *EdgeValues)
			{
				const TSharedPtr<FJsonObject>* EdgeObj = nullptr;
				if (!Value.IsValid() || !Value->TryGetObject(EdgeObj) || !EdgeObj || !EdgeObj->IsValid())
				{
					AddIssue(Issues, TEXT("error"), TEXT("invalid_edge_entry"), TEXT("edges"), TEXT("Edge entry must be a JSON object."));
					continue;
				}
				FString FromId;
				FString ToId;
				FString FromPin;
				FString ToPin;
				(*EdgeObj)->TryGetStringField(TEXT("from"), FromId);
				(*EdgeObj)->TryGetStringField(TEXT("to"), ToId);
				(*EdgeObj)->TryGetStringField(TEXT("from_pin"), FromPin);
				(*EdgeObj)->TryGetStringField(TEXT("to_pin"), ToPin);
				UPCGNode** FromNode = NodeById.Find(FromId);
				UPCGNode** ToNode = NodeById.Find(ToId);
				if (!FromNode || !ToNode || !*FromNode || !*ToNode)
				{
					AddIssue(Issues, TEXT("error"), TEXT("edge_node_not_found"), TEXT("edges"), FString::Printf(TEXT("%s -> %s"), *FromId, *ToId));
					continue;
				}
				FString ResolvedFromPin;
				FString ResolvedToPin;
				UPCGPin* SourcePin = ResolvePCGEdgePin(*FromNode, FromPin, true, ResolvedFromPin);
				UPCGPin* TargetPin = ResolvePCGEdgePin(*ToNode, ToPin, false, ResolvedToPin);
				if (!SourcePin || !TargetPin)
				{
					AddIssue(Issues, TEXT("error"), TEXT("edge_pin_not_found"), TEXT("edges"), FString::Printf(TEXT("%s.%s -> %s.%s"), *FromId, *FromPin, *ToId, *ToPin));
					continue;
				}
				Graph->AddLabeledEdge(*FromNode, FName(*ResolvedFromPin), *ToNode, FName(*ResolvedToPin));
				if (!HasPCGEdge(SourcePin, TargetPin))
				{
					AddIssue(Issues, TEXT("error"), TEXT("add_edge_failed"), TEXT("edges"), FString::Printf(TEXT("%s.%s -> %s.%s"), *FromId, *FromPin, *ToId, *ToPin));
					continue;
				}
				OutAppliedEdges++;
			}
		}

		const TSharedPtr<FJsonObject>* GraphSettings = nullptr;
		if (GraphJson->TryGetObjectField(TEXT("graph_settings"), GraphSettings) && GraphSettings && GraphSettings->IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* PropertyValues = nullptr;
			if ((*GraphSettings)->TryGetArrayField(TEXT("properties"), PropertyValues))
			{
				ApplyGraphSettingsPropertyArray(Graph, PropertyValues, Issues, TEXT("graph_settings.properties"));
			}
		}

		const TSharedPtr<FJsonObject>* ParametersObj = nullptr;
		if (GraphJson->TryGetObjectField(TEXT("parameters"), ParametersObj) && ParametersObj && ParametersObj->IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* ParameterValues = nullptr;
			if ((*ParametersObj)->TryGetArrayField(TEXT("parameters"), ParameterValues))
			{
				Graph->UpdateUserParametersStruct([&](FInstancedPropertyBag& Bag)
				{
					ApplyParameterArray(Bag, ParameterValues, Issues, nullptr, true);
				});
			}
		}

		Graph->Recompile();
		Graph->MarkPackageDirty();
		return CountErrors(Issues) == 0;
	}

	static bool PreflightApplyGraphJson(UPCGGraph* ExistingGraph, const TSharedPtr<FJsonObject>& GraphJson, bool bReplaceNodes, TArray<TSharedPtr<FJsonValue>>& Issues, int32& OutAppliedNodes, int32& OutAppliedEdges)
	{
		UPCGGraph* PreflightGraph = ExistingGraph ? DuplicateObject<UPCGGraph>(ExistingGraph, GetTransientPackage()) : nullptr;
		if (!PreflightGraph)
		{
			PreflightGraph = NewObject<UPCGGraph>(GetTransientPackage(), UPCGGraph::StaticClass(), NAME_None, RF_Transient);
		}
		if (!PreflightGraph)
		{
			OutAppliedNodes = 0;
			OutAppliedEdges = 0;
			AddIssue(Issues, TEXT("error"), TEXT("preflight_graph_create_failed"), TEXT("graph"), TEXT("Failed to create a transient PCG graph for apply preflight."));
			return false;
		}

		PreflightGraph->SetFlags(RF_Transient);
		return ApplyGraphJson(PreflightGraph, GraphJson, bReplaceNodes, Issues, OutAppliedNodes, OutAppliedEdges);
	}

	static AActor* FindActorByNameOrLabelDirect(UWorld* World, const FString& NameOrLabel)
	{
		if (!World || NameOrLabel.IsEmpty())
		{
			return nullptr;
		}
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (!Actor)
			{
				continue;
			}
			if (Actor->GetName().Equals(NameOrLabel, ESearchCase::IgnoreCase))
			{
				return Actor;
			}
#if WITH_EDITOR
			if (Actor->GetActorLabel().Equals(NameOrLabel, ESearchCase::IgnoreCase))
			{
				return Actor;
			}
#endif
		}
		return nullptr;
	}

	static bool GetStringAny(const TSharedPtr<FJsonObject>& Params, const TArray<const TCHAR*>& Keys, FString& OutValue)
	{
		if (!Params.IsValid())
		{
			return false;
		}
		for (const TCHAR* Key : Keys)
		{
			if (Key && Params->TryGetStringField(Key, OutValue) && !OutValue.TrimStartAndEnd().IsEmpty())
			{
				OutValue.TrimStartAndEndInline();
				return true;
			}
		}
		return false;
	}

	static UPCGComponent* FindPCGComponent(UWorld* World, const TSharedPtr<FJsonObject>& Params, FString& OutError)
	{
		FString ActorName;
		GetStringAny(Params, { TEXT("actor"), TEXT("actor_name"), TEXT("actor_label") }, ActorName);
		FString ComponentName;
		GetStringAny(Params, { TEXT("component"), TEXT("component_name"), TEXT("pcg_component") }, ComponentName);

		if (!ActorName.IsEmpty())
		{
			AActor* Actor = FindActorByNameOrLabelDirect(World, ActorName);
			if (!Actor)
			{
				OutError = TEXT("actor_not_found");
				return nullptr;
			}

			TArray<UPCGComponent*> Components;
			Actor->GetComponents<UPCGComponent>(Components);
			for (UPCGComponent* Component : Components)
			{
				if (!Component)
				{
					continue;
				}
				if (ComponentName.IsEmpty() ||
					Component->GetName().Equals(ComponentName, ESearchCase::IgnoreCase))
				{
					return Component;
				}
			}
			OutError = TEXT("pcg_component_not_found");
			return nullptr;
		}

		for (TActorIterator<AActor> It(World); It; ++It)
		{
			TArray<UPCGComponent*> Components;
			It->GetComponents<UPCGComponent>(Components);
			for (UPCGComponent* Component : Components)
			{
				if (!Component)
				{
					continue;
				}
				if (ComponentName.IsEmpty() || Component->GetName().Equals(ComponentName, ESearchCase::IgnoreCase))
				{
					return Component;
				}
			}
		}
		OutError = TEXT("pcg_component_not_found");
		return nullptr;
	}

	static FString ComponentGenerationTriggerToString(EPCGComponentGenerationTrigger Trigger)
	{
		return EnumValueToString(StaticEnum<EPCGComponentGenerationTrigger>(), static_cast<int64>(Trigger));
	}

	static bool TryParseGenerationTrigger(const FString& TriggerText, EPCGComponentGenerationTrigger& OutTrigger)
	{
		const UEnum* TriggerEnum = StaticEnum<EPCGComponentGenerationTrigger>();
		const int64 Value = TriggerEnum ? TriggerEnum->GetValueByNameString(TriggerText) : INDEX_NONE;
		if (Value != INDEX_NONE)
		{
			OutTrigger = static_cast<EPCGComponentGenerationTrigger>(Value);
			return true;
		}
		if (TriggerText.Equals(TEXT("on_demand"), ESearchCase::IgnoreCase) || TriggerText.Equals(TEXT("GenerateOnDemand"), ESearchCase::IgnoreCase))
		{
			OutTrigger = EPCGComponentGenerationTrigger::GenerateOnDemand;
			return true;
		}
		if (TriggerText.Equals(TEXT("runtime"), ESearchCase::IgnoreCase) || TriggerText.Equals(TEXT("GenerateAtRuntime"), ESearchCase::IgnoreCase))
		{
			OutTrigger = EPCGComponentGenerationTrigger::GenerateAtRuntime;
			return true;
		}
		if (TriggerText.Equals(TEXT("load"), ESearchCase::IgnoreCase) || TriggerText.Equals(TEXT("GenerateOnLoad"), ESearchCase::IgnoreCase))
		{
			OutTrigger = EPCGComponentGenerationTrigger::GenerateOnLoad;
			return true;
		}
		return false;
	}

	static EPCGComponentGenerationTrigger ParseGenerationTrigger(const FString& TriggerText, EPCGComponentGenerationTrigger DefaultValue)
	{
		EPCGComponentGenerationTrigger Parsed = DefaultValue;
		return TryParseGenerationTrigger(TriggerText, Parsed) ? Parsed : DefaultValue;
	}

	static TSharedPtr<FJsonObject> ComponentToJson(UPCGComponent* Component)
	{
		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("schema"), SchemaComponent);
		if (!Component)
		{
			return Root;
		}

		AActor* Owner = Component->GetOwner();
		Root->SetStringField(TEXT("component_name"), Component->GetName());
		Root->SetStringField(TEXT("component_path"), Component->GetPathName());
		Root->SetStringField(TEXT("actor_name"), Owner ? Owner->GetName() : FString());
#if WITH_EDITOR
		Root->SetStringField(TEXT("actor_label"), Owner ? Owner->GetActorLabel() : FString());
#endif
		Root->SetStringField(TEXT("graph"), Component->GetGraph() ? Component->GetGraph()->GetPathName() : FString());
		Root->SetStringField(TEXT("graph_asset_path"), Component->GetGraph() && Component->GetGraph()->GetOutermost() ? Component->GetGraph()->GetOutermost()->GetName() : FString());
		Root->SetNumberField(TEXT("seed"), Component->Seed);
		Root->SetBoolField(TEXT("activated"), Component->bActivated);
		Root->SetBoolField(TEXT("partitioned"), Component->bIsComponentPartitioned);
		Root->SetStringField(TEXT("generation_trigger"), ComponentGenerationTriggerToString(Component->GenerationTrigger));
		Root->SetBoolField(TEXT("is_generating"), Component->IsGenerating());
		Root->SetBoolField(TEXT("is_cleaning_up"), Component->IsCleaningUp());
		Root->SetBoolField(TEXT("was_generated_this_session"), Component->WasGeneratedThisSession());
		Root->SetBoolField(TEXT("generated"), Component->bGenerated);
		Root->SetNumberField(TEXT("current_generation_task"), static_cast<double>(Component->GetGenerationTaskId()));
		Root->SetNumberField(TEXT("generated_output_count"), Component->GetGeneratedGraphOutput().TaggedData.Num());
		Root->SetArrayField(TEXT("properties"), ExportEditableProperties(Component));
		return Root;
	}

	static TSharedPtr<FJsonObject> PCGDataCollectionToJson(const FPCGDataCollection& Collection, int32 MaxPointsPerData)
	{
		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("schema"), SchemaData);
		TArray<TSharedPtr<FJsonValue>> DataEntries;
		int32 PointDataCount = 0;
		int32 TotalPointCount = 0;
		for (int32 Index = 0; Index < Collection.TaggedData.Num(); ++Index)
		{
			const FPCGTaggedData& TaggedData = Collection.TaggedData[Index];
			TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetNumberField(TEXT("index"), Index);
			Entry->SetStringField(TEXT("pin"), TaggedData.Pin.ToString());
			Entry->SetBoolField(TEXT("used_multiple_times"), TaggedData.bIsUsedMultipleTimes);
			TArray<TSharedPtr<FJsonValue>> Tags;
			for (const FString& Tag : TaggedData.Tags)
			{
				Tags.Add(MakeShared<FJsonValueString>(Tag));
			}
			Entry->SetArrayField(TEXT("tags"), Tags);

			const UPCGData* Data = TaggedData.Data.Get();
			Entry->SetStringField(TEXT("data_class"), Data ? Data->GetClass()->GetPathName() : FString());
			Entry->SetStringField(TEXT("data_path"), Data ? Data->GetPathName() : FString());
			if (const UPCGBasePointData* PointData = Cast<UPCGBasePointData>(Data))
			{
				const int32 NumPoints = PointData->GetNumPoints();
				PointDataCount++;
				TotalPointCount += NumPoints;
				Entry->SetNumberField(TEXT("point_count"), NumPoints);

				TArray<TSharedPtr<FJsonValue>> PointSamples;
				const int32 SampleCount = MaxPointsPerData < 0 ? NumPoints : FMath::Min(NumPoints, MaxPointsPerData);
				for (int32 PointIndex = 0; PointIndex < SampleCount; ++PointIndex)
				{
					const FTransform& Transform = PointData->GetTransform(PointIndex);
					TSharedPtr<FJsonObject> PointObj = MakeShared<FJsonObject>();
					PointObj->SetNumberField(TEXT("index"), PointIndex);
					PointObj->SetObjectField(TEXT("location"), VecToJson(Transform.GetLocation()));
					PointObj->SetObjectField(TEXT("rotation"), RotToJson(Transform.Rotator()));
					PointObj->SetObjectField(TEXT("scale"), VecToJson(Transform.GetScale3D()));
					PointObj->SetNumberField(TEXT("density"), PointData->GetDensity(PointIndex));
					PointObj->SetNumberField(TEXT("seed"), PointData->GetSeed(PointIndex));
					PointObj->SetObjectField(TEXT("bounds_min"), VecToJson(PointData->GetBoundsMin(PointIndex)));
					PointObj->SetObjectField(TEXT("bounds_max"), VecToJson(PointData->GetBoundsMax(PointIndex)));
					PointSamples.Add(MakeShared<FJsonValueObject>(PointObj));
				}
				Entry->SetArrayField(TEXT("point_samples"), PointSamples);
			}
			DataEntries.Add(MakeShared<FJsonValueObject>(Entry));
		}

		Root->SetArrayField(TEXT("data"), DataEntries);
		Root->SetNumberField(TEXT("data_count"), DataEntries.Num());
		Root->SetNumberField(TEXT("point_data_count"), PointDataCount);
		Root->SetNumberField(TEXT("total_point_count"), TotalPointCount);
		return Root;
	}

	static void SetActionStatus(TSharedPtr<FJsonObject>& OutData, UPCGComponent* Component)
	{
		if (!OutData.IsValid())
		{
			OutData = MakeShared<FJsonObject>();
		}
		OutData->SetObjectField(TEXT("component"), ComponentToJson(Component));
	}
}

bool FUeAgentHttpServer::ExecutePCGCommand(const FString& CommandLower, const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	struct FEntry
	{
		const TCHAR* Name;
		bool (FUeAgentHttpServer::*Method)(const FUeAgentRequestContext&, TSharedPtr<FJsonObject>&, FString&) const;
	};

	static const FEntry Entries[] = {
		{ TEXT("pcg_capabilities_query"), &FUeAgentHttpServer::CmdPCGCapabilitiesQuery },
		{ TEXT("pcg_node_catalog_export"), &FUeAgentHttpServer::CmdPCGNodeCatalogExport },
		{ TEXT("pcg_asset_info"), &FUeAgentHttpServer::CmdPCGAssetInfo },
		{ TEXT("pcg_graph_export_folder"), &FUeAgentHttpServer::CmdPCGGraphExportFolder },
		{ TEXT("pcg_graph_validate_folder"), &FUeAgentHttpServer::CmdPCGGraphValidateFolder },
		{ TEXT("pcg_graph_plan_folder"), &FUeAgentHttpServer::CmdPCGGraphPlanFolder },
		{ TEXT("pcg_graph_diff_folder"), &FUeAgentHttpServer::CmdPCGGraphDiffFolder },
		{ TEXT("pcg_graph_apply_folder"), &FUeAgentHttpServer::CmdPCGGraphApplyFolder },
		{ TEXT("pcg_graph_compile"), &FUeAgentHttpServer::CmdPCGGraphCompile },
		{ TEXT("pcg_graph_snapshot"), &FUeAgentHttpServer::CmdPCGGraphSnapshot },
		{ TEXT("pcg_graph_layout"), &FUeAgentHttpServer::CmdPCGGraphLayout },
		{ TEXT("pcg_graph_instance_json"), &FUeAgentHttpServer::CmdPCGGraphInstanceJson },
		{ TEXT("pcg_graph_parameters_query"), &FUeAgentHttpServer::CmdPCGGraphParametersQuery },
		{ TEXT("pcg_graph_parameters_apply_json"), &FUeAgentHttpServer::CmdPCGGraphParametersApplyJson },
		{ TEXT("pcg_component_query"), &FUeAgentHttpServer::CmdPCGComponentQuery },
		{ TEXT("pcg_component_apply_json"), &FUeAgentHttpServer::CmdPCGComponentApplyJson },
		{ TEXT("pcg_component_snapshot_json"), &FUeAgentHttpServer::CmdPCGComponentSnapshotJson },
		{ TEXT("pcg_world_actor_query"), &FUeAgentHttpServer::CmdPCGWorldActorQuery },
		{ TEXT("pcg_world_actor_apply_json"), &FUeAgentHttpServer::CmdPCGWorldActorApplyJson },
		{ TEXT("pcg_partition_probe"), &FUeAgentHttpServer::CmdPCGPartitionProbe },
		{ TEXT("pcg_generate"), &FUeAgentHttpServer::CmdPCGGenerate },
		{ TEXT("pcg_cleanup"), &FUeAgentHttpServer::CmdPCGCleanup },
		{ TEXT("pcg_regenerate"), &FUeAgentHttpServer::CmdPCGRegenerate },
		{ TEXT("pcg_cancel_generation"), &FUeAgentHttpServer::CmdPCGCancelGeneration },
		{ TEXT("pcg_generation_status"), &FUeAgentHttpServer::CmdPCGGenerationStatus },
		{ TEXT("pcg_wait"), &FUeAgentHttpServer::CmdPCGWait },
		{ TEXT("pcg_runtime_generation_probe"), &FUeAgentHttpServer::CmdPCGRuntimeGenerationProbe },
		{ TEXT("pcg_inspection_set"), &FUeAgentHttpServer::CmdPCGInspectionSet },
		{ TEXT("pcg_node_inspect"), &FUeAgentHttpServer::CmdPCGNodeInspect },
		{ TEXT("pcg_data_export_json"), &FUeAgentHttpServer::CmdPCGDataExportJson },
		{ TEXT("pcg_data_assert_json"), &FUeAgentHttpServer::CmdPCGDataAssertJson },
		{ TEXT("pcg_component_runtime_probe"), &FUeAgentHttpServer::CmdPCGComponentRuntimeProbe },
		{ TEXT("pcg_screenshot"), &FUeAgentHttpServer::CmdPCGScreenshot },
		{ TEXT("pcg_profile_report"), &FUeAgentHttpServer::CmdPCGProfileReport },
		{ TEXT("pcg_gpu_capabilities_query"), &FUeAgentHttpServer::CmdPCGGPUCapabilitiesQuery },
		{ TEXT("pcg_compute_graph_validate"), &FUeAgentHttpServer::CmdPCGComputeGraphValidate },
		{ TEXT("pcg_hlsl_validate"), &FUeAgentHttpServer::CmdPCGHLSLValidate },
		{ TEXT("pcg_data_asset_export_json"), &FUeAgentHttpServer::CmdPCGDataAssetExportJson },
		{ TEXT("pcg_data_asset_apply_json"), &FUeAgentHttpServer::CmdPCGDataAssetApplyJson },
		{ TEXT("pcg_level_to_asset_export"), &FUeAgentHttpServer::CmdPCGLevelToAssetExport },
	};

	for (const FEntry& Entry : Entries)
	{
		if (CommandLower == Entry.Name)
		{
			return (this->*Entry.Method)(Ctx, OutData, OutError);
		}
	}

	OutError = FString::Printf(TEXT("unknown_pcg_command:%s"), *CommandLower);
	return false;
}

bool FUeAgentHttpServer::CmdPCGCapabilitiesQuery(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentPCG;
	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("schema"), SchemaCapabilities);
	OutData->SetBoolField(TEXT("pcg_module_loaded"), FModuleManager::Get().IsModuleLoaded(TEXT("PCG")));
	OutData->SetBoolField(TEXT("pcg_editor_module_loaded"), FModuleManager::Get().IsModuleLoaded(TEXT("PCGEditor")));
	OutData->SetBoolField(TEXT("compute_framework_loaded"), FModuleManager::Get().IsModuleLoaded(TEXT("ComputeFramework")));
	OutData->SetBoolField(TEXT("supports_graph_folder_workflow"), true);
	OutData->SetBoolField(TEXT("supports_component_generation"), true);
	OutData->SetBoolField(TEXT("supports_execution_inspection"), true);
	OutData->SetBoolField(TEXT("supports_level_to_asset"), true);
	OutData->SetBoolField(TEXT("supports_viewport_screenshot_bridge"), true);
	OutData->SetStringField(TEXT("rhi"), GDynamicRHI ? GDynamicRHI->GetName() : FString());

	TArray<TSharedPtr<FJsonValue>> Commands;
	static const TCHAR* CommandNames[] = {
		TEXT("pcg_capabilities_query"), TEXT("pcg_node_catalog_export"), TEXT("pcg_asset_info"),
		TEXT("pcg_graph_export_folder"), TEXT("pcg_graph_validate_folder"), TEXT("pcg_graph_plan_folder"),
		TEXT("pcg_graph_diff_folder"), TEXT("pcg_graph_apply_folder"), TEXT("pcg_graph_compile"),
		TEXT("pcg_graph_snapshot"), TEXT("pcg_graph_layout"), TEXT("pcg_graph_instance_json"), TEXT("pcg_graph_parameters_query"),
		TEXT("pcg_graph_parameters_apply_json"), TEXT("pcg_component_query"), TEXT("pcg_component_apply_json"),
		TEXT("pcg_component_snapshot_json"), TEXT("pcg_world_actor_query"), TEXT("pcg_world_actor_apply_json"),
		TEXT("pcg_partition_probe"), TEXT("pcg_generate"), TEXT("pcg_cleanup"), TEXT("pcg_regenerate"),
		TEXT("pcg_cancel_generation"), TEXT("pcg_generation_status"), TEXT("pcg_wait"),
		TEXT("pcg_runtime_generation_probe"), TEXT("pcg_inspection_set"), TEXT("pcg_node_inspect"),
		TEXT("pcg_data_export_json"), TEXT("pcg_data_assert_json"), TEXT("pcg_component_runtime_probe"),
		TEXT("pcg_screenshot"), TEXT("pcg_profile_report"), TEXT("pcg_gpu_capabilities_query"),
		TEXT("pcg_compute_graph_validate"), TEXT("pcg_hlsl_validate"), TEXT("pcg_data_asset_export_json"),
		TEXT("pcg_data_asset_apply_json"), TEXT("pcg_level_to_asset_export")
	};
	for (const TCHAR* Name : CommandNames)
	{
		Commands.Add(MakeShared<FJsonValueString>(Name));
	}
	OutData->SetArrayField(TEXT("commands"), Commands);
	OutData->SetNumberField(TEXT("command_count"), Commands.Num());
	return true;
}

bool FUeAgentHttpServer::CmdPCGNodeCatalogExport(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentPCG;
	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("schema"), SchemaCatalog);

	bool bIncludeDeprecated = false;
	if (Ctx.Params.IsValid())
	{
		Ctx.Params->TryGetBoolField(TEXT("include_deprecated"), bIncludeDeprecated);
	}

	TArray<TSharedPtr<FJsonValue>> Nodes;
	for (TObjectIterator<UClass> It; It; ++It)
	{
		UClass* Class = *It;
		if (!Class || !Class->IsChildOf(UPCGSettings::StaticClass()))
		{
			continue;
		}
		if (Class->HasAnyClassFlags(CLASS_Abstract))
		{
			continue;
		}
		if (!bIncludeDeprecated && Class->HasAnyClassFlags(CLASS_Deprecated | CLASS_NewerVersionExists))
		{
			continue;
		}
		Nodes.Add(MakeShared<FJsonValueObject>(SettingsClassToCatalogJson(Class)));
	}

	Nodes.Sort([](const TSharedPtr<FJsonValue>& A, const TSharedPtr<FJsonValue>& B)
	{
		const TSharedPtr<FJsonObject>* ObjA = nullptr;
		const TSharedPtr<FJsonObject>* ObjB = nullptr;
		FString NameA;
		FString NameB;
		if (A.IsValid() && A->TryGetObject(ObjA) && ObjA && ObjA->IsValid())
		{
			(*ObjA)->TryGetStringField(TEXT("class_name"), NameA);
		}
		if (B.IsValid() && B->TryGetObject(ObjB) && ObjB && ObjB->IsValid())
		{
			(*ObjB)->TryGetStringField(TEXT("class_name"), NameB);
		}
		return NameA < NameB;
	});

	OutData->SetArrayField(TEXT("nodes"), Nodes);
	OutData->SetNumberField(TEXT("node_count"), Nodes.Num());

	FString OutputFile;
	if (Ctx.Params.IsValid() && Ctx.Params->TryGetStringField(TEXT("output_file"), OutputFile) && !OutputFile.TrimStartAndEnd().IsEmpty())
	{
		FString SavedPath;
		if (!SaveJsonFile(OutputFile, OutData, SavedPath, OutError))
		{
			return false;
		}
		OutData->SetStringField(TEXT("output_file"), SavedPath);
	}
	return true;
}

bool FUeAgentHttpServer::CmdPCGAssetInfo(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentPCG;
	FString AssetPath;
	if (!GetStringAny(Ctx.Params, { TEXT("asset_path"), TEXT("graph"), TEXT("graph_asset_path") }, AssetPath))
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	UObject* Asset = LoadAssetObject(AssetPath);
	if (!Asset)
	{
		OutError = TEXT("asset_not_found");
		return false;
	}

	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.pcg.asset_info.v1"));
	OutData->SetStringField(TEXT("asset_path"), Asset->GetOutermost() ? Asset->GetOutermost()->GetName() : FString());
	OutData->SetStringField(TEXT("object_path"), Asset->GetPathName());
	OutData->SetStringField(TEXT("class_path"), Asset->GetClass()->GetPathName());
	OutData->SetBoolField(TEXT("is_graph"), Asset->IsA<UPCGGraph>());
	OutData->SetBoolField(TEXT("is_graph_instance"), Asset->IsA<UPCGGraphInstance>());
	OutData->SetBoolField(TEXT("is_data_asset"), Asset->IsA<UPCGDataAsset>());
	if (UPCGGraph* Graph = Cast<UPCGGraph>(Asset))
	{
		OutData->SetObjectField(TEXT("graph"), ExportGraphToJson(Graph));
	}
	if (UPCGGraphInstance* Instance = Cast<UPCGGraphInstance>(Asset))
	{
		OutData->SetStringField(TEXT("source_graph"), Instance->Graph ? Instance->Graph->GetPathName() : FString());
		OutData->SetObjectField(TEXT("parameters"), ExportParameterBagToJson(Instance->GetUserParametersStruct()));
	}
	if (UPCGDataAsset* DataAsset = Cast<UPCGDataAsset>(Asset))
	{
		OutData->SetObjectField(TEXT("data"), PCGDataCollectionToJson(DataAsset->Data, 8));
	}
	return true;
}

bool FUeAgentHttpServer::CmdPCGGraphExportFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentPCG;
	FString AssetPath;
	FString FolderPathInput;
	if (!GetStringAny(Ctx.Params, { TEXT("asset_path"), TEXT("graph"), TEXT("graph_asset_path") }, AssetPath))
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}
	if (!GetStringAny(Ctx.Params, { TEXT("folder_path"), TEXT("output_folder"), TEXT("folder") }, FolderPathInput))
	{
		OutError = TEXT("missing_folder_path");
		return false;
	}

	UPCGGraph* Graph = LoadAsset<UPCGGraph>(AssetPath);
	if (!Graph)
	{
		OutError = TEXT("pcg_graph_asset_not_found");
		return false;
	}

	const FString FolderPath = ResolveFolderPath(FolderPathInput);
	IFileManager::Get().MakeDirectory(*FolderPath, true);
	IFileManager::Get().MakeDirectory(*FPaths::Combine(FolderPath, TEXT("nodes")), true);
	IFileManager::Get().MakeDirectory(*FPaths::Combine(FolderPath, TEXT("validation")), true);

	TArray<TSharedPtr<FJsonValue>> Files;
	TSharedPtr<FJsonObject> GraphJson = ExportGraphToJson(Graph);

	TSharedPtr<FJsonObject> AssetJson = MakeShared<FJsonObject>();
	AssetJson->SetStringField(TEXT("schema"), SchemaFolder);
	AssetJson->SetStringField(TEXT("asset_path"), Graph->GetOutermost() ? Graph->GetOutermost()->GetName() : AssetPath);
	AssetJson->SetStringField(TEXT("object_path"), Graph->GetPathName());
	AssetJson->SetStringField(TEXT("asset_class"), Graph->GetClass()->GetPathName());

	TSharedPtr<FJsonObject> NodeIndex = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> NodeIndexEntries;
	const TArray<TSharedPtr<FJsonValue>>* NodeValues = nullptr;
	if (GraphJson->TryGetArrayField(TEXT("nodes"), NodeValues) && NodeValues)
	{
		for (const TSharedPtr<FJsonValue>& NodeValue : *NodeValues)
		{
			const TSharedPtr<FJsonObject>* NodeObj = nullptr;
			if (!NodeValue.IsValid() || !NodeValue->TryGetObject(NodeObj) || !NodeObj || !NodeObj->IsValid())
			{
				continue;
			}
			FString Id;
			(*NodeObj)->TryGetStringField(TEXT("id"), Id);
			if (Id.IsEmpty())
			{
				continue;
			}
			const FString NodeFile = FString::Printf(TEXT("nodes/%s.json"), *Id);
			TSharedPtr<FJsonObject> IndexObj = MakeShared<FJsonObject>();
			IndexObj->SetStringField(TEXT("id"), Id);
			IndexObj->SetStringField(TEXT("file"), NodeFile);
			NodeIndexEntries.Add(MakeShared<FJsonValueObject>(IndexObj));
			if (!SaveTrackedJson(FolderPath, NodeFile, *NodeObj, Files, OutError))
			{
				return false;
			}
		}
	}
	NodeIndex->SetArrayField(TEXT("nodes"), NodeIndexEntries);
	NodeIndex->SetNumberField(TEXT("node_count"), NodeIndexEntries.Num());

	TSharedPtr<FJsonObject> Coverage = MakeShared<FJsonObject>();
	Coverage->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.pcg.folder_coverage.v1"));
	Coverage->SetBoolField(TEXT("asset_json"), true);
	Coverage->SetBoolField(TEXT("graph_json"), true);
	Coverage->SetBoolField(TEXT("nodes_index_json"), true);
	Coverage->SetNumberField(TEXT("node_count"), NodeIndexEntries.Num());
	Coverage->SetNumberField(TEXT("edge_count"), GraphJson->GetIntegerField(TEXT("edge_count")));

	if (!SaveTrackedJson(FolderPath, TEXT("asset.json"), AssetJson, Files, OutError)) return false;
	if (!SaveTrackedJson(FolderPath, TEXT("graph.json"), GraphJson, Files, OutError)) return false;
	if (!SaveTrackedJson(FolderPath, TEXT("nodes/index.json"), NodeIndex, Files, OutError)) return false;
	if (!SaveTrackedJson(FolderPath, TEXT("validation/coverage_report.json"), Coverage, Files, OutError)) return false;

	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("schema"), SchemaFolder);
	OutData->SetStringField(TEXT("folder_path"), FolderPath);
	OutData->SetStringField(TEXT("asset_path"), AssetJson->GetStringField(TEXT("asset_path")));
	OutData->SetArrayField(TEXT("files"), Files);
	OutData->SetNumberField(TEXT("file_count"), Files.Num());
	OutData->SetNumberField(TEXT("node_count"), NodeIndexEntries.Num());
	OutData->SetNumberField(TEXT("edge_count"), GraphJson->GetIntegerField(TEXT("edge_count")));
	return true;
}

bool FUeAgentHttpServer::CmdPCGGraphValidateFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentPCG;
	FString FolderPathInput;
	if (!GetStringAny(Ctx.Params, { TEXT("folder_path"), TEXT("input_folder"), TEXT("folder") }, FolderPathInput))
	{
		OutError = TEXT("missing_folder_path");
		return false;
	}
	const FString FolderPath = ResolveFolderPath(FolderPathInput);
	TArray<TSharedPtr<FJsonValue>> Issues;

	TSharedPtr<FJsonObject> AssetJson;
	TSharedPtr<FJsonObject> GraphJson;
	if (!LoadJsonFile(FPaths::Combine(FolderPath, TEXT("asset.json")), AssetJson, OutError))
	{
		AddIssue(Issues, TEXT("error"), TEXT("asset_json_missing_or_invalid"), TEXT("asset.json"), OutError);
	}
	if (!LoadJsonFile(FPaths::Combine(FolderPath, TEXT("graph.json")), GraphJson, OutError))
	{
		AddIssue(Issues, TEXT("error"), TEXT("graph_json_missing_or_invalid"), TEXT("graph.json"), OutError);
	}

	if (GraphJson.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* Edges = nullptr;
		if (!GraphJson->TryGetArrayField(TEXT("nodes"), Nodes) || !Nodes)
		{
			AddIssue(Issues, TEXT("error"), TEXT("nodes_missing"), TEXT("graph.json.nodes"), TEXT("Graph folder must contain nodes array."));
		}
		if (!GraphJson->TryGetArrayField(TEXT("edges"), Edges) || !Edges)
		{
			AddIssue(Issues, TEXT("error"), TEXT("edges_missing"), TEXT("graph.json.edges"), TEXT("Graph folder must contain edges array."));
		}

		TSet<FString> NodeIds;
		if (Nodes)
		{
			for (const TSharedPtr<FJsonValue>& Value : *Nodes)
			{
				const TSharedPtr<FJsonObject>* NodeObj = nullptr;
				if (!Value.IsValid() || !Value->TryGetObject(NodeObj) || !NodeObj || !NodeObj->IsValid())
				{
					AddIssue(Issues, TEXT("error"), TEXT("invalid_node_entry"), TEXT("graph.json.nodes"), TEXT("Node entry must be an object."));
					continue;
				}
				FString Id;
				(*NodeObj)->TryGetStringField(TEXT("id"), Id);
				if (Id.IsEmpty())
				{
					AddIssue(Issues, TEXT("error"), TEXT("missing_node_id"), TEXT("graph.json.nodes"), TEXT("Node entry is missing id."));
					continue;
				}
				if (NodeIds.Contains(Id))
				{
					AddIssue(Issues, TEXT("error"), TEXT("duplicate_node_id"), TEXT("graph.json.nodes"), Id);
				}
				NodeIds.Add(Id);
				FString Role;
				(*NodeObj)->TryGetStringField(TEXT("role"), Role);
				if (Role != TEXT("input") && Role != TEXT("output"))
				{
					FString SettingsClass;
					if (!(*NodeObj)->TryGetStringField(TEXT("settings_class"), SettingsClass) || !ResolvePCGSettingsClass(SettingsClass))
					{
						AddIssue(Issues, TEXT("error"), TEXT("invalid_settings_class"), FString::Printf(TEXT("graph.json.nodes.%s"), *Id), SettingsClass);
					}
				}
			}
		}
		if (Edges)
		{
			for (const TSharedPtr<FJsonValue>& Value : *Edges)
			{
				const TSharedPtr<FJsonObject>* EdgeObj = nullptr;
				if (!Value.IsValid() || !Value->TryGetObject(EdgeObj) || !EdgeObj || !EdgeObj->IsValid())
				{
					AddIssue(Issues, TEXT("error"), TEXT("invalid_edge_entry"), TEXT("graph.json.edges"), TEXT("Edge entry must be an object."));
					continue;
				}
				FString From;
				FString To;
				(*EdgeObj)->TryGetStringField(TEXT("from"), From);
				(*EdgeObj)->TryGetStringField(TEXT("to"), To);
				if (!NodeIds.Contains(From) || !NodeIds.Contains(To))
				{
					AddIssue(Issues, TEXT("error"), TEXT("edge_references_missing_node"), TEXT("graph.json.edges"), FString::Printf(TEXT("%s -> %s"), *From, *To));
				}
			}
		}
	}

	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.pcg.folder_validation.v1"));
	OutData->SetStringField(TEXT("folder_path"), FolderPath);
	OutData->SetArrayField(TEXT("issues"), Issues);
	OutData->SetNumberField(TEXT("issue_count"), Issues.Num());
	OutData->SetNumberField(TEXT("error_count"), CountErrors(Issues));
	OutData->SetBoolField(TEXT("valid"), CountErrors(Issues) == 0);
	return CountErrors(Issues) == 0;
}

bool FUeAgentHttpServer::CmdPCGGraphPlanFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentPCG;
	FString FolderPathInput;
	if (!GetStringAny(Ctx.Params, { TEXT("folder_path"), TEXT("input_folder"), TEXT("folder") }, FolderPathInput))
	{
		OutError = TEXT("missing_folder_path");
		return false;
	}

	TSharedPtr<FJsonObject> Validation;
	FString ValidationError;
	const bool bValidFolder = CmdPCGGraphValidateFolder(Ctx, Validation, ValidationError);

	const FString FolderPath = ResolveFolderPath(FolderPathInput);
	if (!bValidFolder)
	{
		OutData = MakeShared<FJsonObject>();
		OutData->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.pcg.graph_plan.v1"));
		OutData->SetStringField(TEXT("folder_path"), FolderPath);
		OutData->SetObjectField(TEXT("validation"), Validation);
		OutData->SetNumberField(TEXT("error_count"), Validation.IsValid() ? Validation->GetIntegerField(TEXT("error_count")) : 1);
		OutError = TEXT("pcg_graph_folder_validation_failed");
		return false;
	}

	TSharedPtr<FJsonObject> AssetJson;
	TSharedPtr<FJsonObject> GraphJson;
	LoadJsonFile(FPaths::Combine(FolderPath, TEXT("asset.json")), AssetJson, OutError);
	LoadJsonFile(FPaths::Combine(FolderPath, TEXT("graph.json")), GraphJson, OutError);

	FString AssetPath;
	if (Ctx.Params.IsValid())
	{
		Ctx.Params->TryGetStringField(TEXT("asset_path"), AssetPath);
	}
	if (AssetPath.IsEmpty() && AssetJson.IsValid())
	{
		AssetJson->TryGetStringField(TEXT("asset_path"), AssetPath);
	}

	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.pcg.graph_plan.v1"));
	OutData->SetStringField(TEXT("folder_path"), FolderPath);
	OutData->SetStringField(TEXT("asset_path"), AssetPath);
	OutData->SetBoolField(TEXT("asset_exists"), LoadAsset<UPCGGraph>(AssetPath) != nullptr);
	OutData->SetBoolField(TEXT("would_create_asset"), !AssetPath.IsEmpty() && LoadAsset<UPCGGraph>(AssetPath) == nullptr);
	OutData->SetBoolField(TEXT("would_replace_nodes"), true);
	OutData->SetObjectField(TEXT("validation"), Validation);
	if (GraphJson.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* Edges = nullptr;
		GraphJson->TryGetArrayField(TEXT("nodes"), Nodes);
		GraphJson->TryGetArrayField(TEXT("edges"), Edges);
		OutData->SetNumberField(TEXT("planned_node_count"), Nodes ? Nodes->Num() : 0);
		OutData->SetNumberField(TEXT("planned_edge_count"), Edges ? Edges->Num() : 0);
	}
	return true;
}

bool FUeAgentHttpServer::CmdPCGGraphDiffFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentPCG;
	FString FolderPathInput;
	if (!GetStringAny(Ctx.Params, { TEXT("folder_path"), TEXT("input_folder"), TEXT("folder") }, FolderPathInput))
	{
		OutError = TEXT("missing_folder_path");
		return false;
	}
	const FString FolderPath = ResolveFolderPath(FolderPathInput);
	TSharedPtr<FJsonObject> Validation;
	FString ValidationError;
	if (!CmdPCGGraphValidateFolder(Ctx, Validation, ValidationError))
	{
		OutData = MakeShared<FJsonObject>();
		OutData->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.pcg.graph_diff.v1"));
		OutData->SetStringField(TEXT("folder_path"), FolderPath);
		OutData->SetObjectField(TEXT("validation"), Validation);
		OutData->SetNumberField(TEXT("error_count"), Validation.IsValid() ? Validation->GetIntegerField(TEXT("error_count")) : 1);
		OutError = TEXT("pcg_graph_folder_validation_failed");
		return false;
	}

	TSharedPtr<FJsonObject> AssetJson;
	TSharedPtr<FJsonObject> DesiredGraph;
	if (!LoadJsonFile(FPaths::Combine(FolderPath, TEXT("asset.json")), AssetJson, OutError)) return false;
	if (!LoadJsonFile(FPaths::Combine(FolderPath, TEXT("graph.json")), DesiredGraph, OutError)) return false;

	FString AssetPath;
	if (Ctx.Params.IsValid())
	{
		Ctx.Params->TryGetStringField(TEXT("asset_path"), AssetPath);
	}
	if (AssetPath.IsEmpty())
	{
		AssetJson->TryGetStringField(TEXT("asset_path"), AssetPath);
	}

	UPCGGraph* ExistingGraph = LoadAsset<UPCGGraph>(AssetPath);
	TSharedPtr<FJsonObject> ExistingGraphJson = ExistingGraph ? ExportGraphToJson(ExistingGraph) : MakeShared<FJsonObject>();
	const int32 ExistingNodes = ExistingGraph ? ExistingGraphJson->GetIntegerField(TEXT("node_count")) : 0;
	const int32 DesiredNodes = DesiredGraph->GetIntegerField(TEXT("node_count"));
	const int32 ExistingEdges = ExistingGraph ? ExistingGraphJson->GetIntegerField(TEXT("edge_count")) : 0;
	const int32 DesiredEdges = DesiredGraph->GetIntegerField(TEXT("edge_count"));

	TArray<TSharedPtr<FJsonValue>> Changes;
	if (!ExistingGraph)
	{
		TSharedPtr<FJsonObject> Change = MakeShared<FJsonObject>();
		Change->SetStringField(TEXT("kind"), TEXT("create_asset"));
		Change->SetStringField(TEXT("asset_path"), AssetPath);
		Changes.Add(MakeShared<FJsonValueObject>(Change));
	}
	if (ExistingNodes != DesiredNodes)
	{
		TSharedPtr<FJsonObject> Change = MakeShared<FJsonObject>();
		Change->SetStringField(TEXT("kind"), TEXT("node_count"));
		Change->SetNumberField(TEXT("current"), ExistingNodes);
		Change->SetNumberField(TEXT("desired"), DesiredNodes);
		Changes.Add(MakeShared<FJsonValueObject>(Change));
	}
	if (ExistingEdges != DesiredEdges)
	{
		TSharedPtr<FJsonObject> Change = MakeShared<FJsonObject>();
		Change->SetStringField(TEXT("kind"), TEXT("edge_count"));
		Change->SetNumberField(TEXT("current"), ExistingEdges);
		Change->SetNumberField(TEXT("desired"), DesiredEdges);
		Changes.Add(MakeShared<FJsonValueObject>(Change));
	}

	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.pcg.graph_diff.v1"));
	OutData->SetStringField(TEXT("asset_path"), AssetPath);
	OutData->SetArrayField(TEXT("changes"), Changes);
	OutData->SetNumberField(TEXT("change_count"), Changes.Num());
	OutData->SetBoolField(TEXT("different"), Changes.Num() > 0);
	return true;
}

bool FUeAgentHttpServer::CmdPCGGraphApplyFolder(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentPCG;
	FString FolderPathInput;
	if (!GetStringAny(Ctx.Params, { TEXT("folder_path"), TEXT("input_folder"), TEXT("folder") }, FolderPathInput))
	{
		OutError = TEXT("missing_folder_path");
		return false;
	}
	const FString FolderPath = ResolveFolderPath(FolderPathInput);
	TSharedPtr<FJsonObject> AssetJson;
	TSharedPtr<FJsonObject> GraphJson;
	if (!LoadJsonFile(FPaths::Combine(FolderPath, TEXT("asset.json")), AssetJson, OutError)) return false;
	if (!LoadJsonFile(FPaths::Combine(FolderPath, TEXT("graph.json")), GraphJson, OutError)) return false;

	FString AssetPath;
	if (Ctx.Params.IsValid())
	{
		Ctx.Params->TryGetStringField(TEXT("asset_path"), AssetPath);
	}
	if (AssetPath.IsEmpty())
	{
		AssetJson->TryGetStringField(TEXT("asset_path"), AssetPath);
	}
	if (AssetPath.IsEmpty())
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	bool bDryRun = false;
	bool bSave = true;
	bool bReplaceNodes = true;
	if (Ctx.Params.IsValid())
	{
		Ctx.Params->TryGetBoolField(TEXT("dry_run"), bDryRun);
		Ctx.Params->TryGetBoolField(TEXT("save"), bSave);
		Ctx.Params->TryGetBoolField(TEXT("replace_nodes"), bReplaceNodes);
	}

	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.pcg.graph_apply.v1"));
	OutData->SetStringField(TEXT("folder_path"), FolderPath);
	OutData->SetStringField(TEXT("asset_path"), AssetPath);
	OutData->SetBoolField(TEXT("dry_run"), bDryRun);
	if (bDryRun)
	{
		TSharedPtr<FJsonObject> Plan;
		FString PlanError;
		CmdPCGGraphPlanFolder(Ctx, Plan, PlanError);
		OutData->SetObjectField(TEXT("plan"), Plan);
		return true;
	}

	TSharedPtr<FJsonObject> Validation;
	FString ValidationError;
	const bool bValidFolder = CmdPCGGraphValidateFolder(Ctx, Validation, ValidationError);
	OutData->SetObjectField(TEXT("validation"), Validation);
	if (!bValidFolder)
	{
		OutData->SetNumberField(TEXT("error_count"), Validation.IsValid() ? Validation->GetIntegerField(TEXT("error_count")) : 1);
		OutError = TEXT("pcg_graph_folder_validation_failed");
		return false;
	}

	UPCGGraph* ExistingGraph = LoadAsset<UPCGGraph>(AssetPath);
	TArray<TSharedPtr<FJsonValue>> PreflightIssues;
	int32 PreflightNodes = 0;
	int32 PreflightEdges = 0;
	const bool bPreflightApplied = PreflightApplyGraphJson(ExistingGraph, GraphJson, bReplaceNodes, PreflightIssues, PreflightNodes, PreflightEdges);
	OutData->SetArrayField(TEXT("preflight_issues"), PreflightIssues);
	OutData->SetNumberField(TEXT("preflight_error_count"), CountErrors(PreflightIssues));
	OutData->SetNumberField(TEXT("preflight_node_count"), PreflightNodes);
	OutData->SetNumberField(TEXT("preflight_edge_count"), PreflightEdges);
	if (!bPreflightApplied)
	{
		OutData->SetNumberField(TEXT("error_count"), CountErrors(PreflightIssues));
		OutError = TEXT("pcg_graph_apply_preflight_failed");
		return false;
	}

	bool bCreated = false;
	UPCGGraph* Graph = ExistingGraph ? ExistingGraph : CreateOrLoadGraphAsset(AssetPath, true, bCreated, OutError);
	if (!Graph)
	{
		return false;
	}

	TSharedPtr<FJsonObject> RollbackGraphJson = bCreated ? nullptr : ExportGraphToJson(Graph);
	FScopedTransaction Transaction(NSLOCTEXT("UeAgentInterfacePCG", "ApplyPCGGraphFolder", "Apply PCG Graph Folder"), true);
	Graph->Modify();

	TArray<TSharedPtr<FJsonValue>> Issues;
	int32 AppliedNodes = 0;
	int32 AppliedEdges = 0;
	const bool bApplied = ApplyGraphJson(Graph, GraphJson, bReplaceNodes, Issues, AppliedNodes, AppliedEdges);
	if (!bApplied)
	{
		TArray<TSharedPtr<FJsonValue>> RollbackIssues;
		int32 RollbackNodes = 0;
		int32 RollbackEdges = 0;
		const bool bRolledBack = RollbackGraphJson.IsValid() ? ApplyGraphJson(Graph, RollbackGraphJson, true, RollbackIssues, RollbackNodes, RollbackEdges) : false;
		OutData->SetBoolField(TEXT("rolled_back"), bRolledBack);
		OutData->SetArrayField(TEXT("rollback_issues"), RollbackIssues);
		OutData->SetNumberField(TEXT("rollback_error_count"), CountErrors(RollbackIssues));
		OutData->SetArrayField(TEXT("issues"), Issues);
		OutData->SetNumberField(TEXT("error_count"), CountErrors(Issues));
		Transaction.Cancel();
		OutError = TEXT("pcg_graph_apply_failed");
		return false;
	}
	if (bApplied && bSave && !SavePackagesForObjects({ Graph }, OutError))
	{
		return false;
	}

	OutData->SetBoolField(TEXT("created"), bCreated);
	OutData->SetBoolField(TEXT("saved"), bApplied && bSave);
	OutData->SetNumberField(TEXT("applied_node_count"), AppliedNodes);
	OutData->SetNumberField(TEXT("applied_edge_count"), AppliedEdges);
	OutData->SetArrayField(TEXT("issues"), Issues);
	OutData->SetNumberField(TEXT("error_count"), CountErrors(Issues));
	return bApplied;
}

bool FUeAgentHttpServer::CmdPCGGraphCompile(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentPCG;
	FString AssetPath;
	if (!GetStringAny(Ctx.Params, { TEXT("asset_path"), TEXT("graph"), TEXT("graph_asset_path") }, AssetPath))
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}
	UPCGGraph* Graph = LoadAsset<UPCGGraph>(AssetPath);
	if (!Graph)
	{
		OutError = TEXT("pcg_graph_asset_not_found");
		return false;
	}

	Graph->Recompile();
	const bool bPrimed = Graph->PrimeGraphCompilationCache();
	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.pcg.graph_compile.v1"));
	OutData->SetStringField(TEXT("asset_path"), Graph->GetOutermost() ? Graph->GetOutermost()->GetName() : AssetPath);
	OutData->SetBoolField(TEXT("recompiled"), true);
	OutData->SetBoolField(TEXT("primed_compilation_cache"), bPrimed);
	OutData->SetObjectField(TEXT("snapshot"), ExportGraphToJson(Graph));
	return true;
}

bool FUeAgentHttpServer::CmdPCGGraphSnapshot(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentPCG;
	FString AssetPath;
	if (!GetStringAny(Ctx.Params, { TEXT("asset_path"), TEXT("graph"), TEXT("graph_asset_path") }, AssetPath))
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}
	UPCGGraph* Graph = LoadAsset<UPCGGraph>(AssetPath);
	if (!Graph)
	{
		OutError = TEXT("pcg_graph_asset_not_found");
		return false;
	}
	OutData = ExportGraphToJson(Graph);
	return true;
}

bool FUeAgentHttpServer::CmdPCGGraphLayout(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentPCG;
	FString AssetPath;
	if (!GetStringAny(Ctx.Params, { TEXT("asset_path"), TEXT("graph"), TEXT("graph_asset_path") }, AssetPath))
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}
	UPCGGraph* Graph = LoadAsset<UPCGGraph>(AssetPath);
	if (!Graph)
	{
		OutError = TEXT("pcg_graph_asset_not_found");
		return false;
	}
	const bool bHadMaterializedEditorGraph = PCGGraphHasMaterializedEditorGraph(Graph);
	bool bOpenedEditorForGraphInit = false;
	const bool bEditorGraphReady = EnsurePCGEditorGraphMaterialized(Graph, bOpenedEditorForGraphInit);
	if (!bEditorGraphReady)
	{
		const FString CanonicalAssetPath = Graph->GetOutermost() ? Graph->GetOutermost()->GetName() : NormalizeAssetPath(AssetPath);
		OutData = MakeShared<FJsonObject>();
		OutData->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.pcg.graph_layout.v1"));
		OutData->SetStringField(TEXT("asset_path"), CanonicalAssetPath);
		OutData->SetStringField(TEXT("asset_class"), Graph->GetClass()->GetPathName());
		OutData->SetStringField(TEXT("delegated_command"), TEXT("node_graph_layout"));
		OutData->SetStringField(TEXT("pcg_graph_layout_strategy"), TEXT("shared_node_graph_blueprint_layout_core"));
		OutData->SetBoolField(TEXT("pcg_editor_graph_had_materialized_edgraph_before_init"), bHadMaterializedEditorGraph);
		OutData->SetBoolField(TEXT("pcg_editor_graph_opened_editor_for_init"), bOpenedEditorForGraphInit);
		OutError = TEXT("pcg_editor_graph_not_materialized");
		return false;
	}

	TSharedPtr<FJsonObject> LayoutParams = MakeShared<FJsonObject>();
	if (Ctx.Params.IsValid())
	{
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Ctx.Params->Values)
		{
			LayoutParams->SetField(Pair.Key, Pair.Value);
		}
	}

	const FString CanonicalAssetPath = Graph->GetOutermost() ? Graph->GetOutermost()->GetName() : NormalizeAssetPath(AssetPath);
	LayoutParams->SetStringField(TEXT("asset_path"), CanonicalAssetPath);
	LayoutParams->SetStringField(TEXT("adapter"), TEXT("generic_edgraph"));

	bool bSaveAfterLayout = false;
	if (Ctx.Params.IsValid())
	{
		bool bSaveSpecified = Ctx.Params->TryGetBoolField(TEXT("save_after_layout"), bSaveAfterLayout);
		if (!bSaveSpecified)
		{
			bSaveSpecified = Ctx.Params->TryGetBoolField(TEXT("save"), bSaveAfterLayout);
		}
		const TSharedPtr<FJsonObject>* LayoutOptions = nullptr;
		if (!bSaveSpecified
			&& Ctx.Params->TryGetObjectField(TEXT("layout_options"), LayoutOptions)
			&& LayoutOptions
			&& LayoutOptions->IsValid())
		{
			bSaveSpecified = (*LayoutOptions)->TryGetBoolField(TEXT("save_after_layout"), bSaveAfterLayout);
			if (!bSaveSpecified)
			{
				bSaveSpecified = (*LayoutOptions)->TryGetBoolField(TEXT("save"), bSaveAfterLayout);
			}
		}
		if (bSaveSpecified && !LayoutParams->HasField(TEXT("save_after_layout")))
		{
			LayoutParams->SetBoolField(TEXT("save_after_layout"), bSaveAfterLayout);
		}
	}

	bool bCompileAfterLayout = false;
	if (Ctx.Params.IsValid())
	{
		Ctx.Params->TryGetBoolField(TEXT("compile_after_layout"), bCompileAfterLayout);
		if (!bCompileAfterLayout)
		{
			Ctx.Params->TryGetBoolField(TEXT("compile"), bCompileAfterLayout);
		}
		const TSharedPtr<FJsonObject>* LayoutOptions = nullptr;
		if (!bCompileAfterLayout
			&& Ctx.Params->TryGetObjectField(TEXT("layout_options"), LayoutOptions)
			&& LayoutOptions
			&& LayoutOptions->IsValid())
		{
			(*LayoutOptions)->TryGetBoolField(TEXT("compile_after_layout"), bCompileAfterLayout);
			if (!bCompileAfterLayout)
			{
				(*LayoutOptions)->TryGetBoolField(TEXT("compile"), bCompileAfterLayout);
			}
		}
	}

	FUeAgentRequestContext LayoutCtx = Ctx;
	LayoutCtx.Command = TEXT("node_graph_layout");
	LayoutCtx.Params = LayoutParams;
	TSharedPtr<FJsonObject> LayoutData;
	FString LayoutError;
	if (!CmdNodeGraphLayout(LayoutCtx, LayoutData, LayoutError))
	{
		OutData = LayoutData.IsValid() ? LayoutData : MakeShared<FJsonObject>();
		OutData->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.pcg.graph_layout.v1"));
		OutData->SetStringField(TEXT("asset_path"), CanonicalAssetPath);
		OutData->SetStringField(TEXT("delegated_command"), TEXT("node_graph_layout"));
		OutData->SetStringField(TEXT("pcg_graph_layout_strategy"), TEXT("shared_node_graph_blueprint_layout_core"));
		OutData->SetBoolField(TEXT("pcg_editor_graph_had_materialized_edgraph_before_init"), bHadMaterializedEditorGraph);
		OutData->SetBoolField(TEXT("pcg_editor_graph_opened_editor_for_init"), bOpenedEditorForGraphInit);
		OutError = LayoutError.IsEmpty() ? TEXT("pcg_graph_layout_failed") : LayoutError;
		return false;
	}

	OutData = LayoutData.IsValid() ? LayoutData : MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.pcg.graph_layout.v1"));
	OutData->SetStringField(TEXT("asset_path"), CanonicalAssetPath);
	OutData->SetStringField(TEXT("asset_class"), Graph->GetClass()->GetPathName());
	OutData->SetStringField(TEXT("delegated_command"), TEXT("node_graph_layout"));
	OutData->SetStringField(TEXT("pcg_graph_layout_strategy"), TEXT("shared_node_graph_blueprint_layout_core"));
	OutData->SetBoolField(TEXT("pcg_editor_graph_had_materialized_edgraph_before_init"), bHadMaterializedEditorGraph);
	OutData->SetBoolField(TEXT("pcg_editor_graph_opened_editor_for_init"), bOpenedEditorForGraphInit);
	bool bLayoutDesignComplete = false;
	OutData->TryGetBoolField(TEXT("layout_design_complete"), bLayoutDesignComplete);
	OutData->SetBoolField(TEXT("pcg_graph_layout_complete"), bLayoutDesignComplete);

	if (bCompileAfterLayout)
	{
		FUeAgentRequestContext CompileCtx = Ctx;
		CompileCtx.Command = TEXT("pcg_graph_compile");
		CompileCtx.Params = MakeShared<FJsonObject>();
		CompileCtx.Params->SetStringField(TEXT("asset_path"), CanonicalAssetPath);
		TSharedPtr<FJsonObject> CompileData;
		FString CompileError;
		if (!CmdPCGGraphCompile(CompileCtx, CompileData, CompileError))
		{
			OutData->SetBoolField(TEXT("compiled"), false);
			OutData->SetStringField(TEXT("compile_error"), CompileError);
			OutError = TEXT("pcg_graph_layout_compile_failed");
			return false;
		}
		OutData->SetBoolField(TEXT("compiled"), true);
		if (CompileData.IsValid())
		{
			OutData->SetObjectField(TEXT("compile_result"), CompileData);
		}
		if (bSaveAfterLayout)
		{
			FString SaveError;
			if (!SavePackagesForObjects({ Graph }, SaveError))
			{
				OutData->SetBoolField(TEXT("saved_after_compile"), false);
				OutData->SetStringField(TEXT("save_after_compile_error"), SaveError);
				OutError = TEXT("pcg_graph_layout_save_after_compile_failed");
				return false;
			}
			OutData->SetBoolField(TEXT("saved"), true);
			OutData->SetBoolField(TEXT("saved_after_compile"), true);
		}
	}
	else
	{
		OutData->SetBoolField(TEXT("compiled"), false);
	}
	return true;
}

bool FUeAgentHttpServer::CmdPCGGraphInstanceJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentPCG;
	FString AssetPath;
	if (!GetStringAny(Ctx.Params, { TEXT("asset_path"), TEXT("instance_asset_path") }, AssetPath))
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}

	bool bCreate = false;
	bool bSave = true;
	FString GraphPath;
	if (Ctx.Params.IsValid())
	{
		Ctx.Params->TryGetBoolField(TEXT("create"), bCreate);
		Ctx.Params->TryGetBoolField(TEXT("save"), bSave);
		Ctx.Params->TryGetStringField(TEXT("graph"), GraphPath);
		Ctx.Params->TryGetStringField(TEXT("graph_asset_path"), GraphPath);
	}

	UPCGGraphInstance* Instance = LoadAsset<UPCGGraphInstance>(AssetPath);
	UPCGGraphInterface* SourceGraphInterface = nullptr;
	if (!GraphPath.IsEmpty())
	{
		SourceGraphInterface = LoadAsset<UPCGGraphInterface>(GraphPath);
		if (!SourceGraphInterface)
		{
			OutError = TEXT("source_graph_not_found");
			return false;
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* Parameters = nullptr;
	const bool bHasParameters = Ctx.Params.IsValid() && Ctx.Params->TryGetArrayField(TEXT("parameters"), Parameters);
	if (bHasParameters)
	{
		UPCGGraphInstance* PreviewInstance = Instance ? DuplicateObject<UPCGGraphInstance>(Instance, GetTransientPackage()) : NewObject<UPCGGraphInstance>(GetTransientPackage(), UPCGGraphInstance::StaticClass(), NAME_None, RF_Transient);
		if (!PreviewInstance)
		{
			OutError = TEXT("create_graph_instance_preflight_failed");
			return false;
		}
		PreviewInstance->SetFlags(RF_Transient);
		if (SourceGraphInterface)
		{
			PreviewInstance->SetGraph(SourceGraphInterface);
		}
		RefreshGraphInstanceParameters(PreviewInstance);
		TArray<TSharedPtr<FJsonValue>> PreflightIssues;
		PreflightParameterArray(&PreviewInstance->ParametersOverrides.Parameters, Parameters, PreflightIssues, false);
		if (CountErrors(PreflightIssues) > 0)
		{
			OutError = TEXT("graph_instance_parameter_preflight_failed");
			OutData = MakeShared<FJsonObject>();
			OutData->SetArrayField(TEXT("issues"), PreflightIssues);
			OutData->SetNumberField(TEXT("error_count"), CountErrors(PreflightIssues));
			return false;
		}
	}

	bool bCreated = false;
	if (!Instance && bCreate)
	{
		const FString NormalizedAssetPath = NormalizeAssetPath(AssetPath);
		if (!FPackageName::IsValidLongPackageName(NormalizedAssetPath))
		{
			OutError = TEXT("invalid_asset_path");
			return false;
		}
		UPackage* Package = CreatePackage(*NormalizedAssetPath);
		if (!Package)
		{
			OutError = TEXT("create_package_failed");
			return false;
		}
		Instance = NewObject<UPCGGraphInstance>(Package, UPCGGraphInstance::StaticClass(), *FPackageName::GetLongPackageAssetName(NormalizedAssetPath), RF_Public | RF_Standalone | RF_Transactional);
		if (!Instance)
		{
			OutError = TEXT("create_graph_instance_failed");
			return false;
		}
		FAssetRegistryModule::AssetCreated(Instance);
		Instance->MarkPackageDirty();
		Package->MarkPackageDirty();
		bCreated = true;
	}
	if (!Instance)
	{
		OutError = TEXT("pcg_graph_instance_not_found");
		return false;
	}

	FScopedTransaction Transaction(NSLOCTEXT("UeAgentInterfacePCG", "ApplyPCGGraphInstance", "Apply PCG Graph Instance"), bCreated || SourceGraphInterface || bHasParameters);
	Instance->Modify();

	if (SourceGraphInterface)
	{
		Instance->SetGraph(SourceGraphInterface);
		Instance->MarkPackageDirty();
	}
	if (bHasParameters)
	{
		TArray<TSharedPtr<FJsonValue>> Issues;
		TArray<FName> AppliedNames;
		RefreshGraphInstanceParameters(Instance);
		ApplyParameterArray(Instance->ParametersOverrides.Parameters, Parameters, Issues, &AppliedNames, false);
		NotifyGraphInstanceParameterValuesChanged(Instance, AppliedNames);
		Instance->MarkPackageDirty();
		if (CountErrors(Issues) > 0)
		{
			Transaction.Cancel();
			OutError = TEXT("graph_instance_parameter_apply_failed");
			OutData = MakeShared<FJsonObject>();
			OutData->SetArrayField(TEXT("issues"), Issues);
			OutData->SetNumberField(TEXT("error_count"), CountErrors(Issues));
			return false;
		}
	}
	if (bSave && (bCreated || SourceGraphInterface || bHasParameters))
	{
		if (!SavePackagesForObjects({ Instance }, OutError))
		{
			return false;
		}
	}

	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.pcg.graph_instance.v1"));
	OutData->SetStringField(TEXT("asset_path"), Instance->GetOutermost() ? Instance->GetOutermost()->GetName() : AssetPath);
	OutData->SetStringField(TEXT("object_path"), Instance->GetPathName());
	OutData->SetStringField(TEXT("graph"), Instance->Graph ? Instance->Graph->GetPathName() : FString());
	OutData->SetBoolField(TEXT("created"), bCreated);
	OutData->SetObjectField(TEXT("parameters"), ExportParameterBagToJson(Instance->GetUserParametersStruct()));
	return true;
}

bool FUeAgentHttpServer::CmdPCGGraphParametersQuery(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentPCG;
	FString AssetPath;
	if (!GetStringAny(Ctx.Params, { TEXT("asset_path"), TEXT("graph"), TEXT("instance_asset_path") }, AssetPath))
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}
	UPCGGraphInterface* GraphInterface = LoadAsset<UPCGGraphInterface>(AssetPath);
	if (!GraphInterface)
	{
		OutError = TEXT("pcg_graph_interface_not_found");
		return false;
	}
	OutData = ExportParameterBagToJson(GraphInterface->GetUserParametersStruct());
	OutData->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.pcg.graph_parameters.v1"));
	OutData->SetStringField(TEXT("asset_path"), GraphInterface->GetOutermost() ? GraphInterface->GetOutermost()->GetName() : AssetPath);
	return true;
}

bool FUeAgentHttpServer::CmdPCGGraphParametersApplyJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentPCG;
	FString AssetPath;
	if (!GetStringAny(Ctx.Params, { TEXT("asset_path"), TEXT("graph"), TEXT("instance_asset_path") }, AssetPath))
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}
	UPCGGraphInterface* GraphInterface = LoadAsset<UPCGGraphInterface>(AssetPath);
	if (!GraphInterface)
	{
		OutError = TEXT("pcg_graph_interface_not_found");
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* Parameters = nullptr;
	if (!Ctx.Params.IsValid() || !Ctx.Params->TryGetArrayField(TEXT("parameters"), Parameters))
	{
		OutError = TEXT("missing_parameters");
		return false;
	}
	bool bSave = true;
	Ctx.Params->TryGetBoolField(TEXT("save"), bSave);

	TArray<TSharedPtr<FJsonValue>> Issues;
	int32 PreflightAppliedCount = 0;
	if (UPCGGraph* Graph = Cast<UPCGGraph>(GraphInterface))
	{
		PreflightAppliedCount = PreflightParameterArray(Graph->GetUserParametersStruct(), Parameters, Issues, true);
	}
	else if (UPCGGraphInstance* Instance = Cast<UPCGGraphInstance>(GraphInterface))
	{
		UPCGGraphInstance* PreviewInstance = DuplicateObject<UPCGGraphInstance>(Instance, GetTransientPackage());
		if (!PreviewInstance)
		{
			OutError = TEXT("graph_parameters_preflight_failed");
			return false;
		}
		PreviewInstance->SetFlags(RF_Transient);
		RefreshGraphInstanceParameters(PreviewInstance);
		PreflightAppliedCount = PreflightParameterArray(&PreviewInstance->ParametersOverrides.Parameters, Parameters, Issues, false);
	}
	if (CountErrors(Issues) > 0)
	{
		OutError = TEXT("graph_parameters_preflight_failed");
		OutData = ExportParameterBagToJson(GraphInterface->GetUserParametersStruct());
		OutData->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.pcg.graph_parameters_apply.v1"));
		OutData->SetNumberField(TEXT("applied_count"), 0);
		OutData->SetNumberField(TEXT("preflight_applied_count"), PreflightAppliedCount);
		OutData->SetArrayField(TEXT("issues"), Issues);
		OutData->SetNumberField(TEXT("error_count"), CountErrors(Issues));
		return false;
	}

	int32 AppliedCount = 0;
	FScopedTransaction Transaction(NSLOCTEXT("UeAgentInterfacePCG", "ApplyPCGGraphParameters", "Apply PCG Graph Parameters"), true);
	GraphInterface->Modify();
	if (UPCGGraph* Graph = Cast<UPCGGraph>(GraphInterface))
	{
		Graph->UpdateUserParametersStruct([&](FInstancedPropertyBag& Bag)
		{
			AppliedCount = ApplyParameterArray(Bag, Parameters, Issues);
		});
		Graph->MarkPackageDirty();
	}
	else if (UPCGGraphInstance* Instance = Cast<UPCGGraphInstance>(GraphInterface))
	{
		TArray<FName> AppliedNames;
		RefreshGraphInstanceParameters(Instance);
		AppliedCount = ApplyParameterArray(Instance->ParametersOverrides.Parameters, Parameters, Issues, &AppliedNames, false);
		NotifyGraphInstanceParameterValuesChanged(Instance, AppliedNames);
		Instance->MarkPackageDirty();
	}
	if (CountErrors(Issues) > 0)
	{
		Transaction.Cancel();
		OutError = TEXT("graph_parameters_apply_failed");
		OutData = ExportParameterBagToJson(GraphInterface->GetUserParametersStruct());
		OutData->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.pcg.graph_parameters_apply.v1"));
		OutData->SetNumberField(TEXT("applied_count"), AppliedCount);
		OutData->SetArrayField(TEXT("issues"), Issues);
		OutData->SetNumberField(TEXT("error_count"), CountErrors(Issues));
		return false;
	}

	if (CountErrors(Issues) == 0 && bSave && !SavePackagesForObjects({ GraphInterface }, OutError))
	{
		return false;
	}

	OutData = ExportParameterBagToJson(GraphInterface->GetUserParametersStruct());
	OutData->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.pcg.graph_parameters_apply.v1"));
	OutData->SetNumberField(TEXT("applied_count"), AppliedCount);
	OutData->SetArrayField(TEXT("issues"), Issues);
	OutData->SetNumberField(TEXT("error_count"), CountErrors(Issues));
	return CountErrors(Issues) == 0;
}

bool FUeAgentHttpServer::CmdPCGComponentQuery(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentPCG;
	FString WorldError;
	UWorld* World = GetEditorWorld(WorldError);
	if (!World)
	{
		OutError = WorldError;
		return false;
	}
	UPCGComponent* Component = FindPCGComponent(World, Ctx.Params, OutError);
	if (!Component)
	{
		return false;
	}
	OutData = ComponentToJson(Component);
	return true;
}

bool FUeAgentHttpServer::CmdPCGComponentApplyJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentPCG;
	FString WorldError;
	UWorld* World = GetEditorWorld(WorldError);
	if (!World)
	{
		OutError = WorldError;
		return false;
	}

	FString ActorName;
	if (!GetStringAny(Ctx.Params, { TEXT("actor"), TEXT("actor_name"), TEXT("actor_label") }, ActorName))
	{
		OutError = TEXT("missing_actor");
		return false;
	}
	AActor* Actor = FindActorByNameOrLabelDirect(World, ActorName);
	if (!Actor)
	{
		OutError = TEXT("actor_not_found");
		return false;
	}

	FString ComponentName;
	GetStringAny(Ctx.Params, { TEXT("component"), TEXT("component_name"), TEXT("pcg_component") }, ComponentName);
	if (ComponentName.IsEmpty())
	{
		ComponentName = TEXT("PCGComponent");
	}

	FString GraphPath;
	UPCGGraphInterface* GraphInterface = nullptr;
	if (GetStringAny(Ctx.Params, { TEXT("graph"), TEXT("graph_asset_path"), TEXT("asset_path") }, GraphPath))
	{
		GraphInterface = LoadAsset<UPCGGraphInterface>(GraphPath);
		if (!GraphInterface)
		{
			OutError = TEXT("pcg_graph_interface_not_found");
			return false;
		}
	}

	FString TriggerText;
	EPCGComponentGenerationTrigger ParsedTrigger = EPCGComponentGenerationTrigger::GenerateOnDemand;
	const bool bHasGenerationTrigger = Ctx.Params->TryGetStringField(TEXT("generation_trigger"), TriggerText);
	if (bHasGenerationTrigger && !TryParseGenerationTrigger(TriggerText, ParsedTrigger))
	{
		OutError = TEXT("invalid_generation_trigger");
		return false;
	}

	UPCGComponent* Component = nullptr;
	TArray<UPCGComponent*> Components;
	Actor->GetComponents<UPCGComponent>(Components);
	for (UPCGComponent* Existing : Components)
	{
		if (Existing && Existing->GetName().Equals(ComponentName, ESearchCase::IgnoreCase))
		{
			Component = Existing;
			break;
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* Properties = nullptr;
	TArray<TSharedPtr<FJsonValue>> Issues;
	if (Ctx.Params->TryGetArrayField(TEXT("properties"), Properties))
	{
		UPCGComponent* PreviewComponent = Component ? DuplicateObject<UPCGComponent>(Component, GetTransientPackage()) : NewObject<UPCGComponent>(GetTransientPackage(), UPCGComponent::StaticClass(), NAME_None, RF_Transient);
		if (!PreviewComponent)
		{
			OutError = TEXT("component_property_preflight_failed");
			return false;
		}
		PreviewComponent->SetFlags(RF_Transient);
		if (GraphInterface)
		{
			PreviewComponent->SetGraph(GraphInterface);
		}
		ApplyPropertyArray(PreviewComponent, Properties, Issues, TEXT("component.properties"));
		if (CountErrors(Issues) > 0)
		{
			OutError = TEXT("component_property_preflight_failed");
			OutData = MakeShared<FJsonObject>();
			OutData->SetArrayField(TEXT("issues"), Issues);
			OutData->SetNumberField(TEXT("error_count"), CountErrors(Issues));
			return false;
		}
	}

	bool bCreated = false;
	FScopedTransaction Transaction(NSLOCTEXT("UeAgentInterfacePCG", "ApplyPCGComponent", "Apply PCG Component"), true);
	Actor->Modify();
	if (!Component)
	{
		Component = NewObject<UPCGComponent>(Actor, *ComponentName, RF_Transactional);
		if (!Component)
		{
			Transaction.Cancel();
			OutError = TEXT("create_pcg_component_failed");
			return false;
		}
		Actor->AddInstanceComponent(Component);
		Component->RegisterComponent();
		bCreated = true;
	}
	Component->Modify();

	if (GraphInterface)
	{
		Component->SetGraph(GraphInterface);
	}

	double NumberValue = 0.0;
	if (Ctx.Params->TryGetNumberField(TEXT("seed"), NumberValue))
	{
		Component->Seed = static_cast<int32>(NumberValue);
	}
	bool BoolValue = false;
	if (Ctx.Params->TryGetBoolField(TEXT("activated"), BoolValue))
	{
		Component->bActivated = BoolValue;
	}
	if (Ctx.Params->TryGetBoolField(TEXT("partitioned"), BoolValue))
	{
		Component->bIsComponentPartitioned = BoolValue;
	}
	if (bHasGenerationTrigger)
	{
		Component->GenerationTrigger = ParsedTrigger;
	}
	if (Properties)
	{
		ApplyPropertyArray(Component, Properties, Issues, TEXT("component.properties"));
		if (CountErrors(Issues) > 0)
		{
			Transaction.Cancel();
			OutError = TEXT("component_property_apply_failed");
			OutData = ComponentToJson(Component);
			OutData->SetBoolField(TEXT("created"), bCreated);
			OutData->SetArrayField(TEXT("issues"), Issues);
			OutData->SetNumberField(TEXT("error_count"), CountErrors(Issues));
			return false;
		}
	}

	bool bSaveLevel = false;
	Ctx.Params->TryGetBoolField(TEXT("save_level"), bSaveLevel);
	Actor->MarkPackageDirty();
	if (bSaveLevel)
	{
		if (!UEditorLoadingAndSavingUtils::SaveDirtyPackages(true, true))
		{
			OutError = TEXT("save_level_failed");
			return false;
		}
	}

	OutData = ComponentToJson(Component);
	OutData->SetBoolField(TEXT("created"), bCreated);
	OutData->SetArrayField(TEXT("issues"), Issues);
	OutData->SetNumberField(TEXT("error_count"), CountErrors(Issues));
	return CountErrors(Issues) == 0;
}

bool FUeAgentHttpServer::CmdPCGComponentSnapshotJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	return CmdPCGComponentQuery(Ctx, OutData, OutError);
}

bool FUeAgentHttpServer::CmdPCGWorldActorQuery(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentPCG;
	FString WorldError;
	UWorld* World = GetEditorWorld(WorldError);
	if (!World)
	{
		OutError = WorldError;
		return false;
	}

	TArray<TSharedPtr<FJsonValue>> Actors;
	for (TActorIterator<APCGWorldActor> It(World); It; ++It)
	{
		APCGWorldActor* Actor = *It;
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), Actor->GetName());
#if WITH_EDITOR
		Obj->SetStringField(TEXT("label"), Actor->GetActorLabel());
		Obj->SetBoolField(TEXT("treat_editor_viewport_as_generation_source"), Actor->bTreatEditorViewportAsGenerationSource);
#endif
		Obj->SetNumberField(TEXT("partition_grid_size"), Actor->PartitionGridSize);
		Obj->SetBoolField(TEXT("enable_world_partition_generation_sources"), Actor->bEnableWorldPartitionGenerationSources);
		Actors.Add(MakeShared<FJsonValueObject>(Obj));
	}

	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.pcg.world_actor.v1"));
	OutData->SetArrayField(TEXT("world_actors"), Actors);
	OutData->SetNumberField(TEXT("world_actor_count"), Actors.Num());
	return true;
}

bool FUeAgentHttpServer::CmdPCGWorldActorApplyJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentPCG;
	FString WorldError;
	UWorld* World = GetEditorWorld(WorldError);
	if (!World)
	{
		OutError = WorldError;
		return false;
	}

	double RequestedPartitionGridSize = 0.0;
	const bool bHasPartitionGridSize = Ctx.Params.IsValid() && Ctx.Params->TryGetNumberField(TEXT("partition_grid_size"), RequestedPartitionGridSize);
	if (bHasPartitionGridSize && RequestedPartitionGridSize <= 0.0)
	{
		OutError = TEXT("invalid_partition_grid_size");
		return false;
	}

	APCGWorldActor* WorldActor = nullptr;
	for (TActorIterator<APCGWorldActor> It(World); It; ++It)
	{
		WorldActor = *It;
		break;
	}
	bool bCreated = false;
#if WITH_EDITOR
	if (!WorldActor)
	{
		FActorSpawnParameters SpawnParams;
		SpawnParams.Name = MakeUniqueObjectName(World, APCGWorldActor::StaticClass(), TEXT("PCGWorldActor"));
		SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		SpawnParams.ObjectFlags |= RF_Transactional;
		WorldActor = World->SpawnActor<APCGWorldActor>(APCGWorldActor::StaticClass(), FTransform::Identity, SpawnParams);
		bCreated = WorldActor != nullptr;
	}
#endif
	if (!WorldActor)
	{
		OutError = TEXT("pcg_world_actor_create_failed");
		return false;
	}

	double NumberValue = 0.0;
	if (bHasPartitionGridSize)
	{
		WorldActor->PartitionGridSize = static_cast<uint32>(RequestedPartitionGridSize);
	}
	bool BoolValue = false;
	if (Ctx.Params->TryGetBoolField(TEXT("enable_world_partition_generation_sources"), BoolValue))
	{
		WorldActor->bEnableWorldPartitionGenerationSources = BoolValue;
	}
#if WITH_EDITOR
	if (Ctx.Params->TryGetBoolField(TEXT("treat_editor_viewport_as_generation_source"), BoolValue))
	{
		WorldActor->bTreatEditorViewportAsGenerationSource = BoolValue;
	}
#endif
	WorldActor->MarkPackageDirty();

	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.pcg.world_actor_apply.v1"));
	OutData->SetBoolField(TEXT("created"), bCreated);
	OutData->SetStringField(TEXT("name"), WorldActor->GetName());
	OutData->SetNumberField(TEXT("partition_grid_size"), WorldActor->PartitionGridSize);
	OutData->SetBoolField(TEXT("enable_world_partition_generation_sources"), WorldActor->bEnableWorldPartitionGenerationSources);
	return true;
}

bool FUeAgentHttpServer::CmdPCGPartitionProbe(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentPCG;
	FString WorldError;
	UWorld* World = GetEditorWorld(WorldError);
	if (!World)
	{
		OutError = WorldError;
		return false;
	}

	TArray<TSharedPtr<FJsonValue>> Partitions;
	for (TActorIterator<APCGPartitionActor> It(World); It; ++It)
	{
		APCGPartitionActor* Actor = *It;
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), Actor->GetName());
#if WITH_EDITOR
		Obj->SetStringField(TEXT("label"), Actor->GetActorLabel());
#endif
		Obj->SetNumberField(TEXT("pcg_grid_size"), Actor->GetPCGGridSize());
		Obj->SetBoolField(TEXT("using_2d_grid"), Actor->IsUsing2DGrid());
		Obj->SetBoolField(TEXT("runtime_generated"), Actor->IsRuntimeGenerated());
		Obj->SetBoolField(TEXT("has_local_pcg_components"), Actor->HasLocalPCGComponents());
		Obj->SetNumberField(TEXT("local_component_count"), Actor->GetAllLocalPCGComponents().Num());
		Obj->SetNumberField(TEXT("original_component_count"), Actor->GetAllOriginalPCGComponents().Num());
		Partitions.Add(MakeShared<FJsonValueObject>(Obj));
	}

	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.pcg.partition_probe.v1"));
	OutData->SetArrayField(TEXT("partition_actors"), Partitions);
	OutData->SetNumberField(TEXT("partition_actor_count"), Partitions.Num());
	return true;
}

bool FUeAgentHttpServer::CmdPCGGenerate(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentPCG;
	FString WorldError;
	UWorld* World = GetEditorWorld(WorldError);
	if (!World)
	{
		OutError = WorldError;
		return false;
	}
	UPCGComponent* Component = FindPCGComponent(World, Ctx.Params, OutError);
	if (!Component)
	{
		return false;
	}
	bool bForce = true;
	bool bWait = false;
	Ctx.Params->TryGetBoolField(TEXT("force"), bForce);
	Ctx.Params->TryGetBoolField(TEXT("wait"), bWait);
	FString TriggerText;
	const bool bHasGenerationTrigger = Ctx.Params->TryGetStringField(TEXT("generation_trigger"), TriggerText);
	EPCGComponentGenerationTrigger Trigger = EPCGComponentGenerationTrigger::GenerateOnDemand;
	if (bHasGenerationTrigger && !TryParseGenerationTrigger(TriggerText, Trigger))
	{
		OutError = TEXT("invalid_generation_trigger");
		return false;
	}
	const FPCGTaskId TaskId = Component->GenerateLocalGetTaskId(Trigger, bForce);
	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.pcg.generate.v1"));
	OutData->SetNumberField(TEXT("task_id"), static_cast<double>(TaskId));
	SetActionStatus(OutData, Component);
	if (bWait)
	{
		TSharedPtr<FJsonObject> WaitData;
		FString WaitError;
		CmdPCGWait(Ctx, WaitData, WaitError);
		OutData->SetObjectField(TEXT("wait"), WaitData);
	}
	return true;
}

bool FUeAgentHttpServer::CmdPCGCleanup(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentPCG;
	FString WorldError;
	UWorld* World = GetEditorWorld(WorldError);
	if (!World)
	{
		OutError = WorldError;
		return false;
	}
	UPCGComponent* Component = FindPCGComponent(World, Ctx.Params, OutError);
	if (!Component)
	{
		return false;
	}
	bool bRemoveComponents = true;
	Ctx.Params->TryGetBoolField(TEXT("remove_components"), bRemoveComponents);
	const TArray<FPCGTaskId> Dependencies;
	const FPCGTaskId TaskId = Component->CleanupLocal(bRemoveComponents, Dependencies);
	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.pcg.cleanup.v1"));
	OutData->SetNumberField(TEXT("task_id"), static_cast<double>(TaskId));
	SetActionStatus(OutData, Component);
	return true;
}

bool FUeAgentHttpServer::CmdPCGRegenerate(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	TSharedPtr<FJsonObject> CleanupData;
	if (!CmdPCGCleanup(Ctx, CleanupData, OutError))
	{
		return false;
	}
	TSharedPtr<FJsonObject> GenerateData;
	if (!CmdPCGGenerate(Ctx, GenerateData, OutError))
	{
		return false;
	}
	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.pcg.regenerate.v1"));
	OutData->SetObjectField(TEXT("cleanup"), CleanupData);
	OutData->SetObjectField(TEXT("generate"), GenerateData);
	return true;
}

bool FUeAgentHttpServer::CmdPCGCancelGeneration(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentPCG;
	FString WorldError;
	UWorld* World = GetEditorWorld(WorldError);
	if (!World)
	{
		OutError = WorldError;
		return false;
	}
	UPCGComponent* Component = FindPCGComponent(World, Ctx.Params, OutError);
	if (!Component)
	{
		return false;
	}
	Component->CancelGeneration();
	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.pcg.cancel_generation.v1"));
	SetActionStatus(OutData, Component);
	return true;
}

bool FUeAgentHttpServer::CmdPCGGenerationStatus(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	return CmdPCGComponentQuery(Ctx, OutData, OutError);
}

bool FUeAgentHttpServer::CmdPCGWait(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentPCG;
	FString WorldError;
	UWorld* World = GetEditorWorld(WorldError);
	if (!World)
	{
		OutError = WorldError;
		return false;
	}
	UPCGComponent* Component = FindPCGComponent(World, Ctx.Params, OutError);
	if (!Component)
	{
		return false;
	}
	double TimeoutSeconds = 10.0;
	Ctx.Params->TryGetNumberField(TEXT("timeout_seconds"), TimeoutSeconds);
	const double StartTime = FPlatformTime::Seconds();
	double LastTickTime = StartTime;
	int32 Iterations = 0;
	UPCGSubsystem* PCGSubsystem = UPCGSubsystem::GetInstance(World);
	while ((Component->IsGenerating() || Component->IsCleaningUp()) && FPlatformTime::Seconds() - StartTime < TimeoutSeconds)
	{
		const double Now = FPlatformTime::Seconds();
		const float DeltaSeconds = FMath::Clamp(static_cast<float>(Now - LastTickTime), 0.001f, 0.05f);
		LastTickTime = Now;
		if (PCGSubsystem)
		{
			PCGSubsystem->Tick(DeltaSeconds);
		}
		FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
		FPlatformProcess::Sleep(0.005f);
		Iterations++;
	}
	const bool bTimedOut = Component->IsGenerating() || Component->IsCleaningUp();
	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.pcg.wait.v1"));
	OutData->SetBoolField(TEXT("completed"), !bTimedOut);
	OutData->SetBoolField(TEXT("timed_out"), bTimedOut);
	OutData->SetNumberField(TEXT("iterations"), Iterations);
	OutData->SetNumberField(TEXT("elapsed_seconds"), FPlatformTime::Seconds() - StartTime);
	SetActionStatus(OutData, Component);
	return !bTimedOut;
}

bool FUeAgentHttpServer::CmdPCGRuntimeGenerationProbe(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentPCG;
	TSharedPtr<FJsonObject> ComponentData;
	if (!CmdPCGComponentQuery(Ctx, ComponentData, OutError))
	{
		return false;
	}
	OutData = ComponentData;
	OutData->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.pcg.runtime_generation_probe.v1"));
	return true;
}

bool FUeAgentHttpServer::CmdPCGInspectionSet(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentPCG;
	FString WorldError;
	UWorld* World = GetEditorWorld(WorldError);
	if (!World)
	{
		OutError = WorldError;
		return false;
	}
	UPCGComponent* Component = FindPCGComponent(World, Ctx.Params, OutError);
	if (!Component)
	{
		return false;
	}
	bool bEnabled = true;
	Ctx.Params->TryGetBoolField(TEXT("enabled"), bEnabled);
	if (bEnabled)
	{
		Component->GetExecutionState().GetInspection().EnableInspection();
	}
	else
	{
		Component->GetExecutionState().GetInspection().DisableInspection();
	}
	bool bClear = false;
	Ctx.Params->TryGetBoolField(TEXT("clear"), bClear);
	if (bClear)
	{
		Component->GetExecutionState().GetInspection().ClearInspectionData(true);
	}
	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.pcg.inspection_set.v1"));
	OutData->SetBoolField(TEXT("enabled"), Component->GetExecutionState().GetInspection().IsInspecting());
	SetActionStatus(OutData, Component);
	return true;
}

bool FUeAgentHttpServer::CmdPCGNodeInspect(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentPCG;
	FString WorldError;
	UWorld* World = GetEditorWorld(WorldError);
	if (!World)
	{
		OutError = WorldError;
		return false;
	}
	UPCGComponent* Component = FindPCGComponent(World, Ctx.Params, OutError);
	if (!Component)
	{
		return false;
	}
	FString NodeIdOrTitle;
	GetStringAny(Ctx.Params, { TEXT("node"), TEXT("node_id"), TEXT("node_title") }, NodeIdOrTitle);

	UPCGGraph* Graph = Component->GetGraph();
	TArray<TSharedPtr<FJsonValue>> Nodes;
	if (Graph)
	{
		TSet<FString> UsedIds;
		for (UPCGNode* Node : Graph->GetNodes())
		{
			const FString NodeId = MakeStableNodeId(Graph, Node, UsedIds);
			if (NodeIdOrTitle.IsEmpty() ||
				NodeId.Equals(NodeIdOrTitle, ESearchCase::IgnoreCase) ||
				Node->GetNodeTitle(EPCGNodeTitleType::ListView).ToString().Equals(NodeIdOrTitle, ESearchCase::IgnoreCase) ||
				Node->GetAuthoredTitleName().ToString().Equals(NodeIdOrTitle, ESearchCase::IgnoreCase))
			{
				TSharedPtr<FJsonObject> NodeObj = NodeToJson(Graph, Node, NodeId);
				const FPCGStack Stack;
				const FPCGGraphExecutionInspection& Inspection = Component->GetExecutionState().GetInspection();
				NodeObj->SetBoolField(TEXT("was_executed"), Inspection.WasNodeExecuted(Node, Stack));
				NodeObj->SetBoolField(TEXT("produced_data"), Inspection.HasNodeProducedData(Node, Stack));
				NodeObj->SetBoolField(TEXT("gpu_to_cpu_readback"), Inspection.DidNodeTriggerGPUToCPUReadback(Node, Stack));
				NodeObj->SetBoolField(TEXT("cpu_to_gpu_upload"), Inspection.DidNodeTriggerCPUToGPUUpload(Node, Stack));
				Nodes.Add(MakeShared<FJsonValueObject>(NodeObj));
			}
		}
	}

	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.pcg.node_inspect.v1"));
	OutData->SetArrayField(TEXT("nodes"), Nodes);
	OutData->SetNumberField(TEXT("node_count"), Nodes.Num());
	SetActionStatus(OutData, Component);
	return true;
}

bool FUeAgentHttpServer::CmdPCGDataExportJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentPCG;
	FString WorldError;
	UWorld* World = GetEditorWorld(WorldError);
	if (!World)
	{
		OutError = WorldError;
		return false;
	}
	UPCGComponent* Component = FindPCGComponent(World, Ctx.Params, OutError);
	if (!Component)
	{
		return false;
	}
	double MaxPointsNumber = 16.0;
	Ctx.Params->TryGetNumberField(TEXT("max_points_per_data"), MaxPointsNumber);
	OutData = PCGDataCollectionToJson(Component->GetGeneratedGraphOutput(), static_cast<int32>(MaxPointsNumber));
	OutData->SetObjectField(TEXT("component"), ComponentToJson(Component));

	FString OutputFile;
	if (Ctx.Params->TryGetStringField(TEXT("output_file"), OutputFile) && !OutputFile.TrimStartAndEnd().IsEmpty())
	{
		FString SavedPath;
		if (!SaveJsonFile(OutputFile, OutData, SavedPath, OutError))
		{
			return false;
		}
		OutData->SetStringField(TEXT("output_file"), SavedPath);
	}
	return true;
}

bool FUeAgentHttpServer::CmdPCGDataAssertJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentPCG;
	TSharedPtr<FJsonObject> Data;
	if (!CmdPCGDataExportJson(Ctx, Data, OutError))
	{
		return false;
	}
	TArray<TSharedPtr<FJsonValue>> Issues;
	double MinDataCount = -1.0;
	double MinTotalPointCount = -1.0;
	if (Ctx.Params->TryGetNumberField(TEXT("min_data_count"), MinDataCount) && Data->GetNumberField(TEXT("data_count")) < MinDataCount)
	{
		AddIssue(Issues, TEXT("error"), TEXT("min_data_count_failed"), TEXT("data_count"), FString::Printf(TEXT("Expected at least %.0f data entries."), MinDataCount));
	}
	if (Ctx.Params->TryGetNumberField(TEXT("min_total_point_count"), MinTotalPointCount) && Data->GetNumberField(TEXT("total_point_count")) < MinTotalPointCount)
	{
		AddIssue(Issues, TEXT("error"), TEXT("min_total_point_count_failed"), TEXT("total_point_count"), FString::Printf(TEXT("Expected at least %.0f points."), MinTotalPointCount));
	}
	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.pcg.data_assert.v1"));
	OutData->SetBoolField(TEXT("passed"), CountErrors(Issues) == 0);
	OutData->SetArrayField(TEXT("issues"), Issues);
	OutData->SetObjectField(TEXT("data"), Data);
	return CountErrors(Issues) == 0;
}

bool FUeAgentHttpServer::CmdPCGComponentRuntimeProbe(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	return CmdPCGRuntimeGenerationProbe(Ctx, OutData, OutError);
}

bool FUeAgentHttpServer::CmdPCGScreenshot(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentPCG;
	TSharedPtr<FJsonObject> FrameData;
	FString FrameError;
	if (Ctx.Params.IsValid() && (Ctx.Params->HasField(TEXT("actor")) || Ctx.Params->HasField(TEXT("actor_name")) || Ctx.Params->HasField(TEXT("actor_label"))))
	{
		FUeAgentRequestContext FrameCtx = Ctx;
		TSharedPtr<FJsonObject> FrameParams = MakeShared<FJsonObject>();
		FrameParams->Values = Ctx.Params->Values;
		if (!FrameParams->HasField(TEXT("id")))
		{
			FString ActorId;
			if (GetStringAny(FrameParams, { TEXT("actor"), TEXT("actor_name"), TEXT("actor_label") }, ActorId) && !ActorId.IsEmpty())
			{
				FrameParams->SetStringField(TEXT("id"), ActorId);
			}
		}
		FrameCtx.Params = FrameParams;
		if (!CmdViewportFrameActor(FrameCtx, FrameData, FrameError))
		{
			OutError = FrameError.IsEmpty() ? TEXT("pcg_frame_actor_failed") : FrameError;
			return false;
		}
	}

	TSharedPtr<FJsonObject> ScreenshotData;
	TArray<TSharedPtr<FJsonValue>> Artifacts;
	if (!CmdScreenshotViewport(Ctx, ScreenshotData, Artifacts, OutError))
	{
		return false;
	}
	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.pcg.screenshot.v1"));
	OutData->SetObjectField(TEXT("frame_actor"), FrameData);
	OutData->SetObjectField(TEXT("screenshot"), ScreenshotData);
	OutData->SetArrayField(TEXT("artifacts"), Artifacts);
	return true;
}

bool FUeAgentHttpServer::CmdPCGProfileReport(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentPCG;
	FString AssetPath;
	if (GetStringAny(Ctx.Params, { TEXT("asset_path"), TEXT("graph"), TEXT("graph_asset_path") }, AssetPath))
	{
		UPCGGraph* Graph = LoadAsset<UPCGGraph>(AssetPath);
		if (!Graph)
		{
			OutError = TEXT("pcg_graph_asset_not_found");
			return false;
		}
		TSharedPtr<FJsonObject> Snapshot = ExportGraphToJson(Graph);
		OutData = MakeShared<FJsonObject>();
		OutData->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.pcg.profile_report.v1"));
		OutData->SetStringField(TEXT("mode"), TEXT("graph_static"));
		OutData->SetNumberField(TEXT("node_count"), Snapshot->GetIntegerField(TEXT("node_count")));
		OutData->SetNumberField(TEXT("edge_count"), Snapshot->GetIntegerField(TEXT("edge_count")));
		OutData->SetObjectField(TEXT("graph"), Snapshot);
		return true;
	}

	TSharedPtr<FJsonObject> ComponentData;
	if (!CmdPCGComponentQuery(Ctx, ComponentData, OutError))
	{
		return false;
	}
	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.pcg.profile_report.v1"));
	OutData->SetStringField(TEXT("mode"), TEXT("component_runtime"));
	OutData->SetObjectField(TEXT("component"), ComponentData);
	return true;
}

bool FUeAgentHttpServer::CmdPCGGPUCapabilitiesQuery(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentPCG;
	TSharedPtr<FJsonObject> Catalog;
	FString CatalogError;
	CmdPCGNodeCatalogExport(Ctx, Catalog, CatalogError);
	int32 GPUCapableNodes = 0;
	const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
	if (Catalog.IsValid() && Catalog->TryGetArrayField(TEXT("nodes"), Nodes) && Nodes)
	{
		for (const TSharedPtr<FJsonValue>& Value : *Nodes)
		{
			const TSharedPtr<FJsonObject>* Obj = nullptr;
			bool bSupportsGPU = false;
			if (Value.IsValid() && Value->TryGetObject(Obj) && Obj && Obj->IsValid() && (*Obj)->TryGetBoolField(TEXT("supports_gpu_toggle"), bSupportsGPU) && bSupportsGPU)
			{
				GPUCapableNodes++;
			}
		}
	}

	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.pcg.gpu_capabilities.v1"));
	OutData->SetStringField(TEXT("rhi"), GDynamicRHI ? GDynamicRHI->GetName() : FString());
	OutData->SetBoolField(TEXT("compute_framework_loaded"), FModuleManager::Get().IsModuleLoaded(TEXT("ComputeFramework")));
	OutData->SetNumberField(TEXT("gpu_capable_node_class_count"), GPUCapableNodes);
	return true;
}

bool FUeAgentHttpServer::CmdPCGComputeGraphValidate(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentPCG;
	FString AssetPath;
	if (!GetStringAny(Ctx.Params, { TEXT("asset_path"), TEXT("graph"), TEXT("graph_asset_path") }, AssetPath))
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}
	UPCGGraph* Graph = LoadAsset<UPCGGraph>(AssetPath);
	if (!Graph)
	{
		OutError = TEXT("pcg_graph_asset_not_found");
		return false;
	}
	TArray<TSharedPtr<FJsonValue>> GPUNodeEntries;
	for (UPCGNode* Node : Graph->GetNodes())
	{
		if (UPCGSettings* Settings = Node ? Node->GetSettings() : nullptr)
		{
			if (Settings->ShouldExecuteOnGPU())
			{
				GPUNodeEntries.Add(MakeShared<FJsonValueObject>(NodeToJson(Graph, Node, Node->GetName())));
			}
		}
	}
	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.pcg.compute_graph_validate.v1"));
	OutData->SetStringField(TEXT("asset_path"), Graph->GetOutermost() ? Graph->GetOutermost()->GetName() : AssetPath);
	OutData->SetArrayField(TEXT("gpu_nodes"), GPUNodeEntries);
	OutData->SetNumberField(TEXT("gpu_node_count"), GPUNodeEntries.Num());
	OutData->SetBoolField(TEXT("compute_framework_loaded"), FModuleManager::Get().IsModuleLoaded(TEXT("ComputeFramework")));
	OutData->SetBoolField(TEXT("valid"), true);
	return true;
}

bool FUeAgentHttpServer::CmdPCGHLSLValidate(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentPCG;
	FString AssetPath;
	if (!GetStringAny(Ctx.Params, { TEXT("asset_path"), TEXT("graph"), TEXT("graph_asset_path") }, AssetPath))
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}
	UPCGGraph* Graph = LoadAsset<UPCGGraph>(AssetPath);
	if (!Graph)
	{
		OutError = TEXT("pcg_graph_asset_not_found");
		return false;
	}

	TArray<TSharedPtr<FJsonValue>> HLSLNodes;
	for (UPCGNode* Node : Graph->GetNodes())
	{
		UPCGSettings* Settings = Node ? Node->GetSettings() : nullptr;
		if (!Settings)
		{
			continue;
		}
		const FString ClassName = Settings->GetClass()->GetName();
		const bool bLooksLikeHLSL = ClassName.Contains(TEXT("HLSL"), ESearchCase::IgnoreCase) || ClassName.Contains(TEXT("Compute"), ESearchCase::IgnoreCase);
		if (!bLooksLikeHLSL)
		{
			continue;
		}
		TSharedPtr<FJsonObject> NodeObj = NodeToJson(Graph, Node, Node->GetName());
		HLSLNodes.Add(MakeShared<FJsonValueObject>(NodeObj));
	}
	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.pcg.hlsl_validate.v1"));
	OutData->SetStringField(TEXT("asset_path"), Graph->GetOutermost() ? Graph->GetOutermost()->GetName() : AssetPath);
	OutData->SetArrayField(TEXT("hlsl_nodes"), HLSLNodes);
	OutData->SetNumberField(TEXT("hlsl_node_count"), HLSLNodes.Num());
	OutData->SetBoolField(TEXT("valid"), true);
	return true;
}

bool FUeAgentHttpServer::CmdPCGDataAssetExportJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentPCG;
	FString AssetPath;
	if (!GetStringAny(Ctx.Params, { TEXT("asset_path"), TEXT("data_asset") }, AssetPath))
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}
	UPCGDataAsset* DataAsset = LoadAsset<UPCGDataAsset>(AssetPath);
	if (!DataAsset)
	{
		OutError = TEXT("pcg_data_asset_not_found");
		return false;
	}
	double MaxPointsNumber = 16.0;
	Ctx.Params->TryGetNumberField(TEXT("max_points_per_data"), MaxPointsNumber);
	OutData = PCGDataCollectionToJson(DataAsset->Data, static_cast<int32>(MaxPointsNumber));
	OutData->SetStringField(TEXT("asset_path"), DataAsset->GetOutermost() ? DataAsset->GetOutermost()->GetName() : AssetPath);
	OutData->SetStringField(TEXT("object_path"), DataAsset->GetPathName());
	OutData->SetStringField(TEXT("name"), DataAsset->Name);
#if WITH_EDITORONLY_DATA
	OutData->SetStringField(TEXT("object_path_source"), DataAsset->ObjectPath.ToString());
	OutData->SetBoolField(TEXT("expose_to_library"), DataAsset->bExposeToLibrary);
	OutData->SetStringField(TEXT("category"), DataAsset->Category.ToString());
	OutData->SetStringField(TEXT("description"), DataAsset->Description.ToString());
#endif
	return true;
}

bool FUeAgentHttpServer::CmdPCGDataAssetApplyJson(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentPCG;
	FString AssetPath;
	if (!GetStringAny(Ctx.Params, { TEXT("asset_path"), TEXT("data_asset") }, AssetPath))
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}
	bool bCreate = true;
	bool bSave = true;
	Ctx.Params->TryGetBoolField(TEXT("create"), bCreate);
	Ctx.Params->TryGetBoolField(TEXT("save"), bSave);

	UPCGDataAsset* DataAsset = LoadAsset<UPCGDataAsset>(AssetPath);
	bool bCreated = false;
	if (!DataAsset && bCreate)
	{
		const FString NormalizedAssetPath = NormalizeAssetPath(AssetPath);
		if (!FPackageName::IsValidLongPackageName(NormalizedAssetPath))
		{
			OutError = TEXT("invalid_asset_path");
			return false;
		}
		UPackage* Package = CreatePackage(*NormalizedAssetPath);
		if (!Package)
		{
			OutError = TEXT("create_package_failed");
			return false;
		}
		DataAsset = NewObject<UPCGDataAsset>(Package, UPCGDataAsset::StaticClass(), *FPackageName::GetLongPackageAssetName(NormalizedAssetPath), RF_Public | RF_Standalone | RF_Transactional);
		if (!DataAsset)
		{
			OutError = TEXT("create_pcg_data_asset_failed");
			return false;
		}
		FAssetRegistryModule::AssetCreated(DataAsset);
		Package->MarkPackageDirty();
		bCreated = true;
	}
	if (!DataAsset)
	{
		OutError = TEXT("pcg_data_asset_not_found");
		return false;
	}
	FString TextValue;
	if (Ctx.Params->TryGetStringField(TEXT("name"), TextValue))
	{
		DataAsset->Name = TextValue;
	}
#if WITH_EDITORONLY_DATA
	if (Ctx.Params->TryGetStringField(TEXT("object_path_source"), TextValue) || Ctx.Params->TryGetStringField(TEXT("object_path"), TextValue))
	{
		DataAsset->ObjectPath = FSoftObjectPath(TextValue);
	}
	bool BoolValue = false;
	if (Ctx.Params->TryGetBoolField(TEXT("expose_to_library"), BoolValue))
	{
		DataAsset->bExposeToLibrary = BoolValue;
	}
	if (Ctx.Params->TryGetStringField(TEXT("category"), TextValue))
	{
		DataAsset->Category = FText::FromString(TextValue);
	}
	if (Ctx.Params->TryGetStringField(TEXT("description"), TextValue))
	{
		DataAsset->Description = FText::FromString(TextValue);
	}
#endif
	DataAsset->MarkPackageDirty();
	if (bSave && !SavePackagesForObjects({ DataAsset }, OutError))
	{
		return false;
	}

	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.pcg.data_asset_apply.v1"));
	OutData->SetBoolField(TEXT("created"), bCreated);
	OutData->SetStringField(TEXT("asset_path"), DataAsset->GetOutermost() ? DataAsset->GetOutermost()->GetName() : AssetPath);
	OutData->SetStringField(TEXT("object_path"), DataAsset->GetPathName());
	return true;
}

bool FUeAgentHttpServer::CmdPCGLevelToAssetExport(const FUeAgentRequestContext& Ctx, TSharedPtr<FJsonObject>& OutData, FString& OutError) const
{
	using namespace UeAgentPCG;
	FString WorldError;
	UWorld* World = GetEditorWorld(WorldError);
	if (!World)
	{
		OutError = WorldError;
		return false;
	}

	FString AssetPath;
	if (!GetStringAny(Ctx.Params, { TEXT("asset_path"), TEXT("output_asset_path") }, AssetPath))
	{
		OutError = TEXT("missing_asset_path");
		return false;
	}
	const FString NormalizedAssetPath = NormalizeAssetPath(AssetPath);
	const FString AssetName = FPackageName::GetLongPackageAssetName(NormalizedAssetPath);
	const FString AssetFolder = FPackageName::GetLongPackagePath(NormalizedAssetPath);
	if (AssetName.IsEmpty() || AssetFolder.IsEmpty())
	{
		OutError = TEXT("invalid_asset_path");
		return false;
	}
	bool bSave = true;
	Ctx.Params->TryGetBoolField(TEXT("save"), bSave);

	FPCGAssetExporterParameters Parameters;
	Parameters.bOpenSaveDialog = false;
	Parameters.AssetName = AssetName;
	Parameters.AssetPath = AssetFolder;
	Parameters.bSaveOnExportEnded = bSave;
	UPackage* ModifiedPackage = UPCGLevelToAsset::CreateOrUpdatePCGAsset(World, Parameters);
	if (!ModifiedPackage)
	{
		OutError = TEXT("level_to_asset_export_failed");
		return false;
	}

	OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.pcg.level_to_asset_export.v1"));
	OutData->SetStringField(TEXT("asset_path"), NormalizedAssetPath);
	OutData->SetStringField(TEXT("package"), ModifiedPackage->GetName());
	OutData->SetBoolField(TEXT("saved"), bSave);
	return true;
}

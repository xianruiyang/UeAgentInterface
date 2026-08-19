// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/PCG/UeAgentPCGDomainBackend.h"

#include "Module/UeAgentInterfaceLogger.h"
#include "Utilities/AuthoringCore/UeAgentDetailsAuthoringService.h"
#include "Utilities/UeAgentAuthoringFileIO.h"
#include "Utilities/UeAgentAuthoringJson.h"
#include "Utilities/UeAgentJsonDiagnostics.h"
#include "Utilities/UeAgentPropertyValueCodec.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Async/TaskGraphInterfaces.h"
#include "EdGraph/EdGraph.h"
#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "FileHelpers.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/Parse.h"
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

	FString NormalizeAssetPath(const FString& InPath)
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

	FString ResolveFolderPath(const FString& InPath)
	{
		FString Folder = InPath;
		Folder.TrimStartAndEndInline();
		Folder.ReplaceInline(TEXT("\\"), TEXT("/"));
		return UeAgentJsonDiagnostics::ResolveProjectRelativeFilePath(Folder);
	}

	UObject* LoadAssetObject(const FString& InPath)
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


	bool SavePackagesForObjects(const TArray<UObject*>& Objects, FString& OutError)
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

	bool SaveAuthoringFile(const FString& OutputPathInput, const TSharedPtr<FJsonObject>& Root, FString& OutOutputPath, FString& OutError)
	{
		UeAgentAuthoringFileIO::FSaveOptions Options;
		Options.SourceCommand = TEXT("pcg_json_export");
		return UeAgentAuthoringFileIO::SaveObject(OutputPathInput, Root, Options, OutOutputPath, OutError);
	}

	bool PCGGraphHasMaterializedEditorGraph(const UPCGGraph* Graph)
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

	static bool IsHeadlessPCGEditorGraphInit()
	{
		const TCHAR* CommandLine = FCommandLine::Get();
		return IsRunningCommandlet()
			|| FApp::IsUnattended()
			|| FParse::Param(CommandLine, TEXT("NullRHI"));
	}

	void ClosePCGEditorOpenedForGraphInit(
		UAssetEditorSubsystem* AssetEditorSubsystem,
		UPCGGraph* Graph,
		const bool bOpenedEditorForGraphInit)
	{
		if (!bOpenedEditorForGraphInit
			|| !IsHeadlessPCGEditorGraphInit()
			|| !AssetEditorSubsystem
			|| !Graph)
		{
			return;
		}

		if (IAssetEditorInstance* EditorInstance = AssetEditorSubsystem->FindEditorForAsset(Graph, false))
		{
			const TSharedPtr<FTabManager> TabManager = EditorInstance->GetAssociatedTabManager();
			if (TabManager.IsValid())
			{
				TabManager->CloseAllAreas();
			}
		}

		PumpSlateForPCGEditorGraphInit(2);
		AssetEditorSubsystem->CloseAllEditorsForAsset(Graph);
		PumpSlateForPCGEditorGraphInit(2);
	}

	bool EnsurePCGEditorGraphMaterialized(UPCGGraph* Graph, bool& bOutOpenedEditor)
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
		if (IsHeadlessPCGEditorGraphInit())
		{
			return false;
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
		IAssetEditorInstance* EditorInstance = AssetEditorSubsystem->FindEditorForAsset(Graph, false);
		if (!EditorInstance)
		{
			if (!AssetEditorSubsystem->OpenEditorForAsset(Graph))
			{
				return false;
			}
			bOutOpenedEditor = true;
			EditorInstance = AssetEditorSubsystem->FindEditorForAsset(Graph, false);
		}
		PumpSlateForPCGEditorGraphInit(8);
		return PCGGraphHasMaterializedEditorGraph(Graph);
	}

	static TSharedPtr<FJsonObject> MakeIssue(const FString& Severity, const FString& Code, const FString& Path, const FString& Message)
	{
		return UeAgentJsonDiagnostics::MakeIssue(Severity, Code, Path, Message);
	}

	void AddIssue(TArray<TSharedPtr<FJsonValue>>& Issues, const FString& Severity, const FString& Code, const FString& Path, const FString& Message)
	{
		Issues.Add(MakeShared<FJsonValueObject>(MakeIssue(Severity, Code, Path, Message)));
	}

	int32 CountErrors(const TArray<TSharedPtr<FJsonValue>>& Issues)
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

	static TArray<TSharedPtr<FJsonValue>> ExportEditableProperties(UObject* Object)
	{
		TArray<TSharedPtr<FJsonValue>> Properties;
		if (!Object) return Properties;
		UeAgentAuthoringState::FStateSnapshot Snapshot;
		TSharedPtr<FJsonObject> Document;
		FString Error;
		if (UeAgentDetailsAuthoringService::CaptureObjectProperties(
				Object, UeAgentDetailsAuthoringService::FCapturePolicy(),
				Snapshot, Document, Error))
		{
			UeAgentDetailsAuthoringService::DecodePropertiesDocument(Document, Properties, Error);
		}
		return Properties;
	}

	static void RefreshPCGImportedPropertySideEffects(UObject* Object, const FString& PropertyName)
	{
		if (UPCGBlueprintSettings* BlueprintSettings = Cast<UPCGBlueprintSettings>(Object))
		{
			if (PropertyName == TEXT("BlueprintElementType"))
			{
				const FClassProperty* ElementTypeProperty = FindFProperty<FClassProperty>(BlueprintSettings->GetClass(), TEXT("BlueprintElementType"));
				UClass* ElementType = ElementTypeProperty
					? Cast<UClass>(ElementTypeProperty->GetObjectPropertyValue_InContainer(BlueprintSettings))
					: nullptr;
				UPCGBlueprintElement* ElementInstance = nullptr;
				BlueprintSettings->SetElementType(ElementType, ElementInstance);
			}
		}
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

	int32 ApplyPropertyArray(UObject* Object, const TArray<TSharedPtr<FJsonValue>>* PropertyValues, TArray<TSharedPtr<FJsonValue>>& Issues, const FString& PathPrefix)
	{
		if (!Object || !PropertyValues)
		{
			return 0;
		}

		TSharedPtr<FJsonObject> PropertiesRoot = MakeShared<FJsonObject>();
		PropertiesRoot->SetStringField(TEXT("schema"), TEXT("uai.uobject_properties.v2"));
		PropertiesRoot->SetArrayField(TEXT("properties"), *PropertyValues);
		UeAgentAuthoringDirectApply::FResult ApplyResult;
		FString ApplyError;
		if (!UeAgentDetailsAuthoringService::ApplyObjectProperties(
				Object, PropertiesRoot, UeAgentAuthoringObjectApply::FOptions(),
				ApplyResult, ApplyError))
		{
			AddIssue(Issues, TEXT("error"), TEXT("property_apply_failed"), PathPrefix,
				ApplyError.IsEmpty() ? FString(TEXT("property_apply_failed")) : ApplyError);
			return 0;
		}

		for (const TSharedPtr<FJsonValue>& Value : *PropertyValues)
		{
			const TSharedPtr<FJsonObject> Descriptor = Value.IsValid() ? Value->AsObject() : nullptr;
			FString PropertyPath;
			if (Descriptor.IsValid() && Descriptor->TryGetStringField(TEXT("property_path"), PropertyPath))
			{
				RefreshPCGImportedPropertySideEffects(Object, PropertyPath);
			}
		}
		return ApplyResult.PropertiesSeen;
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
		UeAgentAuthoringJson::ApplyAuthoringMetadata(PinObj, TEXT("pin_label"), PinProperties.Label.ToString(), PinProperties.Label.ToString(), TEXT("PCG/Pins"), TEXT("pin"));
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

	UClass* ResolvePCGSettingsClass(const FString& ClassPathInput)
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

	TSharedPtr<FJsonObject> SettingsClassToCatalogJson(UClass* Class)
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

	FString MakeStableNodeId(UPCGGraph* Graph, UPCGNode* Node, TSet<FString>& UsedIds)
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

	static TArray<UPCGNode*> GatherExportNodes(UPCGGraph* Graph)
	{
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

		if (!Graph)
		{
			return ExportNodes;
		}

		AddExportNode(Graph->GetInputNode());
		for (UPCGNode* Node : Graph->GetNodes())
		{
			AddExportNode(Node);
		}
		AddExportNode(Graph->GetOutputNode());
		return ExportNodes;
	}

	TMap<FString, UPCGNode*> BuildExportNodeMapById(UPCGGraph* Graph)
	{
		TMap<FString, UPCGNode*> NodeById;
		TSet<FString> UsedIds;
		for (UPCGNode* Node : GatherExportNodes(Graph))
		{
			NodeById.Add(MakeStableNodeId(Graph, Node, UsedIds), Node);
		}
		return NodeById;
	}

	TSharedPtr<FJsonObject> NodeToJson(UPCGGraph* Graph, UPCGNode* Node, const FString& NodeId)
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
		UeAgentAuthoringJson::ApplyAuthoringMetadata(NodeObj, TEXT("node_id"), NodeId, Node->GetNodeTitle(EPCGNodeTitleType::ListView).ToString(), TEXT("PCG/Nodes"), TEXT("object"));
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

	TSharedPtr<FJsonObject> ExportParameterBagToJson(const FInstancedPropertyBag* Bag);

	TSharedPtr<FJsonObject> ExportGraphToJson(UPCGGraph* Graph)
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
		TArray<UPCGNode*> ExportNodes = GatherExportNodes(Graph);

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

	TSharedPtr<FJsonObject> ExportParameterBagToJson(const FInstancedPropertyBag* Bag)
	{
		FInstancedPropertyBag EmptyBag;
		UeAgentPropertyValueCodec::FOptions Options;
		const TSharedPtr<FJsonValue> Encoded = UeAgentPropertyValueCodec::EncodePropertyBag(Bag ? *Bag : EmptyBag, nullptr, Options);
		return Encoded.IsValid() && Encoded->Type == EJson::Object ? Encoded->AsObject() : MakeShared<FJsonObject>();
	}

	int32 GetParameterBagPropertyCount(const FInstancedPropertyBag* Bag)
	{
		return Bag && Bag->GetPropertyBagStruct()
			? Bag->GetPropertyBagStruct()->GetPropertyDescs().Num()
			: 0;
	}

	int32 ApplyCanonicalParameterBag(
		FInstancedPropertyBag& Bag,
		const TSharedPtr<FJsonObject>& BagJson,
		TArray<TSharedPtr<FJsonValue>>& Issues,
		TArray<FName>* OutAppliedNames,
		bool bAllowLayoutChanges)
	{
		UeAgentPropertyValueCodec::FOptions CodecOptions;
		FInstancedPropertyBag Decoded;
		FString CodecError;
		if (!BagJson.IsValid()
			|| !UeAgentPropertyValueCodec::DecodePropertyBag(MakeShared<FJsonValueObject>(BagJson), Decoded, nullptr, CodecOptions, CodecError))
		{
			AddIssue(Issues, TEXT("error"), TEXT("parameter_bag_invalid"), TEXT("parameters"), CodecError.IsEmpty() ? TEXT("Invalid canonical property bag.") : CodecError);
			return 0;
		}

		const TConstArrayView<FPropertyBagPropertyDesc> DecodedDescs = Decoded.GetPropertyBagStruct()
			? Decoded.GetPropertyBagStruct()->GetPropertyDescs()
			: TConstArrayView<FPropertyBagPropertyDesc>();
		const int32 DecodedCount = DecodedDescs.Num();
		if (bAllowLayoutChanges)
		{
			Bag = MoveTemp(Decoded);
			if (OutAppliedNames)
			{
				for (const FPropertyBagPropertyDesc& Desc : Bag.GetPropertyBagStruct() ? Bag.GetPropertyBagStruct()->GetPropertyDescs() : TConstArrayView<FPropertyBagPropertyDesc>())
				{
					OutAppliedNames->Add(Desc.Name);
				}
			}
			return DecodedCount;
		}

		const TConstArrayView<FPropertyBagPropertyDesc> ExistingDescs = Bag.GetPropertyBagStruct()
			? Bag.GetPropertyBagStruct()->GetPropertyDescs()
			: TConstArrayView<FPropertyBagPropertyDesc>();
		if (ExistingDescs.Num() != DecodedDescs.Num())
		{
			AddIssue(Issues, TEXT("error"), TEXT("parameter_schema_mismatch"), TEXT("parameters.schema"), TEXT("Graph instance parameters must exactly match the parent graph schema."));
			return 0;
		}

		const FStructProperty* BagStorageProperty = FindFProperty<FStructProperty>(FInstancedPropertyBag::StaticStruct(), TEXT("Value"));
		FInstancedStruct* ExistingStorage = BagStorageProperty ? BagStorageProperty->ContainerPtrToValuePtr<FInstancedStruct>(&Bag) : nullptr;
		const FInstancedStruct* DecodedStorage = BagStorageProperty ? BagStorageProperty->ContainerPtrToValuePtr<FInstancedStruct>(&Decoded) : nullptr;
		if ((ExistingDescs.Num() > 0) && (!ExistingStorage || !ExistingStorage->GetMutableMemory() || !DecodedStorage || !DecodedStorage->GetMemory()))
		{
			AddIssue(Issues, TEXT("error"), TEXT("parameter_bag_storage_unavailable"), TEXT("parameters"), TEXT("Property bag storage is unavailable."));
			return 0;
		}

		for (const FPropertyBagPropertyDesc& DesiredDesc : DecodedDescs)
		{
			const FPropertyBagPropertyDesc* ExistingDesc = Bag.FindPropertyDescByName(DesiredDesc.Name);
			if (!ExistingDesc
				|| ExistingDesc->ValueType != DesiredDesc.ValueType
				|| ExistingDesc->ValueTypeObject.Get() != DesiredDesc.ValueTypeObject.Get()
				|| ExistingDesc->ContainerTypes != DesiredDesc.ContainerTypes
				|| !ExistingDesc->CachedProperty || !DesiredDesc.CachedProperty)
			{
				AddIssue(Issues, TEXT("error"), TEXT("parameter_schema_mismatch"), FString::Printf(TEXT("parameters.schema.%s"), *DesiredDesc.Name.ToString()), TEXT("Graph instance parameters must exactly match the parent graph schema."));
				return 0;
			}
		}

		for (const FPropertyBagPropertyDesc& DesiredDesc : DecodedDescs)
		{
			const FPropertyBagPropertyDesc* ExistingDesc = Bag.FindPropertyDescByName(DesiredDesc.Name);
			void* ExistingValue = ExistingDesc->CachedProperty->ContainerPtrToValuePtr<void>(ExistingStorage->GetMutableMemory());
			const void* DesiredValue = DesiredDesc.CachedProperty->ContainerPtrToValuePtr<void>(DecodedStorage->GetMemory());
			ExistingDesc->CachedProperty->CopyCompleteValue(ExistingValue, DesiredValue);
			if (OutAppliedNames) OutAppliedNames->Add(DesiredDesc.Name);
		}
		return DecodedCount;
	}

	int32 PreflightCanonicalParameterBag(
		const FInstancedPropertyBag* SourceBag,
		const TSharedPtr<FJsonObject>& BagJson,
		TArray<TSharedPtr<FJsonValue>>& Issues,
		bool bAllowLayoutChanges)
	{
		FInstancedPropertyBag Preview;
		if (SourceBag) Preview = *SourceBag;
		return ApplyCanonicalParameterBag(Preview, BagJson, Issues, nullptr, bAllowLayoutChanges);
	}

	void RefreshGraphInstanceParameters(UPCGGraphInstance* Instance)
	{
		if (Instance)
		{
			Instance->OnGraphParametersChanged(EPCGGraphParameterEvent::GraphPostLoad, NAME_None);
		}
	}

	void NotifyGraphInstanceParameterValuesChanged(UPCGGraphInstance* Instance, const TArray<FName>& ParameterNames)
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

	UPCGGraph* CreateOrLoadGraphAsset(const FString& AssetPathInput, bool bCreateIfMissing, bool& bOutCreated, FString& OutError)
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

	bool ApplyGraphJson(UPCGGraph* Graph, const TSharedPtr<FJsonObject>& GraphJson, bool bReplaceNodes, TArray<TSharedPtr<FJsonValue>>& Issues, int32& OutAppliedNodes, int32& OutAppliedEdges)
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
		}

		const TSharedPtr<FJsonObject>* ParametersObj = nullptr;
		if (GraphJson->TryGetObjectField(TEXT("parameters"), ParametersObj) && ParametersObj && ParametersObj->IsValid())
		{
			Graph->UpdateUserParametersStruct([&](FInstancedPropertyBag& Bag)
			{
				ApplyCanonicalParameterBag(Bag, *ParametersObj, Issues, nullptr, true);
			});
		}

		Graph->Recompile();
		Graph->MarkPackageDirty();
		return CountErrors(Issues) == 0;
	}

	bool PreflightApplyGraphJson(UPCGGraph* ExistingGraph, const TSharedPtr<FJsonObject>& GraphJson, bool bReplaceNodes, TArray<TSharedPtr<FJsonValue>>& Issues, int32& OutAppliedNodes, int32& OutAppliedEdges)
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

	AActor* FindActorByNameOrLabelDirect(UWorld* World, const FString& NameOrLabel)
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

	bool GetStringAny(const TSharedPtr<FJsonObject>& Params, const TArray<const TCHAR*>& Keys, FString& OutValue)
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

	UPCGComponent* FindPCGComponent(UWorld* World, const TSharedPtr<FJsonObject>& Params, FString& OutError)
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

	bool TryParseGenerationTrigger(const FString& TriggerText, EPCGComponentGenerationTrigger& OutTrigger)
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

	TSharedPtr<FJsonObject> ComponentToJson(UPCGComponent* Component)
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

	TSharedPtr<FJsonObject> PCGDataCollectionToJson(const FPCGDataCollection& Collection, int32 MaxPointsPerData)
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

	void SetActionStatus(TSharedPtr<FJsonObject>& OutData, UPCGComponent* Component)
	{
		if (!OutData.IsValid())
		{
			OutData = MakeShared<FJsonObject>();
		}
		OutData->SetObjectField(TEXT("component"), ComponentToJson(Component));
	}

	void BuildAuthoringGraphAndLayout(
		const TSharedPtr<FJsonObject>& SourceGraph,
		TSharedPtr<FJsonObject>& OutGraph,
		TSharedPtr<FJsonObject>& OutLayout)
	{
		OutGraph = MakeShared<FJsonObject>();
		OutLayout = MakeShared<FJsonObject>();
		OutLayout->SetStringField(TEXT("schema"), TEXT("ue_agent_interface.pcg_graph_layout.v1"));

		if (!SourceGraph.IsValid())
		{
			OutGraph->SetArrayField(TEXT("nodes"), {});
			OutGraph->SetArrayField(TEXT("edges"), {});
			OutLayout->SetArrayField(TEXT("nodes"), {});
			return;
		}

		for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : SourceGraph->Values)
		{
			if (Field.Key != TEXT("nodes"))
			{
				OutGraph->SetField(Field.Key, Field.Value);
			}
		}

		TArray<TSharedPtr<FJsonValue>> CleanNodes;
		TArray<TSharedPtr<FJsonValue>> LayoutNodes;
		const TArray<TSharedPtr<FJsonValue>>* SourceNodes = nullptr;
		if (SourceGraph->TryGetArrayField(TEXT("nodes"), SourceNodes) && SourceNodes)
		{
			for (const TSharedPtr<FJsonValue>& NodeValue : *SourceNodes)
			{
				const TSharedPtr<FJsonObject>* NodeObj = nullptr;
				if (!NodeValue.IsValid() || !NodeValue->TryGetObject(NodeObj) || !NodeObj || !NodeObj->IsValid())
				{
					continue;
				}

				TSharedPtr<FJsonObject> CleanNode = MakeShared<FJsonObject>();
				for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : (*NodeObj)->Values)
				{
					if (Field.Key != TEXT("pos_x") && Field.Key != TEXT("pos_y"))
					{
						CleanNode->SetField(Field.Key, Field.Value);
					}
				}
				CleanNodes.Add(MakeShared<FJsonValueObject>(CleanNode));

				double PosX = 0.0;
				double PosY = 0.0;
				const bool bHasPosX = (*NodeObj)->TryGetNumberField(TEXT("pos_x"), PosX);
				const bool bHasPosY = (*NodeObj)->TryGetNumberField(TEXT("pos_y"), PosY);
				if (bHasPosX || bHasPosY)
				{
					FString Id;
					(*NodeObj)->TryGetStringField(TEXT("id"), Id);
					TSharedPtr<FJsonObject> LayoutNode = MakeShared<FJsonObject>();
					LayoutNode->SetStringField(TEXT("id"), Id);
					LayoutNode->SetNumberField(TEXT("x"), PosX);
					LayoutNode->SetNumberField(TEXT("y"), PosY);
					LayoutNodes.Add(MakeShared<FJsonValueObject>(LayoutNode));
				}
			}
		}
		OutGraph->SetArrayField(TEXT("nodes"), CleanNodes);
		OutLayout->SetArrayField(TEXT("nodes"), LayoutNodes);
	}
}

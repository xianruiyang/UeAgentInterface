// Copyright Epic Games, Inc. All Rights Reserved.

#include "Domains/PCG/UeAgentPCGDomainBackend.h"

#include "Core/Contract/UeAgentDiagnostic.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringDiagnostics.h"
#include "Core/HjsonWorkspace/UeAgentGraphAuthoringProjection.h"
#include "Utilities/UeAgentReadableName.h"
#include "Utilities/AuthoringCore/UeAgentAuthoringTypeDescriptors.h"
#include "Utilities/EditorResources/UeAgentEditorResourcePersistence.h"

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
	static UE::UAI::Contract::FDiagnostic BackendFailure(const TCHAR* Code, const FString& Reason,
		const FString& Field, TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected, const bool bNativeResult = false)
	{
		auto Error = UE::UAI::Contract::FDiagnostic::Failure(Code, Reason, TEXT("pcg_backend"));
		Error.PropertyPath = Field;
		Error.ActualValue = MoveTemp(Actual);
		Error.ExpectedValue = MoveTemp(Expected);
		Error.ActualValuePresent = Error.ActualValue.IsValid();
		Error.ExpectedValuePresent = Error.ExpectedValue.IsValid();
		if (bNativeResult) Error.SourceKind = TEXT("native_result");
		return Error;
	}

	static bool ApplyAuthoredNodePosition(UPCGNode& Node, const FJsonObject& Object, UE::UAI::Contract::FDiagnostic& Error)
	{
#if WITH_EDITOR
		const TCHAR* XField = Object.HasField(TEXT("pos_x")) ? TEXT("pos_x") : TEXT("x");
		const TCHAR* YField = Object.HasField(TEXT("pos_y")) ? TEXT("pos_y") : TEXT("y");
		if (!Object.HasField(XField) && !Object.HasField(YField)) return true;
		int32 NativeX, NativeY; Node.GetNodePosition(NativeX, NativeY);
		double X = NativeX, Y = NativeY;
		const TCHAR* InvalidField = nullptr;
		if (Object.HasField(XField) && !Object.TryGetNumberField(XField, X)) InvalidField = XField;
		else if (Object.HasField(YField) && !Object.TryGetNumberField(YField, Y)) InvalidField = YField;
		if (InvalidField)
		{
			Error = BackendFailure(TEXT("pcg_node_position_number_required"), TEXT("A node position component must be a number."),
				InvalidField, Object.TryGetField(InvalidField), MakeShared<FJsonValueString>(TEXT("number")));
			UeAgentAuthoringDiagnostics::LocateAuthoringSource(Error, Object, InvalidField);
			return false;
		}
		if (!UeAgentPropertyValueCodec::TryConvertInt32(X, NativeX, Error))
		{
			UeAgentAuthoringDiagnostics::LocateAuthoringSource(Error, Object, XField);
			return false;
		}
		if (!UeAgentPropertyValueCodec::TryConvertInt32(Y, NativeY, Error))
		{
			UeAgentAuthoringDiagnostics::LocateAuthoringSource(Error, Object, YField);
			return false;
		}
		Node.SetNodePosition(NativeX, NativeY);
#endif
		return true;
	}

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


	bool SavePackagesForObjects(const TArray<UObject*>& Objects, UE::UAI::Contract::FDiagnostic& OutError)
	{
		TArray<UPackage*> PackagesToSave;
		for (UObject* Object : Objects)
		{
			if (Object && Object->GetOutermost())
			{
				PackagesToSave.AddUnique(Object->GetOutermost());
			}
		}
		if (PackagesToSave.Num() > 0 && !UE::UAI::EditorResources::SavePackages(PackagesToSave, false, OutError))
		{
			OutError = UE::UAI::Contract::FDiagnostic::WrapFailure(TEXT("save_package_failed"), OutError, TEXT("pcg_backend"));
			return false;
		}
		return true;
	}

	bool SaveAuthoringFile(const FString& OutputPathInput, const TSharedPtr<FJsonObject>& Root, FString& OutOutputPath, UE::UAI::Contract::FDiagnostic& OutError)
	{
		UeAgentAuthoringFileIO::FSaveOptions Options;
		Options.SourceCommand = TEXT("pcg_json_export");
		return UeAgentAuthoringFileIO::SaveObject(OutputPathInput, Root, Options, OutOutputPath, OutError);
	}

	static UE::UAI::Contract::FDiagnostic GraphFailure(const TCHAR* Code, const FString& Reason,
		const FString& Field, TSharedPtr<FJsonValue> Actual, TSharedPtr<FJsonValue> Expected, const bool bNativeResult = false)
	{
		auto Error = BackendFailure(Code, Reason, Field, MoveTemp(Actual), MoveTemp(Expected), bNativeResult);
		Error.Stage = TEXT("graph_apply");
		if (!bNativeResult && Field.StartsWith(TEXT("/"))) Error.Source.JsonPath = Field;
		return Error;
	}

	static void AppendGraphIssue(TArray<UE::UAI::Contract::FDiagnostic>& Issues,
		UE::UAI::Contract::FDiagnostic Error, const FJsonObject* SourceObject = nullptr,
		const FString& Field = FString(), const FString& NodeId = FString())
	{
		if (Error.CandidateName.IsEmpty()) Error.CandidateName = NodeId;
		if (SourceObject) UeAgentAuthoringDiagnostics::LocateAuthoringSource(Error, *SourceObject, Field);
		Issues.Add(MoveTemp(Error));
	}

	int32 CountErrors(const TArray<UE::UAI::Contract::FDiagnostic>& Issues)
    {
        int32 Count = 0;
        for (const auto& Issue : Issues)
            if (Issue.Severity == UE::UAI::Contract::ESeverity::Error) ++Count;
        return Count;
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
		UE::UAI::Contract::FDiagnostic Error;
		if (UeAgentDetailsAuthoringService::CaptureObjectProperties(
				Object, UeAgentDetailsAuthoringService::FCapturePolicy(),
				Snapshot, Document, Error))
		{
			UeAgentDetailsAuthoringService::DecodePropertiesDocument(Document, Properties, Error);
		}
		return Properties;
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

	int32 ApplyPropertyArray(UObject* Object, const TArray<TSharedPtr<FJsonValue>>* PropertyValues, TArray<UE::UAI::Contract::FDiagnostic>& Issues, const FString& PathPrefix)
	{
		if (!Object || !PropertyValues)
		{
			return 0;
		}

		TSharedPtr<FJsonObject> PropertiesRoot = MakeShared<FJsonObject>();
		PropertiesRoot->SetStringField(TEXT("schema"), TEXT("uai.uobject_properties.v2"));
		PropertiesRoot->SetArrayField(TEXT("properties"), *PropertyValues);
		UeAgentAuthoringDirectApply::FResult ApplyResult;
		UE::UAI::Contract::FDiagnostic ApplyError;
		if (!UeAgentDetailsAuthoringService::ApplyObjectProperties(Object, PropertiesRoot, UeAgentAuthoringObjectApply::FOptions(), ApplyResult))
		{
				ApplyError = ApplyResult.Diagnostic;
			if (ApplyError.IsEmpty()) ApplyError = UE::UAI::Contract::FDiagnostic::Failure(TEXT("property_apply_failed"), TEXT("The PCG property update failed."), TEXT("graph_apply"));
            if (ApplyError.PropertyPath.IsEmpty()) ApplyError.PropertyPath = PathPrefix;
            Issues.Add(MoveTemp(ApplyError));
			return 0;
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

		const FString Candidate = UE::UAI::AllocateReadableName(Base, [&](const FString& Name) {
			return UsedIds.Contains(Name) || Name == InputNodeId || Name == OutputNodeId;
		});
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

	static UPCGPin* ResolvePCGEdgePin(UPCGNode* Node, const FString& RequestedLabel, bool bOutputPin, FString& OutResolvedLabel, bool bPublicAlias = false)
	{
		OutResolvedLabel = RequestedLabel;
		if (!Node)
		{
			return nullptr;
		}
		if (bPublicAlias)
		{
			using namespace UE::UAI::HjsonWorkspace;
			TArray<FGraphAuthoringPinAliasInput> Inputs;
			TArray<UPCGPin*> NativePins;
			auto Append = [&](const auto& Pins, EGraphAuthoringEndpointDirection Direction)
			{
				TArray<UPCGPin*> Sorted;
				for (const auto& Pin : Pins)
					if (Pin && !Pin->Properties.Label.IsNone()) Sorted.Add(Pin.Get());
				Sorted.Sort([](const UPCGPin& A, const UPCGPin& B) {
					return A.Properties.Label.ToString() < B.Properties.Label.ToString();
				});
				for (UPCGPin* Pin : Sorted)
				{
					Inputs.Add({FString::FromInt(NativePins.Num()), Pin->Properties.Label.ToString(), 0, Direction});
					NativePins.Add(Pin);
				}
			};
			// Export allocates aliases across both directions. Resolve after node
			// reconstruction using that same public owner, never by removing #N.
			Append(Node->GetInputPins(), EGraphAuthoringEndpointDirection::Input);
			Append(Node->GetOutputPins(), EGraphAuthoringEndpointDirection::Output);
			FGraphAuthoringPinAliasProjection Projection;
			FWorkspaceError AliasError;
			if (!TryBuildGraphAuthoringPinAliasProjection(Inputs, Projection, AliasError)) return nullptr;
			for (const auto& Alias : Projection.SourceIdByAlias)
				if (Alias.Key.Equals(RequestedLabel, ESearchCase::IgnoreCase))
				{
					const int32 Index = FCString::Atoi(*Alias.Value);
					const auto Direction = bOutputPin ? EGraphAuthoringEndpointDirection::Output : EGraphAuthoringEndpointDirection::Input;
					if (!Inputs.IsValidIndex(Index) || Inputs[Index].Direction != Direction) return nullptr;
					OutResolvedLabel = NativePins[Index]->Properties.Label.ToString();
					return NativePins[Index];
				}
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
		TArray<UE::UAI::Contract::FDiagnostic>& Issues,
		TArray<FName>* OutAppliedNames,
		bool bAllowLayoutChanges)
	{
		UeAgentPropertyValueCodec::FOptions CodecOptions;
		FInstancedPropertyBag Decoded;
		UE::UAI::Contract::FDiagnostic CodecError;
		if (!BagJson.IsValid())
		{
			AppendGraphIssue(Issues, GraphFailure(TEXT("parameter_bag_invalid"), TEXT("A canonical property bag object is required."),
				TEXT("parameters"), nullptr, MakeShared<FJsonValueString>(TEXT("a property bag object"))));
			return 0;
		}
		if (!UeAgentPropertyValueCodec::DecodePropertyBag(MakeShared<FJsonValueObject>(BagJson), Decoded, nullptr, CodecOptions, CodecError))
		{
			AppendGraphIssue(Issues, MoveTemp(CodecError), BagJson.Get());
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
			AppendGraphIssue(Issues, GraphFailure(TEXT("parameter_schema_mismatch"), TEXT("The parameter schema has a different number of entries from the parent graph schema."),
				TEXT("/schema"), MakeShared<FJsonValueNumber>(DecodedDescs.Num()), MakeShared<FJsonValueNumber>(ExistingDescs.Num())), BagJson.Get(), TEXT("schema"));
			return 0;
		}

		const FStructProperty* BagStorageProperty = FindFProperty<FStructProperty>(FInstancedPropertyBag::StaticStruct(), TEXT("Value"));
		FInstancedStruct* ExistingStorage = BagStorageProperty ? BagStorageProperty->ContainerPtrToValuePtr<FInstancedStruct>(&Bag) : nullptr;
		const FInstancedStruct* DecodedStorage = BagStorageProperty ? BagStorageProperty->ContainerPtrToValuePtr<FInstancedStruct>(&Decoded) : nullptr;
		if ((ExistingDescs.Num() > 0) && (!ExistingStorage || !ExistingStorage->GetMutableMemory() || !DecodedStorage || !DecodedStorage->GetMemory()))
		{
			AppendGraphIssue(Issues, GraphFailure(TEXT("parameter_bag_storage_unavailable"), TEXT("The property bag requires readable source storage and writable destination storage."),
				!ExistingStorage ? TEXT("destination_storage_available") : !ExistingStorage->GetMutableMemory() ? TEXT("destination_memory_available")
				: !DecodedStorage ? TEXT("source_storage_available") : TEXT("source_memory_available"),
				MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), true));
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
				FString Pointer = TEXT("/schema");
				TSharedPtr<FJsonObject> SchemaEntry;
				const TArray<TSharedPtr<FJsonValue>>* Schema = nullptr;
				if (BagJson->TryGetArrayField(TEXT("schema"), Schema))
				{
					for (int32 Index = 0; Index < Schema->Num(); ++Index)
					{
						const auto Entry = (*Schema)[Index].IsValid() ? (*Schema)[Index]->AsObject() : nullptr;
						FString Name;
						if (Entry && Entry->TryGetStringField(TEXT("name"), Name) && FName(*Name) == DesiredDesc.Name)
						{
							SchemaEntry = Entry;
							Pointer += TEXT("/") + LexToString(Index);
							break;
						}
					}
				}
				auto Error = GraphFailure(TEXT("parameter_schema_mismatch"),
					TEXT("The parameter declaration must match the parent graph schema."), FString(), nullptr, nullptr);
				Error.CandidateName = DesiredDesc.Name.ToString();
				if (!ExistingDesc)
				{
					Error.PublicMessage = TEXT("The supplied parameter does not exist in the parent graph schema.");
					Error.ActualValue = MakeShared<FJsonValueString>(DesiredDesc.Name.ToString());
					Error.ExpectedValue = MakeShared<FJsonValueString>(TEXT("a parameter declared by the parent graph"));
					Pointer += TEXT("/name");
				}
				else if (ExistingDesc->ValueType != DesiredDesc.ValueType
					|| ExistingDesc->ValueTypeObject.Get() != DesiredDesc.ValueTypeObject.Get())
				{
					while (SchemaEntry)
					{
						const TSharedPtr<FJsonObject>* Element = nullptr;
						if (!SchemaEntry->TryGetObjectField(TEXT("element_type"), Element) || !Element || !Element->IsValid()) break;
						Pointer += TEXT("/element_type");
						SchemaEntry = *Element;
					}
					if (ExistingDesc->ValueType != DesiredDesc.ValueType)
					{
						Error.PublicMessage = TEXT("The parameter value type differs from the parent graph declaration.");
						Error.ActualValue = MakeShared<FJsonValueString>(StaticEnum<EPropertyBagPropertyType>()->GetNameStringByValue(static_cast<int64>(DesiredDesc.ValueType)));
						Error.ExpectedValue = MakeShared<FJsonValueString>(StaticEnum<EPropertyBagPropertyType>()->GetNameStringByValue(static_cast<int64>(ExistingDesc->ValueType)));
						Pointer += TEXT("/kind");
					}
					else
					{
						Error.PublicMessage = TEXT("The parameter's referenced type differs from the parent graph declaration.");
						Error.ActualValue = DesiredDesc.ValueTypeObject.Get()
							? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(DesiredDesc.ValueTypeObject.Get()->GetPathName())) : TSharedPtr<FJsonValue>(MakeShared<FJsonValueNull>());
						Error.ExpectedValue = ExistingDesc->ValueTypeObject.Get()
							? TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(ExistingDesc->ValueTypeObject.Get()->GetPathName())) : TSharedPtr<FJsonValue>(MakeShared<FJsonValueNull>());
						Pointer += TEXT("/type_path");
					}
				}
				else if (ExistingDesc->ContainerTypes != DesiredDesc.ContainerTypes)
				{
					Error.PublicMessage = TEXT("The parameter container type differs from the parent graph declaration.");
					if (ExistingDesc->ContainerTypes.Num() != DesiredDesc.ContainerTypes.Num())
					{
						Error.PropertyPath = TEXT("container_depth");
						Error.ActualValue = MakeShared<FJsonValueNumber>(DesiredDesc.ContainerTypes.Num());
						Error.ExpectedValue = MakeShared<FJsonValueNumber>(ExistingDesc->ContainerTypes.Num());
					}
					else
					{
						const auto ContainerName = [](EPropertyBagContainerType Type) {
							return Type == EPropertyBagContainerType::Array ? TEXT("array") : Type == EPropertyBagContainerType::Set ? TEXT("set") : TEXT("none");
						};
						for (uint32 Index = 0; Index < DesiredDesc.ContainerTypes.Num(); ++Index)
						{
							if (DesiredDesc.ContainerTypes[Index] != ExistingDesc->ContainerTypes[Index])
							{
								Error.ActualValue = MakeShared<FJsonValueString>(ContainerName(DesiredDesc.ContainerTypes[Index]));
								Error.ExpectedValue = MakeShared<FJsonValueString>(ContainerName(ExistingDesc->ContainerTypes[Index]));
								Pointer += TEXT("/kind");
								break;
							}
							Pointer += TEXT("/element_type");
						}
					}
				}
				else
				{
					Error.PublicMessage = TEXT("The parameter declaration has no native property available for copying its value.");
					Error.SourceKind = TEXT("native_result");
					Error.PropertyPath = !ExistingDesc->CachedProperty ? TEXT("parent_property_available") : TEXT("decoded_property_available");
					Error.ActualValue = MakeShared<FJsonValueBoolean>(false);
					Error.ExpectedValue = MakeShared<FJsonValueBoolean>(true);
				}
				Error.ActualValuePresent = true;
				Error.ExpectedValuePresent = true;
				if (Error.SourceKind != TEXT("native_result")) Error.Source.InternalJsonPath = Pointer;
				AppendGraphIssue(Issues, MoveTemp(Error), BagJson.Get(), TEXT("schema"));
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
		TArray<UE::UAI::Contract::FDiagnostic>& Issues,
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

	UPCGGraph* CreateOrLoadGraphAsset(const FString& AssetPathInput, bool bCreateIfMissing, bool& bOutCreated, UE::UAI::Contract::FDiagnostic& OutError)
	{
		bOutCreated = false;
		const FString AssetPath = NormalizeAssetPath(AssetPathInput);
		if (UPCGGraph* ExistingGraph = LoadAsset<UPCGGraph>(AssetPath))
		{
			return ExistingGraph;
		}
		if (!bCreateIfMissing)
		{
			OutError = BackendFailure(TEXT("pcg_graph_asset_not_found"), TEXT("The asset path did not resolve to an existing PCG graph and creation is disabled."),
				TEXT("asset_path"), MakeShared<FJsonValueString>(AssetPath), MakeShared<FJsonValueString>(TEXT("an existing PCGGraph asset")));
			OutError.TargetPath = AssetPath;
			return nullptr;
		}
		if (!FPackageName::IsValidLongPackageName(AssetPath))
		{
			OutError = BackendFailure(TEXT("invalid_asset_path"), TEXT("The graph asset path is not a valid long package name."),
				TEXT("asset_path"), MakeShared<FJsonValueString>(AssetPath), MakeShared<FJsonValueString>(TEXT("a valid long package name")));
			OutError.TargetPath = AssetPath;
			return nullptr;
		}

		const FString AssetName = FPackageName::GetLongPackageAssetName(AssetPath);
		UPackage* Package = CreatePackage(*AssetPath);
		if (!Package)
		{
			OutError = BackendFailure(TEXT("create_package_failed"), TEXT("Unreal could not create the package for the PCG graph."),
				TEXT("package_created"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), true);
			OutError.TargetPath = AssetPath;
			return nullptr;
		}

		UPCGGraph* Graph = NewObject<UPCGGraph>(Package, UPCGGraph::StaticClass(), *AssetName, RF_Public | RF_Standalone | RF_Transactional);
		if (!Graph)
		{
			OutError = BackendFailure(TEXT("create_pcg_graph_failed"), TEXT("Unreal could not create the PCG graph object in its package."),
				TEXT("graph_created"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), true);
			OutError.TargetPath = AssetPath;
			return nullptr;
		}
		FAssetRegistryModule::AssetCreated(Graph);
		Graph->MarkPackageDirty();
		Package->MarkPackageDirty();
		bOutCreated = true;
		return Graph;
	}

	bool ApplyGraphJson(UPCGGraph* Graph, const TSharedPtr<FJsonObject>& GraphJson, bool bReplaceNodes, TArray<UE::UAI::Contract::FDiagnostic>& Issues, int32& OutAppliedNodes, int32& OutAppliedEdges, const FGraphNodeCreationOverride* NodeCreationOverride, bool bPreserveExistingNodes, TMap<FString, UPCGNode*>* OutAppliedNodeBindings, const FGraphNodePropertiesApply* ApplyNodeProperties, const UE::UAI::HjsonWorkspace::FGraphAuthoringConnectionContext* Connections, const FString& LogicalTarget)
	{
		OutAppliedNodes = 0;
		OutAppliedEdges = 0;
		if (OutAppliedNodeBindings) OutAppliedNodeBindings->Reset();
		if (!Graph || !GraphJson.IsValid())
		{
			AppendGraphIssue(Issues, GraphFailure(TEXT("invalid_graph_apply_input"), TEXT("Graph apply requires both a native graph and a graph document."),
				!Graph ? TEXT("graph_available") : TEXT("graph_document_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), true));
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* NodeValues = nullptr;
		GraphJson->TryGetArrayField(TEXT("nodes"), NodeValues);
		const TArray<TSharedPtr<FJsonValue>>* EdgeValues = nullptr;
		GraphJson->TryGetArrayField(TEXT("edges"), EdgeValues);

		TMap<FString, UPCGNode*> ExistingNodes;
		const bool bHasConnectionIntent = bPreserveExistingNodes && Connections && Connections->bHasResolvedIntent;
		TSet<UPCGNode*> RetainedNodes;
		if (bPreserveExistingNodes)
		{
			ExistingNodes = BuildExportNodeMapById(Graph);
			if (NodeValues)
			{
				for (const TSharedPtr<FJsonValue>& Value : *NodeValues)
				{
					const TSharedPtr<FJsonObject> NodeObject = Value.IsValid() ? Value->AsObject() : nullptr;
					FString Id, ClassPath;
					if (!NodeObject.IsValid() || !NodeObject->TryGetStringField(TEXT("id"), Id)) continue;
					UPCGNode* Existing = ExistingNodes.FindRef(Id);
					if (Existing && NodeObject->TryGetStringField(TEXT("settings_class"), ClassPath)
						&& Existing->GetSettings() && Existing->GetSettings()->GetClass() == ResolvePCGSettingsClass(ClassPath))
					{
						RetainedNodes.Add(Existing);
					}
				}
			}
		}
		if (bReplaceNodes)
		{
			TArray<UPCGNode*> NodesToRemove;
			for (UPCGNode* Node : Graph->GetNodes())
			{
				if (Node && Node != Graph->GetInputNode() && Node != Graph->GetOutputNode()
					&& !RetainedNodes.Contains(Node))
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
		if (bPreserveExistingNodes)
		{
			TMap<UPCGNode*, FString> IdByNode;
			for (const auto& Pair : ExistingNodes) IdByNode.Add(Pair.Value, Pair.Key);
			// Remove authored deletions before native pin reconstruction. Captured
			// connections must remain attached so UE can retain, rename or break them.
			for (const TPair<FString, UPCGNode*>& Pair : ExistingNodes)
			{
				if (!RetainedNodes.Contains(Pair.Value) && Pair.Value != Graph->GetInputNode()
					&& Pair.Value != Graph->GetOutputNode()) continue;
				for (const UPCGPin* Pin : Pair.Value->GetOutputPins())
				{
					if (!Pin) continue;
					if (!bHasConnectionIntent)
					{
						Graph->RemoveOutboundEdges(Pair.Value, Pin->Properties.Label);
						continue;
					}
					const auto Edges = Pin->Edges;
					for (UPCGEdge* Edge : Edges)
					{
						if (!Edge || Edge->InputPin != Pin || !Edge->OutputPin) continue;
						UPCGPin* Target = Edge->OutputPin;
						if (!Connections->IsUnchanged(Pair.Key, Pin->Properties.Label.ToString(),
							IdByNode.FindRef(Target->Node), Target->Properties.Label.ToString()))
							Graph->RemoveEdge(Pair.Value, Pin->Properties.Label, Target->Node, Target->Properties.Label);
					}
				}
			}
		}

		if (NodeValues)
		{
			for (int32 NodeIndex = 0; NodeIndex < NodeValues->Num(); ++NodeIndex)
			{
				const TSharedPtr<FJsonValue>& Value = (*NodeValues)[NodeIndex];
				const TSharedPtr<FJsonObject>* NodeObj = nullptr;
				if (!Value.IsValid() || !Value->TryGetObject(NodeObj) || !NodeObj || !NodeObj->IsValid())
				{
					AppendGraphIssue(Issues, GraphFailure(TEXT("invalid_node_entry"), TEXT("A graph node entry must be an object."),
						FString::Printf(TEXT("/nodes/%d"), NodeIndex), Value, MakeShared<FJsonValueString>(TEXT("object"))), GraphJson.Get(), TEXT("nodes"));
					continue;
				}

				FString Id;
				(*NodeObj)->TryGetStringField(TEXT("id"), Id);
				if (Id.IsEmpty())
				{
					AppendGraphIssue(Issues, GraphFailure(TEXT("missing_node_id"), TEXT("A graph node entry requires a non-empty id."),
						TEXT("id"), (*NodeObj)->TryGetField(TEXT("id")), MakeShared<FJsonValueString>(TEXT("a non-empty node id"))), NodeObj->Get(), TEXT("id"));
					continue;
				}
				if (Id == InputNodeId || Id == OutputNodeId)
				{
					UPCGNode* Node = (Id == InputNodeId) ? Graph->GetInputNode() : Graph->GetOutputNode();
					UPCGSettings* Settings = Node ? Node->GetSettings() : nullptr;
					if (!Node || !Settings)
					{
						AppendGraphIssue(Issues, GraphFailure(TEXT("input_output_node_not_found"), TEXT("The native graph input/output node and its settings must exist."),
							!Node ? TEXT("node_available") : TEXT("settings_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), true), nullptr, FString(), Id);
						continue;
					}

					FString AuthoredTitle;
					if ((*NodeObj)->TryGetStringField(TEXT("authored_title"), AuthoredTitle))
					{
						Node->NodeTitle = FName(*AuthoredTitle);
					}

					UE::UAI::Contract::FDiagnostic PositionError;
					if (!ApplyAuthoredNodePosition(*Node, **NodeObj, PositionError))
					{ AppendGraphIssue(Issues, PositionError, NodeObj->Get(), FString(), Id); continue; }
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

				UPCGNode* Node = ExistingNodes.FindRef(Id);
				if (!RetainedNodes.Contains(Node)) Node = nullptr;
				const bool bRetained = Node != nullptr;
				UPCGSettings* DefaultSettings = Node ? Node->GetSettings() : nullptr;
				EGraphNodeCreationOverrideDisposition OverrideDisposition =
					EGraphNodeCreationOverrideDisposition::Unhandled;
				if (!bRetained && NodeCreationOverride)
				{
					const FGraphNodeCreationOverrideResult OverrideResult =
						(*NodeCreationOverride)(*Graph, *NodeObj);
					OverrideDisposition = OverrideResult.Disposition;
					if (OverrideDisposition ==
						EGraphNodeCreationOverrideDisposition::Failed)
					{
                        auto Diagnostic = OverrideResult.Diagnostic;
                        if (Diagnostic.IsEmpty())
                            Diagnostic = GraphFailure(TEXT("node_creation_override_diagnostic_missing"),
                                TEXT("The node creation callback reported failure without a diagnostic."),
                                TEXT("diagnostic_available"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), true);
                        AppendGraphIssue(Issues, MoveTemp(Diagnostic), NodeObj->Get(), FString(), Id);
						continue;
					}
					if (OverrideDisposition ==
						EGraphNodeCreationOverrideDisposition::Created)
					{
						Node = OverrideResult.Node;
						DefaultSettings = Node ? Node->GetSettings() : nullptr;
						if (!Node || !DefaultSettings)
						{
							AppendGraphIssue(Issues, GraphFailure(TEXT("node_creation_override_result_invalid"),
								TEXT("The node creation callback reported Created without a node and settings."),
								!Node ? TEXT("node_available") : TEXT("settings_available"),
								MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), true), nullptr, FString(), Id);
							continue;
						}
					}
				}
				if (!bRetained && OverrideDisposition ==
					EGraphNodeCreationOverrideDisposition::Unhandled)
				{
					FString SettingsClassPath;
					if (!(*NodeObj)->TryGetStringField(
							TEXT("settings_class"), SettingsClassPath)
						|| SettingsClassPath.IsEmpty())
					{
						AppendGraphIssue(Issues, GraphFailure(TEXT("missing_settings_class"), TEXT("A settings node requires a non-empty settings_class."),
							TEXT("settings_class"), (*NodeObj)->TryGetField(TEXT("settings_class")), MakeShared<FJsonValueString>(TEXT("a PCGSettings class path"))), NodeObj->Get(), TEXT("settings_class"), Id);
						continue;
					}

					UClass* SettingsClass = ResolvePCGSettingsClass(SettingsClassPath);
					if (!SettingsClass)
					{
						AppendGraphIssue(Issues, GraphFailure(TEXT("settings_class_not_found"), TEXT("The settings class path did not resolve to a PCGSettings class."),
							TEXT("settings_class"), MakeShared<FJsonValueString>(SettingsClassPath), MakeShared<FJsonValueString>(TEXT("a loadable PCGSettings class"))), NodeObj->Get(), TEXT("settings_class"), Id);
						continue;
					}

					Node = Graph->AddNodeOfType(SettingsClass, DefaultSettings);
					if (!Node || !DefaultSettings)
					{
						AppendGraphIssue(Issues, GraphFailure(TEXT("add_node_failed"),
							FString::Printf(TEXT("Unreal did not create a node with settings of class %s."), *SettingsClassPath),
							!Node ? TEXT("node_created") : TEXT("settings_created"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), true), nullptr, FString(), Id);
						continue;
					}
				}

				FString AuthoredTitle;
				if ((*NodeObj)->TryGetStringField(TEXT("authored_title"), AuthoredTitle))
				{
					Node->NodeTitle = FName(*AuthoredTitle);
				}
				else if (!bRetained)
				{
					// The existing creation policy names a new request identity.
					// Omitted titles on retained native nodes must stay untouched.
					Node->NodeTitle = FName(*Id);
				}

				UE::UAI::Contract::FDiagnostic PositionError;
				if (!ApplyAuthoredNodePosition(*Node, **NodeObj, PositionError))
				{ AppendGraphIssue(Issues, PositionError, NodeObj->Get(), FString(), Id); continue; }
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

		// Parameter layouts and reflected settings determine native pin shape.
		// Finish those edits before resolving the request's edge endpoints.
		const TSharedPtr<FJsonObject>* ParametersObj = nullptr;
		if (GraphJson->TryGetObjectField(TEXT("parameters"), ParametersObj) && ParametersObj && ParametersObj->IsValid())
		{
			Graph->UpdateUserParametersStruct([&](FInstancedPropertyBag& Bag)
			{
				ApplyCanonicalParameterBag(Bag, *ParametersObj, Issues, nullptr, true);
			});
		}
		if (CountErrors(Issues) != 0) return false;
		if (ApplyNodeProperties && !(*ApplyNodeProperties)(NodeById)) return false;

		if (EdgeValues)
		{
			for (int32 EdgeIndex = 0; EdgeIndex < EdgeValues->Num(); ++EdgeIndex)
			{
				const TSharedPtr<FJsonValue>& Value = (*EdgeValues)[EdgeIndex];
				const TSharedPtr<FJsonObject>* EdgeObj = nullptr;
				if (!Value.IsValid() || !Value->TryGetObject(EdgeObj) || !EdgeObj || !EdgeObj->IsValid())
				{
					AppendGraphIssue(Issues, GraphFailure(TEXT("invalid_edge_entry"), TEXT("A graph edge entry must be an object."),
						FString::Printf(TEXT("/edges/%d"), EdgeIndex), Value, MakeShared<FJsonValueString>(TEXT("object"))), GraphJson.Get(), TEXT("edges"));
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
				if (bHasConnectionIntent && Connections->IsUnchanged(FromId, FromPin, ToId, ToPin)) continue;
				UPCGNode** FromNode = NodeById.Find(FromId);
				UPCGNode** ToNode = NodeById.Find(ToId);
				if (!FromNode || !ToNode || !*FromNode || !*ToNode)
				{
					const bool bMissingSource = !FromNode || !*FromNode;
					AppendGraphIssue(Issues, GraphFailure(TEXT("edge_node_not_found"), TEXT("An edge endpoint does not resolve to a graph node."),
						bMissingSource ? TEXT("from") : TEXT("to"), MakeShared<FJsonValueString>(bMissingSource ? FromId : ToId),
						MakeShared<FJsonValueString>(TEXT("an existing graph node id"))), EdgeObj->Get(), bMissingSource ? TEXT("from") : TEXT("to"));
					continue;
				}
				FString ResolvedFromPin;
				FString ResolvedToPin;
				UPCGPin* SourcePin = ResolvePCGEdgePin(*FromNode, FromPin, true, ResolvedFromPin, bHasConnectionIntent);
				UPCGPin* TargetPin = ResolvePCGEdgePin(*ToNode, ToPin, false, ResolvedToPin, bHasConnectionIntent);
				if (!SourcePin || !TargetPin)
				{
					AppendGraphIssue(Issues, GraphFailure(TEXT("edge_pin_not_found"),
						FString::Printf(TEXT("The %s pin does not resolve on node %s."), !SourcePin ? TEXT("output") : TEXT("input"), !SourcePin ? *FromId : *ToId),
						!SourcePin ? TEXT("from_pin") : TEXT("to_pin"), MakeShared<FJsonValueString>(!SourcePin ? FromPin : ToPin),
						MakeShared<FJsonValueString>(!SourcePin ? TEXT("an existing output pin") : TEXT("an existing input pin"))),
						EdgeObj->Get(), !SourcePin ? TEXT("from_pin") : TEXT("to_pin"), !SourcePin ? FromId : ToId);
					continue;
				}
				Graph->AddLabeledEdge(*FromNode, FName(*ResolvedFromPin), *ToNode, FName(*ResolvedToPin));
				if (!HasPCGEdge(SourcePin, TargetPin))
				{
					AppendGraphIssue(Issues, GraphFailure(TEXT("add_edge_failed"),
						FString::Printf(TEXT("Unreal did not retain the requested connection %s.%s -> %s.%s."), *FromId, *ResolvedFromPin, *ToId, *ResolvedToPin),
						TEXT("connection_present"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true)), EdgeObj->Get());
					continue;
				}
				OutAppliedEdges++;
			}
		}

		// AddLabeledEdge may keep the newly added edge while evicting an
		// earlier one. Re-resolve every original endpoint after the whole batch.
		if (EdgeValues && CountErrors(Issues) == 0)
		{
			for (const auto& Value : *EdgeValues)
			{
				const auto Edge = Value->AsObject();
				const FString FromId = Edge->GetStringField(TEXT("from"));
				const FString ToId = Edge->GetStringField(TEXT("to"));
				FString FromPin, ToPin, ResolvedFrom, ResolvedTo;
				Edge->TryGetStringField(TEXT("from_pin"), FromPin);
				Edge->TryGetStringField(TEXT("to_pin"), ToPin);
				if (bHasConnectionIntent && Connections->IsUnchanged(FromId, FromPin, ToId, ToPin)) continue;
				UPCGPin* Source = ResolvePCGEdgePin(NodeById.FindRef(FromId), FromPin, true, ResolvedFrom, bHasConnectionIntent);
				UPCGPin* Target = ResolvePCGEdgePin(NodeById.FindRef(ToId), ToPin, false, ResolvedTo, bHasConnectionIntent);
				if (!Source || !Target || !HasPCGEdge(Source, Target)) {
					auto Warning = GraphFailure(TEXT("graph_connection_intent_not_retained"),
						FString::Printf(TEXT("After the graph update, the requested connection %s.%s -> %s.%s is absent."), *FromId, *FromPin, *ToId, *ToPin),
						TEXT("connection_present"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true));
					Warning.Severity = UE::UAI::Contract::ESeverity::Warning;
					Warning.TargetPath = !LogicalTarget.IsEmpty() ? LogicalTarget : Graph->IsAsset() ? Graph->GetPathName() : FString();
					if (Warning.TargetPath != Graph->GetPathName()) Warning.InternalContext = Graph->GetPathName();
					AppendGraphIssue(Issues, MoveTemp(Warning), Edge.Get());
                    UeAgentAuthoringDiagnostics::ReportConsistencyWarning(Issues.Last());
				}
			}
		}
		Graph->MarkPackageDirty();
		if (CountErrors(Issues) != 0) return false;
		// Titles can change export aliases. Keep the request's native bindings
		// for every subsequent mutation in this operation instead of re-resolving names.
		if (OutAppliedNodeBindings) *OutAppliedNodeBindings = MoveTemp(NodeById);
		return true;
	}

	bool PreflightApplyGraphJson(UPCGGraph* ExistingGraph, const TSharedPtr<FJsonObject>& GraphJson, bool bReplaceNodes, TArray<UE::UAI::Contract::FDiagnostic>& Issues, int32& OutAppliedNodes, int32& OutAppliedEdges, const FGraphNodeCreationOverride* NodeCreationOverride)
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
			AppendGraphIssue(Issues, GraphFailure(TEXT("preflight_graph_create_failed"), TEXT("Unreal could not create an isolated PCG graph for apply preflight."),
				TEXT("preflight_graph_created"), MakeShared<FJsonValueBoolean>(false), MakeShared<FJsonValueBoolean>(true), true));
			return false;
		}

		PreflightGraph->SetFlags(RF_Transient);
		return ApplyGraphJson(PreflightGraph, GraphJson, bReplaceNodes, Issues, OutAppliedNodes, OutAppliedEdges, NodeCreationOverride,
			false, nullptr, nullptr, nullptr, ExistingGraph && ExistingGraph->IsAsset() ? ExistingGraph->GetPathName() : FString());
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

	UPCGComponent* FindPCGComponent(UWorld* World, const TSharedPtr<FJsonObject>& Params, UE::UAI::Contract::FDiagnostic& OutError)
	{
		FString ActorName;
		GetStringAny(Params, { TEXT("actor"), TEXT("actor_name"), TEXT("actor_label") }, ActorName);
		FString ComponentName;
		GetStringAny(Params, { TEXT("component"), TEXT("component_name"), TEXT("pcg_component") }, ComponentName);
		auto FailLookup = [&](const TCHAR* Code, const FString& Reason, const bool bActorFailure) -> UPCGComponent*
		{
			auto Actual = MakeShared<FJsonObject>();
			if (!ActorName.IsEmpty()) Actual->SetStringField(TEXT("actor"), ActorName);
			if (!ComponentName.IsEmpty() && !bActorFailure) Actual->SetStringField(TEXT("component"), ComponentName);
			OutError = BackendFailure(Code, Reason, bActorFailure ? TEXT("actor") : TEXT("component"),
				MakeShared<FJsonValueObject>(Actual), MakeShared<FJsonValueString>(bActorFailure
					? TEXT("an actor matching the selector") : TEXT("a PCGComponent matching the selector")));
			OutError.CandidateName = bActorFailure || ComponentName.IsEmpty() ? ActorName : ComponentName;
			if (Params.IsValid())
			{
				const TArray<const TCHAR*> Fields = bActorFailure || ComponentName.IsEmpty()
					? TArray<const TCHAR*>{TEXT("actor"), TEXT("actor_name"), TEXT("actor_label")}
					: TArray<const TCHAR*>{TEXT("component"), TEXT("component_name"), TEXT("pcg_component")};
				FString SourceField;
				for (const TCHAR* Field : Fields)
				{
					FString Value;
					if (Params->TryGetStringField(Field, Value) && !Value.TrimStartAndEnd().IsEmpty())
					{
						SourceField = Field;
						break;
					}
				}
				UeAgentAuthoringDiagnostics::LocateAuthoringSource(OutError, *Params, SourceField);
			}
			return nullptr;
		};

		if (!ActorName.IsEmpty())
		{
			AActor* Actor = FindActorByNameOrLabelDirect(World, ActorName);
			if (!Actor)
			{
				return FailLookup(TEXT("actor_not_found"), TEXT("No actor matches the supplied name or label."), true);
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
			return FailLookup(TEXT("pcg_component_not_found"), TEXT("The selected actor has no PCG component matching the supplied selector."), false);
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
		return FailLookup(TEXT("pcg_component_not_found"), TEXT("The world has no PCG component matching the supplied selector."), false);
	}

	static FString ComponentGenerationTriggerToString(EPCGComponentGenerationTrigger Trigger)
	{
		return EnumValueToString(StaticEnum<EPCGComponentGenerationTrigger>(), static_cast<int64>(Trigger));
	}

	bool TryParseGenerationTrigger(const FString& TriggerText, EPCGComponentGenerationTrigger& OutTrigger)
	{
		const UEnum* TriggerEnum = StaticEnum<EPCGComponentGenerationTrigger>();
		int64 Value = 0;
		if (UeAgentAuthoringTypeDescriptors::TryResolveEnumValueName(TriggerEnum, TriggerText, Value))
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
		return ComponentToJson(Component, true);
	}

	TSharedPtr<FJsonObject> ComponentToJson(UPCGComponent* Component, bool bIncludeProperties)
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
		Root->SetNumberField(TEXT("generated_output_count"), Component->GetGeneratedGraphOutput().TaggedData.Num());
		if (bIncludeProperties) Root->SetArrayField(TEXT("properties"), ExportEditableProperties(Component));
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
